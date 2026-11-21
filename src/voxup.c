/* voxup: test of the voxel route for a bigger sprite. Command line only.
   The four stages of the pasted spec, in plain C:
     1  the enlarged sprite comes in ready-made (an enlarged picture, any square size)
     2  a 3D shape DIM x DIM x DIM as a distance field (below 0 inside, 0 on the surface, above 0 outside):
        puffed from the edge, relief from brightness, or a mix
     3  smoothing of the field near the surface: bilateral filter; optional fill (close) and spike removal (open)
     4  drawn back at OUT x OUT by marching rays through the field: smooth normals, one light, ambient occlusion,
        several rays a pixel, soft or hard edge, free colours / the sprite's own colours / own colours + shades
   Left out of this round on purpose: Taubin smoothing (made for meshes), cellular automata, mesh export.
   Files are raw: int32 width, int32 height, then RGBA bytes (convert PNG to raw and back with any tool).
   Errors and timings go to out\voxup.log (vlog) and errors also to the console. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "vlog.h"

#define DARK 0.06f                      /* at or below this brightness a pixel is outline: never shaded */
#define PI 3.14159265358979

typedef struct {
    int dim, out, alpha, rays, iters;
    float body;                         /* rounding of the coloured body after the outline is taken off, in sprite pixels (0 = off) */
    int solid; float inset;             /* solid: with contour, the whole sprite WITH its outline is the one shape that is smoothed; then it is drawn in by `inset` sprite pixels (1 = the old outline width) and the colours are put back by place and depth */
    int contour; const char *src;       /* contour: the outer shape redrawn from the ORIGINAL sprite (src) as smooth lines, this many smoothing passes (0 = off) */
    int extrude;                        /* 0 puff, 1 brightness, 2 mix */
    int smooth;                         /* 0 none, 1 bilateral, 2 Taubin, 3 both */
    int titers; float lambda, mu, curves, shape;   /* shape: rounding of the outer shape only, in sprite pixels (0 = off); the colours inside are not touched */
      /* Taubin passes and its two steps; curves: rounding of the enlarged picture, in sprite pixels (0 = off) */
    int edge;                           /* 0 soft alpha, 1 hard alpha */
    int palette;                        /* 0 free, 1 own colours, 2 own colours + shades */
    int outline;                        /* 0 keep, 1 thin to one output pixel, 2 take it off and draw a 1-pixel line round the finished picture */
    float thick, bevel, mix, reach, bs, br, close_r, open_r, az, el, light, ao, ambient, tiltx, tilty, soft, cutoff, rim;
} Par;

static double now_ms(void) { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return 1000.0 * (double)c.QuadPart / (double)f.QuadPart; }
static float lum3(const unsigned char *p) { return (0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]) / 255.0f; }
static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

/* ---- distance to the nearest marked pixel (Felzenszwalb-Huttenlocher), in pixels, centre to centre ---- */
static void dt1(const float *f, int n, float *d, int *v, float *z) {
    int k = 0, q; v[0] = 0; z[0] = -1e20f; z[1] = 1e20f;
    for (q = 1; q < n; q++) {
        float s;
        for (;;) { s = ((f[q] + (float)q * q) - (f[v[k]] + (float)v[k] * v[k])) / (2.0f * q - 2.0f * v[k]); if (s <= z[k] && k > 0) k--; else break; }
        if (s <= z[k]) { v[k] = q; z[k] = -1e20f; z[k + 1] = 1e20f; } else { k++; v[k] = q; z[k] = s; z[k + 1] = 1e20f; }
    }
    for (k = 0, q = 0; q < n; q++) { while (z[k + 1] < q) k++; d[q] = (float)(q - v[k]) * (q - v[k]) + f[v[k]]; }
}
/* out[i] = distance from pixel i to the nearest pixel with set[i] != 0 (1e9 when none is set). 0 on failure. */
static int dist2d(const unsigned char *set, int w, int h, float *out) {
    int n = w > h ? w : h, x, y, ok = 0;
    float *f = malloc(sizeof(float) * n), *d = malloc(sizeof(float) * n), *z = malloc(sizeof(float) * (n + 1)); int *v = malloc(sizeof(int) * n);
    if (!f || !d || !z || !v) { vlog(VLOG_ERR, "memory", "out of memory in the distance pass (%d x %d)", w, h); goto done; }
    for (x = 0; x < w; x++) {
        for (y = 0; y < h; y++) f[y] = set[y * w + x] ? 0.0f : 1e12f;
        dt1(f, h, d, v, z);
        for (y = 0; y < h; y++) out[y * w + x] = d[y];
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) f[x] = out[y * w + x];
        dt1(f, w, d, v, z);
        for (x = 0; x < w; x++) out[y * w + x] = d[x] >= 1e11f ? 1e9f : sqrtf(d[x]);
    }
    ok = 1;
done:
    free(f); free(d); free(z); free(v); return ok;
}

/* ---- threads: run fn(lo, hi, ctx) over 0..n in slabs ---- */
typedef void (*SlabFn)(int lo, int hi, void *ctx);
typedef struct { SlabFn fn; int lo, hi; void *ctx; } Job;
static DWORD WINAPI job_run(LPVOID p) { Job *j = p; j->fn(j->lo, j->hi, j->ctx); return 0; }
static void par_for(int n, SlabFn fn, void *ctx) {
    SYSTEM_INFO si; Job jobs[64]; HANDLE th[64]; int t, nt, started = 0;
    GetSystemInfo(&si); nt = (int)si.dwNumberOfProcessors; if (nt > 64) nt = 64; if (nt > n) nt = n; if (nt < 1) nt = 1;
    for (t = 0; t < nt; t++) {
        jobs[t].fn = fn; jobs[t].ctx = ctx; jobs[t].lo = (int)((long long)n * t / nt); jobs[t].hi = (int)((long long)n * (t + 1) / nt);
        th[started] = CreateThread(NULL, 0, job_run, &jobs[t], 0, NULL);
        if (th[started]) started++;
        else { vlog(VLOG_WARN, "threads", "could not start thread %d (%s): that part runs on the main thread", t, vlog_errno()); fn(jobs[t].lo, jobs[t].hi, ctx); }
    }
    if (started) WaitForMultipleObjects((DWORD)started, th, TRUE, INFINITE);
    for (t = 0; t < started; t++) CloseHandle(th[t]);
}

