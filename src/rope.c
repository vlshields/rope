/*
 * rope: a terminal REPL for R.
 *
 * Milestone 1: embed R, own the read-eval loop through R's console hooks,
 * use GNU readline for input, and get interrupts right.
 * Milestone 2: intercept lines before R sees them. A line whose first
 * non-blank character is '%' is a front-end command, not R code.
 * Milestone 3: output history. Every visible top-level result is kept in a
 * ring, numbered by the prompt it came from, and exposed to R as Out and In.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include <readline/history.h>
#include <readline/readline.h>

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

#ifndef ROPE_R_HOME
#define ROPE_R_HOME "/usr/lib/R"
#endif

/* ---- state ---------------------------------------------------------------- */

static char rope_histfile[4096];
static volatile sig_atomic_t rope_in_readline = 0;
static sigjmp_buf rope_jmp;
static void (*rope_default_cleanup)(SA_TYPE, int, int) = NULL;

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
    using_history();
    stifle_history(10000);
    read_history(rope_histfile);
}

static void rope_history_save(void)
{
    if (rope_histfile[0]) write_history(rope_histfile);
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

#define ROPE_FN(f) ((DL_FUNC)(void (*)(void))(f))

static const R_CallMethodDef rope_call_methods[] = {
    { "rope_out_record",   ROPE_FN(rope_out_record),   3 },
    { "rope_out_lookup",   ROPE_FN(rope_out_lookup),   1 },
    { "rope_out_numbers",  ROPE_FN(rope_out_numbers),  0 },
    { "rope_input_number", ROPE_FN(rope_input_number), 0 },
    { NULL, NULL, 0 }
};

/*
 * The R half: an environment attached to the search path as "rope" holding
 * Out and In. Out[[n]] is the result of prompt n, Out[[-1]] the newest stored
 * result; In[[n]] is the expression. Out[i] and In[i] give named lists, and
 * with no index everything stored.
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
    "  attach(env, name = 'rope', warn.conflicts = FALSE)\n"
    "  addTaskCallback(function(expr, value, ok, visible)\n"
    "    .Call('rope_out_record', expr, value, visible), name = 'rope')\n"
    "})\n";

static void rope_setup_cb(void *data)
{
    SEXP exprs = data;
    for (R_xlen_t i = 0; i < XLENGTH(exprs); i++)
        Rf_eval(VECTOR_ELT(exprs, i), R_GlobalEnv);
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
 * first error. Errors are reported by R itself. If `secs` is non-null it
 * receives the time spent evaluating (printing excluded).
 *
 * Returns 1 if everything parsed and ran, 0 otherwise.
 */
static int rope_eval_text(const char *text, struct rope_clock *secs)
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
            R_ToplevelExec(rope_print_cb, value);
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
    if (rope_eval_text(arg, &t)) {
        char wall[32], user[32], sys[32];
        rope_fmt_secs(t.wall, wall, sizeof wall);
        rope_fmt_secs(t.user, user, sizeof user);
        rope_fmt_secs(t.sys, sys, sizeof sys);
        Rprintf("elapsed %s (user %s, system %s)\n", wall, user, sys);
    }
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

/* ---- console hooks -------------------------------------------------------- */

static void rope_sigint(int sig)
{
    (void)sig;
    if (rope_in_readline) siglongjmp(rope_jmp, 1);
}

/*
 * Read one line for R. Returns 0 on EOF, 1 otherwise. The buffer handed to R
 * must end in '\n'.
 *
 * SIGINT while readline is active is ours: we long-jump out of readline,
 * clean up its terminal state, and reprompt. R's own handler is restored on
 * every exit path so that evaluation-time interrupts still reach R.
 */
static int rope_read_console(const char *prompt, unsigned char *buf, int len,
                             int addtohistory)
{
    char *line = NULL;

    if (!isatty(STDIN_FILENO)) {
        /* Piped input: no prompt, no editor, just lines. */
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

    struct sigaction sa_new, sa_old;
    memset(&sa_new, 0, sizeof sa_new);
    sa_new.sa_handler = rope_sigint;
    sigemptyset(&sa_new.sa_mask);
    sigaction(SIGINT, &sa_new, &sa_old);

    for (;;) {
        if (sigsetjmp(rope_jmp, 1) != 0) {
            rope_in_readline = 0;
            rl_free_line_state();
            rl_cleanup_after_signal();
            RL_UNSETSTATE(RL_STATE_ISEARCH | RL_STATE_NSEARCH | RL_STATE_VIMOTION |
                          RL_STATE_NUMERICARG | RL_STATE_MULTIKEY);
            rl_done = 1;
            fputs("^C\n", stdout);
            fflush(stdout);
            continue;
        }
        rope_in_readline = 1;
        line = readline(prompt);
        rope_in_readline = 0;
        break;
    }

    sigaction(SIGINT, &sa_old, NULL);

    if (!line) {
        fputc('\n', stdout);
        return 0;
    }

    size_t n = strlen(line);
    if (n > (size_t)len - 2) {
        fprintf(stderr, "rope: line truncated to %d bytes\n", len - 2);
        n = (size_t)len - 2;
    }
    memcpy(buf, line, n);
    buf[n] = '\n';
    buf[n + 1] = '\0';

    if (addtohistory && n > 0) {
        char *last = NULL;
        HIST_ENTRY *h = history_get(history_length);
        if (h) last = h->line;
        if (!last || strcmp(last, line) != 0) add_history(line);
    }
    free(line);
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
    rope_history_save();
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

    rl_readline_name = "rope";
    rl_catch_signals = 0;
    rope_history_init();

    setup_Rmainloop();
    rope_out_init();
    run_Rmainloop();

    /* Normally unreachable: q() exits through rope_cleanup. */
    rope_history_save();
    Rf_endEmbeddedR(0);
    free(rargv);
    return 0;
}
