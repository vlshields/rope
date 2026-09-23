/*
 * rope: a terminal REPL for R.
 *
 * Milestone 1: embed R, own the read-eval loop through R's console hooks,
 * use GNU readline for input, and get interrupts right.
 * Milestone 2: intercept lines before R sees them. A line whose first
 * non-blank character is '%' is a front-end command, not R code.
 * Milestone 3: output history. Every visible top-level result is kept in a
 * ring, numbered by the prompt it came from, and exposed to R as Out and In.
 * Milestone 4: the line editor. A vendored isocline replaces readline:
 * multi-line editing of whole expressions, syntax colour, completion through
 * R's own utils:::.completeToken, and the prompt shows the number the next
 * result will be filed under.
 * Milestone 5: data frames. Printing a data.frame lays it out for the
 * terminal: as many columns as fit the width, head and tail rows to fit the
 * height, types under the names, colour. %page shows any value in a pager.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <isocline.h>

#include <Rinternals.h>
#include <Rembedded.h>
#include <Rinterface.h>
#include <R_ext/Parse.h>
#include <R_ext/Callbacks.h>
#include <R_ext/Rdynload.h>

/* Exported from libR but not declared in any public header. */
extern void run_Rmainloop(void);
extern Rboolean R_Visible;          /* set by eval: would the REPL autoprint? */
extern char R_ParseErrorMsg[];      /* filled in when R_ParseVector fails    */
extern int R_interrupts_pending;    /* set by R's SIGINT handler             */

#ifndef ROPE_R_HOME
#define ROPE_R_HOME "/usr/lib/R"
#endif

/* ---- state ---------------------------------------------------------------- */

static char rope_histfile[4096];
static void (*rope_default_cleanup)(SA_TYPE, int, int) = NULL;
static SEXP rope_env = NULL;               /* the environment attached as "rope" */

/*
 * Prompt numbering. rope_input_n counts non-blank lines read at a top-level
 * prompt (R's or browser()'s), never at the continuation prompt and never for
 * a readline() call inside an evaluation. Results are filed under the number
 * of the prompt whose evaluation produced them, which is not always the most
 * recent prompt: while f() sits in browser(), lines typed there get their own
 * numbers, and the value of f() still belongs to the prompt that typed f().
 *
 * R tells us when each REPL evaluation starts (R_Busy(1)) and reads (R_Busy(0)),
 * and the browser prompt says how deep we are, so a small stack of numbers,
 * one per REPL depth, keeps track. A depth's entry goes stale when its
 * evaluation errors out; the next read at that depth or shallower discards it.
 */
#define ROPE_MAX_DEPTH 64
static int rope_input_n = 0;               /* number of the last prompt read   */
static int rope_busy = 0;                  /* inside a REPL evaluation?        */
static int rope_evaln[ROPE_MAX_DEPTH];     /* prompt number per REPL depth     */
static int rope_evaldepth = 0;             /* live entries in rope_evaln       */

/* ---- history -------------------------------------------------------------- */

/*
 * Line history is isocline's: loaded here, and written back after every line
 * so nothing is lost if R dies. The file is one entry per line, which is also
 * what readline wrote, so an old file carries over.
 */
static void rope_history_init(void)
{
    const char *env = getenv("ROPE_HISTFILE");
    if (env && *env) {
        snprintf(rope_histfile, sizeof rope_histfile, "%s", env);
    } else {
        const char *home = getenv("HOME");
        if (!home) home = ".";
        snprintf(rope_histfile, sizeof rope_histfile, "%s/.rope_history", home);
    }
    ic_set_history(rope_histfile, 10000);
}

/* ---- output history ------------------------------------------------------- */

/*
 * A ring of the visible, non-NULL results of top-level evaluations, each with
 * the expression that produced it and the number of the prompt it came from.
 * Oldest first. Both SEXPs are kept alive with R_PreserveObject until the
 * entry is evicted. The capacity is getOption("rope.out.size"), read on every
 * push so that changing it takes effect at once.
 */
struct rope_out_entry { int n; SEXP expr, value; };

static struct rope_out_entry *rope_out;
static int rope_out_len = 0, rope_out_cap = 0;

#define ROPE_OUT_DEFAULT_SIZE 100

static int rope_out_capacity(void)
{
    SEXP opt = Rf_GetOption1(Rf_install("rope.out.size"));
    if (opt == R_NilValue || XLENGTH(opt) < 1) return ROPE_OUT_DEFAULT_SIZE;
    double v = Rf_asReal(opt);
    if (ISNAN(v) || v < 0) return ROPE_OUT_DEFAULT_SIZE;
    if (v > 1e6) v = 1e6;
    return (int)v;
}

static void rope_out_release(struct rope_out_entry *e)
{
    R_ReleaseObject(e->expr);
    R_ReleaseObject(e->value);
}

static void rope_out_evict_oldest(void)
{
    rope_out_release(&rope_out[0]);
    rope_out_len--;
    memmove(rope_out, rope_out + 1, (size_t)rope_out_len * sizeof *rope_out);
}

/*
 * Record `value` under prompt `n`. Only what R would have printed goes in:
 * invisible results (assignments, invisible()) and NULL are skipped, as are
 * Out and In themselves. If one prompt produced several visible values, the
 * last one wins.
 */
static void rope_out_push(int n, SEXP expr, SEXP value)
{
    int cap = rope_out_capacity();
    if (Rf_inherits(value, "rope_out") || Rf_inherits(value, "rope_in")) return;

    if (rope_out_len > 0 && rope_out[rope_out_len - 1].n == n) {
        rope_out_release(&rope_out[rope_out_len - 1]);
        rope_out_len--;
    }
    while (rope_out_len > 0 && rope_out_len >= cap) rope_out_evict_oldest();
    if (cap == 0) return;

    if (cap > rope_out_cap) {
        struct rope_out_entry *p = realloc(rope_out, (size_t)cap * sizeof *p);
        if (!p) return;
        rope_out = p;
        rope_out_cap = cap;
    }

    R_PreserveObject(expr);
    R_PreserveObject(value);
    rope_out[rope_out_len].n = n;
    rope_out[rope_out_len].expr = expr;
    rope_out[rope_out_len].value = value;
    rope_out_len++;
}

/*
 * R hands the replacement line for a front-end command back through its own
 * REPL, which fires the task callback once more. That evaluation is not the
 * user's and is never recorded.
 */
static int rope_out_skip_next = 0;

/* .Call entry points. */

/*
 * rope_out_record(expr, value, visible): the task callback. R runs it after
 * every successful top-level evaluation in its REPL, including inside
 * browser(). It is registered through R's addTaskCallback() with a closure
 * wrapper, because Rf_addTaskCallback is declared in the headers but not
 * exported by libR.
 */
static SEXP rope_out_record(SEXP expr, SEXP value, SEXP visible)
{
    if (rope_out_skip_next) {
        rope_out_skip_next = 0;
        return Rf_ScalarLogical(TRUE);
    }
    int n = rope_evaldepth > 0 ? rope_evaln[--rope_evaldepth] : rope_input_n;
    if (Rf_asLogical(visible) == TRUE && value != R_NilValue)
        rope_out_push(n, expr, value);
    return Rf_ScalarLogical(TRUE);    /* keep the callback registered */
}

static struct rope_out_entry *rope_out_find(int n)
{
    if (n < 0) {
        int i = rope_out_len + n;
        return i >= 0 ? &rope_out[i] : NULL;
    }
    for (int i = rope_out_len - 1; i >= 0; i--)
        if (rope_out[i].n == n) return &rope_out[i];
    return NULL;
}

/* rope_out_lookup(i): list(n, expr, value) for prompt i (i < 0 counts back
 * from the newest entry), or NULL if nothing is stored there. */
static SEXP rope_out_lookup(SEXP which)
{
    int n = Rf_asInteger(which);
    if (n == NA_INTEGER || n == 0) return R_NilValue;
    struct rope_out_entry *e = rope_out_find(n);
    if (!e) return R_NilValue;

    SEXP out = PROTECT(Rf_allocVector(VECSXP, 3));
    SET_VECTOR_ELT(out, 0, Rf_ScalarInteger(e->n));
    SET_VECTOR_ELT(out, 1, e->expr);
    SET_VECTOR_ELT(out, 2, e->value);
    SEXP names = PROTECT(Rf_allocVector(STRSXP, 3));
    SET_STRING_ELT(names, 0, Rf_mkChar("n"));
    SET_STRING_ELT(names, 1, Rf_mkChar("expr"));
    SET_STRING_ELT(names, 2, Rf_mkChar("value"));
    Rf_setAttrib(out, R_NamesSymbol, names);
    UNPROTECT(2);
    return out;
}

