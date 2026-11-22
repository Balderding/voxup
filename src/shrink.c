/* shrink.c: a model's drawing made into the sprite, every stage with its own settings (see shrink.h). With the
   defaults it is pick.c's to32 pixel for pixel; `voxup --shrinkcheck DIR` proves it.

   Sources for the methods:
   - Lanczos / box / bilinear / bicubic (a = -0.5): Pillow's Resample.c, the same weights (premultiplied, mode "F").
   - Mitchell-Netravali (B = C = 1/3): Mitchell & Netravali, "Reconstruction filters in computer graphics", 1988.
   - Gaussian: exp(-2 x^2) (sigma half an output pixel), support 2.
   - Median and majority (the commonest colour in each sprite pixel's area): what pixel-art fixers such as unfake.py and
     SpriteGrid do (github.com/painebenjamin/unfake.py, github.com/marksverdhei/spritegrid).
   - DPID: Weber, Waechter, Amend, Guthe, Goesele, "Rapid, Detail-Preserving Image Downscaling", SIGGRAPH Asia 2016:
     a pixel's weight grows with how far it is from the smoothed small picture, (|I - G| / Vmax) ^ lambda.
   - SAID: Arjun, Mitra, Ghosh, "Structure Aware Image Downscaling", arXiv 2510.22551 (Oct 2025): edge map (Sobel),
     edge-guided blend of a bicubic and a sharpened-then-bicubic picture, plus a texture term weighted alpha * E + beta.
     The paper gives no values and mixes full and small pictures in its texture step; ours is the Laplacian of the small
     picture, subtracted (it sharpens), our reading.
   - Superpixels: Gerstner, Decarlo, Alexa, Finkelstein, Gross, Aberman, "Pixelated Image Abstraction", NPAR 2012:
     SLIC superpixels seeded one per sprite pixel, Lab colour + distance (compactness m), positions smoothed towards
     their neighbours between passes; each sprite pixel takes its superpixel's mean colour. Our version keeps the
     palette step separate (stage 6) instead of their annealing.
   - Not built: Kopf, Shamir, Peers, "Content-Adaptive Image Downscaling", 2013 (its own paper calls it very costly);
     Oztireli & Gross, "Perceptually Based Downscaling of Images", 2015 (an SSIM optimisation).
   - Dithering: Floyd-Steinberg 1976, Atkinson (Apple), Bayer 4 x 4 ordered.
   - Selective outline ("sel-out"): pixel-art practice (Derek Yu's tutorial): the outline a darker shade of the colour
     it borders. Corner clean-up: doubled L-shaped corners in a line removed (Aseprite outline tools). */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include "shrink.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>     /* one lock for what the shrinks share (review 3, P3-3) */
#include "vlog.h"

const char *const METHOD_NAMES[M_COUNT] = { "Lanczos", "Nearest", "Box (area)", "Bilinear", "Bicubic", "Mitchell", "Gaussian",
                                            "Median", "Majority", "DPID (2016)", "SAID (2025)", "Superpixels (2012)",
                                            "2-step (4x, 2x, 1x majority)", "Grid snap (the model's own pixels)" };

void shrink_defaults(ShrinkOpt *o) {
    memset(o, 0, sizeof *o);
    o->size = SZ_32; o->cw = 32; o->ch = 32; o->fit = FIT_BOX; o->margin = 1;
    o->bg_dist = 90; o->magenta = 1; o->alpha_cut = 64;
    o->method = M_LANCZOS; o->dpid_lambda = 100; o->said_gamma = 150; o->said_alpha = 30; o->said_beta = 10; o->sp_m = 45; o->sp_iter = 10;
    o->pinholes = 2; o->colours = C_ORIGINAL; o->ncol = 8; o->dist = D_RGB; o->dither = DI_NONE; o->dither_amt = 50;
    o->black = 48; o->pink = 20; o->peel = 4; o->outline = O_BLACK4; o->othick = 1; o->edge = 0;
}
void shrink_size(const ShrinkOpt *o, int ow, int oh, int *W, int *H) {
    if (o->size == SZ_16) { *W = ow; *H = oh; }
    else if (o->size == SZ_32) { *W = ow * 2; *H = oh * 2; }
    else { *W = o->cw < 1 ? 1 : o->cw > 128 ? 128 : o->cw; *H = o->ch < 1 ? 1 : o->ch > 128 ? 128 : o->ch; }
}

/* ---------------------------------------------------------------- settings by name */
typedef struct { const char *key; int off, lo, hi; } Key;
#define K(f, lo, hi) { #f, (int)offsetof(ShrinkOpt, f), lo, hi }
#include <stddef.h>
static const Key KEYS[] = {
    K(size, 0, 2), K(cw, 1, 128), K(ch, 1, 128), K(fit, 0, 2), K(margin, 0, 8),
    K(bg_dist, 10, 250), K(magenta, 0, 1), K(alpha_cut, 1, 254),
    K(contrast, -100, 100), K(brightness, -100, 100), K(saturation, -100, 100), K(smooth, 0, 2), K(sharpen, 0, 300),
    K(method, 0, M_COUNT - 1), K(dpid_lambda, 0, 300), K(said_gamma, 0, 400), K(said_alpha, 0, 200), K(said_beta, 0, 200),
    K(sp_m, 1, 100), K(sp_iter, 1, 30),
    K(pinholes, 0, 3), K(colours, 0, 7), K(ncol, 2, 32), K(dist, 0, 2), K(dither, 0, 3), K(dither_amt, 0, 100),
    K(black, 0, 128), K(pink, 0, 100), K(peel, 0, 4), K(orphans, 0, 1), K(jaggies, 0, 1), K(edgestray, 0, 1), K(kmpp, 0, 1), K(palid, 0, 5), K(outline, 0, 4), K(othick, 1, 4), K(edge, 0, 1),
    K(flood, 0, 1), K(linear, 0, 1), K(silhouette, 0, 1), K(regions, 0, 1), K(notch, 0, 1), K(blocks, 0, 1),
};
const char *const SHRINK_KEYS[] = { "size", "cw", "ch", "fit", "margin", "bg_dist", "magenta", "alpha_cut", "contrast", "brightness",
    "saturation", "smooth", "sharpen", "method", "dpid_lambda", "said_gamma", "said_alpha", "said_beta", "sp_m", "sp_iter",
    "pinholes", "colours", "ncol", "dist", "dither", "dither_amt", "black", "pink", "peel", "orphans", "jaggies", "edgestray", "kmpp", "palid", "outline", "othick", "edge",
    "flood", "linear", "silhouette", "regions", "notch", "blocks", NULL };
int shrink_set(ShrinkOpt *o, const char *key, int v) {
    size_t i;
    for (i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++) if (!strcmp(KEYS[i].key, key)) {
        if (v < KEYS[i].lo) v = KEYS[i].lo; if (v > KEYS[i].hi) v = KEYS[i].hi;
        *(int *)((char *)o + KEYS[i].off) = v; return 1;
    }
    return 0;
}
int shrink_get(const ShrinkOpt *o, const char *key, int *v) {
    size_t i;
    for (i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++) if (!strcmp(KEYS[i].key, key)) { *v = *(const int *)((const char *)o + KEYS[i].off); return 1; }
    return 0;
}
const char *shrink_describe(const ShrinkOpt *o, char *buf, int n) {
    ShrinkOpt d; size_t i; int k = 0;
    shrink_defaults(&d); buf[0] = 0;
    for (i = 0; i < sizeof KEYS / sizeof KEYS[0] && k < n - 40; i++) {
        int a = *(const int *)((const char *)o + KEYS[i].off), b = *(const int *)((const char *)&d + KEYS[i].off);
        if (a != b) k += snprintf(buf + k, (size_t)(n - k), "%s%s=%d", k ? " " : "", KEYS[i].key, a);
    }
    if (!k) snprintf(buf, (size_t)n, "standard");
    return buf;
}

/* ---------------------------------------------------------------- resampling: Pillow's weights for any kernel */
static void *xm(size_t n) { void *p = malloc(n ? n : 1); if (!p) { vlog(VLOG_ERR, "shrink", "out of memory (%zu bytes)", n); vlog_close(); exit(3); } return p; }
static double sincf(double x) { if (x == 0.0) return 1.0; x *= 3.14159265358979323846; return sin(x) / x; }
static double k_lanczos(double x) { if (-3.0 <= x && x < 3.0) return sincf(x) * sincf(x / 3); return 0.0; }
static double k_box(double x) { return x > -0.5 && x <= 0.5 ? 1.0 : 0.0; }
static double k_tri(double x) { if (x < 0.0) x = -x; return x < 1.0 ? 1.0 - x : 0.0; }
static double k_cubic(double x) { const double a = -0.5; if (x < 0.0) x = -x; if (x < 1.0) return ((a + 2.0) * x - (a + 3.0)) * x * x + 1; if (x < 2.0) return (((x - 5) * x + 8) * x - 4) * a; return 0.0; }
static double k_mitchell(double x) {
    const double B = 1.0 / 3, C = 1.0 / 3; if (x < 0) x = -x;
    if (x < 1) return ((12 - 9 * B - 6 * C) * x * x * x + (-18 + 12 * B + 6 * C) * x * x + (6 - 2 * B)) / 6;
    if (x < 2) return ((-B - 6 * C) * x * x * x + (6 * B + 30 * C) * x * x + (-12 * B - 48 * C) * x + (8 * B + 24 * C)) / 6;
    return 0;
}
static double k_gauss(double x) { return x > -2.0 && x < 2.0 ? exp(-2.0 * x * x) : 0.0; }
typedef double (*KernFn)(double);
static int coeffs(int insz, int outsz, KernFn f, double sup, int **bounds, double **kk) {
    double scale = (double)insz / outsz, fs = scale < 1.0 ? 1.0 : scale, support = sup * fs, ss = 1.0 / fs;
    int ks = (int)ceil(support) * 2 + 1, xx, x;
    *kk = xm(sizeof(double) * (size_t)outsz * (size_t)ks); *bounds = xm(sizeof(int) * 2 * (size_t)outsz);
    for (xx = 0; xx < outsz; xx++) {
        double center = (xx + 0.5) * scale, ww = 0.0, *k = *kk + (size_t)xx * ks;
        int xmin = (int)(center - support + 0.5), xmax;
        if (xmin < 0) xmin = 0;
        xmax = (int)(center + support + 0.5);
        if (xmax > insz) xmax = insz;
        xmax -= xmin;
        for (x = 0; x < xmax; x++) { double w = f((x + xmin - center + 0.5) * ss); k[x] = w; ww += w; }
        for (x = 0; x < xmax; x++) if (ww != 0.0) k[x] /= ww;
        for (; x < ks; x++) k[x] = 0;
        (*bounds)[xx * 2] = xmin; (*bounds)[xx * 2 + 1] = xmax;
    }
    return ks;
}
static float *resize_k(const float *in, int iw, int ih, int ow, int oh, KernFn f, double sup) {
    float *t = NULL, *o; const float *src = in; int *b, ks, x, y, i; double *kk;
    if (ow != iw) {
        ks = coeffs(iw, ow, f, sup, &b, &kk); t = xm(sizeof(float) * (size_t)ow * ih);
        for (y = 0; y < ih; y++) for (x = 0; x < ow; x++) {
            double ss = 0.0; const double *k = kk + (size_t)x * ks;
            for (i = 0; i < b[x * 2 + 1]; i++) ss += in[(size_t)y * iw + b[x * 2] + i] * k[i];
            t[(size_t)y * ow + x] = (float)ss;
        }
        free(b); free(kk); src = t;
    }
    o = xm(sizeof(float) * (size_t)ow * oh);
    if (oh != ih) {
        ks = coeffs(ih, oh, f, sup, &b, &kk);
        for (y = 0; y < oh; y++) for (x = 0; x < ow; x++) {
            double ss = 0.0; const double *k = kk + (size_t)y * ks;
            for (i = 0; i < b[y * 2 + 1]; i++) ss += src[(size_t)(b[y * 2] + i) * ow + x] * k[i];
            o[(size_t)y * ow + x] = (float)ss;
        }
        free(b); free(kk);
    } else memcpy(o, src, sizeof(float) * (size_t)ow * oh);
    free(t);
    return o;
}

