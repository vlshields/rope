/*
 * rope graphics: an R graphics device that draws into memory and shows each
 * page in the terminal as a kitty or sixel image.
 *
 * The device rasterises everything itself, so nothing beyond libR is linked.
 * Every primitive R asks for becomes a polygon: strokes are outlined with
 * their caps, joins and dashes, circles are approximated, glyph outlines come
 * from a TrueType or OpenType font through the vendored stb_truetype.h and
 * are filled like any other shape. One scanline filler with sub-pixel
 * coverage does the anti-aliasing for all of them.
 *
 * A page is shown when the next prompt is about to appear, when a new page
 * starts over a drawn one, and when the device closes. Each showing is a new
 * image in the terminal's scrollback, so `plot(x)` on one line and `lines(y)`
 * on the next give two pictures: the terminal is a log, and the page is
 * whatever it looked like at the end of each line.
 *
 * Which image protocol to use is settled once at startup by asking the
 * terminal (rope_gd_init), or by ROPE_GRAPHICS=kitty|sixel|none.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_truetype.h"
#pragma GCC diagnostic pop

#include <Rinternals.h>
#include <R_ext/GraphicsEngine.h>

extern Rboolean R_interrupts_suspended;

#define GD_PI 3.14159265358979323846

/* ---- the terminal --------------------------------------------------------- */

enum gd_format { GD_NONE = 0, GD_KITTY, GD_SIXEL };

static enum gd_format gd_format = GD_NONE;
static int gd_probed = 0;
static double gd_cell_w = 8, gd_cell_h = 16;   /* pixels per character cell */

static int gd_tty_usable(void)
{
    const char *term = getenv("TERM");
    return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO) &&
           term && *term && strcmp(term, "dumb") != 0;
}

/* The cell size from the window size, when the terminal reports pixels. */
static void gd_cell_from_winsize(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0) return;
    if (ws.ws_col > 0 && ws.ws_xpixel > 0) gd_cell_w = (double)ws.ws_xpixel / ws.ws_col;
    if (ws.ws_row > 0 && ws.ws_ypixel > 0) gd_cell_h = (double)ws.ws_ypixel / ws.ws_row;
}

/*
 * Ask the terminal what it can do. Three questions go out together: a kitty
 * graphics query, a request for the cell size (CSI 16 t), and the primary
 * device attributes (CSI c). Every terminal answers the last one, and answers
 * arrive in order, so once it is in, the other two have either arrived or
 * never will. Sixel support is attribute 4 in that answer.
 */
static void gd_query_terminal(void)
{
    struct termios saved, raw;
    if (tcgetattr(STDIN_FILENO, &saved) != 0) return;
    raw = saved;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return;

    static const char query[] =
        "\x1b_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\x1b\\"
        "\x1b[16t"
        "\x1b[c";
    (void)!write(STDOUT_FILENO, query, sizeof query - 1);

    char reply[1024];
    size_t n = 0;
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double left = 1.0 - ((now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) / 1e9);
        if (left <= 0 || n >= sizeof reply - 1) break;
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(STDIN_FILENO, &rd);
        struct timeval tv = { (time_t)left, (suseconds_t)((left - (time_t)left) * 1e6) };
        if (select(STDIN_FILENO + 1, &rd, NULL, NULL, &tv) <= 0) break;
        ssize_t got = read(STDIN_FILENO, reply + n, sizeof reply - 1 - n);
        if (got <= 0) break;
        n += (size_t)got;
        reply[n] = '\0';
        /* The attributes answer: ESC [ ? ... c */
        char *da = strstr(reply, "\x1b[?");
        if (da && strchr(da, 'c')) break;
    }
    tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    reply[n] = '\0';

    if (strstr(reply, "_Gi=31;OK")) gd_format = GD_KITTY;
    char *da = strstr(reply, "\x1b[?");
    if (da) {
        char *end = strchr(da, 'c');
        if (end) {
            *end = '\0';
            for (char *p = da + 3; p && *p; ) {
                if (atoi(p) == 4 && gd_format == GD_NONE) gd_format = GD_SIXEL;
                p = strchr(p, ';');
                if (p) p++;
            }
        }
    }
    char *cell = strstr(reply, "\x1b[6;");
    if (cell) {
        int h = 0, w = 0;
        if (sscanf(cell, "\x1b[6;%d;%dt", &h, &w) == 2 && h > 0 && w > 0) {
            gd_cell_w = w;
            gd_cell_h = h;
        }
    }
}

/* rope_gd_init(): decide the image format once, before R prints anything. */
void rope_gd_init(void)
{
    gd_probed = 1;
    const char *want = getenv("ROPE_GRAPHICS");
    if (want && *want && strcmp(want, "auto") != 0) {
        if (strcmp(want, "kitty") == 0)      gd_format = GD_KITTY;
        else if (strcmp(want, "sixel") == 0) gd_format = GD_SIXEL;
        else if (strcmp(want, "none") != 0)
            fprintf(stderr, "rope: ROPE_GRAPHICS=%s: expected kitty, sixel, none or auto\n", want);
        if (gd_tty_usable()) gd_cell_from_winsize();
        return;
    }
    if (!gd_tty_usable()) return;
    gd_cell_from_winsize();
    const char *term = getenv("TERM");
    if ((term && strstr(term, "kitty")) || getenv("KITTY_WINDOW_ID")) {
        gd_format = GD_KITTY;
        return;
    }
    gd_query_terminal();
}

/* ---- the canvas ----------------------------------------------------------- */

/* Premultiplied RGBA, four floats per pixel, row-major from the top left. */
struct gd_canvas {
    int w, h;
    float *px;
};

static int gd_canvas_init(struct gd_canvas *c, int w, int h)
{
    c->w = w;
    c->h = h;
    c->px = calloc((size_t)w * h * 4, sizeof *c->px);
    return c->px != NULL;
}

static void gd_canvas_clear(struct gd_canvas *c, const float rgba[4])
{
    for (size_t i = 0; i < (size_t)c->w * c->h; i++) {
        c->px[i * 4 + 0] = rgba[0] * rgba[3];
        c->px[i * 4 + 1] = rgba[1] * rgba[3];
        c->px[i * 4 + 2] = rgba[2] * rgba[3];
        c->px[i * 4 + 3] = rgba[3];
    }
}

static void gd_colour(unsigned int col, float rgba[4])
{
    rgba[0] = R_RED(col) / 255.0f;
    rgba[1] = R_GREEN(col) / 255.0f;
    rgba[2] = R_BLUE(col) / 255.0f;
    rgba[3] = R_ALPHA(col) / 255.0f;
}

/* Source-over: `cov` is the coverage of the pixel, 0 to 1. */
static inline void gd_blend(float *p, const float rgba[4], float cov)
{
    float a = rgba[3] * cov;
    if (a <= 0) return;
    float keep = 1 - a;
    p[0] = rgba[0] * a + p[0] * keep;
    p[1] = rgba[1] * a + p[1] * keep;
    p[2] = rgba[2] * a + p[2] * keep;
    p[3] = a + p[3] * keep;
}

/* ---- paths ---------------------------------------------------------------- */

struct gd_pt { float x, y; };

/*
 * A list of subpaths. sub[i] is the index of subpath i's first point; the
 * last one runs to the end. For filling every subpath is closed; for
 * stroking, `closed` says whether the last point joins the first.
 */
struct gd_path {
    struct gd_pt *pt;
    int n, cap;
    int *sub;
    unsigned char *subclosed;   /* per subpath: does the last point join the first? */
    int nsub, subcap;
    int closed;                 /* the flag new subpaths start with */
};

static void gd_path_init(struct gd_path *p)
{
    memset(p, 0, sizeof *p);
}

static void gd_path_free(struct gd_path *p)
{
    free(p->pt);
    free(p->sub);
    free(p->subclosed);
    gd_path_init(p);
}

static int gd_sub_closed(const struct gd_path *p, int i) { return p->subclosed[i]; }

static void gd_path_move(struct gd_path *p, double x, double y);

static void gd_path_line(struct gd_path *p, double x, double y)
{
    if (p->nsub == 0) { gd_path_move(p, x, y); return; }
    if (p->n == p->cap) {
        int cap = p->cap ? p->cap * 2 : 64;
        struct gd_pt *pt = realloc(p->pt, (size_t)cap * sizeof *pt);
        if (!pt) return;
        p->pt = pt;
        p->cap = cap;
    }
    p->pt[p->n].x = (float)x;
    p->pt[p->n].y = (float)y;
    p->n++;
}

static void gd_path_move(struct gd_path *p, double x, double y)
{
    if (p->nsub == p->subcap) {
        int cap = p->subcap ? p->subcap * 2 : 8;
        int *sub = realloc(p->sub, (size_t)cap * sizeof *sub);
        if (!sub) return;
        p->sub = sub;
        unsigned char *sc = realloc(p->subclosed, (size_t)cap);
        if (!sc) return;
        p->subclosed = sc;
        p->subcap = cap;
    }
    p->subclosed[p->nsub] = (unsigned char)p->closed;
    p->sub[p->nsub++] = p->n;
    gd_path_line(p, x, y);
}

static int gd_sub_start(const struct gd_path *p, int i) { return p->sub[i]; }
static int gd_sub_end(const struct gd_path *p, int i)   { return i + 1 < p->nsub ? p->sub[i + 1] : p->n; }

/*
 * Add a closed polygon with counter-clockwise orientation, whichever way it
 * was given. Pieces of a stroke are unioned by the non-zero rule, which only
 * works if they all turn the same way.
 */
static void gd_path_poly(struct gd_path *p, const struct gd_pt *v, int n)
{
    double area = 0;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        area += (double)v[i].x * v[j].y - (double)v[j].x * v[i].y;
    }
    if (area >= 0) {
        gd_path_move(p, v[0].x, v[0].y);
        for (int i = 1; i < n; i++) gd_path_line(p, v[i].x, v[i].y);
    } else {
        gd_path_move(p, v[n - 1].x, v[n - 1].y);
        for (int i = n - 2; i >= 0; i--) gd_path_line(p, v[i].x, v[i].y);
    }
}

static void gd_path_circle(struct gd_path *p, double cx, double cy, double r)
{
    int n = (int)(2 * GD_PI * r / 2.0);
    if (n < 8) n = 8;
    if (n > 256) n = 256;
    for (int i = 0; i < n; i++) {
        double a = 2 * GD_PI * i / n;
        double x = cx + r * cos(a), y = cy + r * sin(a);
        if (i == 0) gd_path_move(p, x, y); else gd_path_line(p, x, y);
    }
}

/* ---- the scanline filler -------------------------------------------------- */

struct gd_edge {
    float x0, y0, x1, y1;   /* y0 < y1 */
    float dxdy;
    int dir;
};

static int gd_edge_cmp(const void *a, const void *b)
{
    const struct gd_edge *ea = a, *eb = b;
    return ea->y0 < eb->y0 ? -1 : ea->y0 > eb->y0;
}

struct gd_cross { float x; int dir; };

static int gd_cross_cmp(const void *a, const void *b)
{
    const struct gd_cross *ca = a, *cb = b;
    return ca->x < cb->x ? -1 : ca->x > cb->x;
}

/*
 * Where drawing may land: a rectangle in pixels (x1, y1 exclusive), and
 * optionally two page-sized coverage buffers, a rasterised clipping path
 * and a mask, that scale every pixel's coverage.
 */
struct gd_clip {
    float x0, y0, x1, y1;
    const float *clipmask, *mask;
    int stride;
};

static inline float gd_clip_at(const struct gd_clip *c, int x, int y)
{
    float m = 1;
    if (c->clipmask) m *= c->clipmask[(size_t)y * c->stride + x];
    if (c->mask) m *= c->mask[(size_t)y * c->stride + x];
    return m;
}

/* What a fill is painted with: one colour, a gradient, or a tile. */
enum gd_paint_kind { GD_SOLID = 0, GD_LINEAR, GD_RADIAL, GD_TILE };

struct gd_canvas;
struct gd_paint {
    enum gd_paint_kind kind;
    float rgba[4];                 /* GD_SOLID */
    double x1, y1, x2, y2;         /* gradient geometry */
    double r1, r2;
    int nstops;
    double *stops;
    float (*colours)[4];
    int extend;                    /* R_GE_patternExtend* */
    const struct gd_canvas *tile;  /* GD_TILE: the page-sized tile drawing */
    double tx, ty, tw, th;         /* the tile rectangle on the page */
};

static void gd_paint_at(const struct gd_paint *p, double x, double y, float rgba[4]);