/* ---- stage 3: filters on the field, only near the surface ---- */
typedef struct { const float *src; float *dst; int n, mode, rad; float ss, sr, band; } Filt;      /* mode 0 bilateral, 1 min (grow), 2 max (shrink), 3 one Taubin step: towards the 6 neighbours by ss (ss below 0 = away) */
static void filt_slab(int lo, int hi, void *ctx) {
    Filt *f = ctx; int n = f->n, r = f->rad, x, y, z, a, b, c;
    float is2 = 1.0f / (2.0f * f->ss * f->ss), ir2 = 1.0f / (2.0f * f->sr * f->sr), r2 = (f->ss + 0.5f) * (f->ss + 0.5f);
    for (z = lo; z < hi; z++) for (y = 0; y < n; y++) for (x = 0; x < n; x++) {
        size_t i = ((size_t)z * n + y) * n + x; float v0 = f->src[i], acc = 0, wsum = 0, best = v0;
        if (fabsf(v0) > f->band) { f->dst[i] = v0; continue; }
        if (f->mode == 3) {
            size_t nn = (size_t)n * n;
            float av = (f->src[x > 0 ? i - 1 : i] + f->src[x < n - 1 ? i + 1 : i] + f->src[y > 0 ? i - n : i] + f->src[y < n - 1 ? i + n : i] + f->src[z > 0 ? i - nn : i] + f->src[z < n - 1 ? i + nn : i]) / 6.0f;
            f->dst[i] = v0 + f->ss * (av - v0); continue;
        }
        for (c = -r; c <= r; c++) { int zz = z + c; if (zz < 0 || zz >= n) continue;
            for (b = -r; b <= r; b++) { int yy = y + b; if (yy < 0 || yy >= n) continue;
                for (a = -r; a <= r; a++) { int xx = x + a; float v, d2 = (float)(a * a + b * b + c * c);
                    if (xx < 0 || xx >= n) continue;
                    v = f->src[((size_t)zz * n + yy) * n + xx];
                    if (f->mode == 0) { float w = expf(-d2 * is2 - (v - v0) * (v - v0) * ir2); acc += w * v; wsum += w; }
                    else if (d2 <= r2) { if (f->mode == 1 ? v < best : v > best) best = v; }
                } } }
        f->dst[i] = f->mode == 0 ? acc / wsum : best;
    }
}
static void filter(float **a, float **b, int n, int mode, float ss, float sr) {
    Filt f; float *t;
    f.src = *a; f.dst = *b; f.n = n; f.mode = mode; f.ss = ss; f.sr = sr; f.rad = mode == 3 ? 1 : mode == 0 ? (int)ceilf(2.0f * ss) : (int)ceilf(ss); f.band = (float)f.rad + 4.0f;
    if (f.rad < 1) return;
    par_for(n, filt_slab, &f);
    t = *a; *a = *b; *b = t;
}

/* ---- field lookup between voxels (trilinear); outside the box the distance grows ---- */
static float fld(const float *v, int n, float x, float y, float z) {
    float ex = 0, m = (float)(n - 1), fx, fy, fz; int x0, y0, z0; const float *p;
    if (x < 0) { ex += -x; x = 0; } else if (x > m) { ex += x - m; x = m; }
    if (y < 0) { ex += -y; y = 0; } else if (y > m) { ex += y - m; y = m; }
    if (z < 0) { ex += -z; z = 0; } else if (z > m) { ex += z - m; z = m; }
    x0 = (int)x; y0 = (int)y; z0 = (int)z; if (x0 > n - 2) x0 = n - 2; if (y0 > n - 2) y0 = n - 2; if (z0 > n - 2) z0 = n - 2;
    fx = x - x0; fy = y - y0; fz = z - z0; p = v + ((size_t)z0 * n + y0) * n + x0;
#define L(a, b, t) ((a) + ((b) - (a)) * (t))
    { float c00 = L(p[0], p[1], fx), c10 = L(p[n], p[n + 1], fx), c01 = L(p[(size_t)n * n], p[(size_t)n * n + 1], fx), c11 = L(p[(size_t)n * n + n], p[(size_t)n * n + n + 1], fx);
      float r = L(L(c00, c10, fy), L(c01, c11, fy), fz); return r > 0 ? r + ex : (ex > 0 ? ex : r); }
#undef L
}

/* ---- own colours + shades: the palette, a half-way shade between neighbours of one material, one lighter and one darker end ---- */
typedef struct { float c[3]; } Col;
static float clum(const float *c) { return 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2]; }
static float chroma_d(const float *a, const float *b) { float la = clum(a), lb = clum(b), s = 0; int k; for (k = 0; k < 3; k++) { float d = (a[k] - la) - (b[k] - lb); s += d * d; } return sqrtf(s); }
static int pal_extend(Col *pal, int n, int max) {
    int i, j, k, base = n;
    for (i = 0; i < base && n + 2 < max; i++) {
        int up = -1, down = 0; float li = clum(pal[i].c);
        if (li <= DARK) continue;
        for (j = 0; j < base; j++) { float lj = clum(pal[j].c);
            if (j == i || lj <= DARK || chroma_d(pal[i].c, pal[j].c) > 0.22f) continue;
            if (lj > li && (up < 0 || lj < clum(pal[up].c))) up = j;
            if (lj < li) down = 1; }
        if (up >= 0) { for (k = 0; k < 3; k++) pal[n].c[k] = (pal[i].c[k] + pal[up].c[k]) * 0.5f; n++; }
        else { for (k = 0; k < 3; k++) pal[n].c[k] = pal[i].c[k] + (1.0f - pal[i].c[k]) * 0.38f; n++; }
        if (!down) { for (k = 0; k < 3; k++) pal[n].c[k] = pal[i].c[k] * 0.72f; n++; }
    }
    return n;
}
/* the palette colour for a shaded colour `want` whose unshaded colour was `base`: same material first, then brightness */
static int pal_pick(const Col *pal, int n, const float *want, const float *base) {
    int i, best = 0; float bc = 1e9f, lw = clum(want);
    for (i = 0; i < n; i++) { float l = clum(pal[i].c), c = fabsf(l - lw) + 1.4f * chroma_d(pal[i].c, base); if (l <= DARK) c += 0.25f; if (c < bc) { bc = c; best = i; } }
    return best;
}

static unsigned char *read_raw(const char *path, int *w, int *h) {
    FILE *f = fopen(path, "rb"); int hd[2]; unsigned char *p; size_t n;
    if (!f) { vlog(VLOG_ERR, "open", "%s: cannot open: %s", path, vlog_errno()); return NULL; }
    if (fread(hd, 4, 2, f) != 2 || hd[0] < 1 || hd[1] < 1 || hd[0] > 4096 || hd[1] > 4096) { vlog(VLOG_ERR, "open", "%s: not a raw picture (bad size header)", path); fclose(f); return NULL; }
    n = (size_t)hd[0] * hd[1] * 4; p = malloc(n);
    if (!p) { vlog(VLOG_ERR, "memory", "%s: out of memory for %d x %d", path, hd[0], hd[1]); fclose(f); return NULL; }
    if (fread(p, 1, n, f) != n) { vlog(VLOG_ERR, "open", "%s: the file is shorter than its %d x %d header says", path, hd[0], hd[1]); free(p); fclose(f); return NULL; }
    fclose(f); *w = hd[0]; *h = hd[1]; return p;
}
static int write_raw(const char *path, const unsigned char *p, int w, int h) {
    FILE *f = fopen(path, "wb"); int hd[2]; int ok;
    if (!f) { vlog(VLOG_ERR, "save", "could not write %s: %s", path, vlog_errno()); return 0; }
    hd[0] = w; hd[1] = h; ok = fwrite(hd, 4, 2, f) == 2 && fwrite(p, 1, (size_t)w * h * 4, f) == (size_t)w * h * 4;
    if (fclose(f) != 0) ok = 0;
    if (!ok) vlog(VLOG_ERR, "save", "could not write all of %s: %s", path, vlog_errno());
    return ok;
}