/* rope_out_numbers(): the prompt numbers with a stored result, oldest first. */
static SEXP rope_out_numbers(void)
{
    SEXP out = PROTECT(Rf_allocVector(INTSXP, rope_out_len));
    for (int i = 0; i < rope_out_len; i++) INTEGER(out)[i] = rope_out[i].n;
    UNPROTECT(1);
    return out;
}

/* rope_input_number(): the number of the prompt being answered. */
static SEXP rope_input_number(void)
{
    return Rf_ScalarInteger(rope_input_n);
}

/* Terminal output, defined further down. */
static SEXP rope_term(void);
static SEXP rope_frame_show(SEXP spec, SEXP page);
static SEXP rope_page_text(SEXP lines);

#define ROPE_FN(f) ((DL_FUNC)(void (*)(void))(f))

static const R_CallMethodDef rope_call_methods[] = {
    { "rope_out_record",   ROPE_FN(rope_out_record),   3 },
    { "rope_out_lookup",   ROPE_FN(rope_out_lookup),   1 },
    { "rope_out_numbers",  ROPE_FN(rope_out_numbers),  0 },
    { "rope_input_number", ROPE_FN(rope_input_number), 0 },
    { "rope_term",         ROPE_FN(rope_term),         0 },
    { "rope_frame_show",   ROPE_FN(rope_frame_show),   2 },
    { "rope_page_text",    ROPE_FN(rope_page_text),    1 },
    { NULL, NULL, 0 }
};

/*
 * The R half: an environment attached to the search path as "rope" holding
 * Out and In. Out[[n]] is the result of prompt n, Out[[-1]] the newest stored
 * result; In[[n]] is the expression. Out[i] and In[i] give named lists, and
 * with no index everything stored.
 *
 * It also replaces print.data.frame, registered in base's S3 table so that
 * both autoprint and print(x) from the prompt find it. The R code picks the
 * rows and formats the cells, since only R knows how to format an R value;
 * the layout, colour and paging are done in C by rope_frame_show. It falls
 * back to base's method whenever output is not going straight to a terminal
 * (a sink, capture.output, knitr), when print() is given arguments it does
 * not know, and when options(rope.frames = FALSE). Classes with their own
 * print method, tibbles and data.tables among them, never reach it.
 */
