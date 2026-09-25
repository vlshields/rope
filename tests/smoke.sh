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

# Data frames: print() goes to base R when stdout is not a terminal; %page
# renders Rope's layout and, with no terminal to page on, writes it out.
printf 'head(mtcars, 2)\n%%page head(mtcars, 2)\n%%page data.frame(x = c(1.5, NA), s = c(NA, "a"))\n%%page 1:3\n%%page\nOut[[-1]]\n%%page stop("nope")\nq()\n' \
    | ROPE_HISTFILE="$tmp/hist" ./rope >"$tmp/out9" 2>"$tmp/err9"
grep -q '^Mazda RX4      21   6  160 110  3.9 2.620 16.46  0  1    4    4$' "$tmp/out9" \
    || { echo "FAIL: piped print of a data frame should be base R's"; fail=1; }
grep -q '^data.frame \[2 .* 11\]$' "$tmp/out9" || { echo "FAIL: %page frame title missing"; fail=1; }
grep -q '^ *mpg *cyl .* carb$' "$tmp/out9" || { echo "FAIL: %page should show every column"; fail=1; }
grep -q '^ *<dbl> *<dbl>' "$tmp/out9" || { echo "FAIL: %page type row missing"; fail=1; }
grep -q '^1   1.5 <NA>$' "$tmp/out9" || { echo "FAIL: %page NA cells wrong"; fail=1; }
grep -q '^  <dbl> <chr>$' "$tmp/out9" || { echo "FAIL: %page chr column not left-aligned"; fail=1; }
[ "$(grep -c '^\[1\] 1 2 3$' "$tmp/out9")" -eq 3 ] || { echo "FAIL: %page of a vector, bare %page or Out wrong"; fail=1; }
grep -q '^Error: nope$' "$tmp/err9" || { echo "FAIL: %page error not reported"; fail=1; }
grep -q "$(printf '\033')" "$tmp/out9" && { echo "FAIL: colour written to a pipe"; fail=1; }

# Inspector: %who lists without forcing promises, %inspect describes.
printf 'x <- 1:5\ndf <- data.frame(a = c(1, NA, 3), s = c("u", "v", "v"))\n%%who\n%%who ^d\n%%inspect df\n%%inspect x * 2\n%%inspect sd\n%%inspect list(p = 1, q = list(r = "z"))\np <- function(a, b) { browser(); a }\np(1 + 2, stop("forced"))\n%%who\n%%inspect b\nc\n%%inspect\n%%inspect 1 +\nq()\n' \
    | ROPE_HISTFILE="$tmp/hist" ./rope >"$tmp/out10" 2>"$tmp/err10"
grep -q '^df  *data.frame \[3 .* 2\]  *[0-9.]* [kB]*B  *a, s$' "$tmp/out10" || { echo "FAIL: %who data frame row"; fail=1; }
grep -q '^x  *int \[5\]  *[0-9]* B  *1 2 3 4 5$' "$tmp/out10" || { echo "FAIL: %who vector row"; fail=1; }
[ "$(grep -c '^x  *int' "$tmp/out10")" -eq 1 ] || { echo "FAIL: %who REGEX should filter"; fail=1; }
grep -q '^  a  *<dbl>  *min 1, median 2, mean 2, max 3, 1 NA$' "$tmp/out10" || { echo "FAIL: %inspect column summary"; fail=1; }
grep -q '^  s  *<chr>  *2 unique$' "$tmp/out10" || { echo "FAIL: %inspect chr column"; fail=1; }
grep -q '^  values  *2 4 6 8 10$' "$tmp/out10" || { echo "FAIL: %inspect of an expression"; fail=1; }
grep -q '^  environment namespace:stats$' "$tmp/out10" || { echo "FAIL: %inspect function environment"; fail=1; }
grep -q '^    \$r  chr \[1\]  "z"$' "$tmp/out10" || { echo "FAIL: %inspect nested list"; fail=1; }
grep -q '^b  *promise  *not yet evaluated: stop("forced")$' "$tmp/out10" || { echo "FAIL: %who should show a promise unforced"; fail=1; }
grep -q '^  expression  stop("forced")$' "$tmp/out10" || { echo "FAIL: %inspect of a promise"; fail=1; }
grep -q 'forced' "$tmp/err10" && { echo "FAIL: a promise was forced"; fail=1; }
grep -q 'usage: %inspect EXPR' "$tmp/err10" || { echo "FAIL: bare %inspect should print usage"; fail=1; }
grep -q 'parse error' "$tmp/err10" || { echo "FAIL: %inspect parse error not reported"; fail=1; }

