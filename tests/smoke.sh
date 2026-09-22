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

# Front-end commands: intercepted before R sees them.
printf '%%time 1 + 1\n%%time invisible(3)\n%%time x <- 40 + 2\nx\n%%time 7 * 6\n.Last.value\n%%time warning("w")\n99\n%%time stop("bad")\n%%time 1 +\n%%time 1 +* 2\n%%time\n%%bogus\n  %%help\nf <- function() {\n%%time 12345\n}\nq()\n' \
    | ROPE_HISTFILE="$tmp/hist" ./rope >"$tmp/out4" 2>"$tmp/err4"
status=$?
[ "$status" -eq 0 ] || { echo "FAIL: commands: exit status $status"; fail=1; }
grep -q '^\[1\] 2$' "$tmp/out4" || { echo "FAIL: %time did not print result"; fail=1; }
grep -q '^elapsed .* (user .*, system .*)$' "$tmp/out4" || { echo "FAIL: %time did not print timing"; fail=1; }
grep -q '^\[1\] 3$' "$tmp/out4" && { echo "FAIL: %time printed an invisible result"; fail=1; }
[ "$(grep -c '^\[1\] 42$' "$tmp/out4")" -eq 3 ] || { echo "FAIL: %time assignment, result or .Last.value wrong"; fail=1; }
grep -q '^\[1\] "w"$' "$tmp/out4" && { echo "FAIL: warning() result should be invisible"; fail=1; }
grep -q 'Warning message' "$tmp/err4" || { echo "FAIL: warning from %time not printed"; fail=1; }
grep -q '^Error: bad$' "$tmp/err4" || { echo "FAIL: error from %time not printed"; fail=1; }
grep -q 'incomplete expression' "$tmp/err4" || { echo "FAIL: incomplete %time expression not reported"; fail=1; }
grep -q "parse error: unexpected '\*'" "$tmp/err4" || { echo "FAIL: %time parse error not reported"; fail=1; }
grep -q 'usage: %time EXPR' "$tmp/err4" || { echo "FAIL: bare %time should print usage"; fail=1; }
grep -q 'unknown command %bogus' "$tmp/err4" || { echo "FAIL: unknown command not reported"; fail=1; }
grep -q '^  %time EXPR' "$tmp/out4" || { echo "FAIL: %help (with leading blanks) not listed"; fail=1; }
grep -q '12345' "$tmp/out4" && { echo "FAIL: command at continuation prompt was intercepted"; fail=1; }
grep -q 'unexpected input' "$tmp/err4" || { echo "FAIL: continuation line did not reach R"; fail=1; }

# Deferred warnings from %time must appear before the next line's output.
printf '%%time warning("w")\n99\nq()\n' | ROPE_HISTFILE="$tmp/hist" ./rope >"$tmp/out5" 2>&1
w=$(grep -n 'Warning message' "$tmp/out5" | head -1 | cut -d: -f1)
n=$(grep -n '^\[1\] 99$' "$tmp/out5" | head -1 | cut -d: -f1)
{ [ -n "$w" ] && [ -n "$n" ] && [ "$w" -lt "$n" ]; } || { echo "FAIL: %time warning printed late"; fail=1; }

# Output history: Out and In.
printf '1 + 1\nx <- 5\ninvisible(9)\nNULL\n%%time 3 * 3\nOut[[1]]\nOut[[2]]\nOut[[5]]\nOut[[-1]]\nlength(Out)\nIn[[5]]\nnames(Out)\nOut[c(1, 5)]\n8; 9\nOut[[-1]]\nq()\n' \
    | ROPE_HISTFILE="$tmp/hist" ./rope >"$tmp/out6" 2>"$tmp/err6"
[ "$(grep -c '^\[1\] 2$' "$tmp/out6")" -eq 3 ] || { echo "FAIL: Out[[1]] wrong"; fail=1; }
grep -q 'Out\[\[2\]\]: nothing stored for input 2' "$tmp/err6" || { echo "FAIL: assignment should not be stored"; fail=1; }
[ "$(grep -c '^\[1\] 9$' "$tmp/out6")" -eq 6 ] || { echo "FAIL: %time result, Out[[-1]] or last-of-line wrong"; fail=1; }
grep -q '^\[1\] 5$' "$tmp/out6" || { echo "FAIL: length(Out) wrong (invisible/NULL stored?)"; fail=1; }
grep -q '^3 \* 3$' "$tmp/out6" || { echo "FAIL: In[[5]] wrong"; fail=1; }
grep -q '^\[1\] "1" *"5" *"6" *"8" *"9" *"10" *"11"$' "$tmp/out6" || { echo "FAIL: names(Out) wrong"; fail=1; }
grep -q '^\$`5`$' "$tmp/out6" || { echo "FAIL: Out[i] not a named list"; fail=1; }

# Results are filed under the prompt that produced them, even from browser().
printf 'f <- function() {\n  browser()\n  "from f"\n}\nf()\n2 + 2\nstop("in browser")\n3 + 3\nc\nstop("top")\nreadline("nm? ")\n%%time 5\nIn[[2]]\nOut[[3]]\nOut[[5]]\nOut[[7]]\nq()\n' \
    | ROPE_HISTFILE="$tmp/hist" ./rope >"$tmp/out7" 2>"$tmp/err7"
grep -q '^f()$' "$tmp/out7" || { echo "FAIL: In[[2]] should be f()"; fail=1; }
grep -q '^\[1\] "%time 5"$' "$tmp/out7" || { echo "FAIL: readline() answer should reach R untouched"; fail=1; }
[ "$(grep -c '^\[1\] 4$' "$tmp/out7")" -eq 2 ] || { echo "FAIL: Out[[3]] from browser wrong"; fail=1; }
[ "$(grep -c '^\[1\] 6$' "$tmp/out7")" -eq 2 ] || { echo "FAIL: Out[[5]] after error in browser wrong"; fail=1; }
[ "$(grep -c '^\[1\] "from f"$' "$tmp/out7")" -eq 1 ] || { echo "FAIL: f() result should be Out[[2]], not Out[[7]]"; fail=1; }
grep -q 'Out\[\[7\]\]: nothing stored' "$tmp/err7" || { echo "FAIL: errored prompt should store nothing"; fail=1; }

# Ring: stored values survive gc(), evicted ones are released.
printf 'e <- new.env(); reg.finalizer(e, function(x) cat("finalized\\n")); e\nrm(e); invisible(gc())\ncat("still held\\n")\noptions(rope.out.size = 2)\n1\n2\ninvisible(gc())\ncat("after eviction\\n")\nnames(Out)\nq()\n' \
    | ROPE_HISTFILE="$tmp/hist" ./rope >"$tmp/out8" 2>"$tmp/err8"
h=$(grep -n '^still held$' "$tmp/out8" | cut -d: -f1)
f=$(grep -n '^finalized$' "$tmp/out8" | cut -d: -f1)
a=$(grep -n '^after eviction$' "$tmp/out8" | cut -d: -f1)
{ [ -n "$h" ] && [ -n "$f" ] && [ -n "$a" ] && [ "$h" -lt "$f" ] && [ "$f" -lt "$a" ]; } \
    || { echo "FAIL: stored value not held, or not released on eviction"; fail=1; }
grep -q '^\[1\] "5" "6"$' "$tmp/out8" || { echo "FAIL: ring size not honoured"; fail=1; }

if [ "$fail" -eq 0 ]; then echo "smoke: ok"; else for f in "$tmp"/out* "$tmp"/err*; do echo "--- $f ---"; cat "$f"; done; fi
exit "$fail"
