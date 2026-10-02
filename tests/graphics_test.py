#!/usr/bin/env python3
"""The graphics device through a pipe: with ROPE_GRAPHICS forcing a format,
every page comes out as a kitty or sixel image on stdout. Both are decoded
here and the pixels checked, so the rasteriser is tested as well as the
encoders."""
import base64, os, re, subprocess, sys, tempfile, zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROPE = os.path.join(ROOT, "rope")

fails = []
def check(name, cond):
    print(("ok   " if cond else "FAIL ") + name)
    if not cond: fails.append(name)

def run(script, fmt, cwd, extra_env={}):
    env = dict(os.environ, ROPE_HISTFILE=os.path.join(cwd, "hist"))
    env.pop("TERM", None)
    env.pop("KITTY_WINDOW_ID", None)
    if fmt is None: env.pop("ROPE_GRAPHICS", None)
    else: env["ROPE_GRAPHICS"] = fmt
    env.update(extra_env)
    r = subprocess.run([ROPE], input=(script + "\nq()\n").encode(), capture_output=True,
                       env=env, cwd=cwd, timeout=60)
    return r.stdout, r.stderr.decode(errors="replace"), r.returncode

class Image:
    def __init__(self, w, h, px):
        self.w, self.h, self.px = w, h, px   # px: list of (r, g, b, a)
    def count(self, pred, x0=0, y0=0, x1=None, y1=None):
        x1 = self.w if x1 is None else x1
        y1 = self.h if y1 is None else y1
        return sum(1 for y in range(y0, y1) for x in range(x0, x1) if pred(self.px[y * self.w + x]))

def dark(p):  return p[3] > 200 and max(p[:3]) < 80
def white(p): return p[3] > 200 and min(p[:3]) > 250

KITTY_CHUNK = re.compile(rb"\x1b_G([^;]*);([^\x1b]*)\x1b\\")

def kitty_images(out):
    """Decode every kitty image in `out`; chunks of one image are consecutive."""
    images, pos = [], 0
    while True:
        m = re.compile(rb"\x1b_Ga=T,").search(out, pos)
        if not m: break
        payload, hdr, p = b"", None, m.start()
        while True:
            c = KITTY_CHUNK.match(out, p)
            if not c: raise AssertionError("malformed kitty chunk")
            keys = dict(kv.split(b"=") for kv in c.group(1).split(b","))
            if hdr is None: hdr = keys
            payload += c.group(2)
            p = c.end()
            if keys.get(b"m", b"0") == b"0": break
        pos = p
        assert hdr[b"f"] == b"32" and hdr[b"o"] == b"z" and hdr[b"q"] == b"2", hdr
        raw = zlib.decompress(base64.b64decode(payload))
        w, h = int(hdr[b"s"]), int(hdr[b"v"])
        assert len(raw) == w * h * 4, (len(raw), w, h)
        images.append(Image(w, h, [tuple(raw[i:i + 4]) for i in range(0, len(raw), 4)]))
    return images