/* An edge rounded without touching the colours inside: what is opaque is blurred by sg pixels and cut again at half.
   Small dents fill, small bumps go, corners round. New edge pixels take the colour of the nearest old one. 0 on failure. */
static int round_mask(unsigned char *img, int W, int H, int alpha, float sg, const char *what) {
    float ks = 0; int rad = (int)ceilf(2.5f * sg), k, x, y, added = 0, cutn = 0;
    float *tmp = malloc(sizeof(float) * W * H), *ind = malloc(sizeof(float) * W * H), *ker = malloc(sizeof(float) * (2 * rad + 1)); unsigned char *src = malloc((size_t)W * H * 4);
    if (!tmp || !ind || !ker || !src) { vlog(VLOG_ERR, "memory", "out of memory rounding the %s (%d x %d)", what, W, H); free(tmp); free(ind); free(ker); free(src); return 0; }
    memcpy(src, img, (size_t)W * H * 4);
    for (k = -rad; k <= rad; k++) { ker[k + rad] = expf(-(float)(k * k) / (2.0f * sg * sg)); ks += ker[k + rad]; }
    for (k = 0; k <= 2 * rad; k++) ker[k] /= ks;
    for (y = 0; y < H; y++) for (x = 0; x < W; x++) { float a = 0; for (k = -rad; k <= rad; k++) { int xx = x + k; if (xx >= 0 && xx < W && src[(y * W + xx) * 4 + 3] >= alpha) a += ker[k + rad]; } tmp[y * W + x] = a; }
    for (y = 0; y < H; y++) for (x = 0; x < W; x++) { float a = 0; for (k = -rad; k <= rad; k++) { int yy = y + k; if (yy >= 0 && yy < H) a += ker[k + rad] * tmp[yy * W + x]; } ind[y * W + x] = a; }
    for (y = 0; y < H; y++) for (x = 0; x < W; x++) { unsigned char *p = img + (y * W + x) * 4; int was = src[(y * W + x) * 4 + 3] >= alpha, is = ind[y * W + x] > 0.5f;
        if (was && !is) { p[3] = 0; cutn++; }
        else if (!was && is) { int r, got = 0, qx, qy;
            for (r = 1; r <= rad + 2 && !got; r++) for (qy = y - r; qy <= y + r && !got; qy++) for (qx = x - r; qx <= x + r && !got; qx++)
                if (qx >= 0 && qy >= 0 && qx < W && qy < H && src[(qy * W + qx) * 4 + 3] >= alpha) { memcpy(p, src + (qy * W + qx) * 4, 4); p[3] = 255; got = 1; }
            added += got; } }
    vlog(VLOG_DBG, "round", "%s rounded: %d pixels added, %d taken off", what, added, cutn);
    free(tmp); free(ind); free(ker); free(src); return 1;
}