/* ---------------------------------------------------------------- colour helpers */
static double lin_t[256];
static void lin_init(void) { static int done; int i; if (done) return; for (i = 0; i < 256; i++) { double c = i / 255.0; lin_t[i] = c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); } done = 1; }
static double labf(double t) { return t > 0.008856 ? cbrt(t) : 7.787 * t + 16.0 / 116; }
static void lab(double r, double g, double b, double *L) {      /* 0..255 RGB (any real) -> CIE Lab (D65) */
    double R, G, B, X, Y, Z;
    #define LIN(v) ((v) <= 0 ? 0 : (v) >= 255 ? 1.0 : ((v) / 255.0 <= 0.04045 ? (v) / 255.0 / 12.92 : pow(((v) / 255.0 + 0.055) / 1.055, 2.4)))
    R = LIN(r); G = LIN(g); B = LIN(b);
    #undef LIN
    X = (0.4124 * R + 0.3576 * G + 0.1805 * B) / 0.95047; Y = 0.2126 * R + 0.7152 * G + 0.0722 * B; Z = (0.0193 * R + 0.1192 * G + 0.9505 * B) / 1.08883;
    L[0] = 116 * labf(Y) - 16; L[1] = 500 * (labf(X) - labf(Y)); L[2] = 200 * (labf(Y) - labf(Z));
}
static double cdist(int mode, const double *a, const double *b, const double *la, const double *lb) {
    double dr = a[0] - b[0], dg = a[1] - b[1], db = a[2] - b[2];
    if (mode == D_LAB) { double x = la[0] - lb[0], y = la[1] - lb[1], z = la[2] - lb[2]; return x * x + y * y + z * z; }
    if (mode == D_REDMEAN) { double rm = (a[0] + b[0]) / 2; return (2 + rm / 256) * dr * dr + 4 * dg * dg + (2 + (255 - rm) / 256) * db * db; }
    return dr * dr + dg * dg + db * db;
}
static int cmp_u8(const void *a, const void *b) { return *(const unsigned char *)a - *(const unsigned char *)b; }
static int cmp_rgb(const void *a, const void *b) { const int *p = a, *q = b; return p[0] != q[0] ? p[0] - q[0] : p[1] != q[1] ? p[1] - q[1] : p[2] - q[2]; }
static int cmp_f(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return x < y ? -1 : x > y; }
static int nb4(const unsigned char *m, int w, int h, int x, int y) {
    return (y > 0 && m[(y - 1) * w + x]) + (y < h - 1 && m[(y + 1) * w + x]) + (x > 0 && m[y * w + x - 1]) + (x < w - 1 && m[y * w + x + 1]);
}
static int bbox_a(const unsigned char *p, int w, int h, int thr, int *x0, int *y0, int *x1, int *y1) {
    int x, y; *x0 = w; *y0 = h; *x1 = -1; *y1 = -1;
    for (y = 0; y < h; y++) for (x = 0; x < w; x++) if (p[((size_t)y * w + x) * 4 + 3] > thr) {
        if (x < *x0) *x0 = x; if (y < *y0) *y0 = y; if (x > *x1) *x1 = x; if (y > *y1) *y1 = y;
    }
    if (*x1 < 0) return 0;
    (*x1)++; (*y1)++; return 1;
}
static unsigned char clamp8(double v) { return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : (int)(v + 0.5)); }

/* a box blur of one float plane, radius r, three times (about a Gaussian of sigma ~ r) */
static void blur3(float *p, int w, int h, int r) {
    float *t = xm(sizeof(float) * (size_t)(w > h ? w : h)); int pass, x, y, i;
    if (r < 1) { free(t); return; }
    for (pass = 0; pass < 3; pass++) {
        for (y = 0; y < h; y++) {
            double s = 0; int n = 0; float *row = p + (size_t)y * w;
            for (i = 0; i <= r && i < w; i++) { s += row[i]; n++; }
            for (x = 0; x < w; x++) {
                t[x] = (float)(s / n);
                if (x + r + 1 < w) { s += row[x + r + 1]; n++; }
                if (x - r >= 0) { s -= row[x - r]; n--; }
            }
            memcpy(row, t, sizeof(float) * (size_t)w);
        }
        for (x = 0; x < w; x++) {
            double s = 0; int n = 0;
            for (i = 0; i <= r && i < h; i++) { s += p[(size_t)i * w + x]; n++; }
            for (y = 0; y < h; y++) {
                t[y] = (float)(s / n);
                if (y + r + 1 < h) { s += p[(size_t)(y + r + 1) * w + x]; n++; }
                if (y - r >= 0) { s -= p[(size_t)(y - r) * w + x]; n--; }
            }
            for (y = 0; y < h; y++) p[(size_t)y * w + x] = t[y];
        }
    }
    free(t);
}

/* ---------------------------------------------------------------- stage 2: before shrinking (on the cut-out crop) */
static void before(unsigned char *c, int w, int h, const ShrinkOpt *o, double scale) {
    size_t n = (size_t)w * h, i; int x, y, k;
    if (o->contrast || o->brightness || o->saturation) for (i = 0; i < n; i++) {
        unsigned char *p = c + i * 4; double v[3], Y;
        for (k = 0; k < 3; k++) v[k] = (p[k] - 128.0) * (1 + o->contrast / 100.0) + 128 + o->brightness * 1.28;
        Y = 0.299 * v[0] + 0.587 * v[1] + 0.114 * v[2];
        for (k = 0; k < 3; k++) p[k] = clamp8(Y + (v[k] - Y) * (1 + o->saturation / 100.0));
    }
    if (o->smooth == SM_MEDIAN || o->smooth == SM_BILATERAL) {
        unsigned char *s = xm(n * 4); memcpy(s, c, n * 4);
        for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
            unsigned char *p = c + ((size_t)y * w + x) * 4; int u, v;
            if (p[3] < 128) continue;
            if (o->smooth == SM_MEDIAN) {
                unsigned char ch[3][9]; int m = 0;
                for (v = -1; v <= 1; v++) for (u = -1; u <= 1; u++) {
                    int yy = y + v, xx = x + u; const unsigned char *q;
                    if (yy < 0 || yy >= h || xx < 0 || xx >= w) continue;
                    q = s + ((size_t)yy * w + xx) * 4; if (q[3] < 128) continue;
                    for (k = 0; k < 3; k++) ch[k][m] = q[k];
                    m++;
                }
                for (k = 0; k < 3; k++) { qsort(ch[k], (size_t)m, 1, cmp_u8); p[k] = ch[k][m / 2]; }
            } else {
                double acc[3] = { 0, 0, 0 }, ws = 0; const unsigned char *c0 = s + ((size_t)y * w + x) * 4;
                for (v = -2; v <= 2; v++) for (u = -2; u <= 2; u++) {
                    int yy = y + v, xx = x + u; const unsigned char *q; double d, wt;
                    if (yy < 0 || yy >= h || xx < 0 || xx >= w) continue;
                    q = s + ((size_t)yy * w + xx) * 4; if (q[3] < 128) continue;
                    d = (double)(q[0] - c0[0]) * (q[0] - c0[0]) + (double)(q[1] - c0[1]) * (q[1] - c0[1]) + (double)(q[2] - c0[2]) * (q[2] - c0[2]);
                    wt = exp(-(u * u + v * v) / 8.0 - d / (2 * 30.0 * 30.0));
                    for (k = 0; k < 3; k++) acc[k] += q[k] * wt;
                    ws += wt;
                }
                for (k = 0; k < 3; k++) p[k] = clamp8(acc[k] / ws);
            }
        }
        free(s);
    }
    if (o->sharpen > 0) {          /* unsharp mask at the size of a sprite pixel (sigma half of one) */
        float *pl = xm(sizeof(float) * n); int r = (int)(scale * 0.5 + 0.5);
        for (k = 0; k < 3; k++) {
            for (i = 0; i < n; i++) pl[i] = c[i * 4 + k];
            blur3(pl, w, h, r < 1 ? 1 : r);
            for (i = 0; i < n; i++) if (c[i * 4 + 3] >= 128) c[i * 4 + k] = clamp8(c[i * 4 + k] + (c[i * 4 + k] - pl[i]) * o->sharpen / 100.0);
        }
        free(pl);
    }
}

/* ---------------------------------------------------------------- stage 4: the methods. In: premultiplied planes
   pl[0..2] (colour x alpha / 255) and pl[3] (alpha), cw x ch. Out: the same, w x h. */
typedef struct { int x0, x1; } Span;
static Span *spans(int insz, int outsz) {
    Span *s = xm(sizeof(Span) * (size_t)outsz); int i; double f = (double)insz / outsz;
    for (i = 0; i < outsz; i++) { s[i].x0 = (int)floor(i * f); s[i].x1 = (int)floor((i + 1) * f); if (s[i].x1 <= s[i].x0) s[i].x1 = s[i].x0 + 1; if (s[i].x1 > insz) s[i].x1 = insz; if (s[i].x0 >= insz) s[i].x0 = insz - 1; }
    return s;
}
static void straight(float *const pl[4], size_t k, double *c) { double a = pl[3][k]; int j; for (j = 0; j < 3; j++) c[j] = a > 0 ? pl[j][k] * 255.0 / a : 0; }

/* majority (review A7): each pixel votes for the original's colour it is nearest to (weighted RGB), pixels near the
   cell's centre a little more (1 to 2); the cell takes the mean of the winning colour's pixels. With no palette, the
   commonest of 32 levels a channel, as before. pal: np RGB triples */
