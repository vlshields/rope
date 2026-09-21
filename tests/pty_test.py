#!/usr/bin/env python3
"""Interactive checks through a pseudo-terminal: Ctrl-C at the prompt, Ctrl-C
during evaluation, Ctrl-D, q(), and history persistence."""
import os, pty, select, signal, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROPE = os.path.join(ROOT, "rope")

def spawn(histfile):
    pid, fd = pty.fork()
    if pid == 0:
        os.environ["ROPE_HISTFILE"] = histfile
        os.environ["TERM"] = "dumb"
        os.execv(ROPE, [ROPE])
    return pid, fd

pending = b""

def read_until(fd, needle, timeout=10.0):
    """Read until needle appears; keep anything after it for the next call."""
    global pending
    buf = pending
    pending = b""
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
                chunk = os.read(fd, 4096)
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
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

with tempfile.TemporaryDirectory() as tmp:
    hist = os.path.join(tmp, "hist")

    pid, fd = spawn(hist)
    read_until(fd, b"> ")

    # 1. Ctrl-C at the prompt clears the line, process stays alive
    os.write(fd, b"half a li")
    read_until(fd, b"half a li")
    os.write(fd, b"\x03")
    out = read_until(fd, b"> ")
    check("ctrl-c at prompt reprompts", b"^C" in out)
    os.write(fd, b"1 + 1\n")
    out = read_until(fd, b"[1] 2")
    check("still alive after ctrl-c", True)

    # 2. Ctrl-C during Sys.sleep
    read_until(fd, b"> ")
    os.write(fd, b"Sys.sleep(30)\n")
    read_until(fd, b"Sys.sleep(30)")
    time.sleep(0.5)
    t0 = time.time()
    os.write(fd, b"\x03")
    out = read_until(fd, b"> ")
    check("ctrl-c interrupts Sys.sleep", time.time() - t0 < 3)

    # 3. Ctrl-C during a busy loop
    os.write(fd, b"while (TRUE) {}\n")
    read_until(fd, b"while (TRUE) {}")
    time.sleep(0.5)
    t0 = time.time()
    os.write(fd, b"\x03")
    out = read_until(fd, b"> ")
    check("ctrl-c interrupts busy loop", time.time() - t0 < 3)
    os.write(fd, b"2 + 2\n")
    read_until(fd, b"[1] 4")
    check("still alive after eval interrupt", True)

    # 4. q() exits cleanly and writes history
    read_until(fd, b"> ")
    os.write(fd, b"q()\n")
    status = wait_exit(pid)
    check("q() exits 0", os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0)
    check("history written on q()", os.path.exists(hist) and "2 + 2" in open(hist).read())
    os.close(fd)

    # 5. history survives a restart, Ctrl-D exits cleanly
    pending = b""
    pid, fd = spawn(hist)
    read_until(fd, b"> ")
    os.write(fd, b"\x1b[A")            # up arrow
    out = read_until(fd, b"q()")
    check("up arrow recalls previous line", True)
    os.write(fd, b"\x15")              # ctrl-u clears the line
    time.sleep(0.2)
    os.write(fd, b"\x04")              # ctrl-d
    status = wait_exit(pid)
    check("ctrl-d exits 0", os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0)
    os.close(fd)

print("pty:", "ok" if not fails else f"{len(fails)} failed")
sys.exit(1 if fails else 0)