static const char rope_r_setup[] =
    "local({\n"
    "  env <- new.env()\n"
    "  numbers <- function() .Call('rope_out_numbers')\n"
    "  lookup <- function(i, what) {\n"
    "    if (!is.numeric(i) || length(i) != 1L || is.na(i) || i == 0)\n"
    "      stop(what, '[[i]]: i must be a single non-zero number', call. = FALSE)\n"
    "    i <- as.integer(i)\n"
    "    r <- .Call('rope_out_lookup', i)\n"
    "    if (is.null(r)) {\n"
    "      if (i < 0) stop(sprintf('%s[[%d]]: only %d results stored', what, i, length(numbers())), call. = FALSE)\n"
    "      stop(sprintf('%s[[%d]]: nothing stored for input %d', what, i, i), call. = FALSE)\n"
    "    }\n"
    "    r\n"
    "  }\n"
    "  many <- function(i, what, field) {\n"
    "    if (missing(i)) i <- numbers()\n"
    "    r <- lapply(i, lookup, what = what)\n"
    "    names(r) <- vapply(r, function(e) as.character(e$n), '')\n"
    "    lapply(r, `[[`, field)\n"
    "  }\n"
    "  brief <- function(expr) {\n"
    "    s <- deparse(expr, width.cutoff = 60L)\n"
    "    if (length(s) > 1L) paste(s[1L], '...') else s\n"
    "  }\n"
    "  show <- function(what) {\n"
    "    ns <- numbers()\n"
    "    if (!length(ns)) { cat(what, ': nothing stored\\n', sep = ''); return(invisible()) }\n"
    "    w <- nchar(as.character(ns[length(ns)]))\n"
    "    for (n in ns) cat(sprintf('%s[[%*d]]  %s\\n', what, w, n, brief(lookup(n, what)$expr)))\n"
    "    invisible()\n"
    "  }\n"
    "  m <- list(\n"
    "    `[[.rope_out` = function(x, i, ...) lookup(i, 'Out')$value,\n"
    "    `[[.rope_in`  = function(x, i, ...) lookup(i, 'In')$expr,\n"
    "    `[.rope_out`  = function(x, i, ...) many(i, 'Out', 'value'),\n"
    "    `[.rope_in`   = function(x, i, ...) many(i, 'In', 'expr'),\n"
    "    length.rope_out = function(x) length(numbers()),\n"
    "    length.rope_in  = function(x) length(numbers()),\n"
    "    names.rope_out  = function(x) as.character(numbers()),\n"
    "    names.rope_in   = function(x) as.character(numbers()),\n"
    "    as.list.rope_out = function(x, ...) x[],\n"
    "    as.list.rope_in  = function(x, ...) x[],\n"
    "    print.rope_out = function(x, ...) { show('Out'); invisible(x) },\n"
    "    print.rope_in  = function(x, ...) { show('In');  invisible(x) }\n"
    "  )\n"
    "  for (nm in names(m)) {\n"
    "    assign(nm, m[[nm]], envir = env)\n"
    "    parts <- regmatches(nm, regexpr('\\\\.rope_', nm), invert = TRUE)[[1L]]\n"
    "    registerS3method(parts[1L], paste0('rope_', parts[2L]), m[[nm]], envir = baseenv())\n"
    "  }\n"
    "  env$Out <- structure(list(), class = 'rope_out')\n"
    "  env$In  <- structure(list(), class = 'rope_in')\n"
    "  env$.rope_complete <- function(line, end) {\n"
    "    utils:::.assignLinebuffer(line)\n"
    "    utils:::.assignEnd(end)\n"
    "    utils:::.guessTokenFromLine()\n"
    "    suppressWarnings(utils:::.completeToken())\n"
    "    list(utils:::.CompletionEnv[['token']], utils:::.retrieveCompletions())\n"
    "  }\n"
    "  frame_type <- function(v) {\n"
    "    if (is.ordered(v)) 'ord'\n"
    "    else if (is.factor(v)) 'fct'\n"
    "    else if (inherits(v, 'Date')) 'date'\n"
    "    else if (inherits(v, 'POSIXt')) 'dttm'\n"
    "    else if (inherits(v, 'difftime')) 'drtn'\n"
    "    else if (is.object(v)) class(v)[1L]\n"
    "    else switch(typeof(v), double = 'dbl', integer = 'int', character = 'chr',\n"
    "                logical = 'lgl', complex = 'cplx', list = 'list', raw = 'raw',\n"
    "                closure = , builtin = , special = 'fn', typeof(v))\n"
    "  }\n"
    "  frame_width <- function(s) {\n"
    "    w <- nchar(s, 'width', allowNA = TRUE)\n"
    "    w[is.na(w)] <- nchar(s[is.na(w)], 'bytes')\n"
    "    as.integer(w)\n"
    "  }\n"
    "  frame_fit <- function(s, cap, ell) {\n"
    "    long <- frame_width(s) > cap\n"
    "    if (any(long)) s[long] <- paste0(strtrim(s[long], cap - 1L), ell)\n"
    "    s\n"
    "  }\n"
    "  frame_cells <- function(v, times) {\n"
    "    if (is.character(v) || is.factor(v)) {\n"
    "      s <- encodeString(as.character(v))\n"
    "      s[is.na(v)] <- '<NA>'\n"
    "      return(s)\n"
    "    }\n"
    "    if (is.list(v) && !is.object(v))\n"
    "      return(vapply(v, function(e) {\n"
    "        if (is.null(e)) 'NULL'\n"
    "        else if (is.data.frame(e)) sprintf('<df [%d %s %d]>', nrow(e), times, length(e))\n"
    "        else if (is.atomic(e) && length(e) == 1L && is.null(dim(e)) && !is.object(e))\n"
    "          encodeString(format(e))\n"
    "        else sprintf('<%s [%d]>', frame_type(e), length(e))\n"
    "      }, ''))\n"
    "    s <- tryCatch(format(v), error = function(e) NULL)\n"
    "    if (!is.character(s) || length(s) != length(v)) s <- as.character(v)\n"
    "    s[is.na(s)] <- 'NA'\n"
    "    encodeString(s)\n"
    "  }\n"
    "  frame_flat <- function(nm, v) {\n"
    "    if (is.data.frame(v))\n"
    "      return(unlist(lapply(seq_along(v), function(i)\n"
    "        frame_flat(paste0(nm, '$', names(v)[i]), v[[i]])), recursive = FALSE))\n"
    "    if (length(dim(v)) == 2L) {\n"
    "      cn <- colnames(v)\n"
    "      if (is.null(cn)) cn <- seq_len(ncol(v))\n"
    "      return(lapply(seq_len(ncol(v)), function(i)\n"
    "        list(name = paste0(nm, '[,', cn[i], ']'), v = v[, i])))\n"
    "    }\n"
    "    list(list(name = nm, v = v))\n"
    "  }\n"
    "  frame <- function(x, n = NULL, page = FALSE) {\n"
    "    term <- .Call('rope_term')\n"
    "    width <- if (is.null(term)) 80L else term[1L]\n"
    "    height <- if (is.null(term)) 24L else term[2L]\n"
    "    utf8 <- isTRUE(l10n_info()[['UTF-8']])\n"
    "    ell <- if (utf8) '…' else '~'\n"
    "    times <- if (utf8) '×' else 'x'\n"
    "    big <- function(k) format(k, big.mark = ',', scientific = FALSE)\n"
    "    nr <- nrow(x)\n"
    "    nc <- length(x)\n"
    "\n"
    "    gap <- 0L\n"
    "    note <- ''\n"
    "    if (!is.null(n) || page) {\n"
    "      k <- if (page) min(nr, 100000L)\n"
    "           else if (is.infinite(n)) nr else min(nr, max(0L, as.integer(n)))\n"
    "      rows <- seq_len(k)\n"
    "      if (k < nr) note <- sprintf('%s more rows', big(nr - k))\n"
    "    } else {\n"
    "      avail <- max(10L, height - 8L)\n"
    "      if (nr <= avail) {\n"
    "        rows <- seq_len(nr)\n"
    "      } else {\n"
    "        head <- as.integer(ceiling((avail - 1L) * 2 / 3))\n"
    "        tail <- avail - 1L - head\n"
    "        rows <- c(seq_len(head), seq.int(nr - tail + 1L, nr))\n"
    "        gap <- head\n"
    "        note <- sprintf('%s more rows (%%page to see them all)', big(nr - head - tail))\n"
    "      }\n"
    "    }\n"
    "\n"
    "    cap <- if (page) 80L else max(10L, width %/% 2L)\n"
    "    auto <- .row_names_info(x) < 0L\n"
    "    labels <- if (auto) as.character(rows)\n"
    "              else frame_fit(encodeString(rownames(x)[rows]), cap, ell)\n"
    "    names <- names(x)\n"
    "    if (is.null(names)) names <- rep('', nc)\n"
    "    names[is.na(names)] <- 'NA'\n"
    "\n"
    "    cols <- list()\n"
    "    hidden <- character()\n"
    "    used <- max(1L, frame_width(labels))\n"
    "    for (j in seq_len(nc)) {\n"
    "      v <- x[[j]]\n"
    "      if (inherits(v, 'AsIs')) oldClass(v) <- setdiff(oldClass(v), 'AsIs')\n"
    "      if (!page && used > width) {\n"
    "        hidden <- c(hidden, paste0(names[j], ' <', frame_type(v), '>'))\n"
    "        next\n"
    "      }\n"
    "      v <- if (length(dim(v)) == 2L) v[rows, , drop = FALSE] else v[rows]\n"
    "      for (p in frame_flat(names[j], v)) {\n"
    "        cells <- frame_fit(frame_cells(p$v, times), cap, ell)\n"
    "        name <- frame_fit(p$name, cap, ell)\n"
    "        type <- paste0('<', frame_type(p$v), '>')\n"
    "        col <- list(name = name, name_w = frame_width(name),\n"
    "                    type = type, type_w = frame_width(type),\n"
    "                    cells = cells, cells_w = frame_width(cells),\n"
    "                    na = if (is.atomic(p$v)) as.logical(is.na(p$v)) else logical(length(cells)),\n"
    "                    right = is.numeric(p$v) || is.logical(p$v) || is.complex(p$v))\n"
    "        used <- used + 1L + max(col$name_w, col$type_w, col$cells_w)\n"
    "        cols[[length(cols) + 1L]] <- col\n"
    "      }\n"
    "    }\n"
    "\n"
    "    spec <- list(title = sprintf('%s [%s %s %s]', class(x)[1L], big(nr), times, big(nc)),\n"
    "                 width = if (page) NA_integer_ else width,\n"
    "                 labels = labels, labels_w = frame_width(labels), labels_right = auto,\n"
    "                 gap = gap, dots = if (utf8) '⋮' else ':', ell = ell,\n"
    "                 cols = cols, hidden = hidden, note = note)\n"
    "    .Call('rope_frame_show', spec, page)\n"
    "  }\n"
    "  print_frame <- function(x, ..., n = NULL) {\n"
    "    if (...length() || !isTRUE(getOption('rope.frames', TRUE)) || sink.number() > 0L ||\n"
    "        !length(x) || is.null(.Call('rope_term')))\n"
    "      return(base::print.data.frame(x, ...))\n"
    "    tryCatch(frame(x, n), error = function(e) base::print.data.frame(x))\n"
    "    invisible(x)\n"
    "  }\n"
    "  registerS3method('print', 'data.frame', print_frame, envir = baseenv())\n"
    "  env$.rope_page <- function(x) {\n"
    "    if (is.data.frame(x) && length(x)) {\n"
    "      cls <- class(x)\n"
    "      cls <- cls[seq_len(match('data.frame', cls, 0L) - 1L)]\n"
    "      own <- vapply(cls, function(cl) !is.null(getS3method('print', cl, optional = TRUE)), NA)\n"
    "      if (!any(own)) return(invisible(frame(x, page = TRUE)))\n"
    "    }\n"
    "    lines <- utils::capture.output(\n"
    "      if (inherits(x, 'tbl_df')) print(x, n = Inf, width = Inf) else print(x))\n"
    "    invisible(.Call('rope_page_text', lines))\n"
    "  }\n"
    "  attach(env, name = 'rope', warn.conflicts = FALSE)\n"
    "  addTaskCallback(function(expr, value, ok, visible)\n"
    "    .Call('rope_out_record', expr, value, visible), name = 'rope')\n"
    "  env\n"
    "})\n";

static void rope_setup_cb(void *data)
{
    SEXP exprs = data, value = R_NilValue;
    for (R_xlen_t i = 0; i < XLENGTH(exprs); i++)
        value = Rf_eval(VECTOR_ELT(exprs, i), R_GlobalEnv);
    if (TYPEOF(value) == ENVSXP) {
        rope_env = value;
        R_PreserveObject(rope_env);
    }
}

static void rope_out_init(void)
{
    R_registerRoutines(R_getEmbeddingDllInfo(), NULL, rope_call_methods, NULL, NULL);

    ParseStatus status;
    SEXP src = PROTECT(Rf_mkString(rope_r_setup));
    SEXP exprs = PROTECT(R_ParseVector(src, -1, &status, R_NilValue));
    if (status != PARSE_OK || !R_ToplevelExec(rope_setup_cb, exprs))
        REprintf("rope: could not set up Out and In\n");
    UNPROTECT(2);
}

/* ---- data frames and the pager -------------------------------------------- */

/*
 * rope_term(): c(width, height) of the terminal standard output goes to, or
 * NULL if it does not go to one. The size is asked for on every call, so a
 * resized window is followed.
 */
