#!/bin/sh
# Non-tty smoke test: pipe a few lines through rope and check stdout/stderr split.
set -u
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail=0

printf '1 + 1\nx <- 1:3\nsum(x)\nq()\n' | ROPE_HISTFILE="$tmp/hist" ./rope >"$tmp/out" 2>"$tmp/err"
status=$?
grep -q '^\[1\] 2$' "$tmp/out" || { echo "FAIL: expected [1] 2"; fail=1; }
grep -q '^\[1\] 6$' "$tmp/out" || { echo "FAIL: expected [1] 6"; fail=1; }
[ "$status" -eq 0 ] || { echo "FAIL: exit status $status"; fail=1; }

printf 'log(-1)\nstop("boom")\nq()\n' | ROPE_HISTFILE="$tmp/hist" ./rope >"$tmp/out2" 2>"$tmp/err2"
grep -q 'NaNs produced' "$tmp/err2" || { echo "FAIL: warning not on stderr"; fail=1; }
grep -q 'boom' "$tmp/err2" || { echo "FAIL: error not on stderr"; fail=1; }
grep -q 'boom' "$tmp/out2" && { echo "FAIL: error leaked to stdout"; fail=1; }

printf 'summary(lm(mpg ~ wt, mtcars))\nq()\n' | ROPE_HISTFILE="$tmp/hist" ./rope >"$tmp/out3" 2>/dev/null
grep -q 'Residual standard error' "$tmp/out3" || { echo "FAIL: multi-line output incomplete"; fail=1; }

if [ "$fail" -eq 0 ]; then echo "smoke: ok"; else echo "--- stdout ---"; cat "$tmp/out"; echo "--- stderr ---"; cat "$tmp/err"; fi
exit "$fail"
