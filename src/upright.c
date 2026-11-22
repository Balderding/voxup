/* upright.c: a tilted sprite stood upright (see upright.h).

   The turn is a line-for-line port of torcado's cleanEdge shader (github.com/torcado194/cleanEdge), which carries this
   notice:

   MIT LICENSE
   Copyright (c) 2022 torcado

   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
   documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
   rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
   permit persons to whom the Software is furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
   Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
   WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
   COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
   OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

   The steps after the turn are ours:
     close   background touching a colour becomes black (an outline all round)
     notch   a black pixel biting into the body is filled with the colour beside it
     peel    black facing the background without touching a colour goes (the outline one pixel wide)
     corner  black in an inside corner of the outline becomes the colour (the outline steps diagonally; centred style)
   Every output pixel is one of the sprite's own colours or black. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "upright.h"

#pragma STDC FP_CONTRACT OFF    /* no fused multiply-add: the same roundings as the reference */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---------------------------------------------------------------- cleanEdge */
typedef struct { int w, h; double *px; double line, thr, hi[3]; } CE;
static const double CLEAR[4] = { 0, 0, 0, 0 };

static double norm(const double *v, int n) {                     /* rounded once, as a correctly rounded norm is */
    long double s = 0; int i;
    for (i = 0; i < n; i++) s += (long double)v[i] * v[i];
    return (double)sqrtl(s);
}
static double cd(const double *a, const double *b) { double d[4] = { a[0] - b[0], a[1] - b[1], a[2] - b[2], a[3] - b[3] }; return norm(d, 4); }
static const double *tex(const CE *e, double x, double y) {
    int ix = (int)floor(x), iy = (int)floor(y);
    return ix >= 0 && ix < e->w && iy >= 0 && iy < e->h ? e->px + ((size_t)iy * e->w + ix) * 4 : CLEAR;
}
static int sim(const CE *e, const double *a, const double *b) { return (a[3] == 0 && b[3] == 0) || cd(a, b) <= e->thr; }
static int sim3(const CE *e, const double *a, const double *b, const double *c) { return sim(e, a, b) && sim(e, b, c); }
static int sim4(const CE *e, const double *a, const double *b, const double *c, const double *d) { return sim(e, a, b) && sim(e, b, c) && sim(e, c, d); }
static int higher(const CE *e, const double *t, const double *o) {
    if (sim(e, t, o)) return 0;
    if (t[3] == o[3]) { double a[3] = { t[0] - e->hi[0], t[1] - e->hi[1], t[2] - e->hi[2] }, b[3] = { o[0] - e->hi[0], o[1] - e->hi[1], o[2] - e->hi[2] }; return norm(a, 3) < norm(b, 3); }
    return t[3] > o[3];
}
static double dtl(const double *pt, double p1x, double p1y, double p2x, double p2y, const double *d) {
    double lx = p2x - p1x, ly = p2y - p1y, px = ly, py = -lx, v[2] = { px, py }, n = norm(v, 2), tx = p1x - pt[0], ty = p1y - pt[1];
    return (px * d[0] + py * d[1] > 0.0 ? 1.0 : -1.0) * ((px * tx + py * ty) / n);
}
#define CX(x) (0.5 + (x) * pd[0])
#define CY(y) (0.5 + (y) * pd[1])
#define PT(x, y) CX(x), CY(y)
static const double *slice_dist(const CE *e, const double *pt0, const double *main, const double *pd,
                                const double *ub, const double *u, const double *uf, const double *uff,
                                const double *b, const double *c, const double *f, const double *ff,
                                const double *db, const double *d, const double *df, const double *dff,
                                const double *ddb, const double *dd, const double *ddf) {
    double lw = e->line < 0.45 ? 0.45 : e->line > 1.142 ? 1.142 : e->line, point[2], neg[2] = { -pd[0], -pd[1] }, dist, against, towards;
    int should, flip = 0;
    point[0] = main[0] * (pt0[0] - 0.5) + 0.5; point[1] = main[1] * (pt0[1] - 0.5) + 0.5;
    against = 4.0 * cd(f, d) + cd(uf, c) + cd(c, db) + cd(ff, df) + cd(df, dd);
    towards = 4.0 * cd(c, df) + cd(u, f) + cd(f, dff) + cd(b, d) + cd(d, ddf);
    should = (against < towards) || ((against < towards + 0.001) && !higher(e, c, f));
    if (sim4(e, f, d, b, u) && sim4(e, uf, df, db, ub) && !sim(e, c, f)) should = 0;
    if (!should) return NULL;
    if (sim3(e, f, d, db) && !sim3(e, f, d, b) && !sim(e, uf, db)) {                  /* lower shallow 2:1 slant */
        if (sim(e, c, df) && higher(e, c, f)) ;
        else { if (higher(e, c, f)) flip = 1; if (sim(e, u, f) && !sim(e, c, df) && !higher(e, c, u)) flip = 1; }
        dist = flip ? lw - dtl(point, PT(1.5, -1.0), PT(-0.5, 0.0), neg) : dtl(point, PT(1.5, 0.0), PT(-0.5, 1.0), pd);
        if (!flip && sim(e, c, uf) && !(sim3(e, c, uf, uff) && !sim3(e, c, uf, ff) && !sim(e, d, uff))) {
            double t = dtl(point, PT(2.0, -1.0), PT(-0.0, 1.0), pd); if (t < dist) dist = t;
        }
        dist -= lw / 2.0;
        return dist <= 0.0 ? (cd(c, f) <= cd(c, d) ? f : d) : NULL;
    } else if (sim3(e, uf, f, d) && !sim3(e, u, f, d) && !sim(e, uf, db)) {          /* forward steep 2:1 slant */
        if (sim(e, c, df) && higher(e, c, d)) ;
        else { if (higher(e, c, d)) flip = 1; if (sim(e, b, d) && !sim(e, c, df) && !higher(e, c, d)) flip = 1; }
        dist = flip ? lw - dtl(point, PT(0.0, -0.5), PT(-1.0, 1.5), neg) : dtl(point, PT(1.0, -0.5), PT(0.0, 1.5), pd);
        if (!flip && sim(e, c, db) && !(sim3(e, c, db, ddb) && !sim3(e, c, db, dd) && !sim(e, f, ddb))) {
            double t = dtl(point, PT(1.0, 0.0), PT(-1.0, 2.0), pd); if (t < dist) dist = t;
        }
        dist -= lw / 2.0;
        return dist <= 0.0 ? (cd(c, f) <= cd(c, d) ? f : d) : NULL;
    } else if (sim(e, f, d)) {                                                          /* 45 diagonal */
        if (sim(e, c, df) && higher(e, c, f)) { if (!sim(e, c, dd) && !sim(e, c, ff)) flip = 1; }
        else { if (higher(e, c, f)) flip = 1; if (!sim(e, c, b) && sim4(e, b, f, d, u)) flip = 1; }
        if (((sim(e, f, db) && sim3(e, u, f, df)) || (sim(e, uf, d) && sim3(e, b, d, df))) && !sim(e, c, df)) flip = 1;
        dist = flip ? lw - dtl(point, PT(1.0, -1.0), PT(-1.0, 1.0), neg) : dtl(point, PT(1.0, 0.0), PT(0.0, 1.0), pd);
        if (!flip && sim3(e, c, uf, uff) && !sim3(e, c, uf, ff) && !sim(e, d, uff)) { double t = dtl(point, PT(1.5, 0.0), PT(-0.5, 1.0), pd); if (t > dist) dist = t; }
        if (!flip && sim3(e, ddb, db, c) && !sim3(e, dd, db, c) && !sim(e, ddb, f)) { double t = dtl(point, PT(1.0, -0.5), PT(0.0, 1.5), pd); if (t > dist) dist = t; }
        dist -= lw / 2.0;
        return dist <= 0.0 ? (cd(c, f) <= cd(c, d) ? f : d) : NULL;
    } else if (sim3(e, ff, df, d) && !sim3(e, ff, df, c) && !sim(e, uff, d)) {       /* far corner of shallow slant */
        if (sim(e, f, dff) && higher(e, f, ff)) ;
        else { if (higher(e, f, ff)) flip = 1; if (sim(e, uf, ff) && !sim(e, f, dff) && !higher(e, f, uf)) flip = 1; }
        dist = flip ? lw - dtl(point, PT(2.5, -1.0), PT(0.5, 0.0), neg) : dtl(point, PT(2.5, 0.0), PT(0.5, 1.0), pd);
        dist -= lw / 2.0;
        return dist <= 0.0 ? (cd(f, ff) <= cd(f, df) ? ff : df) : NULL;
    } else if (sim3(e, f, df, dd) && !sim3(e, c, df, dd) && !sim(e, f, ddb)) {       /* far corner of steep slant */
        if (sim(e, d, ddf) && higher(e, d, dd)) ;
        else { if (higher(e, d, dd)) flip = 1; if (sim(e, db, dd) && !sim(e, d, ddf) && !higher(e, d, dd)) flip = 1; }
        dist = flip ? lw - dtl(point, PT(0.0, 0.5), PT(-1.0, 2.5), neg) : dtl(point, PT(1.0, 0.5), PT(0.0, 2.5), pd);
        dist -= lw / 2.0;
        return dist <= 0.0 ? (cd(d, df) <= cd(d, dd) ? df : dd) : NULL;
    }
    return NULL;
}
static const double *sample(const CE *e, double sx, double sy) {
    double local[2] = { sx - floor(sx), sy - floor(sy) }, cx = floor(sx) + 0.5, cy = floor(sy) + 0.5;
    double pd[2] = { local[0] >= 0.5 ? 1.0 : -1.0, local[1] >= 0.5 ? 1.0 : -1.0 };
    static const double m0[2] = { 1.0, 1.0 }, m1[2] = { -1.0, 1.0 }, m2[2] = { 1.0, -1.0 };
    const double *col, *r;
#define T(x, y) tex(e, cx + (x) * pd[0], cy + (y) * pd[1])
    const double *uub = T(-1, -2), *uu = T(0, -2), *uuf = T(1, -2);
    const double *ubb = T(-2, -1), *ub = T(-1, -1), *u = T(0, -1), *uf = T(1, -1), *uff = T(2, -1);
    const double *bb = T(-2, 0), *b = T(-1, 0), *c = T(0, 0), *f = T(1, 0), *ff = T(2, 0);
    const double *dbb = T(-2, 1), *db = T(-1, 1), *d = T(0, 1), *df = T(1, 1), *dff = T(2, 1);
    const double *ddb = T(-1, 2), *dd = T(0, 2), *ddf = T(1, 2);
#undef T
    col = c;
    if ((r = slice_dist(e, local, m0, pd, ub, u, uf, uff, b, c, f, ff, db, d, df, dff, ddb, dd, ddf))) col = r;
    if ((r = slice_dist(e, local, m1, pd, uf, u, ub, ubb, f, c, b, bb, df, d, db, dbb, ddf, dd, ddb))) col = r;
    if ((r = slice_dist(e, local, m2, pd, db, d, df, dff, b, c, f, ff, ub, u, uf, uff, uub, uu, uuf))) col = r;
    return col;
}
static void ce_init(CE *e, const unsigned char *rgba, int w, int h) {
    int i, k;
    e->w = w; e->h = h; e->line = 1.0; e->thr = 0.0; e->hi[0] = e->hi[1] = e->hi[2] = 1.0;
    e->px = malloc((size_t)w * h * 4 * sizeof(double));
    for (i = 0; i < w * h; i++) for (k = 0; k < 4; k++) e->px[i * 4 + k] = rgba[i * 4 + 3] == 0 ? 0.0 : rgba[i * 4 + k] / 255.0;
}
/* the sprite turned clockwise by `angle` degrees at `scale` output pixels per sprite pixel; malloc'd RGBA M x N */
static unsigned char *ce_render(const CE *e, double angle, double scale, double shx, double shy, int *pm, int *pn) {
    double th = angle * (M_PI / 180.0), co = cos(th), si = sin(th);
    int M = (int)ceil((fabs(e->w * co) + fabs(e->h * si)) * scale) + 2, N = (int)ceil((fabs(e->w * si) + fabs(e->h * co)) * scale) + 2, x, y, k;
    unsigned char *out = calloc((size_t)M * N, 4);
    for (y = 0; y < N; y++) for (x = 0; x < M; x++) {
        double u = (x + 0.5 - M / 2.0) / scale + shx, v = (y + 0.5 - N / 2.0) / scale + shy;
        const double *col = sample(e, co * u + si * v + e->w / 2.0, -si * u + co * v + e->h / 2.0);
        if (col[3] > 0) for (k = 0; k < 4; k++) out[((size_t)y * M + x) * 4 + k] = (unsigned char)nearbyint(col[k] * 255);
    }
    *pm = M; *pn = N;
    return out;
}