/*
 * Scratch space for the filler and the stroker, kept between calls: a
 * scatter plot is a hundred thousand small fills, and the mallocs showed.
 */
static void *gd_scratch(int slot, size_t bytes)
{
    static void *buf[8];
    static size_t cap[8];
    if (bytes > cap[slot]) {
        size_t want = cap[slot] ? cap[slot] : 4096;
        while (want < bytes) want *= 2;
        void *p = realloc(buf[slot], want);
        if (!p) return NULL;
        buf[slot] = p;
        cap[slot] = want;
    }
    return buf[slot];
}

#define GD_SUBSAMPLES 16

/*
 * Fill the closed subpaths of `path` with `rgba` under the non-zero (rule 1)
 * or even-odd (rule 2) rule, clipped to `clip`. Each pixel row is sampled on
 * GD_SUBSAMPLES scanlines; a span's fractional ends contribute their fraction,
 * so both directions are anti-aliased.
 */
static void gd_fill_paint(struct gd_canvas *cv, const struct gd_path *path, int rule,
                          const struct gd_paint *paint, const struct gd_clip *clip)
{
    if (path->n < 3) return;
    if (paint->kind == GD_SOLID && paint->rgba[3] <= 0) return;

    /* Edges, ignoring horizontals. */
    struct gd_edge *edges = gd_scratch(0, (size_t)(path->n + path->nsub) * sizeof *edges);
    if (!edges) return;
    int ne = 0;
    float miny = 1e30f, maxy = -1e30f, minx = 1e30f, maxx = -1e30f;
    for (int s = 0; s < path->nsub; s++) {
        int a = gd_sub_start(path, s), b = gd_sub_end(path, s);
        if (b - a < 2) continue;
        for (int i = a; i < b; i++) {
            struct gd_pt p = path->pt[i], q = path->pt[i + 1 < b ? i + 1 : a];
            if (p.y == q.y) continue;
            struct gd_edge *e = &edges[ne++];
            if (p.y < q.y) { e->x0 = p.x; e->y0 = p.y; e->x1 = q.x; e->y1 = q.y; e->dir = 1; }
            else           { e->x0 = q.x; e->y0 = q.y; e->x1 = p.x; e->y1 = p.y; e->dir = -1; }
            e->dxdy = (e->x1 - e->x0) / (e->y1 - e->y0);
            if (e->y0 < miny) miny = e->y0;
            if (e->y1 > maxy) maxy = e->y1;
            float lo = p.x < q.x ? p.x : q.x, hi = p.x < q.x ? q.x : p.x;
            if (lo < minx) minx = lo;
            if (hi > maxx) maxx = hi;
        }
    }
    if (ne == 0) return;
    if (ne <= 32) {
        for (int i = 1; i < ne; i++) {
            struct gd_edge e = edges[i];
            int j = i - 1;
            while (j >= 0 && edges[j].y0 > e.y0) { edges[j + 1] = edges[j]; j--; }
            edges[j + 1] = e;
        }
    } else qsort(edges, (size_t)ne, sizeof *edges, gd_edge_cmp);

    float cx0 = clip->x0 < 0 ? 0 : clip->x0, cy0 = clip->y0 < 0 ? 0 : clip->y0;
    float cx1 = clip->x1 > cv->w ? cv->w : clip->x1, cy1 = clip->y1 > cv->h ? cv->h : clip->y1;
    if (minx > cx0) cx0 = minx;
    if (maxx < cx1) cx1 = maxx;
    if (miny > cy0) cy0 = miny;
    if (maxy < cy1) cy1 = maxy;
    if (cx0 >= cx1 || cy0 >= cy1) return;
    int row0 = (int)floorf(cy0), row1 = (int)ceilf(cy1);
    int col0 = (int)floorf(cx0), col1 = (int)ceilf(cx1);
    if (row1 > cv->h) row1 = cv->h;
    if (col1 > cv->w) col1 = cv->w;

    float *cov = gd_scratch(1, (size_t)(col1 - col0 + 1) * sizeof *cov);
    struct gd_edge **active = gd_scratch(2, (size_t)ne * sizeof *active);
    struct gd_cross *cross = gd_scratch(3, (size_t)ne * sizeof *cross);
    if (!cov || !active || !cross) return;
    memset(cov, 0, (size_t)(col1 - col0 + 1) * sizeof *cov);

    int nactive = 0, next = 0;
    const float ws = 1.0f / GD_SUBSAMPLES;
    for (int row = row0; row < row1; row++) {
        int touched0 = col1, touched1 = col0 - 1;
        for (int s = 0; s < GD_SUBSAMPLES; s++) {
            float y = row + (s + 0.5f) * ws;
            if (y < cy0 || y >= cy1) continue;
            while (next < ne && edges[next].y0 <= y) active[nactive++] = &edges[next++];
            int k = 0, nc = 0;
            for (int i = 0; i < nactive; i++) {
                struct gd_edge *e = active[i];
                if (e->y1 <= y) continue;          /* finished: drop it */
                active[k++] = e;
                if (e->y0 > y) continue;
                cross[nc].x = e->x0 + (y - e->y0) * e->dxdy;
                cross[nc].dir = e->dir;
                nc++;
            }
            nactive = k;
            if (nc < 2) continue;
            if (nc <= 16) {   /* the usual case: a handful of crossings */
                for (int i = 1; i < nc; i++) {
                    struct gd_cross c = cross[i];
                    int j = i - 1;
                    while (j >= 0 && cross[j].x > c.x) { cross[j + 1] = cross[j]; j--; }
                    cross[j + 1] = c;
                }
            } else qsort(cross, (size_t)nc, sizeof *cross, gd_cross_cmp);
            int wind = 0;
            for (int i = 0; i + 1 < nc; i++) {
                wind += rule == 2 ? 1 : cross[i].dir;
                int inside = rule == 2 ? (wind & 1) : wind != 0;
                if (!inside) continue;
                float xa = cross[i].x, xb = cross[i + 1].x;
                if (xa < cx0) xa = cx0;
                if (xb > cx1) xb = cx1;
                if (xb <= xa) continue;
                int ia = (int)floorf(xa), ib = (int)floorf(xb);
                if (ib >= col1) ib = col1 - 1;
                if (ia < col0) ia = col0;
                if (ia == ib) {
                    cov[ia - col0] += (xb - xa) * ws;
                } else {
                    cov[ia - col0] += (ia + 1 - xa) * ws;
                    for (int x = ia + 1; x < ib; x++) cov[x - col0] += ws;
                    cov[ib - col0] += (xb - ib) * ws;
                }
                if (ia < touched0) touched0 = ia;
                if (ib > touched1) touched1 = ib;
            }
        }
        if (touched1 < touched0) continue;
        float *line = cv->px + ((size_t)row * cv->w + touched0) * 4;
        for (int x = touched0; x <= touched1; x++, line += 4) {
            float c = cov[x - col0];
            cov[x - col0] = 0;
            if (c > 1) c = 1;
            if (clip->clipmask || clip->mask) c *= gd_clip_at(clip, x, row);
            if (c <= 0.0005f) continue;
            if (paint->kind == GD_SOLID) gd_blend(line, paint->rgba, c);
            else {
                float rgba[4];
                gd_paint_at(paint, x + 0.5, row + 0.5, rgba);
                gd_blend(line, rgba, c);
            }
        }
    }
}

static void gd_fill(struct gd_canvas *cv, const struct gd_path *path, int rule,
                    const float rgba[4], const struct gd_clip *clip)
{
    struct gd_paint paint = { GD_SOLID, { rgba[0], rgba[1], rgba[2], rgba[3] },
                              0, 0, 0, 0, 0, 0, 0, NULL, NULL, 0, NULL, 0, 0, 0, 0 };
    gd_fill_paint(cv, path, rule, &paint, clip);
}

/* The colour of a gradient at parameter t, after its extend rule. */
static void gd_gradient_colour(const struct gd_paint *p, double t, float rgba[4])
{
    rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0;
    switch (p->extend) {
    case R_GE_patternExtendNone:
        if (t < 0 || t > 1) return;
        break;
    case R_GE_patternExtendRepeat:
        t -= floor(t);
        break;
    case R_GE_patternExtendReflect:
        t = fabs(t - 2 * floor(t / 2) - 1);
        t = 1 - t;
        break;
    default:
        if (t < 0) t = 0;
        if (t > 1) t = 1;
    }
    if (p->nstops == 0) return;
    if (t <= p->stops[0]) { memcpy(rgba, p->colours[0], sizeof(float[4])); return; }
    for (int i = 1; i < p->nstops; i++) {
        if (t <= p->stops[i]) {
            double span = p->stops[i] - p->stops[i - 1];
            double f = span > 0 ? (t - p->stops[i - 1]) / span : 1;
            for (int k = 0; k < 4; k++)
                rgba[k] = (float)(p->colours[i - 1][k] * (1 - f) + p->colours[i][k] * f);
            return;
        }
    }
    memcpy(rgba, p->colours[p->nstops - 1], sizeof(float[4]));
}

static void gd_paint_at(const struct gd_paint *p, double x, double y, float rgba[4])
{
    switch (p->kind) {
    case GD_SOLID:
        memcpy(rgba, p->rgba, sizeof(float[4]));
        return;
    case GD_LINEAR: {
        double dx = p->x2 - p->x1, dy = p->y2 - p->y1, len2 = dx * dx + dy * dy;
        double t = len2 > 0 ? ((x - p->x1) * dx + (y - p->y1) * dy) / len2 : 0;
        gd_gradient_colour(p, t, rgba);
        return;
    }
    case GD_RADIAL: {
        /* The circle c(t), r(t) between the two given circles that passes
           through (x, y): the larger root of a quadratic in t. */
        double cdx = p->x2 - p->x1, cdy = p->y2 - p->y1, dr = p->r2 - p->r1;
        double pdx = x - p->x1, pdy = y - p->y1;
        double a = cdx * cdx + cdy * cdy - dr * dr;
        double b = pdx * cdx + pdy * cdy + p->r1 * dr;
        double c = pdx * pdx + pdy * pdy - p->r1 * p->r1;
        double t;
        if (fabs(a) < 1e-9) {
            if (fabs(b) < 1e-12) { rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0; return; }
            t = c / (2 * b);
        } else {
            double disc = b * b - a * c;
            if (disc < 0) { rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0; return; }
            double sq = sqrt(disc);
            t = (b + sq) / a;
            if (p->r1 + t * dr < 0) t = (b - sq) / a;
        }
        gd_gradient_colour(p, t, rgba);
        return;
    }
    case GD_TILE: {
        double u = x - p->tx, v = y - p->ty;
        int inside = u >= 0 && u < p->tw && v >= 0 && v < p->th;
        switch (p->extend) {
        case R_GE_patternExtendNone:
            if (!inside) { rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0; return; }
            break;
        case R_GE_patternExtendRepeat:
            u -= p->tw * floor(u / p->tw);
            v -= p->th * floor(v / p->th);
            break;
        case R_GE_patternExtendReflect:
            u = fabs(u - 2 * p->tw * floor(u / (2 * p->tw)) - p->tw);
            v = fabs(v - 2 * p->th * floor(v / (2 * p->th)) - p->th);
            u = p->tw - u;
            v = p->th - v;
            break;
        default:
            if (u < 0) u = 0;
            if (u >= p->tw) u = p->tw - 0.001;
            if (v < 0) v = 0;
            if (v >= p->th) v = p->th - 0.001;
        }
        int px = (int)(p->tx + u), py = (int)(p->ty + v);
        if (px < 0 || py < 0 || px >= p->tile->w || py >= p->tile->h) {
            rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0;
            return;
        }
        const float *q = p->tile->px + ((size_t)py * p->tile->w + px) * 4;
        float a = q[3];
        rgba[0] = a > 0 ? q[0] / a : 0;
        rgba[1] = a > 0 ? q[1] / a : 0;
        rgba[2] = a > 0 ? q[2] / a : 0;
        rgba[3] = a;
        return;
    }
    }
}

/* ---- stroking ------------------------------------------------------------- */

struct gd_pen {
    double width;    /* pixels */
    int cap, join;   /* GE_ROUND_CAP etc. */
    double mitre;    /* mitre limit */
};

/*
 * Outline one open or closed polyline. Segments become quads, joins add a
 * disc, a wedge or a mitre, caps extend or round off the ends. Everything is
 * added counter-clockwise so gd_fill's non-zero rule takes the union.
 */