def sixel_images(out):
    """A small sixel decoder: enough for what rope emits."""
    images = []
    for m in re.finditer(rb"\x1bP([0-9;]*)q(.*?)\x1b\\", out, re.S):
        body = m.group(2)
        pal, px, w, h = {}, {}, 0, 0
        ra = re.match(rb'"(\d+);(\d+);(\d+);(\d+)', body)
        if ra:
            w, h = int(ra.group(3)), int(ra.group(4))
            body = body[ra.end():]
        x = y = colour = 0
        i = 0
        while i < len(body):
            c = body[i:i + 1]
            if c == b"#":
                j = i + 1
                while j < len(body) and body[j:j + 1] in b"0123456789;": j += 1
                parts = body[i + 1:j].split(b";")
                if len(parts) >= 5:
                    pal[int(parts[0])] = tuple(int(v) * 255 // 100 for v in parts[2:5])
                else:
                    colour = int(parts[0])
                i = j
            elif c == b"!":
                j = i + 1
                while body[j:j + 1].isdigit(): j += 1
                n = int(body[i + 1:j])
                bits = body[j] - 63
                for k in range(n):
                    for r in range(6):
                        if bits & (1 << r): px[(x + k, y + r)] = colour
                x += n
                i = j + 1
            elif c == b"$":
                x = 0; i += 1
            elif c == b"-":
                x = 0; y += 6; i += 1
            elif 63 <= body[i] <= 126:
                bits = body[i] - 63
                for r in range(6):
                    if bits & (1 << r): px[(x, y + r)] = colour
                x += 1; i += 1
            else:
                i += 1
        pixels = [pal.get(px.get((x, y), -1), (255, 255, 255)) + (255,) for y in range(h) for x in range(w)]
        images.append(Image(w, h, pixels))
    return images

def strip(out):
    return re.sub(rb"\x1b_G[^\x1b]*\x1b\\|\x1bP.*?\x1b\\", b"", out, flags=re.S).decode(errors="replace")

with tempfile.TemporaryDirectory() as tmp:
    # 1. A plot through kitty: one image per prompt that follows drawing.
    out, err, status = run('plot(1:10, main = "Hello")\ndev.cur()\ndev.size("px")\n'
                           'strwidth("Hello", units = "figure") > 0\n'
                           'plot(1); plot(2)\nplot(3); dev.off()\ndev.cur()\n', "kitty", tmp)
    text = strip(out)
    check("kitty: exit status 0, nothing on stderr", status == 0 and err.strip() == "")
    imgs = kitty_images(out)
    check("kitty: four images (plot; two in one line; plot then dev.off)", len(imgs) == 4)
    check("kitty: default page size for an 80x24 terminal of 8x16 cells",
          imgs and (imgs[0].w, imgs[0].h) == (512, 230))
    check("kitty: dev.cur() names the device", "\nrope \n" in text)
    check("kitty: dev.size() agrees with the image", "[1] 512 230" in text)
    check("kitty: strwidth measures text", "[1] TRUE" in text)
    check("kitty: dev.off() leaves no device", "null device \n" in text)
    if imgs:
        im = imgs[0]
        check("kitty: page is mostly white", im.count(white) > 0.7 * im.w * im.h)
        check("kitty: axes and points are black", im.count(dark) > 300)
        check("kitty: the title is drawn near the top", im.count(dark, 0, 0, im.w, 60) > 40)
        check("kitty: nothing below the x-axis label", im.count(dark, 0, im.h - 4, im.w, im.h) == 0)
    check("kitty: the image precedes the prompt output", out.index(b"\x1b_G") < out.index(b"rope \n"))
    check("kitty: the image ends with a newline", b"\x1b\\\n" in out)

    # 2. Size, colour and dpi arguments; the page is filled with bg.
    out, err, status = run('rope.graphics(width = 300, height = 200, bg = "black", dpi = 150)\n'
                           'par(mar = rep(0, 4)); plot.new(); text(0.5, 0.5, "big", col = "white", cex = 3)\n'
                           'x <- dev.capture(); dim(x); x[1, 1]\n'
                           'options(rope.plot.width = 320, rope.plot.height = 240)\ndev.new(); plot(1)\n', "kitty", tmp)
    text = strip(out)
    imgs = kitty_images(out)
    check("args: exit 0", status == 0 and "Error" not in err)
    check("args: two devices, two images", len(imgs) == 2)
    if len(imgs) == 2:
        check("args: width and height are pixels", (imgs[0].w, imgs[0].h) == (300, 200))
        check("args: bg fills the page", imgs[0].count(dark) > 0.5 * 300 * 200)
        check("args: white text on it", imgs[0].count(white) > 100)
        check("args: options give dev.new() its size", (imgs[1].w, imgs[1].h) == (320, 240))
    check("args: dev.capture() returns the page", "[1] 200 300" in text and '"black"' in text)

    # 3. Sixel: decodable, right size, mostly white with black on it.
    out, err, status = run('plot(1:10, main = "Hello", col = "red", pch = 19)\n', "sixel", tmp)
    imgs = sixel_images(out)
    check("sixel: exit 0", status == 0 and err.strip() == "")
    check("sixel: one image", len(imgs) == 1)
    if imgs:
        im = imgs[0]
        check("sixel: size", (im.w, im.h) == (512, 230))
        check("sixel: mostly white", im.count(white) > 0.7 * im.w * im.h)
        check("sixel: black axes", im.count(dark) > 200)
        check("sixel: red points", im.count(lambda p: p[0] > 200 and p[1] < 60 and p[2] < 60) > 50)
    check("sixel: raster attributes", b'\x1bP0;1;0q"1;1;512;230#' in out)

    # 4. Without a terminal that can show images, R's default device stands.
    out, err, status = run('identical(getOption("device"), rope.graphics)\n', None, tmp)
    check("no terminal: device option untouched", "[1] FALSE" in strip(out) and b"\x1b" not in out)
    out, err, status = run('rope.graphics()\n', "none", tmp)
    check("none: rope.graphics() explains", "cannot show images" in err)
    out, err, status = run('1\n', "bogus", tmp)
    check("bogus: ROPE_GRAPHICS value reported", "expected kitty, sixel, none or auto" in err)

    # 5. Text in every face, symbols, and a rotated raster do not fall over.
    out, err, status = run('plot.new(); for (f in 1:5) text(0.5, f / 6, "Agα", font = f)\n'
                           'text(0.2, 0.5, expression(sum(x[i]^2, i == 1, n)))\n'
                           'rasterImage(as.raster(matrix(1:4 / 4, 2)), 0.6, 0.6, 0.9, 0.9, angle = 20, interpolate = FALSE)\n'
                           'par(lend = "square", ljoin = "mitre"); lines(c(0.1, 0.5, 0.9), c(0.1, 0.9, 0.1), lwd = 8, lty = "dotdash")\n'
                           'polypath(c(0.1, 0.4, 0.4, 0.1, NA, 0.2, 0.3, 0.3, 0.2), c(0.1, 0.1, 0.4, 0.4, NA, 0.2, 0.2, 0.3, 0.3), rule = "evenodd", col = "grey")\n'
                           'rope.graphics(bg = "transparent"); plot(1)\n', "kitty", tmp)
    imgs = kitty_images(out)
    check("faces: exit 0, no errors", status == 0 and "Error" not in err and "warning" not in err.lower())
    check("faces: one image per prompt after drawing", len(imgs) == 6)
    if len(imgs) == 6:
        check("faces: text and shapes drawn", imgs[4].count(dark) > 500)
        check("faces: the even-odd hole stays white", imgs[4].count(white, int(0.22 * 512), int(0.72 * 230), int(0.28 * 512), int(0.78 * 230)) > 0)
        check("transparent bg: alpha 0 outside the drawing", imgs[5].count(lambda p: p[3] == 0) > 0.5 * 512 * 230)

    # 6. grid and lattice: the R 4.1+ device hooks (paths, clipping paths,
    # masks, gradients, groups, glyphs) that grid.newpage() and packages use.
    out, err, status = run('rope.graphics(width = 400, height = 300)\n'
                           'library(lattice); xyplot(Sepal.Length ~ Petal.Length | Species, iris)\n'
                           'library(grid); grid.newpage(); pushViewport(viewport(clip = circleGrob(r = 0.4)))\n'
                           'grid.rect(gp = gpar(fill = linearGradient(c("red", "blue"))))\n'
                           'popViewport(); grid.rect(x = 0.1, y = 0.1, width = 0.1, height = 0.1, gp = gpar(fill = "black"))\n'
                           'grid.newpage(); pushViewport(viewport(mask = as.mask(rectGrob(width = 0.5, height = 0.5, gp = gpar(fill = "black")))))\n'
                           'grid.rect(gp = gpar(fill = "black")); popViewport()\n'
                           'grid.fill(circleGrob(r = 0.1), gp = gpar(fill = "red"))\n'
                           'grid.group(rectGrob(x = 0.9, y = 0.9, width = 0.1, height = 0.1, gp = gpar(fill = "blue")))\n'
                           'str(dev.capabilities()[c("patterns", "clippingPaths", "masks", "paths")])\n', "kitty", tmp)
    text = strip(out)
    imgs = kitty_images(out)
    check("grid: exit 0, no errors", status == 0 and "Error" not in err)
    check("grid: lattice and grid pages", len(imgs) == 7)
    if len(imgs) == 7:
        lat, clip, popped, mask, after = imgs[0], imgs[1], imgs[2], imgs[4], imgs[6]
        red = lambda p: p[0] > 150 and p[2] < 120 and p[3] > 200
        blue = lambda p: p[2] > 150 and p[0] < 120 and p[3] > 200
        check("grid: lattice draws panels and text", lat.count(dark) > 500 and lat.count(white) > 0.6 * 400 * 300)
        check("grid: gradient inside the clip circle", red(clip.px[150 * 400 + 100]) and blue(clip.px[150 * 400 + 300]))
        check("grid: nothing outside the clip circle", clip.count(white, 0, 0, 40, 40) == 1600)
        check("grid: the clip path goes away with the viewport", popped.count(dark, 22, 258, 58, 282) > 800)
        check("grid: mask keeps the middle only", mask.count(dark, 120, 90, 280, 210) > 15000 and mask.count(white, 0, 0, 80, 60) == 4800)
        check("grid: grid.fill path and a group", after.count(red) > 500 and after.count(blue) > 500)
    check("grid: dev.capabilities() reports them", "LinearGradient" in text and "clippingPaths: logi TRUE" in text and 'masks        : chr [1:2]' in text and "paths        : logi TRUE" in text)

print("graphics:", "ok" if not fails else f"{len(fails)} failed")
sys.exit(1 if fails else 0)