static int rope_term_usable(void)
{
    const char *term = getenv("TERM");
    return isatty(STDOUT_FILENO) && term && *term && strcmp(term, "dumb") != 0;
}

static SEXP rope_term(void)
{
    if (!rope_term_usable()) return R_NilValue;
    int cols = 0, rows = 0;
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        cols = ws.ws_col;
        rows = ws.ws_row;
    }
    if (cols <= 0 && getenv("COLUMNS")) cols = atoi(getenv("COLUMNS"));
    if (rows <= 0 && getenv("LINES"))   rows = atoi(getenv("LINES"));
    SEXP out = PROTECT(Rf_allocVector(INTSXP, 2));
    INTEGER(out)[0] = cols > 0 ? cols : 80;
    INTEGER(out)[1] = rows > 0 ? rows : 24;
    UNPROTECT(1);
    return out;
}

static int rope_colour(void)
{
    const char *no = getenv("NO_COLOR");
    return rope_term_usable() && !(no && *no);
}

#define ROPE_SGR_DIM  "\x1b[90m"
#define ROPE_SGR_BOLD "\x1b[1m"
#define ROPE_SGR_NA   "\x1b[31m"
#define ROPE_SGR_OFF  "\x1b[0m"

/* A growable, NUL-terminated byte buffer for rendered output. */
struct rope_buf { char *p; size_t len, cap; };

static void rope_buf_put(struct rope_buf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (b->len + n + 1 > cap) cap *= 2;
        char *p = realloc(b->p, cap);
        if (!p) {
            free(b->p);
            b->p = NULL;
            Rf_error("rope: out of memory rendering output");
        }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void rope_buf_str(struct rope_buf *b, const char *s)
{
    rope_buf_put(b, s, strlen(s));
}

static void rope_buf_pad(struct rope_buf *b, int n)
{
    static const char spaces[] = "                                ";
    while (n > 0) {
        int k = n < (int)sizeof spaces - 1 ? n : (int)sizeof spaces - 1;
        rope_buf_put(b, spaces, (size_t)k);
        n -= k;
    }
}

/* End a line, dropping the padding after its last cell. */
static void rope_buf_eol(struct rope_buf *b)
{
    while (b->len > 0 && b->p[b->len - 1] == ' ') b->len--;
    rope_buf_put(b, "\n", 1);
}

/* `text`, `w` columns wide on screen, aligned in a field `field` wide. */
static void rope_buf_cell(struct rope_buf *b, const char *text, int w, int field,
                          int right, const char *style)
{
    if (right) rope_buf_pad(b, field - w);
    if (style) rope_buf_str(b, style);
    rope_buf_str(b, text);
    if (style) rope_buf_str(b, ROPE_SGR_OFF);
    if (!right) rope_buf_pad(b, field - w);
}

static SEXP rope_get(SEXP list, const char *name)
{
    SEXP names = Rf_getAttrib(list, R_NamesSymbol);
    if (TYPEOF(list) != VECSXP || TYPEOF(names) != STRSXP) return R_NilValue;
    for (R_xlen_t i = 0; i < XLENGTH(list); i++)
        if (strcmp(CHAR(STRING_ELT(names, i)), name) == 0) return VECTOR_ELT(list, i);
    return R_NilValue;
}

static const char *rope_get_str(SEXP list, const char *name)
{
    SEXP s = rope_get(list, name);
    if (TYPEOF(s) != STRSXP || XLENGTH(s) < 1) return "";
    return Rf_translateChar(STRING_ELT(s, 0));
}

static int rope_utf8_width(const char *s)
{
    int w = 0;
    for (; *s; s++)
        if ((*s & 0xC0) != 0x80) w++;
    return w;
}

/*
 * One column as the R side formatted it: name and type (already "<dbl>"),
 * each with its width on screen, then one cell, its width and an NA flag per
 * row shown. Numbers are right-aligned, everything else left.
 */
struct rope_col {
    const char *name, *type;
    int name_w, type_w, width, right;
    SEXP cells, cells_w, na;
};

static int rope_col_read(SEXP c, int nrows, struct rope_col *out)
{
    out->cells = rope_get(c, "cells");
    out->cells_w = rope_get(c, "cells_w");
    out->na = rope_get(c, "na");
    if (TYPEOF(out->cells) != STRSXP || XLENGTH(out->cells) != nrows ||
        TYPEOF(out->cells_w) != INTSXP || XLENGTH(out->cells_w) != nrows ||
        TYPEOF(out->na) != LGLSXP || XLENGTH(out->na) != nrows)
        return 0;
    out->name = rope_get_str(c, "name");
    out->type = rope_get_str(c, "type");
    out->name_w = Rf_asInteger(rope_get(c, "name_w"));
    out->type_w = Rf_asInteger(rope_get(c, "type_w"));
    out->right = Rf_asLogical(rope_get(c, "right")) == TRUE;
    out->width = out->name_w > out->type_w ? out->name_w : out->type_w;
    for (int r = 0; r < nrows; r++)
        if (INTEGER(out->cells_w)[r] > out->width) out->width = INTEGER(out->cells_w)[r];
    return 1;
}

/*
 * Lay out a frame described by the R side (frame() in rope_r_setup):
 * a title, the column names in bold, their types dimmed, the rows with their
 * labels dimmed and NA in red. Columns are taken left to right while they fit
 * `width`; the rest are named in a footer. `width` is NA when paging: then
 * every column is shown and the pager scrolls sideways.
 */
static void rope_frame_render(SEXP spec, struct rope_buf *b)
{
    int colour = rope_colour();
    const char *dim  = colour ? ROPE_SGR_DIM  : NULL;
    const char *bold = colour ? ROPE_SGR_BOLD : NULL;
    const char *red  = colour ? ROPE_SGR_NA   : NULL;

    int width = Rf_asInteger(rope_get(spec, "width"));
    if (width == NA_INTEGER || width < 1) width = INT_MAX;
    SEXP labels = rope_get(spec, "labels"), labels_w = rope_get(spec, "labels_w");
    SEXP cols = rope_get(spec, "cols"), hidden = rope_get(spec, "hidden");
    if (TYPEOF(labels) != STRSXP || TYPEOF(labels_w) != INTSXP ||
        XLENGTH(labels_w) != XLENGTH(labels) || TYPEOF(cols) != VECSXP)
        return;
    int nrows = (int)XLENGTH(labels), ncols = (int)XLENGTH(cols);
    int nhidden = TYPEOF(hidden) == STRSXP ? (int)XLENGTH(hidden) : 0;
    int labels_right = Rf_asLogical(rope_get(spec, "labels_right")) == TRUE;
    int gap = Rf_asInteger(rope_get(spec, "gap"));
    const char *dots = rope_get_str(spec, "dots");
    const char *ell = rope_get_str(spec, "ell");
    const char *note = rope_get_str(spec, "note");

    int lw = 1;
    for (int r = 0; r < nrows; r++)
        if (INTEGER(labels_w)[r] > lw) lw = INTEGER(labels_w)[r];

    struct rope_col *c = (struct rope_col *)R_alloc((size_t)(ncols ? ncols : 1), sizeof *c);
    int ok = 0;
    for (int j = 0; j < ncols; j++)
        if (rope_col_read(VECTOR_ELT(cols, j), nrows, &c[ok])) ok++;
    ncols = ok;

    /* The first column is always shown, even if it overflows. */
    int shown = 0;
    long used = lw;
    while (shown < ncols && (shown == 0 || used + 1 + c[shown].width <= width))
        used += 1 + c[shown++].width;

    rope_buf_cell(b, rope_get_str(spec, "title"), 0, 0, 0, dim);
    rope_buf_eol(b);
    if (shown > 0) {
        rope_buf_pad(b, lw);
        for (int k = 0; k < shown; k++) {
            rope_buf_put(b, " ", 1);
            rope_buf_cell(b, c[k].name, c[k].name_w, c[k].width, c[k].right, bold);
        }
        rope_buf_eol(b);
        rope_buf_pad(b, lw);
        for (int k = 0; k < shown; k++) {
            rope_buf_put(b, " ", 1);
            rope_buf_cell(b, c[k].type, c[k].type_w, c[k].width, c[k].right, dim);
        }
        rope_buf_eol(b);
    }
    for (int r = 0; r < nrows; r++) {
        rope_buf_cell(b, Rf_translateChar(STRING_ELT(labels, r)), INTEGER(labels_w)[r],
                      lw, labels_right, dim);
        for (int k = 0; k < shown; k++) {
            rope_buf_put(b, " ", 1);
            rope_buf_cell(b, Rf_translateChar(STRING_ELT(c[k].cells, r)),
                          INTEGER(c[k].cells_w)[r], c[k].width, c[k].right,
                          LOGICAL(c[k].na)[r] == TRUE ? red : NULL);
        }
        rope_buf_eol(b);
        if (r + 1 == gap) {             /* rows left out between head and tail */
            rope_buf_cell(b, dots, 1, lw, labels_right, dim);
            for (int k = 0; k < shown; k++) {
                rope_buf_put(b, " ", 1);
                rope_buf_cell(b, dots, 1, c[k].width, c[k].right, dim);
            }
            rope_buf_eol(b);
        }
    }

    if (*note) {
        if (dim) rope_buf_str(b, dim);
        rope_buf_str(b, "# ");
        rope_buf_str(b, note);
        if (dim) rope_buf_str(b, ROPE_SGR_OFF);
        rope_buf_eol(b);
    }

    /* Columns that did not fit: named, wrapped to the width, three lines at most. */
    int more = ncols - shown + nhidden;
    if (more > 0) {
        char head[64];
        snprintf(head, sizeof head, "# %d more column%s: ", more, more == 1 ? "" : "s");
        if (dim) rope_buf_str(b, dim);
        rope_buf_str(b, head);
        long pos = (long)strlen(head);
        int lines = 1;
        for (int i = 0; i < more; i++) {
            char item[512];
            if (i < ncols - shown)
                snprintf(item, sizeof item, "%s %s", c[shown + i].name, c[shown + i].type);
            else
                snprintf(item, sizeof item, "%s",
                         Rf_translateChar(STRING_ELT(hidden, i - (ncols - shown))));
            long w = rope_utf8_width(item) + (i + 1 < more ? 1 : 0);
            if (i > 0 && pos + 1 + w > width) {
                if (lines == 3) {
                    rope_buf_str(b, " ");
                    rope_buf_str(b, ell);
                    break;
                }
                rope_buf_eol(b);
                rope_buf_str(b, "#   ");
                pos = 4;
                lines++;
            } else if (i > 0) {
                rope_buf_put(b, " ", 1);
                pos++;
            }
            rope_buf_str(b, item);
            if (i + 1 < more) rope_buf_put(b, ",", 1);
            pos += w;
        }
        if (dim) rope_buf_str(b, ROPE_SGR_OFF);
        rope_buf_eol(b);
    }
}

/*
 * Show `text` in the pager: $ROPE_PAGER, else $PAGER, else less. The text goes
 * through a temporary file rather than a pipe, so quitting the pager early
 * cannot raise SIGPIPE in R. less gets LESS=RSXK unless the user set LESS:
 * colour passes through, long lines scroll sideways instead of wrapping, the
 * page stays on screen afterwards and Ctrl-C quits.
 *
 * R's SIGINT handler stays installed. A Ctrl-C typed in the pager reaches R
 * too, as a pending interrupt; it was meant for the pager, so it is dropped
 * once the pager has exited.
 *
 * When standard output is not a terminal the text is written straight out.
 */
static void rope_page(const char *text)
{
    if (!rope_term_usable()) {
        Rprintf("%s", text);
        return;
    }

    const char *pager = getenv("ROPE_PAGER");
    if (!pager || !*pager) pager = getenv("PAGER");
    if (!pager || !*pager) pager = "less";

    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/rope-page-XXXXXX", R_TempDir ? R_TempDir : "/tmp");
    int fd = mkstemp(path);
    if (fd < 0) {
        REprintf("rope: cannot create a file for the pager: %s\n", strerror(errno));
        Rprintf("%s", text);
        return;
    }
    size_t len = strlen(text), off = 0;
    while (off < len) {
        ssize_t k = write(fd, text + off, len - off);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;
        off += (size_t)k;
    }
    close(fd);

    /* The pager command is the user's, run by the shell, reading the file as
     * its standard input so that less has no file name to show. */
    char cmd[4096];
    snprintf(cmd, sizeof cmd, "%s < \"$1\"", pager);
    fflush(stdout);
    fflush(stderr);
    int status = -1;
    pid_t pid = fork();
    if (pid == 0) {
        setenv("LESS", "RSXK", 0);
        execl("/bin/sh", "sh", "-c", cmd, "rope-pager", path, (char *)NULL);
        _exit(127);
    }
    if (pid > 0) {
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) ;
        R_interrupts_pending = 0;
    }
    unlink(path);
    if (pid < 0 || (WIFEXITED(status) && WEXITSTATUS(status) == 127)) {
        REprintf("rope: could not run the pager '%s'\n", pager);
        Rprintf("%s", text);
    }
}