static void gd_stroke_sub(struct gd_path *out, const struct gd_pt *pt, int n,
                          int closed, const struct gd_pen *pen)
{
    double hw = pen->width / 2;
    /* Drop repeated points. */
    struct gd_pt *v = gd_scratch(4, (size_t)(n + 1) * sizeof *v);
    if (!v) return;
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (m > 0 && fabsf(v[m - 1].x - pt[i].x) < 1e-6f && fabsf(v[m - 1].y - pt[i].y) < 1e-6f) continue;
        v[m++] = pt[i];
    }
    if (closed && m > 1 && fabsf(v[m - 1].x - v[0].x) < 1e-6f && fabsf(v[m - 1].y - v[0].y) < 1e-6f) m--;
    if (m == 1) {
        /* A dot: round caps show it as a disc, square caps as a square. */
        if (pen->cap == GE_ROUND_CAP) gd_path_circle(out, v[0].x, v[0].y, hw);
        else if (pen->cap == GE_SQUARE_CAP) {
            struct gd_pt q[4] = { { v[0].x - hw, v[0].y - hw }, { v[0].x + hw, v[0].y - hw },
                                  { v[0].x + hw, v[0].y + hw }, { v[0].x - hw, v[0].y + hw } };
            gd_path_poly(out, q, 4);
        }
        return;
    }
    if (m < 2) return;
    if (closed && m == 2) closed = 0;
    int nseg = closed ? m : m - 1;
    for (int i = 0; i < nseg; i++) {
        struct gd_pt a = v[i], b = v[(i + 1) % m];
        double dx = b.x - a.x, dy = b.y - a.y, len = hypot(dx, dy);
        if (len <= 0) continue;
        dx /= len; dy /= len;
        double ex = 0, ey = 0;   /* square caps extend the end segments */
        if (!closed && pen->cap == GE_SQUARE_CAP) { ex = dx * hw; ey = dy * hw; }
        double ax = a.x, ay = a.y, bx = b.x, by = b.y;
        if (!closed && i == 0)        { ax -= ex; ay -= ey; }
        if (!closed && i == nseg - 1) { bx += ex; by += ey; }
        double nx = -dy * hw, ny = dx * hw;
        struct gd_pt q[4] = { { (float)(ax + nx), (float)(ay + ny) }, { (float)(bx + nx), (float)(by + ny) },
                              { (float)(bx - nx), (float)(by - ny) }, { (float)(ax - nx), (float)(ay - ny) } };
        gd_path_poly(out, q, 4);
    }
    /* Joins. */
    if (hw > 0.7) {
        int first = closed ? 0 : 1, last = closed ? m : m - 1;
        for (int i = first; i < last; i++) {
            struct gd_pt p = v[(i - 1 + m) % m], c = v[i], q = v[(i + 1) % m];
            double d1x = c.x - p.x, d1y = c.y - p.y, l1 = hypot(d1x, d1y);
            double d2x = q.x - c.x, d2y = q.y - c.y, l2 = hypot(d2x, d2y);
            if (l1 <= 0 || l2 <= 0) continue;
            d1x /= l1; d1y /= l1; d2x /= l2; d2y /= l2;
            if (pen->join == GE_ROUND_JOIN) { gd_path_circle(out, c.x, c.y, hw); continue; }
            double n1x = -d1y, n1y = d1x, n2x = -d2y, n2y = d2x;
            double cosang = n1x * n2x + n1y * n2y;
            for (int side = -1; side <= 1; side += 2) {
                struct gd_pt o1 = { (float)(c.x + side * n1x * hw), (float)(c.y + side * n1y * hw) };
                struct gd_pt o2 = { (float)(c.x + side * n2x * hw), (float)(c.y + side * n2y * hw) };
                double ratio = 1 + cosang > 1e-6 ? sqrt(2 / (1 + cosang)) : 1e9;
                if (pen->join == GE_MITRE_JOIN && ratio <= pen->mitre) {
                    double f = hw / (1 + cosang);
                    struct gd_pt mp = { (float)(c.x + side * (n1x + n2x) * f), (float)(c.y + side * (n1y + n2y) * f) };
                    struct gd_pt t[4] = { c, o1, mp, o2 };
                    gd_path_poly(out, t, 4);
                } else {
                    struct gd_pt t[3] = { c, o1, o2 };
                    gd_path_poly(out, t, 3);
                }
            }
        }
    }
    if (!closed && pen->cap == GE_ROUND_CAP) {
        gd_path_circle(out, v[0].x, v[0].y, hw);
        gd_path_circle(out, v[m - 1].x, v[m - 1].y, hw);
    }
}

/*
 * Cut the subpaths of `in` into dashes after R's lty pattern: hex digits of
 * on, off, on, off lengths in units of the line width.
 */
static void gd_dash(const struct gd_path *in, struct gd_path *out, int lty, double unit)
{
    double pat[8];
    int np = 0;
    for (int v = lty; v && np < 8; v >>= 4) pat[np++] = (v & 15) * unit;
    if (np == 0) return;
    for (int s = 0; s < in->nsub; s++) {
        int a = gd_sub_start(in, s), b = gd_sub_end(in, s);
        int m = b - a + (gd_sub_closed(in, s) ? 1 : 0);
        int pi = 0, on = 1, drawing = 0;
        double left = pat[0];
        for (int i = 0; i + 1 < m; i++) {
            struct gd_pt p = in->pt[a + i], q = in->pt[a + (i + 1) % (b - a)];
            double dx = q.x - p.x, dy = q.y - p.y, len = hypot(dx, dy), pos = 0;
            if (len <= 0) continue;
            if (on && !drawing) { gd_path_move(out, p.x, p.y); drawing = 1; }
            while (len - pos > 1e-9) {
                double step = left < len - pos ? left : len - pos;
                pos += step;
                left -= step;
                double x = p.x + dx * pos / len, y = p.y + dy * pos / len;
                if (on) gd_path_line(out, x, y);
                if (left <= 1e-9) {
                    pi = (pi + 1) % np;
                    left = pat[pi];
                    on = !on;
                    if (on) { gd_path_move(out, x, y); drawing = 1; }
                    else drawing = 0;
                }
            }
        }
    }
}

/*
 * A thin line along a pixel row or column, drawn between two pixel centres,
 * comes out as one solid row rather than two half-grey ones. Subpaths made
 * only of horizontal and vertical segments (axes, boxes, ticks, grid lines)
 * are moved onto pixel centres for that; anything else is left alone.
 */
static void gd_snap(struct gd_path *p)
{
    for (int s = 0; s < p->nsub; s++) {
        int a = gd_sub_start(p, s), b = gd_sub_end(p, s), aligned = 1;
        for (int i = a; i + 1 < b && aligned; i++)
            if (p->pt[i].x != p->pt[i + 1].x && p->pt[i].y != p->pt[i + 1].y) aligned = 0;
        if (!aligned || b - a < 2) continue;
        for (int i = a; i < b; i++) {
            p->pt[i].x = floorf(p->pt[i].x) + 0.5f;
            p->pt[i].y = floorf(p->pt[i].y) + 0.5f;
        }
    }
}

static void gd_stroke(struct gd_canvas *cv, const struct gd_path *path, const struct gd_pen *pen,
                      int lty, const float rgba[4], const struct gd_clip *clip)
{
    if (rgba[3] <= 0 || lty == LTY_BLANK || pen->width <= 0) return;
    struct gd_path dashed, outline, snapped;
    gd_path_init(&dashed);
    gd_path_init(&outline);
    gd_path_init(&snapped);
    const struct gd_path *src = path;
    if (pen->width <= 1.5) {
        snapped = *path;
        snapped.pt = gd_scratch(5, (size_t)path->n * sizeof *snapped.pt);
        if (snapped.pt) {
            memcpy(snapped.pt, path->pt, (size_t)path->n * sizeof *snapped.pt);
            gd_snap(&snapped);
            src = &snapped;
        }
    }
    if (lty != LTY_SOLID) {
        gd_dash(src, &dashed, lty, pen->width > 1 ? pen->width : 1);
        src = &dashed;
    }
    for (int s = 0; s < src->nsub; s++) {
        int a = gd_sub_start(src, s), b = gd_sub_end(src, s);
        gd_stroke_sub(&outline, src->pt + a, b - a, gd_sub_closed(src, s), pen);
    }
    gd_fill(cv, &outline, 1, rgba, clip);
    gd_path_free(&dashed);
    gd_path_free(&outline);
}

/* ---- fonts ---------------------------------------------------------------- */

struct gd_font {
    char key[256];
    unsigned char *data;
    stbtt_fontinfo info;
    int ok;
    int ascent, descent, linegap;   /* font units */
};

#define GD_MAX_FONTS 32
static struct gd_font gd_fonts[GD_MAX_FONTS];
static int gd_nfonts = 0;
static int gd_font_warned = 0;

static unsigned char *gd_read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || st.st_size <= 0 || st.st_size > 64 * 1024 * 1024) { fclose(f); return NULL; }
    unsigned char *data = malloc((size_t)st.st_size);
    if (data && fread(data, 1, (size_t)st.st_size, f) != (size_t)st.st_size) { free(data); data = NULL; }
    fclose(f);
    return data;
}

static int gd_font_load_index(struct gd_font *font, const char *path, int index)
{
    unsigned char *data = gd_read_file(path);
    if (!data) return 0;
    int off = stbtt_GetFontOffsetForIndex(data, index);
    if (off < 0 || !stbtt_InitFont(&font->info, data, off)) { free(data); return 0; }
    font->data = data;
    stbtt_GetFontVMetrics(&font->info, &font->ascent, &font->descent, &font->linegap);
    font->ok = 1;
    return 1;
}

static int gd_font_load(struct gd_font *font, const char *path)
{
    return gd_font_load_index(font, path, 0);
}

/* A font by file and index, as R's glyph interface names them. */
static struct gd_font *gd_font_file(const char *file, int index)
{
    char key[256];
    snprintf(key, sizeof key, "%s#%d", file, index);
    for (int i = 0; i < gd_nfonts; i++)
        if (!strcmp(gd_fonts[i].key, key)) return &gd_fonts[i];
    if (gd_nfonts == GD_MAX_FONTS) return &gd_fonts[0];
    struct gd_font *font = &gd_fonts[gd_nfonts++];
    memset(font, 0, sizeof *font);
    snprintf(font->key, sizeof font->key, "%s", key);
    gd_font_load_index(font, file, index);
    return font;
}

/* Ask fontconfig for the file of a pattern such as "sans-serif:bold". */
static int gd_font_match(const char *pattern, char *out, size_t n)
{
    char clean[256];
    size_t k = 0;
    for (const char *p = pattern; *p && k < sizeof clean - 1; p++)
        if (isalnum((unsigned char)*p) || strchr(" _-.:=,", *p)) clean[k++] = *p;
    clean[k] = '\0';
    char cmd[512];
    snprintf(cmd, sizeof cmd, "fc-match -f '%%{file}' '%s' 2>/dev/null", clean);
    FILE *f = popen(cmd, "r");
    if (!f) return 0;
    size_t got = fread(out, 1, n - 1, f);
    pclose(f);
    out[got] = '\0';
    return got > 0 && out[0] == '/';
}

static const char *gd_family_name(const char *family)
{
    char low[64];
    size_t i;
    for (i = 0; family[i] && i < sizeof low - 1; i++) low[i] = (char)tolower((unsigned char)family[i]);
    low[i] = '\0';
    if (!*low || !strcmp(low, "sans") || !strcmp(low, "helvetica") || !strcmp(low, "arial") ||
        !strcmp(low, "symbol"))
        return "sans-serif";
    if (!strcmp(low, "serif") || !strcmp(low, "times")) return "serif";
    if (!strcmp(low, "mono") || !strcmp(low, "courier") || !strcmp(low, "monospace")) return "monospace";
    return family;
}

/*
 * The font for R's fontfamily and fontface (1 plain, 2 bold, 3 italic,
 * 4 bold italic, 5 symbol). ROPE_FONT names a family to use instead of the
 * default sans, or a font file to use for everything. Fontconfig picks the
 * file; without it a few well-known paths are tried.
 */
static struct gd_font *gd_font_get(const char *family, int face)
{
    if (face < 1 || face > 5) face = 1;
    const char *fam = gd_family_name(family);
    const char *env = getenv("ROPE_FONT");
    if (env && *env && !strcmp(fam, "sans-serif")) fam = env;
    int bold = face == 2 || face == 4, italic = face == 3 || face == 4;
    if (face == 5) bold = italic = 0;
    char key[256];
    snprintf(key, sizeof key, "%s|%d%d", fam, bold, italic);
    for (int i = 0; i < gd_nfonts; i++)
        if (!strcmp(gd_fonts[i].key, key)) return &gd_fonts[i];
    if (gd_nfonts == GD_MAX_FONTS) return &gd_fonts[0];