static int run(const char *in, const char *outp, const Par *P) {
    int W, H, n = P->dim, x, y, z, i, npal = 0, res = 1, hits_total = 0;
    unsigned char *img = read_raw(in, &W, &H), *set = NULL, *o = NULL;
    float *din = NULL, *dout = NULL, *dins = NULL, *hgt = NULL, *A = NULL, *B = NULL;
    Col pal[256]; double t0, t1, t2, t3, t4; size_t nv = (size_t)n * n * n; long inside = 0;
    if (!img) return 1;
    if (W != H) { vlog(VLOG_ERR, "open", "%s: the picture must be square, this one is %d x %d", in, W, H); goto done; }
    t0 = now_ms();
    set = malloc((size_t)W * H); din = malloc(sizeof(float) * W * H); dout = malloc(sizeof(float) * W * H); dins = malloc(sizeof(float) * W * H);
    hgt = malloc(sizeof(float) * n * n); A = malloc(sizeof(float) * nv); B = malloc(sizeof(float) * nv); o = calloc((size_t)P->out * P->out, 4);
    if (!set || !din || !dout || !dins || !hgt || !A || !B || !o) { vlog(VLOG_ERR, "memory", "out of memory for a %d x %d x %d shape (%.0f MB needed)", n, n, n, nv * 8.0 / 1048576.0); goto done; }

    /* the palette: every colour of the enlarged sprite (cleanEdge never blends, so these are the sprite's own) */
    for (i = 0; i < W * H; i++) { const unsigned char *p = img + i * 4; int k;
        if (p[3] < P->alpha) continue;
        for (k = 0; k < npal; k++) if (fabsf(pal[k].c[0] * 255 - p[0]) < 0.5f && fabsf(pal[k].c[1] * 255 - p[1]) < 0.5f && fabsf(pal[k].c[2] * 255 - p[2]) < 0.5f) break;
        if (k == npal && npal < 64) { pal[npal].c[0] = p[0] / 255.0f; pal[npal].c[1] = p[1] / 255.0f; pal[npal].c[2] = p[2] / 255.0f; npal++; }
    }
    if (!npal) { vlog(VLOG_ERR, "open", "%s: every pixel is clear, there is nothing to build", in); goto done; }
    if (npal >= 64 && P->palette) vlog(VLOG_WARN, "open", "%s: more than 64 colours; only the first 64 are used as the sprite's own", in);

    if (P->curves > 0) {
        float sg = P->curves * (float)W / 16.0f; int rad = (int)ceilf(2.5f * sg), l, k, changed = 0;
        unsigned char *lab = malloc((size_t)W * H), *nlab = malloc((size_t)W * H);
        float *ind = malloc(sizeof(float) * W * H), *tmp = malloc(sizeof(float) * W * H), *best = malloc(sizeof(float) * W * H), *ker = malloc(sizeof(float) * (2 * rad + 1)), ks = 0;
        if (!lab || !nlab || !ind || !tmp || !best || !ker) { vlog(VLOG_ERR, "memory", "out of memory for the curve pass (%d x %d)", W, H); free(lab); free(nlab); free(ind); free(tmp); free(best); free(ker); goto done; }
        for (k = -rad; k <= rad; k++) { ker[k + rad] = expf(-(float)(k * k) / (2.0f * sg * sg)); ks += ker[k + rad]; }
        for (k = 0; k <= 2 * rad; k++) ker[k] /= ks;
        for (i = 0; i < W * H; i++) { const unsigned char *p = img + i * 4; lab[i] = 0; best[i] = -1; nlab[i] = 0;
            if (p[3] >= P->alpha) { float bd = 1e9f; for (k = 0; k < npal; k++) { float d = fabsf(pal[k].c[0] * 255 - p[0]) + fabsf(pal[k].c[1] * 255 - p[1]) + fabsf(pal[k].c[2] * 255 - p[2]); if (d < bd) { bd = d; lab[i] = (unsigned char)(k + 1); } } } }
        for (l = 0; l <= npal; l++) {
            float edge = l == 0 ? 1.0f : 0.0f;                  /* beyond the picture everything is clear */
            for (y = 0; y < H; y++) for (x = 0; x < W; x++) { float a = 0; for (k = -rad; k <= rad; k++) { int xx = x + k; a += ker[k + rad] * (xx < 0 || xx >= W ? edge : (lab[y * W + xx] == l ? 1.0f : 0.0f)); } tmp[y * W + x] = a; }
            for (y = 0; y < H; y++) for (x = 0; x < W; x++) { float a = 0; for (k = -rad; k <= rad; k++) { int yy = y + k; a += ker[k + rad] * (yy < 0 || yy >= H ? edge : tmp[yy * W + x]); } ind[y * W + x] = a; }
            for (i = 0; i < W * H; i++) if (ind[i] > best[i]) { best[i] = ind[i]; nlab[i] = (unsigned char)l; }
        }
        for (i = 0; i < W * H; i++) { unsigned char *p = img + i * 4; if (nlab[i] != lab[i]) changed++;
            if (!nlab[i]) p[3] = 0; else { p[0] = (unsigned char)(pal[nlab[i] - 1].c[0] * 255 + 0.5f); p[1] = (unsigned char)(pal[nlab[i] - 1].c[1] * 255 + 0.5f); p[2] = (unsigned char)(pal[nlab[i] - 1].c[2] * 255 + 0.5f); p[3] = 255; } }
        vlog(VLOG_DBG, "curves", "rounded %d of %d pixels", changed, W * H);
        free(lab); free(nlab); free(ind); free(tmp); free(best); free(ker);
    }

    if (P->shape > 0 && !round_mask(img, W, H, P->alpha, P->shape * (float)W / 16.0f, "outer shape")) goto done;

    /* outline down to one output pixel: cut the outside of the black rim away */
    if (P->outline && !P->solid) {
        float S = (float)W / 16.0f, keep = P->outline == 2 ? -1.0f : (float)W / (float)P->out; int any = 0, cut = 0;
        for (i = 0; i < W * H; i++) { set[i] = img[i * 4 + 3] >= P->alpha && lum3(img + i * 4) > DARK; any |= set[i]; }
        if (!any) vlog(VLOG_WARN, "outline", "%s: the sprite is all outline colour; the outline is left as it is", in);
        else {
            if (!dist2d(set, W, H, dins)) goto done;
            for (i = 0; i < W * H; i++) set[i] = img[i * 4 + 3] < P->alpha;
            if (!dist2d(set, W, H, dout)) goto done;
            for (y = 0; y < H; y++) for (x = 0; x < W; x++) { float d; i = y * W + x; d = dout[i];
                if (x + 1 < d) d = (float)x + 1; if (W - x < d) d = (float)(W - x); if (y + 1 < d) d = (float)y + 1; if (H - y < d) d = (float)(H - y);
                if (img[i * 4 + 3] >= P->alpha && lum3(img + i * 4) <= DARK && d <= 1.5f * S + 1 && dins[i] > keep + 0.5f) { img[i * 4 + 3] = 0; cut++; } }
            vlog(VLOG_DBG, "outline", "trimmed %d pixels of outline", cut);
        }
    }

    if (P->body > 0 && !P->solid && !round_mask(img, W, H, P->alpha, P->body * (float)W / 16.0f, "body")) goto done;      /* after the outline is off: dents the outline left in the body fill in */

    /* the outer shape from the pixels of the original sprite: the edge between sprite and background is walked as a closed
       line through the middle of every pixel side, smoothed without shrinking (Taubin, here on a line, which it is made for),
       and filled again at the big size. A staircase of any slope becomes a straight edge; one-pixel parts keep their width. */
    if (P->contour > 0 || P->solid) {
        int G, gh, ne = 0, nvx = 0, nl = 0, e, k, it, ok2 = 0, added = 0, cutn = 0, S2 = W / 16 > 1 ? W / 16 : 1;
        unsigned char *sp = NULL, *in = NULL, *used = NULL, *M = NULL, *old = NULL; int *ed = NULL, *ls = NULL; float *vx = NULL, *vy = NULL, *tx2 = NULL, *ty2 = NULL, *xs = NULL;
        G = 16; gh = 16;
        if (P->contour > 0) { sp = read_raw(P->src, &G, &gh); if (!sp) goto done; }
        if (G != gh || G > 128) { vlog(VLOG_ERR, "open", "%s: the original must be square and at most 128 x 128, this one is %d x %d", P->src, G, gh); free(sp); goto done; }
        in = calloc((size_t)G * G, 1); ed = malloc(sizeof(int) * 16 * G * G); used = calloc((size_t)4 * G * G, 1); ls = malloc(sizeof(int) * (4 * G * G + 1));
        vx = malloc(sizeof(float) * 4 * G * G); vy = malloc(sizeof(float) * 4 * G * G); tx2 = malloc(sizeof(float) * 4 * G * G); ty2 = malloc(sizeof(float) * 4 * G * G); xs = malloc(sizeof(float) * 4 * G * G);
        M = calloc((size_t)W * H, 1); old = malloc((size_t)W * H * 4);
        if (!in || !ed || !used || !ls || !vx || !vy || !tx2 || !ty2 || !xs || !M || !old) { vlog(VLOG_ERR, "memory", "out of memory for the contour pass"); goto cdone; }
#define OPQ(X, Y) ((X) >= 0 && (Y) >= 0 && (X) < G && (Y) < G && sp[((Y) * G + (X)) * 4 + 3] >= P->alpha)
#define INS(X, Y) ((X) >= 0 && (Y) >= 0 && (X) < G && (Y) < G && in[(Y) * G + (X)])
        if (!sp) { for (i = 0; i < W * H; i++) M[i] = img[i * 4 + 3] >= P->alpha; }
        else {
        for (y = 0; y < G; y++) for (x = 0; x < G; x++) { int rim = 0, a, b;
            if (!OPQ(x, y)) continue;
            if (!P->solid && P->outline == 2 && lum3(sp + (y * G + x) * 4) <= DARK) for (b = -1; b <= 1; b++) for (a = -1; a <= 1; a++) if (!OPQ(x + a, y + b)) rim = 1;      /* the black rim is not part of the body */
            in[y * G + x] = !rim; }
        for (y = 0; y < G; y++) for (x = 0; x < G; x++) { if (!in[y * G + x]) continue;      /* sides, walked with the body on the right */
            if (!INS(x, y - 1)) { ed[ne * 4] = x; ed[ne * 4 + 1] = y; ed[ne * 4 + 2] = x + 1; ed[ne * 4 + 3] = y; ne++; }
            if (!INS(x + 1, y)) { ed[ne * 4] = x + 1; ed[ne * 4 + 1] = y; ed[ne * 4 + 2] = x + 1; ed[ne * 4 + 3] = y + 1; ne++; }
            if (!INS(x, y + 1)) { ed[ne * 4] = x + 1; ed[ne * 4 + 1] = y + 1; ed[ne * 4 + 2] = x; ed[ne * 4 + 3] = y + 1; ne++; }
            if (!INS(x - 1, y)) { ed[ne * 4] = x; ed[ne * 4 + 1] = y + 1; ed[ne * 4 + 2] = x; ed[ne * 4 + 3] = y; ne++; } }
        if (!ne) { vlog(VLOG_WARN, "contour", "%s: no body found in the original, the outer shape is left as it is", P->src); ok2 = 1; goto cdone; }
        for (e = 0; e < ne; e++) { int cur = e, guard = 0;
            if (used[e]) continue;
            ls[nl++] = nvx;
            for (;;) { int nx2 = -1, c2, dx2 = ed[cur * 4 + 2] - ed[cur * 4], dy2 = ed[cur * 4 + 3] - ed[cur * 4 + 1];
                used[cur] = 1; vx[nvx] = (float)(ed[cur * 4] + ed[cur * 4 + 2]) * 0.5f; vy[nvx] = (float)(ed[cur * 4 + 1] + ed[cur * 4 + 3]) * 0.5f; nvx++;
                for (c2 = 0; c2 < ne; c2++) { if (ed[c2 * 4] != ed[cur * 4 + 2] || ed[c2 * 4 + 1] != ed[cur * 4 + 3] || (used[c2] && c2 != e)) continue;
                    if (nx2 < 0 || dx2 * (ed[c2 * 4 + 3] - ed[c2 * 4 + 1]) - dy2 * (ed[c2 * 4 + 2] - ed[c2 * 4]) < 0) nx2 = c2; }      /* where two pixels touch at a corner, go on to the other one: they stay joined */
                if (nx2 < 0 || nx2 == e || ++guard > ne) break;
                cur = nx2; } }
        ls[nl] = nvx;
        for (k = 0; k < nl; k++) { int a = ls[k], n2 = ls[k + 1] - a, j;
            for (it = 0; it < P->contour * 2 && n2 >= 3; it++) { float w = it & 1 ? P->mu : P->lambda;
                for (j = 0; j < n2; j++) { int pj = a + (j + n2 - 1) % n2, nj = a + (j + 1) % n2; tx2[a + j] = vx[a + j] + w * ((vx[pj] + vx[nj]) * 0.5f - vx[a + j]); ty2[a + j] = vy[a + j] + w * ((vy[pj] + vy[nj]) * 0.5f - vy[a + j]); }
                memcpy(vx + a, tx2 + a, sizeof(float) * n2); memcpy(vy + a, ty2 + a, sizeof(float) * n2); } }
        for (y = 0; y < H; y++) { float yc = ((float)y + 0.5f) * (float)G / (float)H; int nx3 = 0, j, a2, b2;
            for (k = 0; k < nl; k++) { int a = ls[k], n2 = ls[k + 1] - a;
                for (j = 0; j < n2; j++) { float ax = vx[a + j], ay = vy[a + j], bx = vx[a + (j + 1) % n2], by = vy[a + (j + 1) % n2];
                    if ((ay <= yc) != (by <= yc)) xs[nx3++] = ax + (yc - ay) / (by - ay) * (bx - ax); } }
            for (a2 = 1; a2 < nx3; a2++) { float t = xs[a2]; for (b2 = a2 - 1; b2 >= 0 && xs[b2] > t; b2--) xs[b2 + 1] = xs[b2]; xs[b2 + 1] = t; }
            for (a2 = 0; a2 + 1 < nx3; a2 += 2) for (x = 0; x < W; x++) { float xc = ((float)x + 0.5f) * (float)G / (float)W; if (xc >= xs[a2] && xc < xs[a2 + 1]) M[y * W + x] = 1; } }
        }
        memcpy(old, img, (size_t)W * H * 4);
        if (P->solid) {
            /* the smooth solid shape is drawn in by the inset; then every pixel of it takes the colour of the old pixel that is
               nearest in place AND sits as deep inside its own shape (old depth counted from the inside of the old outline) */
            float Sf = (float)W / 16.0f, ins = P->inset * Sf; int R = S2 + 2, lost = 0;
            for (i = 0; i < W * H; i++) set[i] = old[i * 4 + 3] < P->alpha;
            if (!dist2d(set, W, H, dins)) goto cdone;
            for (i = 0; i < W * H; i++) set[i] = !M[i];
            if (!dist2d(set, W, H, din)) goto cdone;
            for (y = 0; y < H; y++) for (x = 0; x < W; x++) { float b = (float)(x + 1); i = y * W + x;
                if ((float)(W - x) < b) b = (float)(W - x); if ((float)(y + 1) < b) b = (float)(y + 1); if ((float)(H - y) < b) b = (float)(H - y);
                if (dins[i] > b) dins[i] = b; if (din[i] > b) din[i] = b; }
            for (y = 0; y < H; y++) for (x = 0; x < W; x++) { unsigned char *p = img + (y * W + x) * 4; float dn, bc = 1e18f; int qx, qy, bi = -1;
                i = y * W + x; dn = din[i] - 0.5f - ins;
                if (!M[i] || dn < 0) { p[3] = 0; continue; }
                for (qy = y - R; qy <= y + R; qy++) for (qx = x - R; qx <= x + R; qx++) { int q = qy * W + qx; float dd, c;
                    if (qx < 0 || qy < 0 || qx >= W || qy >= H || old[q * 4 + 3] < P->alpha || dins[q] - 0.5f < Sf + 0.5f) continue;
                    dd = (dins[q] - 0.5f - Sf) - dn; c = (float)((qx - x) * (qx - x) + (qy - y) * (qy - y)) + 4.0f * dd * dd;
                    if (c < bc) { bc = c; bi = q; } }
                if (bi < 0) { p[3] = 0; lost++; continue; }
                memcpy(p, old + bi * 4, 4); p[3] = 255; added++; }
            if (lost) vlog(VLOG_DBG, "solid", "%d pixels of the new shape found no colour near them and were left clear", lost);
        }
        else for (y = 0; y < H; y++) for (x = 0; x < W; x++) { unsigned char *p = img + (y * W + x) * 4; int was = old[(y * W + x) * 4 + 3] >= P->alpha;
            if (!M[y * W + x]) { if (was) cutn++; p[3] = 0; }
            else if (!was) { int r, got = 0, qx, qy;
                for (r = 1; r <= 3 * S2 && !got; r++) for (qy = y - r; qy <= y + r && !got; qy++) for (qx = x - r; qx <= x + r && !got; qx++)
                    if (qx >= 0 && qy >= 0 && qx < W && qy < H && old[(qy * W + qx) * 4 + 3] >= P->alpha) { memcpy(p, old + (qy * W + qx) * 4, 4); p[3] = 255; got = 1; }
                added += got; } }
        vlog(VLOG_DBG, "contour", "%d closed lines, %d points; %d pixels added, %d taken off", nl, nvx, added, cutn);
        ok2 = 1;
#undef OPQ
#undef INS
cdone:
        free(sp); free(in); free(ed); free(used); free(ls); free(vx); free(vy); free(tx2); free(ty2); free(xs); free(M); free(old);
        if (!ok2) goto done;
    }

    /* stage 2: height of the shape over each pixel, then the field */
    for (i = 0; i < W * H; i++) set[i] = img[i * 4 + 3] < P->alpha;
    if (!dist2d(set, W, H, din)) goto done;                       /* inside pixels: distance to the outside */
    for (i = 0; i < W * H; i++) set[i] = !set[i];
    if (!dist2d(set, W, H, dout)) goto done;                      /* outside pixels: distance to the sprite */
    {
        float sc = (float)W / (float)n, R = P->reach * (float)n / 16.0f, T = P->thick * (float)n * 0.45f, m = P->extrude == 0 ? 0.0f : P->extrude == 1 ? 1.0f : P->mix;
        for (y = 0; y < n; y++) for (x = 0; x < n; x++) {
            int sx = (int)(((float)x + 0.5f) * sc), sy = (int)(((float)y + 0.5f) * sc), si; float d, h;
            if (sx >= W) sx = W - 1; if (sy >= H) sy = H - 1; si = sy * W + sx;
            if (img[si * 4 + 3] >= P->alpha) {
                float e = din[si], t, puff;
                if (sx + 1 < e) e = (float)sx + 1; if (W - sx < e) e = (float)(W - sx); if (sy + 1 < e) e = (float)sy + 1; if (H - sy < e) e = (float)(H - sy);
                d = (e - 0.5f) / sc; t = clampf(d / R, 0, 1); puff = 1.0f - powf(1.0f - t, P->bevel);
                h = T * ((1.0f - m) * puff + m * sqrtf(puff) * lum3(img + si * 4));
                if (h < 0.75f) h = 0.75f;
                inside++;
            } else h = -(dout[si] - 0.5f) / sc;
            hgt[y * n + x] = h;
        }
    }
    if (!inside) { vlog(VLOG_ERR, "shape", "%s: nothing of the sprite is left after the outline was thinned", in); goto done; }
    {
        float zc = (float)(n - 1) * 0.5f;
        for (z = 0; z < n; z++) for (y = 0; y < n; y++) for (x = 0; x < n; x++) {
            int xa = x > 0 ? x - 1 : x, xb = x < n - 1 ? x + 1 : x, ya = y > 0 ? y - 1 : y, yb = y < n - 1 ? y + 1 : y;
            float h = hgt[y * n + x], gx = (hgt[y * n + xb] - hgt[y * n + xa]) / (float)(xb - xa ? xb - xa : 1), gy = (hgt[yb * n + x] - hgt[ya * n + x]) / (float)(yb - ya ? yb - ya : 1);
            float g = sqrtf(1.0f + gx * gx + gy * gy); if (g > 4) g = 4;
            A[((size_t)z * n + y) * n + x] = (fabsf((float)z - zc) - h) / g;
        }
    }
    t1 = now_ms();

    /* stage 3: smoothing (sizes are given for a 64-cube shape and scale with DIM) */
    {
        float k = (float)n / 64.0f;
        if (P->close_r > 0) { filter(&A, &B, n, 1, P->close_r * k, 0); filter(&A, &B, n, 2, P->close_r * k, 0); }
        if (P->open_r > 0) { filter(&A, &B, n, 2, P->open_r * k, 0); filter(&A, &B, n, 1, P->open_r * k, 0); }
        if (P->smooth == 2 || P->smooth == 3) for (i = 0; i < P->titers * (n / 64 > 1 ? n / 64 : 1); i++) { filter(&A, &B, n, 3, P->lambda, 0); filter(&A, &B, n, 3, P->mu, 0); }
        if (P->smooth == 1 || P->smooth == 3) for (i = 0; i < P->iters; i++) filter(&A, &B, n, 0, P->bs * k, P->br * k);
    }
    t2 = now_ms();

    /* stage 4: rays */
    if (P->palette == 2) npal = pal_extend(pal, npal, 256);
    {
        float az = (float)(P->az * PI / 180), el = (float)(P->el * PI / 180), Lx = sinf(az) * cosf(el), Ly = -cosf(az) * cosf(el), Lz = -sinf(el), flat = sinf(el);
        float tx = (float)(P->tiltx * PI / 180), ty = (float)(P->tilty * PI / 180), c = (float)(n - 1) * 0.5f, vs = (float)n / (float)P->out, isc = (float)W / (float)n;
        float dx = sinf(ty) * cosf(tx), dy = -sinf(tx), dz = cosf(ty) * cosf(tx), ad = 1.5f * (float)n / 64.0f;
        float ux = cosf(ty), uy = 0, uz = -sinf(ty), wx = sinf(ty) * sinf(tx), wy = cosf(tx), wz = cosf(ty) * sinf(tx);      /* picture right and down, in the shape */
        int R = P->rays, px, py, a, b;
        for (py = 0; py < P->out; py++) for (px = 0; px < P->out; px++) {
            float sum[3] = { 0, 0, 0 }, base[3] = { 0, 0, 0 }, bestc = 1e9f; int hit = 0, dark = 0, split = !(P->edge == 0 && P->palette == 0);      /* split: outline rays are counted apart, never mixed into the colour */
            for (b = 0; b < R; b++) for (a = 0; a < R; a++) {
                float u = ((float)px + ((float)a + 0.5f) / R) * vs - 0.5f - c, v = ((float)py + ((float)b + 0.5f) / R) * vs - 0.5f - c;
                float ox = c + u * ux + v * wx - dx * (float)n, oy = c + u * uy + v * wy - dy * (float)n, oz = c + u * uz + v * wz - dz * (float)n;
                float t = 0, tp = 0, s, sp = 1, tmax = (float)n * 2.0f; int found = 0;
                while (t < tmax) { s = fld(A, n, ox + dx * t, oy + dy * t, oz + dz * t); if (s < 0) { found = 1; break; } tp = t; sp = s; t += s > 1 ? s * 0.6f : 0.4f; }
                if (!found) {
                    if (P->outline == 2 && P->edge == 0) {      /* a miss just beside the sprite is a ray on the outline */
                        int qx = (int)((c + u + 0.5f) * isc), qy = (int)((c + v + 0.5f) * isc);
                        if (qx >= 0 && qy >= 0 && qx < W && qy < H && img[(qy * W + qx) * 4 + 3] < P->alpha && dout[qy * W + qx] - 0.5f <= (float)W / (float)P->out) { hit++; if (split) dark++; }
                    }
                    continue;
                }
                { int k; float lo = tp, hi = t; (void)sp; for (k = 0; k < 8; k++) { float mid = (lo + hi) * 0.5f; if (fld(A, n, ox + dx * mid, oy + dy * mid, oz + dz * mid) < 0) hi = mid; else lo = mid; } t = (lo + hi) * 0.5f; }
                {
                    float hx = ox + dx * t, hy = oy + dy * t, hz = oz + dz * t, nx, ny, nz, nl, f, occ = 0, w = 0.5f, col[3]; int k, sx, sy, r, got = 0; const unsigned char *p = NULL;
                    nx = fld(A, n, hx + 1, hy, hz) - fld(A, n, hx - 1, hy, hz); ny = fld(A, n, hx, hy + 1, hz) - fld(A, n, hx, hy - 1, hz); nz = fld(A, n, hx, hy, hz + 1) - fld(A, n, hx, hy, hz - 1);
                    nl = sqrtf(nx * nx + ny * ny + nz * nz); if (nl < 1e-6f) { nx = 0; ny = 0; nz = -1; } else { nx /= nl; ny /= nl; nz /= nl; }
                    /* colour: the sprite pixel under the hit (the nearest opaque one when smoothing moved the surface a little) */
                    sx = (int)((hx + 0.5f) * isc); sy = (int)((hy + 0.5f) * isc);
                    for (r = 0; r <= 6 && !got; r++) { int qx, qy; for (qy = sy - r; qy <= sy + r && !got; qy++) for (qx = sx - r; qx <= sx + r && !got; qx++)
                        if (qx >= 0 && qy >= 0 && qx < W && qy < H && img[(qy * W + qx) * 4 + 3] >= P->alpha) { p = img + (qy * W + qx) * 4; got = 1; } }
                    if (!got) continue;
                    col[0] = p[0] / 255.0f; col[1] = p[1] / 255.0f; col[2] = p[2] / 255.0f;
                    if (lum3(p) > DARK) {
                        for (k = 1; k <= 5; k++) { float dd = ad * (float)k, sv = fld(A, n, hx + nx * dd, hy + ny * dd, hz + nz * dd); occ += w * clampf((dd - sv) / dd, 0, 1); w *= 0.5f; }
                        f = (1.0f + P->light * ((nx * Lx + ny * Ly + nz * Lz) - flat)) * (1.0f - P->ao * clampf(occ, 0, 1));
                        if (P->rim > 0) { float fr = 1.0f + (nx * dx + ny * dy + nz * dz); f *= 1.0f - P->rim * clampf(fr, 0, 1) * clampf(fr, 0, 1); }      /* fr: 0 facing the eye, 1 edge-on */
                        if (f < P->ambient) f = P->ambient;
                    } else { f = 1.0f; if (split) { dark++; hit++; continue; } }
                    for (k = 0; k < 3; k++) sum[k] += clampf(col[k] * f, 0, 1);
                    { float cd = (((float)a + 0.5f) / R - 0.5f) * (((float)a + 0.5f) / R - 0.5f) + (((float)b + 0.5f) / R - 0.5f) * (((float)b + 0.5f) / R - 0.5f);
                      if (cd < bestc) { bestc = cd; base[0] = col[0]; base[1] = col[1]; base[2] = col[2]; } }
                    hit++;
                }
            }
            if (hit) {
                float cov = (float)hit / (float)(R * R), al, rgb[3]; unsigned char *q = o + (py * P->out + px) * 4; int k;
                for (k = 0; k < 3; k++) rgb[k] = dark * 2 >= hit ? 0.0f : sum[k] / (float)(hit - dark);
                if (P->edge == 0) al = clampf((cov - 0.5f) / (P->soft < 0.02f ? 0.02f : P->soft) + 0.5f, 0, 1); else al = cov >= P->cutoff ? 1.0f : 0.0f;
                if (al <= 0) continue;
                if (P->palette && dark * 2 < hit) { const float *pc = pal[pal_pick(pal, npal, rgb, base)].c; rgb[0] = pc[0]; rgb[1] = pc[1]; rgb[2] = pc[2]; }
                for (k = 0; k < 3; k++) q[k] = (unsigned char)(rgb[k] * 255.0f + 0.5f);
                q[3] = (unsigned char)(al * 255.0f + 0.5f); hits_total++;
            }
        }
    }
    if (P->outline == 2 && P->edge == 1) {                     /* the 1-pixel line: every clear pixel beside the picture */
        int W2 = P->out; unsigned char *m = malloc((size_t)W2 * W2);
        if (!m) { vlog(VLOG_ERR, "memory", "out of memory for the outline pass"); goto done; }
        for (i = 0; i < W2 * W2; i++) m[i] = o[i * 4 + 3] != 0;
        for (y = 0; y < W2; y++) for (x = 0; x < W2; x++) { i = y * W2 + x;
            if (!m[i] && ((x > 0 && m[i - 1]) || (x < W2 - 1 && m[i + 1]) || (y > 0 && m[i - W2]) || (y < W2 - 1 && m[i + W2]))) { o[i * 4] = o[i * 4 + 1] = o[i * 4 + 2] = 0; o[i * 4 + 3] = 255; } }
        free(m);
    }
    t3 = now_ms();
    if (!hits_total) vlog(VLOG_WARN, "draw", "%s: no ray hit the shape, the picture is empty", in);
    if (!write_raw(outp, o, P->out, P->out)) goto done;
    t4 = now_ms();
    for (i = 0, inside = 0; (size_t)i < nv; i++) if (A[i] < 0) inside++;
    vlog(VLOG_INFO, "voxup", "%s -> %s  %d^3 (%ld voxels inside), out %d: shape %.0f ms, smooth %.0f ms, rays %.0f ms, total %.0f ms", in, outp, n, inside, P->out, t1 - t0, t2 - t1, t3 - t2, t4 - t0);
    printf("%d^3  inside %ld  shape %.0f ms  smooth %.0f ms  rays %.0f ms  total %.0f ms  backend C\n", n, inside, t1 - t0, t2 - t1, t3 - t2, t4 - t0);
    res = 0;
done:
    free(img); free(set); free(din); free(dout); free(dins); free(hgt); free(A); free(B); free(o);
    return res;
}