static void m_cells_sp(float *const pl[4], int cw, int ch, int w, int h, int method, float *rs[4], const unsigned char *pal, int np, Span *sx, Span *sy) {
    int x, y, u, v, j, mw = 1, mh = 1; (void)ch;
    float *buf; unsigned *keys;
    for (x = 0; x < w; x++) if (sx[x].x1 - sx[x].x0 > mw) mw = sx[x].x1 - sx[x].x0;
    for (y = 0; y < h; y++) if (sy[y].x1 - sy[y].x0 > mh) mh = sy[y].x1 - sy[y].x0;
    buf = xm(sizeof(float) * 3 * (size_t)mw * mh); keys = xm(sizeof(unsigned) * (size_t)mw * mh);
    for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
        size_t o = (size_t)y * w + x; double asum = 0; int n = 0, cnt = 0;
        for (v = sy[y].x0; v < sy[y].x1; v++) for (u = sx[x].x0; u < sx[x].x1; u++) {
            size_t k = (size_t)v * cw + u; asum += pl[3][k]; cnt++;
            if (pl[3][k] >= 128) { double c[3]; straight(pl, k, c); for (j = 0; j < 3; j++) buf[(size_t)n * 3 + j] = (float)c[j]; n++; }
        }
        rs[3][o] = (float)(asum / cnt);
        if (method == M_NEAREST) {
            size_t k = (size_t)((sy[y].x0 + sy[y].x1) / 2) * cw + (sx[x].x0 + sx[x].x1) / 2;
            rs[3][o] = pl[3][k]; for (j = 0; j < 3; j++) rs[j][o] = pl[j][k];
            continue;
        }
        if (!n) { for (j = 0; j < 3; j++) rs[j][o] = 0; continue; }
        if (method == M_MEDIAN) {
            float *t = xm(sizeof(float) * (size_t)n);
            for (j = 0; j < 3; j++) { int i; for (i = 0; i < n; i++) t[i] = buf[(size_t)i * 3 + j]; qsort(t, (size_t)n, sizeof(float), cmp_f); rs[j][o] = t[n / 2] * rs[3][o] / 255.0f; }
            free(t);
        } else if (np > 0) {
            double vote[256], m[3] = { 0, 0, 0 }, cxm = (sx[x].x0 + sx[x].x1 - 1) / 2.0, cym = (sy[y].x0 + sy[y].x1 - 1) / 2.0;
            double rx = (sx[x].x1 - sx[x].x0) / 2.0 + 0.5, ry = (sy[y].x1 - sy[y].x0) / 2.0 + 0.5; int i, best = 0, bc = 0, q;
            for (q = 0; q < np; q++) vote[q] = 0;
            i = 0;
            for (v = sy[y].x0; v < sy[y].x1; v++) for (u = sx[x].x0; u < sx[x].x1; u++) {
                size_t k = (size_t)v * cw + u; double dx, dy, dd, bd = 1e30;
                if (pl[3][k] < 128) continue;
                for (q = 0; q < np; q++) {
                    double dr = buf[(size_t)i * 3] - pal[q * 3], dg = buf[(size_t)i * 3 + 1] - pal[q * 3 + 1], db = buf[(size_t)i * 3 + 2] - pal[q * 3 + 2];
                    dd = 2 * dr * dr + 4 * dg * dg + 3 * db * db; if (dd < bd) { bd = dd; keys[i] = (unsigned)q; }
                }
                dx = (u - cxm) / rx; dy = (v - cym) / ry; dd = sqrt(dx * dx + dy * dy); if (dd > 1) dd = 1;
                vote[keys[i]] += 2.0 - dd; i++;
            }
            for (q = 1; q < np; q++) if (vote[q] > vote[best]) best = q;
            for (i = 0; i < n; i++) if (keys[i] == (unsigned)best) { for (j = 0; j < 3; j++) m[j] += buf[(size_t)i * 3 + j]; bc++; }
            for (j = 0; j < 3; j++) rs[j][o] = (float)(m[j] / (bc ? bc : 1) * rs[3][o] / 255.0);
        } else {             /* majority: the commonest colour, 32 levels a channel; its pixels' mean */
            int i, best = 0, bc = 0; double m[3] = { 0, 0, 0 };
            for (i = 0; i < n; i++) keys[i] = ((unsigned)buf[i * 3] >> 3) << 10 | ((unsigned)buf[i * 3 + 1] >> 3) << 5 | ((unsigned)buf[i * 3 + 2] >> 3);
            for (i = 0; i < n; i++) { int c2 = 0, q; for (q = 0; q < n; q++) c2 += keys[q] == keys[i]; if (c2 > bc) { bc = c2; best = i; } }
            for (i = 0; i < n; i++) if (keys[i] == keys[best]) for (j = 0; j < 3; j++) m[j] += buf[(size_t)i * 3 + j];
            for (j = 0; j < 3; j++) rs[j][o] = (float)(m[j] / bc * rs[3][o] / 255.0);
        }
    }
    free(buf); free(keys);
}
static void m_cells(float *const pl[4], int cw, int ch, int w, int h, int method, float *rs[4], const unsigned char *pal, int np) {
    Span *sx = spans(cw, w), *sy = spans(ch, h);
    m_cells_sp(pl, cw, ch, w, h, method, rs, pal, np, sx, sy);
    free(sx); free(sy);
}

/* grid snap (B6, 2026-11-22; the idea of unfake.js / proper-pixel-art, written here from their descriptions): a model
   that draws "pixel art" draws it with soft cells of its own (often a fraction of a pixel wide: 1024 / 128 = 8, or
   10.24). The cell size and where the grid starts are found from the edges: the colour and cover changes summed down
   each column (across each row), and the step and offset whose lines fall on the most change win. Each cell then takes
   its commonest colour, from its middle only (the soft seams left out), and the clean cells vote into the sprite's
   pixels as Majority does. No grid clear enough (its lines less than 1.6 times the average change): Majority. */
/* the shrinks run on three threads (the reshaper, the drawing, the window): what they share, the palette and the last
   grid found, goes through this lock, copied whole (review 3, P3-3) */
static CRITICAL_SECTION SH_CS; static volatile LONG SH_INIT;
static void sh_lock(void) {
    if (SH_INIT != 2) { if (InterlockedCompareExchange(&SH_INIT, 1, 0) == 0) { InitializeCriticalSection(&SH_CS); InterlockedExchange(&SH_INIT, 2); } else while (SH_INIT != 2) Sleep(0); }
    EnterCriticalSection(&SH_CS);
}
static void sh_unlock(void) { LeaveCriticalSection(&SH_CS); }
static double G_CX, G_CY; static int G_STARS;
void shrink_grid_info(double *cx, double *cy, int *st) { sh_lock(); if (cx) *cx = G_CX; if (cy) *cy = G_CY; if (st) *st = G_STARS; sh_unlock(); }
static void grid_note(double cx, double cy, int st, const char *line) {     /* the last grid, as one; logged when it changes */
    static double lx = -1, ly = -1; static int ls = -1; int changed;
    sh_lock(); G_CX = cx; G_CY = cy; G_STARS = st;
    changed = fabs(cx - lx) > 0.01 || fabs(cy - ly) > 0.01 || st != ls; lx = cx; ly = cy; ls = st; sh_unlock();
    if (changed) vlog(VLOG_INFO, "shrink", "%s", line);
}
static double grid_find(const double *prof, int n, double s_max, double *best_off, double *score) {   /* returns the step (0: none), *best_off; *score: the weaker axis's */
    double mean = 0, best = 0, bs = 0, bo = 0, s, o; int i;
    struct { double s, o, sc; } cand[512]; int nc = 0;
    for (i = 1; i < n; i++) mean += prof[i];
    mean /= n > 1 ? n - 1 : 1;
    if (mean <= 0) return 0;
    for (s = 3.0; s <= s_max && nc < 512; s += 0.05) {
        double bsc = 0, bof = 0;
        for (o = 0; o < s; o += 0.25) {
            double sum = 0; int k = 0; double x;
            for (x = o; x < n; x += s) { int u = (int)(x + 0.5); if (u >= 1 && u < n) { sum += prof[u]; k++; } }
            if (k >= 4 && sum / k > bsc) { bsc = sum / k; bof = o; }
        }
        cand[nc].s = s; cand[nc].o = bof; cand[nc].sc = bsc / mean; if (cand[nc].sc > best) best = cand[nc].sc; nc++;
    }
    for (i = 0; i < nc; i++) if (cand[i].sc >= best * 0.92) { bs = cand[i].s; bo = cand[i].o; break; }   /* the smallest step nearly as good (its multiples score as well) */
    if (*score == 0 || best < *score) *score = best;      /* the weaker axis decides */
    *best_off = bo;
    return best >= 1.6 ? bs : -best;
}
static Span *grid_spans(int n, double step, double off, int *count) {     /* each cell's middle: 20% left off each side */
    Span *sp = xm(sizeof(Span) * (size_t)(n / step + 4)); int k = 0; double a = off - step;
    for (; a < n; a += step) {
        double b = a + step, lo = a < 0 ? 0 : a, hi = b > n ? n : b;
        if (hi - lo < step * 0.45) continue;                                   /* a sliver at the edge */
        sp[k].x0 = (int)ceil(lo + step * 0.2); sp[k].x1 = (int)floor(hi - step * 0.2);
        if (sp[k].x1 <= sp[k].x0) { sp[k].x0 = (int)lo; sp[k].x1 = (int)lo + 1; }
        if (sp[k].x1 > n) sp[k].x1 = n; if (sp[k].x0 >= n) sp[k].x0 = n - 1;
        k++;
    }
    *count = k; return sp;
}
static void m_grid(float *const pl[4], int cw, int ch, int w, int h, float *rs[4], const unsigned char *pal, int np) {
    double *px = calloc((size_t)cw, sizeof(double)), *py = calloc((size_t)ch, sizeof(double)), ox = 0, oy = 0, sx, sy, score = 0; int x, y, j; char line[300];
    for (y = 0; y < ch; y++) for (x = 0; x < cw; x++) {
        size_t k = (size_t)y * cw + x; double d;
        if (x) { d = fabs(pl[3][k] - pl[3][k - 1]); for (j = 0; j < 3; j++) d += fabs(pl[j][k] - pl[j][k - 1]); px[x] += d; }
        if (y) { d = fabs(pl[3][k] - pl[3][k - cw]); for (j = 0; j < 3; j++) d += fabs(pl[j][k] - pl[j][k - cw]); py[y] += d; }
    }
    sx = grid_find(px, cw, (double)cw / w * 1.5 > 3.5 ? (double)cw / w * 1.5 : 3.5, &ox, &score);
    sy = grid_find(py, ch, (double)ch / h * 1.5 > 3.5 ? (double)ch / h * 1.5 : 3.5, &oy, &score);
    free(px); free(py);
    if (sx <= 0 || sy <= 0) {            /* no grid: Majority */
        snprintf(line, sizeof line, "grid snap: no clear grid (best %.2f / %.2f times the average edge): Majority", -sx, -sy);
        grid_note(0, 0, 0, line);
        m_cells(pl, cw, ch, w, h, M_MAJORITY, rs, pal, np); return;
    }
    {   int nx, ny, c; Span *gx = grid_spans(cw, sx, ox, &nx), *gy = grid_spans(ch, sy, oy, &ny); float *cl[4];
        for (c = 0; c < 4; c++) cl[c] = xm(sizeof(float) * (size_t)nx * ny);
        m_cells_sp(pl, cw, ch, nx, ny, M_MAJORITY, cl, pal, np, gx, gy);     /* each cell: its commonest colour */
        m_cells(cl, nx, ny, w, h, M_MAJORITY, rs, pal, np);                  /* the clean cells: into the sprite */
        for (c = 0; c < 4; c++) free(cl[c]);
        free(gx); free(gy);
        {   int st = score >= 3.0 ? 3 : score >= 2.2 ? 2 : 1;      /* 1 to 3: how far the grid's lines stand out */
            snprintf(line, sizeof line, "grid snap: cells %.2f x %.2f px (offset %.2f, %.2f), %d x %d cells into %d x %d, lines %.2f times the average edge (%d of 3)", sx, sy, ox, oy, nx, ny, w, h, score, st);
            grid_note(sx, sy, st, line); }
    }
}
/* DPID (Weber 2016): weights (|I(q) - G(p)| / Vmax) ^ lambda over the sprite pixel's area, G the area means smoothed
   by [1 2 1] x [1 2 1] / 16; alpha weighs in too */