    struct gd_font *font = &gd_fonts[gd_nfonts++];
    memset(font, 0, sizeof *font);
    snprintf(font->key, sizeof font->key, "%s", key);

    char path[1024];
    if (strchr(fam, '/') && gd_font_load(font, fam)) return font;
    snprintf(path, sizeof path, "%s%s%s", fam, bold ? ":bold" : "", italic ? ":italic" : "");
    char file[1024];
    if (gd_font_match(path, file, sizeof file) && gd_font_load(font, file)) return font;

    static const char *const fallback[4][6] = {
        { "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
          "/usr/share/fonts/dejavu/DejaVuSans.ttf", "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
          "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", NULL },
        { "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
          "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
          "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf", NULL },
        { "/usr/share/fonts/truetype/dejavu/DejaVuSans-Oblique.ttf", "/usr/share/fonts/TTF/DejaVuSans-Oblique.ttf",
          "/usr/share/fonts/dejavu/DejaVuSans-Oblique.ttf", "/usr/share/fonts/truetype/liberation/LiberationSans-Italic.ttf",
          "/usr/share/fonts/truetype/noto/NotoSans-Italic.ttf", NULL },
        { "/usr/share/fonts/truetype/dejavu/DejaVuSans-BoldOblique.ttf", "/usr/share/fonts/TTF/DejaVuSans-BoldOblique.ttf",
          "/usr/share/fonts/dejavu/DejaVuSans-BoldOblique.ttf", "/usr/share/fonts/truetype/liberation/LiberationSans-BoldItalic.ttf",
          "/usr/share/fonts/truetype/noto/NotoSans-BoldItalic.ttf", NULL },
    };
    for (int i = 0; fallback[bold + 2 * italic][i]; i++)
        if (gd_font_load(font, fallback[bold + 2 * italic][i])) return font;
    /* Any face of the plain font is better than nothing. */
    for (int i = 0; fallback[0][i]; i++)
        if (gd_font_load(font, fallback[0][i])) return font;
    if (!gd_font_warned) {
        gd_font_warned = 1;
        fprintf(stderr, "rope: no font found for plot text; install fontconfig or set ROPE_FONT to a .ttf file\n");
    }
    return font;
}

/* One code point of UTF-8; malformed bytes come back one at a time. */
static int gd_utf8_next(const unsigned char **s)
{
    const unsigned char *p = *s;
    int c = p[0], n = 0;
    if (c < 0x80) n = 0;
    else if ((c & 0xE0) == 0xC0) { c &= 0x1F; n = 1; }
    else if ((c & 0xF0) == 0xE0) { c &= 0x0F; n = 2; }
    else if ((c & 0xF8) == 0xF0) { c &= 0x07; n = 3; }
    else { *s = p + 1; return c; }
    for (int i = 1; i <= n; i++) {
        if ((p[i] & 0xC0) != 0x80) { *s = p + 1; return p[0]; }
        c = (c << 6) | (p[i] & 0x3F);
    }
    *s = p + n + 1;
    return c;
}

struct gd_textstate {
    struct gd_font *font;
    double px;        /* font size in pixels */
    double scale;     /* font units to pixels */
};

static struct gd_textstate gd_text_font(const pGEcontext gc, double dpi)
{
    struct gd_textstate t;
    t.font = gd_font_get(gc->fontfamily, gc->fontface);
    t.px = gc->cex * gc->ps * dpi / 72.0;
    t.scale = t.font->ok ? stbtt_ScaleForMappingEmToPixels(&t.font->info, (float)t.px) : 0;
    return t;
}

static double gd_text_width(const struct gd_textstate *t, const char *str)
{
    if (!t->font->ok) return 0;
    double w = 0;
    int prev = 0;
    for (const unsigned char *s = (const unsigned char *)str; *s; ) {
        int c = gd_utf8_next(&s);
        int g = stbtt_FindGlyphIndex(&t->font->info, c);
        int adv, lsb;
        stbtt_GetGlyphHMetrics(&t->font->info, g, &adv, &lsb);
        if (prev) w += stbtt_GetGlyphKernAdvance(&t->font->info, prev, g) * t->scale;
        w += adv * t->scale;
        prev = g;
    }
    return w;
}

/*
 * Add a glyph's outline to `out`, placed at pen position `pen` along a
 * baseline rotated by `rot` radians. Glyph space has y up; the device has y
 * down, so the y axis is flipped on the way.
 */
static void gd_glyph_outline(struct gd_path *out, const stbtt_fontinfo *info, int glyph,
                             double scale, double ox, double oy, double pen, double cs, double sn)
{
    stbtt_vertex *v;
    int n = stbtt_GetGlyphShape(info, glyph, &v);
    double lx = 0, ly = 0;
#define GD_MAP(gx, gy, X, Y) do { double tx_ = (gx) * scale + pen, ty_ = (gy) * scale; \
        (X) = ox + tx_ * cs - ty_ * sn; (Y) = oy - tx_ * sn - ty_ * cs; } while (0)
    for (int i = 0; i < n; i++) {
        double x, y;
        switch (v[i].type) {
        case STBTT_vmove:
            GD_MAP(v[i].x, v[i].y, x, y);
            gd_path_move(out, x, y);
            lx = v[i].x; ly = v[i].y;
            break;
        case STBTT_vline:
            GD_MAP(v[i].x, v[i].y, x, y);
            gd_path_line(out, x, y);
            lx = v[i].x; ly = v[i].y;
            break;
        case STBTT_vcurve: {
            double d = (hypot(v[i].cx - lx, v[i].cy - ly) + hypot(v[i].x - v[i].cx, v[i].y - v[i].cy)) * scale;
            int steps = (int)(d / 2.5) + 2;
            if (steps > 24) steps = 24;
            for (int k = 1; k <= steps; k++) {
                double t = (double)k / steps, u = 1 - t;
                double gx = u * u * lx + 2 * u * t * v[i].cx + t * t * v[i].x;
                double gy = u * u * ly + 2 * u * t * v[i].cy + t * t * v[i].y;
                GD_MAP(gx, gy, x, y);
                gd_path_line(out, x, y);
            }
            lx = v[i].x; ly = v[i].y;
            break;
        }
        case STBTT_vcubic: {
            double d = (hypot(v[i].cx - lx, v[i].cy - ly) + hypot(v[i].cx1 - v[i].cx, v[i].cy1 - v[i].cy) +
                        hypot(v[i].x - v[i].cx1, v[i].y - v[i].cy1)) * scale;
            int steps = (int)(d / 2.5) + 2;
            if (steps > 32) steps = 32;
            for (int k = 1; k <= steps; k++) {
                double t = (double)k / steps, u = 1 - t;
                double gx = u * u * u * lx + 3 * u * u * t * v[i].cx + 3 * u * t * t * v[i].cx1 + t * t * t * v[i].x;
                double gy = u * u * u * ly + 3 * u * u * t * v[i].cy + 3 * u * t * t * v[i].cy1 + t * t * t * v[i].y;
                GD_MAP(gx, gy, x, y);
                gd_path_line(out, x, y);
            }
            lx = v[i].x; ly = v[i].y;
            break;
        }
        }
    }
#undef GD_MAP
    stbtt_FreeShape(info, v);
}

/* ---- zlib deflate, for kitty --------------------------------------------- */

/*
 * A plain deflate encoder: one block with the fixed Huffman code, greedy
 * LZ77 matches found through a hash of the next three bytes. Plots are
 * mostly flat colour, which this shrinks by an order of magnitude or more,
 * and it saves linking zlib.
 */
struct gd_bits {
    unsigned char *buf;
    size_t len, cap;
    unsigned int acc;
    int nacc;
};

static void gd_bits_byte(struct gd_bits *b, unsigned char c)
{
    if (b->len == b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 65536;
        unsigned char *p = realloc(b->buf, cap);
        if (!p) return;
        b->buf = p;
        b->cap = cap;
    }
    b->buf[b->len++] = c;
}

static void gd_bits_put(struct gd_bits *b, unsigned int value, int n)
{
    b->acc |= value << b->nacc;
    b->nacc += n;
    while (b->nacc >= 8) {
        gd_bits_byte(b, (unsigned char)(b->acc & 255));
        b->acc >>= 8;
        b->nacc -= 8;
    }
}

/* Huffman codes go out most significant bit first. */
static void gd_bits_code(struct gd_bits *b, unsigned int code, int n)
{
    unsigned int r = 0;
    for (int i = 0; i < n; i++) { r = (r << 1) | (code & 1); code >>= 1; }
    gd_bits_put(b, r, n);
}

static void gd_deflate_literal(struct gd_bits *b, int v)
{
    if (v < 144) gd_bits_code(b, 0x30 + v, 8);
    else gd_bits_code(b, 0x190 + v - 144, 9);
}

static void gd_deflate_match(struct gd_bits *b, int len, int dist)
{
    static const int lbase[] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
                                 67, 83, 99, 115, 131, 163, 195, 227, 258 };
    static const int lext[]  = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4,
                                 5, 5, 5, 5, 0 };
    static const int dbase[] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
                                 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
    static const int dext[]  = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10,
                                 11, 11, 12, 12, 13, 13 };
    int li = 28;
    while (li > 0 && lbase[li] > len) li--;
    int code = 257 + li;
    if (code < 280) gd_bits_code(b, code - 256, 7);
    else gd_bits_code(b, 0xC0 + code - 280, 8);
    if (lext[li]) gd_bits_put(b, (unsigned)(len - lbase[li]), lext[li]);
    int di = 29;
    while (di > 0 && dbase[di] > dist) di--;
    gd_bits_code(b, (unsigned)di, 5);
    if (dext[di]) gd_bits_put(b, (unsigned)(dist - dbase[di]), dext[di]);
}

static unsigned char *gd_zlib(const unsigned char *src, size_t n, size_t *outlen)
{
    struct gd_bits b = { 0 };
    gd_bits_byte(&b, 0x78);
    gd_bits_byte(&b, 0x01);
    gd_bits_put(&b, 1, 1);   /* final block */
    gd_bits_put(&b, 1, 2);   /* fixed Huffman */
    enum { HBITS = 15, HSIZE = 1 << HBITS, WINDOW = 32768, MAXLEN = 258 };
    int *head = malloc(HSIZE * sizeof *head);
    if (!head) { free(b.buf); return NULL; }
    for (int i = 0; i < HSIZE; i++) head[i] = -1;
    size_t i = 0;
    while (i < n) {
        int best = 0, bestd = 0;
        if (i + 3 <= n) {
            unsigned h = ((src[i] << 16) | (src[i + 1] << 8) | src[i + 2]) * 2654435761u >> (32 - HBITS);
            int cand = head[h];
            head[h] = (int)i;
            if (cand >= 0 && i - (size_t)cand <= WINDOW) {
                size_t lim = n - i < MAXLEN ? n - i : MAXLEN, k = 0;
                while (k < lim && src[cand + k] == src[i + k]) k++;
                if (k >= 3) { best = (int)k; bestd = (int)(i - (size_t)cand); }
            }
        }
        if (best) {
            gd_deflate_match(&b, best, bestd);
            /* Index the bytes inside the match too, cheaply: every fourth. */
            for (size_t j = i + 1; j < i + (size_t)best && j + 3 <= n; j += 4) {
                unsigned h = ((src[j] << 16) | (src[j + 1] << 8) | src[j + 2]) * 2654435761u >> (32 - HBITS);
                head[h] = (int)j;
            }
            i += (size_t)best;
        } else {
            gd_deflate_literal(&b, src[i]);
            i++;
        }
    }
    gd_bits_code(&b, 0, 7);   /* end of block */
    if (b.nacc > 0) gd_bits_put(&b, 0, 8 - b.nacc);
    unsigned a = 1, s = 0;
    for (size_t j = 0; j < n; j++) { a = (a + src[j]) % 65521; s = (s + a) % 65521; }
    unsigned adler = (s << 16) | a;
    gd_bits_byte(&b, (unsigned char)(adler >> 24));
    gd_bits_byte(&b, (unsigned char)(adler >> 16));
    gd_bits_byte(&b, (unsigned char)(adler >> 8));
    gd_bits_byte(&b, (unsigned char)adler);
    free(head);
    *outlen = b.len;
    return b.buf;
}

/* ---- image output --------------------------------------------------------- */

struct gd_out {
    char *p;
    size_t len, cap;
};

static void gd_out_put(struct gd_out *o, const char *s, size_t n)
{
    if (o->len + n + 1 > o->cap) {
        size_t cap = o->cap ? o->cap : 65536;
        while (cap < o->len + n + 1) cap *= 2;
        char *p = realloc(o->p, cap);
        if (!p) return;
        o->p = p;
        o->cap = cap;
    }
    memcpy(o->p + o->len, s, n);
    o->len += n;
    o->p[o->len] = '\0';
}