/* ---------------------------------------------------------------- the steps on the turned picture */
typedef struct { int w, h; unsigned char *p; } Img;
#define PX(a, x, y) ((a)->p + ((size_t)(y) * (a)->w + (x)) * 4)
#define OP(a, x, y) (PX(a, x, y)[3] > 0)
#define BLK(a, x, y) (OP(a, x, y) && PX(a, x, y)[0] + PX(a, x, y)[1] + PX(a, x, y)[2] == 0)
#define COL(a, x, y) (OP(a, x, y) && !BLK(a, x, y))
enum { M_OP, M_BLK, M_COL, M_BG };
static unsigned char *mask(const Img *a, int which) {
    unsigned char *m = malloc((size_t)a->w * a->h); int x, y;
    for (y = 0; y < a->h; y++) for (x = 0; x < a->w; x++)
        m[y * a->w + x] = which == M_OP ? OP(a, x, y) : which == M_BLK ? BLK(a, x, y) : which == M_COL ? COL(a, x, y) : !OP(a, x, y);
    return m;
}
/* any 4-neighbour (or 8) set in m; outside the picture counts as not set */
static int near(const unsigned char *m, int w, int h, int x, int y, int diag) {
    static const int d4[8][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }, { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } }; int i;
    for (i = 0; i < (diag ? 8 : 4); i++) { int X = x + d4[i][0], Y = y + d4[i][1]; if (X >= 0 && X < w && Y >= 0 && Y < h && m[Y * w + X]) return 1; }
    return 0;
}
static void step_close(Img *a) {
    unsigned char *op = mask(a, M_OP), *col = mask(a, M_COL); int x, y;
    for (y = 0; y < a->h; y++) for (x = 0; x < a->w; x++)
        if (!op[y * a->w + x] && near(col, a->w, a->h, x, y, 0)) { unsigned char *q = PX(a, x, y); q[0] = q[1] = q[2] = 0; q[3] = 255; }
    free(op); free(col);
}
static void step_notch(Img *a) {
    unsigned char *blk = mask(a, M_BLK), *col = mask(a, M_COL), *src = malloc((size_t)a->w * a->h * 4); int x, y, W = a->w;
    memcpy(src, a->p, (size_t)a->w * a->h * 4);
    for (y = 1; y < a->h - 1; y++) for (x = 1; x < W - 1; x++)
        if (blk[y * W + x] && col[(y - 1) * W + x] && col[(y + 1) * W + x] && col[y * W + x - 1] != col[y * W + x + 1] && (blk[y * W + x - 1] || blk[y * W + x + 1]))
            memcpy(PX(a, x, y), src + ((size_t)y * W + (col[y * W + x + 1] ? x + 1 : x - 1)) * 4, 4);
    free(blk); free(col); free(src);
}
static void step_peel(Img *a) {
    for (;;) {
        unsigned char *blk = mask(a, M_BLK), *col = mask(a, M_COL), *bg = mask(a, M_BG), *kill = calloc((size_t)a->w * a->h, 1); int x, y, any = 0;
        for (y = 0; y < a->h; y++) for (x = 0; x < a->w; x++)
            if (blk[y * a->w + x] && !near(col, a->w, a->h, x, y, 0) && near(bg, a->w, a->h, x, y, 0)) kill[y * a->w + x] = 1, any = 1;
        for (y = 0; y < a->h; y++) for (x = 0; x < a->w; x++) if (kill[y * a->w + x]) memset(PX(a, x, y), 0, 4);
        free(blk); free(col); free(bg); free(kill);
        if (!any) return;
    }
}
static void step_corner(Img *a) {
    static const int dg[4][2] = { { -1, -1 }, { -1, 1 }, { 1, -1 }, { 1, 1 } };     /* (dy, dx) */
    unsigned char *op = mask(a, M_OP), *blk = mask(a, M_BLK), *col = mask(a, M_COL), *src = malloc((size_t)a->w * a->h * 4); int x, y, i, W = a->w;
    memcpy(src, a->p, (size_t)a->w * a->h * 4);
    for (y = 1; y < a->h - 1; y++) for (x = 1; x < W - 1; x++) {
        if (!blk[y * W + x] || !op[y * W + x + 1] || !op[y * W + x - 1] || !op[(y + 1) * W + x] || !op[(y - 1) * W + x]) continue;
        for (i = 0; i < 4; i++) {
            int dy = dg[i][0], dx = dg[i][1];
            if (blk[(y + dy) * W + x] && blk[y * W + x + dx] && !op[(y + dy) * W + x + dx] && col[(y - dy) * W + x] && col[y * W + x - dx]) {
                memcpy(PX(a, x, y), src + ((size_t)y * W + x - dx) * 4, 4); break;
            }
        }
    }
    free(op); free(blk); free(col); free(src);
}
/* cropped to the opaque pixels (a new picture; an empty one stays as it is) */
static Img crop(Img a) {
    int x, y, x0 = a.w, y0 = a.h, x1 = -1, y1 = -1; Img o;
    for (y = 0; y < a.h; y++) for (x = 0; x < a.w; x++) if (OP(&a, x, y)) { if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
    if (x1 < 0) return a;
    o.w = x1 - x0 + 1; o.h = y1 - y0 + 1; o.p = malloc((size_t)o.w * o.h * 4);
    for (y = 0; y < o.h; y++) memcpy(o.p + (size_t)y * o.w * 4, PX(&a, x0, y0 + y), (size_t)o.w * 4);
    free(a.p);
    return o;
}

/* the grip (swords: the rows below the widest row of the lower half, the crossguard) or the shaft (the whole sprite)
   one pixel slimmer each side when it is 4+ wide, the right side only when it is 3 wide */
static void slim(Img *a, int sword) {
    int W = a->w, H = a->h, y, x, r0 = -1, r1 = -1, guard = -1, gw = -1, *l = calloc(H, sizeof(int)), *r = calloc(H, sizeof(int)), g0 = 0, g1 = 0, best = 0, both;
    unsigned char *op = mask(a, M_OP), *blk = mask(a, M_BLK), *col = mask(a, M_COL);
    for (y = 0; y < H; y++) { l[y] = -1; for (x = 0; x < W; x++) if (op[y * W + x]) { if (l[y] < 0) l[y] = x; r[y] = x; } if (l[y] >= 0) { if (r0 < 0) r0 = y; r1 = y; } }
    if (r0 < 0) goto done;
    if (sword) {
        for (y = r0; y <= r1; y++) if (l[y] >= 0 && y > r0 + (r1 - r0) * 0.5 && r[y] - l[y] >= gw) { gw = r[y] - l[y]; guard = y; }
        if (guard < 0) goto done;
    } else guard = r0 - 1;
    /* the most common row span among the candidate rows (the first one seen wins a tie) */
    for (y = guard + 1; y <= r1; y++) if (l[y] >= 0) {
        int n = 0, k, seen = 0;
        for (k = guard + 1; k < y; k++) if (l[k] >= 0 && l[k] == l[y] && r[k] == r[y]) { seen = 1; break; }
        if (seen) continue;
        for (k = guard + 1; k <= r1; k++) if (l[k] >= 0 && l[k] == l[y] && r[k] == r[y]) n++;
        if (n > best) { best = n; g0 = l[y]; g1 = r[y]; }
    }
    if (!best || g1 - g0 - 1 < 3) goto done;
    both = g1 - g0 - 1 >= 4;
    for (y = guard + 1; y <= r1; y++) if (l[y] >= 0) {
        int alone = l[y] == g0 && r[y] == g1, beside = (l[y] < g0 || r[y] > g1) && blk[y * W + g0] && blk[y * W + g1], k;
        unsigned char *q;
        if (beside) for (k = g0 + 1; k < g1; k++) if (!col[y * W + k]) { beside = 0; break; }
        if (alone || beside) {
            if (both) {
                q = PX(a, g0, y); if (alone || g0 < 1 || !op[y * W + g0 - 1]) memset(q, 0, 4); else { q[0] = q[1] = q[2] = 0; q[3] = 255; }
                q = PX(a, g0 + 1, y); q[0] = q[1] = q[2] = 0; q[3] = 255;
            }
            q = PX(a, g1, y); if (alone || g1 + 1 >= W || !op[y * W + g1 + 1]) memset(q, 0, 4); else { q[0] = q[1] = q[2] = 0; q[3] = 255; }
            q = PX(a, g1 - 1, y); q[0] = q[1] = q[2] = 0; q[3] = 255;
        } else if (sword && r[y] - l[y] > g1 - g0) {
            int xs[2] = { both ? g0 + 1 : g1 - 1, g1 - 1 }, n = both ? 2 : 1;
            for (k = 0; k < n; k++) if (col[y * W + xs[k]]) { q = PX(a, xs[k], y); q[0] = q[1] = q[2] = 0; q[3] = 255; }
        }
    }
    step_close(a); step_peel(a);
done:
    free(l); free(r); free(op); free(blk); free(col);
}

/* ---------------------------------------------------------------- measuring and the whole step */
void upright_defaults(UprightOpt *o) { memset(o, 0, sizeof *o); o->auto_turn = 1; o->outline = -1; }

/* the long axis of the opaque pixels: the covariance's larger eigenvector, pointed at its upper end */
static int axis(const unsigned char *rgba, int w, int h, double *vx, double *vy, double *elong) {
    double sx = 0, sy = 0, a = 0, b = 0, d = 0, mx, my, l1, l0, q; int x, y, n = 0;
    for (y = 0; y < h; y++) for (x = 0; x < w; x++) if (rgba[(y * w + x) * 4 + 3] > 0) { sx += x; sy += y; n++; }
    if (n < 2) return n;
    mx = sx / n; my = sy / n;
    for (y = 0; y < h; y++) for (x = 0; x < w; x++) if (rgba[(y * w + x) * 4 + 3] > 0) { a += (x - mx) * (x - mx); b += (x - mx) * (y - my); d += (y - my) * (y - my); }
    a /= n - 1; b /= n - 1; d /= n - 1;
    q = sqrt((a - d) * (a - d) / 4 + b * b); l1 = (a + d) / 2 + q; l0 = (a + d) / 2 - q;
    if (b != 0) { *vx = b; *vy = l1 - a; } else if (a >= d) { *vx = 1; *vy = 0; } else { *vx = 0; *vy = 1; }
    if (*vy > 0) { *vx = -*vx; *vy = -*vy; }
    *elong = sqrt(l1 / (l0 > 1e-6 ? l0 : 1e-6));
    return n;
}
int upright_measure(const unsigned char *rgba, int w, int h, double *tilt, double *elong, int *tilted) {
    const double DEG = 180.0 / M_PI, SNAP[3] = { 45.0, atan(0.5) * DEG, atan(2.0) * DEG };
    double vx = 0, vy = 0, el = 0, t, lev; int n = axis(rgba, w, h, &vx, &vy, &el), i;
    if (n < 2) { *tilt = 0; *elong = 0; *tilted = 0; return 0; }
    t = atan2(vx, -vy) * DEG;
    lev = fabs(atan2(vy, vx) * DEG); if (lev > 90) lev = 180 - lev;            /* 0 level, 90 upright */
    *elong = el; *tilted = n >= 6 && el >= 1.6 && lev >= 20 && lev <= 70;
    for (i = 0; i < 3; i++) if (fabs(fabs(t) - SNAP[i]) <= 8) { t = t < 0 ? -SNAP[i] : SNAP[i]; break; }
    *tilt = t;
    return 1;
}
static int outlined(const unsigned char *rgba, int w, int h) {
    Img a = { w, h, (unsigned char *)rgba }; unsigned char *blk = mask(&a, M_BLK), *col = mask(&a, M_COL), *bg = mask(&a, M_BG);
    int x, y, nb = 0, nc = 0, exp = 0, fen = 0, r;
    for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
        if (col[y * w + x]) { nc++; if (near(bg, w, h, x, y, 0)) exp++; }
        if (blk[y * w + x]) { nb++; if (near(col, w, h, x, y, 1)) fen++; }
    }
    r = nb > 0 && (double)exp / (nc > 1 ? nc : 1) < 0.15 && (double)fen / (nb > 1 ? nb : 1) > 0.9;
    free(blk); free(col); free(bg);
    return r;
}
static int top_row(const Img *o) { int x, n = 0; for (x = 0; x < o->w; x++) n += OP(o, x, 0); return n; }

