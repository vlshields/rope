#!/usr/bin/env python3
"""Interactive checks through a pseudo-terminal: Ctrl-C at the prompt, Ctrl-C
during evaluation (R's and %time's), Ctrl-D, q(), history persistence, the
numbered prompt, multi-line editing, completion, data frame layout, the
pager and the debugger."""
import fcntl, os, pty, re, select, shutil, signal, struct, sys, tempfile, termios, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROPE = os.path.join(ROOT, "rope")

def spawn(histfile, env={}):
    pid, fd = pty.fork()
    if pid == 0:
        os.environ["ROPE_HISTFILE"] = histfile
        os.environ["TERM"] = "xterm"
        for k in ("PAGER", "ROPE_PAGER", "LESS", "NO_COLOR"):
            os.environ.pop(k, None)
        os.environ.update(env)
        os.execv(ROPE, [ROPE])
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
    return pid, fd

# The editor redraws the line with escape sequences; compare plain text only.
ESC = re.compile(rb"\x1b\[[0-9;?]*[ -/]*[@-~]|\x1b[()][A-Z0-9]|\x1b[=>]|\r")
PARTIAL_ESC = re.compile(rb"\x1b(\[[0-9;?]*[ -/]*)?$")

pending = b""

def read_until(fd, needle, timeout=10.0):
    """Read until needle appears; keep anything after it for the next call."""
    global pending
    buf = pending
    pending = b""
    held = b""
    end = time.time() + timeout
    while True:
        i = buf.find(needle)
        if i >= 0:
            i += len(needle)
            pending = buf[i:]
            return buf[:i]
        if time.time() >= end:
            break
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try:
                chunk = held + os.read(fd, 4096)
            except OSError:
                break
            if not chunk:
                break
            m = PARTIAL_ESC.search(chunk)
            held = chunk[m.start():] if m else b""
            if m: chunk = chunk[:m.start()]
            buf += ESC.sub(b"", chunk)
    raise AssertionError(f"timed out waiting for {needle!r}; got {buf!r}")

def wait_exit(pid, timeout=10.0):
    end = time.time() + timeout
    while time.time() < end:
        wpid, status = os.waitpid(pid, os.WNOHANG)
        if wpid:
            return status
        time.sleep(0.05)
    os.kill(pid, signal.SIGKILL)
    raise AssertionError("process did not exit")

fails = []
def check(name, cond):
    print(("ok   " if cond else "FAIL ") + name)
    if not cond: fails.append(name)

ENTER = b"\r"     # the editor runs the tty raw: Enter is CR, LF inserts a newline