static void gd_out_str(struct gd_out *o, const char *s) { gd_out_put(o, s, strlen(s)); }

static void gd_out_char(struct gd_out *o, char c) { gd_out_put(o, &c, 1); }

static const char gd_b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/*
 * The kitty graphics protocol: the RGBA pixels, zlib-compressed and base64
 * encoded, transmitted and shown at the cursor in chunks of 4096 characters.
 * q=2 asks the terminal not to reply, so nothing lands in the input.
 */
static void gd_emit_kitty(struct gd_out *o, const struct gd_canvas *cv)
{
    size_t npx = (size_t)cv->w * cv->h;
    unsigned char *rgba = malloc(npx * 4);
    if (!rgba) return;
    for (size_t i = 0; i < npx; i++) {
        const float *p = cv->px + i * 4;
        float a = p[3];
        float r = a > 0 ? p[0] / a : 0, g = a > 0 ? p[1] / a : 0, b = a > 0 ? p[2] / a : 0;
        rgba[i * 4 + 0] = (unsigned char)(fminf(fmaxf(r, 0), 1) * 255 + 0.5f);
        rgba[i * 4 + 1] = (unsigned char)(fminf(fmaxf(g, 0), 1) * 255 + 0.5f);
        rgba[i * 4 + 2] = (unsigned char)(fminf(fmaxf(b, 0), 1) * 255 + 0.5f);
        rgba[i * 4 + 3] = (unsigned char)(fminf(fmaxf(a, 0), 1) * 255 + 0.5f);
    }
    size_t zlen = 0;
    unsigned char *z = gd_zlib(rgba, npx * 4, &zlen);
    free(rgba);
    if (!z) return;

    size_t b64len = (zlen + 2) / 3 * 4;
    char *b64 = malloc(b64len + 1);
    if (!b64) { free(z); return; }
    size_t k = 0;
    for (size_t i = 0; i < zlen; i += 3) {
        unsigned v = (unsigned)z[i] << 16;
        if (i + 1 < zlen) v |= (unsigned)z[i + 1] << 8;
        if (i + 2 < zlen) v |= z[i + 2];
        b64[k++] = gd_b64[(v >> 18) & 63];
        b64[k++] = gd_b64[(v >> 12) & 63];
        b64[k++] = i + 1 < zlen ? gd_b64[(v >> 6) & 63] : '=';
        b64[k++] = i + 2 < zlen ? gd_b64[v & 63] : '=';
    }
    free(z);

    char head[128];
    for (size_t i = 0; i < k; i += 4096) {
        size_t n = k - i < 4096 ? k - i : 4096;
        int more = i + n < k;
        if (i == 0)
            snprintf(head, sizeof head, "\x1b_Ga=T,f=32,o=z,q=2,s=%d,v=%d,m=%d;", cv->w, cv->h, more);
        else
            snprintf(head, sizeof head, "\x1b_Gm=%d;", more);
        gd_out_str(o, head);
        gd_out_put(o, b64 + i, n);
        gd_out_str(o, "\x1b\\");
    }
    gd_out_str(o, "\n");
    free(b64);
}

/*
 * Sixel needs a palette: the page is composited over white, then the
 * colours are cut down to 256 by median cut over a 15-bit histogram.
 */
struct gd_box { int start, end; long count; };

static int gd_sort_axis;   /* 0 red, 1 green, 2 blue: the axis a box is cut along */

static int gd_key_cmp(const void *a, const void *b)
{
    unsigned ka = *(const unsigned *)a, kb = *(const unsigned *)b;
    int va = (ka >> (10 - 5 * gd_sort_axis)) & 31, vb = (kb >> (10 - 5 * gd_sort_axis)) & 31;
    return va - vb;
}

static void gd_emit_sixel(struct gd_out *o, const struct gd_canvas *cv)
{
    size_t npx = (size_t)cv->w * cv->h;
    unsigned char *rgb = malloc(npx * 3);
    unsigned *count = calloc(32768, sizeof *count);
    unsigned long *sum = calloc(32768 * 3, sizeof *sum);
    unsigned short *map = malloc(32768 * sizeof *map);
    unsigned *keys = malloc(32768 * sizeof *keys);
    unsigned char *idx = malloc(npx);
    if (!rgb || !count || !sum || !map || !keys || !idx) goto done;

    for (size_t i = 0; i < npx; i++) {
        const float *p = cv->px + i * 4;
        float keep = 1 - p[3];
        int r = (int)(fminf(fmaxf(p[0] + keep, 0), 1) * 255 + 0.5f);
        int g = (int)(fminf(fmaxf(p[1] + keep, 0), 1) * 255 + 0.5f);
        int b = (int)(fminf(fmaxf(p[2] + keep, 0), 1) * 255 + 0.5f);
        rgb[i * 3] = (unsigned char)r; rgb[i * 3 + 1] = (unsigned char)g; rgb[i * 3 + 2] = (unsigned char)b;
        unsigned key = (unsigned)((r >> 3) << 10 | (g >> 3) << 5 | (b >> 3));
        count[key]++;
        sum[key * 3] += (unsigned long)r; sum[key * 3 + 1] += (unsigned long)g; sum[key * 3 + 2] += (unsigned long)b;
    }
    int nkeys = 0;
    for (unsigned k = 0; k < 32768; k++) if (count[k]) keys[nkeys++] = k;

    struct gd_box boxes[256];
    int nboxes = 1;
    boxes[0].start = 0; boxes[0].end = nkeys; boxes[0].count = (long)npx;
    while (nboxes < 256) {
        /* Split the most populous box that still holds more than one colour. */
        int pick = -1;
        for (int i = 0; i < nboxes; i++)
            if (boxes[i].end - boxes[i].start > 1 && (pick < 0 || boxes[i].count > boxes[pick].count)) pick = i;
        if (pick < 0) break;
        struct gd_box *bx = &boxes[pick];
        int lo[3] = { 31, 31, 31 }, hi[3] = { 0, 0, 0 };
        for (int i = bx->start; i < bx->end; i++)
            for (int a = 0; a < 3; a++) {
                int v = (keys[i] >> (10 - 5 * a)) & 31;
                if (v < lo[a]) lo[a] = v;
                if (v > hi[a]) hi[a] = v;
            }
        int axis = 0;
        for (int a = 1; a < 3; a++) if (hi[a] - lo[a] > hi[axis] - lo[axis]) axis = a;
        gd_sort_axis = axis;
        qsort(keys + bx->start, (size_t)(bx->end - bx->start), sizeof *keys, gd_key_cmp);
        long half = 0, acc = 0;
        int cut = bx->start + 1;
        for (int i = bx->start; i < bx->end; i++) {
            acc += count[keys[i]];
            if (acc * 2 >= bx->count) { cut = i + 1; half = acc; break; }
        }
        if (cut >= bx->end) { cut = bx->end - 1; half = bx->count - count[keys[cut]]; }
        struct gd_box *nb = &boxes[nboxes++];
        nb->start = cut; nb->end = bx->end; nb->count = bx->count - half;
        bx->end = cut; bx->count = half;
    }
    unsigned char pal[256][3];
    for (int i = 0; i < nboxes; i++) {
        unsigned long s[3] = { 0, 0, 0 }, c = 0;
        for (int j = boxes[i].start; j < boxes[i].end; j++) {
            map[keys[j]] = (unsigned short)i;
            for (int a = 0; a < 3; a++) s[a] += sum[keys[j] * 3 + a];
            c += count[keys[j]];
        }
        for (int a = 0; a < 3; a++) pal[i][a] = (unsigned char)(c ? s[a] / c : 0);
    }
    for (size_t i = 0; i < npx; i++) {
        unsigned key = (unsigned)((rgb[i * 3] >> 3) << 10 | (rgb[i * 3 + 1] >> 3) << 5 | (rgb[i * 3 + 2] >> 3));
        idx[i] = (unsigned char)map[key];
    }

    char buf[128];
    snprintf(buf, sizeof buf, "\x1bP0;1;0q\"1;1;%d;%d", cv->w, cv->h);
    gd_out_str(o, buf);
    for (int i = 0; i < nboxes; i++) {
        snprintf(buf, sizeof buf, "#%d;2;%d;%d;%d", i, (pal[i][0] * 100 + 127) / 255,
                 (pal[i][1] * 100 + 127) / 255, (pal[i][2] * 100 + 127) / 255);
        gd_out_str(o, buf);
    }
    unsigned char *seen = malloc(256);
    unsigned char *bits = malloc((size_t)cv->w);
    if (!seen || !bits) { free(seen); free(bits); goto done; }
    for (int band = 0; band < cv->h; band += 6) {
        int rows = cv->h - band < 6 ? cv->h - band : 6;
        memset(seen, 0, 256);
        for (int r = 0; r < rows; r++)
            for (int x = 0; x < cv->w; x++) seen[idx[(size_t)(band + r) * cv->w + x]] = 1;
        int first = 1;
        for (int c = 0; c < nboxes; c++) {
            if (!seen[c]) continue;
            if (!first) gd_out_char(o, '$');
            first = 0;
            snprintf(buf, sizeof buf, "#%d", c);
            gd_out_str(o, buf);
            int last = -1;
            for (int x = 0; x < cv->w; x++) {
                int b = 0;
                for (int r = 0; r < rows; r++)
                    if (idx[(size_t)(band + r) * cv->w + x] == c) b |= 1 << r;
                bits[x] = (unsigned char)b;
                if (b) last = x;
            }
            for (int x = 0; x <= last; ) {
                int run = 1;
                while (x + run <= last && bits[x + run] == bits[x]) run++;
                if (run >= 4) { snprintf(buf, sizeof buf, "!%d", run); gd_out_str(o, buf); gd_out_char(o, (char)(63 + bits[x])); }
                else for (int k = 0; k < run; k++) gd_out_char(o, (char)(63 + bits[x]));
                x += run;
            }
        }
        gd_out_char(o, '-');
    }
    gd_out_str(o, "\x1b\\\n");
    free(seen);
    free(bits);
done:
    free(rgb); free(count); free(sum); free(map); free(keys); free(idx);
}

/* ---- the device ----------------------------------------------------------- */

#define GD_MAX_PATTERNS 64
#define GD_MAX_GROUPS 64

struct gd_group { SEXP source, destination; int used; };

struct gd_dev {
    struct gd_canvas cv;
    double dpi;
    struct gd_clip clip;
    int dirty;                 /* drawn on since it was last shown */
    pDevDesc dd;
    struct gd_path *record;    /* when set, shapes are collected here, not drawn */
    float *clipmask, *mask;    /* page-sized coverage, or NULL */
    struct gd_paint *patterns[GD_MAX_PATTERNS];
    struct gd_group groups[GD_MAX_GROUPS];
};

#define GD_MAX_DEVICES 64
static struct gd_dev *gd_devices[GD_MAX_DEVICES];
static int gd_ndevices = 0;

static void gd_show(struct gd_dev *g)
{
    g->dirty = 0;
    if (gd_format == GD_NONE) return;
    struct gd_out o = { 0 };
    if (gd_format == GD_KITTY) gd_emit_kitty(&o, &g->cv);
    else gd_emit_sixel(&o, &g->cv);
    fflush(stdout);
    if (o.len) fwrite(o.p, 1, o.len, stdout);
    fflush(stdout);
    free(o.p);
}

/* rope_gd_flush(): show every page drawn on since it was last shown. */
void rope_gd_flush(void)
{
    for (int i = 0; i < gd_ndevices; i++)
        if (gd_devices[i]->dirty) gd_show(gd_devices[i]);
}

static double gd_lwd(const pGEcontext gc, const struct gd_dev *g)
{
    double w = gc->lwd * g->dpi / 96.0;
    return w < 1 ? 1 : w;
}

static struct gd_pen gd_pen_of(const pGEcontext gc, const struct gd_dev *g)
{
    struct gd_pen pen;
    pen.width = gd_lwd(gc, g);
    pen.cap = gc->lend;
    pen.join = gc->ljoin;
    pen.mitre = gc->lmitre > 1 ? gc->lmitre : 1;
    return pen;
}

/* The pattern a gcontext's fill refers to, if any. */
static const struct gd_paint *gd_pattern_of(const struct gd_dev *g, const pGEcontext gc)
{
    SEXP ref = gc->patternFill;
    if (!ref || ref == R_NilValue || TYPEOF(ref) != INTSXP || XLENGTH(ref) < 1) return NULL;
    int i = INTEGER(ref)[0];
    if (i < 0 || i >= GD_MAX_PATTERNS) return NULL;
    return g->patterns[i];
}