int upright_make(const unsigned char *rgba, int w, int h, const UprightOpt *o, unsigned char *out32, double *turn_used, double *scale_used) {
    CE e; double turn, scale = 1.5, el; int line = o->outline < 0 ? outlined(rgba, w, h) : o->outline, x, y, tl;
    Img best = { 0, 0, NULL };
    if (o->auto_turn) { double t; if (!upright_measure(rgba, w, h, &t, &el, &tl)) return 0; turn = -t; } else turn = o->turn;
    ce_init(&e, rgba, w, h);
    for (;;) {
        static const double VS[4] = { 0.0, 0.25, 0.5, 0.75 }; int k, nv = o->centred ? 4 : 1, bt = 1 << 30;
        for (k = 0; k < nv; k++) {
            Img a;
            a.p = ce_render(&e, turn, scale, o->centred ? 0.5 / scale : 0.0, VS[k] / scale, &a.w, &a.h);
            if (line) { step_close(&a); step_notch(&a); step_peel(&a); if (o->centred) { step_corner(&a); step_peel(&a); } }
            a = crop(a);
            if (o->centred && (a.w > 32 || a.h > 32) && scale > 0.8) { free(a.p); continue; }
            if (!best.p || (o->centred && top_row(&a) < bt)) { free(best.p); best = a; bt = top_row(&a); } else free(a.p);
        }
        if ((best.p && best.w <= 32 && best.h <= 32) || scale <= 0.8) break;
        free(best.p); best.p = NULL;
        scale = nearbyint((scale - 0.05) * 100) / 100;
    }
    free(e.px);
    if (!best.p) return 0;
    if (o->slim) slim(&best, o->slim == UP_SLIM_SWORD);
    memset(out32, 0, 32 * 32 * 4);
    {   int cw = best.w < 32 ? best.w : 32, ch = best.h < 32 ? best.h : 32, ox = (32 - cw) / 2, oy = (32 - ch) / 2;
        for (y = 0; y < ch; y++) for (x = 0; x < cw; x++) memcpy(out32 + ((y + oy) * 32 + x + ox) * 4, PX(&best, x, y), 4); }
    free(best.p);
    if (turn_used) *turn_used = turn;
    if (scale_used) *scale_used = scale;
    return 1;
}
unsigned char *upright_big(const unsigned char *rgba, int w, int h, double turn, int size, int *ow, int *oh) {
    CE e; Img a; double scale;
    ce_init(&e, rgba, w, h);
    a.p = ce_render(&e, turn, 1.0, 0, 0, &a.w, &a.h); a = crop(a);
    scale = (double)size / (a.w > a.h ? a.w : a.h); free(a.p);
    a.p = ce_render(&e, turn, scale, 0, 0, &a.w, &a.h); a = crop(a);
    free(e.px);
    *ow = a.w; *oh = a.h;
    return a.p;
}
