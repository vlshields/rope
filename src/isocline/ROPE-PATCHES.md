# isocline, as vendored in Rope

Upstream: https://github.com/daanx/isocline (MIT, see LICENSE), `main` branch
at commit 8d6dc1e (re-vendored 2026-09-23 after the first copy was lost).
Layout is upstream's: `include/isocline.h`, `src/*.c`, `src/*.h`.
`src/isocline.c` includes the other sources, so the Makefile compiles that
one file.

Every change to upstream is marked with a `rope:` comment. They are:

- `include/isocline.h`, `src/isocline.c`, `src/env.h`: `ic_set_is_complete()`
  installs a hook that Enter consults; when it says the input is incomplete a
  newline is inserted instead of returning. `ic_readline_cancelled()` tells a
  Ctrl-C (returned as an empty string) from an empty line.
- `src/editline.c`: the Enter handling for the hook; a bare newline byte
  (pasted or typed-ahead text, Ctrl-J) is treated as Enter when the hook is
  set; `^C` is printed when a line is cancelled.
- `src/editline.c`: auto-pairing is rewritten for R (`edit_auto_brace` and a
  small string/comment lexer, `rope_lex_at`): no pairing in strings, comments,
  after a name, or while input is already waiting (pastes); Backspace in an
  empty pair deletes both; Enter in `{}` inserts an indented line.
- `src/tty.c`, `src/tty.h`: `tty_has_pending()` tells whether more input is
  already waiting, for the paste check above.
- `src/completions.c`: `ic_completion_input()` is declared upstream but was
  never defined.
- `src/tty.c`: raw mode is entered and left with `TCSADRAIN` instead of
  `TCSAFLUSH`, so input typed before the prompt appears is not thrown away.
- `src/history.c`: `IC_MAX_HISTORY` can be set from the compiler command line
  (the Makefile sets 10000; upstream hard-codes 200).