/* rope_frame_show(spec, page): print or page a frame the R side formatted. */
static SEXP rope_frame_show(SEXP spec, SEXP page)
{
    struct rope_buf b = { NULL, 0, 0 };
    rope_frame_render(spec, &b);
    if (b.p) {
        if (Rf_asLogical(page) == TRUE) rope_page(b.p);
        else Rprintf("%s", b.p);
    }
    free(b.p);
    return R_NilValue;
}

/* rope_page_text(lines): page printed output, one element per line. */
static SEXP rope_page_text(SEXP lines)
{
    if (TYPEOF(lines) != STRSXP) return R_NilValue;
    struct rope_buf b = { NULL, 0, 0 };
    for (R_xlen_t i = 0; i < XLENGTH(lines); i++) {
        rope_buf_str(&b, Rf_translateChar(STRING_ELT(lines, i)));
        rope_buf_put(&b, "\n", 1);
    }
    if (b.p) rope_page(b.p);
    free(b.p);
    return R_NilValue;
}

/* ---- evaluating R code ourselves ------------------------------------------ */

/*
 * .Last.value lives in the base environment behind a locked binding. R's own
 * REPL sets it directly; from outside we have to unlock, assign and relock.
 */
static void rope_set_last_value(SEXP value)
{
    R_unLockBinding(R_LastvalueSymbol, R_BaseEnv);
    Rf_defineVar(R_LastvalueSymbol, value, R_BaseEnv);
    R_LockBinding(R_LastvalueSymbol, R_BaseEnv);
}

static void rope_print_cb(void *data)
{
    Rf_PrintValue((SEXP)data);
}

/* Show a value in the pager, through .rope_page in the rope environment. */
static void rope_page_cb(void *data)
{
    SEXP fn = rope_env ? Rf_findVarInFrame(rope_env, Rf_install(".rope_page")) : R_UnboundValue;
    if (TYPEOF(fn) != CLOSXP) {
        Rf_PrintValue((SEXP)data);
        return;
    }
    /* quote() so that a language object is passed as itself, not evaluated */
    SEXP call = PROTECT(Rf_lang2(fn, Rf_lang2(Rf_install("quote"), (SEXP)data)));
    Rf_eval(call, R_GlobalEnv);
    UNPROTECT(1);
}

/*
 * Evaluate one expression under a fresh top-level context so that errors and
 * interrupts unwind to us instead of to R's REPL. This is what R_tryEval does,
 * except that R_ToplevelExec restores R_Visible on the way out, so the flag
 * has to be captured inside the callback, before that happens.
 */
struct rope_eval {
    SEXP expr, env, value;
    Rboolean visible;
};

static void rope_eval_cb(void *data)
{
    struct rope_eval *ev = data;
    ev->value = Rf_eval(ev->expr, ev->env);
    ev->visible = R_Visible;
    R_PreserveObject(ev->value);
}

/*
 * Returns 1 on success, 0 if evaluation errored or was interrupted. On success
 * *value is kept alive by R_PreserveObject; the caller must R_ReleaseObject it.
 */
static int rope_eval_one(SEXP expr, SEXP env, SEXP *value, Rboolean *visible)
{
    struct rope_eval ev = { expr, env, R_NilValue, FALSE };
    if (!R_ToplevelExec(rope_eval_cb, &ev)) return 0;
    *value = ev.value;
    *visible = ev.visible;
    return 1;
}