/* While a path is being recorded, shapes go there instead of the page. */
static void gd_record(struct gd_dev *g, const struct gd_path *p)
{
    for (int s = 0; s < p->nsub; s++) {
        int a = gd_sub_start(p, s), b = gd_sub_end(p, s);
        g->record->closed = gd_sub_closed(p, s);
        for (int i = a; i < b; i++) {
            if (i == a) gd_path_move(g->record, p->pt[i].x, p->pt[i].y);
            else gd_path_line(g->record, p->pt[i].x, p->pt[i].y);
        }
    }
}

static void gd_fill_stroke(struct gd_dev *g, struct gd_path *p, int rule, const pGEcontext gc)
{
    if (g->record) { gd_record(g, p); return; }
    float rgba[4];
    const struct gd_paint *pattern = gd_pattern_of(g, gc);
    if (pattern) {
        gd_fill_paint(&g->cv, p, rule, pattern, &g->clip);
    } else if (!R_TRANSPARENT(gc->fill)) {
        gd_colour((unsigned)gc->fill, rgba);
        gd_fill(&g->cv, p, rule, rgba, &g->clip);
    }
    if (!R_TRANSPARENT(gc->col) && gc->lty != LTY_BLANK) {
        gd_colour((unsigned)gc->col, rgba);
        struct gd_pen pen = gd_pen_of(gc, g);
        gd_stroke(&g->cv, p, &pen, gc->lty, rgba, &g->clip);
    }
    g->dirty = 1;
}

static void gd_cb_activate(const pDevDesc dd) { (void)dd; }
static void gd_cb_deactivate(pDevDesc dd) { (void)dd; }
static void gd_cb_mode(int mode, pDevDesc dd) { (void)mode; (void)dd; }
static int gd_cb_holdflush(pDevDesc dd, int level) { (void)dd; (void)level; return 0; }

static void gd_cb_size(double *left, double *right, double *bottom, double *top, pDevDesc dd)
{
    struct gd_dev *g = dd->deviceSpecific;
    *left = 0; *right = g->cv.w; *bottom = g->cv.h; *top = 0;
}

/*
 * A rectangular clip replaces any clipping path as well: that is how the
 * engine restores a parent viewport's clip after a clipping path, and how
 * cairo treats it. A clipping path set afterwards intersects the rectangle.
 */
static void gd_cb_clip(double x0, double x1, double y0, double y1, pDevDesc dd)
{
    struct gd_dev *g = dd->deviceSpecific;
    free(g->clipmask);
    g->clipmask = NULL;
    g->clip.clipmask = NULL;
    g->clip.x0 = (float)fmin(x0, x1);
    g->clip.x1 = (float)fmax(x0, x1);
    g->clip.y0 = (float)fmin(y0, y1);
    g->clip.y1 = (float)fmax(y0, y1);
}

static void gd_cb_newpage(const pGEcontext gc, pDevDesc dd)
{
    struct gd_dev *g = dd->deviceSpecific;
    if (g->dirty) gd_show(g);
    float rgba[4] = { 1, 1, 1, 0 };
    if (!R_TRANSPARENT(gc->fill)) gd_colour((unsigned)gc->fill, rgba);
    gd_canvas_clear(&g->cv, rgba);
    g->clip.x0 = 0; g->clip.y0 = 0; g->clip.x1 = (float)g->cv.w; g->clip.y1 = (float)g->cv.h;
    free(g->clipmask);
    free(g->mask);
    g->clipmask = g->mask = NULL;
    g->clip.clipmask = g->clip.mask = NULL;
}

static void gd_cb_close(pDevDesc dd)
{
    struct gd_dev *g = dd->deviceSpecific;
    if (g->dirty) gd_show(g);
    for (int i = 0; i < gd_ndevices; i++)
        if (gd_devices[i] == g) {
            memmove(gd_devices + i, gd_devices + i + 1, (size_t)(gd_ndevices - i - 1) * sizeof *gd_devices);
            gd_ndevices--;
            break;
        }
    free(g->cv.px);
    free(g->clipmask);
    free(g->mask);
    for (int i = 0; i < GD_MAX_PATTERNS; i++)
        if (g->patterns[i]) {
            struct gd_paint *p = g->patterns[i];
            free(p->stops);
            free(p->colours);
            if (p->tile) { free(p->tile->px); free((void *)p->tile); }
            free(p);
        }
    for (int i = 0; i < GD_MAX_GROUPS; i++)
        if (g->groups[i].used) {
            R_ReleaseObject(g->groups[i].source);
            if (g->groups[i].destination != R_NilValue) R_ReleaseObject(g->groups[i].destination);
        }
    free(g);
    dd->deviceSpecific = NULL;
}

static void gd_cb_line(double x1, double y1, double x2, double y2, const pGEcontext gc, pDevDesc dd)
{
    struct gd_dev *g = dd->deviceSpecific;
    if (R_TRANSPARENT(gc->col) && !g->record) return;
    struct gd_path p;
    gd_path_init(&p);
    gd_path_move(&p, x1, y1);
    gd_path_line(&p, x2, y2);
    if (g->record) { gd_record(g, &p); gd_path_free(&p); return; }
    float rgba[4];
    gd_colour((unsigned)gc->col, rgba);
    struct gd_pen pen = gd_pen_of(gc, g);
    gd_stroke(&g->cv, &p, &pen, gc->lty, rgba, &g->clip);
    g->dirty = 1;
    gd_path_free(&p);
}

static void gd_cb_polyline(int n, double *x, double *y, const pGEcontext gc, pDevDesc dd)
{
    struct gd_dev *g = dd->deviceSpecific;
    if ((R_TRANSPARENT(gc->col) && !g->record) || n < 1) return;
    struct gd_path p;
    gd_path_init(&p);
    for (int i = 0; i < n; i++) {
        if (!R_FINITE(x[i]) || !R_FINITE(y[i])) continue;
        if (p.n == 0) gd_path_move(&p, x[i], y[i]); else gd_path_line(&p, x[i], y[i]);
    }
    if (g->record) { gd_record(g, &p); gd_path_free(&p); return; }
    float rgba[4];
    gd_colour((unsigned)gc->col, rgba);
    struct gd_pen pen = gd_pen_of(gc, g);
    gd_stroke(&g->cv, &p, &pen, gc->lty, rgba, &g->clip);
    g->dirty = 1;
    gd_path_free(&p);
}

static void gd_cb_polygon(int n, double *x, double *y, const pGEcontext gc, pDevDesc dd)
{
    struct gd_dev *g = dd->deviceSpecific;
    if (n < 2) return;
    struct gd_path p;
    gd_path_init(&p);
    p.closed = 1;
    for (int i = 0; i < n; i++) {
        if (!R_FINITE(x[i]) || !R_FINITE(y[i])) continue;
        if (p.n == 0) gd_path_move(&p, x[i], y[i]); else gd_path_line(&p, x[i], y[i]);
    }
    gd_fill_stroke(g, &p, 1, gc);
    gd_path_free(&p);
}

static void gd_cb_path(double *x, double *y, int npoly, int *nper, Rboolean winding,
                       const pGEcontext gc, pDevDesc dd)
{
    struct gd_dev *g = dd->deviceSpecific;
    struct gd_path p;
    gd_path_init(&p);
    p.closed = 1;
    int k = 0;
    for (int i = 0; i < npoly; i++) {
        for (int j = 0; j < nper[i]; j++, k++) {
            if (!R_FINITE(x[k]) || !R_FINITE(y[k])) continue;
            if (j == 0) gd_path_move(&p, x[k], y[k]); else gd_path_line(&p, x[k], y[k]);
        }
    }
    gd_fill_stroke(g, &p, winding ? 1 : 2, gc);
    gd_path_free(&p);
}

static void gd_cb_rect(double x0, double y0, double x1, double y1, const pGEcontext gc, pDevDesc dd)
{
    struct gd_dev *g = dd->deviceSpecific;
    struct gd_path p;
    gd_path_init(&p);
    p.closed = 1;
    gd_path_move(&p, x0, y0);
    gd_path_line(&p, x1, y0);
    gd_path_line(&p, x1, y1);
    gd_path_line(&p, x0, y1);
    gd_fill_stroke(g, &p, 1, gc);
    gd_path_free(&p);
}

static void gd_cb_circle(double x, double y, double r, const pGEcontext gc, pDevDesc dd)
{
    struct gd_dev *g = dd->deviceSpecific;
    struct gd_path p;
    gd_path_init(&p);
    p.closed = 1;
    gd_path_circle(&p, x, y, r > 0.5 ? r : 0.5);
    gd_fill_stroke(g, &p, 1, gc);
    gd_path_free(&p);
}

/*
 * A raster image with its bottom-left corner at (x, y), `width` by `height`
 * device units (height is negative here, since y grows downwards), turned
 * `rot` degrees counter-clockwise. Each device pixel inside the image's
 * outline is mapped back to a source pixel, by nearest neighbour or by
 * bilinear interpolation.
 */
static void gd_cb_raster(unsigned int *raster, int w, int h, double x, double y, double width,
                         double height, double rot, Rboolean interpolate, const pGEcontext gc, pDevDesc dd)
{
    (void)gc;
    struct gd_dev *g = dd->deviceSpecific;
    if (w <= 0 || h <= 0 || g->record) return;
    double th = rot * GD_PI / 180, cs = cos(th), sn = sin(th);
    /* u runs along the image's width, v from its bottom edge to its top. */
    double ux = width * cs, uy = -width * sn;
    double vx = height * sn, vy = height * cs;
    double det = ux * vy - uy * vx;
    if (fabs(det) < 1e-12) return;
    double xs[4] = { x, x + ux, x + ux + vx, x + vx }, ys[4] = { y, y + uy, y + uy + vy, y + vy };
    double minx = xs[0], maxx = xs[0], miny = ys[0], maxy = ys[0];
    for (int i = 1; i < 4; i++) {
        if (xs[i] < minx) minx = xs[i];
        if (xs[i] > maxx) maxx = xs[i];
        if (ys[i] < miny) miny = ys[i];
        if (ys[i] > maxy) maxy = ys[i];
    }
    int px0 = (int)floor(fmax(minx, g->clip.x0)), px1 = (int)ceil(fmin(maxx, g->clip.x1));
    int py0 = (int)floor(fmax(miny, g->clip.y0)), py1 = (int)ceil(fmin(maxy, g->clip.y1));
    if (px0 < 0) px0 = 0;
    if (py0 < 0) py0 = 0;
    if (px1 > g->cv.w) px1 = g->cv.w;
    if (py1 > g->cv.h) py1 = g->cv.h;
    for (int py = py0; py < py1; py++) {
        for (int px = px0; px < px1; px++) {
            double dx = px + 0.5 - x, dy = py + 0.5 - y;
            double s = (dx * vy - dy * vx) / det, t = (ux * dy - uy * dx) / det;
            if (s < 0 || s >= 1 || t < 0 || t >= 1) continue;
            double sx = s * w, sy = (1 - t) * h;   /* row 0 of the image is its top */
            float rgba[4];
            if (interpolate) {
                double fx = sx - 0.5, fy = sy - 0.5;
                int ix = (int)floor(fx), iy = (int)floor(fy);
                double ax = fx - ix, ay = fy - iy;
                float acc[4] = { 0, 0, 0, 0 };
                for (int j = 0; j < 2; j++)
                    for (int i = 0; i < 2; i++) {
                        int cx = ix + i, cy = iy + j;
                        if (cx < 0) cx = 0;
                        if (cy < 0) cy = 0;
                        if (cx >= w) cx = w - 1;
                        if (cy >= h) cy = h - 1;
                        double wgt = (i ? ax : 1 - ax) * (j ? ay : 1 - ay);
                        unsigned c = raster[cy * w + cx];
                        float a = R_ALPHA(c) / 255.0f;
                        acc[0] += (float)wgt * R_RED(c) / 255.0f * a;
                        acc[1] += (float)wgt * R_GREEN(c) / 255.0f * a;
                        acc[2] += (float)wgt * R_BLUE(c) / 255.0f * a;
                        acc[3] += (float)wgt * a;
                    }
                if (acc[3] <= 0) continue;
                rgba[0] = acc[0] / acc[3]; rgba[1] = acc[1] / acc[3]; rgba[2] = acc[2] / acc[3]; rgba[3] = acc[3];
            } else {
                int ix = (int)sx, iy = (int)sy;
                if (ix >= w) ix = w - 1;
                if (iy >= h) iy = h - 1;
                gd_colour(raster[iy * w + ix], rgba);
            }
            gd_blend(g->cv.px + ((size_t)py * g->cv.w + px) * 4, rgba, gd_clip_at(&g->clip, px, py));
        }
    }
    g->dirty = 1;
}