# Debugger: the source around each stop, the stack, moving between frames and
# evaluating in the selected one.
printf 'g <- function(y) {\n  z <- y * 2\n  browser()\n  z + 1\n}\nf <- function(x) {\n  a <- x + 1\n  g(a)\n}\nf(1)\n%%where\n%%up\na * 100\n%%who\n%%up\n%%down\nz\nn\nc\nIn[[6]]\nOut[[6]]\n%%up\nq()\n' \
    | ROPE_HISTFILE="$tmp/hist" ./rope >"$tmp/out11" 2>"$tmp/err11"
grep -q '^→ 3    browser()$' "$tmp/out11" || { echo "FAIL: no listing at browser()"; fail=1; }
grep -q '^  1  f(1)  line 3$' "$tmp/out11" || { echo "FAIL: %where outer frame"; fail=1; }
grep -q '^→ 2  g(a)  line 3$' "$tmp/out11" || { echo "FAIL: %where innermost frame"; fail=1; }
grep -q '^→ 3    g(a)$' "$tmp/out11" || { echo "FAIL: %up listing"; fail=1; }
grep -q '^\[1\] 200$' "$tmp/out11" || { echo "FAIL: evaluation in the selected frame"; fail=1; }
grep -q '^x  *dbl \[1\]' "$tmp/out11" || { echo "FAIL: %who in the selected frame"; fail=1; }
grep -q 'already at the outermost frame' "$tmp/err11" || { echo "FAIL: %up past the outermost frame"; fail=1; }
grep -q '^\[1\] 4$' "$tmp/out11" || { echo "FAIL: %down should return to the innermost frame"; fail=1; }
grep -q '^→ 4    z + 1$' "$tmp/out11" || { echo "FAIL: no listing after n"; fail=1; }
grep -q '^a \* 100$' "$tmp/out11" || { echo "FAIL: In should show the line as typed"; fail=1; }
[ "$(grep -c '^\[1\] 200$' "$tmp/out11")" -eq 2 ] || { echo "FAIL: Out of a line run in the selected frame"; fail=1; }
grep -q 'not in the debugger' "$tmp/err11" || { echo "FAIL: %up outside the debugger"; fail=1; }

# Post-mortem: %where and %debug after an uncaught error.
printf '%%debug\nh <- function(q) g2(q)\ng2 <- function(x) {\n  y <- x\n  log(y)\n}\nh("a")\n%%where\n%%debug\ny\n%%up\nq\nc\ntry(stop("caught"))\n%%where\nstop("top")\n%%debug\nk <- function(n) k(n + 1)\nk(1)\n%%debug\nq()\n' \
    | ROPE_HISTFILE="$tmp/hist" ./rope >"$tmp/out12" 2>"$tmp/err12"
grep -q 'no error to debug' "$tmp/err12" || { echo "FAIL: %debug before any error"; fail=1; }
grep -q '^last error: non-numeric argument to mathematical function$' "$tmp/out12" || { echo "FAIL: %where after an error"; fail=1; }
grep -q '^  2  g2(q)  line 3$' "$tmp/out12" || { echo "FAIL: stack of the last error"; fail=1; }
grep -q '^post-mortem of log(y): non-numeric' "$tmp/out12" || { echo "FAIL: %debug header"; fail=1; }
grep -q '^→ 3    log(y)$' "$tmp/out12" || { echo "FAIL: %debug listing"; fail=1; }
grep -q 'Called from' "$tmp/out12" && { echo "FAIL: %debug's own browser() call shown"; fail=1; }
[ "$(grep -c '^\[1\] "a"$' "$tmp/out12")" -eq 2 ] || { echo "FAIL: values in post-mortem frames"; fail=1; }
[ "$(grep -c '^last error: non-numeric' "$tmp/out12")" -eq 2 ] || { echo "FAIL: a caught error replaced the last one"; fail=1; }
grep -q 'last error happened at top level' "$tmp/err12" || { echo "FAIL: %debug of a top-level error"; fail=1; }
grep -q 'too deep to keep' "$tmp/err12" || { echo "FAIL: %debug after infinite recursion"; fail=1; }

if [ "$fail" -eq 0 ]; then echo "smoke: ok"; else for f in "$tmp"/out* "$tmp"/err*; do echo "--- $f ---"; cat "$f"; done; fi
exit "$fail"