static void m_dpid(float *const pl[4], int cw, int ch, int w, int h, double lambda, float *rs[4]) {
    Span *sx = spans(cw, w), *sy = spans(ch, h); int x, y, u, v, j;
    double *g = xm(sizeof(double) * 4 * (size_t)w * h), *gs = xm(sizeof(double) * 4 * (size_t)w * h); const double vmax = sqrt(3.0) * 255;
    for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
        double s[4] = { 0, 0, 0, 0 }; int c = 0;
        for (v = sy[y].x0; v < sy[y].x1; v++) for (u = sx[x].x0; u < sx[x].x1; u++) { size_t k = (size_t)v * cw + u; for (j = 0; j < 4; j++) s[j] += pl[j][k]; c++; }
        for (j = 0; j < 4; j++) g[((size_t)y * w + x) * 4 + j] = s[j] / c;
    }
    for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
        double s[4] = { 0, 0, 0, 0 }, ws = 0;
        for (v = -1; v <= 1; v++) for (u = -1; u <= 1; u++) {
            int yy = y + v, xx = x + u; double wt = (2 - abs(u)) * (2 - abs(v));
            if (yy < 0 || yy >= h || xx < 0 || xx >= w) continue;
            for (j = 0; j < 4; j++) s[j] += g[((size_t)yy * w + xx) * 4 + j] * wt;
            ws += wt;
        }
        for (j = 0; j < 4; j++) gs[((size_t)y * w + x) * 4 + j] = s[j] / ws;
    }
    for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
        size_t o = (size_t)y * w + x; double gc[3], s[3] = { 0, 0, 0 }, ws = 0, ga = gs[o * 4 + 3];
        for (j = 0; j < 3; j++) gc[j] = ga > 0 ? gs[o * 4 + j] * 255 / ga : 0;
        for (v = sy[y].x0; v < sy[y].x1; v++) for (u = sx[x].x0; u < sx[x].x1; u++) {
            size_t k = (size_t)v * cw + u; double c[3], d, wt;
            if (pl[3][k] <= 0) continue;
            straight(pl, k, c);
            d = sqrt((c[0] - gc[0]) * (c[0] - gc[0]) + (c[1] - gc[1]) * (c[1] - gc[1]) + (c[2] - gc[2]) * (c[2] - gc[2]));
            wt = (lambda == 0 ? 1.0 : pow(d / vmax, lambda)) * pl[3][k];
            for (j = 0; j < 3; j++) s[j] += c[j] * wt;
            ws += wt;
        }
        rs[3][o] = (float)g[o * 4 + 3];
        for (j = 0; j < 3; j++) rs[j][o] = (float)((ws > 1e-9 ? s[j] / ws : gc[j]) * rs[3][o] / 255.0);
    }
    free(g); free(gs); free(sx); free(sy);
}
/* SAID (Arjun 2025): edge-guided blend + texture term, on the premultiplied planes */
static void m_said(float *const pl[4], int cw, int ch, int w, int h, const ShrinkOpt *o, float *rs[4]) {
    size_t n = (size_t)cw * ch, i; int x, y, j; double scale = (double)cw / w;
    float *E = xm(sizeof(float) * n), *Ed, *t = xm(sizeof(float) * n), *IB, *Isd, emin = 1e30f, emax = -1e30f;
    double gam = o->said_gamma / 100.0, al = o->said_alpha / 100.0, be = o->said_beta / 100.0;
    for (y = 0; y < ch; y++) for (x = 0; x < cw; x++) {          /* Sobel on the luma of the premultiplied picture */
        double gx = 0, gy = 0; int u, v;
        for (v = -1; v <= 1; v++) for (u = -1; u <= 1; u++) {
            int yy = y + v < 0 ? 0 : y + v >= ch ? ch - 1 : y + v, xx = x + u < 0 ? 0 : x + u >= cw ? cw - 1 : x + u;
            size_t k = (size_t)yy * cw + xx; double L = 0.299 * pl[0][k] + 0.587 * pl[1][k] + 0.114 * pl[2][k];
            gx += L * u * (v == 0 ? 2 : 1); gy += L * v * (u == 0 ? 2 : 1);
        }
        E[(size_t)y * cw + x] = (float)sqrt(gx * gx + gy * gy);
    }
    for (i = 0; i < n; i++) { if (E[i] < emin) emin = E[i]; if (E[i] > emax) emax = E[i]; }
    for (i = 0; i < n; i++) E[i] = emax > emin ? (E[i] - emin) / (emax - emin) : 0;
    Ed = resize_k(E, cw, ch, w, h, k_cubic, 2.0);
    { float m = 0; for (i = 0; i < (size_t)w * h; i++) if (Ed[i] > m) m = Ed[i]; for (i = 0; i < (size_t)w * h; i++) Ed[i] = m > 0 ? Ed[i] / m : 0; }
    for (j = 0; j < 4; j++) {
        IB = resize_k(pl[j], cw, ch, w, h, k_cubic, 2.0);
        if (j == 3) { memcpy(rs[3], IB, sizeof(float) * (size_t)w * h); free(IB); break; }
        memcpy(t, pl[j], sizeof(float) * n); blur3(t, cw, ch, (int)(scale * 0.5 + 0.5) < 1 ? 1 : (int)(scale * 0.5 + 0.5));
        for (i = 0; i < n; i++) t[i] = (float)(pl[j][i] + gam * (pl[j][i] - t[i]));
        Isd = resize_k(t, cw, ch, w, h, k_cubic, 2.0);
        for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
            size_t k = (size_t)y * w + x; double e = Ed[k], ip = (1 - e) * IB[k] + e * Isd[k], lap = 0; int c = 0;
            if (x > 0) { lap += IB[k - 1]; c++; } if (x < w - 1) { lap += IB[k + 1]; c++; }
            if (y > 0) { lap += IB[k - w]; c++; } if (y < h - 1) { lap += IB[k + w]; c++; }
            lap -= c * IB[k];
            rs[j][k] = (float)(ip - (al * e + be) * lap);
        }
        free(IB); free(Isd);
    }
    for (i = 0; i < (size_t)w * h; i++) for (j = 0; j < 3; j++) { if (rs[j][i] < 0) rs[j][i] = 0; if (rs[j][i] > rs[3][i] && rs[3][i] > 0) rs[j][i] = rs[3][i]; }
    free(E); free(Ed); free(t);
}
/* superpixels (Gerstner 2012): SLIC seeded one per sprite pixel, Lab + position (compactness m), positions smoothed */
static void m_superpixel(float *const pl[4], int cw, int ch, int w, int h, const ShrinkOpt *o, float *rs[4]) {
    size_t n = (size_t)cw * ch, i; int x, y, it, k, j; double S = sqrt((double)n / ((size_t)w * h)), m = o->sp_m;
    int K = w * h, *lbl = xm(sizeof(int) * n); double *cen = xm(sizeof(double) * 5 * K), *acc = xm(sizeof(double) * 6 * K), *L = xm(sizeof(double) * 3 * n);
    double sx = (double)cw / w, sy = (double)ch / h;
    for (i = 0; i < n; i++) { double c[3]; straight(pl, i, c); lab(c[0], c[1], c[2], L + i * 3); }
    for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
        double s[3] = { 0, 0, 0 }; int c = 0, u, v;
        k = y * w + x; cen[k * 5 + 3] = (x + 0.5) * sx; cen[k * 5 + 4] = (y + 0.5) * sy;
        for (v = (int)(y * sy); v < (int)((y + 1) * sy) && v < ch; v++) for (u = (int)(x * sx); u < (int)((x + 1) * sx) && u < cw; u++) { size_t q = (size_t)v * cw + u; for (j = 0; j < 3; j++) s[j] += L[q * 3 + j]; c++; }
        for (j = 0; j < 3; j++) cen[k * 5 + j] = c ? s[j] / c : 0;
    }
    for (it = 0; it < o->sp_iter; it++) {
        memset(acc, 0, sizeof(double) * 6 * K);
        for (y = 0; y < ch; y++) for (x = 0; x < cw; x++) {
            size_t q = (size_t)y * cw + x; int gx = (int)(x / sx), gy = (int)(y / sy), u, v, best = -1; double bd = 1e300;
            if (pl[3][q] < 128) { lbl[q] = -1; continue; }
            for (v = gy - 1; v <= gy + 1; v++) for (u = gx - 1; u <= gx + 1; u++) {
                double dc, ds, d; int c2;
                if (u < 0 || v < 0 || u >= w || v >= h) continue;
                c2 = v * w + u;
                dc = (L[q * 3] - cen[c2 * 5]) * (L[q * 3] - cen[c2 * 5]) + (L[q * 3 + 1] - cen[c2 * 5 + 1]) * (L[q * 3 + 1] - cen[c2 * 5 + 1]) + (L[q * 3 + 2] - cen[c2 * 5 + 2]) * (L[q * 3 + 2] - cen[c2 * 5 + 2]);
                ds = (x - cen[c2 * 5 + 3]) * (x - cen[c2 * 5 + 3]) + (y - cen[c2 * 5 + 4]) * (y - cen[c2 * 5 + 4]);
                d = dc + ds / (S * S) * m * m;
                if (d < bd) { bd = d; best = c2; }
            }
            lbl[q] = best;
            for (j = 0; j < 3; j++) acc[best * 6 + j] += L[q * 3 + j];
            acc[best * 6 + 3] += x; acc[best * 6 + 4] += y; acc[best * 6 + 5] += 1;
        }
        for (k = 0; k < K; k++) if (acc[k * 6 + 5] > 0) for (j = 0; j < 5; j++) cen[k * 5 + j] = acc[k * 6 + j] / acc[k * 6 + 5];
        {   /* positions pulled 40% towards their grid neighbours' mean (Gerstner's Laplacian smoothing) */
            double *np = xm(sizeof(double) * 2 * K);
            for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
                double s0 = 0, s1 = 0; int c = 0; k = y * w + x;
                if (x > 0) { s0 += cen[(k - 1) * 5 + 3]; s1 += cen[(k - 1) * 5 + 4]; c++; }
                if (x < w - 1) { s0 += cen[(k + 1) * 5 + 3]; s1 += cen[(k + 1) * 5 + 4]; c++; }
                if (y > 0) { s0 += cen[(k - w) * 5 + 3]; s1 += cen[(k - w) * 5 + 4]; c++; }
                if (y < h - 1) { s0 += cen[(k + w) * 5 + 3]; s1 += cen[(k + w) * 5 + 4]; c++; }
                np[k * 2] = c ? 0.6 * cen[k * 5 + 3] + 0.4 * s0 / c : cen[k * 5 + 3];
                np[k * 2 + 1] = c ? 0.6 * cen[k * 5 + 4] + 0.4 * s1 / c : cen[k * 5 + 4];
            }
            for (k = 0; k < K; k++) { cen[k * 5 + 3] = np[k * 2]; cen[k * 5 + 4] = np[k * 2 + 1]; }
            free(np);
        }
    }
    {   /* each sprite pixel: its superpixel's mean colour (in RGB), alpha its area's cover */
        double *rgb = xm(sizeof(double) * 4 * K); Span *spx = spans(cw, w), *spy = spans(ch, h);
        memset(rgb, 0, sizeof(double) * 4 * K);
        for (i = 0; i < n; i++) if (lbl[i] >= 0) { double c[3]; straight(pl, i, c); for (j = 0; j < 3; j++) rgb[lbl[i] * 4 + j] += c[j]; rgb[lbl[i] * 4 + 3] += 1; }
        for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
            double a = 0; int c = 0, u, v; k = y * w + x;
            for (v = spy[y].x0; v < spy[y].x1; v++) for (u = spx[x].x0; u < spx[x].x1; u++) { a += pl[3][(size_t)v * cw + u]; c++; }
            rs[3][k] = (float)(a / c);
            for (j = 0; j < 3; j++) rs[j][k] = rgb[k * 4 + 3] > 0 ? (float)(rgb[k * 4 + j] / rgb[k * 4 + 3] * rs[3][k] / 255.0) : 0;
        }
        free(rgb); free(spx); free(spy);
    }
    free(lbl); free(cen); free(acc); free(L);
}