static int pick(const char *opt, const char *v, const char *const *names, int n, int *out) {
    int i; char list[256] = "";
    for (i = 0; i < n; i++) { if (!strcmp(v, names[i])) { *out = i; return 1; } strncat(list, i ? ", " : "", sizeof list - strlen(list) - 1); strncat(list, names[i], sizeof list - strlen(list) - 1); }
    vlog(VLOG_ERR, "args", "%s: one of %s, not %s", opt, list, v); return 0;
}
static int num(const char *opt, const char *v, float lo, float hi, float *out) {
    char *e; double d = strtod(v, &e);
    if (e == v || *e || d < lo || d > hi) { vlog(VLOG_ERR, "args", "%s: a number from %g to %g, not %s", opt, lo, hi, v); return 0; }
    *out = (float)d; return 1;
}

int voxup_cli(int argc, char **argv) {   /* the command line; main() is in pick.c (the window when there are no arguments) */
    static const char *const EX[] = { "puff", "light", "mix" }, *const SM[] = { "none", "bilateral", "taubin", "both" }, *const ED[] = { "soft", "hard" }, *const PA[] = { "free", "own", "shades" }, *const OU[] = { "keep", "thin", "redraw" };
    Par P; int i, r; float f; const char *in = NULL, *out = NULL;
    P.dim = 128; P.out = 32; P.alpha = 128; P.rays = 4; P.iters = 1; P.extrude = 0; P.smooth = 3; P.edge = 1; P.palette = 0; P.outline = 2;
    P.thick = 0.35f; P.bevel = 1.6f; P.mix = 0.35f; P.reach = 2.5f; P.bs = 1.2f; P.br = 0.8f; P.close_r = 0; P.open_r = 0; P.az = 315; P.el = 40; P.light = 0.9f; P.ao = 0.5f; P.ambient = 0.3f;
    P.tiltx = 0; P.tilty = 0; P.titers = 6; P.lambda = 0.5f; P.mu = -0.53f; P.curves = 0.25f; P.shape = 0.8f;      /* the look picked on 2026-11-20: hard edge, colour areas rounded a little, outer shape rounded, 1-pixel outline redrawn */
    P.contour = 0; P.src = NULL; P.solid = 0; P.inset = 1.0f; P.body = 0; P.soft = 1.0f; P.cutoff = 0.5f; P.rim = 0;
    vlog_open(NULL, 0); vlog_echo(1);
    for (i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL; int ok = 1, used = 1;
        if (a[0] != '-') { if (!in) in = a; else if (!out) out = a; else { vlog(VLOG_ERR, "args", "one input and one output file, %s is a third", a); ok = 0; } used = 0; }
        else if (!v) { vlog(VLOG_ERR, "args", "%s needs a value", a); ok = 0; }
        else if (!strcmp(a, "--dim")) { ok = num(a, v, 32, 256, &f); P.dim = (int)f; if (ok && P.dim != 32 && P.dim != 64 && P.dim != 128 && P.dim != 256) { vlog(VLOG_ERR, "args", "--dim: 32, 64, 128 or 256, not %s", v); ok = 0; } }
        else if (!strcmp(a, "--out")) { ok = num(a, v, 16, 256, &f); P.out = (int)f; }
        else if (!strcmp(a, "--alpha")) { ok = num(a, v, 1, 255, &f); P.alpha = (int)f; }
        else if (!strcmp(a, "--rays")) { ok = num(a, v, 1, 16, &f); P.rays = (int)f; }
        else if (!strcmp(a, "--iters")) { ok = num(a, v, 0, 12, &f); P.iters = (int)f; }
        else if (!strcmp(a, "--extrude")) ok = pick(a, v, EX, 3, &P.extrude);
        else if (!strcmp(a, "--smooth")) ok = pick(a, v, SM, 4, &P.smooth);
        else if (!strcmp(a, "--edge")) ok = pick(a, v, ED, 2, &P.edge);
        else if (!strcmp(a, "--palette")) ok = pick(a, v, PA, 3, &P.palette);
        else if (!strcmp(a, "--outline")) ok = pick(a, v, OU, 3, &P.outline);
        else if (!strcmp(a, "--thick")) ok = num(a, v, 0.05f, 1, &P.thick);
        else if (!strcmp(a, "--bevel")) ok = num(a, v, 0.5f, 3, &P.bevel);
        else if (!strcmp(a, "--mix")) ok = num(a, v, 0, 1, &P.mix);
        else if (!strcmp(a, "--reach")) ok = num(a, v, 0.5f, 8, &P.reach);
        else if (!strcmp(a, "--bs")) ok = num(a, v, 0.5f, 3, &P.bs);
        else if (!strcmp(a, "--br")) ok = num(a, v, 0.1f, 2, &P.br);
        else if (!strcmp(a, "--close")) ok = num(a, v, 0, 2, &P.close_r);
        else if (!strcmp(a, "--open")) ok = num(a, v, 0, 2, &P.open_r);
        else if (!strcmp(a, "--az")) ok = num(a, v, 0, 360, &P.az);
        else if (!strcmp(a, "--el")) ok = num(a, v, -90, 90, &P.el);
        else if (!strcmp(a, "--light")) ok = num(a, v, 0, 2, &P.light);
        else if (!strcmp(a, "--ao")) ok = num(a, v, 0, 1, &P.ao);
        else if (!strcmp(a, "--ambient")) ok = num(a, v, 0, 1, &P.ambient);
        else if (!strcmp(a, "--tiltx")) ok = num(a, v, -15, 15, &P.tiltx);
        else if (!strcmp(a, "--tilty")) ok = num(a, v, -15, 15, &P.tilty);
        else if (!strcmp(a, "--soft")) ok = num(a, v, 0, 1, &P.soft);
        else if (!strcmp(a, "--cutoff")) ok = num(a, v, 0.05f, 1, &P.cutoff);
        else if (!strcmp(a, "--titers")) { ok = num(a, v, 0, 12, &f); P.titers = (int)f; }
        else if (!strcmp(a, "--lambda")) ok = num(a, v, 0.1f, 0.6f, &P.lambda);
        else if (!strcmp(a, "--mu")) ok = num(a, v, -0.65f, -0.1f, &P.mu);
        else if (!strcmp(a, "--curves")) ok = num(a, v, 0, 1.5f, &P.curves);
        else if (!strcmp(a, "--contour")) { ok = num(a, v, 0, 60, &f); P.contour = (int)f; }
        else if (!strcmp(a, "--src")) P.src = v;
        else if (!strcmp(a, "--solid")) { ok = num(a, v, 0, 1, &f); P.solid = (int)f; }
        else if (!strcmp(a, "--inset")) ok = num(a, v, 0, 1.5f, &P.inset);
        else if (!strcmp(a, "--body")) ok = num(a, v, 0, 1.5f, &P.body);
        else if (!strcmp(a, "--shape")) ok = num(a, v, 0, 1.5f, &P.shape);
        else if (!strcmp(a, "--rim")) ok = num(a, v, 0, 1, &P.rim);
        else { vlog(VLOG_ERR, "args", "unknown switch %s", a); ok = 0; }
        if (!ok) { vlog_close(); return 255; }
        i += used;
    }
    if (!in || !out) { fprintf(stderr, "voxup IN.raw OUT.raw [--dim 64|128|256] [--out 32] [--extrude puff|light|mix] [--thick] [--bevel] [--mix] [--reach]\n  [--curves] [--shape] [--body] [--contour N --src ORIGINAL.raw [--solid 1 --inset]] [--smooth none|bilateral|taubin|both] [--titers] [--lambda] [--mu] [--iters] [--bs] [--br] [--close] [--open] [--az] [--el] [--light] [--ao] [--ambient]\n  [--tiltx] [--tilty] [--rays] [--edge soft|hard] [--soft] [--cutoff] [--palette free|own|shades] [--outline keep|thin|redraw] [--rim]\n"); vlog_close(); return 255; }
    if (P.contour > 0 && !P.src) { vlog(VLOG_ERR, "args", "--contour needs --src with the original sprite"); vlog_close(); return 255; }
    r = run(in, out, &P);
    vlog_close();
    return r;
}