static void gd_cb_metric(int c, const pGEcontext gc, double *ascent, double *descent, double *width, pDevDesc dd)
{
    struct gd_dev *g = dd->deviceSpecific;
    *ascent = *descent = *width = 0;
    struct gd_textstate t = gd_text_font(gc, g->dpi);
    if (!t.font->ok) return;
    if (c == 0) c = 'M';
    if (c < 0) c = -c;
    int glyph = stbtt_FindGlyphIndex(&t.font->info, c);
    int x0, y0, x1, y1, adv, lsb;
    if (stbtt_GetGlyphBox(&t.font->info, glyph, &x0, &y0, &x1, &y1)) {
        *ascent = y1 > 0 ? y1 * t.scale : 0;
        *descent = y0 < 0 ? -y0 * t.scale : 0;
    }
    stbtt_GetGlyphHMetrics(&t.font->info, glyph, &adv, &lsb);
    *width = adv * t.scale;
}

static double gd_cb_strwidth(const char *str, const pGEcontext gc, pDevDesc dd)
{
    struct gd_dev *g = dd->deviceSpecific;
    struct gd_textstate t = gd_text_font(gc, g->dpi);
    return gd_text_width(&t, str);
}

static void gd_cb_text(double x, double y, const char *str, double rot, double hadj,
                       const pGEcontext gc, pDevDesc dd)
{
    struct gd_dev *g = dd->deviceSpecific;
    if (R_TRANSPARENT(gc->col) || !*str || g->record) return;
    struct gd_textstate t = gd_text_font(gc, g->dpi);
    if (!t.font->ok) return;
    double th = rot * GD_PI / 180, cs = cos(th), sn = sin(th);
    double pen = -hadj * gd_text_width(&t, str);
    struct gd_path p;
    gd_path_init(&p);
    int prev = 0;
    for (const unsigned char *s = (const unsigned char *)str; *s; ) {
        int c = gd_utf8_next(&s);
        int glyph = stbtt_FindGlyphIndex(&t.font->info, c);
        if (prev) pen += stbtt_GetGlyphKernAdvance(&t.font->info, prev, glyph) * t.scale;
        gd_glyph_outline(&p, &t.font->info, glyph, t.scale, x, y, pen, cs, sn);
        int adv, lsb;
        stbtt_GetGlyphHMetrics(&t.font->info, glyph, &adv, &lsb);
        pen += adv * t.scale;
        prev = glyph;
    }
    float rgba[4];
    gd_colour((unsigned)gc->col, rgba);
    gd_fill(&g->cv, &p, 1, rgba, &g->clip);
    g->dirty = 1;
    gd_path_free(&p);
}

static SEXP gd_cb_cap(pDevDesc dd)
{
    struct gd_dev *g = dd->deviceSpecific;
    SEXP out = PROTECT(Rf_allocMatrix(INTSXP, g->cv.h, g->cv.w));
    int *v = INTEGER(out);
    for (int py = 0; py < g->cv.h; py++)
        for (int px = 0; px < g->cv.w; px++) {
            const float *p = g->cv.px + ((size_t)py * g->cv.w + px) * 4;
            float a = p[3];
            int r = (int)(fminf(fmaxf(a > 0 ? p[0] / a : 0, 0), 1) * 255 + 0.5f);
            int gg = (int)(fminf(fmaxf(a > 0 ? p[1] / a : 0, 0), 1) * 255 + 0.5f);
            int b = (int)(fminf(fmaxf(a > 0 ? p[2] / a : 0, 0), 1) * 255 + 0.5f);
            int al = (int)(fminf(fmaxf(a, 0), 1) * 255 + 0.5f);
            v[px * g->cv.h + py] = (int)R_RGBA(r, gg, b, al);
        }
    UNPROTECT(1);
    return out;
}

/* ---- paths, patterns, clipping paths, masks, groups, glyphs ----------------- */

/*
 * These take R functions that draw: the drawing is run with the device
 * pointed somewhere else. R_ToplevelExec keeps an error inside from
 * unwinding through the device with the redirection still in place.
 */
static void gd_eval_cb(void *data)
{
    SEXP call = PROTECT(Rf_lang1((SEXP)data));
    Rf_eval(call, R_GlobalEnv);
    UNPROTECT(1);
}

static int gd_run(SEXP fn)
{
    return R_ToplevelExec(gd_eval_cb, fn);
}

/* Collect what `fn` draws as one path. */
static void gd_record_path(struct gd_dev *g, SEXP fn, struct gd_path *out)
{
    gd_path_init(out);
    struct gd_path *saved = g->record;
    g->record = out;
    gd_run(fn);
    g->record = saved;
}

/* Draw what `fn` draws onto a fresh transparent page instead of the real one. */
static int gd_draw_offscreen(struct gd_dev *g, SEXP fn, struct gd_canvas *out)
{
    if (!gd_canvas_init(out, g->cv.w, g->cv.h)) return 0;
    struct gd_canvas saved = g->cv;
    struct gd_clip savedclip = g->clip;
    struct gd_path *savedrec = g->record;
    g->cv = *out;
    g->clip.clipmask = g->clip.mask = NULL;
    g->record = NULL;
    gd_run(fn);
    *out = g->cv;
    g->cv = saved;
    g->clip = savedclip;
    g->record = savedrec;
    return 1;
}

static void gd_cb_stroke(SEXP path, const pGEcontext gc, pDevDesc dd)
{
    struct gd_dev *g = dd->deviceSpecific;
    struct gd_path p;
    gd_record_path(g, path, &p);
    if (!R_TRANSPARENT(gc->col) && gc->lty != LTY_BLANK) {
        float rgba[4];
        gd_colour((unsigned)gc->col, rgba);
        struct gd_pen pen = gd_pen_of(gc, g);
        gd_stroke(&g->cv, &p, &pen, gc->lty, rgba, &g->clip);
        g->dirty = 1;
    }
    gd_path_free(&p);
}

static void gd_cb_fill(SEXP path, int rule, const pGEcontext gc, pDevDesc dd)
{
    struct gd_dev *g = dd->deviceSpecific;
    struct gd_path p;
    gd_record_path(g, path, &p);
    const struct gd_paint *pattern = gd_pattern_of(g, gc);
    if (pattern) gd_fill_paint(&g->cv, &p, rule == R_GE_evenOddRule ? 2 : 1, pattern, &g->clip);
    else if (!R_TRANSPARENT(gc->fill)) {
        float rgba[4];
        gd_colour((unsigned)gc->fill, rgba);
        gd_fill(&g->cv, &p, rule == R_GE_evenOddRule ? 2 : 1, rgba, &g->clip);
    }
    g->dirty = 1;
    gd_path_free(&p);
}

static void gd_cb_fillstroke(SEXP path, int rule, const pGEcontext gc, pDevDesc dd)
{
    struct gd_dev *g = dd->deviceSpecific;
    struct gd_path p;
    gd_record_path(g, path, &p);
    gd_fill_stroke(g, &p, rule == R_GE_evenOddRule ? 2 : 1, gc);
    gd_path_free(&p);
}

static SEXP gd_cb_setpattern(SEXP pattern, pDevDesc dd)
{
    struct gd_dev *g = dd->deviceSpecific;
    int slot = 0;
    while (slot < GD_MAX_PATTERNS && g->patterns[slot]) slot++;
    if (slot == GD_MAX_PATTERNS) return R_NilValue;
    struct gd_paint *p = calloc(1, sizeof *p);
    if (!p) return R_NilValue;
    int type = R_GE_patternType(pattern);
    if (type == R_GE_linearGradientPattern || type == R_GE_radialGradientPattern) {
        int n;
        if (type == R_GE_linearGradientPattern) {
            p->kind = GD_LINEAR;
            p->x1 = R_GE_linearGradientX1(pattern); p->y1 = R_GE_linearGradientY1(pattern);
            p->x2 = R_GE_linearGradientX2(pattern); p->y2 = R_GE_linearGradientY2(pattern);
            p->extend = R_GE_linearGradientExtend(pattern);
            n = R_GE_linearGradientNumStops(pattern);
        } else {
            p->kind = GD_RADIAL;
            p->x1 = R_GE_radialGradientCX1(pattern); p->y1 = R_GE_radialGradientCY1(pattern);
            p->r1 = R_GE_radialGradientR1(pattern);
            p->x2 = R_GE_radialGradientCX2(pattern); p->y2 = R_GE_radialGradientCY2(pattern);
            p->r2 = R_GE_radialGradientR2(pattern);
            p->extend = R_GE_radialGradientExtend(pattern);
            n = R_GE_radialGradientNumStops(pattern);
        }
        p->stops = malloc((size_t)(n > 0 ? n : 1) * sizeof *p->stops);
        p->colours = malloc((size_t)(n > 0 ? n : 1) * sizeof *p->colours);
        if (!p->stops || !p->colours) { free(p->stops); free(p->colours); free(p); return R_NilValue; }
        p->nstops = n;
        for (int i = 0; i < n; i++) {
            p->stops[i] = type == R_GE_linearGradientPattern ? R_GE_linearGradientStop(pattern, i)
                                                             : R_GE_radialGradientStop(pattern, i);
            gd_colour(type == R_GE_linearGradientPattern ? R_GE_linearGradientColour(pattern, i)
                                                         : R_GE_radialGradientColour(pattern, i), p->colours[i]);
        }
    } else if (type == R_GE_tilingPattern) {
        p->kind = GD_TILE;
        double x = R_GE_tilingPatternX(pattern), y = R_GE_tilingPatternY(pattern);
        double w = R_GE_tilingPatternWidth(pattern), h = R_GE_tilingPatternHeight(pattern);
        p->tx = fmin(x, x + w); p->ty = fmin(y, y + h);
        p->tw = fabs(w); p->th = fabs(h);
        p->extend = R_GE_tilingPatternExtend(pattern);
        if (p->tw < 1 || p->th < 1) { free(p); return R_NilValue; }
        struct gd_canvas *tile = calloc(1, sizeof *tile);
        if (!tile || !gd_draw_offscreen(g, R_GE_tilingPatternFunction(pattern), tile)) {
            free(tile);
            free(p);
            return R_NilValue;
        }
        p->tile = tile;
    } else {
        free(p);
        return R_NilValue;
    }
    g->patterns[slot] = p;
    return Rf_ScalarInteger(slot);
}

static void gd_cb_releasepattern(SEXP ref, pDevDesc dd)
{
    struct gd_dev *g = dd->deviceSpecific;
    if (ref == R_NilValue || TYPEOF(ref) != INTSXP || XLENGTH(ref) < 1) return;
    int i = INTEGER(ref)[0];
    if (i < 0 || i >= GD_MAX_PATTERNS || !g->patterns[i]) return;
    struct gd_paint *p = g->patterns[i];
    free(p->stops);
    free(p->colours);
    if (p->tile) { free(p->tile->px); free((void *)p->tile); }
    free(p);
    g->patterns[i] = NULL;
}

/*
 * A clipping path is rasterised once into a coverage buffer; a mask is
 * drawn offscreen and its alpha (or luminance times alpha) kept. Both then
 * scale the coverage of everything drawn. The references handed back are
 * tokens: the engine passes the path again whenever it sets one.
 */
static SEXP gd_cb_setclippath(SEXP path, SEXP ref, pDevDesc dd)
{
    (void)ref;
    struct gd_dev *g = dd->deviceSpecific;
    free(g->clipmask);
    g->clipmask = NULL;
    g->clip.clipmask = NULL;
    if (path == R_NilValue) return R_NilValue;
    struct gd_path p;
    gd_record_path(g, path, &p);
    struct gd_canvas cv;
    if (gd_canvas_init(&cv, g->cv.w, g->cv.h)) {
        struct gd_clip all = { 0, 0, (float)cv.w, (float)cv.h, NULL, NULL, cv.w };
        float white[4] = { 1, 1, 1, 1 };
        gd_fill(&cv, &p, R_GE_clipPathFillRule(path) == R_GE_evenOddRule ? 2 : 1, white, &all);
        g->clipmask = malloc((size_t)cv.w * cv.h * sizeof *g->clipmask);
        if (g->clipmask) {
            for (size_t i = 0; i < (size_t)cv.w * cv.h; i++) g->clipmask[i] = cv.px[i * 4 + 3];
            g->clip.clipmask = g->clipmask;
        }
        free(cv.px);
    }
    gd_path_free(&p);
    return Rf_ScalarInteger(1);
}