/* ---------------------------------------------------------------- stage 6: palettes */
static int kmeans(const unsigned char *px, int W, int H, int N, int (*pal)[3], int dmode, int pp) {
    int n = 0, i, j, k, it; double (*pts)[3] = xm(sizeof(double[3]) * (size_t)W * H), (*c)[3] = xm(sizeof(double[3]) * (size_t)N), (*cl)[3] = xm(sizeof(double[3]) * (size_t)N), (*lp)[3];
    for (i = 0; i < W * H; i++) { const unsigned char *p = px + (size_t)i * 4; if (p[3] >= 128 && (p[0] | p[1] | p[2])) { pts[n][0] = p[0]; pts[n][1] = p[1]; pts[n][2] = p[2]; n++; } }
    if (!n) { free(pts); free(c); free(cl); return 0; }
    lp = xm(sizeof(double[3]) * (size_t)n); for (i = 0; i < n; i++) lab(pts[i][0], pts[i][1], pts[i][2], lp[i]);
    if (pp) {   /* k-means++ (Arthur & Vassilvitskii 2007): each seed drawn with odds by its squared distance to the nearest
                   seed so far; a fixed-seed generator, so the same sprite still gives the same palette (B7) */
        unsigned st = 2463534242u; double *dd = xm(sizeof(double) * (size_t)n), tot;
        #define RNDU() (st ^= st << 13, st ^= st >> 17, st ^= st << 5, (st & 0xffffff) / 16777216.0)
        i = (int)(RNDU() * n); for (j = 0; j < 3; j++) c[0][j] = pts[i][j];
        for (k = 1; k < N; k++) { double r;
            for (tot = 0, i = 0; i < n; i++) { double d = 1e300; int q; for (q = 0; q < k; q++) { double e = (pts[i][0] - c[q][0]) * (pts[i][0] - c[q][0]) + (pts[i][1] - c[q][1]) * (pts[i][1] - c[q][1]) + (pts[i][2] - c[q][2]) * (pts[i][2] - c[q][2]); if (e < d) d = e; } dd[i] = d; tot += d; }
            r = RNDU() * tot; for (i = 0; i < n - 1 && (r -= dd[i]) > 0; i++) ;
            for (j = 0; j < 3; j++) c[k][j] = pts[i][j]; }
        #undef RNDU
        free(dd);
    } else
    /* farthest-first seeds from the mean (no randomness: the same sprite gives the same palette) */
    { double m[3] = { 0, 0, 0 }; for (i = 0; i < n; i++) for (j = 0; j < 3; j++) m[j] += pts[i][j] / n;
      for (k = 0; k < N; k++) { int best = 0; double bd = -1;
        for (i = 0; i < n; i++) { double d = 1e300; int q;
            if (!k) { d = (pts[i][0] - m[0]) * (pts[i][0] - m[0]) + (pts[i][1] - m[1]) * (pts[i][1] - m[1]) + (pts[i][2] - m[2]) * (pts[i][2] - m[2]); d = -d; }
            else for (q = 0; q < k; q++) { double e = (pts[i][0] - c[q][0]) * (pts[i][0] - c[q][0]) + (pts[i][1] - c[q][1]) * (pts[i][1] - c[q][1]) + (pts[i][2] - c[q][2]) * (pts[i][2] - c[q][2]); if (e < d) d = e; }
            if (d > bd) { bd = d; best = i; } }
        for (j = 0; j < 3; j++) c[k][j] = pts[best][j]; } }
    for (it = 0; it < 12; it++) {
        double (*s)[4] = xm(sizeof(double[4]) * (size_t)N); memset(s, 0, sizeof(double[4]) * (size_t)N);
        for (k = 0; k < N; k++) lab(c[k][0], c[k][1], c[k][2], cl[k]);
        for (i = 0; i < n; i++) { int best = 0; double bd = 1e300; for (k = 0; k < N; k++) { double d = cdist(dmode, pts[i], c[k], lp[i], cl[k]); if (d < bd) { bd = d; best = k; } }
            for (j = 0; j < 3; j++) s[best][j] += pts[i][j]; s[best][3] += 1; }
        for (k = 0; k < N; k++) if (s[k][3] > 0) for (j = 0; j < 3; j++) c[k][j] = s[k][j] / s[k][3];
        free(s);
    }
    for (k = 0; k < N; k++) for (j = 0; j < 3; j++) pal[k][j] = (int)(c[k][j] + 0.5);
    free(pts); free(c); free(cl); free(lp);
    return N < n ? N : n;
}
/* Wu's colour quantizer (Xiaolin Wu, "Efficient Statistical Computations for Optimal Color Quantization", Graphics
   Gems II, 1991), written here from the paper's method: a 33^3 histogram of 5-bit colours with cumulative moments; the
   box with the most variance is cut where the two halves' summed variance drops most, until N boxes; each box's mean
   is a palette colour. Opaque, non-black pixels only (as k-means). */
enum { WU_R, WU_G, WU_B };
#define WI(r, g, b) ((r) * 33 * 33 + (g) * 33 + (b))
typedef struct { int r0, r1, g0, g1, b0, b1, vol; } WuBox;
static double wu_vol(const WuBox *c, const double *m) {
    return m[WI(c->r1, c->g1, c->b1)] - m[WI(c->r1, c->g1, c->b0)] - m[WI(c->r1, c->g0, c->b1)] + m[WI(c->r1, c->g0, c->b0)]
         - m[WI(c->r0, c->g1, c->b1)] + m[WI(c->r0, c->g1, c->b0)] + m[WI(c->r0, c->g0, c->b1)] - m[WI(c->r0, c->g0, c->b0)];
}
static double wu_bottom(const WuBox *c, int dir, const double *m) {
    if (dir == WU_R) return -m[WI(c->r0, c->g1, c->b1)] + m[WI(c->r0, c->g1, c->b0)] + m[WI(c->r0, c->g0, c->b1)] - m[WI(c->r0, c->g0, c->b0)];
    if (dir == WU_G) return -m[WI(c->r1, c->g0, c->b1)] + m[WI(c->r1, c->g0, c->b0)] + m[WI(c->r0, c->g0, c->b1)] - m[WI(c->r0, c->g0, c->b0)];
    return -m[WI(c->r1, c->g1, c->b0)] + m[WI(c->r1, c->g0, c->b0)] + m[WI(c->r0, c->g1, c->b0)] - m[WI(c->r0, c->g0, c->b0)];
}
static double wu_top(const WuBox *c, int dir, int pos, const double *m) {
    if (dir == WU_R) return m[WI(pos, c->g1, c->b1)] - m[WI(pos, c->g1, c->b0)] - m[WI(pos, c->g0, c->b1)] + m[WI(pos, c->g0, c->b0)];
    if (dir == WU_G) return m[WI(c->r1, pos, c->b1)] - m[WI(c->r1, pos, c->b0)] - m[WI(c->r0, pos, c->b1)] + m[WI(c->r0, pos, c->b0)];
    return m[WI(c->r1, c->g1, pos)] - m[WI(c->r1, c->g0, pos)] - m[WI(c->r0, c->g1, pos)] + m[WI(c->r0, c->g0, pos)];
}
static double wu_var(const WuBox *c, double *const M[5]) {
    double dr = wu_vol(c, M[1]), dg = wu_vol(c, M[2]), db = wu_vol(c, M[3]), w = wu_vol(c, M[0]);
    return w > 0 ? wu_vol(c, M[4]) - (dr * dr + dg * dg + db * db) / w : 0;
}
static double wu_max(const WuBox *c, int dir, int first, int last, int *cut, double wr, double wg, double wb, double ww, double *const M[5]) {
    double br = wu_bottom(c, dir, M[1]), bg = wu_bottom(c, dir, M[2]), bb = wu_bottom(c, dir, M[3]), bw = wu_bottom(c, dir, M[0]), best = 0; int i;
    *cut = -1;
    for (i = first; i < last; i++) {
        double hr = br + wu_top(c, dir, i, M[1]), hg = bg + wu_top(c, dir, i, M[2]), hb = bb + wu_top(c, dir, i, M[3]), hw = bw + wu_top(c, dir, i, M[0]), t;
        if (hw <= 0) continue;
        t = (hr * hr + hg * hg + hb * hb) / hw;
        hr = wr - hr; hg = wg - hg; hb = wb - hb; hw = ww - hw;
        if (hw <= 0) continue;
        t += (hr * hr + hg * hg + hb * hb) / hw;
        if (t > best) { best = t; *cut = i; }
    }
    return best;
}
static int wu_cut(WuBox *a, WuBox *b, double *const M[5]) {
    double wr = wu_vol(a, M[1]), wg = wu_vol(a, M[2]), wb = wu_vol(a, M[3]), ww = wu_vol(a, M[0]), mr, mg, mb; int cr, cg, cb, dir;
    mr = wu_max(a, WU_R, a->r0 + 1, a->r1, &cr, wr, wg, wb, ww, M);
    mg = wu_max(a, WU_G, a->g0 + 1, a->g1, &cg, wr, wg, wb, ww, M);
    mb = wu_max(a, WU_B, a->b0 + 1, a->b1, &cb, wr, wg, wb, ww, M);
    if (mr >= mg && mr >= mb) { dir = WU_R; if (cr < 0) return 0; } else if (mg >= mr && mg >= mb) dir = WU_G; else dir = WU_B;
    b->r1 = a->r1; b->g1 = a->g1; b->b1 = a->b1;
    if (dir == WU_R) { b->r0 = a->r1 = cr; b->g0 = a->g0; b->b0 = a->b0; }
    else if (dir == WU_G) { b->g0 = a->g1 = cg; b->r0 = a->r0; b->b0 = a->b0; }
    else { b->b0 = a->b1 = cb; b->r0 = a->r0; b->g0 = a->g0; }
    a->vol = (a->r1 - a->r0) * (a->g1 - a->g0) * (a->b1 - a->b0); b->vol = (b->r1 - b->r0) * (b->g1 - b->g0) * (b->b1 - b->b0);
    return 1;
}
static int wu(const unsigned char *px, int W, int H, int N, int (*pal)[3]) {
    double *M[5]; WuBox box[256]; double vv[256]; int i, r, g, b, k, next = 0, n = 0, K = N;
    for (i = 0; i < 5; i++) M[i] = calloc(33 * 33 * 33, sizeof(double));
    for (i = 0; i < W * H; i++) { const unsigned char *p = px + (size_t)i * 4; int q;
        if (p[3] < 128 || !(p[0] | p[1] | p[2])) continue;
        q = WI((p[0] >> 3) + 1, (p[1] >> 3) + 1, (p[2] >> 3) + 1); n++;
        M[0][q] += 1; M[1][q] += p[0]; M[2][q] += p[1]; M[3][q] += p[2]; M[4][q] += (double)p[0] * p[0] + (double)p[1] * p[1] + (double)p[2] * p[2]; }
    if (!n) { for (i = 0; i < 5; i++) free(M[i]); return 0; }
    for (r = 1; r <= 32; r++) {         /* the moments summed: each cell holds all from (0,0,0) to it */
        double area[5][33]; memset(area, 0, sizeof area);
        for (g = 1; g <= 32; g++) { double line[5] = { 0, 0, 0, 0, 0 };
            for (b = 1; b <= 32; b++) { int q = WI(r, g, b);
                for (i = 0; i < 5; i++) { line[i] += M[i][q]; area[i][b] += line[i]; M[i][q] = M[i][WI(r - 1, g, b)] + area[i][b]; } } }
    }
    if (K > 256) K = 256;
    box[0].r0 = box[0].g0 = box[0].b0 = 0; box[0].r1 = box[0].g1 = box[0].b1 = 32;
    for (i = 1; i < K; i++) {
        if (wu_cut(&box[next], &box[i], M)) { vv[next] = box[next].vol > 1 ? wu_var(&box[next], M) : 0; vv[i] = box[i].vol > 1 ? wu_var(&box[i], M) : 0; }
        else { vv[next] = 0; i--; }
        next = 0; { double t = vv[0]; for (k = 1; k <= i; k++) if (vv[k] > t) { t = vv[k]; next = k; } if (t <= 0) { K = i + 1; break; } }
    }
    for (k = 0, i = 0; k < K; k++) { double w = wu_vol(&box[k], M[0]);
        if (w > 0) { pal[i][0] = (int)(wu_vol(&box[k], M[1]) / w + 0.5); pal[i][1] = (int)(wu_vol(&box[k], M[2]) / w + 0.5); pal[i][2] = (int)(wu_vol(&box[k], M[3]) / w + 0.5); i++; } }
    for (k = 0; k < 5; k++) free(M[k]);
    return i;
}
#undef WI
static unsigned char GPAL[256 * 3]; static int GPAL_N;
void shrink_set_palette(const unsigned char *rgb, int n) { if (n > 256) n = 256; if (n < 0) n = 0; sh_lock(); if (n) memcpy(GPAL, rgb, (size_t)n * 3); GPAL_N = n; sh_unlock(); }
typedef struct { int lo, hi; } Box;
static int g_axis;
static int cmp_axis(const void *a, const void *b) { return ((const int *)a)[g_axis] - ((const int *)b)[g_axis]; }
static int mediancut(const unsigned char *px, int W, int H, int N, int (*pal)[3]) {
    int n = 0, i, j, nb = 1; int (*pts)[3] = xm(sizeof(int[3]) * (size_t)W * H); Box b[64];
    for (i = 0; i < W * H; i++) { const unsigned char *p = px + (size_t)i * 4; if (p[3] >= 128 && (p[0] | p[1] | p[2])) { pts[n][0] = p[0]; pts[n][1] = p[1]; pts[n][2] = p[2]; n++; } }
    if (!n) { free(pts); return 0; }
    b[0].lo = 0; b[0].hi = n;
    while (nb < N) {     /* split the box with the widest channel range at its median */
        int bi = -1, ax = 0, rng = -1;
        for (i = 0; i < nb; i++) { int mn[3] = { 255, 255, 255 }, mx[3] = { 0, 0, 0 }, q; if (b[i].hi - b[i].lo < 2) continue;
            for (q = b[i].lo; q < b[i].hi; q++) for (j = 0; j < 3; j++) { if (pts[q][j] < mn[j]) mn[j] = pts[q][j]; if (pts[q][j] > mx[j]) mx[j] = pts[q][j]; }
            for (j = 0; j < 3; j++) if (mx[j] - mn[j] > rng) { rng = mx[j] - mn[j]; bi = i; ax = j; } }
        if (bi < 0 || rng <= 0) break;
        g_axis = ax; qsort(pts + b[bi].lo, (size_t)(b[bi].hi - b[bi].lo), sizeof pts[0], cmp_axis);
        { int mid = (b[bi].lo + b[bi].hi) / 2; b[nb].lo = mid; b[nb].hi = b[bi].hi; b[bi].hi = mid; nb++; }
    }
    for (i = 0; i < nb; i++) { double s[3] = { 0, 0, 0 }; int q; for (q = b[i].lo; q < b[i].hi; q++) for (j = 0; j < 3; j++) s[j] += pts[q][j];
        for (j = 0; j < 3; j++) pal[i][j] = (int)(s[j] / (b[i].hi - b[i].lo) + 0.5); }
    free(pts);
    return nb;
}