struct rope_clock { double wall, user, sys; };

static struct rope_clock rope_clock_now(void)
{
    struct rope_clock c = { 0, 0, 0 };
    struct timespec ts;
    struct rusage ru;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        c.wall = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        c.user = (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1e6;
        c.sys  = (double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec / 1e6;
    }
    return c;
}

/*
 * Parse `text` and evaluate each expression in the global environment the way
 * the top level would: set .Last.value, autoprint visible results, stop at the
 * first error. Errors are reported by R itself. Visible results are shown with
 * `show` (called under R_ToplevelExec). If `secs` is non-null it receives the
 * time spent evaluating (showing excluded).
 *
 * Returns 1 if everything parsed and ran, 0 otherwise.
 */
static int rope_eval_text(const char *text, struct rope_clock *secs,
                          void (*show)(void *))
{
    ParseStatus status;
    SEXP src = PROTECT(Rf_mkString(text));
    SEXP exprs = PROTECT(R_ParseVector(src, -1, &status, R_NilValue));
    int ok = 0;

    switch (status) {
    case PARSE_OK:
        ok = 1;
        break;
    case PARSE_INCOMPLETE:
        REprintf("rope: incomplete expression\n");
        break;
    default:
        REprintf("rope: parse error: %s\n", R_ParseErrorMsg);
        break;
    }

    if (secs) secs->wall = secs->user = secs->sys = 0;

    for (R_xlen_t i = 0; ok && i < XLENGTH(exprs); i++) {
        SEXP value;
        Rboolean visible;
        struct rope_clock t0 = rope_clock_now();
        ok = rope_eval_one(VECTOR_ELT(exprs, i), R_GlobalEnv, &value, &visible);
        struct rope_clock t1 = rope_clock_now();
        if (secs) {
            secs->wall += t1.wall - t0.wall;
            secs->user += t1.user - t0.user;
            secs->sys  += t1.sys  - t0.sys;
        }
        if (!ok) break;
        PROTECT(value);
        R_ReleaseObject(value);
        rope_set_last_value(value);
        if (visible) {
            R_ToplevelExec(show, value);
            if (value != R_NilValue) rope_out_push(rope_input_n, VECTOR_ELT(exprs, i), value);
        }
        UNPROTECT(1);
    }

    UNPROTECT(2);
    return ok;
}

/* ---- front-end commands --------------------------------------------------- */

/*
 * A command receives the rest of the line after its name, with surrounding
 * blanks stripped. It returns 1 if it evaluated R code, so that the caller can
 * let R flush deferred warnings afterwards, 0 otherwise.
 */
struct rope_cmd {
    const char *name;
    const char *usage;
    const char *help;
    int (*run)(const struct rope_cmd *self, const char *arg);
};

static const struct rope_cmd rope_cmds[];

static void rope_fmt_secs(double s, char *out, size_t n)
{
    const char *unit;
    double v;
    if (s < 0) s = 0;
    if      (s < 1e-6) { v = s * 1e9; unit = "ns"; }
    else if (s < 1e-3) { v = s * 1e6; unit = "\xc2\xb5s"; }
    else if (s < 1)    { v = s * 1e3; unit = "ms"; }
    else if (s < 60)   { v = s;       unit = "s";  }
    else {
        snprintf(out, n, "%d min %.1f s", (int)(s / 60), fmod(s, 60));
        return;
    }
    if      (v < 10)  snprintf(out, n, "%.2f %s", v, unit);
    else if (v < 100) snprintf(out, n, "%.1f %s", v, unit);
    else              snprintf(out, n, "%.0f %s", v, unit);
}

static int rope_cmd_time(const struct rope_cmd *self, const char *arg)
{
    if (!*arg) {
        REprintf("rope: usage: %s\n", self->usage);
        return 0;
    }
    struct rope_clock t;
    if (rope_eval_text(arg, &t, rope_print_cb)) {
        char wall[32], user[32], sys[32];
        rope_fmt_secs(t.wall, wall, sizeof wall);
        rope_fmt_secs(t.user, user, sizeof user);
        rope_fmt_secs(t.sys, sys, sizeof sys);
        Rprintf("elapsed %s (user %s, system %s)\n", wall, user, sys);
    }
    return 1;
}

/* With no argument, the last value is shown again. */
static int rope_cmd_page(const struct rope_cmd *self, const char *arg)
{
    (void)self;
    rope_eval_text(*arg ? arg : ".Last.value", NULL, rope_page_cb);
    return 1;
}

static int rope_cmd_help(const struct rope_cmd *self, const char *arg)
{
    (void)self; (void)arg;
    int width = 0;
    for (const struct rope_cmd *c = rope_cmds; c->name; c++) {
        int w = (int)strlen(c->usage);
        if (w > width) width = w;
    }
    Rprintf("rope commands (a line starting with %% never reaches R):\n");
    for (const struct rope_cmd *c = rope_cmds; c->name; c++)
        Rprintf("  %-*s  %s\n", width, c->usage, c->help);
    return 0;
}

static const struct rope_cmd rope_cmds[] = {
    { "time", "%time EXPR", "evaluate EXPR, print its value and how long it took",
      rope_cmd_time },
    { "page", "%page [EXPR]", "show the value of EXPR (default: the last value) in the pager",
      rope_cmd_page },
    { "help", "%help",      "list these commands", rope_cmd_help },
    { NULL, NULL, NULL, NULL }
};

static int rope_is_continue_prompt(const char *prompt)
{
    SEXP opt = Rf_GetOption1(Rf_install("continue"));
    if (TYPEOF(opt) != STRSXP || XLENGTH(opt) < 1) return 0;
    return strcmp(prompt, CHAR(STRING_ELT(opt, 0))) == 0;
}

/*
 * Look at a line destined for R. If it is a front-end command, run it and
 * replace the line with what R should see instead. Lines typed at the
 * continuation prompt are part of an unfinished R expression and are always
 * left alone.
 *
 * `buf` holds the line terminated by '\n' and NUL, and has room for `len`
 * bytes. Returns 1 if the line was a command, 0 if it is R's.
 */
static int rope_intercept(const char *prompt, unsigned char *buf, int len)
{
    char *line = (char *)buf;
    while (*line == ' ' || *line == '\t') line++;
    if (*line != '%') return 0;
    if (rope_is_continue_prompt(prompt)) return 0;

    /* Split "%name arg..." into its parts, in place. */
    char *name = line + 1;
    char *arg = name;
    while (*arg && !isspace((unsigned char)*arg)) arg++;
    if (*arg) *arg++ = '\0';
    while (isspace((unsigned char)*arg)) arg++;
    char *end = arg + strlen(arg);
    while (end > arg && isspace((unsigned char)end[-1])) *--end = '\0';

    int evaluated = 0;
    const struct rope_cmd *cmd = NULL;
    if (*name) {
        for (const struct rope_cmd *c = rope_cmds; c->name; c++)
            if (strcmp(c->name, name) == 0) { cmd = c; break; }
    }
    if (cmd) {
        evaluated = cmd->run(cmd, arg);
    } else if (*name) {
        REprintf("rope: unknown command %%%s (try %%help)\n", name);
    } else {
        rope_cmd_help(NULL, "");
    }

    /*
     * Hand R something harmless in place of the line. After we evaluated code
     * ourselves, make it an expression rather than a blank so that R's loop
     * prints any deferred warnings now instead of after the next real input.
     */
    snprintf((char *)buf, (size_t)len, "%s\n", evaluated ? "invisible(.Last.value)" : "");
    rope_out_skip_next = evaluated;
    return 1;
}

/*
 * A line has been read for R. Number it if it answers a REPL prompt, and then
 * see whether it is a front-end command. Reads made while R is busy come from
 * readline() or the like inside an evaluation: they are data, never prompts
 * and never commands.
 */
static void rope_line_read(const char *prompt, unsigned char *buf, int len)
{
    if (rope_busy) return;
    if (!rope_is_continue_prompt(prompt)) {
        int depth = 0;
        sscanf(prompt, "Browse[%d]> ", &depth);
        if (depth < 0) depth = 0;
        if (depth > ROPE_MAX_DEPTH) depth = ROPE_MAX_DEPTH;
        rope_evaldepth = depth;        /* anything deeper died in an error */
        const char *p = (const char *)buf;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '\n' && *p != '\0') rope_input_n++;
    }
    rope_intercept(prompt, buf, len);
}

