# Rope

Rope is a [REPL](https://en.wikipedia.org/wiki/Read%E2%80%93eval%E2%80%93print_loop) for the R programming language.
It has support for timing, output history, object inspection and inline graphics.
Rope also contains terminal-aware data frame rendering and a debugger front end.

REPLs are great tools for testing code interactively, benchmarking, and debugging.
My goal with Rope was to have an R-REPL with no extra dependencies or runtimes.
That means it should run on a vanilla R install with the features expected from a modern REPL: tab completion,
syntax colouring, multi-line editing, history search, etc.

## Getting started

Rope needs R built with a shared `libR` (`R CMD config --ldflags` should list
`-lR`) and a C11 compiler. Everything else is vendored.

    make          # builds ./rope
    ./rope

Arguments after `rope` are passed through to R. `make check` runs the tests.

For the full list of keys, `%` commands and build options, see
[REFERENCE.md](REFERENCE.md).

### Windows

Rope is developed on Linux and uses POSIX terminal and process APIs, so it
does not build natively on Windows. Running it under WSL should work in
principle, but that has not been tested.

## Bug reports

Please open an issue at <https://github.com/vlshields/rope/issues>. It helps
to include:

- the output of `R --version` and your OS and terminal
- the exact input that triggers the problem
- what you expected and what happened instead


## Known issues

- `%time` around code that stops in `browser()` (or in a function under
  `debug()`) includes the time spent paused, and the browser command that
  ends the pause (such as `c`) is replayed at the top-level prompt.