/* ---------------------------------------------------------------- the whole */
int shrink(const unsigned char *drw, int dw, int dh, const unsigned char *orig, int ow, int oh, const ShrinkOpt *o, unsigned char *out) {
    int W, H, i, x, y, c, cx0, cy0, cx1, cy1, cw, chh, bx0, by0, bx1, by1, x0, y0, x1, y1, bw, bh, w, h, ox, oy, it;
    unsigned char ch[3][256], *cut; double med[3]; size_t n = (size_t)dw * dh;
    lin_init();
    shrink_size(o, ow, oh, &W, &H);
    if (dw < 16 || dh < 16) return 0;
    /* 1. cut-out */
    for (c = 0; c < 3; c++) {
        int k = 0;
        for (y = 0; y < 8; y++) for (x = 0; x < 8; x++) {
            ch[c][k++] = drw[((size_t)y * dw + x) * 3 + c]; ch[c][k++] = drw[((size_t)y * dw + dw - 8 + x) * 3 + c];
            ch[c][k++] = drw[((size_t)(dh - 8 + y) * dw + x) * 3 + c]; ch[c][k++] = drw[((size_t)(dh - 8 + y) * dw + dw - 8 + x) * 3 + c];
        }
        qsort(ch[c], 256, 1, cmp_u8); med[c] = (ch[c][127] + ch[c][128]) / 2.0;
    }
    cut = xm(n * 4);
    for (i = 0; i < (int)n; i++) {
        int r = drw[i * 3], g = drw[i * 3 + 1], b = drw[i * 3 + 2];
        double d = sqrt((r - med[0]) * (r - med[0]) + (g - med[1]) * (g - med[1]) + (b - med[2]) * (b - med[2]));
        int bg = d < o->bg_dist || (o->magenta && r - g > 90 && b - g > 90);
        cut[i * 4] = (unsigned char)r; cut[i * 4 + 1] = (unsigned char)g; cut[i * 4 + 2] = (unsigned char)b; cut[i * 4 + 3] = bg ? 0 : 255;
    }
    if (o->flood) {     /* also cleared: near-background pixels joined to the border (looser distance, any pink tint) */
        int *q = xm(sizeof(int) * n), qh = 0, qt = 0; unsigned char *seen = calloc(n, 1);
        #define NEARBG(k) (sqrt((drw[(k) * 3] - med[0]) * (drw[(k) * 3] - med[0]) + (drw[(k) * 3 + 1] - med[1]) * (drw[(k) * 3 + 1] - med[1]) + \
                          (drw[(k) * 3 + 2] - med[2]) * (drw[(k) * 3 + 2] - med[2])) < o->bg_dist * 1.4 || \
                          (drw[(k) * 3] - drw[(k) * 3 + 1] > 40 && drw[(k) * 3 + 2] - drw[(k) * 3 + 1] > 40))
        for (x = 0; x < dw; x++) { int a = x, b = (dh - 1) * dw + x; if (NEARBG(a) && !seen[a]) { seen[a] = 1; q[qt++] = a; } if (NEARBG(b) && !seen[b]) { seen[b] = 1; q[qt++] = b; } }
        for (y = 0; y < dh; y++) { int a = y * dw, b = y * dw + dw - 1; if (NEARBG(a) && !seen[a]) { seen[a] = 1; q[qt++] = a; } if (NEARBG(b) && !seen[b]) { seen[b] = 1; q[qt++] = b; } }
        while (qh < qt) {
            int k = q[qh++], kx = k % dw, ky = k / dw, d2; static const int D[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
            cut[k * 4 + 3] = 0;
            for (d2 = 0; d2 < 4; d2++) { int xx = kx + D[d2][0], yy = ky + D[d2][1], kk = yy * dw + xx;
                if (xx >= 0 && yy >= 0 && xx < dw && yy < dh && !seen[kk] && NEARBG(kk)) { seen[kk] = 1; q[qt++] = kk; } }
        }
        #undef NEARBG
        free(q); free(seen);
    }
    if (!bbox_a(cut, dw, dh, 0, &cx0, &cy0, &cx1, &cy1) || !bbox_a(orig, ow, oh, 0, &bx0, &by0, &bx1, &by1)) { free(cut); return 0; }
    cw = cx1 - cx0; chh = cy1 - cy0;
    /* 3. fit: the original's box scaled to the sprite, the whole sprite, or stretched; the margin kept free */
    if (o->fit == FIT_BOX) { x0 = bx0 * W / ow; y0 = by0 * H / oh; x1 = bx1 * W / ow; y1 = by1 * H / oh; }
    else { x0 = 0; y0 = 0; x1 = W; y1 = H; }
    if (x0 < o->margin) x0 = o->margin; if (y0 < o->margin) y0 = o->margin;
    if (x1 > W - o->margin) x1 = W - o->margin; if (y1 > H - o->margin) y1 = H - o->margin;
    if (x1 <= x0) { x0 = 0; x1 = W; } if (y1 <= y0) { y0 = 0; y1 = H; }
    bw = x1 - x0; bh = y1 - y0;
    {
        double s = (double)bw / cw < (double)bh / chh ? (double)bw / cw : (double)bh / chh;
        float *pl[4], *rs[4]; unsigned char *crop, *sil = NULL, pal[256 * 3]; int lin, npal = 0;
        if (o->fit == FIT_STRETCH) { w = bw; h = bh; }
        else { w = (int)nearbyint(cw * s); if (w < 1) w = 1; h = (int)nearbyint(chh * s); if (h < 1) h = 1; }
        crop = xm((size_t)cw * chh * 4);
        for (y = 0; y < chh; y++) memcpy(crop + (size_t)y * cw * 4, cut + ((size_t)(cy0 + y) * dw + cx0) * 4, (size_t)cw * 4);
        free(cut);
        /* 2. before shrinking (needs the scale for the sharpening) */
        before(crop, cw, chh, o, (double)cw / w);
        for (c = 0; c < 4; c++) pl[c] = xm(sizeof(float) * (size_t)cw * chh);
        lin = o->linear && o->method != M_SUPERPIXEL;     /* superpixels measure in Lab from sRGB themselves */
        for (i = 0; i < cw * chh; i++) { double a = crop[i * 4 + 3]; for (c = 0; c < 3; c++) pl[c][i] = (float)((lin ? lin_t[crop[i * 4 + c]] * 255.0 : crop[i * 4 + c]) * (a / 255)); pl[3][i] = (float)a; }
        free(crop);
        /* 4. the method (majority and 2-step vote over the original's own colours) */
        {   int q;
            for (i = 0; i < ow * oh && npal < 256; i++) if (orig[i * 4 + 3] >= 128) {
                for (q = 0; q < npal && memcmp(pal + q * 3, orig + i * 4, 3); q++) ;
                if (q == npal) { memcpy(pal + npal * 3, orig + i * 4, 3); npal++; }
            }
        }
        if (o->method == M_TWOSTEP) {         /* Lanczos to 4x, then the commonest colour 4x -> 2x -> 1x */
            float *a4[4], *a2[4];
            for (c = 0; c < 4; c++) { a4[c] = resize_k(pl[c], cw, chh, w * 4, h * 4, k_lanczos, 3.0); a2[c] = xm(sizeof(float) * (size_t)w * h * 4); rs[c] = xm(sizeof(float) * (size_t)w * h); }
            m_cells(a4, w * 4, h * 4, w * 2, h * 2, M_MAJORITY, a2, pal, npal);
            m_cells(a2, w * 2, h * 2, w, h, M_MAJORITY, rs, pal, npal);
            for (c = 0; c < 4; c++) { free(a4[c]); free(a2[c]); }
        } else if (o->method <= M_GAUSS && o->method != M_NEAREST) {
            KernFn f = o->method == M_BOX ? k_box : o->method == M_BILINEAR ? k_tri : o->method == M_BICUBIC ? k_cubic : o->method == M_MITCHELL ? k_mitchell : o->method == M_GAUSS ? k_gauss : k_lanczos;
            double sup = o->method == M_BOX ? 0.5 : o->method == M_BILINEAR ? 1.0 : o->method == M_LANCZOS ? 3.0 : 2.0;
            for (c = 0; c < 4; c++) rs[c] = resize_k(pl[c], cw, chh, w, h, f, sup);
        } else {
            for (c = 0; c < 4; c++) rs[c] = xm(sizeof(float) * (size_t)w * h);
            if (o->method == M_DPID) m_dpid(pl, cw, chh, w, h, o->dpid_lambda / 100.0, rs);
            else if (o->method == M_SAID) m_said(pl, cw, chh, w, h, o, rs);
            else if (o->method == M_SUPERPIXEL) m_superpixel(pl, cw, chh, w, h, o, rs);
            else if (o->method == M_GRID) m_grid(pl, cw, chh, w, h, rs, pal, npal);
            else m_cells(pl, cw, chh, w, h, o->method, rs, pal, npal);
        }
        for (c = 0; c < 4; c++) free(pl[c]);
        if (o->silhouette) {      /* the shape on its own: half covered, or a thin line (covered more than both
                                     neighbours across it, at least a fifth covered) */
            unsigned char *keep = xm((size_t)w * h);
            for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
                float a = rs[3][(size_t)y * w + x];
                #define AT(xx, yy) ((xx) < 0 || (yy) < 0 || (xx) >= w || (yy) >= h ? 0.0f : rs[3][(size_t)(yy) * w + (xx)])
                int ridge = a >= 51 && ((a > AT(x - 1, y) && a > AT(x + 1, y)) || (a > AT(x, y - 1) && a > AT(x, y + 1)) ||
                                        (a > AT(x - 1, y - 1) && a > AT(x + 1, y + 1)) || (a > AT(x + 1, y - 1) && a > AT(x - 1, y + 1)));
                #undef AT
                keep[(size_t)y * w + x] = a >= 128 || ridge;
            }
            sil = keep;
        }
        memset(out, 0, (size_t)W * H * 4);
        ox = x0 + (int)floor((bw - w) / 2.0); oy = y0 + (int)floor((bh - h) / 2.0);
        for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
            size_t k = (size_t)y * w + x; float al = rs[3][k], m = al > 1.0f ? al : 1.0f; unsigned char px[4];
            for (c = 0; c < 3; c++) {
                float v = rs[c][k] * 255 / m; v = v < 0 ? 0 : v > 255 ? 255 : v;
                if (lin) { double l = v / 255.0; v = (float)(255.0 * (l <= 0.0031308 ? 12.92 * l : 1.055 * pow(l, 1 / 2.4) - 0.055)); }
                px[c] = (unsigned char)(int)(double)v;
            }
            px[3] = sil ? (sil[k] ? 255 : 0) : al >= o->alpha_cut ? 255 : 0;
            if (!px[3]) px[0] = px[1] = px[2] = 0;
            if (oy + y >= 0 && oy + y < H && ox + x >= 0 && ox + x < W) memcpy(out + ((size_t)(oy + y) * W + ox + x) * 4, px, 4);
        }
        for (c = 0; c < 4; c++) free(rs[c]);
        free(sil);
    }
    {
        unsigned char *on = xm((size_t)W * H), *m2 = xm((size_t)W * H);
        int (*pal)[3] = xm(sizeof(int[3]) * (size_t)(ow * oh * 3 + 64)), np = 0, ninner = 0, usepal, outl = o->outline == O_AUTO ? (orig_outlined(orig, ow, oh) ? O_BLACK4 : O_NONE) : o->outline;
        /* 5. pinholes */
        for (it = 0; it < o->pinholes; it++) {
            for (i = 0; i < W * H; i++) on[i] = out[i * 4 + 3] > 127;
            for (y = 0; y < H; y++) for (x = 0; x < W; x++) {
                int s[3] = { 0, 0, 0 }, k = 0, v, u, d;
                static const int D[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
                if (on[y * W + x] || nb4(on, W, H, x, y) < 3) continue;
                for (d = 0; d < 4; d++) {
                    v = y + D[d][0]; u = x + D[d][1];
                    if (v < 0 || v >= H || u < 0 || u >= W || !on[v * W + u]) continue;
                    for (c = 0; c < 3; c++) s[c] += out[((size_t)v * W + u) * 4 + c];
                    k++;
                }
                for (c = 0; c < 3; c++) out[((size_t)y * W + x) * 4 + c] = (unsigned char)(int)((double)s[c] / k);
                out[((size_t)y * W + x) * 4 + 3] = 255;
            }
        }
        /* 6. colours: the palette (the original's own, sorted as numpy's unique; black left out when 2+ others) */
        for (i = 0; i < ow * oh; i++) if (orig[i * 4 + 3] >= 128) {
            int r = orig[i * 4], g = orig[i * 4 + 1], b = orig[i * 4 + 2], j;
            for (j = 0; j < np; j++) if (pal[j][0] == r && pal[j][1] == g && pal[j][2] == b) break;
            if (j == np) { pal[np][0] = r; pal[np][1] = g; pal[np][2] = b; np++; }
        }
        qsort(pal, (size_t)np, sizeof pal[0], cmp_rgb);
        for (i = 0; i < np; i++) ninner += pal[i][0] + pal[i][1] + pal[i][2] > 0;
        if (ninner >= 2) { int j = 0; for (i = 0; i < np; i++) if (pal[i][0] + pal[i][1] + pal[i][2] > 0) { pal[j][0] = pal[i][0]; pal[j][1] = pal[i][1]; pal[j][2] = pal[i][2]; j++; } np = j; }
        if (o->colours == C_SHADES) {         /* each colour also at 70% and 130% */
            int base = np, j, q;
            for (i = 0; i < base; i++) for (q = 0; q < 2; q++) {
                double f = q ? 1.3 : 0.7; int r = clamp8(pal[i][0] * f), g = clamp8(pal[i][1] * f), b = clamp8(pal[i][2] * f);
                for (j = 0; j < np; j++) if (pal[j][0] == r && pal[j][1] == g && pal[j][2] == b) break;
                if (j == np) { pal[np][0] = r; pal[np][1] = g; pal[np][2] = b; np++; }
            }
        }
        if (o->colours == C_KMEANS) np = kmeans(out, W, H, o->ncol, pal, o->dist, o->kmpp);
        if (o->colours == C_WU) np = wu(out, W, H, o->ncol, pal);
        if (o->colours == C_PALETTE) { int gn; sh_lock(); gn = GPAL_N;      /* copied whole under the lock: never half old, half new */
            for (np = 0; np < gn; np++) { pal[np][0] = GPAL[np * 3]; pal[np][1] = GPAL[np * 3 + 1]; pal[np][2] = GPAL[np * 3 + 2]; } sh_unlock(); }
        if (o->colours == C_MEDIANCUT) np = mediancut(out, W, H, o->ncol, pal);
        usepal = o->colours == C_ORIGINAL || o->colours == C_SHADES || o->colours == C_KMEANS || o->colours == C_MEDIANCUT || o->colours == C_WU || (o->colours == C_PALETTE && np > 0);
        if (np && usepal && o->regions && !o->dither) {
            /* by region: patches of like colour (Lab within 12 of the patch's first pixel, 4-connected) each take the
               palette colour most of their pixels are nearest to (Lab) */
            int *rg = xm(sizeof(int) * (size_t)W * H), *q = xm(sizeof(int) * (size_t)W * H), *vote = xm(sizeof(int) * (size_t)np), nr = 0, j;
            double (*L)[3] = xm(sizeof(double[3]) * (size_t)W * H), (*plab)[3] = xm(sizeof(double[3]) * (size_t)np);
            for (j = 0; j < np; j++) lab(pal[j][0], pal[j][1], pal[j][2], plab[j]);
            for (i = 0; i < W * H; i++) {
                unsigned char *p = out + (size_t)i * 4; rg[i] = -1;
                if (p[3] < 128) continue;
                if (o->black && p[0] < o->black && p[1] < o->black && p[2] < o->black) { p[0] = p[1] = p[2] = 0; rg[i] = -2; continue; }
                lab(p[0], p[1], p[2], L[i]);
            }
            for (i = 0; i < W * H; i++) if (rg[i] == -1 && out[(size_t)i * 4 + 3] >= 128) {
                int qh = 0, qt = 0, best = 0, k2, d2; static const int D[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
                memset(vote, 0, sizeof(int) * (size_t)np);
                rg[i] = nr; q[qt++] = i;
                while (qh < qt) {
                    int k = q[qh++], kx = k % W, ky = k / W; double bd = 1e300;
                    for (j = 0; j < np; j++) { double e = (L[k][0] - plab[j][0]) * (L[k][0] - plab[j][0]) + (L[k][1] - plab[j][1]) * (L[k][1] - plab[j][1]) + (L[k][2] - plab[j][2]) * (L[k][2] - plab[j][2]); if (e < bd) { bd = e; best = j; } }
                    vote[best]++;
                    for (d2 = 0; d2 < 4; d2++) { int xx = kx + D[d2][0], yy = ky + D[d2][1], kk = yy * W + xx; double e;
                        if (xx < 0 || yy < 0 || xx >= W || yy >= H || rg[kk] != -1 || out[(size_t)kk * 4 + 3] < 128) continue;
                        e = (L[kk][0] - L[i][0]) * (L[kk][0] - L[i][0]) + (L[kk][1] - L[i][1]) * (L[kk][1] - L[i][1]) + (L[kk][2] - L[i][2]) * (L[kk][2] - L[i][2]);
                        if (e < 144) { rg[kk] = nr; q[qt++] = kk; } }
                }
                for (j = 1, best = 0; j < np; j++) if (vote[j] > vote[best]) best = j;
                for (k2 = 0; k2 < qt; k2++) { unsigned char *p = out + (size_t)q[k2] * 4; p[0] = (unsigned char)pal[best][0]; p[1] = (unsigned char)pal[best][1]; p[2] = (unsigned char)pal[best][2]; }
                nr++;
            }
            free(rg); free(q); free(vote); free(L); free(plab);
        } else if (np) {
            double (*pl)[3] = xm(sizeof(double[3]) * (size_t)np), (*plab)[3] = xm(sizeof(double[3]) * (size_t)np);
            float *err = xm(sizeof(float) * 3 * (size_t)W * H); static const int bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
            int dith = usepal ? o->dither : DI_NONE;
            memset(err, 0, sizeof(float) * 3 * (size_t)W * H);
            for (i = 0; i < np; i++) { for (c = 0; c < 3; c++) pl[i][c] = pal[i][c]; lab(pl[i][0], pl[i][1], pl[i][2], plab[i]); }
            for (y = 0; y < H; y++) for (x = 0; x < W; x++) {
                int k = y * W + x, best = 0, j; unsigned char *p = out + (size_t)k * 4; double v[3], bd = -1, vl[3] = { 0, 0, 0 }, e[3];
                if (p[3] < 128) continue;
                for (c = 0; c < 3; c++) v[c] = p[c] + err[k * 3 + c];
                if (dith == DI_BAYER) for (c = 0; c < 3; c++) v[c] += (bayer[(y & 3) * 4 + (x & 3)] / 16.0 - 0.5) * o->dither_amt * 0.64;
                if (o->dist == D_LAB) lab(v[0], v[1], v[2], vl);
                for (j = 0; j < np; j++) {
                    double d = o->dist == D_RGB && !dith ? (double)(long)((p[0] - pal[j][0]) * (p[0] - pal[j][0]) + (p[1] - pal[j][1]) * (p[1] - pal[j][1]) + (p[2] - pal[j][2]) * (p[2] - pal[j][2]))
                                                         : cdist(o->dist, v, pl[j], vl, plab[j]);
                    if (bd < 0 || d < bd) { bd = d; best = j; }
                }
                if (o->black && v[0] < o->black && v[1] < o->black && v[2] < o->black) { p[0] = p[1] = p[2] = 0; }
                else if (usepal || (o->colours == C_OWN && o->pink && p[0] - p[1] > o->pink && p[2] - p[1] > o->pink)) {
                    p[0] = (unsigned char)pal[best][0]; p[1] = (unsigned char)pal[best][1]; p[2] = (unsigned char)pal[best][2];
                }
                if (dith == DI_FLOYD || dith == DI_ATKINSON) {
                    double s = o->dither_amt / 100.0;
                    for (c = 0; c < 3; c++) e[c] = (v[c] - p[c]) * s;
                    #define ADD(dx, dy, f) do { int xx = x + (dx), yy = y + (dy); if (xx >= 0 && xx < W && yy < H) for (c = 0; c < 3; c++) err[(yy * W + xx) * 3 + c] += (float)(e[c] * (f)); } while (0)
                    if (dith == DI_FLOYD) { ADD(1, 0, 7 / 16.0); ADD(-1, 1, 3 / 16.0); ADD(0, 1, 5 / 16.0); ADD(1, 1, 1 / 16.0); }
                    else { ADD(1, 0, 1 / 8.0); ADD(2, 0, 1 / 8.0); ADD(-1, 1, 1 / 8.0); ADD(0, 1, 1 / 8.0); ADD(1, 1, 1 / 8.0); ADD(0, 2, 1 / 8.0); }
                    #undef ADD
                }
            }
            free(pl); free(plab); free(err);
        } else if (o->black) for (i = 0; i < W * H; i++) { unsigned char *p = out + (size_t)i * 4; if (p[3] >= 128 && p[0] < o->black && p[1] < o->black && p[2] < o->black) p[0] = p[1] = p[2] = 0; }
        /* 7. the model's own dark rim peeled */
        for (it = 0; it < o->peel; it++) {
            int any = 0;
            for (y = 0; y < H; y++) for (x = 0; x < W; x++) { int k = y * W + x; on[k] = out[k * 4 + 3] > 127; m2[k] = !on[k] || x == 0 || y == 0 || x == W - 1 || y == H - 1; }
            for (y = 0; y < H; y++) for (x = 0; x < W; x++) {
                int k = y * W + x; unsigned char *p = out + (size_t)k * 4;
                if (on[k] && nb4(m2, W, H, x, y) > 0 && !p[0] && !p[1] && !p[2]) { p[0] = p[1] = p[2] = p[3] = 0; any = 1; }
            }
            if (!any) break;
        }
        /* 8. stray edge pixels (B7; a rule from a Russian write-up of model sprites, Habr 2026): a covered pixel with 6 or
           more of its 8 neighbours empty (or outside) is a speck on the edge, not part of the shape: it goes */
        if (o->edgestray) {
            unsigned char *cp = xm((size_t)W * H * 4); int gone = 0; memcpy(cp, out, (size_t)W * H * 4);
            for (y = 0; y < H; y++) for (x = 0; x < W; x++) {
                int u, v, empty = 0;
                if (cp[((size_t)y * W + x) * 4 + 3] < 128) continue;
                for (v = -1; v <= 1; v++) for (u = -1; u <= 1; u++) { int xx = x + u, yy = y + v;
                    if (u || v) empty += xx < 0 || yy < 0 || xx >= W || yy >= H || cp[((size_t)yy * W + xx) * 4 + 3] < 128; }
                if (empty >= 6) { memset(out + ((size_t)y * W + x) * 4, 0, 4); gone++; }
            }
            free(cp); (void)gone;
        }
        /* 8a. lone pixels: one that shares its colour with none of its 8 neighbours takes the commonest among them (3+) */
        if (o->orphans) {
            unsigned char *cp = xm((size_t)W * H * 4); memcpy(cp, out, (size_t)W * H * 4);
            for (y = 0; y < H; y++) for (x = 0; x < W; x++) {
                const unsigned char *p = cp + ((size_t)y * W + x) * 4; int u, v, same = 0, bestn = 0, bi = -1, cand[8], nc = 0, q;
                if (p[3] < 128) continue;
                for (v = -1; v <= 1; v++) for (u = -1; u <= 1; u++) {
                    int xx = x + u, yy = y + v; const unsigned char *t;
                    if ((!u && !v) || xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
                    t = cp + ((size_t)yy * W + xx) * 4; if (t[3] < 128) continue;
                    if (!memcmp(t, p, 3)) same++; else cand[nc++] = yy * W + xx;
                }
                if (same) continue;
                for (q = 0; q < nc; q++) { int r = 0, s2; for (s2 = 0; s2 < nc; s2++) r += !memcmp(cp + (size_t)cand[q] * 4, cp + (size_t)cand[s2] * 4, 3); if (r > bestn) { bestn = r; bi = cand[q]; } }
                if (bi >= 0 && bestn >= 3) memcpy(out + ((size_t)y * W + x) * 4, cp + (size_t)bi * 4, 3);
            }
            free(cp);
        }
        /* 8c. without an outline, a smooth edge if wanted: whole pixels only. A clear pixel with 5+ of its 8
           neighbours filled is filled (the commonest of them); a filled edge pixel with 2 or fewer is cleared */
        if (outl == O_NONE && o->edge) for (it = 0; it < 2; it++) {
            unsigned char *cp = xm((size_t)W * H * 4); memcpy(cp, out, (size_t)W * H * 4);
            for (i = 0; i < W * H; i++) on[i] = cp[i * 4 + 3] > 127;
            for (y = 0; y < H; y++) for (x = 0; x < W; x++) {
                int k = y * W + x, u, v, cnt = 0, cand[8], nc = 0, q, bestn = 0, bi = -1;
                for (v = -1; v <= 1; v++) for (u = -1; u <= 1; u++) {
                    int xx = x + u, yy = y + v;
                    if ((!u && !v) || xx < 0 || yy < 0 || xx >= W || yy >= H || !on[yy * W + xx]) continue;
                    cnt++; cand[nc++] = yy * W + xx;
                }
                if (!on[k] && cnt >= 5) {
                    for (q = 0; q < nc; q++) { int r = 0, s2; for (s2 = 0; s2 < nc; s2++) r += !memcmp(cp + (size_t)cand[q] * 4, cp + (size_t)cand[s2] * 4, 3); if (r > bestn) { bestn = r; bi = cand[q]; } }
                    memcpy(out + (size_t)k * 4, cp + (size_t)bi * 4, 4);
                } else if (on[k] && cnt <= 2) memset(out + (size_t)k * 4, 0, 4);
            }
            free(cp);
        }
        /* 9. the outline, othick rings (the rings after the first take the colour of the ring inside them) */
        if (outl != O_NONE) for (int ring = 0; ring < (o->othick < 1 ? 1 : o->othick); ring++) {
            unsigned char *cp = xm((size_t)W * H * 4); memcpy(cp, out, (size_t)W * H * 4);
            for (i = 0; i < W * H; i++) on[i] = out[i * 4 + 3] > 127;
            for (y = 0; y < H; y++) for (x = 0; x < W; x++) {
                int k = y * W + x, u, v, touch = 0; unsigned char *p = out + (size_t)k * 4;
                if (on[k]) continue;
                touch = nb4(on, W, H, x, y) > 0;
                if (!touch && outl == O_BLACK8) for (v = -1; v <= 1; v += 2) for (u = -1; u <= 1; u += 2) { int xx = x + u, yy = y + v; if (xx >= 0 && yy >= 0 && xx < W && yy < H && on[yy * W + xx]) touch = 1; }
                if (!touch) continue;
                p[0] = p[1] = p[2] = 0; p[3] = 255;
                if (ring > 0) {        /* an outer ring: the colour of the ring pixel beside it */
                    static const int D[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } }; int d;
                    for (d = 0; d < 4; d++) { int xx = x + D[d][1], yy = y + D[d][0];
                        if (xx >= 0 && yy >= 0 && xx < W && yy < H && on[yy * W + xx]) { memcpy(p, cp + ((size_t)yy * W + xx) * 4, 3); break; } }
                } else if (outl == O_SELECTIVE) {   /* a darker shade of the commonest coloured neighbour */
                    static const int D[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } }; int d, bestn = 0, bi = -1, q, cand[4], nc = 0;
                    for (d = 0; d < 4; d++) { int xx = x + D[d][1], yy = y + D[d][0]; const unsigned char *t;
                        if (xx < 0 || yy < 0 || xx >= W || yy >= H || !on[yy * W + xx]) continue;
                        t = cp + ((size_t)yy * W + xx) * 4; if (t[0] | t[1] | t[2]) cand[nc++] = yy * W + xx; }
                    for (q = 0; q < nc; q++) { int r = 0, s2; for (s2 = 0; s2 < nc; s2++) r += !memcmp(cp + (size_t)cand[q] * 4, cp + (size_t)cand[s2] * 4, 3); if (r > bestn) { bestn = r; bi = cand[q]; } }
                    if (bi >= 0) for (c = 0; c < 3; c++) p[c] = (unsigned char)(cp[(size_t)bi * 4 + c] * 45 / 100);
                }
            }
            free(cp);
        }
        /* 8b. doubled corners: a black pixel whose two black 4-neighbours make an L, with no other colour beside it */
        if (o->jaggies) for (it = 0; it < 2; it++) {
            int any = 0;
            for (i = 0; i < W * H; i++) on[i] = out[i * 4 + 3] > 127 && !(out[i * 4] | out[i * 4 + 1] | out[i * 4 + 2]);   /* black */
            for (y = 0; y < H; y++) for (x = 0; x < W; x++) {
                int k = y * W + x, l = x > 0 && on[k - 1], r = x < W - 1 && on[k + 1], u = y > 0 && on[k - W], d = y < H - 1 && on[k + W], q, col = 0;
                static const int D[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
                if (!on[k] || l + r + u + d != 2 || (l && r) || (u && d)) continue;
                for (q = 0; q < 4; q++) { int xx = x + D[q][1], yy = y + D[q][0]; if (xx >= 0 && yy >= 0 && xx < W && yy < H && out[(yy * W + xx) * 4 + 3] > 127 && !on[yy * W + xx]) col = 1; }
                if (col) continue;
                { int dx = r ? 1 : -1, dy = d ? 1 : -1, xx = x + dx, yy = y + dy;    /* the L's inside corner must not be black too (a 2 x 2 block) */
                  if (xx >= 0 && yy >= 0 && xx < W && yy < H && on[yy * W + xx]) continue; }
                out[k * 4] = out[k * 4 + 1] = out[k * 4 + 2] = out[k * 4 + 3] = 0; on[k] = 0; any = 1;
            }
            if (!any) break;
        }
        /* 8d. a black pixel biting into the body (colour above and below, colour on one side, black on the other) takes
           the colour beside it; the same turned on its side */
        if (o->notch) {
            unsigned char *cp = xm((size_t)W * H * 4); memcpy(cp, out, (size_t)W * H * 4);
            #define BK(xx, yy) (cp[((yy) * W + (xx)) * 4 + 3] > 127 && !(cp[((yy) * W + (xx)) * 4] | cp[((yy) * W + (xx)) * 4 + 1] | cp[((yy) * W + (xx)) * 4 + 2]))
            #define CO(xx, yy) (cp[((yy) * W + (xx)) * 4 + 3] > 127 && !BK(xx, yy))
            for (y = 1; y < H - 1; y++) for (x = 1; x < W - 1; x++) {
                if (!BK(x, y)) continue;
                if (CO(x, y - 1) && CO(x, y + 1) && CO(x - 1, y) != CO(x + 1, y) && (BK(x - 1, y) || BK(x + 1, y)))
                    memcpy(out + ((size_t)y * W + x) * 4, cp + ((size_t)y * W + (CO(x + 1, y) ? x + 1 : x - 1)) * 4, 4);
                else if (CO(x - 1, y) && CO(x + 1, y) && CO(x, y - 1) != CO(x, y + 1) && (BK(x, y - 1) || BK(x, y + 1)))
                    memcpy(out + ((size_t)y * W + x) * 4, cp + ((size_t)(CO(x, y + 1) ? y + 1 : y - 1) * W + x) * 4, 4);
            }
            free(cp);
        }
        /* 8e. a 2 x 2 block of black: the one of its pixels with no clear neighbour and the most coloured ones takes the
           colour of one of them (the outline stays one pixel wide) */
        if (o->blocks) {
            unsigned char *cp = xm((size_t)W * H * 4); memcpy(cp, out, (size_t)W * H * 4);
            for (y = 0; y < H - 1; y++) for (x = 0; x < W - 1; x++) {
                int k, best = -1, bn = 0, bsrc = -1;
                if (!(BK(x, y) && BK(x + 1, y) && BK(x, y + 1) && BK(x + 1, y + 1))) continue;
                for (k = 0; k < 4; k++) {
                    int xx = x + (k & 1), yy = y + (k >> 1), d2, ncol = 0, clear = 0, src = -1; static const int D[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
                    for (d2 = 0; d2 < 4; d2++) { int ax = xx + D[d2][0], ay = yy + D[d2][1];
                        if (ax < 0 || ay < 0 || ax >= W || ay >= H || cp[(ay * W + ax) * 4 + 3] <= 127) { clear = 1; continue; }
                        if (CO(ax, ay)) { ncol++; src = ay * W + ax; } }
                    if (!clear && ncol > bn) { bn = ncol; best = yy * W + xx; bsrc = src; }
                }
                if (best >= 0) { memcpy(out + (size_t)best * 4, cp + (size_t)bsrc * 4, 4); memcpy(cp + (size_t)best * 4, cp + (size_t)bsrc * 4, 4); }
            }
            free(cp);
            #undef BK
            #undef CO
        }
        free(on); free(m2); free(pal);
    }
    return 1;
}
/* the original has an outline: black fences its colours in (few colours face the background, black touches colour) */
int orig_outlined(const unsigned char *rgba, int w, int h) {
    int x, y, nb = 0, nc = 0, ex = 0, fe = 0, u, v;
    for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
        const unsigned char *p = rgba + ((size_t)y * w + x) * 4; int op = p[3] > 0, bk = op && !(p[0] | p[1] | p[2]), bg4 = 0, col8 = 0;
        if (!op) continue;
        for (v = -1; v <= 1; v++) for (u = -1; u <= 1; u++) {
            int xx = x + u, yy = y + v; const unsigned char *q;
            if ((!u && !v) || xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
            q = rgba + ((size_t)yy * w + xx) * 4;
            if ((!u || !v) && !q[3]) bg4 = 1;
            if (q[3] && (q[0] | q[1] | q[2])) col8 = 1;
        }
        if (bk) { nb++; fe += col8; } else { nc++; ex += bg4; }
    }
    return nb > 0 && (double)ex / (nc > 1 ? nc : 1) < 0.15 && (double)fe / (nb > 1 ? nb : 1) > 0.9;
}