/* ---- line editor ---------------------------------------------------------- */

/*
 * Input comes through isocline (vendored under src/isocline). It owns the
 * terminal while a line is being edited: the tty is in raw mode with ISIG
 * off, so Ctrl-C arrives as a keypress and cancels the line instead of
 * raising SIGINT. R's signal handlers stay installed throughout and take
 * over the moment the line is handed back and evaluation starts.
 */

static const char *rope_edit_prompt = "";  /* R's prompt for the line being edited */

static const char *rope_continue_prompt(void)
{
    SEXP opt = Rf_GetOption1(Rf_install("continue"));
    if (TYPEOF(opt) != STRSXP || XLENGTH(opt) < 1) return "+ ";
    return CHAR(STRING_ELT(opt, 0));
}

/*
 * What the user sees. R's top-level prompt gets the number of the line about
 * to be typed in front of it, so that "[7]> " and Out[[7]] match. Browser
 * prompts, continuation prompts and readline() prompts are shown as R sent
 * them. isocline reads the prompt as markup, so '[' has to be escaped.
 */
static void rope_display_prompt(const char *prompt, char *out, size_t n)
{
    SEXP opt = Rf_GetOption1(Rf_install("prompt"));
    int top = !rope_busy && TYPEOF(opt) == STRSXP && XLENGTH(opt) > 0 &&
              strcmp(prompt, CHAR(STRING_ELT(opt, 0))) == 0;
    size_t i = 0;
    if (top && n > 16) i = (size_t)snprintf(out, n, "\\[%d]", rope_input_n + 1);
    for (const char *p = prompt; *p && i + 2 < n; p++) {
        if (*p == '[') out[i++] = '\\';
        out[i++] = *p;
    }
    out[i] = '\0';
}

/*
 * Enter submits the buffer only when R could parse it completely. Anything
 * unfinished, an open brace or quote or a trailing operator, gets a newline
 * instead, so a whole expression is edited as one unit and R never has to
 * ask for a continuation. Command lines and lines read as data are taken as
 * typed, and so is a line at R's own continuation prompt: it is a fragment.
 */
struct rope_parse_check { const char *text; ParseStatus status; };

static void rope_parse_check_cb(void *data)
{
    struct rope_parse_check *pc = data;
    SEXP src = PROTECT(Rf_mkString(pc->text));
    R_ParseVector(src, -1, &pc->status, R_NilValue);
    UNPROTECT(1);
}

static bool rope_is_complete(const char *input, void *arg)
{
    (void)arg;
    if (rope_busy || rope_is_continue_prompt(rope_edit_prompt)) return true;
    const char *p = input;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '%') return true;
    struct rope_parse_check pc = { input, PARSE_OK };
    if (!R_ToplevelExec(rope_parse_check_cb, &pc)) return true;
    return pc.status != PARSE_INCOMPLETE;
}

/*
 * Completion is R's own. .rope_complete in the rope environment drives
 * utils:::.completeToken and returns the token being completed and the
 * candidates; each candidate replaces the token. R counts characters where
 * isocline counts bytes, so the cursor position is converted.
 */
static void rope_completer(ic_completion_env_t *cenv, const char *prefix)
{
    (void)prefix;
    if (!rope_env) return;
    long cursor = 0;
    const char *input = ic_completion_input(cenv, &cursor);
    if (!input) return;
    int nchars = 0;
    for (long i = 0; i < cursor && input[i]; i++)
        if ((input[i] & 0xC0) != 0x80) nchars++;

    SEXP fn = Rf_findVarInFrame(rope_env, Rf_install(".rope_complete"));
    if (fn == R_UnboundValue || TYPEOF(fn) != CLOSXP) return;
    SEXP call = PROTECT(Rf_lang3(fn, Rf_mkString(input), Rf_ScalarInteger(nchars)));
    int err = 0;
    SEXP res = R_tryEvalSilent(call, R_GlobalEnv, &err);
    if (!err && res != NULL && TYPEOF(res) == VECSXP && XLENGTH(res) == 2) {
        PROTECT(res);
        SEXP token = VECTOR_ELT(res, 0), comps = VECTOR_ELT(res, 1);
        if (TYPEOF(token) == STRSXP && XLENGTH(token) == 1 && TYPEOF(comps) == STRSXP) {
            const char *tok = Rf_translateCharUTF8(STRING_ELT(token, 0));
            size_t tlen = strlen(tok);
            for (R_xlen_t i = 0; i < XLENGTH(comps); i++) {
                const char *c = Rf_translateCharUTF8(STRING_ELT(comps, i));
                if (strncmp(c, tok, tlen) != 0) continue;
                if (!ic_add_completion_prim(cenv, c, NULL, NULL, (long)tlen, 0)) break;
            }
        }
        UNPROTECT(1);
    }
    UNPROTECT(1);
}

static void rope_no_completer(ic_completion_env_t *cenv, const char *prefix)
{
    (void)cenv; (void)prefix;
}

/*
 * Syntax colour: a small R lexer that only has to be right about where
 * strings, comments and numbers start and end. Everything it does not
 * recognise is left in the default colour.
 */
static const char *const rope_keywords[] = {
    "if", "else", "repeat", "while", "function", "for", "next", "break", "in", NULL
};
static const char *const rope_constants[] = {
    "TRUE", "FALSE", "NULL", "NA", "Inf", "NaN",
    "NA_integer_", "NA_real_", "NA_character_", "NA_complex_", NULL
};

static int rope_in_list(const char *s, size_t n, const char *const *list)
{
    for (; *list; list++)
        if (strlen(*list) == n && memcmp(*list, s, n) == 0) return 1;
    return 0;
}

static int rope_is_ident(unsigned char c)
{
    return isalnum(c) || c == '.' || c == '_' || c >= 0x80;
}

/* s[i] is r or R and s[i+1] a quote. Index just past the raw string, or -1
 * if it is not one. An unterminated raw string runs to the end. */
static long rope_raw_string_end(const char *s, long i, long n)
{
    char q = s[i + 1];
    long j = i + 2;
    int dashes = 0;
    while (j < n && s[j] == '-') { dashes++; j++; }
    if (j >= n) return -1;
    char close;
    switch (s[j]) {
    case '(': close = ')'; break;
    case '[': close = ']'; break;
    case '{': close = '}'; break;
    default: return -1;
    }
    for (j++; j < n; j++) {
        if (s[j] != close) continue;
        long k = j + 1;
        int d = 0;
        while (k < n && s[k] == '-' && d < dashes) { d++; k++; }
        if (d == dashes && k < n && s[k] == q) return k + 1;
    }
    return n;
}