with tempfile.TemporaryDirectory() as tmp:
    hist = os.path.join(tmp, "hist")

    pid, fd = spawn(hist)
    read_until(fd, b"[1]> ")
    check("prompt is numbered from 1", True)

    # 1. Ctrl-C at the prompt clears the line, process stays alive
    os.write(fd, b"half a li")
    read_until(fd, b"half a li")
    os.write(fd, b"\x03")
    out = read_until(fd, b"^C")
    read_until(fd, b"[1]> ")
    check("ctrl-c at prompt reprompts with same number", True)
    os.write(fd, b"1 + 1" + ENTER)
    out = read_until(fd, b"[1] 2")
    check("still alive after ctrl-c", True)

    # 2. Ctrl-C during Sys.sleep
    read_until(fd, b"[2]> ")
    os.write(fd, b"Sys.sleep(30)" + ENTER)
    read_until(fd, b"Sys.sleep(30)\n")
    time.sleep(0.5)
    t0 = time.time()
    os.write(fd, b"\x03")
    out = read_until(fd, b"[3]> ")
    check("ctrl-c interrupts Sys.sleep", time.time() - t0 < 3)

    # 3. Ctrl-C during a busy loop
    os.write(fd, b"while (TRUE) {}" + ENTER)
    read_until(fd, b"while (TRUE) {}\n")
    time.sleep(0.5)
    t0 = time.time()
    os.write(fd, b"\x03")
    out = read_until(fd, b"[4]> ")
    check("ctrl-c interrupts busy loop", time.time() - t0 < 3)
    os.write(fd, b"2 + 2" + ENTER)
    read_until(fd, b"[1] 4")
    check("still alive after eval interrupt", True)

    # 4. Ctrl-C during a %time evaluation unwinds to our context, not R's
    read_until(fd, b"[5]> ")
    os.write(fd, b"%time Sys.sleep(30)" + ENTER)
    read_until(fd, b"Sys.sleep(30)\n")
    time.sleep(0.5)
    t0 = time.time()
    os.write(fd, b"\x03")
    out = read_until(fd, b"[6]> ")
    check("ctrl-c interrupts %time", time.time() - t0 < 3 and b"elapsed" not in out)
    os.write(fd, b"%time 3 + 3" + ENTER)
    out = read_until(fd, b"elapsed")
    check("%time works after interrupt", b"[1] 6" in out)

    # 5. An unfinished expression continues on a new line instead of being sent
    read_until(fd, b"[7]> ")
    os.write(fd, b"f <- function(x) {" + ENTER)
    out = read_until(fd, b"+ ")
    os.write(fd, b"x * 10" + ENTER + b"}" + ENTER)
    out = read_until(fd, b"[8]> ")
    check("open brace continues the line", b"\n" in out and b"[8]> " in out)
    os.write(fd, b"f(4); f" + ENTER)
    out = read_until(fd, b"[9]> ")
    check("multi-line expression is one input", b"[1] 40" in out and b"x * 10" in out)
    os.write(fd, b"\"open" + ENTER + b"quote\"" + ENTER)
    out = read_until(fd, b"[10]> ")
    check("open quote continues the line", b'"open\\nquote"' in out)

    # 6. Tab completion comes from R
    os.write(fd, b"mtcars$mp\t")
    out = read_until(fd, b"mtcars$mpg")
    os.write(fd, b"[1]" + ENTER)
    out = read_until(fd, b"[11]> ")
    check("tab completes mtcars$mp to mpg", b"[1] 21" in out)

    # 7. An answer to readline() is not a prompt: no number, no history
    os.write(fd, b"nm <- readline('name? ')" + ENTER)
    read_until(fd, b")\nname? ")
    os.write(fd, b"%time" + ENTER)
    read_until(fd, b"[12]> ")
    os.write(fd, b"nm" + ENTER)
    out = read_until(fd, b"[13]> ")
    check("readline() answer reaches R untouched", b'"%time"' in out)

    # 8. q() exits cleanly and writes history
    os.write(fd, b"q()" + ENTER)
    status = wait_exit(pid)
    check("q() exits 0", os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0)
    saved = open(hist).read()
    check("history written on q()", "2 + 2" in saved and "q()" in saved)
    check("readline() answer not in history", "%time\n" not in saved)
    os.close(fd)

    # 9. history survives a restart, Ctrl-D exits cleanly
    pending = b""
    pid, fd = spawn(hist)
    read_until(fd, b"[1]> ")
    os.write(fd, b"\x1b[A")            # up arrow
    out = read_until(fd, b"q()")
    check("up arrow recalls previous line", True)
    os.write(fd, b"\x1b[A")
    read_until(fd, b"nm")
    os.write(fd, b"\x1b[A")
    out = read_until(fd, b"readline('name? ')")
    check("history skips the readline() answer", True)
    os.write(fd, b"\x15")              # ctrl-u clears the line
    time.sleep(0.2)
    os.write(fd, b"\x04")              # ctrl-d
    status = wait_exit(pid)
    check("ctrl-d exits 0", os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0)
    os.close(fd)

    # 10. Data frames are laid out for the 80x24 terminal
    pending = b""
    pid, fd = spawn(hist)
    read_until(fd, b"[1]> ")
    os.write(fd, b"mtcars" + ENTER)
    out = read_until(fd, b"[2]> ")
    check("frame has a title and a type row",
          "data.frame [32 \u00d7 11]".encode() in out and b"<dbl>" in out)
    check("frame shows head and tail rows",
          b"Mazda RX4 " in out and b"Volvo 142E" in out and b"Cadillac" not in out
          and b"17 more rows" in out)
    check("columns that do not fit are listed", b"1 more column: carb <dbl>" in out)
    check("no line is wider than the terminal",
          all(len(l.decode()) <= 80 for l in out.split(b"\n")))
    os.write(fd, b"print(iris, n = 2)" + ENTER)
    out = read_until(fd, b"[3]> ")
    check("print(x, n = ) limits rows", b"setosa" in out and b"148 more rows" in out)
    os.write(fd, b"options(rope.frames = FALSE); head(iris, 1)" + ENTER)
    out = read_until(fd, b"[4]> ")
    check("options(rope.frames = FALSE) gives base printing",
          b"<dbl>" not in out and b"1          5.1" in out)
    os.write(fd, b"q()" + ENTER)
    wait_exit(pid)
    os.close(fd)

    # 11. %page runs the pager; a Ctrl-C typed in the pager is not R's
    if shutil.which("less"):
        pending = b""
        pid, fd = spawn(hist)
        read_until(fd, b"[1]> ")
        os.write(fd, b"%page iris" + ENTER)
        read_until(fd, b" 20 ")
        os.write(fd, b"G")                 # less: jump to the end
        out = read_until(fd, b"virginica")
        check("%page shows every row in less", b"\n150 " in out + read_until(fd, b"(END)"))
        os.write(fd, b"\x03")
        read_until(fd, b"[2]> ")
        os.write(fd, b"Sys.sleep(0.3); 42" + ENTER)
        out = read_until(fd, b"[3]> ")
        check("ctrl-c in the pager does not interrupt R later",
              b"[1] 42" in out and b"nterrupt" not in out)
        os.write(fd, b"q()" + ENTER)
        wait_exit(pid)
        os.close(fd)

    # 12. The debugger at a real terminal: the stop is listed in colour, %up
    # selects a frame for the lines that follow, Ctrl-C cancels a line.
    pending = b""
    pid, fd = spawn(hist)
    read_until(fd, b"[1]> ")
    os.write(fd, b"g <- function(y) { z <- y * 2; browser(); z }" + ENTER)
    read_until(fd, b"[2]> ")
    os.write(fd, b"f <- function(x) { a <- x + 1; g(a) }" + ENTER)
    read_until(fd, b"[3]> ")
    os.write(fd, b"f(1)" + ENTER)
    raw = b""
    end = time.time() + 10
    while b"Browse[1]> " not in ESC.sub(b"", raw) and time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r: raw += os.read(fd, 4096)
    check("browser stop is listed, the current line in bold",
          "\x1b[1m→ 1  g <- function(y)".encode() in raw)
    os.write(fd, b"%up" + ENTER)
    out = read_until(fd, b"Browse[1]> ")
    os.write(fd, b"a * 10" + ENTER)
    out = read_until(fd, b"[1] 20")
    check("%up then a line evaluates in the outer frame", True)
    os.write(fd, b"half a li\x03")
    read_until(fd, b"^C")
    read_until(fd, b"Browse[1]> ")
    os.write(fd, b"x" + ENTER)
    read_until(fd, b"x\n")
    out = read_until(fd, b"Browse[1]> ")
    check("ctrl-c at the browser keeps the selected frame", b"[1] 1" in out)
    os.write(fd, b"c" + ENTER)
    read_until(fd, b"> ")
    os.write(fd, b"q()" + ENTER)
    wait_exit(pid)
    os.close(fd)

print("pty:", "ok" if not fails else f"{len(fails)} failed")
sys.exit(1 if fails else 0)