static void gd_cb_releaseclippath(SEXP ref, pDevDesc dd) { (void)ref; (void)dd; }

static SEXP gd_cb_setmask(SEXP path, SEXP ref, pDevDesc dd)
{
    (void)ref;
    struct gd_dev *g = dd->deviceSpecific;
    free(g->mask);
    g->mask = NULL;
    g->clip.mask = NULL;
    if (path == R_NilValue) return R_NilValue;
    int luminance = R_GE_maskType(path) == R_GE_luminanceMask;
    struct gd_canvas cv;
    if (!gd_draw_offscreen(g, path, &cv)) return R_NilValue;
    g->mask = malloc((size_t)cv.w * cv.h * sizeof *g->mask);
    if (g->mask) {
        for (size_t i = 0; i < (size_t)cv.w * cv.h; i++) {
            const float *q = cv.px + i * 4;
            g->mask[i] = luminance ? 0.2126f * q[0] + 0.7152f * q[1] + 0.0722f * q[2] : q[3];
        }
        g->clip.mask = g->mask;
    }
    free(cv.px);
    return Rf_ScalarInteger(1);
}

static void gd_cb_releasemask(SEXP ref, pDevDesc dd) { (void)ref; (void)dd; }

/*
 * Groups are kept as their two drawing functions and drawn when used, with
 * the plain "over" operator and no transformation: the compositing
 * operators and affine transforms of R 4.2's groups are not supported.
 */
static SEXP gd_cb_definegroup(SEXP source, int op, SEXP destination, pDevDesc dd)
{
    (void)op;
    struct gd_dev *g = dd->deviceSpecific;
    int slot = 0;
    while (slot < GD_MAX_GROUPS && g->groups[slot].used) slot++;
    if (slot == GD_MAX_GROUPS) return R_NilValue;
    g->groups[slot].used = 1;
    g->groups[slot].source = source;
    g->groups[slot].destination = destination;
    R_PreserveObject(source);
    if (destination != R_NilValue) R_PreserveObject(destination);
    return Rf_ScalarInteger(slot);
}

static void gd_cb_usegroup(SEXP ref, SEXP trans, pDevDesc dd)
{
    (void)trans;
    struct gd_dev *g = dd->deviceSpecific;
    if (ref == R_NilValue || TYPEOF(ref) != INTSXP || XLENGTH(ref) < 1) return;
    int i = INTEGER(ref)[0];
    if (i < 0 || i >= GD_MAX_GROUPS || !g->groups[i].used) return;
    if (g->groups[i].destination != R_NilValue) gd_run(g->groups[i].destination);
    gd_run(g->groups[i].source);
}

static void gd_cb_releasegroup(SEXP ref, pDevDesc dd)
{
    struct gd_dev *g = dd->deviceSpecific;
    if (ref == R_NilValue || TYPEOF(ref) != INTSXP || XLENGTH(ref) < 1) return;
    int i = INTEGER(ref)[0];
    if (i < 0 || i >= GD_MAX_GROUPS || !g->groups[i].used) return;
    R_ReleaseObject(g->groups[i].source);
    if (g->groups[i].destination != R_NilValue) R_ReleaseObject(g->groups[i].destination);
    g->groups[i].used = 0;
}

/* dev.capabilities() wants every entry as an integer vector, yes/no included. */
static SEXP gd_cb_capabilities(SEXP cap)
{
    SEXP v;
    v = PROTECT(Rf_allocVector(INTSXP, 3));
    INTEGER(v)[0] = R_GE_linearGradientPattern;
    INTEGER(v)[1] = R_GE_radialGradientPattern;
    INTEGER(v)[2] = R_GE_tilingPattern;
    SET_VECTOR_ELT(cap, R_GE_capability_patterns, v);
    SET_VECTOR_ELT(cap, R_GE_capability_clippingPaths, Rf_ScalarInteger(1));
    v = PROTECT(Rf_allocVector(INTSXP, 2));
    INTEGER(v)[0] = R_GE_alphaMask;
    INTEGER(v)[1] = R_GE_luminanceMask;
    SET_VECTOR_ELT(cap, R_GE_capability_masks, v);
    SET_VECTOR_ELT(cap, R_GE_capability_compositing, Rf_ScalarInteger(R_GE_compositeOver));
    SET_VECTOR_ELT(cap, R_GE_capability_transformations, Rf_ScalarInteger(0));
    SET_VECTOR_ELT(cap, R_GE_capability_paths, Rf_ScalarInteger(1));
    SET_VECTOR_ELT(cap, R_GE_capability_glyphs, Rf_ScalarInteger(1));
    UNPROTECT(2);
    return cap;
}

/* Typeset glyphs: indices into a named font file, each at its own place. */
static void gd_cb_glyph(int n, int *glyphs, double *x, double *y, SEXP font, double size,
                        int colour, double rot, pDevDesc dd)
{
    struct gd_dev *g = dd->deviceSpecific;
    if (R_TRANSPARENT(colour) || n < 1 || g->record) return;
    struct gd_font *f = gd_font_file(R_GE_glyphFontFile(font), R_GE_glyphFontIndex(font));
    if (!f->ok) return;
    double px = size * g->dpi / 72.0;
    double scale = stbtt_ScaleForMappingEmToPixels(&f->info, (float)px);
    double th = rot * GD_PI / 180, cs = cos(th), sn = sin(th);
    struct gd_path p;
    gd_path_init(&p);
    for (int i = 0; i < n; i++)
        gd_glyph_outline(&p, &f->info, glyphs[i], scale, x[i], y[i], 0, cs, sn);
    float rgba[4];
    gd_colour((unsigned)colour, rgba);
    gd_fill(&g->cv, &p, 1, rgba, &g->clip);
    g->dirty = 1;
    gd_path_free(&p);
}

/* ---- the R side ----------------------------------------------------------- */

/* rope_gd_format(): "kitty", "sixel" or NULL, for the R side. */
SEXP rope_gd_format(void)
{
    if (!gd_probed) rope_gd_init();
    if (gd_format == GD_KITTY) return Rf_mkString("kitty");
    if (gd_format == GD_SIXEL) return Rf_mkString("sixel");
    return R_NilValue;
}

static double gd_num(SEXP x, double dflt)
{
    if (TYPEOF(x) != REALSXP || XLENGTH(x) < 1 || !R_FINITE(REAL(x)[0])) return dflt;
    return REAL(x)[0];
}

/*
 * Default page size: most of the terminal's width and about half its
 * height, in pixels, from the cell size the probe found. Text is scaled the
 * same way: a terminal with tall cells is a high-resolution display, so the
 * device's dpi grows with the cell height.
 */
static void gd_defaults(double *w, double *h, double *dpi)
{
    int cols = 80, rows = 24;
    struct winsize ws;
    if (gd_tty_usable() && ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        cols = ws.ws_col;
        rows = ws.ws_row;
    }
    double tw = cols * gd_cell_w, th = rows * gd_cell_h;
    *w = floor(tw * 0.8);
    *h = floor(fmin(th * 0.6, *w * 0.75));
    if (*w < 200) *w = 200;
    if (*h < 150) *h = 150;
    double scale = gd_cell_h / 17.0;
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;
    *dpi = floor(96 * scale + 0.5);
}

/* rope_gd_open(width, height, pointsize, dpi, bg): open a page. */
SEXP rope_gd_open(SEXP width, SEXP height, SEXP pointsize, SEXP dpi, SEXP bg)
{
    if (!gd_probed) rope_gd_init();
    if (gd_format == GD_NONE)
        Rf_error("rope: this terminal cannot show images; see ROPE_GRAPHICS in the README");
    double dw, dh, ddpi;
    gd_defaults(&dw, &dh, &ddpi);
    int w = (int)gd_num(width, dw), h = (int)gd_num(height, dh);
    double ps = gd_num(pointsize, 12), res = gd_num(dpi, ddpi);
    if (w < 16 || h < 16 || w > 20000 || h > 20000) Rf_error("rope: page size %dx%d is out of range", w, h);
    if (res < 10 || res > 1000) Rf_error("rope: dpi %g is out of range", res);
    unsigned int bgcol = R_TRANWHITE;
    if (TYPEOF(bg) == STRSXP && XLENGTH(bg) >= 1) bgcol = R_GE_str2col(CHAR(STRING_ELT(bg, 0)));

    R_GE_checkVersionOrDie(R_GE_version);
    R_CheckDeviceAvailable();
    if (gd_ndevices == GD_MAX_DEVICES) Rf_error("rope: too many graphics devices open");

    struct gd_dev *g = calloc(1, sizeof *g);
    pDevDesc dd = calloc(1, sizeof *dd);
    if (!g || !dd || !gd_canvas_init(&g->cv, w, h)) {
        free(g);
        free(dd);
        Rf_error("rope: could not allocate a %dx%d page", w, h);
    }
    g->dpi = res;
    g->dd = dd;
    g->clip.x0 = 0; g->clip.y0 = 0; g->clip.x1 = (float)w; g->clip.y1 = (float)h;
    g->clip.stride = w;
    float clear[4] = { 1, 1, 1, 0 };
    gd_canvas_clear(&g->cv, clear);

    dd->deviceSpecific = g;
    dd->left = 0; dd->right = w; dd->bottom = h; dd->top = 0;
    dd->clipLeft = 0; dd->clipRight = w; dd->clipBottom = h; dd->clipTop = 0;
    dd->xCharOffset = 0.4900;
    dd->yCharOffset = 0.3333;
    dd->yLineBias = 0.2;
    dd->ipr[0] = dd->ipr[1] = 1.0 / res;
    dd->cra[0] = 0.9 * ps * res / 72.0;
    dd->cra[1] = 1.2 * ps * res / 72.0;
    dd->gamma = 1;
    dd->canClip = TRUE;
    dd->canChangeGamma = FALSE;
    dd->canHAdj = 2;
    dd->startps = ps;
    dd->startcol = R_RGB(0, 0, 0);
    dd->startfill = (int)bgcol;
    dd->startlty = LTY_SOLID;
    dd->startfont = 1;
    dd->startgamma = 1;
    dd->displayListOn = TRUE;
    dd->hasTextUTF8 = TRUE;
    dd->wantSymbolUTF8 = TRUE;
    dd->useRotatedTextInContour = TRUE;
    dd->haveTransparency = 2;
    dd->haveTransparentBg = 3;
    dd->haveRaster = 2;
    dd->haveCapture = 2;
    dd->haveLocator = 1;
    dd->deviceVersion = R_GE_version;

    dd->activate = gd_cb_activate;
    dd->deactivate = gd_cb_deactivate;
    dd->close = gd_cb_close;
    dd->circle = gd_cb_circle;
    dd->clip = gd_cb_clip;
    dd->line = gd_cb_line;
    dd->metricInfo = gd_cb_metric;
    dd->mode = gd_cb_mode;
    dd->newPage = gd_cb_newpage;
    dd->polygon = gd_cb_polygon;
    dd->polyline = gd_cb_polyline;
    dd->rect = gd_cb_rect;
    dd->path = gd_cb_path;
    dd->raster = gd_cb_raster;
    dd->cap = gd_cb_cap;
    dd->size = gd_cb_size;
    dd->strWidth = gd_cb_strwidth;
    dd->strWidthUTF8 = gd_cb_strwidth;
    dd->text = gd_cb_text;
    dd->textUTF8 = gd_cb_text;
    dd->holdflush = gd_cb_holdflush;
    dd->setPattern = gd_cb_setpattern;
    dd->releasePattern = gd_cb_releasepattern;
    dd->setClipPath = gd_cb_setclippath;
    dd->releaseClipPath = gd_cb_releaseclippath;
    dd->setMask = gd_cb_setmask;
    dd->releaseMask = gd_cb_releasemask;
    dd->defineGroup = gd_cb_definegroup;
    dd->useGroup = gd_cb_usegroup;
    dd->releaseGroup = gd_cb_releasegroup;
    dd->stroke = gd_cb_stroke;
    dd->fill = gd_cb_fill;
    dd->fillStroke = gd_cb_fillstroke;
    dd->capabilities = gd_cb_capabilities;
    dd->glyph = gd_cb_glyph;

    Rboolean suspended = R_interrupts_suspended;
    R_interrupts_suspended = TRUE;
    pGEDevDesc gdd = GEcreateDevDesc(dd);
    gd_devices[gd_ndevices++] = g;
    GEaddDevice2(gdd, "rope");
    R_interrupts_suspended = suspended;
    return R_NilValue;
}
