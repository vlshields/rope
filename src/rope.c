/*
 * rope: a terminal REPL for R.
 *
 * Milestone 1: embed R, own the read-eval loop through R's console hooks,
 * use GNU readline for input, and get interrupts right.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <readline/history.h>
#include <readline/readline.h>

#include <Rinternals.h>
#include <Rembedded.h>
#include <Rinterface.h>

/* Exported from libR but not declared in any public header. */
extern void run_Rmainloop(void);

#ifndef ROPE_R_HOME
#define ROPE_R_HOME "/usr/lib/R"
#endif

/* ---- state ---------------------------------------------------------------- */

static char rope_histfile[4096];
static volatile sig_atomic_t rope_in_readline = 0;
static sigjmp_buf rope_jmp;
static void (*rope_default_cleanup)(SA_TYPE, int, int) = NULL;

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

static void rope_busy(int which)
{
    (void)which;
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
    ptr_R_Busy = rope_busy;
    ptr_R_CleanUp = rope_cleanup;

    rl_readline_name = "rope";
    rl_catch_signals = 0;
    rope_history_init();

    setup_Rmainloop();
    run_Rmainloop();

    /* Normally unreachable: q() exits through rope_cleanup. */
    rope_history_save();
    Rf_endEmbeddedR(0);
    free(rargv);
    return 0;
}