static void rope_highlight(ic_highlight_env_t *henv, const char *input, void *arg)
{
    (void)arg;
    long n = (long)strlen(input), i = 0;

    /* A front-end command: colour its name, then the rest as R. */
    while (i < n && (input[i] == ' ' || input[i] == '\t')) i++;
    if (i < n && input[i] == '%') {
        long j = i + 1;
        while (j < n && !isspace((unsigned char)input[j])) j++;
        ic_highlight(henv, i, j - i, "rope-command");
        i = j;
    } else {
        i = 0;
    }

    while (i < n) {
        unsigned char c = (unsigned char)input[i];
        if (c == '#') {
            long j = i;
            while (j < n && input[j] != '\n') j++;
            ic_highlight(henv, i, j - i, "rope-comment");
            i = j;
        } else if ((c == 'r' || c == 'R') && i + 1 < n &&
                   (input[i + 1] == '"' || input[i + 1] == '\'') &&
                   (i == 0 || !rope_is_ident((unsigned char)input[i - 1])) &&
                   rope_raw_string_end(input, i, n) > 0) {
            long j = rope_raw_string_end(input, i, n);
            ic_highlight(henv, i, j - i, "rope-string");
            i = j;
        } else if (c == '"' || c == '\'' || c == '`') {
            long j = i + 1;
            while (j < n && input[j] != (char)c) {
                if (input[j] == '\\' && c != '`' && j + 1 < n) j++;
                j++;
            }
            if (j < n) j++;
            if (c != '`') ic_highlight(henv, i, j - i, "rope-string");
            i = j;
        } else if (isdigit(c) || (c == '.' && i + 1 < n && isdigit((unsigned char)input[i + 1]))) {
            long j = i;
            int hex = input[i] == '0' && i + 1 < n && (input[i + 1] == 'x' || input[i + 1] == 'X');
            while (j < n) {
                unsigned char d = (unsigned char)input[j];
                if (isalnum(d) || d == '.') j++;
                else if ((d == '+' || d == '-') && !hex && j > i &&
                         (input[j - 1] == 'e' || input[j - 1] == 'E')) j++;
                else break;
            }
            ic_highlight(henv, i, j - i, "rope-number");
            i = j;
        } else if (rope_is_ident(c)) {
            long j = i;
            while (j < n && rope_is_ident((unsigned char)input[j])) j++;
            if (rope_in_list(input + i, (size_t)(j - i), rope_keywords))
                ic_highlight(henv, i, j - i, "rope-keyword");
            else if (rope_in_list(input + i, (size_t)(j - i), rope_constants))
                ic_highlight(henv, i, j - i, "rope-constant");
            i = j;
        } else if (c == '%') {
            long j = i + 1;
            while (j < n && input[j] != '%' && input[j] != '\n') j++;
            if (j < n && input[j] == '%') {
                ic_highlight(henv, i, j - i + 1, "rope-operator");
                i = j + 1;
            } else {
                i++;
            }
        } else if (strncmp(input + i, "<<-", 3) == 0 || strncmp(input + i, "->>", 3) == 0) {
            ic_highlight(henv, i, 3, "rope-operator");
            i += 3;
        } else if (strncmp(input + i, "<-", 2) == 0 || strncmp(input + i, "->", 2) == 0 ||
                   strncmp(input + i, "|>", 2) == 0) {
            ic_highlight(henv, i, 2, "rope-operator");
            i += 2;
        } else {
            i++;
        }
    }
}

static void rope_no_highlight(ic_highlight_env_t *henv, const char *input, void *arg)
{
    (void)henv; (void)input; (void)arg;
}

static void rope_editor_init(void)
{
    ic_set_prompt_marker("", "");          /* prompts arrive whole from R */
    ic_enable_hint(false);                 /* a hint would run R's completer on every pause */
    ic_enable_brace_insertion(false);      /* no auto-closing: type what you mean */
    ic_set_default_completer(rope_completer, NULL);
    ic_set_default_highlighter(rope_highlight, NULL);
    ic_set_is_complete(rope_is_complete, NULL);
    ic_style_def("rope-keyword",  "ansi-magenta");
    ic_style_def("rope-constant", "ansi-cyan");
    ic_style_def("rope-string",   "ansi-green");
    ic_style_def("rope-number",   "ansi-yellow");
    ic_style_def("rope-comment",  "ansi-darkgray");
    ic_style_def("rope-operator", "ansi-magenta");
    ic_style_def("rope-command",  "bold ansi-cyan");
}

/* ---- console hooks -------------------------------------------------------- */

/*
 * Read one line for R. Returns 0 on EOF, 1 otherwise. The buffer handed to R
 * must end in '\n'. A multi-line expression from the editor is handed over
 * whole: R's REPL consumes its buffer one line at a time and only asks for
 * more input once the buffer is empty.
 */
static int rope_editor_usable(void)
{
    const char *term = getenv("TERM");
    return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO) &&
           term && *term && strcmp(term, "dumb") != 0;
}

static int rope_read_console(const char *prompt, unsigned char *buf, int len,
                             int addtohistory)
{
    char *line = NULL;

    if (!rope_editor_usable()) {
        /* Piped input or a dumb terminal: no editor, just lines. */
        if (isatty(STDIN_FILENO)) {
            fputs(prompt, stdout);
            fflush(stdout);
        }
        if (!fgets((char *)buf, len, stdin)) return 0;
        size_t n = strlen((char *)buf);
        if (n == 0 || buf[n - 1] != '\n') {
            if (n >= (size_t)len - 1) {
                int c;
                while ((c = getchar()) != EOF && c != '\n') ;
                n = (size_t)len - 2;
            }
            buf[n] = '\n';
            buf[n + 1] = '\0';
        }
        rope_line_read(prompt, buf, len);
        return 1;
    }

    /*
     * Show R's prompt, numbered when it is the top-level one. Further rows of
     * a multi-line expression carry R's continuation prompt.
     */
    char shown[512];
    rope_display_prompt(prompt, shown, sizeof shown);
    ic_set_prompt_marker("", rope_continue_prompt());
    rope_edit_prompt = prompt;

    for (;;) {
        if (rope_busy)      /* readline() and friends: plain text, no R help */
            line = ic_readline_ex(shown, rope_no_completer, NULL, rope_no_highlight, NULL);
        else
            line = ic_readline(shown);
        if (!line) {                     /* Ctrl-D on an empty line */
            fputc('\n', stdout);
            return 0;
        }
        if (ic_readline_cancelled()) {   /* Ctrl-C: the editor said ^C, ask again */
            ic_free(line);
            continue;
        }
        break;
    }

    size_t n = strlen(line);
    if (n > (size_t)len - 2) {
        fprintf(stderr, "rope: line truncated to %d bytes\n", len - 2);
        n = (size_t)len - 2;
    }
    memcpy(buf, line, n);
    buf[n] = '\n';
    buf[n + 1] = '\0';

    /*
     * isocline files every line it returns, except empty and one-character
     * ones. R says when a line answers readline() inside an evaluation rather
     * than the REPL: that is data, not history.
     */
    if (!addtohistory && n > 1) ic_history_remove_last();
    ic_free(line);
    rope_line_read(prompt, buf, len);
    return 1;
}

static void rope_write_console_ex(const char *buf, int len, int otype)
{
    FILE *f = otype ? stderr : stdout;
    fwrite(buf, 1, (size_t)len, f);
    fflush(f);
}

static void rope_show_message(const char *msg)
{
    fputs(msg, stderr);
    fputc('\n', stderr);
    fflush(stderr);
}

/*
 * R's REPL calls this with 1 just before it evaluates a line and with 0 just
 * before it reads one. A browser command such as `c` is not an evaluation and
 * never reaches the 1.
 */
static void rope_busy_hook(int which)
{
    rope_busy = which != 0;
    if (which && rope_evaldepth < ROPE_MAX_DEPTH)
        rope_evaln[rope_evaldepth++] = rope_input_n;
}

static void rope_cleanup(SA_TYPE saveact, int status, int run_last)
{
    if (rope_default_cleanup) rope_default_cleanup(saveact, status, run_last);
    else exit(status);
}

/* ---- main ----------------------------------------------------------------- */

int main(int argc, char **argv)
{
    setenv("R_HOME", ROPE_R_HOME, 0);

    /* R's defaults first, then whatever the user passed through. */
    static const char *base[] = { "rope", "--quiet", "--no-save", "--no-restore" };
    int nbase = (int)(sizeof base / sizeof base[0]);
    char **rargv = calloc((size_t)(nbase + argc), sizeof *rargv);
    if (!rargv) { perror("rope"); return 1; }
    int rargc = 0;
    for (int i = 0; i < nbase; i++) rargv[rargc++] = (char *)base[i];
    for (int i = 1; i < argc; i++) rargv[rargc++] = argv[i];

    /* We really are the main program: R can measure the C stack from here. */
    R_running_as_main_program = 1;
    Rf_initialize_R(rargc, rargv);

    R_Interactive = TRUE;
    R_Outputfile = NULL;
    R_Consolefile = NULL;

    rope_default_cleanup = ptr_R_CleanUp;
    ptr_R_ReadConsole = rope_read_console;
    ptr_R_WriteConsole = NULL;
    ptr_R_WriteConsoleEx = rope_write_console_ex;
    ptr_R_ShowMessage = rope_show_message;
    ptr_R_Busy = rope_busy_hook;
    ptr_R_CleanUp = rope_cleanup;

    rope_history_init();
    rope_editor_init();

    setup_Rmainloop();
    rope_out_init();
    run_Rmainloop();

    /* Normally unreachable: q() exits through rope_cleanup. */
    Rf_endEmbeddedR(0);
    free(rargv);
    return 0;
}
