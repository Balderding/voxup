/* pick.c: voxup's window: an item's sprite redrawn by image models with random seeds, one of them chosen and kept.
   Folders: voxup.ini beside the exe (voxup.ini.example).

     voxup                         the window: load an item, draw a round (the models and counts set in the window, random
                                   seeds), new seeds as often as wanted, pick one, Keep -> NNN.png in voxup.ini's game folders
     voxup IN.raw OUT.raw ...      the upscaler's command line, as before (voxup.c)
     voxup --to32 DRAWING.png ITEM OUT.png      one drawing to the 32 x 32 sprite (the window's own step; for checks)
     voxup --upright SPRITE.png OUT.png [turn= style= slim= outline= big=]   the sprite stood upright at 32 x 32
     voxup --selftest SHOT.bmp [ITEM] [--get ID] [--draw ID N [ID N ...]] [--keep [--undo]] [--find T] [--redraw N] [--key CODE]   the window off screen (never
                                   shown): optionally gets a model, draws for real, then writes the window's picture
     voxup --models                the model list: which are ready, what each still needs

   The models are data: models\models.txt + one ComfyUI API graph each (written after ComfyUI's own workflow
   templates). A missing model is downloaded from its links into ComfyUI's models folder when Get is pressed.

   ComfyUI runs hidden (no window, no browser), started from its command line on voxup.ini's port (8189 if not set);
   jobs go over its local HTTP API. If one is already running there it is used and
   left running; one voxup started is stopped when voxup closes (and by a job object if voxup dies).
   Everything voxup makes goes to the work folder (voxup.ini; out\pick beside the exe if not set): NNN\ (in.png, MODEL-sSEED.png drawings, -32.png sprites,
   rounds.txt, desc.txt, kept.txt), keep.txt (every keep), backup\ (a game picture before it was replaced), comfy.log. */
#define WIN32_LEAN_AND_MEAN
#define _CRT_RAND_S
#include <windows.h>
#include <winhttp.h>
#include <shlobj.h>
#include <shellapi.h>
#include <commdlg.h>
#include <bcrypt.h>
#include <wincred.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <ctype.h>
#include "nk_conf.h"
#include "third_party/nuklear.h"
#include "third_party/nuklear_gdi.h"
#include "nk_ext.h"
#include "third_party/stb_image.h"
#include "third_party/stb_image_write.h"
#include "vlog.h"
#include "embed.h"
#include "shrink.h"
#include "upright.h"
/* the built-in engine (stable-diffusion.cpp, built in with -DVOXUP_SD): models drawn inside voxup, no ComfyUI */
#ifdef VOXUP_SD
#include "stable-diffusion.h"
#define SD_BUILT 1
#else
#define SD_BUILT 0
#endif

int voxup_cli(int argc, char **argv);
HDC nk_gdi_memdc(void);
HBITMAP nk_gdi_membitmap(void);

/* ---------------------------------------------------------------- where things are */
/* every folder comes from voxup.ini beside the exe (voxup.ini.example says what each one is) */
static char P_SPRITES[MAX_PATH], P_NAMES[MAX_PATH], P_DESC[4][MAX_PATH], P_ENLARGED[MAX_PATH], P_COMFY[MAX_PATH], P_YAML[MAX_PATH], P_SHARED[MAX_PATH];
static char P_UPRIGHT[4][MAX_PATH]; static int N_UPRIGHT;
static char P_PALETTE[MAX_PATH];     /* voxup.ini palette = : a palette file for Colours: A palette (B8) */     /* upright = folders of NNN.png, 32 x 32, the item stood upright */
static char P_GAME[4][MAX_PATH]; static int N_DESC, N_GAME, PORT = 8189, ITEMS_MAX = 512, GAME_MAX = 32;
static char SKIP[512];                      /* items left out of the list (voxup.ini skip = ...) */
static const char *GAMEDIRS[4]; static char GAMETAG[4][16];

#define BG "a flat, even, solid bright magenta background (pure pink-purple, the same everywhere; the item itself has no " \
           "magenta in it and no pink glow on it)"
/* the standard prompts: "draw" (describe and draw anew) and "edit" (an instruction); {d} = what the item is */
static const char *KLEIN_T = "This is a tiny, rough picture of an item from a fantasy dungeon game. It is {d}. Draw a new, richly "
    "detailed picture of this same item, laid out exactly as in the picture: the same pose, direction and proportions. Match "
    "the original picture's colours exactly: every part keeps the colour it has there, and no other colours are added. "
    "Hand-painted, crisp and clean, alone on " BG ", no shadow on the ground, no text, no border.";
static const char *KONTEXT_T = "This is a rough picture of an item from a fantasy dungeon game. It is {d}. Redraw it as a new, "
    "richly detailed hand-painted game item, laid out exactly as in the picture: the same pose, direction, size and "
    "proportions. Keep the picture's colours: every part keeps the colour it has there. Crisp and clean, alone on " BG
    ", no shadow on the ground, no text, no border.";
/* "tags": Stable Diffusion's kind of prompt (short phrases), for models that repaint the item's picture */
static const char *SD_T = "pixel art, game item icon, {d}, single item, centered, crisp clean pixels, bold dark outline, "
    "flat solid bright magenta background, no text, no border";
static const char *std_prompt(int kind) { return kind == 1 ? KONTEXT_T : kind == 2 ? SD_T : KLEIN_T; }

/* ---------------------------------------------------------------- small helpers */
static char WORK[MAX_PATH];                 /* out\pick beside the exe */
static char EXEDIR[MAX_PATH];               /* the folder voxup.exe is in: voxup.ini is there */
static char MODELS[MAX_PATH];               /* voxup.ini models = (else models beside the exe): models.txt and one ComfyUI graph per model */

static void *xmalloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) { vlog(VLOG_ERR, "mem", "out of memory (%u bytes)", (unsigned)n); exit(3); } return p; }
static int exists(const char *p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }
static void mkdirs(const char *p) {
    char b[MAX_PATH]; size_t i, n = strlen(p);
    if (n >= sizeof b) return;
    memcpy(b, p, n + 1);
    for (i = 3; i <= n; i++) if (b[i] == '\\' || b[i] == 0) { char c = b[i]; b[i] = 0; CreateDirectoryA(b, NULL); b[i] = c; }
}
static char *read_all(const char *p, size_t *len) {
    FILE *f = fopen(p, "rb"); long n; char *b;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    b = xmalloc((size_t)n + 1);
    if (n > 0 && fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f); b[n] = 0; if (len) *len = (size_t)n; return b;
}
static int write_all(const char *p, const void *d, size_t n) {
    FILE *f = fopen(p, "wb"); int ok;
    if (!f) { vlog(VLOG_ERR, "file", "could not write %s: %s", p, vlog_errno()); return 0; }
    ok = fwrite(d, 1, n, f) == n; ok &= fclose(f) == 0;
    if (!ok) vlog(VLOG_ERR, "file", "could not write %s: %s", p, vlog_errno());
    return ok;
}
static void append_line(const char *p, const char *line) {
    FILE *f = fopen(p, "ab");
    if (!f) { vlog(VLOG_ERR, "file", "could not add to %s: %s", p, vlog_errno()); return; }
    fputs(line, f); fputs("\r\n", f); fclose(f);
}
typedef struct { unsigned char *p; size_t n, cap; } Buf;
static void buf_add(Buf *b, const void *d, size_t n) {
    if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2; b->p = realloc(b->p, b->cap); if (!b->p) { vlog(VLOG_ERR, "mem", "out of memory"); exit(3); } }
    memcpy(b->p + b->n, d, n); b->n += n; b->p[b->n] = 0;
}
static void buf_str(Buf *b, const char *s) { buf_add(b, s, strlen(s)); }
static void png_cb(void *ctx, void *d, int n) { buf_add((Buf *)ctx, d, (size_t)n); }

/* ---------------------------------------------------------------- 32 x 32 from a drawing: the standard shrink, kept
   as the reference shrink.c is checked against (--shrinkcheck). Its resampling follows Pillow's (THIRD_PARTY.md), so it
   gives the same pixels as Pillow's LANCZOS on premultiplied "F" pictures. */
static double sincf(double x) { if (x == 0.0) return 1.0; x *= 3.14159265358979323846; return sin(x) / x; }
static double lanczos(double x) { if (-3.0 <= x && x < 3.0) return sincf(x) * sincf(x / 3); return 0.0; }
/* Pillow's precompute_coeffs (Resample.c), the Lanczos filter, a whole-picture box */
static int coeffs(int insz, int outsz, int **bounds, double **kk) {
    double scale = (double)insz / outsz, fs = scale < 1.0 ? 1.0 : scale, support = 3.0 * fs, ss = 1.0 / fs;
    int ks = (int)ceil(support) * 2 + 1, xx, x;
    *kk = xmalloc(sizeof(double) * (size_t)outsz * (size_t)ks); *bounds = xmalloc(sizeof(int) * 2 * (size_t)outsz);
    for (xx = 0; xx < outsz; xx++) {
        double center = (xx + 0.5) * scale, ww = 0.0, *k = *kk + (size_t)xx * ks;
        int xmin = (int)(center - support + 0.5), xmax;
        if (xmin < 0) xmin = 0;
        xmax = (int)(center + support + 0.5);
        if (xmax > insz) xmax = insz;
        xmax -= xmin;
        for (x = 0; x < xmax; x++) { double w = lanczos((x + xmin - center + 0.5) * ss); k[x] = w; ww += w; }
        for (x = 0; x < xmax; x++) if (ww != 0.0) k[x] /= ww;
        for (; x < ks; x++) k[x] = 0;
        (*bounds)[xx * 2] = xmin; (*bounds)[xx * 2 + 1] = xmax;
    }
    return ks;
}
/* Pillow's resize(LANCZOS) of a mode "F" picture: horizontal pass, then vertical, sums in double, stored as float */
static float *resize_f(const float *in, int iw, int ih, int ow, int oh) {
    float *t = NULL, *o; const float *src = in; int *b, ks, x, y, i; double *kk;
    if (ow != iw) {
        ks = coeffs(iw, ow, &b, &kk); t = xmalloc(sizeof(float) * (size_t)ow * ih);
        for (y = 0; y < ih; y++) for (x = 0; x < ow; x++) {
            double ss = 0.0; const double *k = kk + (size_t)x * ks;
            for (i = 0; i < b[x * 2 + 1]; i++) ss += in[(size_t)y * iw + b[x * 2] + i] * k[i];
            t[(size_t)y * ow + x] = (float)ss;
        }
        free(b); free(kk); src = t;
    }
    o = xmalloc(sizeof(float) * (size_t)ow * oh);
    if (oh != ih) {
        ks = coeffs(ih, oh, &b, &kk);
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
static int bbox_a(const unsigned char *p, int w, int h, int thr, int *x0, int *y0, int *x1, int *y1) {
    int x, y; *x0 = w; *y0 = h; *x1 = -1; *y1 = -1;
    for (y = 0; y < h; y++) for (x = 0; x < w; x++) if (p[((size_t)y * w + x) * 4 + 3] > thr) {
        if (x < *x0) *x0 = x; if (y < *y0) *y0 = y; if (x > *x1) *x1 = x; if (y > *y1) *y1 = y;
    }
    if (*x1 < 0) return 0;
    (*x1)++; (*y1)++; return 1;      /* end exclusive, as Pillow's getbbox */
}
static int cmp_u8(const void *a, const void *b) { return *(const unsigned char *)a - *(const unsigned char *)b; }
static int cmp_rgb(const void *a, const void *b) { const int *p = a, *q = b; return p[0] != q[0] ? p[0] - q[0] : p[1] != q[1] ? p[1] - q[1] : p[2] - q[2]; }
/* 4-neighbour count of set cells, nothing outside the picture */
static int nb4(const unsigned char *m, int w, int h, int x, int y) {
    return (y > 0 && m[(y - 1) * w + x]) + (y < h - 1 && m[(y + 1) * w + x]) + (x > 0 && m[y * w + x - 1]) + (x < w - 1 && m[y * w + x + 1]);
}

/* drawing: RGB dw x dh; orig: the sprite RGBA ow x oh. out: RGBA (2 ow) x (2 oh). 0 when there is nothing to cut out */
/* clamp 1: the original's own colours only; 0: the model's own
   colours kept, only near-black made pure black (lines, and the rim peel below); every other step the same */
static int to32(const unsigned char *drw, int dw, int dh, const unsigned char *orig, int ow, int oh, unsigned char *out, int clamp) {
    const int W = ow * 2, H = oh * 2;
    unsigned char ch[3][256], *cut; double med[3];
    int i, x, y, c, cx0, cy0, cx1, cy1, cw, chh, bx0, by0, bx1, by1, x0, y0, x1, y1, bw, bh, w, h, ox, oy, it;
    size_t n = (size_t)dw * dh;
    /* 1. cut-out: near the corners' colour, or plainly magenta */
    for (c = 0; c < 3; c++) {
        int k = 0;
        for (y = 0; y < 8; y++) for (x = 0; x < 8; x++) {
            ch[c][k++] = drw[((size_t)y * dw + x) * 3 + c]; ch[c][k++] = drw[((size_t)y * dw + dw - 8 + x) * 3 + c];
            ch[c][k++] = drw[((size_t)(dh - 8 + y) * dw + x) * 3 + c]; ch[c][k++] = drw[((size_t)(dh - 8 + y) * dw + dw - 8 + x) * 3 + c];
        }
        qsort(ch[c], 256, 1, cmp_u8); med[c] = (ch[c][127] + ch[c][128]) / 2.0;
    }
    cut = xmalloc(n * 4);
    for (i = 0; i < (int)n; i++) {
        int r = drw[i * 3], g = drw[i * 3 + 1], b = drw[i * 3 + 2];
        double d = sqrt((r - med[0]) * (r - med[0]) + (g - med[1]) * (g - med[1]) + (b - med[2]) * (b - med[2]));
        int bg = d < 90 || (r - g > 90 && b - g > 90);
        cut[i * 4] = (unsigned char)r; cut[i * 4 + 1] = (unsigned char)g; cut[i * 4 + 2] = (unsigned char)b; cut[i * 4 + 3] = bg ? 0 : 255;
    }
    if (!bbox_a(cut, dw, dh, 0, &cx0, &cy0, &cx1, &cy1) || !bbox_a(orig, ow, oh, 0, &bx0, &by0, &bx1, &by1)) { free(cut); return 0; }
    cw = cx1 - cx0; chh = cy1 - cy0;
    /* 2. fit to the original's box x 2, one pixel in; premultiplied Lanczos; kept where a quarter is covered */
    x0 = bx0 * 2 > 1 ? bx0 * 2 : 1; y0 = by0 * 2 > 1 ? by0 * 2 : 1;
    x1 = bx1 * 2 < W - 1 ? bx1 * 2 : W - 1; y1 = by1 * 2 < H - 1 ? by1 * 2 : H - 1;
    bw = x1 - x0; bh = y1 - y0;
    {
        double s = (double)bw / cw < (double)bh / chh ? (double)bw / cw : (double)bh / chh;
        float *pl[4], *rs[4];
        w = (int)nearbyint(cw * s); if (w < 1) w = 1;
        h = (int)nearbyint(chh * s); if (h < 1) h = 1;
        for (c = 0; c < 4; c++) pl[c] = xmalloc(sizeof(float) * (size_t)cw * chh);
        for (y = 0; y < chh; y++) for (x = 0; x < cw; x++) {
            const unsigned char *q = cut + ((size_t)(cy0 + y) * dw + cx0 + x) * 4; double a = q[3];
            for (c = 0; c < 3; c++) pl[c][(size_t)y * cw + x] = (float)(q[c] * (a / 255));
            pl[3][(size_t)y * cw + x] = (float)a;
        }
        for (c = 0; c < 4; c++) { rs[c] = resize_f(pl[c], cw, chh, w, h); free(pl[c]); }
        memset(out, 0, (size_t)W * H * 4);
        ox = x0 + (int)floor((bw - w) / 2.0); oy = y0 + (int)floor((bh - h) / 2.0);
        for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
            size_t k = (size_t)y * w + x; float al = rs[3][k], m = al > 1.0f ? al : 1.0f;
            unsigned char px[4];
            for (c = 0; c < 3; c++) { float v = rs[c][k] * 255 / m; v = v < 0 ? 0 : v > 255 ? 255 : v; px[c] = (unsigned char)(int)(double)v; }
            px[3] = al >= 64 ? 255 : 0;
            if (!px[3]) px[0] = px[1] = px[2] = 0;
            if (oy + y >= 0 && oy + y < H && ox + x >= 0 && ox + x < W) memcpy(out + ((size_t)(oy + y) * W + ox + x) * 4, px, 4);
        }
        for (c = 0; c < 4; c++) free(rs[c]);
    }
    free(cut);
    {
        unsigned char *on = xmalloc((size_t)W * H), *m2 = xmalloc((size_t)W * H);
        int (*cols)[3] = xmalloc(sizeof(int[3]) * (size_t)ow * oh), nc = 0, ninner = 0;
        /* 3. pinholes: an empty pixel with three or four filled neighbours takes their mean (twice) */
        for (it = 0; it < 2; it++) {
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
        /* 4. the original's own colours only (sorted as numpy's unique); pure black kept for drawn lines */
        for (i = 0; i < ow * oh; i++) if (orig[i * 4 + 3] >= 128) {
            int r = orig[i * 4], g = orig[i * 4 + 1], b = orig[i * 4 + 2], j;
            for (j = 0; j < nc; j++) if (cols[j][0] == r && cols[j][1] == g && cols[j][2] == b) break;
            if (j == nc) { cols[nc][0] = r; cols[nc][1] = g; cols[nc][2] = b; nc++; }
        }
        qsort(cols, (size_t)nc, sizeof cols[0], cmp_rgb);
        for (i = 0; i < nc; i++) ninner += cols[i][0] + cols[i][1] + cols[i][2] > 0;
        if (ninner >= 2) { int j = 0; for (i = 0; i < nc; i++) if (cols[i][0] + cols[i][1] + cols[i][2] > 0) { cols[j][0] = cols[i][0]; cols[j][1] = cols[i][1]; cols[j][2] = cols[i][2]; j++; } nc = j; }
        if (nc) for (i = 0; i < W * H; i++) if (out[i * 4 + 3] >= 128) {
            unsigned char *p = out + (size_t)i * 4; int best = 0, j; long bd = -1;
            for (j = 0; j < nc; j++) {
                long d = (long)(p[0] - cols[j][0]) * (p[0] - cols[j][0]) + (long)(p[1] - cols[j][1]) * (p[1] - cols[j][1]) + (long)(p[2] - cols[j][2]) * (p[2] - cols[j][2]);
                if (bd < 0 || d < bd) { bd = d; best = j; }
            }
            if (p[0] < 48 && p[1] < 48 && p[2] < 48) p[0] = p[1] = p[2] = 0;
            else if (clamp || (p[0] - p[1] > 20 && p[2] - p[1] > 20)) {   /* model's colours: pink from the background still snapped */ p[0] = (unsigned char)cols[best][0]; p[1] = (unsigned char)cols[best][1]; p[2] = (unsigned char)cols[best][2]; }
        }
        /* 5. the model's own dark rim peeled off (black pixels touching the outside or the picture's edge), up to 4 layers */
        for (it = 0; it < 4; it++) {
            int any = 0;
            for (y = 0; y < H; y++) for (x = 0; x < W; x++) {
                int k = y * W + x; on[k] = out[k * 4 + 3] > 127;
                m2[k] = !on[k] || x == 0 || y == 0 || x == W - 1 || y == H - 1;
            }
            for (y = 0; y < H; y++) for (x = 0; x < W; x++) {
                int k = y * W + x; unsigned char *p = out + (size_t)k * 4;
                if (on[k] && nb4(m2, W, H, x, y) > 0 && !p[0] && !p[1] && !p[2]) { p[0] = p[1] = p[2] = p[3] = 0; any = 1; }
            }
            if (!any) break;
        }
        /* 6. exactly one black pixel of outline round it */
        for (i = 0; i < W * H; i++) on[i] = out[i * 4 + 3] > 127;
        for (y = 0; y < H; y++) for (x = 0; x < W; x++) {
            int k = y * W + x;
            if (!on[k] && nb4(on, W, H, x, y) > 0) { out[k * 4] = out[k * 4 + 1] = out[k * 4 + 2] = 0; out[k * 4 + 3] = 255; }
        }
        free(on); free(m2); free(cols);
    }
    return 1;
}

static unsigned char *load_rgba(const char *p, int *w, int *h) {
    int n; unsigned char *d = stbi_load(p, w, h, &n, 4);
    if (!d) vlog(VLOG_ERR, "png", "could not read %s: %s", p, stbi_failure_reason());
    return d;
}
static int to32_cli(int argc, char **argv) {
    char op[MAX_PATH]; int dw, dh, ow, oh, n; unsigned char *d, *o, out[64 * 64 * 4];
    int clamp = !(argc == 6 && !strcmp(argv[5], "--own"));
    if (argc != 5 && clamp) { fprintf(stderr, "voxup --to32 DRAWING.png ITEM OUT.png [--own]   (--own: the model's own colours)\n"); return 255; }
    snprintf(op, sizeof op, "%s\\%03d.png", P_SPRITES, atoi(argv[3]));
    d = stbi_load(argv[2], &dw, &dh, &n, 3);
    if (!d) { vlog(VLOG_ERR, "to32", "could not read %s: %s", argv[2], stbi_failure_reason()); return 1; }
    if (!(o = load_rgba(op, &ow, &oh))) return 1;
    if (ow > 32 || oh > 32 || !to32(d, dw, dh, o, ow, oh, out, clamp)) { vlog(VLOG_ERR, "to32", "%s: nothing to cut out", argv[2]); return 1; }
    if (!stbi_write_png(argv[4], ow * 2, oh * 2, 4, out, ow * 8)) { vlog(VLOG_ERR, "to32", "could not write %s", argv[4]); return 1; }
    return 0;
}

/* voxup --shrink DRAWING ITEM OUT.png [key=value ...]: one drawing through shrink.c with those settings */
static int shrink_cli(int argc, char **argv) {
    char op[MAX_PATH]; int dw, dh, ow, oh, n, i, W, H; unsigned char *d, *o, out[128 * 128 * 4]; ShrinkOpt so;
    if (argc < 5) { fprintf(stderr, "voxup --shrink DRAWING.png ITEM OUT.png [key=value ...]\nkeys:"); for (i = 0; SHRINK_KEYS[i]; i++) fprintf(stderr, " %s", SHRINK_KEYS[i]); fprintf(stderr, "\n"); return 255; }
    shrink_defaults(&so);
    for (i = 5; i < argc; i++) { char k[40]; int v; if (sscanf(argv[i], "%39[^=]=%d", k, &v) != 2 || !shrink_set(&so, k, v)) { vlog(VLOG_ERR, "shrink", "not a setting: %s", argv[i]); return 255; } }
    snprintf(op, sizeof op, "%s\\%03d.png", P_SPRITES, atoi(argv[3]));
    if (!(d = stbi_load(argv[2], &dw, &dh, &n, 3))) { vlog(VLOG_ERR, "shrink", "could not read %s: %s", argv[2], stbi_failure_reason()); return 1; }
    if (!(o = load_rgba(op, &ow, &oh))) return 1;
    shrink_size(&so, ow, oh, &W, &H);
    if (!shrink(d, dw, dh, o, ow, oh, &so, out)) { vlog(VLOG_ERR, "shrink", "%s: nothing to cut out", argv[2]); return 1; }
    if (!stbi_write_png(argv[4], W, H, 4, out, W * 4)) { vlog(VLOG_ERR, "shrink", "could not write %s", argv[4]); return 1; }
    return 0;
}
/* "turn=auto|DEG style=classic|centred slim=off|sword|shaft outline=auto|on|off" into o; 0 for a word it does not know */
static int upright_set(UprightOpt *o, const char *kv) {
    char k[16], v[24];
    if (sscanf(kv, "%15[^=]=%23s", k, v) != 2) return 0;
    if (!strcmp(k, "turn")) { if (!strcmp(v, "auto") || !strcmp(v, "force")) o->auto_turn = 1; else { o->auto_turn = 0; o->turn = atof(v); } }
    else if (!strcmp(k, "style")) o->centred = !strcmp(v, "centred");
    else if (!strcmp(k, "slim")) o->slim = !strcmp(v, "sword") ? UP_SLIM_SWORD : !strcmp(v, "shaft") ? UP_SLIM_SHAFT : UP_SLIM_OFF;
    else if (!strcmp(k, "outline")) o->outline = !strcmp(v, "on") ? 1 : !strcmp(v, "off") ? 0 : -1;
    else return 0;
    return 1;
}
/* voxup --upright SPRITE.png OUT.png [turn=..] [style=..] [slim=..] [outline=..] [big=PX]: the sprite stood upright at
   32 x 32 (with big=PX: the large clean picture the models get instead) */
static int upright_cli(int argc, char **argv) {
    UprightOpt o; int w, h, i, big = 0, ok; unsigned char *d, out[32 * 32 * 4]; double turn = 0, scale = 0;
    if (argc < 4) { fprintf(stderr, "voxup --upright SPRITE.png OUT.png [turn=auto|force|DEG] [style=classic|centred] [slim=off|sword|shaft] [outline=auto|on|off] [big=PX]\n"); return 255; }
    upright_defaults(&o);
    for (i = 4; i < argc; i++) {
        if (!strncmp(argv[i], "big=", 4)) big = atoi(argv[i] + 4);
        else if (!upright_set(&o, argv[i])) { vlog(VLOG_ERR, "upright", "not a setting: %s", argv[i]); return 255; }
    }
    if (!(d = load_rgba(argv[2], &w, &h))) return 1;
    {   int force = 0; for (i = 4; i < argc; i++) force |= !strcmp(argv[i], "turn=force");
        if (o.auto_turn && !force) {     /* auto: as the window does, only an item that lies tilted (A17) */
            double t, el; int tl = 0;
            if (!upright_measure(d, w, h, &t, &el, &tl) || !tl) { printf("not tilted (long axis %.1f degrees off level): not turned; turn=force turns it anyway\n", t); stbi_image_free(d); return 0; }
        }
    }
    if (big > 0) {
        int bw, bh; unsigned char *b;
        if (o.auto_turn) { double t, el; int tl; upright_measure(d, w, h, &t, &el, &tl); o.turn = -t; }
        b = upright_big(d, w, h, o.turn, big, &bw, &bh);
        ok = b && stbi_write_png(argv[3], bw, bh, 4, b, bw * 4); free(b);
    } else {
        if (!upright_make(d, w, h, &o, out, &turn, &scale)) { vlog(VLOG_ERR, "upright", "%s: nothing to stand up", argv[2]); stbi_image_free(d); return 1; }
        ok = stbi_write_png(argv[3], 32, 32, 4, out, 32 * 4);
        printf("turn %.1f scale %.2f\n", turn, scale);
    }
    stbi_image_free(d);
    if (!ok) { vlog(VLOG_ERR, "upright", "could not write %s", argv[3]); return 1; }
    return 0;
}
/* ---------------------------------------------------------------- palettes (B8): Colours "A palette" maps every pixel
   to one of a palette's colours: a file (.hex RRGGBB lines (Lospec), GIMP .gpl, Paint.NET .txt AARRGGBB, or a .png:
   its colours), a built-in retro one, or the set's colours (every colour of the originals the Find box lets through) */
static const char *const PAL_NAMES[PAL_COUNT] = { "A palette file", "EGA 16", "PICO-8", "Game Boy", "C64", "The set's colours (Find)" };
static const char *const PAL_HEX[PAL_COUNT] = { "",
    "000000 0000AA 00AA00 00AAAA AA0000 AA00AA AA5500 AAAAAA 555555 5555FF 55FF55 55FFFF FF5555 FF55FF FFFF55 FFFFFF",
    "000000 1D2B53 7E2553 008751 AB5236 5F574F C2C3C7 FFF1E8 FF004D FFA300 FFEC27 00E436 29ADFF 83769C FF77A8 FFCCAA",
    "0F380F 306230 8BAC0F 9BBC0F",
    "000000 FFFFFF 68372B 70A4B2 6F3D86 588D43 352879 B8C76F 6F4F25 433900 9A6759 444444 6C6C6C 9AD284 6C5EB5 959595", "" };
static int pal_add(unsigned char *rgb, int n, int r, int g, int b) {
    int i; for (i = 0; i < n; i++) if (rgb[i * 3] == r && rgb[i * 3 + 1] == g && rgb[i * 3 + 2] == b) return n;
    if (n >= 256) return n; rgb[n * 3] = (unsigned char)r; rgb[n * 3 + 1] = (unsigned char)g; rgb[n * 3 + 2] = (unsigned char)b; return n + 1;
}
static int pal_hexlist(const char *t, unsigned char *rgb) {     /* RRGGBB words (a # or AA before them allowed) */
    int n = 0; const char *p = t;
    while (*p) { unsigned v; int len = 0;
        while (*p && !isxdigit((unsigned char)*p)) { if (*p == ';') { while (*p && *p != '\n') p++; } else p++; }
        while (isxdigit((unsigned char)p[len])) len++;
        if (len == 6 || len == 8) { sscanf(p, "%x", &v); n = pal_add(rgb, n, (int)((v >> 16) & 255), (int)((v >> 8) & 255), (int)(v & 255)); }
        p += len ? len : 1;
    }
    return n;
}
static int pal_file(const char *path, unsigned char *rgb) {
    const char *e = strrchr(path, '.'); char *t; int n = 0;
    if (e && !_stricmp(e, ".png")) {
        int w, h, i; unsigned char *d = load_rgba(path, &w, &h);
        if (!d) { vlog(VLOG_WARN, "palette", "could not read %s", path); return 0; }
        for (i = 0; i < w * h; i++) if (d[i * 4 + 3] >= 128) n = pal_add(rgb, n, d[i * 4], d[i * 4 + 1], d[i * 4 + 2]);
        stbi_image_free(d);
    } else if (!(t = read_all(path, NULL))) { vlog(VLOG_WARN, "palette", "could not read %s: %s", path, vlog_errno()); return 0; }
    else if (strncmp(t, "GIMP Palette", 12) == 0) {      /* .gpl: "R G B name" lines after the header */
        char *l; for (l = strtok(t, "\r\n"); l; l = strtok(NULL, "\r\n")) { int r, g, b; if (*l != '#' && sscanf(l, "%d %d %d", &r, &g, &b) == 3) n = pal_add(rgb, n, r & 255, g & 255, b & 255); }
        free(t);
    } else { n = pal_hexlist(t, rgb); free(t); }
    if (!n) vlog(VLOG_WARN, "palette", "%s: no colours found in it", path);
    return n;
}
static int item_fits(int k);
/* the palette for o's palid, into shrink.c; returns how many colours */
static int palette_apply(const ShrinkOpt *o, int nlist, const int *list) {
    unsigned char rgb[256 * 3]; int n = 0, i;
    if (o->palid == PAL_FILE) { if (P_PALETTE[0]) n = pal_file(P_PALETTE, rgb); }
    else if (o->palid == PAL_SET) {
        for (i = 0; i < nlist; i++) { char p[MAX_PATH]; int w, h, j; unsigned char *d;
            if (!item_fits(list[i])) continue;
            snprintf(p, sizeof p, "%s\\%03d.png", P_SPRITES, list[i]);
            if (!(d = load_rgba(p, &w, &h))) continue;
            for (j = 0; j < w * h; j++) if (d[j * 4 + 3] >= 128) n = pal_add(rgb, n, d[j * 4], d[j * 4 + 1], d[j * 4 + 2]);
            stbi_image_free(d); }
    } else n = pal_hexlist(PAL_HEX[o->palid], rgb);
    shrink_set_palette(rgb, n);
    return n;
}
/* voxup --shrinkall SEEDSDIR OUTDIR [key=value ...]: every drawing there (SEEDSDIR\NNN\MODEL-sSEED.png) shrunk with
   those settings into OUTDIR\NNN-MODEL-sSEED.png, for measuring settings against each other (B7) */
static int shrinkall_cli(int argc, char **argv) {
    WIN32_FIND_DATAA fd, fi; HANDLE h, hi; char pat[MAX_PATH]; int total = 0, i; ShrinkOpt so;
    if (argc < 4) { fprintf(stderr, "voxup --shrinkall SEEDSDIR OUTDIR [key=value ...]\n"); return 255; }
    shrink_defaults(&so);
    for (i = 4; i < argc; i++) { char k[40]; int v;
        if (!strncmp(argv[i], "palfile=", 8)) { snprintf(P_PALETTE, MAX_PATH, "%s", argv[i] + 8); continue; }
        if (sscanf(argv[i], "%39[^=]=%d", k, &v) != 2 || !shrink_set(&so, k, v)) { vlog(VLOG_ERR, "shrink", "not a setting: %s", argv[i]); return 255; } }
    if (so.colours == C_PALETTE) printf("palette: %d colours\n", palette_apply(&so, 0, NULL));
    mkdirs(argv[3]);
    snprintf(pat, sizeof pat, "%s\\*", argv[2]);
    if ((h = FindFirstFileA(pat, &fd)) == INVALID_HANDLE_VALUE) { vlog(VLOG_ERR, "check", "nothing in %s", argv[2]); return 1; }
    do {
        char sp[MAX_PATH]; int ow, oh, item; unsigned char *orig;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !isdigit((unsigned char)fd.cFileName[0])) continue;
        item = atoi(fd.cFileName); snprintf(sp, sizeof sp, "%s\\%03d.png", P_SPRITES, item);
        if (!(orig = load_rgba(sp, &ow, &oh))) continue;
        snprintf(pat, sizeof pat, "%s\\%s\\*-s*.png", argv[2], fd.cFileName);
        if ((hi = FindFirstFileA(pat, &fi)) != INVALID_HANDLE_VALUE) {
            do {
                char p[MAX_PATH], op[MAX_PATH]; int dw, dh, n, W, H; unsigned char *d, b[128 * 128 * 4];
                if (strstr(fi.cFileName, "-32")) continue;
                snprintf(p, sizeof p, "%s\\%s\\%s", argv[2], fd.cFileName, fi.cFileName);
                if (!(d = stbi_load(p, &dw, &dh, &n, 3))) continue;
                memset(b, 0, sizeof b);
                if (shrink(d, dw, dh, orig, ow, oh, &so, b)) {
                    shrink_size(&so, ow, oh, &W, &H);
                    snprintf(op, sizeof op, "%s\\%03d-%s", argv[3], item, fi.cFileName);
                    if (stbi_write_png(op, W, H, 4, b, W * 4)) total++;
                }
                stbi_image_free(d);
            } while (FindNextFileA(hi, &fi));
            FindClose(hi);
        }
        stbi_image_free(orig);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    printf("%d sprites written to %s\n", total, argv[3]);
    return 0;
}
/* voxup --shrinkcheck SEEDSDIR: shrink.c with its defaults against to32 (both colour ways) on every drawing there */
static int shrinkcheck_cli(int argc, char **argv) {
    WIN32_FIND_DATAA fd, fi; HANDLE h, hi; char pat[MAX_PATH]; int total = 0, bad = 0;
    if (argc < 3) { fprintf(stderr, "voxup --shrinkcheck SEEDSDIR (SEEDSDIR\\NNN\\MODEL-sSEED.png)\n"); return 255; }
    snprintf(pat, sizeof pat, "%s\\*", argv[2]);
    if ((h = FindFirstFileA(pat, &fd)) == INVALID_HANDLE_VALUE) { vlog(VLOG_ERR, "check", "nothing in %s", argv[2]); return 1; }
    do {
        char sp[MAX_PATH]; int ow, oh, item; unsigned char *orig;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !isdigit((unsigned char)fd.cFileName[0])) continue;
        item = atoi(fd.cFileName); snprintf(sp, sizeof sp, "%s\\%03d.png", P_SPRITES, item);
        if (!(orig = load_rgba(sp, &ow, &oh))) continue;
        snprintf(pat, sizeof pat, "%s\\%s\\*-s*.png", argv[2], fd.cFileName);
        if ((hi = FindFirstFileA(pat, &fi)) != INVALID_HANDLE_VALUE) {
            do {
                char p[MAX_PATH]; int dw, dh, n, way; unsigned char *d, a[128 * 128 * 4], b[128 * 128 * 4]; ShrinkOpt so;
                if (strstr(fi.cFileName, "-32")) continue;
                snprintf(p, sizeof p, "%s\\%s\\%s", argv[2], fd.cFileName, fi.cFileName);
                if (!(d = stbi_load(p, &dw, &dh, &n, 3))) continue;
                for (way = 0; way < 2; way++) {
                    int r1, r2;
                    shrink_defaults(&so); if (way) so.colours = C_OWN;
                    memset(a, 0, sizeof a); memset(b, 0, sizeof b);
                    r1 = to32(d, dw, dh, orig, ow, oh, a, !way); r2 = shrink(d, dw, dh, orig, ow, oh, &so, b);
                    total++;
                    if (r1 != r2 || memcmp(a, b, (size_t)ow * oh * 16)) { bad++; printf("DIFFER %s (%s)\n", p, way ? "model's colours" : "original's colours"); }
                }
                stbi_image_free(d);
            } while (FindNextFileA(hi, &fi));
            FindClose(hi);
        }
        stbi_image_free(orig);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    printf("%d sprites compared, %d differ\n", total, bad);
    return bad ? 1 : 0;
}

/* ---------------------------------------------------------------- ComfyUI over HTTP (WinHTTP) */
static HINTERNET hs, hc;
static int http(const wchar_t *verb, const char *path, const void *body, DWORD blen, const wchar_t *hdr, Buf *out, DWORD tmo) {
    wchar_t wp[2048]; HINTERNET rq; DWORD code = 0, sz = sizeof code, got;
    if (!hs) { hs = WinHttpOpen(L"voxup", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0); hc = hs ? WinHttpConnect(hs, L"127.0.0.1", PORT, 0) : NULL; }
    if (!hc) return 0;
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 2048);
    rq = WinHttpOpenRequest(hc, verb, wp, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!rq) return 0;
    WinHttpSetTimeouts(rq, (int)tmo, (int)tmo, (int)tmo, (int)tmo);
    if (!WinHttpSendRequest(rq, hdr ? hdr : WINHTTP_NO_ADDITIONAL_HEADERS, hdr ? (DWORD)-1L : 0, (LPVOID)body, blen, blen, 0) || !WinHttpReceiveResponse(rq, NULL)) { WinHttpCloseHandle(rq); return 0; }
    WinHttpQueryHeaders(rq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
    if (out) {
        out->n = 0;
        for (;;) {
            char b[65536];
            if (!WinHttpReadData(rq, b, sizeof b, &got) || !got) break;
            buf_add(out, b, got);
        }
        if (!out->p) buf_add(out, "", 0);
    }
    WinHttpCloseHandle(rq);
    return (int)code;
}
/* the string value of "key" in a JSON text, from `from` on; 0 if not there */
static const char *jstr(const char *j, const char *key, char *o, int n) {
    char k[64]; const char *p; int i = 0;
    snprintf(k, sizeof k, "\"%s\"", key);
    if (!j || !(p = strstr(j, k))) return NULL;
    p += strlen(k);
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') return NULL;
    for (p++; *p && *p != '"' && i < n - 1; p++) { if (*p == '\\' && p[1]) p++; o[i++] = *p; }
    o[i] = 0; return p;
}
static void json_esc(Buf *b, const char *s) {
    buf_str(b, "\"");
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s; char e[8];
        if (c == '"' || c == '\\') { e[0] = '\\'; e[1] = (char)c; buf_add(b, e, 2); }
        else if (c < 0x20) { snprintf(e, sizeof e, "\\u%04x", c); buf_str(b, e); }
        else buf_add(b, s, 1);
    }
    buf_str(b, "\"");
}
static void url_esc(char *o, int n, const char *s) {
    int i = 0;
    for (; *s && i < n - 4; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') o[i++] = (char)c;
        else i += snprintf(o + i, (size_t)(n - i), "%%%02X", c);
    }
    o[i] = 0;
}

/* ---------------------------------------------------------------- the state */
enum { ST_EMPTY, ST_WAIT, ST_DRAW, ST_DONE, ST_FAIL, ST_STOP };
/* a shot: its sprite as the settings make it (res, rw x rh, made with settings generation gen), the drawing kept in memory
   while its round is shown (drw), the window's picture of it (bm, of generation bmgen) */
/* src: what the model was given, so the drawing is shrunk into that picture's box (FROM_UP: the upright one) */
enum { FROM_TOOL, FROM_UP, FROM_ORIG };
typedef struct { int model; unsigned seed; int src; volatile int st; unsigned char *res; int rw, rh; volatile LONG gen; HBITMAP bm; LONG bmgen;
                 unsigned char *drw; int dw, dh; char err[200]; } Shot;
#define MAXSHOT 40
typedef struct { int n; Shot s[MAXSHOT]; } Round;
#define MAXR 200
/* a model of models.txt: its graph, the files it needs, how many pictures a round, its prompt */
#define MAXM 24
#define MAXF 6
typedef struct { char dir[32], name[112], url[512], sha[65]; long long size; } MFile;     /* sha: its SHA-256 (hex), checked after Get */
/* the built-in engine's lines of a model ("sd ctx KEY VALUE", "sd img KEY VALUE"): where 'c' context, 'i' each picture */
#define MAXSD 24
typedef struct { char where, key[64], val[600]; } SdLine;
typedef struct {
    char id[24], name[40], note[240]; int edit; char *graph; MFile f[MAXF]; int nf;
    char licu[4][200], licn[4][80]; int nlic, gated, recommended;     /* its files' licences (asked once before Get); needs a login; the one to get first */
    int n, ready; char prompt[4096];
    /* the built-in engine: its lines; how the item's picture goes in (1 a reference picture, 2 the start picture, so
       strength says how much may change); a LoRA (models.txt "sd lora DIR NAME") at lora_w % */
    SdLine sd[MAXSD]; int nsd, sdimage, strength, has_lora, lora_w; char lora[MAX_PATH];
} Model;
static struct {
    int list[512], nlist, pos;                 /* the items with a 16 x 16 original, minus voxup.ini's skip list */
    char name[512][48]; char *desc[512]; unsigned char kept[512];
    int item;
    Round *r; int nr, cur, sel;                 /* the item's rounds, the one shown, the chosen shot (-1) */
    char descbuf[4096];                         /* what it is */
    Model m[MAXM]; int nm, pmodel;
    ShrinkOpt opt; volatile LONG optgen;        /* the shrink settings (shrink.h) and their generation */
    volatile LONG rs_busy, rs_abort; int tab;   /* the reshaping thread; the right panel: 0 chosen, 1 settings */              /* the models; the one whose prompt is shown */
    volatile LONG dl_busy, dl_stop; int dl_model; volatile LONGLONG dl_done, dl_total; char dl_msg[300];
    unsigned char orig[16 * 16 * 4], tool[32 * 32 * 4], up[32 * 32 * 4];
    unsigned char upfit[16 * 16 * 4];           /* the upright picture's box at 16 x 16, in the original's colours */
    unsigned char upfit32[32 * 32 * 4];         /* the same box at 32 x 32, exact (not snapped to the 16 grid; A17) */
    int has_up, from, upfrom;                   /* an upright picture found; what the models are given (FROM_*) */
    /* the upright picture: hand-made (an upright = folder) or made here (NNN\upright.png, settings in NNN\upright.txt) */
    int up_hand, up_mode, up_style, up_slim;    /* UPM_*, 0 classic / 1 centred, UP_SLIM_* */
    double up_turn; char up_note[80];           /* the turn it was made with; why there is none */
    HBITMAP bm_orig, bm_tool, bm_up, pv_up, pv_draw, pv_small, pv_orig; int pv_for, pv_w, pv_h; LONG pv_gen;
    HBITMAP bm_mini;                            /* the original at 32 x 32, in each picture's corner */
    int undo_item, undo_had[4]; char undo_stamp[32];   /* the keep being undone: the item, which game pictures it replaced */
    struct { int kind, item, had[4]; char stamp[32]; ShrinkOpt opt; } undo[64]; int nundo;   /* B5: 1 a keep, 2 the settings before a change */
    char keptinfo[200];
    volatile LONG busy, stop; int drawing_round;
    CRITICAL_SECTION cs; char status[600];
    HWND wnd; HANDLE comfy, job; int upitem; char upname[128];
    int engine; char sdstep[40];                /* 0: models with sd lines drawn by the built-in engine, 1: all by ComfyUI */
    /* 2026-11-21, fewer clicks: keys (none while a text box is typed in), the item filter, the queue, presets,
       the item's own settings, the Compare tab, each picture's progress */
    int typing, find_focus, find_typing, auto_next; char find[48]; signed char tilt[512];   /* tilt: -1 not measured */
    int desc_made; const char *desc_word;
    char problem[400]; int prob_seen;          /* the last WARN/ERR, shown under the status until clicked (review A11) */      /* the upright description was made from the other; a tilt word left in it */
    int queue[512], nq, qrun;                   /* items to draw a round of, in turn (work\queue.txt) */
    int own_opt; ShrinkOpt gopt; int opt_loaded; /* the item has its own settings (NNN\shrink.txt); everyone else's */
    char pname[16][32], pset[16][400]; int np, nbuiltin; char newpreset[32]; unsigned char phid[16];   /* phid: a Compare-only preset (not on the bar, no F key) */
    int cmp_for; LONG cmp_gen; HBITMAP cmp_bm[17], cmp_bm1[17]; int cmp_w[17], cmp_h[17], cmp_w1[17], cmp_h1[17];   /* ...; bm1: the same at 1x (A17) */ char cmp_score[17][64];
    volatile LONG sd_step, sd_steps; DWORD draw_t0; DWORD avg_ms[MAXM]; int nkeeps;
    int show_get, nget, unfocus; WINDOWPLACEMENT wp; int have_wp;   /* models to get shown; Esc in a text box; the window's place */
    /* the side panels' widths: wanted (pick.ini split=), shown; a divider dragged; the grid's columns; the panels'
       inner widths */
    int want[2], split[2], drag, drag_w, cur_we, per; float drag_x; int inl, ing, inr;
    int empty; char empty_msg[400];             /* no sprites yet: the window asks for the folder (one exe) */
    int lic_ask, need_token; char tok[256];     /* Get waits for the licence click (model + 1); a Hugging Face token asked for (never saved in a file or log) */
    float ui;                                   /* the window's scale: its monitor's DPI / 96 (B9) */
    unsigned folds; int keys_open; int fold[2];   /* ...; a side panel folded to a strip (0 left, 1 right; pick.ini fold=) */              /* Settings sections folded (bit per section); the key list shown */
    unsigned prob_gone[32]; int nprob_gone; float prob_gap; struct nk_rect prob_rect;   /* Problem texts clicked away (not shown again); the room it kept */
} S;
/* drawn inside voxup: the built-in engine is in this voxup, the model has its lines, and the engine is chosen (or the
   model has no ComfyUI graph) */
static int use_sd(int m) { return SD_BUILT && S.m[m].nsd > 0 && (S.engine == 0 || !S.m[m].graph); }

static void status(const char *fmt, ...) {
    va_list a; char b[600];
    va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    EnterCriticalSection(&S.cs); strcpy(S.status, b); LeaveCriticalSection(&S.cs);
    vlog(VLOG_INFO, "pick", "%s", b);
    if (S.wnd) PostMessageA(S.wnd, WM_APP, 0, 0);
}
static void itemdir(int item, char *o, int n) { snprintf(o, (size_t)n, "%s\\%03d", WORK, item); }
static void shotpath(int item, const Shot *s, int small, char *o, int n) {
    char d[MAX_PATH]; itemdir(item, d, sizeof d);
    snprintf(o, (size_t)n, "%s\\%s-s%u%s.png", d, S.m[s->model].id, s->seed, small == 2 ? "-32u" : small ? "-32" : "");
}

/* ---------------------------------------------------------------- the models (models\models.txt) */
static int find_model(const char *id) { int i; for (i = 0; i < S.nm; i++) if (!strcmp(S.m[i].id, id)) return i; return -1; }
static void file_path(const MFile *f, char *o, int n) { snprintf(o, (size_t)n, "%s\\%s\\%s", P_SHARED, f->dir, f->name); }
static int file_there(const MFile *f) {
    char p[MAX_PATH]; file_path(f, p, sizeof p);
    if (exists(p)) return 1;
    if (!strcmp(f->dir, "text_encoders")) { snprintf(p, sizeof p, "%s\\clip\\%s", P_SHARED, f->name); return exists(p); }   /* the YAML reads both */
    return 0;
}
/* what a model still needs, in bytes (a file two models share counts once: it is one file) */
static long long model_missing(int m) { long long t = 0; int i; for (i = 0; i < S.m[m].nf; i++) if (!file_there(&S.m[m].f[i])) t += S.m[m].f[i].size; return t; }
/* a model this voxup can draw: by the built-in engine (its sd lines), or by ComfyUI when voxup.ini comfy = is set */
static int usable(int m) { return (SD_BUILT && S.m[m].nsd > 0) || (P_COMFY[0] && S.m[m].graph); }
static void check_ready(void) { int i; for (i = 0; i < S.nm; i++) S.m[i].ready = model_missing(i) == 0 && S.m[i].nf > 0 && usable(i); }
/* a file of the model list: the models folder's own (voxup.ini models =, else models\ beside the exe), else the copy
   built into voxup.exe (embed.c); p gets where it came from. NULL when neither has it. */
static char *model_file(const char *name, char *p, size_t np, int *built_in) {
    char *t; size_t n; const char *e;
    snprintf(p, np, "%s\\%s", MODELS, name);
    if ((t = read_all(p, NULL))) { if (built_in) *built_in = 0; return t; }
    if (!(e = embed_get(name, &n))) return NULL;
    t = xmalloc(n + 1); memcpy(t, e, n + 1); snprintf(p, np, "%s (built in)", name); if (built_in) *built_in = 1; return t;
}
static void load_models(void) {
    char p[MAX_PATH], *t, *l; Model *m = NULL; int nbuilt = 0, bi = 0;
    if (!(t = model_file("models.txt", p, sizeof p, &bi))) { vlog(VLOG_ERR, "models", "no model list: %s\\models.txt missing and none built in", MODELS); return; }
    nbuilt += bi;
    for (l = strtok(t, "\r\n"); l; l = strtok(NULL, "\r\n")) {
        char k[16] = "", *v; while (*l == ' ') l++;
        if (!*l || *l == '#') continue;
        sscanf(l, "%15s", k); v = l + strlen(k); while (*v == ' ') v++;
        if (!strcmp(k, "model")) {
            if (S.nm >= MAXM) { vlog(VLOG_WARN, "models", "more than %d models in %s: the rest left out", MAXM, p); break; }
            m = &S.m[S.nm++]; memset(m, 0, sizeof *m); snprintf(m->id, sizeof m->id, "%s", v); snprintf(m->name, sizeof m->name, "%s", v);
        } else if (!m) vlog(VLOG_WARN, "models", "%s: \"%s\" before the first model line", p, l);
        else if (!strcmp(k, "name")) snprintf(m->name, sizeof m->name, "%s", v);
        else if (!strcmp(k, "note")) snprintf(m->note, sizeof m->note, "%s", v);
        else if (!strcmp(k, "licence") && m->nlic < 4) { char u[200] = ""; int nn = 0; sscanf(v, "%199s %n", u, &nn);
            snprintf(m->licu[m->nlic], sizeof m->licu[0], "%s", u); snprintf(m->licn[m->nlic], sizeof m->licn[0], "%s", nn ? v + nn : u);
            { int j, dup = 0; for (j = 0; j < m->nlic; j++) dup |= !strcmp(m->licn[j], m->licn[m->nlic]); if (!dup) m->nlic++; } }
        else if (!strcmp(k, "gated")) m->gated = 1;
        else if (!strcmp(k, "recommended")) m->recommended = 1;
        else if (!strcmp(k, "prompt")) m->edit = !strcmp(v, "edit") ? 1 : !strcmp(v, "tags") ? 2 : 0;
        else if (!strcmp(k, "sd")) {       /* the built-in engine: "sd ctx|img KEY VALUE", "sd image ref|init", "sd strength N", "sd lora DIR NAME N" */
            char w[16] = "", key[64] = "", a[112] = ""; int nn = 0;
            sscanf(v, "%15s %63s %n", w, key, &nn);
            if (!strcmp(w, "image")) m->sdimage = !strcmp(key, "init") ? 2 : 1;
            else if (!strcmp(w, "strength")) m->strength = atoi(key);
            else if (!strcmp(w, "lora") && sscanf(v, "%*s %63s %111s %d", key, a, &m->lora_w) == 3) { snprintf(m->lora, sizeof m->lora, "%s\\%s", key, a); m->has_lora = 1; }
            else if ((!strcmp(w, "ctx") || !strcmp(w, "img")) && key[0] && nn && m->nsd < MAXSD) {
                SdLine *s = &m->sd[m->nsd++]; s->where = w[0]; snprintf(s->key, sizeof s->key, "%s", key); snprintf(s->val, sizeof s->val, "%s", v + nn);
            } else vlog(VLOG_WARN, "models", "%s: bad sd line for %s: %s", p, m->id, v);
        }
        else if (!strcmp(k, "file")) {
            MFile f; memset(&f, 0, sizeof f);
            if (m->nf >= MAXF || sscanf(v, "%31s %111s %lld %511s %64s", f.dir, f.name, &f.size, f.url, f.sha) < 4) vlog(VLOG_WARN, "models", "%s: bad file line for %s: %s", p, m->id, v);
            else m->f[m->nf++] = f;
        } else vlog(VLOG_WARN, "models", "%s: unknown line: %s", p, l);
    }
    free(t);
    { int i, j; for (i = j = 0; i < S.nm; i++) {      /* a model with neither a graph nor the built-in engine's lines cannot draw */
        int sd = SD_BUILT && S.m[i].nsd > 0;
        { char nm[40]; snprintf(nm, sizeof nm, "%s.json", S.m[i].id); S.m[i].graph = model_file(nm, p, sizeof p, &bi); if (S.m[i].graph) nbuilt += bi; }
        if (S.m[i].graph && (!strstr(S.m[i].graph, "\"@IMAGE@\"") || !strstr(S.m[i].graph, "\"@PROMPT@\"") || !strstr(S.m[i].graph, "\"@SEED@\""))) {
            vlog(VLOG_ERR, "models", "%s lacks \"@IMAGE@\", \"@PROMPT@\" or \"@SEED@\": not used", p); free(S.m[i].graph); S.m[i].graph = NULL; }
        if (!S.m[i].graph && !sd) { vlog(VLOG_ERR, "models", "model %s left out: no graph %s%s", S.m[i].id, p, SD_BUILT ? " and no sd lines" : " (and this voxup has no built-in engine)"); continue; }
        if (!S.m[i].strength) S.m[i].strength = 60;
        S.m[j++] = S.m[i];
    } S.nm = j; }
    check_ready();
    vlog(VLOG_INFO, "models", "%d models from %s (%d of the files built in)", S.nm, MODELS, nbuilt);
}

/* a picture for the window: RGBA w x h, each pixel scale x scale, on the sheets' checker (or as it is when opaque) */
static HBITMAP mkbmp(const unsigned char *rgba, int w, int h, int scale, int rgb3) {
    BITMAPINFO bi; unsigned char *bits; HBITMAP b; int W, H, x, y, row, q;
    {   /* the zoom on screen: the largest whole one that fits the picture's box at the window's scale (review 3, P2-2) */
        int zs = (int)(scale * (S.ui > 0 ? S.ui : 1.0f) + 0.001f); scale = zs > 0 ? zs : 1; }
    W = w * scale; H = h * scale;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 24; bi.bmiHeader.biCompression = BI_RGB;
    b = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    if (!b) return NULL;
    row = (W * 3 + 3) & ~3; q = W / 16 > 0 ? W / 16 : 1;
    for (y = 0; y < H; y++) for (x = 0; x < W; x++) {
        const unsigned char *p = rgb3 ? rgba + ((size_t)(y / scale) * w + x / scale) * 3 : rgba + ((size_t)(y / scale) * w + x / scale) * 4;
        int a = rgb3 ? 255 : p[3], ck = ((x / q) + (y / q)) & 1, bg[3], c;
        bg[0] = ck ? 58 : 70; bg[1] = ck ? 62 : 74; bg[2] = ck ? 70 : 84;
        for (c = 0; c < 3; c++) bits[(size_t)y * row + x * 3 + (2 - c)] = (unsigned char)((p[c] * a + bg[c] * (255 - a)) / 255);
    }
    return b;
}
static struct nk_image nkimg(HBITMAP b, int w, int h) {
    struct nk_image i = nk_image_ptr(b); i.w = (nk_ushort)w; i.h = (nk_ushort)h; i.region[0] = i.region[1] = 0; i.region[2] = (nk_ushort)w; i.region[3] = (nk_ushort)h; return i;
}

/* item names (strings block 4: "4: a_shortsword" -> "shortsword") and the written descriptions */
static void load_names(void) {
    char *t = P_NAMES[0] ? read_all(P_NAMES, NULL) : NULL, *l;
    if (!t) { if (P_NAMES[0]) vlog(VLOG_WARN, "pick", "no item names: %s missing", P_NAMES); return; }
    for (l = strtok(t, "\r\n"); l; l = strtok(NULL, "\r\n")) {
        char *c = strchr(l, ':'), *s, *e; int k;
        if (!c || l[0] < '0' || l[0] > '9' || strchr(l, ':') != c) continue;     /* "6:4: ..." are other forms */
        k = atoi(l); if (k < 0 || k >= 512) continue;
        s = c + 1; while (*s == ' ') s++;
        if ((e = strchr(s, '_')) && e - s <= 4) s = e + 1;
        if ((e = strchr(s, '&'))) *e = 0;
        snprintf(S.name[k], sizeof S.name[k], "%s", s);
    }
    free(t);
}
static void load_desc(const char *p) {
    char *t = read_all(p, NULL), *l;
    if (!t) { vlog(VLOG_WARN, "pick", "no descriptions: %s missing", p); return; }
    for (l = strtok(t, "\r\n"); l; l = strtok(NULL, "\r\n")) {
        char *bar = strchr(l, '|'); int k;
        if (l[0] == '#' || !bar) continue;
        k = atoi(l); if (k < 0 || k >= 512) continue;
        free(S.desc[k]); S.desc[k] = _strdup(bar + 1);
    }
    free(t);
}
/* the description shown wrapped (Nuklear's edit box does not wrap); joined again with spaces for the prompt */
static void wrap(const char *s, char *o, int n, int width) {
    int i = 0, col = 0;
    while (*s == ' ' || *s == '\r' || *s == '\n') s++;
    for (; *s && i < n - 2; s++) {
        char c = *s == '\r' || *s == '\n' ? ' ' : *s;
        if (c == ' ' && col >= width) { o[i++] = '\n'; col = 0; continue; }
        o[i++] = c; col++;
    }
    o[i] = 0;
}
static void unwrap(const char *s, char *o, int n) {
    int i = 0;
    for (; *s && i < n - 1; s++) { char c = *s == '\r' || *s == '\n' ? ' ' : *s; if (c == ' ' && i && o[i - 1] == ' ') continue; o[i++] = c; }
    while (i && o[i - 1] == ' ') i--;
    o[i] = 0;
}

/* a shot's sprite made again from its drawing with these settings (the drawing read once and kept) */
static void set_res(Shot *s, const unsigned char *px, int w, int h, LONG gen) {
    EnterCriticalSection(&S.cs);
    if (s->rw * s->rh != w * h || !s->res) { free(s->res); s->res = xmalloc((size_t)w * h * 4); }
    memcpy(s->res, px, (size_t)w * h * 4); s->rw = w; s->rh = h; s->gen = gen;
    LeaveCriticalSection(&S.cs);
}
/* the picture a drawing is shrunk to: its box sets the sprite's place, its colours the palette */
static const unsigned char *fitpic(int src) { return src == FROM_UP && S.has_up ? S.upfit : S.orig; }
/* the picture shrink() fits a drawing into, and its size: an upright drawing into the upright picture's own box at
   32 x 32 (A17: the 16 x 16 box snapped it to even pixels), the size setting kept: 32 x 32 is that box's own size,
   16 x 16 a custom 16 x 16 */
static const unsigned char *fit_for(int src, ShrinkOpt *oo, int *w, int *h) {
    if (src == FROM_UP && S.has_up) {
        if (oo->size == SZ_32) oo->size = SZ_16;
        else if (oo->size == SZ_16) { oo->size = SZ_CUSTOM; oo->cw = 16; oo->ch = 16; }
        *w = *h = 32; return S.upfit32;
    }
    *w = *h = 16; return S.orig;
}
/* Outline auto decided here: the box fitpic gives for an upright drawing has no black in it, so the upright picture
   itself is asked (the same rule, shrink.c orig_outlined) */
static void outline_for(ShrinkOpt *o, int src) {
    if (o->outline != O_AUTO) return;
    o->outline = (src == FROM_UP && S.has_up ? orig_outlined(S.up, 32, 32) : orig_outlined(S.orig, 16, 16)) ? O_BLACK4 : O_NONE;
}
static void reshape_one(int item, Shot *s, const ShrinkOpt *o, LONG gen) {
    unsigned char *out; int W, H;      /* its own buffer: the reshaper and the drawing thread both come here (review A1) */
    if (!s->drw) {
        char p[MAX_PATH]; int n; shotpath(item, s, 0, p, sizeof p);
        if (!(s->drw = stbi_load(p, &s->dw, &s->dh, &n, 3))) { vlog(VLOG_WARN, "shrink", "no drawing %s", p); s->gen = gen; return; }
    }
    shrink_size(o, 16, 16, &W, &H);
    { ShrinkOpt oo = *o; outline_for(&oo, s->src);
      out = xmalloc(128 * 128 * 4);
      { int fw, fh; const unsigned char *fp = fit_for(s->src, &oo, &fw, &fh);
        if (shrink(s->drw, s->dw, s->dh, fp, fw, fh, &oo, out)) set_res(s, out, W, H, gen); else s->gen = gen; }
      free(out); }
}
/* the reshaper's thread: its item is fixed when it starts (never S.item, which choose() changes), and stop_reshape
   waits for it to end before the rounds it works on are freed (review A2) */
static HANDLE RS_TH; static int RS_ITEM;
static DWORD WINAPI rs_worker(LPVOID arg) {
    (void)arg;
    for (;;) {
        ShrinkOpt o; LONG gen; int cur, i, r, did = 0, item = RS_ITEM;
        EnterCriticalSection(&S.cs); o = S.opt; gen = S.optgen; cur = S.cur; LeaveCriticalSection(&S.cs);
        if (S.rs_abort) break;
        for (r = 0; r < S.nr; r++) if (r != cur) for (i = 0; i < S.r[r].n; i++) {     /* only the shown round's drawings kept */
            Shot *s = &S.r[r].s[i];
            if (s->drw && s->st != ST_DRAW && !(S.busy && r == S.drawing_round)) { stbi_image_free(s->drw); s->drw = NULL; }
        }
        if (cur < S.nr) for (i = 0; i < S.r[cur].n; i++) {
            Shot *s = &S.r[cur].s[i];
            if (S.rs_abort || S.optgen != gen || S.cur != cur) break;
            if (s->st != ST_DONE || s->gen == gen) continue;
            reshape_one(item, s, &o, gen); did = 1;
            if (S.wnd) PostMessageA(S.wnd, WM_APP, 0, 0);
        }
        if (!did && S.optgen == gen && S.cur == cur) break;
    }
    InterlockedExchange(&S.rs_busy, 0);
    if (S.wnd) PostMessageA(S.wnd, WM_APP, 0, 0);
    return 0;
}
static void kick_reshape(void) {
    HANDLE th;
    if (InterlockedCompareExchange(&S.rs_busy, 1, 0) != 0) return;      /* running: it sees the change itself */
    S.rs_abort = 0; RS_ITEM = S.item;
    if (RS_TH) { CloseHandle(RS_TH); RS_TH = NULL; }      /* the last one has ended (rs_busy was 0) */
    th = CreateThread(NULL, 0, rs_worker, NULL, 0, NULL);
    if (th) RS_TH = th; else InterlockedExchange(&S.rs_busy, 0);
}
static void stop_reshape(void) {
    S.rs_abort = 1;
    if (RS_TH) { WaitForSingleObject(RS_TH, INFINITE); CloseHandle(RS_TH); RS_TH = NULL; }
    InterlockedExchange(&S.rs_busy, 0);
}
static void free_rounds(void) {
    int i, j;
    stop_reshape();
    for (i = 0; i < S.nr; i++) for (j = 0; j < S.r[i].n; j++) { Shot *s = &S.r[i].s[j]; if (s->bm) DeleteObject(s->bm); free(s->res); if (s->drw) stbi_image_free(s->drw); memset(s, 0, sizeof *s); }
    S.nr = 0; S.cur = 0; S.sel = -1; S.pv_for = -1;
    if (S.pv_draw) DeleteObject(S.pv_draw); if (S.pv_small) DeleteObject(S.pv_small);
    S.pv_draw = S.pv_small = NULL;
}
static int seed_used(unsigned seed) {
    int i, j;
    for (i = 0; i < S.nr; i++) for (j = 0; j < S.r[i].n; j++) if (S.r[i].s[j].seed == seed) return 1;
    return 0;
}
static void read_kept(int item) {
    char p[MAX_PATH], *t; itemdir(item, p, sizeof p); strcat(p, "\\kept.txt");
    S.keptinfo[0] = 0;
    if ((t = read_all(p, NULL))) { char *e = strpbrk(t, "\r\n"); if (e) *e = 0; snprintf(S.keptinfo, sizeof S.keptinfo, "Kept: %s", t); free(t); }
}
/* the description for what the models are given: desc-upright.txt for the upright picture, desc.txt for the others */
static void desc_path(int item, int from, char *p, int n) { itemdir(item, p, n); strcat(p, from == FROM_UP ? "\\desc-upright.txt" : "\\desc.txt"); }
/* a description made to fit the upright picture: the lying words swapped for standing ones (the models follow the
   words over the picture, so "lying diagonally" draws the sword diagonal again), else a sentence saying it stands */
static void swap_all(char *s, size_t n, const char *from, const char *to) {
    char *q = s; size_t lf = strlen(from), lt = strlen(to);
    while ((q = strstr(q, from))) {
        if (strlen(s) - lf + lt + 1 > n) return;
        memmove(q + lt, q + lf, strlen(q + lf) + 1); memcpy(q, to, lt); q += lt;
    }
}
static void upright_words(const char *in, char *out, size_t n) {
    static const char *SW[][2] = {
        { "lying diagonally", "standing straight upright" }, { "lies diagonally", "stands straight upright" },
        { "from the upper left to the lower right", "from the top to the bottom" },
        { "from the upper right to the lower left", "from the top to the bottom" },
        { "from the lower right to the upper left", "from the bottom to the top" },
        { "from the lower left to the upper right", "from the bottom to the top" },
        { "at the upper left", "at the top" }, { "at the lower right", "at the bottom" },
        { "at the upper right", "at the top" }, { "at the lower left", "at the bottom" },
        { "to the lower right", "to the bottom" }, { "to the lower left", "to the bottom" },
        { "to the upper left", "to the top" }, { "to the upper right", "to the top" },
        { "the upper left", "the top" }, { "the upper right", "the top" },
        { "the lower right", "the bottom" }, { "the lower left", "the bottom" },
        { "upper left", "top" }, { "upper right", "top" }, { "lower right", "bottom" }, { "lower left", "bottom" },
        { "diagonally", "upright" }, { "diagonal", "upright" },
        { "picture's length", "picture's height" } };
    size_t i;
    snprintf(out, n, "%s", in);
    if (!strstr(out, "diagonal")) {
        size_t l = strlen(out); while (l && (out[l - 1] == ' ' || out[l - 1] == '\n' || out[l - 1] == '\r')) out[--l] = 0;
        snprintf(out + l, n - l, "%sIt stands straight upright.", !l ? "" : strchr(".!?", out[l - 1]) ? " " : ". ");
        return;
    }
    for (i = 0; i < sizeof SW / sizeof SW[0]; i++) swap_all(out, n, SW[i][0], SW[i][1]);
}
/* a word left in an upright description that still says it lies tilted (review A8) */
static const char *tilt_word(const char *t) {
    static const char *W[] = { "upper left", "upper right", "lower left", "lower right", "diagonal", "lying", "slant", "tilted" };
    size_t i; for (i = 0; i < sizeof W / sizeof W[0]; i++) if (strstr(t, W[i])) return W[i];
    return NULL;
}
static void load_desc_text(int item) {
    char p[MAX_PATH], *t;
    desc_path(item, S.from, p, sizeof p);
    if (!(t = read_all(p, NULL)) && S.from == FROM_UP) {
        /* no upright description yet: the other one made upright (saved as desc-upright.txt when a round starts) */
        char *o = NULL; desc_path(item, FROM_TOOL, p, sizeof p);
        if ((o = read_all(p, NULL)) || S.desc[item]) {
            const char *src = o ? o : S.desc[item]; size_t n = strlen(src) + 200; t = xmalloc(n); upright_words(src, t, n); free(o);
            S.desc_made = 1; S.desc_word = tilt_word(t);
        }
    }
    if (t) { wrap(t, S.descbuf, sizeof S.descbuf, 34); free(t); }
    else if (S.desc[item]) wrap(S.desc[item], S.descbuf, sizeof S.descbuf, 34);
    else S.descbuf[0] = 0;
}
static const char *FROM_NAMES[] = { "tool", "upright", "original" };
static void save_from(int item) {
    char p[MAX_PATH]; itemdir(item, p, sizeof p); mkdirs(p); strcat(p, "\\from.txt");
    write_all(p, FROM_NAMES[S.from], strlen(FROM_NAMES[S.from]));
}
/* the upright picture made here: Auto (only when the original lies tilted; the tilt measured), turned 45 right or
   left, or Off; Classic (blade centre between two pixel columns) or Centred; the grip as it is or one pixel slimmer */
enum { UPM_AUTO, UPM_RIGHT, UPM_LEFT, UPM_OFF };
static const char *UPM_WORDS[] = { "auto", "right", "left", "off" };
static void up_settings(int item, int save) {
    char p[MAX_PATH], *t; static const char *SL[] = { "off", "sword", "shaft" };
    itemdir(item, p, sizeof p);
    if (save) mkdirs(p);
    strcat(p, "\\upright.txt");
    if (save) {
        char s[96];
        snprintf(s, sizeof s, "mode=%s style=%s slim=%s\n", UPM_WORDS[S.up_mode], S.up_style ? "centred" : "classic", SL[S.up_slim]);
        write_all(p, s, strlen(s)); return;
    }
    S.up_mode = UPM_AUTO; S.up_style = 0; S.up_slim = UP_SLIM_OFF;
    if ((t = read_all(p, NULL))) {
        char *w; int i;
        for (w = strtok(t, " \r\n"); w; w = strtok(NULL, " \r\n")) {
            if (!strncmp(w, "mode=", 5)) for (i = 0; i < 4; i++) if (!strcmp(w + 5, UPM_WORDS[i])) S.up_mode = i;
            if (!strncmp(w, "style=", 6)) S.up_style = !strcmp(w + 6, "centred");
            if (!strncmp(w, "slim=", 5)) for (i = 0; i < 3; i++) if (!strcmp(w + 5, SL[i])) S.up_slim = i;
        }
        free(t);
    }
}
/* made from the original with the item's settings into S.up (and NNN\upright.png); 0 = none (with the reason) */
static int up_make(int item) {
    UprightOpt o; char p[MAX_PATH]; double t, el; int tl, b0, b1, b2, b3;
    if (S.up_mode == UPM_OFF) { snprintf(S.up_note, sizeof S.up_note, "(turned off)"); return 0; }
    if (!bbox_a(S.orig, 16, 16, 1, &b0, &b1, &b2, &b3)) { snprintf(S.up_note, sizeof S.up_note, "(no original)"); return 0; }
    upright_defaults(&o); o.centred = S.up_style; o.slim = S.up_slim;
    if (S.up_mode == UPM_AUTO) {
        if (!upright_measure(S.orig, 16, 16, &t, &el, &tl) || !tl) { snprintf(S.up_note, sizeof S.up_note, "(not tilted)"); return 0; }
    } else { o.auto_turn = 0; o.turn = S.up_mode == UPM_RIGHT ? 45 : -45; }
    if (!upright_make(S.orig, 16, 16, &o, S.up, &S.up_turn, NULL)) { snprintf(S.up_note, sizeof S.up_note, "(could not make it)"); return 0; }
    itemdir(item, p, sizeof p); mkdirs(p); strcat(p, "\\upright.png");
    if (!stbi_write_png(p, 32, 32, 4, S.up, 32 * 4)) vlog(VLOG_WARN, "upright", "could not write %s", p);
    vlog(VLOG_INFO, "upright", "%03d stood upright: turned %.1f degrees, %s%s", item, S.up_turn, S.up_style ? "centred" : "classic", S.up_slim ? ", grip slimmed" : "");
    return 1;
}
/* the upright picture: the first upright = folder with NNN.png at 32 x 32 (hand-made, always wins), else NNN\upright.png
   made here before (kept, so a hand touch-up there stays until Remake), else made now. Then its box at 16 x 16 filled
   with the original's colours: the shrink takes only a box and a colour set from the picture it fits to */
static void load_upright(int item, int remake) {
    char p[MAX_PATH]; int i, w, h, x0, y0, x1, y1, x, y, nc = 0, k = 0; unsigned char *d, cols[256][3];
    S.has_up = S.up_hand = 0; S.up_turn = 0; S.up_note[0] = 0; memset(S.upfit, 0, sizeof S.upfit); memset(S.upfit32, 0, sizeof S.upfit32);
    for (i = 0; i < N_UPRIGHT && !S.has_up; i++) {
        snprintf(p, sizeof p, "%s\\%03d.png", P_UPRIGHT[i], item);
        if (!exists(p) || !(d = load_rgba(p, &w, &h))) continue;
        if (w == 32 && h == 32) { memcpy(S.up, d, sizeof S.up); S.has_up = S.up_hand = 1; }
        else vlog(VLOG_WARN, "pick", "%s is %d x %d, not 32 x 32: not used", p, w, h);
        stbi_image_free(d);
    }
    if (!S.has_up) {
        up_settings(item, 0);
        itemdir(item, p, sizeof p); strcat(p, "\\upright.png");
        if (!remake && S.up_mode != UPM_OFF && exists(p) && (d = load_rgba(p, &w, &h))) {
            if (w == 32 && h == 32) {
                double t, el; int tl;
                memcpy(S.up, d, sizeof S.up); S.has_up = 1;
                if (S.up_mode == UPM_AUTO) { upright_measure(S.orig, 16, 16, &t, &el, &tl); S.up_turn = -t; } else S.up_turn = S.up_mode == UPM_RIGHT ? 45 : -45;
            }
            stbi_image_free(d);
        }
        if (!S.has_up) S.has_up = up_make(item);
    }
    if (!S.has_up || !bbox_a(S.up, 32, 32, 127, &x0, &y0, &x1, &y1)) { S.has_up = 0; return; }
    for (i = 0; i < 256 && nc < 256; i++) if (S.orig[i * 4 + 3] >= 128) { cols[nc][0] = S.orig[i * 4]; cols[nc][1] = S.orig[i * 4 + 1]; cols[nc][2] = S.orig[i * 4 + 2]; nc++; }
    if (!nc) for (i = 0; i < 1024 && nc < 256; i++) if (S.up[i * 4 + 3] >= 128) { cols[nc][0] = S.up[i * 4]; cols[nc][1] = S.up[i * 4 + 1]; cols[nc][2] = S.up[i * 4 + 2]; nc++; }
    for (y = y0 / 2; y < (y1 + 1) / 2 && y < 16; y++) for (x = x0 / 2; x < (x1 + 1) / 2 && x < 16; x++) {
        unsigned char *o = S.upfit + (y * 16 + x) * 4;
        o[0] = cols[k % nc][0]; o[1] = cols[k % nc][1]; o[2] = cols[k % nc][2]; o[3] = 255; k++;
    }
    for (k = 0, y = y0; y <= y1 && y < 32; y++) for (x = x0; x <= x1 && x < 32; x++) {     /* the exact box, x1 and y1 included (A17) */
        unsigned char *o = S.upfit32 + (y * 32 + x) * 4;
        o[0] = cols[k % nc][0]; o[1] = cols[k % nc][1]; o[2] = cols[k % nc][2]; o[3] = 255; k++;
    }
    vlog(VLOG_INFO, "upright", "%03d: the upright picture's box x %d-%d y %d-%d (of 32); on the 16 grid it was x %d-%d y %d-%d",
         item, x0, x1, y0, y1, x0 / 2 * 2, (x1 + 1) / 2 * 2 - 1, y0 / 2 * 2, (y1 + 1) / 2 * 2 - 1);
}
/* ---------------------------------------------------------------- shrink presets: one click (or F1-F8) each.
   A preset is "key=value ..." over the standard settings; the size, fit and colours stay as they are. Built-in ones first,
   then the user's own from work\presets.txt ("NAME|key=value ...", a later line of the same name wins) */
static const char *const PRESET_B[][2] = {
    { "Standard", "" }, { "Clean patches", "regions=1 dist=1" }, { "Sword", "regions=1 dist=1 notch=1 blocks=1" },
    { "Round thing", "regions=1 dist=1 orphans=1 jaggies=1" }, { "No outline", "outline=0 edge=1" }, { "Outline auto", "outline=4" },
};
/* tried, kept as options, not better on our measures (B6, B7): in Compare only, to see them on a drawing */
static const char *const PRESET_X[][2] = { { "Grid snap", "method=13" }, { "Edge specks off", "edgestray=1" } };
/* kept when a preset is taken: the size and fit, and the colours chosen over the pictures (Original's / Model's) */
static int size_key(const char *k) { return !strcmp(k, "size") || !strcmp(k, "cw") || !strcmp(k, "ch") || !strcmp(k, "fit") || !strcmp(k, "margin") || !strcmp(k, "colours") || !strcmp(k, "ncol"); }
static void opt_parse(ShrinkOpt *o, const char *line) {      /* "key=value ..." onto o */
    const char *q = line; char k[40]; int v, n;
    while (*q) { while (*q == ' ' || *q == '\t') q++; if (!*q) break;
        if (sscanf(q, "%39[^= ]=%d%n", k, &v, &n) == 2) { shrink_set(o, k, v); q += n; } else while (*q && *q != ' ') q++; }
}
static void opt_line(const ShrinkOpt *o, int with_size, char *b, int n) {   /* the keys that differ from the standard */
    ShrinkOpt d; int i, a, c, l = 0; shrink_defaults(&d); b[0] = 0;
    for (i = 0; SHRINK_KEYS[i]; i++) {
        if (!with_size && size_key(SHRINK_KEYS[i])) continue;
        shrink_get(o, SHRINK_KEYS[i], &a); shrink_get(&d, SHRINK_KEYS[i], &c);
        if (a != c && l < n - 40) l += snprintf(b + l, (size_t)(n - l), "%s%s=%d", l ? " " : "", SHRINK_KEYS[i], a);
    }
}
static void preset_apply_to(ShrinkOpt *o, int p) {
    ShrinkOpt d; int i, v; shrink_defaults(&d);
    for (i = 0; SHRINK_KEYS[i]; i++) if (size_key(SHRINK_KEYS[i])) { shrink_get(o, SHRINK_KEYS[i], &v); shrink_set(&d, SHRINK_KEYS[i], v); }
    if (p >= 0 && p < S.np) opt_parse(&d, S.pset[p]);
    *o = d;
}
static int preset_now(void) {    /* the preset the settings are now, or -1 */
    int p; for (p = 0; p < S.np; p++) { ShrinkOpt o = S.opt; preset_apply_to(&o, p); if (!memcmp(&o, &S.opt, sizeof o)) return p; }
    return -1;
}
static void load_presets(void) {
    char p[MAX_PATH], *t, *l; int i;
    S.np = 0; memset(S.phid, 0, sizeof S.phid);
    for (i = 0; i < (int)(sizeof PRESET_B / sizeof PRESET_B[0]); i++) { snprintf(S.pname[S.np], 32, "%s", PRESET_B[i][0]); snprintf(S.pset[S.np], 400, "%s", PRESET_B[i][1]); S.np++; }
    S.nbuiltin = S.np;
    snprintf(p, sizeof p, "%s\\presets.txt", WORK);
    if ((t = read_all(p, NULL))) for (l = strtok(t, "\r\n"); l; l = strtok(NULL, "\r\n")) {     /* the user's own (the file may be missing) */
        char *bar = strchr(l, '|'); int k;
        if (!bar || bar == l) continue; *bar = 0;
        for (k = 0; k < S.np && strcmp(S.pname[k], l); k++) ;
        if (k == S.np) { if (S.np >= 16) continue; S.np++; }
        snprintf(S.pname[k], 32, "%s", l); snprintf(S.pset[k], 400, "%s", bar + 1);
    }
    free(t);
    for (i = 0; i < (int)(sizeof PRESET_X / sizeof PRESET_X[0]) && S.np < 16; i++) {
        snprintf(S.pname[S.np], 32, "%s", PRESET_X[i][0]); snprintf(S.pset[S.np], 400, "%s", PRESET_X[i][1]); S.phid[S.np] = 1; S.np++; }
}
static void save_preset(const char *name) {
    char p[MAX_PATH], l[500], s[400];
    if (!name[0] || strchr(name, '|')) { status("Give the preset a name first (no | in it)."); return; }
    opt_line(&S.opt, 0, s, sizeof s);
    snprintf(p, sizeof p, "%s\\presets.txt", WORK); snprintf(l, sizeof l, "%s|%s", name, s); append_line(p, l);
    load_presets(); status("Preset \"%s\" saved: one click in the bar over the pictures.", name);
}
/* the item's own shrink settings: NNN\shrink.txt (all keys that differ from the standard, the size too) */
static void item_opt_path(int item, char *p, int n) { itemdir(item, p, n); strcat(p, "\\shrink.txt"); }
static void item_opt_save(int item) {
    char p[MAX_PATH], s[600]; itemdir(item, p, sizeof p); mkdirs(p); item_opt_path(item, p, sizeof p);
    opt_line(&S.opt, 1, s, sizeof s); write_all(p, s, strlen(s));
}
static void item_opt_leave(void) {          /* before another item is shown */
    if (!S.opt_loaded) return;
    if (S.own_opt) item_opt_save(S.item); else S.gopt = S.opt;
}
static void item_opt_arrive(int item) {
    char p[MAX_PATH], *t;
    if (!S.opt_loaded) { S.gopt = S.opt; S.opt_loaded = 1; }
    item_opt_path(item, p, sizeof p);
    if ((t = read_all(p, NULL))) { shrink_defaults(&S.opt); opt_parse(&S.opt, t); free(t); S.own_opt = 1; }
    else { S.opt = S.gopt; S.own_opt = 0; }
}
static void item_opt_toggle(void) {         /* "These settings for this item only" */
    char p[MAX_PATH], q[MAX_PATH];
    if (!S.own_opt) { S.own_opt = 1; item_opt_save(S.item); status("%03d keeps these settings of its own; the other items keep theirs.", S.item); return; }
    S.own_opt = 0; item_opt_path(S.item, p, sizeof p); snprintf(q, sizeof q, "%s", p); strcpy(q + strlen(q) - 4, "-off.txt");
    MoveFileExA(p, q, MOVEFILE_REPLACE_EXISTING);          /* kept as shrink-off.txt, not deleted */
    S.opt = S.gopt; status("%03d uses the common settings again (its own are kept in shrink-off.txt).", S.item);
}

/* an item: its pictures, description, and the rounds made before (rounds.txt: "ROUND MODEL SEED [upright]"; the
   pictures thrown out are in rejected.txt, "MODEL SEED", and not shown) */
static void choose(int pos) {
    char p[MAX_PATH], *t; int item, w, h;
    unsigned char *d;
    if (S.busy) return;
    if (pos < 0) pos = S.nlist - 1;
    if (pos >= S.nlist) pos = 0;
    stop_reshape();       /* before S.item changes: the reshaper may still be working on this item's rounds */
    item_opt_leave();
    S.pos = pos; item = S.item = S.list[pos];
    item_opt_arrive(item);
    S.cmp_for = -1;
    free_rounds();
    if (S.bm_orig) DeleteObject(S.bm_orig); if (S.bm_tool) DeleteObject(S.bm_tool); if (S.bm_up) DeleteObject(S.bm_up); if (S.pv_up) DeleteObject(S.pv_up); if (S.pv_orig) DeleteObject(S.pv_orig);
    S.bm_orig = S.bm_tool = S.bm_up = S.pv_up = S.pv_orig = NULL;
    if (S.bm_mini) DeleteObject(S.bm_mini); S.bm_mini = NULL;
    memset(S.orig, 0, sizeof S.orig);
    snprintf(p, sizeof p, "%s\\%03d.png", P_SPRITES, item);
    if ((d = load_rgba(p, &w, &h))) { if (w == 16 && h == 16) { memcpy(S.orig, d, sizeof S.orig); S.bm_orig = mkbmp(S.orig, 16, 16, 6, 0); S.pv_orig = mkbmp(S.orig, 16, 16, 12, 0); S.bm_mini = mkbmp(S.orig, 16, 16, 2, 0); } stbi_image_free(d); }
    snprintf(p, sizeof p, "%s\\%03d.png", P_ENLARGED, item);
    if (P_ENLARGED[0] && (d = load_rgba(p, &w, &h))) { if (w == 32 && h == 32) { memcpy(S.tool, d, sizeof S.tool); S.bm_tool = mkbmp(S.tool, 32, 32, 3, 0); } else vlog(VLOG_WARN, "pick", "%s is %d x %d, not 32 x 32: not used", p, w, h); stbi_image_free(d); }
    load_upright(item, 0);
    if (S.has_up) { S.bm_up = mkbmp(S.up, 32, 32, 3, 0); S.pv_up = mkbmp(S.up, 32, 32, 6, 0); }
    /* what the models are given: from.txt, else the upright picture when there is one, else our tool's */
    S.from = S.has_up ? FROM_UP : FROM_TOOL;
    itemdir(item, p, sizeof p); strcat(p, "\\from.txt");
    if ((t = read_all(p, NULL))) { int f; for (f = 0; f < 3; f++) if (!strncmp(t, FROM_NAMES[f], strlen(FROM_NAMES[f]))) S.from = f; free(t); }
    if (S.from == FROM_UP && !S.has_up) S.from = FROM_TOOL;
    if (S.from == FROM_TOOL && !S.bm_tool) S.from = S.has_up ? FROM_UP : FROM_ORIG;   /* no tool picture: never point at an empty slot */
    S.desc_made = 0; S.desc_word = NULL;
    load_desc_text(item);
    {   char *rj = NULL, *rf; itemdir(item, p, sizeof p); strcat(p, "\\rejected.txt");
    if ((rf = read_all(p, NULL))) { size_t n = strlen(rf); rj = xmalloc(n + 3); rj[0] = '\n'; memcpy(rj + 1, rf, n); rj[n + 1] = '\n'; rj[n + 2] = 0; free(rf);
                                  for (rf = rj; *rf; rf++) if (*rf == '\r') *rf = '\n'; }
    itemdir(item, p, sizeof p); strcat(p, "\\rounds.txt");
    if ((t = read_all(p, NULL))) {
        char *l, unk[24] = ""; int nunk = 0;     /* pictures of models no longer in models.txt: said once for the item */
        for (l = strtok(t, "\r\n"); l; l = strtok(NULL, "\r\n")) {
            int rd, mi; char m[24], src[16] = "", key[48]; unsigned seed; Shot s; char sp[MAX_PATH];
            if (sscanf(l, "%d %23s %u %15s", &rd, m, &seed, src) < 3 || rd < 0 || rd >= MAXR) continue;
            snprintf(key, sizeof key, "\n%s %u\n", m, seed);
            if (rj && strstr(rj, key)) continue;        /* thrown out */
            if ((mi = find_model(m)) < 0) { if (!nunk++) snprintf(unk, sizeof unk, "%s", m); continue; }
            memset(&s, 0, sizeof s); s.model = mi; s.seed = seed; s.src = !strcmp(src, "upright") ? FROM_UP : FROM_TOOL;
            shotpath(item, &s, 1, sp, sizeof sp);
            if (!(d = load_rgba(sp, &w, &h))) continue;
            if (w == 32 && h == 32) {
                while (S.nr <= rd) { memset(&S.r[S.nr], 0, sizeof S.r[0]); S.nr++; }
                /* the standard sprite stands in until the settings' one is made (it is that one with the standard settings) */
                if (S.r[rd].n < MAXSHOT) { s.st = ST_DONE; S.r[rd].s[S.r[rd].n] = s; set_res(&S.r[rd].s[S.r[rd].n], d, 32, 32, -1); S.r[rd].n++; }
            }
            stbi_image_free(d);
        }
        if (nunk) vlog(VLOG_WARN, "pick", "%03d: %d picture%s of model%s not in models.txt (\"%s\"%s): not shown", item, nunk, nunk > 1 ? "s" : "", nunk > 1 ? "s" : "", unk, nunk > 1 ? " ..." : "");
        free(t);
    }
    free(rj); }
    S.cur = S.nr ? S.nr - 1 : 0;
    read_kept(item);
    kick_reshape();
    if (S.desc_made && S.desc_word) status("%03d: its upright description was made from the other and still says \"%s\": fix it before drawing (the models follow the words).", item, S.desc_word);
    else if (S.desc_made) status("%03d: its upright description was made from the other one: check it before drawing.", item);
    else status(S.nr ? "%03d %s: %d earlier round(s) loaded." : "%03d %s: press Space to draw.", item, S.name[item], S.nr);
}

/* ---------------------------------------------------------------- the worker: ComfyUI started if needed, the round drawn */
static int comfy_alive(void) { Buf b = { 0 }; int ok = http(L"GET", "/system_stats", NULL, 0, NULL, &b, 2000) == 200; free(b.p); return ok; }
static int comfy_ready(void) {
    char cmd[2048], dir[MAX_PATH], logp[MAX_PATH]; STARTUPINFOA si; PROCESS_INFORMATION pi; SECURITY_ATTRIBUTES sa; HANDLE lg; int i;
    if (comfy_alive()) return 1;
    if (S.comfy && WaitForSingleObject(S.comfy, 0) == WAIT_TIMEOUT) goto wait;     /* ours, still starting */
    if (!P_COMFY[0]) { status("Set comfy = (your ComfyUI folder) in voxup.ini beside voxup.exe, or start ComfyUI on port %d yourself.", PORT); return 0; }
    snprintf(dir, sizeof dir, "%s\\comfy", WORK);
    { char yaml[MAX_PATH + 40] = ""; if (P_YAML[0]) snprintf(yaml, sizeof yaml, "--extra-model-paths-config \"%s\" ", P_YAML);
    snprintf(cmd, sizeof cmd, "\"%s\\.venv\\Scripts\\python.exe\" -s \"%s\\main.py\" --listen 127.0.0.1 --port %d %s"
             "--input-directory \"%s\\input\" --output-directory \"%s\\output\" --temp-directory \"%s\\temp\" --disable-auto-launch",
             P_COMFY, P_COMFY, PORT, yaml, dir, dir, dir); }
    snprintf(logp, sizeof logp, "%s\\input", dir); mkdirs(logp);
    snprintf(logp, sizeof logp, "%s\\output", dir); mkdirs(logp);
    snprintf(logp, sizeof logp, "%s\\comfy.log", WORK);
    memset(&sa, 0, sizeof sa); sa.nLength = sizeof sa; sa.bInheritHandle = TRUE;
    lg = CreateFileA(logp, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    memset(&si, 0, sizeof si); si.cb = sizeof si; si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    si.hStdOutput = si.hStdError = lg; si.hStdInput = NULL;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, P_COMFY, &si, &pi)) {
        vlog(VLOG_ERR, "comfy", "could not start ComfyUI (%s): %s", cmd, vlog_errno());
        status("Could not start ComfyUI: %s", vlog_errno());
        if (lg != INVALID_HANDLE_VALUE) CloseHandle(lg);
        return 0;
    }
    if (S.job) AssignProcessToJobObject(S.job, pi.hProcess);
    ResumeThread(pi.hThread); CloseHandle(pi.hThread);
    if (lg != INVALID_HANDLE_VALUE) CloseHandle(lg);
    S.comfy = pi.hProcess;
    vlog(VLOG_INFO, "comfy", "started ComfyUI hidden, pid %lu, log %s", pi.dwProcessId, logp);
wait:
    { DWORD t0 = GetTickCount();
    for (i = 0; i < 600; i++) {
        if (S.stop) return 0;
        if (WaitForSingleObject(S.comfy, 0) != WAIT_TIMEOUT) { status("ComfyUI stopped while starting: see %s\\comfy.log", WORK); return 0; }
        if (comfy_alive()) return 1;
        if (i % 4 == 0) status("Starting ComfyUI in the background (%lu s; about 30 s)...", (GetTickCount() - t0) / 1000);
        Sleep(500);
    } }
    status("ComfyUI did not answer within 5 minutes: see %s\\comfy.log", WORK);
    return 0;
}
/* the item's picture for the models, 1024 x 1024 RGB (the caller frees it): what Draw from says (our tool's 128 x 128
   enlarged sprite, the upright picture, or the original), its box 70% of a magenta square; also written as NNN\in.png */
static unsigned char *make_input(int item) {
    char p[MAX_PATH]; size_t len; char *raw = NULL; int w, h, x0, y0, x1, y1, cw, ch, nw, nh, ox, oy, x, y;
    unsigned char *pic; Buf png = { 0 }; double f;
    if (S.from == FROM_UP && S.has_up && !S.up_hand) {
        /* made here: the same turn drawn large and clean from the 16 x 16 original (not the 32 x 32 in 22 x blocks) */
        unsigned char *b = upright_big(S.orig, 16, 16, S.up_turn, 716, &w, &h);
        if (b) { len = 8 + (size_t)w * h * 4; raw = xmalloc(len); memcpy(raw, &w, 4); memcpy(raw + 4, &h, 4); memcpy(raw + 8, b, (size_t)w * h * 4); free(b); }
        snprintf(p, sizeof p, "the upright picture of %03d", item);
    } else if (S.from == FROM_UP && S.has_up) {
        len = 8 + 32 * 32 * 4; raw = xmalloc(len); w = h = 32; memcpy(raw, &w, 4); memcpy(raw + 4, &h, 4); memcpy(raw + 8, S.up, 32 * 32 * 4);
        snprintf(p, sizeof p, "the upright picture of %03d", item);
    } else if (S.from == FROM_TOOL) {
        snprintf(p, sizeof p, "%s\\raw\\%03d_in.raw", P_ENLARGED, item);
        if (P_ENLARGED[0] && (raw = read_all(p, &len)) && len < 8) { free(raw); raw = NULL; }
        if (!raw) vlog(VLOG_WARN, "draw", "%03d: drawing from our tool's picture, but %s is %s: the models get the 16 x 16 original", item, p, P_ENLARGED[0] ? "missing or empty" : "not set (voxup.ini enlarged =)");
    }
    if (!raw) {
        /* no enlarged picture (raw\NNN_in.raw: width, height as 4-byte ints, then RGBA): the original sprite itself */
        int ow, oh; unsigned char *o; char sp[MAX_PATH];
        snprintf(sp, sizeof sp, "%s\\%03d.png", P_SPRITES, item);
        if (!(o = load_rgba(sp, &ow, &oh))) { status("No picture for %03d (%s)", item, sp); return NULL; }
        len = 8 + (size_t)ow * oh * 4; raw = xmalloc(len); memcpy(raw, &ow, 4); memcpy(raw + 4, &oh, 4); memcpy(raw + 8, o, (size_t)ow * oh * 4);
        stbi_image_free(o); snprintf(p, sizeof p, "%s", sp);
    }
    memcpy(&w, raw, 4); memcpy(&h, raw + 4, 4);
    if (w <= 0 || h <= 0 || len < 8 + (size_t)w * h * 4 || !bbox_a((unsigned char *)raw + 8, w, h, 127, &x0, &y0, &x1, &y1)) { status("%s is not a picture", p); free(raw); return NULL; }
    cw = x1 - x0; ch = y1 - y0; f = 716.0 / (cw > ch ? cw : ch);
    nw = (int)nearbyint(cw * f); nh = (int)nearbyint(ch * f); ox = (1024 - nw) / 2; oy = (1024 - nh) / 2;
    pic = xmalloc(1024 * 1024 * 3);
    for (x = 0; x < 1024 * 1024; x++) { pic[x * 3] = 255; pic[x * 3 + 1] = 0; pic[x * 3 + 2] = 255; }
    for (y = 0; y < nh; y++) for (x = 0; x < nw; x++) {
        const unsigned char *q = (unsigned char *)raw + 8 + ((size_t)(y0 + (int)((y + 0.5) * ch / nh)) * w + x0 + (int)((x + 0.5) * cw / nw)) * 4;
        unsigned char *o = pic + ((size_t)(oy + y) * 1024 + ox + x) * 3; int c;
        for (c = 0; c < 3; c++) o[c] = (unsigned char)((q[c] * q[3] + o[c] * (255 - q[3]) + 127) / 255);
    }
    free(raw);
    stbi_write_png_to_func(png_cb, &png, 1024, 1024, 3, pic, 1024 * 3);
    itemdir(item, p, sizeof p); mkdirs(p); strcat(p, "\\in.png"); write_all(p, png.p, png.n);
    free(png.p);
    return pic;
}
/* the picture sent to ComfyUI once per item and Draw-from choice */
static int upload_input(int item) {
    char hdr[200], bnd[40], nm[64]; unsigned char *pic; Buf png = { 0 }, body = { 0 }, res = { 0 }; int code;
    if (S.upitem == item && S.upfrom == S.from && S.upname[0]) return 1;
    if (!(pic = make_input(item))) return 0;
    stbi_write_png_to_func(png_cb, &png, 1024, 1024, 3, pic, 1024 * 3);
    free(pic);
    snprintf(bnd, sizeof bnd, "voxupBoundary%08x", (unsigned)GetTickCount());
    snprintf(nm, sizeof nm, "voxup_%03d.png", item);
    buf_str(&body, "--"); buf_str(&body, bnd); buf_str(&body, "\r\nContent-Disposition: form-data; name=\"image\"; filename=\""); buf_str(&body, nm);
    buf_str(&body, "\"\r\nContent-Type: image/png\r\n\r\n"); buf_add(&body, png.p, png.n);
    buf_str(&body, "\r\n--"); buf_str(&body, bnd); buf_str(&body, "\r\nContent-Disposition: form-data; name=\"overwrite\"\r\n\r\ntrue\r\n--"); buf_str(&body, bnd); buf_str(&body, "--\r\n");
    snprintf(hdr, sizeof hdr, "Content-Type: multipart/form-data; boundary=%s", bnd);
    { wchar_t wh[200]; MultiByteToWideChar(CP_UTF8, 0, hdr, -1, wh, 200); code = http(L"POST", "/upload/image", body.p, (DWORD)body.n, wh, &res, 60000); }
    free(png.p); free(body.p);
    if (code != 200 || !jstr((char *)res.p, "name", S.upname, sizeof S.upname)) { status("ComfyUI did not take the picture (HTTP %d)", code); free(res.p); return 0; }
    free(res.p); S.upitem = item; S.upfrom = S.from;
    return 1;
}
/* the model's graph (models\ID.json) with its blanks filled: "@IMAGE@" the uploaded picture, "@PROMPT@" the prompt,
   "@SEED@" the seed (a number, so the quotes go too), "@PREFIX@" the output name */
static void graph(Buf *g, int model, const char *image, const char *prompt, unsigned seed) {
    const char *p = S.m[model].graph, *q; char t[64];
    buf_str(g, "{\"client_id\":\"voxup\",\"prompt\":");
    while ((q = strstr(p, "\"@"))) {
        buf_add(g, p, (size_t)(q - p));
        if (!strncmp(q, "\"@IMAGE@\"", 9)) { json_esc(g, image); p = q + 9; }
        else if (!strncmp(q, "\"@PROMPT@\"", 10)) { json_esc(g, prompt); p = q + 10; }
        else if (!strncmp(q, "\"@SEED@\"", 8)) { snprintf(t, sizeof t, "%u", seed); buf_str(g, t); p = q + 8; }
        else if (!strncmp(q, "\"@PREFIX@\"", 10)) { snprintf(t, sizeof t, "\"voxup_%s\"", S.m[model].id); buf_str(g, t); p = q + 10; }
        /* the window's per-model settings, as numbers: how much may change (0..1), the LoRA's strength (0..2) */
        else if (!strncmp(q, "\"@STRENGTH@\"", 12)) { snprintf(t, sizeof t, "%.2f", S.m[model].strength / 100.0); buf_str(g, t); p = q + 12; }
        else if (!strncmp(q, "\"@LORA@\"", 8)) { snprintf(t, sizeof t, "%.2f", S.m[model].lora_w / 100.0); buf_str(g, t); p = q + 8; }
        else { buf_add(g, q, 2); p = q + 2; }
    }
    buf_str(g, p);
    while (g->n && (g->p[g->n - 1] == '\n' || g->p[g->n - 1] == '\r' || g->p[g->n - 1] == ' ')) g->p[--g->n] = 0;
    buf_str(g, "}");
}
#if 0   /* the first two graphs, kept for reference: now models\klein.json and kontext.json (the same nodes) */
static void graph_old(Buf *g, int model, const char *image, const char *prompt, unsigned seed) {
    char t[1024];
    buf_str(g, "{\"client_id\":\"voxup\",\"prompt\":");
    if (model == 0) {
        snprintf(t, sizeof t, "{\"1\":{\"class_type\":\"LoadImage\",\"inputs\":{\"image\":\"%s\"}},"
            "\"2\":{\"class_type\":\"UNETLoader\",\"inputs\":{\"unet_name\":\"flux-2-klein-base-4b-fp8.safetensors\",\"weight_dtype\":\"default\"}},"
            "\"3\":{\"class_type\":\"CLIPLoader\",\"inputs\":{\"clip_name\":\"qwen_3_4b.safetensors\",\"type\":\"flux2\",\"device\":\"default\"}},"
            "\"4\":{\"class_type\":\"VAELoader\",\"inputs\":{\"vae_name\":\"flux2-vae.safetensors\"}},"
            "\"5\":{\"class_type\":\"VAEEncode\",\"inputs\":{\"pixels\":[\"1\",0],\"vae\":[\"4\",0]}},"
            "\"6\":{\"class_type\":\"CLIPTextEncode\",\"inputs\":{\"clip\":[\"3\",0],\"text\":", image);
        buf_str(g, t); json_esc(g, prompt);
        snprintf(t, sizeof t, "}},\"7\":{\"class_type\":\"CLIPTextEncode\",\"inputs\":{\"text\":\"\",\"clip\":[\"3\",0]}},"
            "\"8\":{\"class_type\":\"ReferenceLatent\",\"inputs\":{\"conditioning\":[\"6\",0],\"latent\":[\"5\",0]}},"
            "\"9\":{\"class_type\":\"ReferenceLatent\",\"inputs\":{\"conditioning\":[\"7\",0],\"latent\":[\"5\",0]}},"
            "\"10\":{\"class_type\":\"CFGGuider\",\"inputs\":{\"model\":[\"2\",0],\"positive\":[\"8\",0],\"negative\":[\"9\",0],\"cfg\":5.0}},"
            "\"11\":{\"class_type\":\"Flux2Scheduler\",\"inputs\":{\"steps\":20,\"width\":1024,\"height\":1024}},"
            "\"12\":{\"class_type\":\"EmptyFlux2LatentImage\",\"inputs\":{\"width\":1024,\"height\":1024,\"batch_size\":1}},"
            "\"13\":{\"class_type\":\"KSamplerSelect\",\"inputs\":{\"sampler_name\":\"euler\"}},"
            "\"14\":{\"class_type\":\"RandomNoise\",\"inputs\":{\"noise_seed\":%u}},", seed);
        buf_str(g, t);
        buf_str(g, "\"15\":{\"class_type\":\"SamplerCustomAdvanced\",\"inputs\":{\"noise\":[\"14\",0],\"guider\":[\"10\",0],\"sampler\":[\"13\",0],\"sigmas\":[\"11\",0],\"latent_image\":[\"12\",0]}},"
            "\"16\":{\"class_type\":\"VAEDecode\",\"inputs\":{\"samples\":[\"15\",0],\"vae\":[\"4\",0]}},"
            "\"17\":{\"class_type\":\"SaveImage\",\"inputs\":{\"images\":[\"16\",0],\"filename_prefix\":\"voxup_klein\"}}}}");
    } else {
        snprintf(t, sizeof t, "{\"1\":{\"class_type\":\"LoadImage\",\"inputs\":{\"image\":\"%s\"}},"
            "\"2\":{\"class_type\":\"UNETLoader\",\"inputs\":{\"unet_name\":\"flux1-dev-kontext_fp8_scaled.safetensors\",\"weight_dtype\":\"default\"}},"
            "\"3\":{\"class_type\":\"DualCLIPLoader\",\"inputs\":{\"clip_name1\":\"clip_l.safetensors\",\"clip_name2\":\"t5xxl_fp8_e4m3fn_scaled.safetensors\",\"type\":\"flux\"}},"
            "\"4\":{\"class_type\":\"VAELoader\",\"inputs\":{\"vae_name\":\"ae.safetensors\"}},"
            "\"5\":{\"class_type\":\"FluxKontextImageScale\",\"inputs\":{\"image\":[\"1\",0]}},"
            "\"6\":{\"class_type\":\"VAEEncode\",\"inputs\":{\"pixels\":[\"5\",0],\"vae\":[\"4\",0]}},"
            "\"7\":{\"class_type\":\"CLIPTextEncode\",\"inputs\":{\"clip\":[\"3\",0],\"text\":", image);
        buf_str(g, t); json_esc(g, prompt);
        snprintf(t, sizeof t, "}},\"8\":{\"class_type\":\"ReferenceLatent\",\"inputs\":{\"conditioning\":[\"7\",0],\"latent\":[\"6\",0]}},"
            "\"9\":{\"class_type\":\"FluxGuidance\",\"inputs\":{\"conditioning\":[\"8\",0],\"guidance\":2.5}},"
            "\"10\":{\"class_type\":\"ConditioningZeroOut\",\"inputs\":{\"conditioning\":[\"7\",0]}},"
            "\"12\":{\"class_type\":\"KSampler\",\"inputs\":{\"model\":[\"2\",0],\"positive\":[\"9\",0],\"negative\":[\"10\",0],\"latent_image\":[\"6\",0],"
            "\"seed\":%u,\"steps\":24,\"cfg\":1.0,\"sampler_name\":\"euler\",\"scheduler\":\"simple\",\"denoise\":1.0}},", seed);
        buf_str(g, t);
        buf_str(g, "\"13\":{\"class_type\":\"VAEDecode\",\"inputs\":{\"samples\":[\"12\",0],\"vae\":[\"4\",0]}},"
            "\"14\":{\"class_type\":\"SaveImage\",\"inputs\":{\"images\":[\"13\",0],\"filename_prefix\":\"voxup_kontext\"}}}}");
    }
}
#endif
static void make_prompt(const char *tmpl, const char *desc, char *o, int n) {
    const char *p = tmpl, *q; int i = 0;
    while ((q = strstr(p, "{d}")) && i < n - 1) {
        int k = (int)(q - p); if (k > n - 1 - i) k = n - 1 - i;
        memcpy(o + i, p, (size_t)k); i += k;
        k = (int)strlen(desc); if (k > n - 1 - i) k = n - 1 - i;
        memcpy(o + i, desc, (size_t)k); i += k; p = q + 3;
    }
    { int k = (int)strlen(p); if (k > n - 1 - i) k = n - 1 - i; memcpy(o + i, p, (size_t)k); i += k; }
    o[i] = 0;
}
static void finish_shot(int item, int round, Shot *s, const void *pngb, size_t pngn);
/* one shot: the job sent, waited for, the picture fetched, made 32 x 32, saved */
static void draw_one(int item, int round, Shot *s, const char *desc, const char *tmpl) {
    char prompt[12000], pid[80], fn[260], sub[260], ty[32], q[1200], a[600], b[600];
    Buf g = { 0 }, res = { 0 }; int code, t;
    make_prompt(tmpl, desc, prompt, sizeof prompt);
    graph(&g, s->model, S.upname, prompt, s->seed);
    code = http(L"POST", "/prompt", g.p, (DWORD)g.n, L"Content-Type: application/json", &res, 30000);
    free(g.p);
    if (code != 200 || !jstr((char *)res.p, "prompt_id", pid, sizeof pid)) {
        snprintf(s->err, sizeof s->err, "ComfyUI refused the job (HTTP %d): %.120s", code, res.p ? (char *)res.p : "");
        vlog(VLOG_ERR, "comfy", "%s", s->err); free(res.p); s->st = ST_FAIL; return;
    }
    for (t = 0;; t++) {
        if (S.stop) { http(L"POST", "/interrupt", "", 0, NULL, NULL, 5000); s->st = ST_STOP; free(res.p); return; }
        if (t > 3000) { snprintf(s->err, sizeof s->err, "no picture after 20 minutes"); s->st = ST_FAIL; free(res.p); return; }
        snprintf(q, sizeof q, "/history/%s", pid);
        if (http(L"GET", q, NULL, 0, NULL, &res, 10000) == 200 && strstr((char *)res.p, pid)) break;
        Sleep(400);
    }
    if (strstr((char *)res.p, "\"status_str\": \"error\"") || strstr((char *)res.p, "\"status_str\":\"error\"")) {
        char m[300] = ""; jstr((char *)res.p, "exception_message", m, sizeof m);
        snprintf(s->err, sizeof s->err, "ComfyUI: %s", m[0] ? m : "the job failed");
        vlog(VLOG_ERR, "comfy", "%s %u: %s", S.m[s->model].id, s->seed, s->err); free(res.p); s->st = ST_FAIL; return;
    }
    {
        const char *im = strstr((char *)res.p, "\"images\"");
        if (!im || !jstr(im, "filename", fn, sizeof fn)) { snprintf(s->err, sizeof s->err, "ComfyUI gave no picture"); free(res.p); s->st = ST_FAIL; return; }
        if (!jstr(im, "subfolder", sub, sizeof sub)) sub[0] = 0;
        if (!jstr(im, "type", ty, sizeof ty)) strcpy(ty, "output");
    }
    free(res.p); res.p = NULL; res.n = res.cap = 0;
    url_esc(a, sizeof a, fn); url_esc(b, sizeof b, sub);
    snprintf(q, sizeof q, "/view?filename=%s&subfolder=%s&type=%s", a, b, ty);
    if (http(L"GET", q, NULL, 0, NULL, &res, 60000) != 200 || res.n < 8) { snprintf(s->err, sizeof s->err, "could not fetch %s", fn); free(res.p); s->st = ST_FAIL; return; }
    finish_shot(item, round, s, res.p, res.n); free(res.p);
}
#if SD_BUILT
/* ---------------------------------------------------------------- the built-in engine (stable-diffusion.cpp) */
/* SDX: the loaded model, kept between rounds; only the worker uses it. SDX_FAILED: a model that would not load this
   round (not tried again until the next round) */
static sd_ctx_t *SDX; static int SDX_MODEL = -1, SDX_FAILED = -1;
static volatile LONG SDX_DRAWING;
static void sd_log(enum sd_log_level_t lv, const char *t, void *data) {
    char b[600]; size_t n; (void)data;
    if (lv < SD_LOG_WARN) return;
    snprintf(b, sizeof b, "%s", t); n = strlen(b); while (n && (b[n - 1] == '\n' || b[n - 1] == '\r')) b[--n] = 0;
    vlog(lv == SD_LOG_ERROR ? VLOG_ERR : VLOG_WARN, "engine", "%s", b);
}
static void sd_progress(int step, int steps, float t, void *data) {
    (void)data; (void)t;
    if (SDX_DRAWING && steps > 0) { InterlockedExchange(&S.sd_step, step); InterlockedExchange(&S.sd_steps, steps); EnterCriticalSection(&S.cs); snprintf(S.sdstep, sizeof S.sdstep, " - step %d of %d", step, steps); LeaveCriticalSection(&S.cs); if (S.wnd) PostMessageA(S.wnd, WM_APP, 0, 0); }
}
/* "files:DIR/NAME" or "files:DIR\NAME" = that file in the models folder (voxup.ini model_files) */
static void sd_value(const char *v, char *o, size_t n) {
    if (!strncmp(v, "files:", 6)) { char *c; snprintf(o, n, "%s\\%s", P_SHARED, v + 6); for (c = o; *c; c++) if (*c == '/') *c = '\\'; }
    else snprintf(o, n, "%s", v);
}
/* one option by its written value: tried as a whole number, a number, true/false, then as text */
static int sd_set(sd_opt_t *o, const char *key, const char *v) {
    char *e; long long i = strtoll(v, &e, 10); double f;
    if (*v && !*e && sd_opt_set_int(o, key, i) == SD_STATUS_OK) return 1;
    f = strtod(v, &e); if (*v && !*e && sd_opt_set_float(o, key, f) == SD_STATUS_OK) return 1;
    if ((!strcmp(v, "true") || !strcmp(v, "false")) && sd_opt_set_bool(o, key, v[0] == 't') == SD_STATUS_OK) return 1;
    return sd_opt_set_string(o, key, v) == SD_STATUS_OK;
}
static void sd_unload(void) { if (SDX) { sd_ctx_free(SDX); SDX = NULL; SDX_MODEL = -1; vlog(VLOG_INFO, "engine", "model unloaded"); } }
/* vulkan-1.dll comes with the graphics driver; voxup.exe loads it only when the built-in engine is used (a delay
   import), so the window opens without it. --novulkan in the self-test acts as if it were missing. */
static int VK_STATE = -1, TEST_NOVK;
static int vk_there(void) {
    if (VK_STATE < 0) {
        HMODULE h = TEST_NOVK ? NULL : LoadLibraryA("vulkan-1.dll");
        VK_STATE = h != NULL;
        if (!h) vlog(VLOG_WARN, "engine", "no Vulkan graphics driver (vulkan-1.dll%s): the built-in engine cannot draw; update the graphics driver (ComfyUI can still draw)", TEST_NOVK ? ", self-test --novulkan" : " not found");
    }
    return VK_STATE;
}
static int sd_load(int model) {
    Model *M = &S.m[model]; sd_ctx_opt_t *o; int i; char v[MAX_PATH + 600]; sd_status_t st;
    if (SDX && SDX_MODEL == model) return 1;
    if (SDX_FAILED == model) return 0;
    if (!vk_there()) { status("No Vulkan graphics driver found: update the graphics driver to draw with the built-in engine (ComfyUI can still draw)."); SDX_FAILED = model; return 0; }
    sd_unload();
    if (!(o = sd_ctx_opt_create())) return 0;
    for (i = 0; i < M->nsd; i++) if (M->sd[i].where == 'c') {
        sd_value(M->sd[i].val, v, sizeof v);
        if (!sd_set(o, M->sd[i].key, v)) { vlog(VLOG_ERR, "engine", "%s: %s = %s refused: %s", M->id, M->sd[i].key, v, sd_get_last_error()); sd_opt_free(o); return 0; }
    }
    status("Loading %s into the built-in engine...", M->name);
    st = sd_ctx_create(o, &SDX); sd_opt_free(o);
    if (st != SD_STATUS_OK || !SDX) { status("The built-in engine could not load %s: %s", M->name, sd_get_last_error()); vlog(VLOG_ERR, "engine", "%s: load failed (%d): %s", M->id, (int)st, sd_get_last_error()); SDX = NULL; SDX_FAILED = model; return 0; }
    SDX_MODEL = model; vlog(VLOG_INFO, "engine", "loaded %s (%s)", M->id, sd_get_model_version_name(SDX));
    return 1;
}
/* one shot drawn inside voxup: pic is the item's 1024 x 1024 picture (make_input) */
static void draw_one_sd(int item, int round, Shot *s, const char *desc, const char *tmpl, const unsigned char *pic) {
    Model *M = &S.m[s->model]; char prompt[12000], v[MAX_PATH + 600]; sd_img_gen_opt_t *o = NULL; sd_result_t *r = NULL;
    sd_tensor_t *in = NULL; const sd_tensor_t *im; int64_t shape[3] = { 3, 1024, 1024 }; sd_status_t st; int i, ok = 0;
    if (!sd_load(s->model)) { snprintf(s->err, sizeof s->err, "the built-in engine could not load %s", M->name); s->st = ST_FAIL; return; }
    make_prompt(tmpl, desc, prompt, sizeof prompt);
    if (!(o = sd_img_gen_opt_create()) || !(in = sd_tensor_create(SD_TENSOR_U8, 3, shape, pic, 1024 * 1024 * 3))) goto fail;
    if (sd_opt_set_string(o, "prompt", prompt) || sd_opt_set_int(o, "seed", (int64_t)s->seed) ||
        sd_opt_set_int(o, "width", 1024) || sd_opt_set_int(o, "height", 1024)) goto fail;
    if (M->sdimage == 2) { if (sd_opt_set_tensor(o, "init_image", in) || sd_opt_set_float(o, "strength", M->strength / 100.0)) goto fail; }
    else { const sd_tensor_t *refs[1]; refs[0] = in; if (sd_opt_set_tensors(o, "ref_images", refs, 1)) goto fail; }
    if (M->has_lora && M->lora_w > 0) {
        sd_lora_t l; char lp[MAX_PATH]; memset(&l, 0, sizeof l); snprintf(lp, sizeof lp, "%s\\%s", P_SHARED, M->lora);
        l.path = lp; l.multiplier = M->lora_w / 100.0f;
        if (sd_opt_set_loras(o, "loras", &l, 1)) goto fail;
    }
    for (i = 0; i < M->nsd; i++) if (M->sd[i].where == 'i') {
        sd_value(M->sd[i].val, v, sizeof v);
        if (!sd_set(o, M->sd[i].key, v)) { vlog(VLOG_ERR, "engine", "%s: %s = %s refused: %s", M->id, M->sd[i].key, v, sd_get_last_error()); goto fail; }
    }
    InterlockedExchange(&SDX_DRAWING, 1);
    st = sd_generate_image(SDX, o, &r);
    InterlockedExchange(&SDX_DRAWING, 0);
    EnterCriticalSection(&S.cs); S.sdstep[0] = 0; LeaveCriticalSection(&S.cs);
    if (S.stop) { sd_cancel_generation(SDX, SD_CANCEL_RESET); s->st = ST_STOP; goto done; }
    if (st != SD_STATUS_OK || !r || sd_result_get_image_count(r) < 1 || !(im = sd_result_get_image(r, 0))) goto fail;
    {   /* the picture: U8 [C, W, H] */
        int c = (int)sd_tensor_dim(im, 0), w = (int)sd_tensor_dim(im, 1), h = (int)sd_tensor_dim(im, 2); Buf png = { 0 };
        if (sd_tensor_type(im) != SD_TENSOR_U8 || (c != 3 && c != 4) || w < 16 || h < 16) { snprintf(s->err, sizeof s->err, "the engine gave a %d x %d x %d picture", c, w, h); s->st = ST_FAIL; goto done; }
        stbi_write_png_to_func(png_cb, &png, w, h, c, sd_tensor_data(im), w * c);
        finish_shot(item, round, s, png.p, png.n); free(png.p); ok = 1;
    }
    goto done;
fail:
    snprintf(s->err, sizeof s->err, "built-in engine: %s", sd_get_last_error()[0] ? sd_get_last_error() : "the picture failed");
    vlog(VLOG_ERR, "engine", "%s %u: %s", M->id, s->seed, s->err); s->st = ST_FAIL;
done:
    (void)ok;
    if (r) sd_result_free(r); if (in) sd_tensor_free(in); if (o) sd_opt_free(o);
}
#endif
/* a fetched or drawn picture (PNG bytes): saved, made 32 x 32 and to the settings, added to rounds.txt */
static void finish_shot(int item, int round, Shot *s, const void *pngb, size_t pngn) {
    char path[MAX_PATH], line[100]; int dw, dh, n; unsigned char *d;
    shotpath(item, s, 0, path, sizeof path); write_all(path, pngb, pngn);
    d = stbi_load_from_memory(pngb, (int)pngn, &dw, &dh, &n, 3);
    { static unsigned char std32[32 * 32 * 4];
    if (!d || dw < 16 || dh < 16 || !to32(d, dw, dh, fitpic(s->src), 16, 16, std32, 1)) { snprintf(s->err, sizeof s->err, "nothing to cut out of the picture"); if (d) stbi_image_free(d); s->st = ST_FAIL; return; }
    s->drw = d; s->dw = dw; s->dh = dh;
    { ShrinkOpt o; LONG gen; EnterCriticalSection(&S.cs); o = S.opt; gen = S.optgen; LeaveCriticalSection(&S.cs); set_res(s, std32, 32, 32, -1); reshape_one(item, s, &o, gen); }
    shotpath(item, s, 1, path, sizeof path);       /* the standard sprite, as before (rounds.txt's shots load from it) */
    if (!stbi_write_png(path, 32, 32, 4, std32, 32 * 4)) vlog(VLOG_ERR, "png", "could not write %s", path); }
    itemdir(item, path, sizeof path); strcat(path, "\\rounds.txt");
    snprintf(line, sizeof line, "%d %s %u%s", round, S.m[s->model].id, s->seed, s->src == FROM_UP ? " upright" : ""); append_line(path, line);
    s->st = ST_DONE;
}
static DWORD WINAPI worker(LPVOID arg) {
    int item = S.item, rd = S.drawing_round, i, done = 0; Round *R = &S.r[rd];
    char desc[4096], (*tm)[4096] = xmalloc(sizeof(*tm) * MAXM); DWORD t0 = GetTickCount();
    (void)arg;
    unwrap(S.descbuf, desc, sizeof desc);
    for (i = 0; i < S.nm; i++) unwrap(S.m[i].prompt, tm[i], sizeof tm[i]);     /* the prompts as they were at Draw */
again:
    {   /* ComfyUI made ready when a shot first needs it; the item's picture made once for the built-in engine. The
           round is gone through until no shot waits: one thrown out (right-click) while drawing is drawn again too */
        int comfy_ok = -1, last_model = -1; unsigned char *pic = NULL, pic_fail = 0, freed = 0;
#if SD_BUILT
        SDX_FAILED = -1;      /* a model that failed last round may load now (its files may have been got since) */
#endif
        for (;;) {
            Shot *s = NULL; int sd; DWORD t1;
            for (i = 0; i < R->n; i++) if (R->s[i].st == ST_WAIT) { s = &R->s[i]; break; }
            if (!s) break;
            if (S.stop) { for (i = 0; i < R->n; i++) if (R->s[i].st == ST_WAIT) R->s[i].st = ST_STOP; break; }
            sd = use_sd(s->model);
            if (!sd && comfy_ok < 0) comfy_ok = comfy_ready() && upload_input(item);
            if (!sd && !comfy_ok) { s->st = S.stop ? ST_STOP : ST_FAIL; snprintf(s->err, sizeof s->err, "ComfyUI did not start (see the log)"); continue; }
            if (sd && !pic && !pic_fail && !(pic = make_input(item))) pic_fail = 1;
            if (sd && pic_fail) { s->st = ST_FAIL; snprintf(s->err, sizeof s->err, "no picture to give the model"); continue; }
#if SD_BUILT
            /* the graphics card's memory for whichever draws: ComfyUI (ours, still running) asked to let go of its
               models, or the built-in engine's model unloaded */
            if (sd && !freed && S.comfy && WaitForSingleObject(S.comfy, 0) == WAIT_TIMEOUT && comfy_alive())
                http(L"POST", "/free", "{\"unload_models\":true,\"free_memory\":true}", 41, L"Content-Type: application/json", NULL, 30000);
            if (sd) freed = 1;
#endif
            InterlockedExchange(&S.sd_step, 0); InterlockedExchange(&S.sd_steps, 0); S.draw_t0 = t1 = GetTickCount();
            s->st = ST_DRAW;
            status("Drawing %d of %d: %s seed %u%s%s", (int)(s - R->s) + 1, R->n, S.m[s->model].name, s->seed, sd ? " (built-in)" : "",
                   s->model != last_model ? " (loading the model first)" : "");
#if SD_BUILT
            if (sd) draw_one_sd(item, rd, s, desc, tm[s->model], pic);
            else { sd_unload(); draw_one(item, rd, s, desc, tm[s->model]); }
#else
            draw_one(item, rd, s, desc, tm[s->model]);
#endif
            if (s->st == ST_DONE) {       /* how long a picture takes: each picture's progress bar while it draws */
                DWORD dt = GetTickCount() - t1; done++;
                if (s->model == last_model) S.avg_ms[s->model] = S.avg_ms[s->model] ? (S.avg_ms[s->model] * 3 + dt) / 4 : dt;
            }
            last_model = s->model;
            PostMessageA(S.wnd, WM_APP, 0, 0);
        }
        free(pic);
    }
    {   int have = 0; for (i = 0; i < R->n; i++) have += R->s[i].st == ST_DONE;      /* the whole round, earlier ones too */
        if (!S.stop) status("Round %d done: %d of %d there (%d drawn now, %lu s). Double-click one to keep it, or Space for new seeds.",
               rd + 1, have, R->n, done, (GetTickCount() - t0) / 1000);
        else status("Stopped: %d drawn. Double-click one you like, or Space for new seeds.", done); }
    {   /* a picture thrown out after the last look (redraw_one sets ST_WAIT under S.cs and only starts a worker when
           busy is 0): look once more under S.cs before busy is cleared, so none is left waiting (review A3) */
        int more = 0;
        EnterCriticalSection(&S.cs);
        for (i = 0; i < R->n; i++) if (R->s[i].st == ST_WAIT) { if (S.stop) R->s[i].st = ST_STOP; else more = 1; }   /* (review 2, P3-1) */
        if (!more) InterlockedExchange(&S.busy, 0);
        LeaveCriticalSection(&S.cs);
        if (more) goto again;
    }
    free(tm);
    PostMessageA(S.wnd, WM_APP, 0, 0);
    return 0;
}
static unsigned rnd(void) { unsigned v = 0; rand_s(&v); return (v & 0x7fffffff) | 1; }
static unsigned test_seeds[64]; static int ntest_seeds, test_seed_i;      /* self-test --seeds: these first, in order */
static void save_text(int item) {
    char p[MAX_PATH], d[4096];
    itemdir(item, p, sizeof p); mkdirs(p); desc_path(item, S.from, p, sizeof p);
    unwrap(S.descbuf, d, sizeof d); write_all(p, d, strlen(d));
    { int m; for (m = 0; m < S.nm; m++) {         /* a model's prompt file: written once it differs from the standard one */
        char std[4096], w[4096]; const char *st = std_prompt(S.m[m].edit);
        snprintf(p, sizeof p, "%s\\%s-prompt.txt", WORK, S.m[m].id);
        wrap(st, w, sizeof w, 34); unwrap(w, std, sizeof std); unwrap(S.m[m].prompt, d, sizeof d);
        if (exists(p) || strcmp(d, std)) write_all(p, d, strlen(d));
    } }
}
static int round_size(void) { int m, t = 0; for (m = 0; m < S.nm; m++) if (S.m[m].ready) t += S.m[m].n; return t; }
/* NNN\round-R.txt: what a round was drawn with (review A4): the description, the picture it drew from, each model's
   engine, strength, LoRA and filled-in prompt. keep.txt takes the description from here, not from the box as it is now */
static void round_info_path(int item, int round, char *p, int n) { itemdir(item, p, n); snprintf(p + strlen(p), n - strlen(p), "\\round-%d.txt", round + 1); }
static void write_round_info(int item, int round, const Round *R) {
    Buf b = { 0 }; char p[MAX_PATH], d[4096], t[4096], pr[12000], l[400]; int m, i, n;
    unwrap(S.descbuf, d, sizeof d);
    snprintf(l, sizeof l, "round %d of %03d, drawn from: %s\r\ndescription: ", round + 1, item, FROM_NAMES[R->n ? R->s[0].src : S.from]); buf_str(&b, l);
    buf_str(&b, d); buf_str(&b, "\r\n");
    for (m = 0; m < S.nm; m++) {
        for (i = n = 0; i < R->n; i++) n += R->s[i].model == m;
        if (!n) continue;
        snprintf(l, sizeof l, "model %s: %d picture(s), engine %s", S.m[m].id, n, use_sd(m) ? "built-in" : "ComfyUI"); buf_str(&b, l);
        if (S.m[m].sdimage == 2) { snprintf(l, sizeof l, ", strength %d%%", S.m[m].strength); buf_str(&b, l); }
        if (S.m[m].has_lora) { snprintf(l, sizeof l, ", LoRA %d%%", S.m[m].lora_w); buf_str(&b, l); }
        unwrap(S.m[m].prompt, t, sizeof t); make_prompt(t, d, pr, sizeof pr);
        buf_str(&b, "\r\nprompt: "); buf_str(&b, pr); buf_str(&b, "\r\n");
    }
    round_info_path(item, round, p, sizeof p);
    if (!write_all(p, b.p ? (const void *)b.p : "", b.n)) vlog(VLOG_WARN, "round", "could not write %s", p);
    free(b.p);
}
/* the description a picture was drawn with: its own "redraw" line in round-R.txt (drawn again later, review 2 P3-2),
   else the round's, else the box as it is */
static void round_desc(int item, int round, const char *mid, unsigned seed, char *d, int n) {
    char p[MAX_PATH], *t, *q, *e, k[80];
    round_info_path(item, round, p, sizeof p);
    snprintf(k, sizeof k, "\nredraw %s %u description: ", mid, seed);
    if ((t = read_all(p, NULL)) && ((q = strstr(t, k)) != NULL)) { q += strlen(k); if ((e = strpbrk(q, "\r\n"))) *e = 0; snprintf(d, n, "%s", q); free(t); return; }
    if (t && (q = strstr(t, "\ndescription: "))) {
        q += 14; if ((e = strpbrk(q, "\r\n"))) *e = 0; snprintf(d, n, "%s", q); free(t); return;
    }
    free(t); unwrap(S.descbuf, d, n);
}
static void start_round(void) {
    Round *R; int i, m; HANDLE th; char d[64];
    if (S.busy || round_size() < 1) return;
    if (S.nr >= MAXR) { status("%03d has %d rounds, the most one item can keep: no new round (A17).", S.item, MAXR); return; }
    if (round_size() > MAXSHOT) { status("At most %d pictures a round: set fewer.", MAXSHOT); return; }
    unwrap(S.descbuf, d, sizeof d);
    if (!d[0]) { status("Write what the item is first (the box under the pictures)."); return; }
    S.desc_made = 0;     /* drawn with it: the "made from the other, check it" label has done its work (P3-4) */
    save_text(S.item);
    R = &S.r[S.nr]; memset(R, 0, sizeof *R);
    for (m = 0; m < S.nm; m++) for (i = 0; S.m[m].ready && i < S.m[m].n; i++) {
        unsigned s; if (test_seed_i < ntest_seeds) s = test_seeds[test_seed_i++]; else do s = rnd(); while (seed_used(s));
        R->s[R->n].model = m; R->s[R->n].seed = s; R->s[R->n].st = ST_WAIT; R->s[R->n].src = S.from == FROM_UP && S.has_up ? FROM_UP : S.from;
        R->n++;
    }
    write_round_info(S.item, S.nr, R);
    S.drawing_round = S.nr; S.cur = S.nr; S.nr++; S.sel = -1;
    S.stop = 0; InterlockedExchange(&S.busy, 1);
    th = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    if (th) CloseHandle(th); else { InterlockedExchange(&S.busy, 0); status("Could not start the drawing thread: %s", vlog_errno()); }
}
static int UNDOING;
/* a shrink setting changed: a new generation, the shown round made again (the main loop each frame; keep() too, so a
   key that changes a setting and Enter in the same batch keep the new one, review 2 P3-5) */
static int palette_apply(const ShrinkOpt *o, int nlist, const int *list);
static void opt_check(void) {
    static ShrinkOpt last; static int have, last_item = -1; static DWORD last_t; static unsigned pal_key;
    if (S.opt.colours == C_PALETTE) {    /* a new palette (another one chosen, its file, the Find box for the set's): the round remade */
        unsigned k = 5381u + (unsigned)S.opt.palid * 7919u; const char *c;
        for (c = P_PALETTE; *c; c++) k = k * 33 + (unsigned char)*c;
        if (S.opt.palid == PAL_SET) for (c = S.find; *c; c++) k = k * 31 + (unsigned char)*c;
        if (k != pal_key) { int n; pal_key = k; stop_reshape(); n = palette_apply(&S.opt, S.nlist, S.list);
            if (!n) status("The palette has no colours yet: %s", S.opt.palid == PAL_FILE ? "choose a palette file (Settings, Colours)." : "no item fits the Find box.");
            InterlockedIncrement(&S.optgen); kick_reshape(); }
    } else pal_key = 0;
    if (!have || memcmp(&last, &S.opt, sizeof last)) {
        if (have) {
            InterlockedIncrement(&S.optgen); kick_reshape(); if (S.own_opt) item_opt_save(S.item);
            if (!UNDOING && last_item == S.item && (GetTickCount() - last_t > 700 || !S.nundo || S.undo[S.nundo - 1].kind != 2)) {
                int u; if (S.nundo == 64) { memmove(&S.undo[0], &S.undo[1], sizeof S.undo[0] * 63); S.nundo--; }
                u = S.nundo++; S.undo[u].kind = 2; S.undo[u].item = S.item; S.undo[u].opt = last;     /* the settings from before */
            }
            last_t = GetTickCount();
        }
        last = S.opt; have = 1;
    }
    last_item = S.item; UNDOING = 0;
}
static void keep(void) {
    Shot *s; char dst[MAX_PATH], bk[MAX_PATH], line[6000], d[4096], stamp[32], how[600]; int i, ok = 1, rw, rh; unsigned char *px; time_t t = time(NULL); struct tm *tm = localtime(&t);
    if (S.sel < 0 || S.cur >= S.nr || S.sel >= S.r[S.cur].n) return;
    s = &S.r[S.cur].s[S.sel];
    if (s->st != ST_DONE) return;
    opt_check();
    if (s->gen != S.optgen) {      /* not yet remade with the settings as they are: remade now, so Keep writes what the
                                      settings say (review A10); the reshaper is stopped first, then goes on */
        stop_reshape(); reshape_one(S.item, s, &S.opt, S.optgen); kick_reshape();
        if (!s->drw) { char sp[MAX_PATH]; shotpath(S.item, s, 0, sp, sizeof sp); status("The drawing %s is gone: not kept (its sprite would be the old one).", sp); return; }
    }
    strftime(stamp, sizeof stamp, "%Y-%m-%d_%H%M%S", tm);
    {   /* two keeps in the same second get their own backup folders (else the second's backup overwrites the first's
           and undoing both cannot put the picture from before back; B5) */
        static char prev[32]; static int dup; char base[32]; snprintf(base, sizeof base, "%s", stamp);
        if (!strcmp(base, prev)) snprintf(stamp + strlen(stamp), sizeof stamp - strlen(stamp), "-%d", ++dup); else dup = 0;
        snprintf(prev, sizeof prev, "%s", base);
    }
    if (!s->res) return;
    EnterCriticalSection(&S.cs); rw = s->rw; rh = s->rh; px = xmalloc((size_t)rw * rh * 4); memcpy(px, s->res, (size_t)rw * rh * 4); LeaveCriticalSection(&S.cs);
    shrink_describe(&S.opt, how, sizeof how);
    if (rw > GAME_MAX || rh > GAME_MAX || !N_GAME) {    /* bigger than the game takes (voxup.ini game_max), or no game folder: WORK\export */
        snprintf(dst, sizeof dst, "%s\\export", WORK); mkdirs(dst);
        snprintf(dst + strlen(dst), sizeof dst - strlen(dst), "\\%03d_%dx%d_%s_s%u.png", S.item, rw, rh, S.m[s->model].id, s->seed);
        if (!stbi_write_png(dst, rw, rh, 4, px, rw * 4)) { vlog(VLOG_ERR, "keep", "could not write %s", dst); ok = 0; }
        else if (N_GAME) status("%d x %d is bigger than the game takes (%d x %d): saved as %s", rw, rh, GAME_MAX, GAME_MAX, dst);
        else status("Saved as %s (voxup.ini has no game = folder)", dst);
        free(px);
        round_desc(S.item, S.cur, S.m[s->model].id, s->seed, d, sizeof d);
        snprintf(line, sizeof line, "%s  %03d %s  %s seed %u  round %d  %dx%d  shrink: %s  EXPORT %s  | %s", stamp, S.item, S.name[S.item], S.m[s->model].name, s->seed, S.cur + 1, rw, rh, how, dst, d);
        snprintf(bk, sizeof bk, "%s\\keep.txt", WORK); append_line(bk, line);
        S.nkeeps++; return;
    }
    for (i = 0; i < N_GAME; i++) {
        snprintf(dst, sizeof dst, "%s\\%03d.png", GAMEDIRS[i], S.item);
        S.undo_had[i] = exists(dst) != 0;
        if (exists(dst)) {
            snprintf(bk, sizeof bk, "%s\\backup\\%s\\%s", WORK, stamp, GAMETAG[i]); mkdirs(bk);
            snprintf(bk + strlen(bk), sizeof bk - strlen(bk), "\\%03d.png", S.item);
            if (!CopyFileA(dst, bk, FALSE)) { vlog(VLOG_ERR, "keep", "could not back up %s to %s: %s", dst, bk, vlog_errno()); ok = 0; continue; }
        } else mkdirs(GAMEDIRS[i]);
        if (!stbi_write_png(dst, rw, rh, 4, px, rw * 4)) { vlog(VLOG_ERR, "keep", "could not write %s", dst); ok = 0; }
    }
    free(px);
    round_desc(S.item, S.cur, S.m[s->model].id, s->seed, d, sizeof d);
    snprintf(line, sizeof line, "%s  %03d %s  %s seed %u  round %d  %dx%d  shrink: %s  | %s", stamp, S.item, S.name[S.item], S.m[s->model].name, s->seed, S.cur + 1, rw, rh, how, d);
    snprintf(dst, sizeof dst, "%s\\keep.txt", WORK); append_line(dst, line);
    itemdir(S.item, dst, sizeof dst); strcat(dst, "\\kept.txt");
    snprintf(line, sizeof line, "%s seed %u (round %d, %dx%d, %s, %s)", S.m[s->model].name, s->seed, S.cur + 1, rw, rh, how, stamp); write_all(dst, line, strlen(line));
    S.kept[S.item] = 1; read_kept(S.item);
    S.undo_item = S.item; snprintf(S.undo_stamp, sizeof S.undo_stamp, "%s", stamp);
    {   int u; if (S.nundo == 64) { memmove(&S.undo[0], &S.undo[1], sizeof S.undo[0] * 63); S.nundo--; }
        u = S.nundo++; S.undo[u].kind = 1; S.undo[u].item = S.item; memcpy(S.undo[u].had, S.undo_had, sizeof S.undo_had); snprintf(S.undo[u].stamp, sizeof S.undo[u].stamp, "%s", stamp); }
    S.nkeeps++;
    if (ok) status("Kept %s seed %u as %03d's new picture (%s%s).", S.m[s->model].name, s->seed, S.item, GAMEDIRS[0], N_GAME > 1 ? " and the other game folders" : "");
    else status("Keep failed for part of it: %s", vlog_last());
}
/* the last keep taken back: each game folder gets its picture from before (backup\STAMP); where there was none, the
   kept picture is moved to backup\STAMP-undone (nothing is deleted). kept.txt becomes kept-undone.txt */
static void undo_keep(void) {
    char dst[MAX_PATH], bk[MAX_PATH], un[MAX_PATH], line[300]; int i, ok = 1, item = S.undo_item, earlier = 0;
    if (item < 0) return;
    for (i = 0; i < S.nundo; i++) earlier |= S.undo[i].kind == 1 && S.undo[i].item == item;   /* an earlier keep of it is still on the stack */
    for (i = 0; i < N_GAME; i++) {
        snprintf(dst, sizeof dst, "%s\\%03d.png", GAMEDIRS[i], item);
        snprintf(un, sizeof un, "%s\\backup\\%s-undone\\%s", WORK, S.undo_stamp, GAMETAG[i]); mkdirs(un);
        snprintf(un + strlen(un), sizeof un - strlen(un), "\\%03d.png", item);
        if (exists(dst) && !CopyFileA(dst, un, FALSE)) { vlog(VLOG_ERR, "undo", "could not save %s to %s: %s", dst, un, vlog_errno()); ok = 0; continue; }
        if (S.undo_had[i]) {
            snprintf(bk, sizeof bk, "%s\\backup\\%s\\%s\\%03d.png", WORK, S.undo_stamp, GAMETAG[i], item);
            if (!CopyFileA(bk, dst, FALSE)) { vlog(VLOG_ERR, "undo", "could not put %s back: %s", bk, vlog_errno()); ok = 0; }
        } else if (exists(dst) && !MoveFileExA(dst, un, MOVEFILE_REPLACE_EXISTING)) { vlog(VLOG_ERR, "undo", "could not move %s to %s: %s", dst, un, vlog_errno()); ok = 0; }
    }
    itemdir(item, dst, sizeof dst); strcat(dst, "\\kept.txt");
    itemdir(item, un, sizeof un); strcat(un, "\\kept-undone.txt");
    if (!earlier && exists(dst)) MoveFileExA(dst, un, MOVEFILE_REPLACE_EXISTING);
    snprintf(line, sizeof line, "%s  %03d UNDONE (the pictures from before put back; the kept one in backup\\%s-undone)", S.undo_stamp, item, S.undo_stamp);
    snprintf(dst, sizeof dst, "%s\\keep.txt", WORK); append_line(dst, line);
    S.kept[item] = (unsigned char)earlier; if (item == S.item) read_kept(item);
    S.undo_item = -1;
    if (ok) status("%03d: the keep is undone; the picture from before is back in the game folders.", item);
    else status("Undo failed for part of it: %s", vlog_last());
}

/* Ctrl+Z: the last thing done this session, a keep or a setting change (B5) */
static void undo_any(void) {
    int u;
    if (!S.nundo || S.busy) return;
    u = --S.nundo;
    if (S.undo[u].kind == 1) {
        S.undo_item = S.undo[u].item; memcpy(S.undo_had, S.undo[u].had, sizeof S.undo_had); snprintf(S.undo_stamp, sizeof S.undo_stamp, "%s", S.undo[u].stamp);
        undo_keep();
    } else if (S.undo[u].item == S.item) {
        char d[600]; S.opt = S.undo[u].opt; UNDOING = 1;
        shrink_describe(&S.opt, d, sizeof d); vlog(VLOG_INFO, "undo", "settings back to before: %s", d);
        status("Settings back to how they were (Ctrl+Z again goes further back).");
    } else status("That setting change was on item %03d: go there to undo it.", S.undo[u].item), S.nundo++;
}
static const char *undo_what(char *b, size_t n) {
    if (!S.nundo) { b[0] = 0; return b; }
    if (S.undo[S.nundo - 1].kind == 1) snprintf(b, n, "Undo the keep of %03d (Ctrl+Z)", S.undo[S.nundo - 1].item);
    else snprintf(b, n, "Undo the setting change (Ctrl+Z)");
    return b;
}
/* ---------------------------------------------------------------- the item filter (the Find box): words, all must fit:
   a number (the item's number starts with it), kept, new (not kept), tilted, queued, or a part of the name */
static int item_tilted(int k) {
    if (S.tilt[k] < 0) {
        char p[MAX_PATH]; int w, h; unsigned char *d; double t, el; int tl = 0;
        snprintf(p, sizeof p, "%s\\%03d.png", P_SPRITES, k);
        if ((d = load_rgba(p, &w, &h))) { if (w == 16 && h == 16) upright_measure(d, 16, 16, &t, &el, &tl); stbi_image_free(d); }
        S.tilt[k] = (signed char)(tl != 0);
    }
    return S.tilt[k];
}
static int queued(int k) { int i; for (i = 0; i < S.nq; i++) if (S.queue[i] == k) return 1; return 0; }
static int item_fits(int k) {
    char f[48], *w, num[8], num3[8], nm[48]; int i;
    if (!S.find[0]) return 1;
    snprintf(f, sizeof f, "%s", S.find); for (i = 0; f[i]; i++) f[i] = (char)tolower((unsigned char)f[i]);
    snprintf(num, sizeof num, "%d", k); snprintf(num3, sizeof num3, "%03d", k);
    snprintf(nm, sizeof nm, "%s", S.name[k]); for (i = 0; nm[i]; i++) nm[i] = (char)tolower((unsigned char)nm[i]);
    for (w = strtok(f, " ,"); w; w = strtok(NULL, " ,")) {
        if (isdigit((unsigned char)w[0])) { if (strncmp(num, w, strlen(w)) && strncmp(num3, w, strlen(w))) return 0; }
        else if (!strcmp(w, "kept")) { if (!S.kept[k]) return 0; }
        else if (!strcmp(w, "new")) { if (S.kept[k]) return 0; }
        else if (!strcmp(w, "tilted")) { if (!item_tilted(k)) return 0; }
        else if (!strcmp(w, "queued")) { if (!queued(k)) return 0; }
        else if (!strstr(nm, w)) return 0;
    }
    return 1;
}
static int count_fits(void) { int i, n = 0; for (i = 0; i < S.nlist; i++) n += item_fits(S.list[i]); return n; }
static void step_item(int dir) {        /* the next / earlier item the filter lets through */
    int i, p = S.pos;
    for (i = 0; i < S.nlist; i++) { p = (p + dir + S.nlist) % S.nlist; if (item_fits(S.list[p])) { choose(p); return; } }
    status("No item fits \"%s\".", S.find);
}
static void first_fit(void) { int i; for (i = 0; i < S.nlist; i++) if (item_fits(S.list[i])) { if (i != S.pos) choose(i); return; } status("No item fits \"%s\".", S.find); }

/* ---------------------------------------------------------------- the queue: a round drawn for each item in turn
   (overnight), with the model counts and settings as they are; work\queue.txt */
static void queue_save(void) {
    char p[MAX_PATH]; Buf b = { 0 }; int i; char l[16];
    for (i = 0; i < S.nq; i++) { snprintf(l, sizeof l, "%d\r\n", S.queue[i]); buf_str(&b, l); }
    snprintf(p, sizeof p, "%s\\queue.txt", WORK); write_all(p, b.p ? (char *)b.p : "", b.n); free(b.p);
}
static void queue_load(void) {
    char p[MAX_PATH], *t, *l; snprintf(p, sizeof p, "%s\\queue.txt", WORK); S.nq = 0;
    if (!(t = read_all(p, NULL))) return;
    for (l = strtok(t, "\r\n"); l; l = strtok(NULL, "\r\n")) { int k = atoi(l); if (k >= 0 && k < 512 && S.nq < 512 && !queued(k)) S.queue[S.nq++] = k; }
    free(t);
}
static void queue_toggle(int k) {
    int i;
    for (i = 0; i < S.nq; i++) if (S.queue[i] == k) { memmove(S.queue + i, S.queue + i + 1, sizeof(int) * (size_t)(S.nq - i - 1)); S.nq--; queue_save(); status("%03d taken off the queue (%d left).", k, S.nq); return; }
    if (S.nq < 512) { S.queue[S.nq++] = k; queue_save(); status("%03d is on the queue (%d): Run queue draws a round of each.", k, S.nq); }
}
static void queue_add_fits(void) { int i, n = 0; for (i = 0; i < S.nlist; i++) { int k = S.list[i]; if (item_fits(k) && !queued(k) && S.nq < 512) { S.queue[S.nq++] = k; n++; } } queue_save(); status("%d item(s) put on the queue (%d on it).", n, S.nq); }
/* the main loop, when nothing draws: the next queued item's round. An item leaves the queue only when its round has
   started; when one cannot start, the queue stops and says why, the item still first on it (review A5) */
static void queue_tick(void) {
    int k, i, nr0; char d[64];
    if (!S.qrun || S.busy || S.rs_busy) return;
    if (!S.nq) { S.qrun = 0; status("The queue is done."); return; }
    k = S.queue[0];
    for (i = 0; i < S.nlist && S.list[i] != k; i++) ;
    if (i == S.nlist) { S.qrun = 0; status("Queue stopped: %03d is not one of the items voxup shows (voxup.ini items = and skip =): take it off the queue (Q on it), then R to go on.", k); return; }
    if (S.list[S.pos] != k) choose(i);
    nr0 = S.nr; start_round();
    if (S.nr > nr0) { memmove(S.queue, S.queue + 1, sizeof(int) * (size_t)(S.nq - 1)); S.nq--; queue_save(); return; }
    S.qrun = 0; unwrap(S.descbuf, d, sizeof d);
    if (round_size() < 1) status("Queue stopped at %03d: no model is set to draw (every count is 0, or none is on this computer). Set one, then R to go on.", k);
    else if (round_size() > MAXSHOT) status("Queue stopped at %03d: more than %d pictures a round. Set fewer, then R to go on.", k, MAXSHOT);
    else if (!d[0]) status("Queue stopped at %03d: it has no description. Write one, then R to go on.", k);
    else if (S.nr >= MAXR) status("Queue stopped at %03d: it has %d rounds, the most one item can keep.", k, MAXR);
    else status("Queue stopped at %03d: its round did not start.", k);
    vlog(VLOG_WARN, "queue", "stopped at %03d: no round started", k);
}

/* ---------------------------------------------------------------- one picture thrown out (right-click, Delete) and
   drawn again in its place with a new seed; rejected.txt keeps it out of the round when the item is loaded again
   (its files stay) */
static void redraw_one(int k) {
    Round *R; Shot *s; char p[MAX_PATH], l[80]; unsigned sd; HANDLE th;
    if (!S.nr || S.cur >= S.nr) return;
    R = &S.r[S.cur]; if (k < 0 || k >= R->n) return;
    s = &R->s[k];
    if (s->st == ST_DRAW || s->st == ST_WAIT) return;
    if (S.busy && S.drawing_round != S.cur) { status("Another round is drawing: throw this one out when it is done."); return; }
    if (!S.m[s->model].ready) { status("%s is not on this computer any more.", S.m[s->model].name); return; }
    stop_reshape();
    if (s->st == ST_DONE) { itemdir(S.item, p, sizeof p); strcat(p, "\\rejected.txt"); snprintf(l, sizeof l, "%s %u", S.m[s->model].id, s->seed); append_line(p, l); }
    EnterCriticalSection(&S.cs);
    if (s->bm) DeleteObject(s->bm); s->bm = NULL; free(s->res); s->res = NULL; if (s->drw) stbi_image_free(s->drw); s->drw = NULL;
    s->gen = 0; s->bmgen = 0; s->err[0] = 0;
    do sd = rnd(); while (seed_used(sd));
    s->seed = sd; s->src = S.from == FROM_UP && S.has_up ? FROM_UP : S.from; s->st = ST_WAIT;
    {   /* the box now says something else than when the round was drawn: this picture's own line (review 2, P3-2) */
        char was[4096], now[4096], rp[MAX_PATH], line[4200], *t2, *q2, *e2; was[0] = 0;
        unwrap(S.descbuf, now, sizeof now); round_info_path(S.item, S.cur, rp, sizeof rp);
        if ((t2 = read_all(rp, NULL)) && (q2 = strstr(t2, "\ndescription: "))) { q2 += 14; if ((e2 = strpbrk(q2, "\r\n"))) *e2 = 0; snprintf(was, sizeof was, "%s", q2); }
        free(t2);
        if (strcmp(was, now)) { snprintf(line, sizeof line, "redraw %s %u description: %s", S.m[s->model].id, sd, now); append_line(rp, line); }
    }
    {   int running = S.busy != 0;       /* tested under S.cs: the worker looks again under it before it ends */
        if (!running) { S.drawing_round = S.cur; S.stop = 0; InterlockedExchange(&S.busy, 1); }
        LeaveCriticalSection(&S.cs);
        if (S.sel == k) S.sel = -1;
        S.pv_for = -1; S.cmp_for = -1;
        kick_reshape();
        status("Picture %d thrown out: a new one is drawn in its place.", k + 1);
        if (running) return;             /* the drawing round's worker comes to it */
    }
    th = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    if (th) CloseHandle(th); else { InterlockedExchange(&S.busy, 0); status("Could not start the drawing thread: %s", vlog_errno()); }
}

/* ---------------------------------------------------------------- Compare: the chosen drawing through every preset,
   with a score each: shape (how much of the sprite and the original's box at this size cover each other), colour
   (the mean distance of each pixel from the original's nearest colour), lone (pixels of a colour no neighbour has) */
/* ref: the shape to match (the original 16 x 16, or the upright picture 32 x 32 for a drawing made from it) */
static void score_sprite(const unsigned char *spr, int W, int H, const unsigned char *ref, int rs, char *o, int n) {
    int x, y, both = 0, any = 0, lone = 0, cnt = 0, i, np = 0; double cerr = 0; unsigned char pal[256][3];
    for (i = 0; i < 256; i++) if (S.orig[i * 4 + 3] >= 128 && np < 256) { pal[np][0] = S.orig[i * 4]; pal[np][1] = S.orig[i * 4 + 1]; pal[np][2] = S.orig[i * 4 + 2]; np++; }
    for (y = 0; y < H; y++) for (x = 0; x < W; x++) {
        const unsigned char *q = spr + ((size_t)y * W + x) * 4; int a = q[3] >= 128, b = ref[((y * rs / H) * rs + x * rs / W) * 4 + 3] >= 128;
        both += a && b; any += a || b;
        if (!a) continue;
        cnt++;
        if (np) { int best = 1 << 30; for (i = 0; i < np; i++) { int dr = q[0] - pal[i][0], dg = q[1] - pal[i][1], db = q[2] - pal[i][2], d = dr * dr + dg * dg + db * db; if (d < best) best = d; } cerr += sqrt((double)best); }
        { int same = 0, dx[4] = { 1, -1, 0, 0 }, dy[4] = { 0, 0, 1, -1 }, j;
          for (j = 0; j < 4; j++) { int u = x + dx[j], v = y + dy[j]; const unsigned char *r; if (u < 0 || v < 0 || u >= W || v >= H) continue; r = spr + ((size_t)v * W + u) * 4; if (r[3] >= 128 && r[0] == q[0] && r[1] == q[1] && r[2] == q[2]) same = 1; }
          lone += !same; }
    }
    {   /* col only when the colours are not the original's own (then it is always 0); lone per 100 pixels (review A13) */
        int col = cnt ? (int)(cerr / cnt + 0.5) : 0, lp = cnt ? (lone * 100 + cnt / 2) / cnt : 0, fit = any ? both * 100 / any : 0;
        if (col) snprintf(o, (size_t)n, "fit %d%%  col %d  lone %d%%", fit, col, lp);
        else snprintf(o, (size_t)n, "fit %d%%  lone %d%%", fit, lp);
    }
}
static void compare_make(Shot *s) {
    char p[MAX_PATH]; int dw, dh, nn, i, W, H, z; unsigned char *d; static unsigned char out[128 * 128 * 4];
    /* made again for another item, round, picture or preset list, and after any setting change or upright Remake
       (both bump S.optgen): the colours, fit and upright box are part of every picture (review A9) */
    LONG key = (LONG)(S.item * 100000 + S.cur * 100 + S.sel), gen = S.optgen * 64 + S.np;
    if (S.cmp_for == key && S.cmp_gen == gen) return;
    for (i = 0; i < 17; i++) { if (S.cmp_bm[i]) DeleteObject(S.cmp_bm[i]); if (S.cmp_bm1[i]) DeleteObject(S.cmp_bm1[i]); S.cmp_bm[i] = S.cmp_bm1[i] = NULL; S.cmp_score[i][0] = 0; }
    S.cmp_for = key; S.cmp_gen = gen;
    shotpath(S.item, s, 0, p, sizeof p);
    if (!(d = stbi_load(p, &dw, &dh, &nn, 3))) { vlog(VLOG_WARN, "compare", "no drawing %s", p); return; }
    for (i = 0; i < S.np && i < 16; i++) {
        ShrinkOpt o = S.opt; preset_apply_to(&o, i); shrink_size(&o, 16, 16, &W, &H);
        outline_for(&o, s->src);
        { int fw, fh; const unsigned char *fp = fit_for(s->src, &o, &fw, &fh); if (!shrink(d, dw, dh, fp, fw, fh, &o, out)) continue; }
        z = 96 / (W > H ? W : H); if (z < 1) z = 1;
        S.cmp_bm[i] = mkbmp(out, W, H, z, 0); S.cmp_w[i] = W * z; S.cmp_h[i] = H * z;
        S.cmp_bm1[i] = mkbmp(out, W, H, 1, 0); S.cmp_w1[i] = W; S.cmp_h1[i] = H;     /* as small as in the game */
        if (s->src == FROM_UP && S.has_up) score_sprite(out, W, H, S.up, 32, S.cmp_score[i], sizeof S.cmp_score[i]);
        else score_sprite(out, W, H, S.orig, 16, S.cmp_score[i], sizeof S.cmp_score[i]);
    }
    stbi_image_free(d);
}

/* ---------------------------------------------------------------- keys (none while a text box is typed in) */
static void after_keep(void) {          /* "After Keep: the next new item" */
    int i, p = S.pos;
    if (!S.auto_next || S.busy) return;
    for (i = 0; i < S.nlist; i++) { p = (p + 1) % S.nlist; if (!S.kept[S.list[p]] && item_fits(S.list[p])) { choose(p); return; } }
}
static void keep_next(void) { int n = S.nkeeps; keep(); if (S.nkeeps != n) after_keep(); }
static void move_sel(int dx, int dy) {
    Round *R; int k;
    if (!S.nr) return;
    R = &S.r[S.cur]; if (!R->n) return;
    k = S.sel < 0 ? 0 : S.sel + dx + dy * (S.per > 0 ? S.per : 5);     /* up/down: a row of the grid as it is laid out now */
    if (k < 0) k = 0;
    if (k >= R->n) k = R->n - 1;
    S.sel = k; if (S.tab == 1) S.tab = 0;
}
static void fold_set(int side, int f);
static void set_tab(int t) { S.tab = t; if (S.fold[1]) fold_set(1, 0); }
static int hotkey(int vk) {
    int ctrl = GetKeyState(VK_CONTROL) < 0, busy = S.busy != 0;
    switch (vk) {
    case VK_PRIOR: if (!busy) step_item(-1); return 1;
    case VK_NEXT: if (!busy) step_item(1); return 1;
    case VK_LEFT: move_sel(-1, 0); return 1;
    case VK_RIGHT: move_sel(1, 0); return 1;
    case VK_UP: move_sel(0, -1); return 1;
    case VK_DOWN: move_sel(0, 1); return 1;
    case VK_SPACE: if (!busy) start_round(); return 1;
    case VK_RETURN: case 'K': if (!ctrl) keep_next(); return 1;
    case VK_DELETE: case 'X': redraw_one(S.sel); return 1;
    case 'Z': if (ctrl && !busy) undo_any(); return ctrl;
    case VK_ESCAPE: if (busy && !S.stop) { InterlockedExchange(&S.stop, 1); status("Stopping after this picture...");
#if SD_BUILT
                        if (SDX_DRAWING && SDX) sd_cancel_generation(SDX, SD_CANCEL_ALL);
#endif
                    } S.qrun = 0; return 1;
    case VK_HOME: case VK_OEM_COMMA: if (S.cur > 0) { S.cur--; S.sel = -1; kick_reshape(); } return 1;
    case VK_END: case VK_OEM_PERIOD: if (S.cur < S.nr - 1) { S.cur++; S.sel = -1; kick_reshape(); } return 1;
    case 'F': if (ctrl) { S.find_focus = 1; return 1; } return 0;
    case VK_OEM_4: if (ctrl) { fold_set(0, !S.fold[0]); return 1; } return 0;      /* Ctrl+[ : the left panel folded / open */
    case VK_OEM_6: if (ctrl) { fold_set(1, !S.fold[1]); return 1; } return 0;      /* Ctrl+] : the right panel */
    case VK_OEM_2: S.find_focus = 1; return 1;                     /* the / key */
    case 'Q': queue_toggle(S.item); return 1;
    case 'R': if (!busy && S.nq) { S.qrun = 1; status("Running the queue: a round for each of %d item(s).", S.nq); } return 1;
    case '1': set_tab(0); return 1;
    case '2': set_tab(1); return 1;
    case '3': set_tab(2); return 1;
    case 'O': S.opt.colours = C_ORIGINAL; return 1;
    case 'M': S.opt.colours = C_OWN; return 1;
    }
    if (vk >= VK_F1 && vk <= VK_F12 && vk - VK_F1 < S.np && !S.phid[vk - VK_F1]) { preset_apply_to(&S.opt, vk - VK_F1); status("Preset: %s", S.pname[vk - VK_F1]); return 1; }
    return 0;
}

/* ---------------------------------------------------------------- getting a model: its missing files from their links.
   Each file goes to NAME.part first (a stopped one goes on from where it stopped, by an HTTP range), is checked against
   models.txt's size, and only then gets its real name, so ComfyUI never sees half a file. */
static void dl_say(const char *fmt, ...) {
    va_list a; char b[300]; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    EnterCriticalSection(&S.cs); strcpy(S.dl_msg, b); LeaveCriticalSection(&S.cs);
    vlog(VLOG_INFO, "get", "%s", b);
    if (S.wnd) PostMessageA(S.wnd, WM_APP, 0, 0);
}
static double gb(long long n) { return (double)n / 1e9; }
static void ini_set(const char *key, const char *val);
/* the licences clicked through (voxup.ini licence_ok = ID,ID,...): Get never downloads a model before that */
static char LIC_OK[2048];
static int lic_ok(int m) {
    char k[32]; size_t n; const char *q;
    if (!S.m[m].nlic) return 1;
    snprintf(k, sizeof k, "%s", S.m[m].id); n = strlen(k);
    for (q = LIC_OK; (q = strstr(q, k)); q += n) if ((q == LIC_OK || q[-1] == ',') && (q[n] == ',' || !q[n])) return 1;
    return 0;
}
static void lic_accept(int m) {
    if (lic_ok(m)) return;
    if (LIC_OK[0]) strncat(LIC_OK, ",", sizeof LIC_OK - strlen(LIC_OK) - 1);
    strncat(LIC_OK, S.m[m].id, sizeof LIC_OK - strlen(LIC_OK) - 1);
    ini_set("licence_ok", LIC_OK); vlog(VLOG_INFO, "get", "%s: licence accepted", S.m[m].id);
}
/* the Hugging Face token: Windows' Credential Manager only (never voxup.ini, never the log) */
#define HF_CRED L"voxup:huggingface"
static int hf_token(char *out, size_t n) {
    PCREDENTIALW c = NULL; int ok = 0;
    if (CredReadW(HF_CRED, CRED_TYPE_GENERIC, 0, &c)) {
        if (c->CredentialBlobSize && c->CredentialBlobSize < n) { memcpy(out, c->CredentialBlob, c->CredentialBlobSize); out[c->CredentialBlobSize] = 0; ok = 1; }
        SecureZeroMemory(c->CredentialBlob, c->CredentialBlobSize); CredFree(c);
    }
    return ok;
}
static int hf_token_save(const char *t) {
    CREDENTIALW c; memset(&c, 0, sizeof c);
    c.Type = CRED_TYPE_GENERIC; c.TargetName = (LPWSTR)HF_CRED; c.CredentialBlobSize = (DWORD)strlen(t); c.CredentialBlob = (LPBYTE)t;
    c.Persist = CRED_PERSIST_LOCAL_MACHINE; c.UserName = (LPWSTR)L"huggingface";
    if (!CredWriteW(&c, 0)) { vlog(VLOG_ERR, "get", "could not keep the Hugging Face token in Credential Manager: %s", vlog_errno()); return 0; }
    vlog(VLOG_INFO, "get", "Hugging Face token kept in Windows' Credential Manager (%s)", "voxup:huggingface"); return 1;
}
/* a file's SHA-256 as hex (Windows' BCrypt); 0 when it could not be read */
static int sha256_file(const char *path, char hex[65]) {
    BCRYPT_ALG_HANDLE a = NULL; BCRYPT_HASH_HANDLE h = NULL; unsigned char d[32]; FILE *f; static unsigned char b[1 << 20]; size_t n; int i, ok = 0;
    if (!(f = fopen(path, "rb"))) return 0;
    if (BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0 && BCryptCreateHash(a, &h, NULL, 0, NULL, 0, 0) == 0) {
        ok = 1;
        while ((n = fread(b, 1, sizeof b, f)) > 0) { if (BCryptHashData(h, b, (ULONG)n, 0)) { ok = 0; break; } if (S.dl_stop) { ok = -1; break; } }
        if (ok == 1 && BCryptFinishHash(h, d, 32, 0)) ok = 0;
    }
    if (h) BCryptDestroyHash(h); if (a) BCryptCloseAlgorithmProvider(a, 0);
    fclose(f);
    if (ok == 1) for (i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", d[i]);
    return ok;
}
/* one file; 1 = there, 0 = failed (dl_msg says why), -1 = stopped */
/* the token goes only to Hugging Face: the host is huggingface.co or ends in .huggingface.co, any case (review 3,
   P2-3: "huggingface.co.example.net" got it before) */
static int hf_host(const wchar_t *h) {
    size_t n = wcslen(h), k = wcslen(L".huggingface.co");
    return !_wcsicmp(h, L"huggingface.co") || (n > k && !_wcsicmp(h + n - k, L".huggingface.co"));
}
static int get_file(const MFile *f) {
    static HINTERNET net; wchar_t wu[1024], host[256], path[1024], hdr[100]; URL_COMPONENTS uc; HINTERNET con = NULL, rq = NULL;
    char dst[MAX_PATH], part[MAX_PATH], dir[MAX_PATH]; long long have = 0; DWORD code = 0, sz = sizeof code, got; FILE *o = NULL; int r = 0;
    file_path(f, dst, sizeof dst); snprintf(part, sizeof part, "%s.part", dst);
    snprintf(dir, sizeof dir, "%s\\%s", P_SHARED, f->dir); mkdirs(dir);
    { WIN32_FILE_ATTRIBUTE_DATA fa; if (GetFileAttributesExA(part, GetFileExInfoStandard, &fa)) have = ((long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow; }
    if (have > f->size) have = 0;
    if (have == f->size && have > 0) {     /* all here already, never named (a check cut short, a crash): no request, the check (review 3, P2-1) */
        dl_say("%s is all here already: checking it", f->name); S.dl_done += have; goto whole; }
    if (!net) net = WinHttpOpen(L"voxup/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!net) net = WinHttpOpen(L"voxup/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!net) { dl_say("Could not use the internet (WinHTTP): %s", vlog_errno()); return 0; }
    MultiByteToWideChar(CP_UTF8, 0, f->url, -1, wu, 1024);
    {   int hop;    /* redirects followed here, not by WinHTTP: the token is decided again for every host (P2-3) */
        for (hop = 0; ; hop++) {
            DWORD pol = WINHTTP_OPTION_REDIRECT_POLICY_NEVER; int tok;
            memset(&uc, 0, sizeof uc); uc.dwStructSize = sizeof uc;
            uc.lpszHostName = host; uc.dwHostNameLength = 256; uc.lpszUrlPath = path; uc.dwUrlPathLength = 1024;
            { wchar_t extra[512]; uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 512;
              if (!WinHttpCrackUrl(wu, 0, 0, &uc)) { dl_say("Bad link for %s: %ls", f->name, wu); return 0; }
              wcsncat(path, extra, 1023 - wcslen(path)); }
            con = WinHttpConnect(net, host, uc.nPort, 0);
            if (con) rq = WinHttpOpenRequest(con, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
            if (!rq) { dl_say("Could not reach %ls for %s: %s", host, f->name, vlog_errno()); goto done; }
            WinHttpSetTimeouts(rq, 30000, 30000, 60000, 120000);
            WinHttpSetOption(rq, WINHTTP_OPTION_REDIRECT_POLICY, &pol, sizeof pol);
            tok = hf_host(host);
            {   char tk[256]; int len = 0; wchar_t all[400]; all[0] = 0; code = 0;
                if (have) { swprintf(hdr, 100, L"Range: bytes=%lld-\r\n", have); wcscat(all, hdr); }
                if (tok && hf_token(tk, sizeof tk)) { wchar_t a[300]; swprintf(a, 300, L"Authorization: Bearer %hs\r\n", tk); wcscat(all, a); SecureZeroMemory(a, sizeof a); }
                SecureZeroMemory(tk, sizeof tk); len = (int)wcslen(all);
                if (!WinHttpSendRequest(rq, len ? all : WINHTTP_NO_ADDITIONAL_HEADERS, len ? (DWORD)-1L : 0, NULL, 0, 0, 0)) code = 1;
                SecureZeroMemory(all, sizeof all);
            }
            if (code == 1 || !WinHttpReceiveResponse(rq, NULL)) {
                dl_say("No answer for %s (%s): try Get again", f->name, vlog_errno()); goto done; }
            sz = sizeof code;
            WinHttpQueryHeaders(rq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
            if (code != 301 && code != 302 && code != 303 && code != 307 && code != 308) break;
            {   wchar_t loc[1024]; DWORD ls = sizeof loc;
                if (hop >= 8 || !WinHttpQueryHeaders(rq, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, loc, &ls, WINHTTP_NO_HEADER_INDEX)) {
                    dl_say("%s: too many redirects, or one with no address (HTTP %lu)", f->name, code); goto done; }
                if (loc[0] == L'/') { wchar_t b[1024]; swprintf(b, 1024, L"%ls://%ls:%u%ls", uc.nScheme == INTERNET_SCHEME_HTTPS ? L"https" : L"http", host, (unsigned)uc.nPort, loc); wcsncpy(wu, b, 1023); }
                else wcsncpy(wu, loc, 1023);
                wu[1023] = 0;
                vlog(VLOG_INFO, "get", "%s: redirected (HTTP %lu) to %ls", f->name, code, wu);
            }
            WinHttpCloseHandle(rq); rq = NULL; WinHttpCloseHandle(con); con = NULL;
        }
    }
    if (code == 401 || code == 403) { S.need_token = 1;
        dl_say("This model needs a free Hugging Face login: make one at https://huggingface.co/join, accept the model's terms on its page, then paste a read token below (%s, HTTP %lu).", f->name, code); goto done; }
    if (code == 416 && have > 0) {      /* the server has nothing past the part: it is whole only if it is the file's size */
        if (have == f->size) { S.dl_done += have; goto whole; }
        vlog(VLOG_WARN, "get", "%s: the server has %lld bytes or fewer, models.txt says %lld: the part is removed", part, have, f->size);
        if (!DeleteFileA(part)) vlog(VLOG_ERR, "get", "could not remove %s: %s", part, vlog_errno());
        dl_say("%s on the server is not the size models.txt says: the part was removed (HTTP 416).", f->name); goto done; }
    if (code == 200) have = 0;                     /* the whole file came, not the rest: start the part again */
    else if (code != 206) { dl_say("%s: the server said HTTP %lu", f->name, code); goto done; }
    if (!(o = fopen(part, have ? "ab" : "wb"))) { dl_say("Could not write %s: %s", part, vlog_errno()); goto done; }
    S.dl_done += have;
    for (;;) {
        static char b[1 << 20];
        if (S.dl_stop) { r = -1; goto done; }
        if (!WinHttpReadData(rq, b, sizeof b, &got)) { dl_say("%s broke off (%s): Get goes on from there", f->name, vlog_errno()); goto done; }
        if (!got) break;
        if (fwrite(b, 1, got, o) != got) { dl_say("Could not write %s (disk full?): %s", part, vlog_errno()); goto done; }
        have += got; S.dl_done += got;
        { static DWORD last; if (GetTickCount() - last > 500) { last = GetTickCount(); if (S.wnd) PostMessageA(S.wnd, WM_APP, 0, 0); } }
    }
    fclose(o); o = NULL;
whole:
    if (have != f->size) { dl_say("%s came out %lld bytes, not %lld: kept as .part, Get tries again", f->name, have, f->size); goto done; }
    if (f->sha[0]) {     /* the file as its maker put it up: else the part goes (a bad one never gets a real name) */
        char hx[65]; int k;
        dl_say("Checking %s (SHA-256)...", f->name);
        if ((k = sha256_file(part, hx)) < 0) { r = -1; goto done; }
        if (!k) { dl_say("Could not read %s to check it: %s", part, vlog_errno()); goto done; }
        if (_stricmp(hx, f->sha)) {
            vlog(VLOG_ERR, "get", "%s: SHA-256 %s, models.txt says %s: the downloaded part is removed", part, hx, f->sha);
            if (!DeleteFileA(part)) vlog(VLOG_ERR, "get", "could not remove %s: %s", part, vlog_errno());
            dl_say("%s came out damaged (its checksum does not match): removed, Get starts it again.", f->name); goto done;
        }
        vlog(VLOG_INFO, "get", "%s: SHA-256 matches", f->name);
    }
    if (!MoveFileExA(part, dst, MOVEFILE_REPLACE_EXISTING)) { dl_say("Could not name %s: %s", dst, vlog_errno()); goto done; }
    vlog(VLOG_INFO, "get", "got %s (%lld bytes) from %s", dst, f->size, f->url);
    r = 1;
done:
    if (o) fclose(o);
    if (rq) WinHttpCloseHandle(rq);
    if (con) WinHttpCloseHandle(con);
    return r;
}
static DWORD WINAPI dl_worker(LPVOID arg) {
    int m = S.dl_model, i, r = 1; Model *M = &S.m[m]; ULARGE_INTEGER fr;
    (void)arg;
    mkdirs(P_SHARED);       /* else the first Get's check finds no folder and is skipped (review 3, P3-5) */
    if (GetDiskFreeSpaceExA(P_SHARED, &fr, NULL, NULL) && (long long)fr.QuadPart < S.dl_total + 2000000000LL)
        { dl_say("Not enough room for %s: it needs %.1f GB, %.1f GB are free (2 GB kept spare)", M->name, gb(S.dl_total), gb((long long)fr.QuadPart)); r = 0; }
    for (i = 0; r == 1 && i < M->nf; i++) {
        if (file_there(&M->f[i])) continue;
        dl_say("Getting %s: %s (%.1f GB)", M->name, M->f[i].name, gb(M->f[i].size));
        r = get_file(&M->f[i]);
    }
    check_ready();
    if (r == 1) dl_say("%s is ready: set how many pictures it draws, then Draw.", M->name);
    else if (r < 0) dl_say("Stopped getting %s. Get goes on from where it stopped.", M->name);
    InterlockedExchange(&S.dl_busy, 0);
    if (S.wnd) PostMessageA(S.wnd, WM_APP, 0, 0);
    return 0;
}
static void start_get(int m) {
    HANDLE th;
    if (S.dl_busy || m < 0 || m >= S.nm || S.m[m].ready) return;
    if (!lic_ok(m)) { S.lic_ask = m + 1; return; }      /* the licence shown, one click to accept: never a download before */
    S.lic_ask = 0;
    if (!P_SHARED[0]) { dl_say("Set model_files = in voxup.ini first."); return; }
    S.dl_model = m; S.dl_total = model_missing(m); S.dl_done = 0; S.dl_stop = 0;
    InterlockedExchange(&S.dl_busy, 1);
    th = CreateThread(NULL, 0, dl_worker, NULL, 0, NULL);
    if (th) CloseHandle(th); else { InterlockedExchange(&S.dl_busy, 0); dl_say("Could not start getting it: %s", vlog_errno()); }
}

static void choose_palette(void);
static const char *TEST_PICK;      /* the self-test's answer to a folder or file picker (--pick-folder, --pick-file) */
/* ---------------------------------------------------------------- the window */
/* every text colour at least 4.5 : 1 against the ground it is drawn on (WCAG 2; --contrast in the self-test lists them).
   TEAL2 is for lines and frames only; TEALT is teal for text. No red anywhere. (B1, 2026-11-22) */
static const struct nk_color TEAL = { 0, 128, 128, 255 }, TEAL2 = { 0, 160, 160, 255 }, TEALH = { 0, 105, 105, 255 }, TEALT = { 60, 200, 200, 255 },
                             GOLD = { 230, 200, 110, 255 }, DIM = { 165, 165, 175, 255 };
static const struct nk_color MODEL_COL[6] = { { 120, 210, 200, 255 }, { 150, 190, 230, 255 }, { 230, 200, 110, 255 }, { 150, 215, 140, 255 }, { 190, 170, 235, 255 }, { 220, 220, 225, 255 } };
static double lum(struct nk_color c) {
    double v[3]; int i; v[0] = c.r / 255.0; v[1] = c.g / 255.0; v[2] = c.b / 255.0;
    for (i = 0; i < 3; i++) v[i] = v[i] <= 0.03928 ? v[i] / 12.92 : pow((v[i] + 0.055) / 1.055, 2.4);
    return 0.2126 * v[0] + 0.7152 * v[1] + 0.0722 * v[2];
}
static double contrast(struct nk_color a, struct nk_color b) { double x = lum(a), y = lum(b); return x > y ? (x + 0.05) / (y + 0.05) : (y + 0.05) / (x + 0.05); }
/* the self-test's --contrast: each text colour against each ground it is used on; the number below 4.5 : 1 */
static int contrast_report(struct nk_context *ctx) {
    struct { const char *what; struct nk_color fg, bg; } P[32]; int n = 0, i, low = 0;
    struct nk_color win = ctx->style.window.fixed_background.data.color, btn = ctx->style.button.normal.data.color, cell = { 40, 42, 50, 255 };
#define PAIR(w, f, b) do { P[n].what = w; P[n].fg = f; P[n].bg = b; n++; } while (0)
    PAIR("text / window", ctx->style.text.color, win); PAIR("button text / button", ctx->style.button.text_normal, btn);
    PAIR("GOLD / window", GOLD, win); PAIR("DIM / window", DIM, win); PAIR("DIM / button", DIM, btn); PAIR("DIM / cell", DIM, cell);
    PAIR("TEALT / window", TEALT, win); PAIR("TEALT / cell", TEALT, cell); PAIR("GOLD / cell", GOLD, cell);
    PAIR("white / main button", nk_rgb(255, 255, 255), TEAL); PAIR("white / main button hovered", nk_rgb(255, 255, 255), TEALH);
    PAIR("light grey / window", nk_rgb(215, 215, 220), win);
    for (i = 0; i < 6; i++) PAIR("a model's colour / window", MODEL_COL[i], win);
#undef PAIR
    for (i = 0; i < n; i++) { double c = contrast(P[i].fg, P[i].bg); low += c < 4.5;
        vlog(c < 4.5 ? VLOG_WARN : VLOG_INFO, "contrast", "%-28s %d,%d,%d on %d,%d,%d: %.1f : 1%s", P[i].what, P[i].fg.r, P[i].fg.g, P[i].fg.b, P[i].bg.r, P[i].bg.g, P[i].bg.b, c, c < 4.5 ? "  BELOW 4.5" : ""); }
    vlog(VLOG_INFO, "contrast", "%d pairs, %d below 4.5 : 1", n, low);
    return low;
}
static int button(struct nk_context *ctx, const char *label, int main, int enabled) {
    int r;
    if (!enabled) nk_widget_disable_begin(ctx);
    if (main && enabled) {
        nk_style_push_style_item(ctx, &ctx->style.button.normal, nk_style_item_color(TEAL));
        nk_style_push_style_item(ctx, &ctx->style.button.hover, nk_style_item_color(TEALH));
        nk_style_push_color(ctx, &ctx->style.button.text_normal, nk_rgb(255, 255, 255));
    }
    r = nk_button_label(ctx, label);
    if (main && enabled) { nk_style_pop_style_item(ctx); nk_style_pop_style_item(ctx); nk_style_pop_color(ctx); }
    if (!enabled) nk_widget_disable_end(ctx);
    return r && enabled;
}
static void preview(int r, int i) {
    char p[MAX_PATH]; int w, h, n, x, y; unsigned char *d, *sm;
    if (S.pv_for == r * 100 + i && S.pv_gen == S.r[r].s[i].gen) return;
    if (S.pv_draw) DeleteObject(S.pv_draw); if (S.pv_small) DeleteObject(S.pv_small);
    S.pv_draw = S.pv_small = NULL; S.pv_for = r * 100 + i; S.pv_gen = S.r[r].s[i].gen;
    { Shot *s = &S.r[r].s[i]; EnterCriticalSection(&S.cs);
      if (s->res) { int z = 192 / (s->rw > s->rh ? s->rw : s->rh); S.pv_small = mkbmp(s->res, s->rw, s->rh, z < 1 ? 1 : z, 0); S.pv_w = s->rw * (z < 1 ? 1 : z); S.pv_h = s->rh * (z < 1 ? 1 : z); }
      LeaveCriticalSection(&S.cs); }
    shotpath(S.item, &S.r[r].s[i], 0, p, sizeof p);
    if ((d = stbi_load(p, &w, &h, &n, 3))) {          /* the drawing, 4 x 4 averaged to 192 x 192 (sized to fit) */
        int o = 192, c; sm = xmalloc((size_t)o * o * 3);
        for (y = 0; y < o; y++) for (x = 0; x < o; x++) for (c = 0; c < 3; c++) {
            int x0 = x * w / o, x1 = (x + 1) * w / o, y0 = y * h / o, y1 = (y + 1) * h / o, u, v; long s = 0, k = 0;
            for (v = y0; v < y1; v++) for (u = x0; u < x1; u++) { s += d[((size_t)v * w + u) * 3 + c]; k++; }
            sm[((size_t)y * o + x) * 3 + c] = (unsigned char)(k ? s / k : 0);
        }
        S.pv_draw = mkbmp(sm, o, o, 1, 1); free(sm); stbi_image_free(d);
    }
}
/* ---------------------------------------------------------------- the shrink settings (shrink.h), in pipeline order */
static void s_head(struct nk_context *ctx, const char *t) { nk_layout_row_dynamic(ctx, 22, 1); nk_label_colored(ctx, t, NK_TEXT_LEFT, GOLD); }
/* a Settings section: its title folds it open or shut (kept in pick.ini folds=); the basic ones start open (B3) */
#define FOLDS_DEFAULT (~((1u << 0) | (1u << 4) | (1u << 7)))     /* Size, Colours, Outline open */
static int s_section(struct nk_context *ctx, const char *t, int i) {
    char l[80]; int open = !(S.folds & (1u << i)); struct nk_rect b;
    snprintf(l, sizeof l, "%s %s", open ? "-" : "+", t);
    nk_layout_row_dynamic(ctx, 22, 1); b = nk_widget_bounds(ctx);
    nk_label_colored(ctx, l, NK_TEXT_LEFT, GOLD);
    if (nk_input_is_mouse_click_down_in_rect(&ctx->input, NK_BUTTON_LEFT, b, nk_true)) S.folds ^= 1u << i;
    if (nk_input_is_mouse_hovering_rect(&ctx->input, b)) nk_ext_tooltip(ctx, open ? "Click: fold this away" : "Click: show these settings");
    return open;
}
static void s_combo(struct nk_context *ctx, const char *label, const char *const *items, int n, int *v) {
    static const int TW[] = { 108, 0 }; nk_ext_row(ctx, 24, 2, TW); nk_label(ctx, label, NK_TEXT_LEFT);
    *v = nk_combo(ctx, items, n, *v < 0 || *v >= n ? 0 : *v, 22, nk_vec2(220, 22.0f * n + 16));
}
static void s_int(struct nk_context *ctx, const char *label, int *v, int lo, int hi, int step) {
    char nm[64]; snprintf(nm, sizeof nm, "#%s:", label);      /* the name shown once, inside the box */
    nk_layout_row_dynamic(ctx, 24, 1); nk_property_int(ctx, nm, lo, v, hi, step, (float)step);
}
static void s_check(struct nk_context *ctx, const char *label, int *v) {     /* Off / On, plainly */
    static const char *const OO[] = { "Off", "On" }; int b = *v != 0;
    static const int TW[] = { 0, 66 }; struct nk_rect lb; nk_ext_row(ctx, 24, 2, TW); lb = nk_widget_bounds(ctx); nk_label(ctx, label, NK_TEXT_LEFT);
    if (nk_input_is_mouse_hovering_rect(&ctx->input, lb)) nk_ext_tooltip(ctx, label);      /* cut short in a narrow panel: in full here */
    b = nk_combo(ctx, OO, 2, b, 22, nk_vec2(80, 64)); *v = b;
}
static void s_note(struct nk_context *ctx, const char *t) { nk_ext_wrap_label(ctx, t, DIM, S.inr); }
static void settings_panel(struct nk_context *ctx) {
    static const char *const SIZE[] = { "16 x 16", "32 x 32", "Any size (type it)" }, *const FIT[] = { "The original's box", "The whole sprite", "Stretched to fill" };
    static const char *const SMOOTH[] = { "None", "Median 3 x 3", "Bilateral (keeps edges)" };
    static const char *const DIST[] = { "RGB", "Lab (as seen)", "Weighted RGB" }, *const DITH[] = { "None", "Floyd-Steinberg", "Atkinson", "Ordered (Bayer 4x4)" };
    static const char *const LINE[] = { "Take it off", "Leave it" }, *const OUTL[] = { "None", "Black", "Black, corners too", "Selective (darker shade)", "Auto (as the original)" };
    static const char *const EDGE[] = { "Rough (as shrunk)", "Smooth (whole pixels)" };
    ShrinkOpt *o = &S.opt; int line = o->peel > 0 ? 0 : 1;
    char d[600];
    {   /* this item's own settings; the settings saved as a preset (a button over the pictures) */
        int own = S.own_opt; nk_flags ef;
        s_check(ctx, "These settings for this item only", &own);
        if (own != S.own_opt) item_opt_toggle();
        { static const int TW[] = { 0, 116 }; nk_ext_row(ctx, 24, 2, TW); }
        ef = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD | NK_EDIT_SIG_ENTER, S.newpreset, sizeof S.newpreset, nk_filter_default);
        if (ef & NK_EDIT_ACTIVE) S.find_typing = 1;
        if (button(ctx, "Save as preset", 0, S.newpreset[0] != 0) || ((ef & NK_EDIT_COMMITED) && S.newpreset[0])) { save_preset(S.newpreset); S.newpreset[0] = 0; nk_edit_unfocus(ctx); }
    }
    if (s_section(ctx, "Size", 0)) {
    s_combo(ctx, "Sprite size", SIZE, 3, &o->size);
    if (o->size == SZ_CUSTOM) { s_int(ctx, "Width", &o->cw, 1, 128, 1); s_int(ctx, "Height", &o->ch, 1, 128, 1);
        if (o->cw > 32 || o->ch > 32) s_note(ctx, "Over 32 x 32 the game cannot take it: Keep saves it in out\\pick\\export."); }
    s_combo(ctx, "Fit", FIT, 3, &o->fit);
    s_int(ctx, "Margin", &o->margin, 0, 8, 1);
    }
    if (s_section(ctx, "Cutting out the background", 1)) {
    s_int(ctx, "Colour distance", &o->bg_dist, 10, 250, 5);
    s_check(ctx, "Plain magenta is background", &o->magenta);
    s_int(ctx, "Cover kept (of 255)", &o->alpha_cut, 1, 254, 4);
    s_check(ctx, "Flood from the border", &o->flood);
    if (o->flood) s_note(ctx, "Pink fringe and the model's shadow go too; holes inside stay.");
    }
    if (s_section(ctx, "Before shrinking", 2)) {
    s_int(ctx, "Contrast", &o->contrast, -100, 100, 5);
    s_int(ctx, "Brightness", &o->brightness, -100, 100, 5);
    s_int(ctx, "Saturation", &o->saturation, -100, 100, 5);
    s_combo(ctx, "Smoothing", SMOOTH, 3, &o->smooth);
    s_int(ctx, "Sharpen %", &o->sharpen, 0, 300, 10);
    }
    if (s_section(ctx, "Shrinking", 3)) {
    if (o->method == M_GRID) { s_head(ctx, "Method: Grid snap (under More methods)"); }
    else s_combo(ctx, "Method", METHOD_NAMES, M_COUNT - 1, &o->method);      /* Grid snap: under More methods */
    if (o->method == M_DPID) { s_int(ctx, "Detail (x100)", &o->dpid_lambda, 0, 300, 10); s_note(ctx, "DPID, Weber et al. 2016: pixels that stand out count more."); }
    if (o->method == M_SAID) { s_int(ctx, "Sharpen (x100)", &o->said_gamma, 0, 400, 10); s_int(ctx, "Edge texture", &o->said_alpha, 0, 200, 5); s_int(ctx, "Texture", &o->said_beta, 0, 200, 5);
        s_note(ctx, "SAID, Arjun et al. 2025: sharper along edges, texture kept."); }
    if (o->method == M_SUPERPIXEL) { s_int(ctx, "Compactness", &o->sp_m, 1, 100, 1); s_int(ctx, "Passes", &o->sp_iter, 1, 30, 1); s_note(ctx, "Superpixels, Gerstner et al. 2012 (pixelated image abstraction)."); }
    if (o->method == M_MAJORITY) s_note(ctx, "Each pixel the commonest colour of its area.");
    if (o->method == M_TWOSTEP) s_note(ctx, "Lanczos to 4 times the size, then the commonest colour twice: 4x to 2x to 1x.");
    if (o->method == M_GRID) {      /* the last grid found, and how clearly */
        double gx, gy; int st; char t[200]; shrink_grid_info(&gx, &gy, &st);
        if (st) snprintf(t, sizeof t, "Finds the model's own pixel grid (last: cells %.1f x %.1f px, fit %s), each cell its commonest colour, then into the sprite.", gx, gy, st == 3 ? "clear" : st == 2 ? "good" : "weak");
        else snprintf(t, sizeof t, "Finds the model's own pixel grid, each cell its commonest colour, then into the sprite. Last drawing: no clear grid, so Majority was used.");
        s_note(ctx, t);
    }
    s_check(ctx, "In linear light", &o->linear);
    s_check(ctx, "Shape decided on its own", &o->silhouette);
    if (o->silhouette) s_note(ctx, "Half-covered pixels kept, and thin lines (covered more than both neighbours).");
    s_int(ctx, "Fill pinholes", &o->pinholes, 0, 3, 1);
    }
    if (s_section(ctx, "Colours", 4)) {
    if (o->colours >= C_KMEANS && o->colours != C_PALETTE) s_note(ctx, "Colours: found from the drawing (under More methods).");
    else {      /* the colour finders: under More methods; A palette: here (B8) */
        static const char *const CB[] = { "The original's", "Original's + shades", "The model's own", "Model's own, as drawn", "A palette (a file or a retro one)" };
        int ci = o->colours == C_PALETTE ? 4 : o->colours, c0 = ci;
        s_combo(ctx, "Colours", CB, 5, &ci);
        if (ci != c0) o->colours = ci == 4 ? C_PALETTE : ci;
    }
    if (o->colours == C_PALETTE) {
        s_combo(ctx, "Palette", PAL_NAMES, PAL_COUNT, &o->palid);
        if (o->palid == PAL_FILE) {
            static const int TW[] = { 0, 90 }; char lab[MAX_PATH + 20]; const char *b = strrchr(P_PALETTE, '\\');
            nk_ext_row(ctx, 24, 2, TW);
            snprintf(lab, sizeof lab, "%s", P_PALETTE[0] ? (b ? b + 1 : P_PALETTE) : "(no file yet)"); nk_label_colored(ctx, lab, NK_TEXT_LEFT, P_PALETTE[0] ? TEALT : DIM);
            if (button(ctx, "Choose...", !P_PALETTE[0], 1)) choose_palette();
            s_note(ctx, ".hex (Lospec), .gpl (GIMP), .txt (Paint.NET) or a .png of the colours.");
        } else if (o->palid == PAL_SET) s_note(ctx, "Every colour of the originals the Find box lets through: one palette for a set of items.");
    }
    if (o->colours != C_OWN && o->colours != C_ASDRAWN) {
        s_combo(ctx, "Nearest by", DIST, 3, &o->dist);
        s_combo(ctx, "Dithering", DITH, 4, &o->dither);
        if (o->dither) s_int(ctx, "Dither %", &o->dither_amt, 0, 100, 5);
        else s_check(ctx, "One colour per patch (vote)", &o->regions);
    }
    if (o->colours == C_OWN) s_int(ctx, "Pink snapped at", &o->pink, 0, 100, 5);
    s_int(ctx, "Black below", &o->black, 0, 128, 4);
    }
    if (s_section(ctx, "The model's black line", 5)) {
    s_combo(ctx, "Its line", LINE, 2, &line);
    if (line == 1) o->peel = 0; else { if (!o->peel) o->peel = 4; s_int(ctx, "Layers taken off", &o->peel, 1, 4, 1); }
    }
    if (s_section(ctx, "Clean-up", 6)) {
    s_check(ctx, "Lone pixels take their neighbours' colour", &o->orphans);
    s_check(ctx, "Doubled corners in lines off", &o->jaggies);
    }
    if (s_section(ctx, "Outline", 7)) {
    s_combo(ctx, "Outline", OUTL, 5, &o->outline);
    s_check(ctx, "Black bites into it filled", &o->notch);
    s_check(ctx, "2 x 2 black blocks broken", &o->blocks);
    if (o->outline != O_NONE) s_int(ctx, "Thickness", &o->othick, 1, 4, 1);
    else s_combo(ctx, "Edge", EDGE, 2, &o->edge);
    }
    if (s_section(ctx, "More methods (tried; not better by default)", 8)) {
        static const char *const FIND[] = { "No: the Colours above", "k-means", "Median cut", "Wu (1991)" };
        static int before_grid = -1; int g = o->method == M_GRID, f = o->colours == C_KMEANS ? 1 : o->colours == C_MEDIANCUT ? 2 : o->colours == C_WU ? 3 : 0, f0 = f;
        s_check(ctx, "Grid snap: the model's own pixel grid", &g);
        if (g != (o->method == M_GRID)) { if (g) { before_grid = o->method; o->method = M_GRID; } else o->method = before_grid >= 0 && before_grid != M_GRID ? before_grid : M_SUPERPIXEL; }
        s_note(ctx, "Helps models that draw fine fake pixels (pixel-art add-ons); on today's drawings it leaves a few more lone pixels.");
        s_combo(ctx, "Colours found", FIND, 4, &f);
        if (f != f0) o->colours = f == 1 ? C_KMEANS : f == 2 ? C_MEDIANCUT : f == 3 ? C_WU : C_ORIGINAL;
        if (f) { s_int(ctx, "How many", &o->ncol, 2, 32, 1); if (f == 1) s_check(ctx, "k-means++ seeds", &o->kmpp); }
        s_note(ctx, "Colours found from the drawing instead of the original's: helps when the original has too few colours for it. Wu and k-means++ keep the drawing's colours a little closer.");
        s_check(ctx, "Specks on the edge go (6 of 8 around empty)", &o->edgestray);
        s_note(ctx, "Helps drawings with loose specks at their edge; it can trim thin tips too.");
    }
    nk_layout_row_dynamic(ctx, 8, 1); nk_spacing(ctx, 1);
    nk_layout_row_dynamic(ctx, 28, 1);
    if (button(ctx, "Back to the standard settings", 0, 1)) shrink_defaults(o);
    shrink_describe(o, d, sizeof d);
    nk_ext_wrap_label(ctx, d, DIM, S.inr);
}
/* what the models are given (a click on a before picture): the description so far saved for the one before, the new
   one's loaded */
static void set_from(int nf) {
    char d[4096];
    if (nf == FROM_UP && !S.has_up) return;
    unwrap(S.descbuf, d, sizeof d);
    if (d[0]) { char p[MAX_PATH]; itemdir(S.item, p, sizeof p); mkdirs(p); desc_path(S.item, S.from, p, sizeof p); write_all(p, d, strlen(d)); }
    S.from = nf; save_from(S.item); load_desc_text(S.item);
    if (nf == FROM_UP) status("%03d: the models get the upright picture; describe it standing upright.", S.item);
    else status("%03d: the models get %s picture.", S.item, nf == FROM_TOOL ? "our tool's" : "the original");
}
/* the Compare tab: the original 16 and the upright 32 beside the chosen drawing through each preset; a click on one
   takes that preset for the whole round */
static void compare_panel(struct nk_context *ctx, Shot *s) {
    int i, now = preset_now();
    nk_layout_row_static(ctx, 18, 96, 2); nk_label_colored(ctx, "Original 16", NK_TEXT_LEFT, DIM); nk_label_colored(ctx, "Upright 32", NK_TEXT_LEFT, DIM);
    nk_layout_row_static(ctx, 96, 96, 2);
    if (S.bm_orig) nk_image(ctx, nkimg(S.bm_orig, 96, 96)); else nk_spacing(ctx, 1);
    if (S.bm_up) nk_image(ctx, nkimg(S.bm_up, 96, 96)); else nk_label_colored(ctx, "(none)", NK_TEXT_LEFT, DIM);
    if (!s || s->st != ST_DONE) { nk_ext_wrap_label(ctx, "Click a picture (or an arrow key): its drawing is shown here through every preset.", DIM, S.inr); return; }
    compare_make(s);
    { char t[96]; snprintf(t, sizeof t, "%s seed %u, by preset (click one to use it):", S.m[s->model].name, s->seed); nk_ext_wrap_label(ctx, t, GOLD, S.inr); }
    {   int cper = (S.inr + 4) / 156; if (cper < 1) cper = 1;     /* as many as fit beside each other */
    for (i = 0; i < S.np; i += cper) {
        int j; struct nk_command_buffer *cv; const struct nk_user_font *f = ctx->style.font;
        nk_layout_row_static(ctx, 96 + 34, 152, cper);
        for (j = i; j < i + cper; j++) {
            struct nk_rect b; enum nk_widget_layout_states ws;
            if (j >= S.np) break;          /* not nk_spacing: filling the row with it starts an empty one */
            ws = nk_widget(&b, ctx); if (ws == NK_WIDGET_INVALID) continue;
            cv = nk_window_get_canvas(ctx);
            nk_fill_rect(cv, nk_rect(b.x + 38, b.y, 96, 96), 0, nk_rgb(64, 68, 77));
            if (S.cmp_bm[j]) { struct nk_image im = nkimg(S.cmp_bm[j], S.cmp_w[j], S.cmp_h[j]);
                nk_draw_image(cv, nk_rect(b.x + 38 + (96 - S.cmp_w[j]) / 2, b.y + (96 - S.cmp_h[j]) / 2, (float)S.cmp_w[j], (float)S.cmp_h[j]), &im, nk_rgb(255, 255, 255)); }
            if (S.cmp_bm1[j]) { struct nk_image im = nkimg(S.cmp_bm1[j], S.cmp_w1[j], S.cmp_h1[j]);     /* 1x, top left */
                nk_fill_rect(cv, nk_rect(b.x + 1, b.y, (float)S.cmp_w1[j] + 2, (float)S.cmp_h1[j] + 2), 0, nk_rgb(64, 68, 77));
                nk_draw_image(cv, nk_rect(b.x + 2, b.y + 1, (float)S.cmp_w1[j], (float)S.cmp_h1[j]), &im, nk_rgb(255, 255, 255)); }
            nk_draw_text(cv, nk_rect(b.x, b.y + 97, 156, 16), S.pname[j], (int)strlen(S.pname[j]), f, nk_rgb(45, 45, 45), j == now ? TEALT : GOLD);
            nk_draw_text(cv, nk_rect(b.x, b.y + 113, 156, 16), S.cmp_score[j], (int)strlen(S.cmp_score[j]), f, nk_rgb(45, 45, 45), DIM);
            if (j == now) nk_stroke_rect(cv, nk_rect(b.x + 36, b.y - 2, 100, 100), 0, 2, TEAL2);
            if (nk_input_is_mouse_hovering_rect(&ctx->input, b)) nk_ext_tooltip(ctx, S.cmp_score[j]);
            if (ws == NK_WIDGET_VALID && nk_input_is_mouse_click_down_in_rect(&ctx->input, NK_BUTTON_LEFT, b, nk_true)) { preset_apply_to(&S.opt, j); status("Preset: %s", S.pname[j]); }
        }
    }
    }
    nk_ext_wrap_label(ctx, "fit: how much the sprite and the original's (or the upright picture's) shape cover each other. col (only when the colours are not the original's): how far they are from the original's. lone: the share of single pixels of a colour.", DIM, S.inr);
}
/* the lines between the panels (2026-11-22): drag one to make the panels beside it wider or narrower, double-click it
   for its standard place; the widths are kept in pick.ini (split=) and the grid takes what is left */
static void settings(int save);
enum { STRIP_W = 30, SPLIT_W = 12, LEFT_MIN = 290, RIGHT_MIN = 290, GRID_MIN = 160, LEFT_DEF = 330, RIGHT_DEF = 330, WIN_MIN_W = 900, WIN_MIN_H = 600 };
static void split_fit(int avail, int *gw) {     /* the wanted widths fitted into the window: the sides give way, never below their least */
    int l = S.want[0] < LEFT_MIN ? LEFT_MIN : S.want[0], r = S.want[1] < RIGHT_MIN ? RIGHT_MIN : S.want[1], room = avail - GRID_MIN;
    if (S.fold[0]) l = STRIP_W;
    if (S.fold[1]) r = STRIP_W;
    if (l + r > room && !S.fold[0] && !S.fold[1]) {
        int over = l + r - room, el = l - LEFT_MIN, er = r - RIGHT_MIN, dl;
        if (el + er > 0) { dl = el + er > over ? over * el / (el + er) : el; l -= dl; r -= el + er > over ? over - dl : er; }
    }
    else if (l + r > room) { if (!S.fold[0]) l = room - r < LEFT_MIN ? LEFT_MIN : room - r; if (!S.fold[1]) r = room - l < RIGHT_MIN ? RIGHT_MIN : room - l; }
    S.split[0] = l; S.split[1] = r; *gw = avail - l - r; if (*gw < 40) *gw = 40;
}
static void fold_set(int side, int f) {
    if (S.fold[side] == f) return;
    S.fold[side] = f; S.drag = 0;
    status(f ? "%s panel folded (%s or a click on its strip opens it)." : "%s panel open again.", side ? "The right" : "The left", side ? "Ctrl+]" : "Ctrl+[");
    settings(1);
}
/* a folded side panel: a strip with its name down it; a click anywhere on it opens the panel (B4) */
static void fold_strip(struct nk_context *ctx, int side) {
    const char *t = side ? "CHOSEN" : "ITEM"; struct nk_rect b = nk_widget_bounds(ctx);
    if (nk_group_begin(ctx, side ? "strip-r" : "strip-l", NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR)) {
        int i;
        nk_layout_row_dynamic(ctx, 24, 1); nk_label_colored(ctx, side ? "<" : ">", NK_TEXT_CENTERED, TEALT);
        for (i = 0; t[i]; i++) { char c[2] = { t[i], 0 }; nk_layout_row_dynamic(ctx, 16, 1); nk_label_colored(ctx, c, NK_TEXT_CENTERED, GOLD); }
        nk_group_end(ctx);
    }
    if (nk_input_is_mouse_hovering_rect(&ctx->input, b)) nk_ext_tooltip(ctx, side ? "Click: open the right panel again (Ctrl+])" : "Click: open the left panel again (Ctrl+[)");
    if (nk_input_is_mouse_click_down_in_rect(&ctx->input, NK_BUTTON_LEFT, b, nk_true)) fold_set(side, 0);
}
static void split_bar(struct nk_context *ctx, int which) {
    int r = nk_ext_splitter(ctx, S.drag == which, TEAL2, nk_rgb(70, 74, 84), S.drag ? NULL : S.fold[which - 1] ? "Click: open the folded panel" : "Drag: wider or narrower. Double-click: the standard width. Ctrl+[ / Ctrl+]: fold the left / right panel.");
    if ((r & NK_EXT_SPLIT_HOVER) || S.drag == which) S.cur_we = 1;
    if (r & NK_EXT_SPLIT_DOUBLE) { S.want[which - 1] = which == 1 ? LEFT_DEF : RIGHT_DEF; S.drag = 0; status("Panel put back to its standard width."); }
    else if ((r & NK_EXT_SPLIT_PRESS) && S.fold[which - 1]) fold_set(which - 1, 0);
    else if ((r & NK_EXT_SPLIT_PRESS) && !S.drag) { S.drag = which; S.drag_x = ctx->input.mouse.pos.x; S.drag_w = S.split[which - 1]; S.want[which - 1] = S.drag_w; }
}
static int gui_frame(struct nk_context *ctx, int W, int H) {
    char lab[700]; int i, busy = S.busy != 0, gw;
    nk_ext_frame(GetTickCount());
    if (!nk_begin(ctx, "voxup", nk_rect(0, 0, (float)W, (float)H), NK_WINDOW_NO_SCROLLBAR)) { nk_end(ctx); return 0; }
    if (S.unfocus) { nk_edit_unfocus(ctx); S.unfocus = 0; }
    {   /* the panels' widths: a divider being dragged moves its panel's edge with the mouse */
        int avail = W - (int)(2 * ctx->style.window.padding.x + 4 * ctx->style.window.spacing.x) - 2 * SPLIT_W - 4;
        S.cur_we = 0;
        if (S.drag && !ctx->input.mouse.buttons[NK_BUTTON_LEFT].down) { S.drag = 0; settings(1); }
        if (S.drag) {
            int dx = (int)(ctx->input.mouse.pos.x - S.drag_x), v = S.drag == 1 ? S.drag_w + dx : S.drag_w - dx;
            int most = avail - GRID_MIN - (S.drag == 1 ? S.split[1] : S.split[0]), least = S.drag == 1 ? LEFT_MIN : RIGHT_MIN;
            S.want[S.drag - 1] = v > most ? most : v < least ? least : v;
        }
        split_fit(avail, &gw);
        {   /* the grid's columns: as many 128-pixel pictures as fit beside each other */
            int inner = gw - (int)(2 * ctx->style.window.group_padding.x + ctx->style.window.scrollbar_size.x) - 8;
            S.per = (inner + 4) / 132; if (S.per < 1) S.per = 1; if (S.per > 10) S.per = 10;
            S.ing = inner; inner = (int)(2 * ctx->style.window.group_padding.x + ctx->style.window.scrollbar_size.x);
            S.inl = S.split[0] - inner; S.inr = S.split[1] - inner;
        }
    }
    nk_layout_row_begin(ctx, NK_STATIC, (float)(H - 16), 5);
    /* left: the item */
    nk_layout_row_push(ctx, (float)S.split[0]);
    if (S.fold[0]) fold_strip(ctx, 0);
    else if (nk_group_begin(ctx, "item", NK_WINDOW_BORDER)) {
        {   /* Find: the list, < > and PgUp/PgDn keep to the items that fit; Enter goes to the first */
            struct nk_rect bb; nk_flags ef; char old[48]; snprintf(old, sizeof old, "%s", S.find);
            static const int TW[] = { 22, 36, 0, 64 }; nk_ext_row(ctx, 24, 4, TW);
            bb = nk_widget_bounds(ctx); if (button(ctx, "<", 0, 1)) fold_set(0, 1);
            if (nk_input_is_mouse_hovering_rect(&ctx->input, bb)) nk_ext_tooltip(ctx, "Fold this panel to a strip (Ctrl+[)");
            nk_label_colored(ctx, "Item", NK_TEXT_LEFT, GOLD);
            bb = nk_widget_bounds(ctx);
            if (S.find_focus) { nk_edit_focus(ctx, NK_EDIT_FIELD | NK_EDIT_SIG_ENTER); S.find_focus = 0; }
            ef = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD | NK_EDIT_SIG_ENTER, S.find, sizeof S.find, nk_filter_default);
            S.find_typing = (ef & NK_EDIT_ACTIVE) != 0;
            if (nk_input_is_mouse_hovering_rect(&ctx->input, bb)) nk_ext_tooltip(ctx, "Find (Ctrl+F or /): a number, part of the name, kept, new, tilted or queued; Enter goes to the first.");
            if ((ef & NK_EDIT_COMMITED) && !busy) { first_fit(); nk_edit_unfocus(ctx); }
            if (S.find[0]) { snprintf(lab, sizeof lab, "%d fit", count_fits()); nk_label_colored(ctx, lab, NK_TEXT_LEFT, DIM); }
            else nk_label_colored(ctx, "(find)", NK_TEXT_LEFT, DIM);
        }
        { static const int TW[] = { 30, 0, 30 }; nk_ext_row(ctx, 28, 3, TW); }
        if (button(ctx, "<", 0, !busy)) step_item(-1);
        snprintf(lab, sizeof lab, "%03d %s%s%s", S.item, S.name[S.item], S.kept[S.item] ? "  (kept)" : "", queued(S.item) ? "  (queued)" : "");
        if (!busy && nk_combo_begin_label(ctx, lab, nk_vec2(300, 520))) {
            nk_layout_row_dynamic(ctx, 22, 1);
            for (i = 0; i < S.nlist; i++) {
                int k = S.list[i]; char l[96];
                if (!item_fits(k)) continue;
                snprintf(l, sizeof l, "%03d %s%s%s", k, S.name[k], S.kept[k] ? "  (kept)" : "", queued(k) ? "  (queued)" : "");
                if (nk_combo_item_label(ctx, l, NK_TEXT_LEFT)) choose(i);
            }
            nk_combo_end(ctx);
        } else if (busy) { nk_widget_disable_begin(ctx); nk_button_label(ctx, lab); nk_widget_disable_end(ctx); }
        if (button(ctx, ">", 0, !busy)) step_item(1);
        {   /* the queue: this item on or off (Q), every item found, Run */
            nk_layout_row_dynamic(ctx, 24, 3);
            if (button(ctx, queued(S.item) ? "Off queue" : "Queue it (Q)", 0, 1)) queue_toggle(S.item);
            if (button(ctx, "Queue found", 0, S.find[0] != 0)) queue_add_fits();
            if (S.qrun) { snprintf(lab, sizeof lab, "Stop queue (%d)", S.nq); if (button(ctx, lab, 0, 1)) S.qrun = 0; }
            else { snprintf(lab, sizeof lab, "Run queue (%d)", S.nq); if (button(ctx, lab, S.nq > 0, S.nq > 0 && !busy)) { S.qrun = 1; status("Running the queue: a round for each of %d item(s).", S.nq); } }
        }
        /* the three before pictures: the original, our tool's enlarged one, the upright one (voxup.ini upright =) */
        /* the three before pictures (the original, our tool's enlarged one, the upright one): a click on one = the
           models draw from it (framed) */
        nk_layout_row_dynamic(ctx, 18, 1); nk_label_colored(ctx, "Draw from: click a picture (framed)", NK_TEXT_LEFT, GOLD);
        {   static const int FR[3] = { FROM_ORIG, FROM_TOOL, FROM_UP }; static const char *const FL[3] = { "Original 16", "Enlarged 32", "Upright 32" };
            HBITMAP bm[3]; int c, c0, pick = -1, bper = (S.inl + 4) / 104;
            if (bper < 1) bper = 1; if (bper > 3) bper = 3;
            bm[0] = S.bm_orig; bm[1] = S.bm_tool; bm[2] = S.bm_up;
            for (c0 = 0; c0 < 3; c0 += bper) {     /* as many to a row as fit the panel */
            nk_layout_row_static(ctx, 18, 100, bper); for (c = c0; c < c0 + bper && c < 3; c++) nk_label_colored(ctx, FL[c], NK_TEXT_LEFT, DIM);
            nk_layout_row_static(ctx, 100, 100, bper);
            for (c = c0; c < c0 + bper && c < 3; c++) {
                struct nk_rect b; enum nk_widget_layout_states ws = nk_widget(&b, ctx); struct nk_command_buffer *cv;
                if (ws == NK_WIDGET_INVALID) continue;
                cv = nk_window_get_canvas(ctx);
                if (bm[c]) { struct nk_image im = nkimg(bm[c], 96, 96); nk_draw_image(cv, nk_rect(b.x + 2, b.y + 2, 96, 96), &im, nk_rgb(255, 255, 255)); }
                else { const char *t = c == 2 && S.up_note[0] ? S.up_note : "(none)"; nk_draw_text(cv, nk_rect(b.x + 4, b.y + 40, 96, 18), t, (int)strlen(t), ctx->style.font, nk_rgb(45, 45, 45), DIM); }
                if (S.from == FR[c]) nk_stroke_rect(cv, nk_rect(b.x, b.y, 100, 100), 0, 2, TEAL2);
                if (ws == NK_WIDGET_VALID && bm[c] && nk_input_is_mouse_click_down_in_rect(&ctx->input, NK_BUTTON_LEFT, b, nk_true)) pick = FR[c];
                if (nk_input_is_mouse_hovering_rect(&ctx->input, b)) nk_ext_tooltip(ctx, S.from == FR[c] ? "The models draw from this one." : "Click: the models draw from this one.");
            }
            }
            if (pick >= 0 && pick != S.from) { if (busy) status("Drawing: choose what it draws from when the round is done."); else set_from(pick); }
        }
        if (S.up_hand) { nk_layout_row_dynamic(ctx, 20, 1); nk_label_colored(ctx, "Upright: hand-made (your upright folder)", NK_TEXT_LEFT, DIM); }
        else {   /* the upright picture made here: how, and Remake (a touch-up in NNN\upright.png stays until then) */
            static const char *ml[] = { "Auto (if tilted)", "Turn right 45", "Turn left 45", "Off" }, *stl[] = { "Classic", "Centred" },
                              *sll[] = { "Grip as it is", "Slim sword grip", "Slim shaft" };
            int m = S.up_mode, st = S.up_style, sl = S.up_slim, remake = 0; struct nk_rect bb;
            if (busy) nk_widget_disable_begin(ctx);
            static const int TW[] = { 60, 0, 0 }; nk_ext_row(ctx, 24, 3, TW);
            bb = nk_widget_bounds(ctx); nk_label(ctx, "Upright", NK_TEXT_LEFT);
            if (nk_input_is_mouse_hovering_rect(&ctx->input, bb)) nk_ext_tooltip(ctx, "Stands a tilted item upright at 32 x 32 from the original (cleanEdge turn, no new colours). Auto: only when it lies tilted.");
            m = nk_combo(ctx, ml, 4, m, 22, nk_vec2(150, 120));
            st = nk_combo(ctx, stl, 2, st, 22, nk_vec2(110, 70));
            nk_ext_row(ctx, 24, 3, TW);
            nk_label(ctx, "", NK_TEXT_LEFT);
            sl = nk_combo(ctx, sll, 3, sl, 22, nk_vec2(150, 100));
            bb = nk_widget_bounds(ctx); remake = nk_button_label(ctx, "Remake");
            if (nk_input_is_mouse_hovering_rect(&ctx->input, bb)) nk_ext_tooltip(ctx, "Makes the upright picture again from the original (a touch-up made by hand in its upright.png goes).");
            if (busy) nk_widget_disable_end(ctx);
            if (!busy && (m != S.up_mode || st != S.up_style || sl != S.up_slim || remake)) {
                S.up_mode = m; S.up_style = st; S.up_slim = sl; up_settings(S.item, 1);
                stop_reshape();         /* it may be shrinking into the box load_upright rewrites (review 2, P2-1) */
                load_upright(S.item, 1);
                if (S.bm_up) DeleteObject(S.bm_up); if (S.pv_up) DeleteObject(S.pv_up); S.bm_up = S.pv_up = NULL;
                if (S.has_up) { S.bm_up = mkbmp(S.up, 32, 32, 3, 0); S.pv_up = mkbmp(S.up, 32, 32, 6, 0); }
                if (S.from == FROM_UP && !S.has_up) { S.from = FROM_TOOL; save_from(S.item); load_desc_text(S.item); }
                S.upitem = -1;                                  /* the models' picture made again */
                InterlockedIncrement(&S.optgen); kick_reshape(); /* upright shots shrink into the new box */
                if (S.has_up) status("%03d: upright picture made (turned %.0f degrees).", S.item, S.up_turn);
                else status("%03d: no upright picture %s.", S.item, S.up_note);
            }
        }
        nk_layout_row_dynamic(ctx, 20, 1);
        if (S.keptinfo[0]) nk_label_colored(ctx, S.keptinfo, NK_TEXT_LEFT, TEALT); else nk_label_colored(ctx, "Not kept yet", NK_TEXT_LEFT, DIM);
        if (S.desc_made) S.desc_word = tilt_word(S.descbuf);      /* as the words are now (review 2, P3-4) */
        if (S.desc_made) nk_ext_wrap_label(ctx, S.desc_word ? "What it is: made from the other, still says it lies tilted" : "What it is: made from the other, check it", GOLD, S.inl);
        else nk_label_colored(ctx, "What it is (goes into the prompt)", NK_TEXT_LEFT, GOLD);
        nk_layout_row_dynamic(ctx, 110, 1);
        nk_edit_string_zero_terminated(ctx, NK_EDIT_BOX, S.descbuf, sizeof S.descbuf, nk_filter_default);
        /* 2026-11-21: the models (models.txt): how many pictures each draws; Only = this one alone; Get = download it */
        nk_layout_row_dynamic(ctx, 20, 1); nk_label_colored(ctx, "Models: how many pictures each", NK_TEXT_LEFT, GOLD);
#if SD_BUILT
        {   /* which engine draws the models that both can draw */
            static const char *el[] = { "Built-in", "ComfyUI" }; struct nk_rect bb;
            static const int TW[] = { 90, 0 }; nk_ext_row(ctx, 24, 2, TW); bb = nk_widget_bounds(ctx); nk_label(ctx, "Engine", NK_TEXT_LEFT);
            if (nk_input_is_mouse_hovering_rect(&ctx->input, bb)) nk_ext_tooltip(ctx, "Built-in: drawn inside voxup, no ComfyUI. ComfyUI: started hidden. A model only one can draw uses that one.");
            if (busy) nk_widget_disable_begin(ctx);
            S.engine = nk_combo(ctx, el, 2, S.engine, 22, nk_vec2(180, 80));
            if (busy) nk_widget_disable_end(ctx);
        }
#endif
        {   int nget = 0; for (i = 0; i < S.nm; i++) nget += !S.m[i].ready && usable(i);
            S.nget = nget; }
        if (S.lic_ask > 0 && S.lic_ask <= S.nm) {    /* Get pressed: the model's licences, one click to accept */
            Model *M = &S.m[S.lic_ask - 1]; int j;
            snprintf(lab, sizeof lab, "Before getting %s (%.1f GB): its licence%s", M->name, gb(model_missing(S.lic_ask - 1)), M->nlic > 1 ? "s" : "");
            nk_ext_wrap_label(ctx, lab, GOLD, S.inl);
            for (j = 0; j < M->nlic; j++) {
                static const int TW[] = { 0, 56 }; struct nk_rect lb; nk_ext_row(ctx, 22, 2, TW);
                lb = nk_widget_bounds(ctx); nk_label_colored(ctx, M->licn[j], NK_TEXT_LEFT, strstr(M->licn[j], "Non-Commercial") ? GOLD : TEALT);
                if (nk_input_is_mouse_hovering_rect(&ctx->input, lb)) nk_ext_tooltip(ctx, M->licu[j]);
                if (button(ctx, "Read", 0, 1)) ShellExecuteA(S.wnd, "open", M->licu[j], NULL, NULL, SW_SHOWNORMAL);
            }
            nk_layout_row_dynamic(ctx, 28, 2);
            if (button(ctx, "Accept and get", 1, 1)) { int mm = S.lic_ask - 1; lic_accept(mm); start_get(mm); }
            if (button(ctx, "Not now", 0, 1)) S.lic_ask = 0;
        }
        {   int nc = 0; for (i = 0; i < S.nm; i++) nc += !usable(i);
            if (nc) { snprintf(lab, sizeof lab, "%d more model%s need ComfyUI (set comfy = in voxup.ini to use them)", nc, nc > 1 ? "s" : ""); nk_ext_wrap_label(ctx, lab, DIM, S.inl); } }
        {   int ready = 0, rec = -1; for (i = 0; i < S.nm; i++) { ready += S.m[i].ready; if (S.m[i].recommended && usable(i) && rec < 0) rec = i; }
            if (!ready && rec >= 0 && !S.dl_busy && !S.lic_ask) {     /* nothing to draw with yet: one click */
                nk_ext_wrap_label(ctx, "Get a model first: one click.", GOLD, S.inl);
                snprintf(lab, sizeof lab, "Get %s (%.1f GB)", S.m[rec].name, gb(model_missing(rec)));
                nk_layout_row_dynamic(ctx, 36, 1); if (button(ctx, lab, 1, 1)) start_get(rec);
            }
        }
        for (i = 0; i < S.nm; i++) {
            Model *M = &S.m[i]; struct nk_rect bb; int dl = S.dl_busy && S.dl_model == i;
            if (!usable(i)) continue;                           /* only ComfyUI draws it, and there is no comfy =: folded away */
            if (!M->ready && !dl && !S.show_get) continue;      /* the ones still to get: folded (the row below) */
            { static const int TW[] = { 0, 80, 80 }; nk_ext_row(ctx, 24, 3, TW); }
            bb = nk_widget_bounds(ctx);
            nk_label_colored(ctx, M->name, NK_TEXT_LEFT, !M->ready ? DIM : M->n ? TEALT : nk_rgb(215, 215, 220));
            if (nk_input_is_mouse_hovering_rect(&ctx->input, bb)) nk_ext_tooltip(ctx, M->note[0] ? M->note : M->name);
            if (M->ready) { if (busy) nk_widget_disable_begin(ctx); nk_property_int(ctx, "#n", 0, &M->n, 20, 1, 1); if (busy) nk_widget_disable_end(ctx); }
            else nk_spacing(ctx, 1);
            if (dl) { snprintf(lab, sizeof lab, "Stop %d%%", S.dl_total ? (int)(S.dl_done * 100 / S.dl_total) : 0); if (button(ctx, lab, 0, !S.dl_stop)) InterlockedExchange(&S.dl_stop, 1); }
            else if (!M->ready) { snprintf(lab, sizeof lab, "Get %.1f GB", gb(model_missing(i))); if (button(ctx, lab, 0, !S.dl_busy)) start_get(i); }
            else if (button(ctx, "Only", 0, !busy)) { int j; for (j = 0; j < S.nm; j++) S.m[j].n = 0; M->n = 20; }
        }
        if (S.nget) {
            nk_layout_row_dynamic(ctx, 24, 1);
            snprintf(lab, sizeof lab, S.show_get ? "Hide the models still to get" : "%d more models to get...", S.nget);
            if (button(ctx, lab, 0, 1)) S.show_get = !S.show_get;
        }
        EnterCriticalSection(&S.cs);
        if (S.dl_busy || S.dl_msg[0]) nk_ext_wrap_label(ctx, S.dl_msg, S.dl_busy ? GOLD : TEALT, S.inl);
        LeaveCriticalSection(&S.cs);
        if (S.need_token) {      /* a model behind a Hugging Face login: the token, shown as stars, kept by Windows */
            static char stars[256]; size_t ol = strlen(S.tok), nl; static const int TW[] = { 0, 70 };
            memset(stars, '*', ol); stars[ol] = 0;
            nk_ext_row(ctx, 24, 2, TW);
            {   static int tok_active; struct nk_window *w = ctx->current; nk_flags ef;
                if (tok_active && w) { int e = (int)strlen(stars); w->edit.cursor = w->edit.sel_start = w->edit.sel_end = e; }
                ef = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, stars, sizeof stars, nk_filter_default);
                tok_active = (ef & NK_EDIT_ACTIVE) != 0; }
            nl = strlen(stars);
            if (nl != ol || strspn(stars, "*") != nl) {    /* rebuilt from the box: a * is the old letter in its turn (review 3, P3-4) */
                char t[256]; size_t j, k = 0, ns = 0, q = 0;
                for (j = 0; j < nl; j++) ns += stars[j] == '*';
                if (ns == ol || (ns < ol && strspn(stars, "*") == nl && nl == ns)) {   /* letters put in anywhere; or cut off at the end */
                    for (j = 0; j < nl && q < sizeof t - 1; j++) t[q++] = stars[j] == '*' ? S.tok[k++] : stars[j];
                    t[q] = 0; snprintf(S.tok, sizeof S.tok, "%s", t);
                } else if (!ns) snprintf(S.tok, sizeof S.tok, "%s", stars);      /* all replaced (a paste over all) */
                else { S.tok[0] = 0; dl_say("Part of the token was changed in the middle: type or paste the whole token again."); }
                SecureZeroMemory(t, sizeof t);
            }
            SecureZeroMemory(stars, sizeof stars);
            if (button(ctx, "Keep it", 1, S.tok[0] != 0)) { if (hf_token_save(S.tok)) { S.need_token = 0; dl_say("Token kept in Windows' Credential Manager: press Get again."); } SecureZeroMemory(S.tok, sizeof S.tok); }
        }
        if (S.dl_busy) { nk_size d = (nk_size)(S.dl_done >> 20), t = (nk_size)(S.dl_total >> 20); nk_layout_row_dynamic(ctx, 14, 1); nk_progress(ctx, &d, t ? t : 1, 0); }
        if (nk_tree_push(ctx, NK_TREE_TAB, "Prompts ({d} = what it is)", NK_MINIMIZED)) {
            if (S.nm) {
                Model *M = &S.m[S.pmodel < S.nm ? S.pmodel : 0];
                nk_layout_row_dynamic(ctx, 24, 1);
                if (nk_combo_begin_label(ctx, M->name, nk_vec2(260, 320))) {
                    nk_layout_row_dynamic(ctx, 22, 1);
                    for (i = 0; i < S.nm; i++) if (nk_combo_item_label(ctx, S.m[i].name, NK_TEXT_LEFT)) S.pmodel = i;
                    nk_combo_end(ctx);
                }
                nk_layout_row_dynamic(ctx, 200, 1); nk_edit_string_zero_terminated(ctx, NK_EDIT_BOX, M->prompt, sizeof M->prompt, nk_filter_default);
                nk_layout_row_dynamic(ctx, 26, 1);
                if (button(ctx, "Back to its standard prompt", 0, !busy)) wrap(std_prompt(M->edit), M->prompt, sizeof M->prompt, 34);
                /* a model that repaints the item's picture: how much it may change; its LoRA's strength (0 = none) */
                if (M->sdimage == 2) { nk_layout_row_dynamic(ctx, 24, 1); nk_property_int(ctx, "#How much may change %:", 5, &M->strength, 100, 5, 1); }
                if (M->has_lora) { nk_layout_row_dynamic(ctx, 24, 1); nk_property_int(ctx, "#Its LoRA %:", 0, &M->lora_w, 200, 5, 1); }
            }
            nk_tree_pop(ctx);
        }
        nk_group_end(ctx);
    }
    nk_layout_row_push(ctx, SPLIT_W); split_bar(ctx, 1);
    /* middle: the round's pictures */
    nk_layout_row_push(ctx, (float)gw);
    if (nk_group_begin(ctx, "grid", NK_WINDOW_BORDER)) {
        Round *R = S.nr ? &S.r[S.cur] : NULL; int r, c, kdone = 0, dbl = -1, rclick = -1;
        /* Draw and Stop over the pictures, the status line under them: always in sight */
        { static const float RT[] = { 0.66f, 0.34f }; nk_layout_row(ctx, NK_DYNAMIC, 32, 2, RT); }
        if (S.ing >= 360) snprintf(lab, sizeof lab, S.nr ? "New %d seeds  (Space)" : "Draw %d  (Space)", round_size());
        else snprintf(lab, sizeof lab, S.nr ? "New %d" : "Draw %d", round_size());
        if (button(ctx, lab, 1, !busy && round_size() > 0 && round_size() <= MAXSHOT)) start_round();
        if (button(ctx, S.stop && busy ? "Stopping..." : S.ing >= 360 ? "Stop  (Esc)" : "Stop", 0, busy && !S.stop)) {
            InterlockedExchange(&S.stop, 1); status("Stopping after this picture..."); S.qrun = 0;
#if SD_BUILT
            if (SDX_DRAWING && SDX) sd_cancel_generation(SDX, SD_CANCEL_ALL);     /* the built-in engine stops at once */
#endif
        }
        EnterCriticalSection(&S.cs); snprintf(lab, sizeof lab, "%.400s%s", S.status, S.sdstep); LeaveCriticalSection(&S.cs); nk_ext_wrap_label(ctx, lab, GOLD, S.ing);
        if (vlog_problems() != S.prob_seen) {
            unsigned hsh = 5381; const char *c; int g, gone = 0; S.prob_seen = vlog_problems();
            for (c = vlog_last(); *c; c++) hsh = hsh * 33 + (unsigned char)*c;
            for (g = 0; g < S.nprob_gone; g++) gone |= S.prob_gone[g] == hsh;
            if (!gone) snprintf(S.problem, sizeof S.problem, "%s", vlog_last());       /* one clicked away is not shown again */
        }
        if (S.problem[0]) {     /* every WARN/ERR reaches the user: the last one, its path and reason, until clicked */
            char pl[460]; struct nk_rect pb;
            snprintf(pl, sizeof pl, "Problem: %.400s  (click to clear; all in voxup.log)", S.problem);
            pb = nk_ext_wrap_label(ctx, pl, GOLD, S.ing);
            if (nk_input_is_mouse_click_down_in_rect(&ctx->input, NK_BUTTON_LEFT, pb, nk_true)) {
                unsigned hsh = 5381; const char *c;
                for (c = S.problem; *c; c++) hsh = hsh * 33 + (unsigned char)*c;
                if (S.nprob_gone < 32) S.prob_gone[S.nprob_gone++] = hsh;
                S.problem[0] = 0; S.prob_gap = pb.h; S.prob_rect = pb;        /* its room stays while the mouse is on it */
            }
            if (S.problem[0] && nk_input_is_mouse_hovering_rect(&ctx->input, pb)) nk_ext_tooltip(ctx, S.problem);
        } else if (S.prob_gap > 0) {    /* cleared: nothing moves up under the mouse until it leaves that place */
            if (nk_input_is_mouse_hovering_rect(&ctx->input, S.prob_rect)) { nk_layout_row_dynamic(ctx, S.prob_gap, 1); nk_spacing(ctx, 1); }
            else S.prob_gap = 0;
        }
        {   /* Back, Next, the round; the colours beside them when the grid is wide, else under them */
            static const int TW1[] = { 80, 80, 0, 150, 150 }, TW2[] = { 80, 80, 0 }; int wide = S.ing >= 720;
            if (S.ing >= 360) nk_ext_row(ctx, 28, wide ? 5 : 3, wide ? TW1 : TW2); else nk_layout_row_dynamic(ctx, 28, 2);
        }
        if (button(ctx, "< Back", 0, S.cur > 0)) { S.cur--; S.sel = -1; kick_reshape(); }
        if (button(ctx, "Next >", 0, S.cur < S.nr - 1)) { S.cur++; S.sel = -1; kick_reshape(); }
        if (R) { int stale = 0; for (i = 0; i < R->n; i++) { kdone += R->s[i].st == ST_DONE; stale += R->s[i].st == ST_DONE && R->s[i].gen != S.optgen; }
                 if (stale) snprintf(lab, sizeof lab, "Round %d of %d: remaking %d...", S.cur + 1, S.nr, stale);
                 else snprintf(lab, sizeof lab, "Round %d of %d: %d drawn", S.cur + 1, S.nr, kdone); }
        else snprintf(lab, sizeof lab, "No pictures yet: press Draw");
        {   struct nk_rect lb; if (S.ing < 360) nk_layout_row_dynamic(ctx, 20, 1);
            lb = nk_widget_bounds(ctx); nk_label_colored(ctx, lab, NK_TEXT_LEFT, GOLD);
            if (nk_input_is_mouse_hovering_rect(&ctx->input, lb)) nk_ext_tooltip(ctx, lab); }
        /* 2026-11-21: the sprites in the original's colours (clamped) or the model's own; Keep takes the one shown */
        if (S.ing < 720) nk_layout_row_dynamic(ctx, 28, S.ing >= 300 ? 2 : 1);
        if (button(ctx, "Original's colours", S.opt.colours == C_ORIGINAL, 1)) S.opt.colours = C_ORIGINAL;
        if (button(ctx, "Model's colours", S.opt.colours == C_OWN, 1)) S.opt.colours = C_OWN;
        if (S.ing < 360 && S.np) {      /* a narrow grid: the presets as one list, so the pictures stay in sight (P2-4) */
            static const int TW[] = { 50, 0 }; int now = preset_now(), p; char cur[48];
            snprintf(cur, sizeof cur, "%s", now >= 0 ? S.pname[now] : "(own settings)");
            nk_ext_row(ctx, 24, 2, TW); nk_label_colored(ctx, "Preset", NK_TEXT_LEFT, DIM);
            if (nk_combo_begin_label(ctx, cur, nk_vec2(220, 26.0f * S.np + 10))) {
                nk_layout_row_dynamic(ctx, 22, 1);
                for (p = 0; p < S.np; p++) if (!S.phid[p] && nk_combo_item_label(ctx, S.pname[p], NK_TEXT_LEFT)) { preset_apply_to(&S.opt, p); status("Preset: %s", S.pname[p]); }
                nk_combo_end(ctx);
            }
        } else
        {   /* the presets: one click each (F1, F2, ...), the one in use lit */
            int now = preset_now(), p, per = (S.ing - 50) / 96;     /* as many to a row as fit */
            if (per < 1) per = 1; if (per > 12) per = 12;
            int shown = 0;
            for (p = 0; p < S.np; p++) {
                struct nk_rect bb; char t[48];
                if (S.phid[p]) continue;
                if (shown++ % per == 0) { int tw[13] = { 50 }; nk_ext_row(ctx, 24, per + 1, tw); nk_label_colored(ctx, shown > 1 ? "" : "Preset", NK_TEXT_LEFT, DIM); }
                bb = nk_widget_bounds(ctx);
                snprintf(t, sizeof t, "%s", S.pname[p]);
                if (button(ctx, t, p == now, 1)) { preset_apply_to(&S.opt, p); status("Preset: %s", S.pname[p]); }
                if (nk_input_is_mouse_hovering_rect(&ctx->input, bb)) { char h[480]; snprintf(h, sizeof h, "F%d. %s", p + 1, S.pset[p][0] ? S.pset[p] : "the standard settings"); nk_ext_tooltip(ctx, h); }
            }
        }
        {   struct nk_rect kb; nk_layout_row_dynamic(ctx, 18, 1); kb = nk_widget_bounds(ctx);
            nk_label_colored(ctx, S.keys_open ? "- Keys (click to fold)" : S.ing >= 420 ? "+ Keys: Space draws, Enter keeps, Esc stops (click for all)" : "+ Keys (click)", NK_TEXT_LEFT, DIM);
            if (nk_input_is_mouse_click_down_in_rect(&ctx->input, NK_BUTTON_LEFT, kb, nk_true)) S.keys_open = !S.keys_open;
            if (S.keys_open) {
                nk_ext_wrap_label(ctx, "Click: look at it   Double-click / Enter: keep   Right-click / Del: draw that one again   Space / Ctrl+Enter: draw   Esc: stop", DIM, S.ing);
                nk_ext_wrap_label(ctx, "Arrows: choose   PgUp/PgDn: item   Home/End: round   Ctrl+Z: undo   Ctrl+F: find   Q / R: queue / run it   1 2 3: panels   Ctrl+[ Ctrl+]: fold a side   F1..: presets", DIM, S.ing);
            }
        }
        for (r = 0; R && r * S.per < R->n; r++) {
            nk_layout_row_static(ctx, 128 + 36, 128, S.per);
            for (c = 0; c < S.per; c++) {
                struct nk_rect b; enum nk_widget_layout_states ws; struct nk_command_buffer *cv; Shot *s; int k = r * S.per + c;
                const struct nk_user_font *f = ctx->style.font; char t[64];
                if (k >= R->n) { nk_spacing(ctx, 1); continue; }
                s = &R->s[k];
                ws = nk_widget(&b, ctx);
                if (ws == NK_WIDGET_INVALID) continue;
                cv = nk_window_get_canvas(ctx);
                if (s->st == ST_DONE && s->res && (!s->bm || s->bmgen != s->gen)) {
                    int z; EnterCriticalSection(&S.cs); z = 128 / (s->rw > s->rh ? s->rw : s->rh); if (z < 1) z = 1;
                    if (s->bm) DeleteObject(s->bm); s->bm = mkbmp(s->res, s->rw, s->rh, z, 0); s->bmgen = s->gen; LeaveCriticalSection(&S.cs);
                }
                if (s->st == ST_DONE && s->bm) {
                    int z = 128 / (s->rw > s->rh ? s->rw : s->rh), iw, ih; struct nk_image im; if (z < 1) z = 1; iw = s->rw * z; ih = s->rh * z;
                    nk_fill_rect(cv, nk_rect(b.x, b.y, 128, 128), 0, nk_rgb(64, 68, 77));
                    im = nkimg(s->bm, iw, ih); nk_draw_image(cv, nk_rect(b.x + (128 - iw) / 2, b.y + (128 - ih) / 2, (float)iw, (float)ih), &im, nk_rgb(255, 255, 255));
                    if (S.bm_mini) {    /* the original, small, in the corner: always there to compare with */
                        nk_fill_rect(cv, nk_rect(b.x + 128 - 35, b.y + 1, 34, 34), 0, nk_rgb(30, 32, 38));
                        im = nkimg(S.bm_mini, 32, 32); nk_draw_image(cv, nk_rect(b.x + 128 - 34, b.y + 2, 32, 32), &im, nk_rgb(255, 255, 255));
                    }
                }
                else {
                    const char *m = s->st == ST_WAIT ? "waiting" : s->st == ST_DRAW ? "drawing..." : s->st == ST_FAIL ? "failed (right-click: again)" : "stopped (right-click: draw)";
                    nk_fill_rect(cv, nk_rect(b.x, b.y, 128, 128), 0, nk_rgb(40, 42, 50));
                    nk_draw_text(cv, nk_rect(b.x + 8, b.y + 54, 120, 20), m, (int)strlen(m), f, nk_rgb(40, 42, 50), s->st == ST_DRAW ? GOLD : DIM);
                    if (s->st == ST_DRAW) {   /* its progress: the built-in engine's steps, else the time against the model's usual */
                        LONG st = S.sd_step, sn = S.sd_steps; DWORD el = GetTickCount() - S.draw_t0, avg = S.avg_ms[s->model]; double fr = 0; char pt[48];
                        if (sn > 0) { fr = (double)st / sn; snprintf(pt, sizeof pt, "step %ld of %ld", (long)st, (long)sn); }
                        else if (avg) { fr = (double)el / avg; if (fr > 0.98) fr = 0.98; snprintf(pt, sizeof pt, "%lu s of about %lu", el / 1000, (avg + 500) / 1000); }
                        else snprintf(pt, sizeof pt, "%lu s", el / 1000);
                        nk_draw_text(cv, nk_rect(b.x + 8, b.y + 76, 120, 18), pt, (int)strlen(pt), f, nk_rgb(40, 42, 50), DIM);
                        nk_fill_rect(cv, nk_rect(b.x + 8, b.y + 100, 112, 8), 0, nk_rgb(60, 64, 72));
                        nk_fill_rect(cv, nk_rect(b.x + 8, b.y + 100, (float)(112 * fr), 8), 0, TEAL2);
                    }
                }
                {   /* the model's name (cut with "..." when it is longer than the cell) and, under it, the seed whole (review 2, P2-3) */
                    char nm[64]; int len; snprintf(nm, sizeof nm, "%s", S.m[s->model].name); len = (int)strlen(nm);
                    while (len > 1 && f->width(f->userdata, f->height, nm, len) > 126) { len--; nm[len] = 0; if (len > 3) { nm[len - 1] = nm[len - 2] = nm[len - 3] = '.'; } }
                    nk_draw_text(cv, nk_rect(b.x, b.y + 130, 128, 17), nm, len, f, nk_rgb(45, 45, 45), MODEL_COL[s->model % 6]);
                    snprintf(t, sizeof t, "seed %u", s->seed);
                    nk_draw_text(cv, nk_rect(b.x, b.y + 147, 128, 17), t, (int)strlen(t), f, nk_rgb(45, 45, 45), DIM);
                    if (nk_input_is_mouse_hovering_rect(&ctx->input, nk_rect(b.x, b.y + 128, 128, 36))) { char tt[96]; snprintf(tt, sizeof tt, "%s, seed %u", S.m[s->model].name, s->seed); nk_ext_tooltip(ctx, tt); }
                }
                if (S.sel == k) nk_stroke_rect(cv, nk_rect(b.x - 3, b.y - 3, 134, 134), 0, 3, TEAL2);
                if (ws == NK_WIDGET_VALID && s->st == ST_DONE && nk_input_mouse_clicked(&ctx->input, NK_BUTTON_LEFT, b)) { S.sel = k; if (S.tab == 1) S.tab = 0; }
                if (ws == NK_WIDGET_VALID && s->st == ST_DONE && nk_input_is_mouse_click_down_in_rect(&ctx->input, NK_BUTTON_DOUBLE, b, nk_true)) dbl = k;
                if (ws == NK_WIDGET_VALID && nk_input_is_mouse_click_down_in_rect(&ctx->input, NK_BUTTON_RIGHT, b, nk_true)) rclick = k;
                if (s->st == ST_FAIL && nk_input_is_mouse_hovering_rect(&ctx->input, b)) nk_ext_tooltip(ctx, s->err);
            }
        }
        nk_group_end(ctx);
        if (rclick >= 0) redraw_one(rclick);
        else if (dbl >= 0) { S.sel = dbl; keep_next(); }
    }
    nk_layout_row_push(ctx, SPLIT_W); split_bar(ctx, 2);
    /* right: the chosen one, and Keep */
    nk_layout_row_push(ctx, (float)S.split[1]);
    if (S.fold[1]) fold_strip(ctx, 1);
    else if (nk_group_begin(ctx, "chosen", NK_WINDOW_BORDER)) {
        Shot *s = S.sel >= 0 && S.nr && S.sel < S.r[S.cur].n ? &S.r[S.cur].s[S.sel] : NULL;
        {   static const int TW[] = { 0, 0, 0, 22 }; struct nk_rect fb; nk_ext_row(ctx, 28, 4, TW);
            if (button(ctx, "Chosen (1)", S.tab == 0, 1)) S.tab = 0;
            if (button(ctx, "Settings (2)", S.tab == 1, 1)) S.tab = 1;
            if (button(ctx, "Compare (3)", S.tab == 2, 1)) S.tab = 2;
            fb = nk_widget_bounds(ctx); if (button(ctx, ">", 0, 1)) fold_set(1, 1);
            if (nk_input_is_mouse_hovering_rect(&ctx->input, fb)) nk_ext_tooltip(ctx, "Fold this panel to a strip (Ctrl+])"); }
        if (S.tab == 1) settings_panel(ctx);
        else if (S.tab == 2) compare_panel(ctx, s);
        else {
        nk_layout_row_dynamic(ctx, 20, 1);
        if (s && s->st == ST_DONE) {
            nk_layout_row_dynamic(ctx, 40, 1);      /* first, so it is in sight at any window size */
            if (button(ctx, "Keep this one (Enter)", 1, 1)) keep_next();
            preview(S.cur, S.sel);
            snprintf(lab, sizeof lab, "%s seed %u", S.m[s->model].name, s->seed); nk_label_colored(ctx, lab, NK_TEXT_LEFT, GOLD);
            snprintf(lab, sizeof lab, "As it will be in the game (%d x %d)", s->rw, s->rh); nk_label_colored(ctx, lab, NK_TEXT_LEFT, DIM);
            nk_layout_row_static(ctx, (float)(S.pv_h ? S.pv_h : 192), S.pv_w ? S.pv_w : 192, 1); if (S.pv_small) nk_image(ctx, nkimg(S.pv_small, S.pv_w, S.pv_h)); else nk_spacing(ctx, 1);
            {   /* what the model was given: the original, or the upright picture */
                int up = s->src == FROM_UP && S.pv_up;
                nk_layout_row_dynamic(ctx, 20, 1); nk_label_colored(ctx, up ? "The upright picture it was given" : "The original", NK_TEXT_LEFT, DIM);
                nk_layout_row_static(ctx, 192, 192, 1); if (up) nk_image(ctx, nkimg(S.pv_up, 192, 192)); else if (S.pv_orig) nk_image(ctx, nkimg(S.pv_orig, 192, 192));
            }
            nk_layout_row_dynamic(ctx, 20, 1); nk_label_colored(ctx, "The model's drawing", NK_TEXT_LEFT, DIM);
            nk_layout_row_static(ctx, 192, 192, 1); if (S.pv_draw) nk_image(ctx, nkimg(S.pv_draw, 192, 192));
            if (S.nundo) { char ub[64]; nk_layout_row_dynamic(ctx, 28, 1); if (button(ctx, undo_what(ub, sizeof ub), 0, !busy)) undo_any(); }   /* this session only */
            s_check(ctx, "After Keep: the next new item", &S.auto_next);
        } else {
            nk_label_colored(ctx, "Click a picture to choose it (or the arrow keys).", NK_TEXT_LEFT, DIM);
            if (S.nundo) { char ub[64]; nk_layout_row_dynamic(ctx, 28, 1); if (button(ctx, undo_what(ub, sizeof ub), 0, !busy)) undo_any(); nk_layout_row_dynamic(ctx, 20, 1); }
            s_check(ctx, "After Keep: the next new item", &S.auto_next); nk_layout_row_dynamic(ctx, 20, 1);
            nk_layout_row_dynamic(ctx, 20, 1); nk_label_colored(ctx, "The original", NK_TEXT_LEFT, DIM);
            nk_layout_row_static(ctx, 192, 192, 1); if (S.pv_orig) nk_image(ctx, nkimg(S.pv_orig, 192, 192));
        }
        }
        nk_group_end(ctx);
    }
    nk_layout_row_end(ctx);
    {   /* a text box or a number being typed in: the keys go to it, not to the window's actions */
        struct nk_window *w = ctx->current;
        S.typing = S.find_typing || (w && (w->edit.active || w->property.active));
    }
    nk_end(ctx);
    return 1;
}

static LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_DESTROY) { S.wp.length = sizeof S.wp; S.have_wp = GetWindowPlacement(w, &S.wp) != 0; PostQuitMessage(0); return 0; }
    if (m == WM_APP) return 0;
    if (m == WM_SETCURSOR && LOWORD(lp) == HTCLIENT && (S.cur_we || S.drag)) { SetCursor(LoadCursor(NULL, IDC_SIZEWE)); return TRUE; }   /* over a divider */
    if (m == WM_DPICHANGED) {     /* moved to a monitor of another scale (B9) */
        RECT *r = (RECT *)lp; S.ui = HIWORD(wp) / 96.0f; nk_gdi_set_scale(S.ui);
        vlog(VLOG_INFO, "dpi", "the window's scale: %.2f (%u DPI)", S.ui, (unsigned)HIWORD(wp));
        SetWindowPos(w, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE); return 0;
    }
    if (m == WM_GETMINMAXINFO) {   /* never smaller than the three panels at their least (Draw, Keep and the original stay in sight) */
        RECT r = { 0, 0, (LONG)(WIN_MIN_W * (S.ui > 0 ? S.ui : 1)), (LONG)(WIN_MIN_H * (S.ui > 0 ? S.ui : 1)) }; MINMAXINFO *mi = (MINMAXINFO *)lp;
        AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_APPWINDOW);
        mi->ptMinTrackSize.x = r.right - r.left; mi->ptMinTrackSize.y = r.bottom - r.top; return 0;
    }
    if (m == WM_CLOSE && S.busy) {
        InterlockedExchange(&S.stop, 1);
#if SD_BUILT
        if (SDX_DRAWING && SDX) sd_cancel_generation(SDX, SD_CANCEL_ALL);     /* not a 10 s wait for the picture (review A16) */
#endif
    }
    if (m == WM_KEYDOWN && wp == VK_RETURN && GetKeyState(VK_CONTROL) < 0) { if (!S.busy) start_round(); return 0; }   /* Ctrl+Enter: draw, even while typing */
    if (m == WM_KEYDOWN && wp == VK_ESCAPE && S.typing) { S.unfocus = 1; return 0; }                 /* Esc: out of the text box */
    if (m == WM_KEYDOWN && !S.typing && hotkey((int)wp)) return 0;
    if (m == WM_SYSKEYDOWN && wp == VK_F10 && !S.typing && hotkey(VK_F10)) return 0;   /* F10 comes as a system key */
    if (nk_gdi_handle_event(w, m, wp, lp)) return 0;
    return DefWindowProcW(w, m, wp, lp);
}
static void save_bmp(const char *path, int W, int H) {      /* the window's real pixels (scaled, B9) */
    BITMAPINFO bi; BITMAPFILEHEADER fh; int row = (W * 3 + 3) & ~3; unsigned char *px = xmalloc((size_t)row * H); FILE *f;
    memset(&bi, 0, sizeof bi); bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 24; bi.bmiHeader.biCompression = BI_RGB;
    GetDIBits(nk_gdi_memdc(), nk_gdi_membitmap(), 0, (UINT)H, px, &bi, DIB_RGB_COLORS);
    memset(&fh, 0, sizeof fh); fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof(BITMAPINFOHEADER); fh.bfSize = fh.bfOffBits + (DWORD)(row * H);
    if ((f = fopen(path, "wb"))) { fwrite(&fh, sizeof fh, 1, f); fwrite(&bi.bmiHeader, sizeof(BITMAPINFOHEADER), 1, f); fwrite(px, (size_t)row, (size_t)H, f); fclose(f); }
    else vlog(VLOG_ERR, "selftest", "could not write %s: %s", path, vlog_errno());
    free(px);
}
static void settings(int save) {
    char p[MAX_PATH], *t, *q; snprintf(p, sizeof p, "%s\\pick.ini", WORK);
    int i, m, any = 0;
    if (save) {       /* item=NNN, then n.ID=N: how many pictures each model draws */
        Buf b = { 0 }; char l[80];
        snprintf(l, sizeof l, "item=%d\r\n", S.item); buf_str(&b, l);
        const ShrinkOpt *g = S.own_opt ? &S.gopt : &S.opt;      /* the common settings (an item's own are in its folder) */
        if (S.own_opt) item_opt_save(S.item);
        for (i = 0; SHRINK_KEYS[i]; i++) { int v; shrink_get(g, SHRINK_KEYS[i], &v); snprintf(l, sizeof l, "shrink.%s=%d\r\n", SHRINK_KEYS[i], v); buf_str(&b, l); }
        snprintf(l, sizeof l, "auto_next=%d\r\ntab=%d\r\nshow_get=%d\r\n", S.auto_next, S.tab, S.show_get); buf_str(&b, l);
        if (S.have_wp) { RECT r = S.wp.rcNormalPosition; snprintf(l, sizeof l, "window=%ld,%ld,%ld,%ld,%d\r\n", r.left, r.top, r.right, r.bottom, S.wp.showCmd == SW_SHOWMAXIMIZED); buf_str(&b, l); }
        for (m = 0; m < S.nm; m++) { snprintf(l, sizeof l, "n.%s=%d\r\n", S.m[m].id, S.m[m].n); buf_str(&b, l); }
        for (m = 0; m < S.nm; m++) if (S.m[m].sdimage == 2) { snprintf(l, sizeof l, "strength.%s=%d\r\n", S.m[m].id, S.m[m].strength); buf_str(&b, l); }
        for (m = 0; m < S.nm; m++) if (S.m[m].has_lora) { snprintf(l, sizeof l, "lora.%s=%d\r\n", S.m[m].id, S.m[m].lora_w); buf_str(&b, l); }
        snprintf(l, sizeof l, "engine=%d\r\nsplit=%d,%d\r\nfolds=%u\r\nkeys=%d\r\nfold=%d,%d\r\n", S.engine, S.want[0], S.want[1], S.folds, S.keys_open, S.fold[0], S.fold[1]); buf_str(&b, l);
        write_all(p, b.p, b.n); free(b.p); return;
    }
    for (m = 0; m < S.nm; m++) S.m[m].n = !strcmp(S.m[m].id, "klein") || !strcmp(S.m[m].id, "kontext") ? 10 : 0;
    if (!(t = read_all(p, NULL))) return;
    if ((q = strstr(t, "item="))) { int k = atoi(q + 5); for (i = 0; i < S.nlist; i++) if (S.list[i] == k) S.pos = i; }
    for (m = 0; m < S.nm; m++) {
        char k[40]; snprintf(k, sizeof k, "\nn.%s=", S.m[m].id);
        if ((q = strstr(t, k))) S.m[m].n = atoi(q + strlen(k));
        else if (!strcmp(S.m[m].id, "klein") && (q = strstr(t, "\nklein="))) S.m[m].n = atoi(q + 7);       /* 2026-11-20's pick.ini */
        else if (!strcmp(S.m[m].id, "kontext") && (q = strstr(t, "\nkontext="))) S.m[m].n = atoi(q + 9);
        if (S.m[m].n < 0 || S.m[m].n > 20) S.m[m].n = 0;
        snprintf(k, sizeof k, "\nstrength.%s=", S.m[m].id); if ((q = strstr(t, k)) && atoi(q + strlen(k)) >= 5 && atoi(q + strlen(k)) <= 100) S.m[m].strength = atoi(q + strlen(k));
        snprintf(k, sizeof k, "\nlora.%s=", S.m[m].id); if ((q = strstr(t, k)) && atoi(q + strlen(k)) >= 0 && atoi(q + strlen(k)) <= 200) S.m[m].lora_w = atoi(q + strlen(k));
        any += S.m[m].ready ? S.m[m].n : 0;
    }
    if (!any) for (m = 0; m < S.nm; m++) if (S.m[m].ready) { S.m[m].n = 10; if (++any == 2) break; }
    if ((q = strstr(t, "\nengine="))) S.engine = atoi(q + 8) == 1;
    if ((q = strstr(t, "\nfolds="))) S.folds = (unsigned)strtoul(q + 7, NULL, 10);
    if ((q = strstr(t, "\nkeys="))) S.keys_open = atoi(q + 6) == 1;
    if ((q = strstr(t, "\nfold="))) { int a = 0, b2 = 0; if (sscanf(q + 6, "%d,%d", &a, &b2) == 2) { S.fold[0] = a == 1; S.fold[1] = b2 == 1; } }
    if ((q = strstr(t, "\nsplit="))) { int a, b2; if (sscanf(q + 7, "%d,%d", &a, &b2) == 2) {     /* fitted to the least widths (review 2, P3-8) */
        S.want[0] = a < LEFT_MIN ? LEFT_MIN : a > 3999 ? 3999 : a; S.want[1] = b2 < RIGHT_MIN ? RIGHT_MIN : b2 > 3999 ? 3999 : b2; } }
    if ((q = strstr(t, "\nauto_next="))) S.auto_next = atoi(q + 11) == 1;
    if ((q = strstr(t, "\ntab="))) S.tab = atoi(q + 5) >= 0 && atoi(q + 5) <= 2 ? atoi(q + 5) : 0;
    if ((q = strstr(t, "\nshow_get="))) S.show_get = atoi(q + 10) == 1;
    if ((q = strstr(t, "\nwindow="))) { long a, b2, c, d; int mx = 0;
        if (sscanf(q + 8, "%ld,%ld,%ld,%ld,%d", &a, &b2, &c, &d, &mx) >= 4 && c - a >= 600 && d - b2 >= 400) {
            memset(&S.wp, 0, sizeof S.wp); S.wp.length = sizeof S.wp; S.wp.rcNormalPosition.left = a; S.wp.rcNormalPosition.top = b2;
            S.wp.rcNormalPosition.right = c; S.wp.rcNormalPosition.bottom = d; S.wp.showCmd = mx ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL; S.have_wp = 2; } }
    if ((q = strstr(t, "\nown="))) S.opt.colours = atoi(q + 5) == 1 ? C_OWN : C_ORIGINAL;      /* 2026-11-21 morning's pick.ini */
    for (i = 0; SHRINK_KEYS[i]; i++) { char k[48]; snprintf(k, sizeof k, "\nshrink.%s=", SHRINK_KEYS[i]); if ((q = strstr(t, k))) shrink_set(&S.opt, SHRINK_KEYS[i], atoi(q + strlen(k))); }
    free(t);
}

/* every item with an original picture (16 x 16 NNN.png in sprites =), minus voxup.ini's skip list */
static void scan_items(void) {
    char p[MAX_PATH]; int k;
    S.nlist = 0;
    if (!P_SPRITES[0]) return;
    for (k = 0; k < ITEMS_MAX; k++) {
        if (SKIP[k]) continue;
        snprintf(p, sizeof p, "%s\\%03d.png", P_SPRITES, k);
        { int w, h, n; if (!stbi_info(p, &w, &h, &n) || w != 16 || h != 16) continue; }    /* the window works with 16 x 16 originals */
        S.list[S.nlist++] = k;
        itemdir(k, p, sizeof p); strcat(p, "\\kept.txt"); S.kept[k] = (unsigned char)exists(p);
    }
}
/* voxup.ini beside the exe: "key = value" set (the line replaced, else added), the rest of the file as it was */
static void ini_set(const char *key, const char *val) {
    char p[MAX_PATH], *t, *l, *e; Buf b = { 0 }; int done = 0; size_t kn = strlen(key);
    snprintf(p, sizeof p, "%s\\voxup.ini", EXEDIR);
    t = read_all(p, NULL);
    for (l = t; l && *l; l = e) {
        char *q = l; e = strchr(l, '\n'); e = e ? e + 1 : l + strlen(l);
        while (*q == ' ' || *q == '\t') q++;
        if (!done && !strncmp(q, key, kn) && (q[kn] == ' ' || q[kn] == '\t' || q[kn] == '=')) {
            char line[MAX_PATH + 64]; snprintf(line, sizeof line, "%s = %s\r\n", key, val); buf_str(&b, line); done = 1;
        } else { size_t n = (size_t)(e - l); char *c = xmalloc(n + 1); memcpy(c, l, n); c[n] = 0; buf_str(&b, c); free(c); }
    }
    if (!done) { char line[MAX_PATH + 64]; snprintf(line, sizeof line, "%s = %s\r\n", key, val); buf_str(&b, line); }
    if (write_all(p, b.p, b.n)) vlog(VLOG_INFO, "ini", "%s: %s = %s", p, key, val);
    free(b.p); free(t);
}
/* the first start: a voxup.ini with plain words, so the next start finds it (nothing else is needed) */
static void ini_first(void) {
    char p[MAX_PATH], t[2 * MAX_PATH + 900];
    snprintf(p, sizeof p, "%s\\voxup.ini", EXEDIR);
    if (exists(p)) return;
    snprintf(t, sizeof t,
        "# voxup.ini: made by voxup on its first start. Lines are \"key = value\"; # starts a comment.\r\n"
        "# Every key is explained in voxup.ini.example (in voxup's source).\r\n"
        "\r\n"
        "# the original sprites, NNN.png (000.png, 001.png ...): voxup asks for this folder\r\n"
        "sprites =\r\n"
        "# where Get puts the model files (several GB each): voxup makes this folder\r\n"
        "model_files = %s\\model-files\r\n"
        "# optional: where Keep also writes NNN.png (may repeat); without it keeps go to out\\pick\\export\r\n"
        "# game = C:\\path\\to\\game\\sprites\r\n", EXEDIR);
    if (write_all(p, t, strlen(t))) vlog(VLOG_INFO, "ini", "first start: wrote %s", p);
}
/* Windows' own folder picker; the self-test gives the answer itself (--pick-folder DIR) */
static int pick_folder(char *out, size_t n) {
    BROWSEINFOW bi; PIDLIST_ABSOLUTE id; wchar_t w[MAX_PATH];
    if (TEST_PICK) { snprintf(out, n, "%s", TEST_PICK); return 1; }
    memset(&bi, 0, sizeof bi); bi.hwndOwner = S.wnd; bi.lpszTitle = L"The folder with your sprites (000.png, 001.png ...)";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (!(id = SHBrowseForFolderW(&bi))) return 0;
    if (!SHGetPathFromIDListW(id, w)) { CoTaskMemFree(id); return 0; }
    CoTaskMemFree(id); WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)n, NULL, NULL); return 1;
}
static void ini_set(const char *key, const char *val);
static void choose_palette(void) {     /* Windows' own file dialog; the choice is kept in voxup.ini palette = (B8) */
    OPENFILENAMEA of; char f[MAX_PATH] = "";
    if (TEST_PICK) snprintf(f, sizeof f, "%s", TEST_PICK);
    else {
        memset(&of, 0, sizeof of); of.lStructSize = sizeof of; of.hwndOwner = S.wnd; of.lpstrFile = f; of.nMaxFile = sizeof f;
        of.lpstrFilter = "Palettes (*.hex;*.gpl;*.txt;*.png)\0*.hex;*.gpl;*.txt;*.png\0All files\0*.*\0"; of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameA(&of)) return;
    }
    snprintf(P_PALETTE, MAX_PATH, "%s", f); ini_set("palette", f); status("Palette: %s", f);
}
static void choose_sprites(void) {
    char d[MAX_PATH];
    if (!pick_folder(d, sizeof d)) return;
    snprintf(P_SPRITES, MAX_PATH, "%s", d); scan_items();
    if (!S.nlist) { snprintf(S.empty_msg, sizeof S.empty_msg, "%s holds no 16 x 16 pictures named 000.png, 001.png ...: choose another folder.", d);
        vlog(VLOG_WARN, "pick", "%s holds no 16 x 16 NNN.png pictures", d); return; }
    ini_set("sprites", d);
    S.empty = 0; S.pos = 0; choose(0);
    status("%d items in %s (saved in voxup.ini). Press Space to draw.", S.nlist, d);
}
static void empty_frame(struct nk_context *ctx, int W, int H) {   /* no sprites yet: one thing to do, said plainly */
    int bw = W < 640 ? W - 40 : 600; struct nk_rect r = nk_rect((float)(W - bw) / 2, (float)(H / 2 - 110), (float)bw, 210);
    if (!nk_begin(ctx, "voxup", nk_rect(0, 0, (float)W, (float)H), NK_WINDOW_NO_SCROLLBAR)) { nk_end(ctx); return; }
    nk_layout_space_begin(ctx, NK_STATIC, (float)(H - 16), 1);
    nk_layout_space_push(ctx, nk_rect(r.x, r.y, r.w, r.h));
    if (nk_group_begin(ctx, "start", NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR)) {
        int i, ready = 0; char lab[200];
        for (i = 0; i < S.nm; i++) ready += S.m[i].ready;
        nk_layout_row_dynamic(ctx, 30, 1); nk_label_colored(ctx, "voxup: the models draw your sprites anew, you keep the best", NK_TEXT_CENTERED, GOLD);
        nk_ext_wrap_label(ctx, "First, the folder with your sprites: pictures of 16 x 16 named 000.png, 001.png, 002.png ...", nk_rgb(215, 215, 220), bw - 20);
        nk_layout_row_dynamic(ctx, 44, 1);
        if (button(ctx, "Choose the sprites folder", 1, 1)) choose_sprites();
        if (S.empty_msg[0]) nk_ext_wrap_label(ctx, S.empty_msg, GOLD, bw - 20);
        snprintf(lab, sizeof lab, "%d models known, %d ready to draw%s", S.nm, ready, ready ? "" : " (Get one in the window once the sprites are in)");
        nk_ext_wrap_label(ctx, lab, DIM, bw - 20);
        nk_group_end(ctx);
    }
    nk_layout_space_end(ctx);
    nk_end(ctx);
}
/* per-monitor DPI v2 (Windows 10 1703+; looked up, so an older Windows simply runs unscaled) (B9) */
static void dpi_aware(void) {
    typedef BOOL (WINAPI *SetCtx)(HANDLE); HMODULE u = GetModuleHandleA("user32.dll"); SetCtx f = u ? (SetCtx)(void *)GetProcAddress(u, "SetProcessDpiAwarenessContext") : NULL;
    if (f && !f((HANDLE)(INT_PTR)-4)) vlog(VLOG_INFO, "dpi", "per-monitor DPI v2 not set: %s", vlog_errno());
}
static float dpi_of(HWND w) {
    typedef UINT (WINAPI *GetDpi)(HWND); HMODULE u = GetModuleHandleA("user32.dll"); GetDpi f = u ? (GetDpi)(void *)GetProcAddress(u, "GetDpiForWindow") : NULL;
    UINT d = f && w ? f(w) : 96; return d ? d / 96.0f : 1.0f;
}
static int gui(int argc, char **argv) {
    const char *shot = NULL, *test_get = NULL; char *draws[2 * MAXM]; int ndraw = 0, drawn = 0; char *sets[64]; int nsets = 0;
    int test_accept = 0, test_contrast = 0, test_dpi = 0; const char *test_fold = NULL; const char *test_from = NULL, *test_split = NULL; int TEV[48][4], ntev = 0, kev = 0;
    int test_keep = 0, test_undo = 0, test_tab = -1, test_item = -1, test_redraw = -1, test_keys[8], ntest_keys = 0, test_clk[8][3], ntest_clk = 0; const char *test_find = NULL; int i, k, W = 1440, H = 940, running = 1, refresh = 1, frames = 0;
    WNDCLASSW wc; RECT rc; DWORD style = WS_OVERLAPPEDWINDOW; HDC dc; GdiFont *font; struct nk_context *ctx; char p[MAX_PATH], *t;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jl;
    if (argc > 1 && !strcmp(argv[1], "--selftest")) {
        shot = argc > 2 ? argv[2] : "selftest.bmp";
        for (i = 3; i < argc; i++) {
            if (!strcmp(argv[i], "--draw")) { while (i + 2 < argc && argv[i + 1][0] != '-' && ndraw < 2 * MAXM - 1) { draws[ndraw++] = argv[i + 1]; draws[ndraw++] = argv[i + 2]; i += 2; } }
            else if (!strcmp(argv[i], "--get") && i + 1 < argc) test_get = argv[++i];   /* Get pressed for that model first */
            else if (!strcmp(argv[i], "--set") && i + 1 < argc && nsets < 64) sets[nsets++] = argv[++i];   /* a shrink setting, key=value */
            else if (!strcmp(argv[i], "--tab") && i + 1 < argc) test_tab = atoi(argv[++i]);                 /* the right panel shown (0-2), over pick.ini's */
            else if (!strcmp(argv[i], "--from") && i + 1 < argc) test_from = argv[++i];               /* tool, upright or original */
            else if (!strcmp(argv[i], "--keep")) test_keep++;        /* may repeat: Keep pressed that many times */     /* Keep pressed too, into out\pick\selftest-game, never the game */
            else if (!strcmp(argv[i], "--undo")) test_undo++;        /* may repeat: Ctrl+Z that many times (after the keeps) */     /* and that keep undone again */
            else if (!strcmp(argv[i], "--redraw") && i + 1 < argc) test_redraw = atoi(argv[++i]);   /* that picture thrown out, drawn again */
            else if (!strcmp(argv[i], "--find") && i + 1 < argc) test_find = argv[++i];             /* the Find box, Enter pressed */
            else if (!strcmp(argv[i], "--click") && i + 1 < argc && ntest_clk < 8) { test_clk[ntest_clk][2] = 1; sscanf(argv[++i], "%d,%d,%d", &test_clk[ntest_clk][0], &test_clk[ntest_clk][1], &test_clk[ntest_clk][2]); ntest_clk++; }   /* X,Y[,N]: N 1 a click, 2 a double-click, 3 a right-click */
            else if (!strcmp(argv[i], "--size") && i + 1 < argc) { sscanf(argv[++i], "%dx%d", &W, &H); if (W < WIN_MIN_W) W = WIN_MIN_W; if (H < WIN_MIN_H) H = WIN_MIN_H; }   /* the hidden window's size */
#if SD_BUILT
            else if (!strcmp(argv[i], "--novulkan")) TEST_NOVK = 1;      /* acts as if vulkan-1.dll were missing */
#endif
            else if (!strcmp(argv[i], "--contrast")) test_contrast = 1;
            else if (!strcmp(argv[i], "--hostcheck")) {   /* which hosts would get the Hugging Face token */
                static const wchar_t *H[] = { L"huggingface.co", L"HuggingFace.CO", L"cdn-lfs.huggingface.co", L"huggingface.co.example.net", L"xhuggingface.co", L"example.com", L"127.0.0.1" }; int h;
                for (h = 0; h < 7; h++) vlog(VLOG_INFO, "hostcheck", "%-28ls %s", H[h], hf_host(H[h]) ? "gets the token" : "no token"); return 0; }
            else if (!strcmp(argv[i], "--dpi") && i + 1 < argc) test_dpi = atoi(argv[++i]);     /* the window drawn as on a monitor of that DPI (96, 144, 192) */
            else if (!strcmp(argv[i], "--fold") && i + 1 < argc) test_fold = argv[++i];   /* L,R: 1 folds that side (over pick.ini's) */     /* each text colour against its ground, in the log */
            else if (!strcmp(argv[i], "--accept-licence")) test_accept = 1;     /* Accept and get, when the licence is asked */
            else if (!strcmp(argv[i], "--pick-folder") && i + 1 < argc) TEST_PICK = argv[++i];    /* the folder picker's answer, Choose pressed */
            else if (!strcmp(argv[i], "--split") && i + 1 < argc) test_split = argv[++i];        /* L,R: the side panels' widths, over pick.ini's */
            else if (!strcmp(argv[i], "--type") && i + 1 < argc) {   /* TEXT typed (WM_CHAR, after the clicks): a text box gets it as from the keyboard */
                const char *c = argv[++i]; for (; *c && ntev < 48; c++) { TEV[ntev][0] = WM_CHAR; TEV[ntev][1] = (unsigned char)*c; TEV[ntev][2] = 0; TEV[ntev++][3] = 0; } }
            else if (!strcmp(argv[i], "--drag") && i + 1 < argc && ntev < 40) {   /* X1,Y1,X2,Y2: a divider dragged, as the window gets the mouse */
                int x1 = 0, y1 = 0, x2 = 0, y2 = 0, st; sscanf(argv[++i], "%d,%d,%d,%d", &x1, &y1, &x2, &y2);
                TEV[ntev][0] = WM_MOUSEMOVE; TEV[ntev][1] = 0; TEV[ntev][2] = x1; TEV[ntev++][3] = y1;
                TEV[ntev][0] = WM_LBUTTONDOWN; TEV[ntev][1] = MK_LBUTTON; TEV[ntev][2] = x1; TEV[ntev++][3] = y1;
                for (st = 1; st <= 4; st++) { TEV[ntev][0] = WM_MOUSEMOVE; TEV[ntev][1] = MK_LBUTTON; TEV[ntev][2] = x1 + (x2 - x1) * st / 4; TEV[ntev++][3] = y1 + (y2 - y1) * st / 4; }
                TEV[ntev][0] = WM_LBUTTONUP; TEV[ntev][1] = 0; TEV[ntev][2] = x2; TEV[ntev++][3] = y2;
            }
            else if (!strcmp(argv[i], "--key") && i + 1 < argc && ntest_keys < 8) test_keys[ntest_keys++] = atoi(argv[++i]);   /* a key pressed (Windows key code), as the window gets it */
            else if (!strcmp(argv[i], "--seeds") && i + 1 < argc) {  /* "A,B,C": the round's seeds, to redraw a known picture */
                char *s = argv[++i]; while (*s && ntest_seeds < 64) { test_seeds[ntest_seeds++] = (unsigned)strtoul(s, &s, 10); if (*s == ',') s++; else break; } }
            else test_item = atoi(argv[i]);
        }
    }
    InitializeCriticalSection(&S.cs);
    S.want[0] = LEFT_DEF; S.want[1] = RIGHT_DEF; S.per = 5; S.folds = FOLDS_DEFAULT;
    S.r = xmalloc(sizeof(Round) * MAXR); S.sel = -1; S.pv_for = -1; S.upitem = -1; S.undo_item = -1;
    S.job = CreateJobObjectA(NULL, NULL);
    if (S.job) { memset(&jl, 0, sizeof jl); jl.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE; SetInformationJobObject(S.job, JobObjectExtendedLimitInformation, &jl, sizeof jl); }
    mkdirs(WORK);
    load_names(); for (k = 0; k < N_DESC; k++) load_desc(P_DESC[k]);
    scan_items();
    if (!S.nlist) {     /* no sprites yet: the window opens and asks for the folder (no message box, no quitting) */
        S.empty = 1;
        if (P_SPRITES[0]) snprintf(S.empty_msg, sizeof S.empty_msg, "%s (voxup.ini sprites =) holds no 16 x 16 pictures named 000.png, 001.png ...", P_SPRITES);
        vlog(VLOG_WARN, "pick", "no items%s%s: the window asks for the sprites folder", P_SPRITES[0] ? " in " : " (sprites = not set)", P_SPRITES);
    }
    load_models();
    if (!S.nm) vlog(VLOG_ERR, "models", "no models at all: the list built into voxup.exe is missing too");
#if SD_BUILT
    vk_there();         /* said at the start (the Problem line), not at the first Draw */
#endif
    for (i = 0; i < S.nm; i++) {     /* each model's prompt: out\pick\ID-prompt.txt when changed, else its standard one */
        snprintf(p, sizeof p, "%s\\%s-prompt.txt", WORK, S.m[i].id);
        if ((t = read_all(p, NULL))) { wrap(t, S.m[i].prompt, sizeof S.m[i].prompt, 34); free(t); }
        else wrap(std_prompt(S.m[i].edit), S.m[i].prompt, sizeof S.m[i].prompt, 34);
    }
    shrink_defaults(&S.opt);
    settings(0);
    if (test_tab >= 0 && test_tab <= 2) S.tab = test_tab;
    if (test_split) { int a, b2; if (sscanf(test_split, "%d,%d", &a, &b2) == 2) { S.want[0] = a; S.want[1] = b2; } }
    memset(S.tilt, -1, sizeof S.tilt); S.cmp_for = -1;
    load_presets(); queue_load();
    for (i = 0; i < nsets; i++) { char k[40]; int v; if (sscanf(sets[i], "%39[^=]=%d", k, &v) != 2 || !shrink_set(&S.opt, k, v)) vlog(VLOG_ERR, "selftest", "--set: not a setting: %s", sets[i]); }
    if (test_item >= 0) for (i = 0; i < S.nlist; i++) if (S.list[i] == test_item) S.pos = i;
    if (ndraw) {
        for (i = 0; i < S.nm; i++) S.m[i].n = 0;
        if (ndraw == 2 && isdigit((unsigned char)draws[0][0])) {             /* the old "--draw KLEIN KONTEXT" */
            if ((k = find_model("klein")) >= 0) S.m[k].n = atoi(draws[0]);
            if ((k = find_model("kontext")) >= 0) S.m[k].n = atoi(draws[1]);
        } else for (i = 0; i + 1 < ndraw; i += 2) {
            int m = find_model(draws[i]);
            if (m < 0) vlog(VLOG_ERR, "selftest", "--draw: no model \"%s\"", draws[i]); else S.m[m].n = atoi(draws[i + 1]);
        }
    }

    memset(&wc, 0, sizeof wc);
    wc.style = CS_DBLCLKS; wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleW(0);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION); wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.lpszClassName = L"voxupWindow";
    RegisterClassW(&wc);
    if (!shot) dpi_aware();
    S.ui = shot ? (test_dpi > 0 ? test_dpi / 96.0f : 1.0f) : 1.0f;
    rc.left = 0; rc.top = 0; rc.right = (LONG)(W * S.ui); rc.bottom = (LONG)(H * S.ui); AdjustWindowRectEx(&rc, style, FALSE, WS_EX_APPWINDOW);
    /* the self-test window is never shown: it is drawn into its own surface and written to a file */
    S.wnd = CreateWindowExW(shot ? WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE : WS_EX_APPWINDOW, wc.lpszClassName, L"voxup: draw 20, keep the best",
                            style | (shot ? 0 : WS_VISIBLE), shot ? -4000 : CW_USEDEFAULT, shot ? -4000 : CW_USEDEFAULT,
                            rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, wc.hInstance, NULL);
    if (!S.wnd) { vlog(VLOG_ERR, "pick", "could not make the window: %s", vlog_errno()); return 1; }
    if (!shot && S.have_wp == 2) {        /* where it was last time (pick.ini window=), if that is still on a screen */
        RECT r = S.wp.rcNormalPosition; if (MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) SetWindowPlacement(S.wnd, &S.wp); }
    if (!shot) {     /* the monitor it opened on: its scale, and the window that big */
        S.ui = dpi_of(S.wnd);
        if (S.ui != 1.0f && S.have_wp != 2) { RECT r = { 0, 0, (LONG)(W * S.ui), (LONG)(H * S.ui) }; AdjustWindowRectEx(&r, style, FALSE, WS_EX_APPWINDOW);
            SetWindowPos(S.wnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE); }
    }
    nk_gdi_set_scale(S.ui);
    vlog(VLOG_INFO, "dpi", "the window's scale: %.2f (%d DPI)", S.ui, (int)(S.ui * 96 + 0.5f));
    dc = GetDC(S.wnd);
    font = nk_gdifont_create("Segoe UI", 16);
    ctx = nk_gdi_init(font, dc, (unsigned)(W * S.ui), (unsigned)(H * S.ui));
    if (shot) {   /* a self-test never keeps into the real game folders (a test click or key can keep too) */
        static char g0[MAX_PATH], g1[MAX_PATH];
        snprintf(g0, sizeof g0, "%s\\selftest-game\\data", WORK); snprintf(g1, sizeof g1, "%s\\selftest-game\\dist", WORK);
        GAMEDIRS[0] = g0; GAMEDIRS[1] = g1; N_GAME = 2; snprintf(GAMETAG[0], 16, "data"); snprintf(GAMETAG[1], 16, "dist");
    }
    if (test_contrast) contrast_report(ctx);
    if (test_fold) { int a = 0, b2 = 0; sscanf(test_fold, "%d,%d", &a, &b2); S.fold[0] = a == 1; S.fold[1] = b2 == 1; }
    if (!S.empty) choose(S.pos);
    if (test_from) { int f; for (f = 0; f < 3; f++) if (!strcmp(test_from, FROM_NAMES[f]) && (f != FROM_UP || S.has_up)) { S.from = f; load_desc_text(S.item); } }
    if (test_find) { snprintf(S.find, sizeof S.find, "%s", test_find); first_fit(); }
    if (shot && test_get) { if ((k = find_model(test_get)) < 0) vlog(VLOG_ERR, "selftest", "--get: no model \"%s\"", test_get); else start_get(k); }
    vlog(VLOG_INFO, "pick", "window open%s, %d items, %d models", shot ? " (self-test, off screen)" : "", S.nlist, S.nm);
    while (running) {
        MSG msg;
        nk_input_begin(ctx);
        if (shot) { Sleep(40); refresh = 1; }
        else if (!refresh) {
            if (GetMessageW(&msg, NULL, 0, 0) <= 0) running = 0;
            else { TranslateMessage(&msg); DispatchMessageW(&msg); }
            refresh = 1;
        } else refresh = 0;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = 0;
            TranslateMessage(&msg); DispatchMessageW(&msg); refresh = 1;
        }
        nk_input_end(ctx);
        GetClientRect(S.wnd, &rc);
        if (shot) { rc.right = W; rc.bottom = H; }
        else { rc.right = (LONG)(rc.right / S.ui); rc.bottom = (LONG)(rc.bottom / S.ui); }     /* logical pixels (B9) */
        if (S.empty) empty_frame(ctx, rc.right > 200 ? rc.right : W, rc.bottom > 200 ? rc.bottom : H);
        else gui_frame(ctx, rc.right > 200 ? rc.right : W, rc.bottom > 200 ? rc.bottom : H);
        opt_check();      /* a changed setting: a new generation, the shown round made again in the background */
        if (!S.empty) queue_tick();
        if (!shot) { static int tm; if (nk_ext_tip_waiting() && !tm) { SetTimer(S.wnd, 7, 100, NULL); tm = 1; } else if (!nk_ext_tip_waiting() && tm) { KillTimer(S.wnd, 7); tm = 0; } }
        nk_gdi_render(nk_rgb(30, 32, 38));
        frames++;
        if (shot && !S.busy && !S.dl_busy && !S.rs_busy && !S.qrun && frames > 3) {
            if (ntest_clk) {   /* the mouse as the window gets it: moved there, then the button(s) */
                static int kc, ph; static const UINT SEQ[4][5] = { { 0 }, { WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP, 0 },
                    { WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP, WM_LBUTTONDBLCLK, WM_LBUTTONUP }, { WM_MOUSEMOVE, WM_RBUTTONDOWN, WM_RBUTTONUP, 0 } };
                int n = test_clk[kc][2] >= 1 && test_clk[kc][2] <= 3 ? test_clk[kc][2] : 1; UINT msg = SEQ[n][ph];
                PostMessageA(S.wnd, msg, msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK ? MK_LBUTTON : msg == WM_RBUTTONDOWN ? MK_RBUTTON : 0, MAKELPARAM(test_clk[kc][0], test_clk[kc][1]));
                if (++ph == 5 || !SEQ[n][ph]) { vlog(VLOG_INFO, "selftest", "mouse %d at %d,%d", n, test_clk[kc][0], test_clk[kc][1]); ph = 0; if (++kc == ntest_clk) ntest_clk = 0; }
                continue; }
            if (S.empty && TEST_PICK) { choose_sprites(); TEST_PICK = NULL; continue; }
            if (S.lic_ask && test_accept) { int mm = S.lic_ask - 1; lic_accept(mm); start_get(mm); test_accept = 0; continue; }
            if (kev < ntev) {   /* a drag: posted to the hidden window one message a frame, the real mouse never moves */
                PostMessageA(S.wnd, (UINT)TEV[kev][0], (WPARAM)TEV[kev][1], MAKELPARAM(TEV[kev][2], TEV[kev][3]));
                if (TEV[kev][0] == WM_LBUTTONUP) vlog(VLOG_INFO, "selftest", "dragged to %d,%d: panels %d | %d", TEV[kev][2], TEV[kev][3], S.want[0], S.want[1]);
                kev++; continue; }
            if (ntest_keys) { static int kk; PostMessageA(S.wnd, WM_KEYDOWN, (WPARAM)test_keys[kk], 0); vlog(VLOG_INFO, "selftest", "key %d pressed", test_keys[kk]); if (++kk == ntest_keys) ntest_keys = 0; continue; }
            if (test_redraw >= 0 && S.nr) { redraw_one(test_redraw); test_redraw = -1; continue; }
            if (ndraw && !drawn) { drawn = 1; if (round_size() > 0) { start_round(); continue; } vlog(VLOG_ERR, "selftest", "--draw: no ready model to draw with"); }
            if (S.nr && S.sel < 0) { for (i = 0; i < S.r[S.cur].n; i++) if (S.r[S.cur].s[i].st == ST_DONE) { S.sel = i; break; } if (S.sel >= 0) continue; }
            if (!test_keep && test_undo) { undo_any(); test_undo--; continue; }
            if (test_keep && S.sel >= 0) {
                static char g0[MAX_PATH], g1[MAX_PATH];
                snprintf(g0, sizeof g0, "%s\\selftest-game\\data", WORK); snprintf(g1, sizeof g1, "%s\\selftest-game\\dist", WORK);
                GAMEDIRS[0] = g0; GAMEDIRS[1] = g1; N_GAME = 2; snprintf(GAMETAG[0], 16, "data"); snprintf(GAMETAG[1], 16, "dist"); keep(); test_keep--; continue;
            }
            if (frames > 6 && S.empty) { save_bmp(shot, (int)(W * S.ui), (int)(H * S.ui)); vlog(VLOG_INFO, "selftest", "wrote %s (the empty start)", shot); break; }
            if (frames > 6) { char dsc[4096]; size_t dl; unwrap(S.descbuf, dsc, sizeof dsc); dl = strlen(dsc);
                vlog(VLOG_INFO, "selftest", "text boxes: Find \"%s\", What it is \"%.40s ... %s\"", S.find, dsc, dl > 40 ? dsc + dl - 40 : "");
                save_bmp(shot, (int)(W * S.ui), (int)(H * S.ui)); vlog(VLOG_INFO, "selftest", "wrote %s (%d x %d, panels %d | grid %d columns | %d%s%s)", shot, W, H, S.split[0], S.per, S.split[1], S.fold[0] ? ", left folded" : "", S.fold[1] ? ", right folded" : ""); break; }
        }
    }
    if (S.busy) { InterlockedExchange(&S.stop, 1); for (i = 0; i < 100 && S.busy; i++) Sleep(100); }
    stop_reshape();
    if (S.dl_busy) { InterlockedExchange(&S.dl_stop, 1); for (i = 0; i < 300 && S.dl_busy; i++) Sleep(100); }   /* its .part stays: Get goes on */
    settings(1);
    /* the venv's python.exe starts the real Python as its child: closing the job ends the whole tree at once */
    if (S.comfy) { TerminateProcess(S.comfy, 0); CloseHandle(S.comfy); vlog(VLOG_INFO, "comfy", "stopped the ComfyUI voxup started"); }
    if (S.job) { CloseHandle(S.job); S.job = NULL; }
    nk_gdifont_del(font); ReleaseDC(S.wnd, dc);
    if (shot) DestroyWindow(S.wnd);
    return 0;
}

/* voxup.ini beside the exe: "key = value" lines, # comments (voxup.ini.example has them all) */
static void read_ini(const char *exe, int selftest) {
    char p[MAX_PATH], *t, *l; int i;
    snprintf(p, sizeof p, "%s\\voxup.ini", exe);
    if (!(t = read_all(p, NULL))) return;
    for (l = strtok(t, "\r\n"); l; l = strtok(NULL, "\r\n")) {
        char k[40] = "", *v, *e; while (*l == ' ' || *l == '\t') l++;
        if (!*l || *l == '#' || !(v = strchr(l, '='))) continue;
        e = v; while (e > l && (e[-1] == ' ' || e[-1] == '\t')) e--;
        snprintf(k, sizeof k, "%.*s", (int)(e - l), l);
        v++; while (*v == ' ' || *v == '\t') v++;
        e = v + strlen(v); while (e > v && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        if (!strcmp(k, "sprites")) snprintf(P_SPRITES, MAX_PATH, "%s", v);
        else if (!strcmp(k, "names")) snprintf(P_NAMES, MAX_PATH, "%s", v);
        else if (!strcmp(k, "desc") && N_DESC < 4) snprintf(P_DESC[N_DESC++], MAX_PATH, "%s", v);
        else if (!strcmp(k, "enlarged")) snprintf(P_ENLARGED, MAX_PATH, "%s", v);
        else if (!strcmp(k, "upright") && N_UPRIGHT < 4) snprintf(P_UPRIGHT[N_UPRIGHT++], MAX_PATH, "%s", v);
        else if (!strcmp(k, "comfy")) snprintf(P_COMFY, MAX_PATH, "%s", v);
        else if (!strcmp(k, "comfy_paths")) snprintf(P_YAML, MAX_PATH, "%s", v);
        else if (!strcmp(k, "model_files")) snprintf(P_SHARED, MAX_PATH, "%s", v);
        else if (!strcmp(k, "game") && N_GAME < 4) { snprintf(P_GAME[N_GAME], MAX_PATH, "%s", v); GAMEDIRS[N_GAME] = P_GAME[N_GAME]; snprintf(GAMETAG[N_GAME], 16, "game%d", N_GAME + 1); N_GAME++; }
        else if (!strcmp(k, "palette")) snprintf(P_PALETTE, MAX_PATH, "%s", v);
        else if (!strcmp(k, "licence_ok")) snprintf(LIC_OK, sizeof LIC_OK, "%s", v);
        else if (!strcmp(k, "game_max")) GAME_MAX = atoi(v) > 0 ? atoi(v) : 32;
        else if (!strcmp(k, "work")) snprintf(WORK, MAX_PATH, selftest ? "%s-selftest" : "%s", v);
        else if (!strcmp(k, "models")) snprintf(MODELS, MAX_PATH, "%s", v);
        else if (!strcmp(k, "port")) PORT = atoi(v) > 0 ? atoi(v) : 8189;
        else if (!strcmp(k, "items")) ITEMS_MAX = atoi(v) > 0 && atoi(v) <= 512 ? atoi(v) : 512;
        else if (!strcmp(k, "skip")) {        /* "64-127, 15, 27-30" */
            char *q = v;
            while (*q) { int a, b; char *z; a = (int)strtol(q, &z, 10); if (z == q) { q++; continue; } b = a; q = z;
                if (*q == '-') { b = (int)strtol(q + 1, &z, 10); q = z; }
                for (i = a; i <= b && i < 512; i++) if (i >= 0) SKIP[i] = 1; }
        }
    }
    free(t);
}
int main(int argc, char **argv) {
    char exe[MAX_PATH], *e; int r;
    GetModuleFileNameA(NULL, exe, sizeof exe);
    if ((e = strrchr(exe, '\\'))) *e = 0;
    snprintf(EXEDIR, sizeof EXEDIR, "%s", exe);
    /* the self-test keeps to a folder of its own: its rounds and keeps never show in the real window */
    snprintf(WORK, sizeof WORK, argc > 1 && !strcmp(argv[1], "--selftest") ? "%s\\out\\pick-selftest" : "%s\\out\\pick", exe);
    snprintf(MODELS, sizeof MODELS, "%s\\models", exe);
    if (argc == 1 || !strcmp(argv[1], "--selftest")) ini_first();     /* the window's first start: voxup.ini made */
    read_ini(exe, argc > 1 && !strcmp(argv[1], "--selftest"));
    if (!P_SHARED[0]) snprintf(P_SHARED, MAX_PATH, "%s\\model-files", exe);      /* Get's folder: beside the exe unless set */
    if (argc > 1 && strcmp(argv[1], "--selftest")) {
        /* the command line: a window program has no console unless its parent's is borrowed */
        if (GetStdHandle(STD_ERROR_HANDLE) == NULL || GetStdHandle(STD_ERROR_HANDLE) == INVALID_HANDLE_VALUE) {
            if (AttachConsole(ATTACH_PARENT_PROCESS)) { freopen("CONOUT$", "w", stdout); freopen("CONOUT$", "w", stderr); }
        }
        if (!strcmp(argv[1], "--models")) {
            int m, f; vlog_open(NULL, 0); vlog_echo(1); load_models();
            for (m = 0; m < S.nm; m++) {
                printf("%-10s %-16s %s\n", S.m[m].id, S.m[m].name, S.m[m].ready ? "ready" : "not got");
                for (f = 0; f < S.m[m].nf; f++) printf("    %-8s %6.2f GB  %s\\%s\n", file_there(&S.m[m].f[f]) ? "there" : "MISSING", gb(S.m[m].f[f].size), S.m[m].f[f].dir, S.m[m].f[f].name);
            }
            vlog_close(); return 0;
        }
        if (!strcmp(argv[1], "--to32")) { vlog_open(NULL, 0); vlog_echo(1); r = to32_cli(argc, argv); vlog_close(); return r; }
        if (!strcmp(argv[1], "--shrink")) { vlog_open(NULL, 0); vlog_echo(1); r = shrink_cli(argc, argv); vlog_close(); return r; }
        if (!strcmp(argv[1], "--upright")) { vlog_open(NULL, 0); vlog_echo(1); r = upright_cli(argc, argv); vlog_close(); return r; }
        if (!strcmp(argv[1], "--shrinkall")) { vlog_open(NULL, 0); vlog_echo(1); r = shrinkall_cli(argc, argv); vlog_close(); return r; }
        if (!strcmp(argv[1], "--shrinkcheck")) { vlog_open(NULL, 0); vlog_echo(1); r = shrinkcheck_cli(argc, argv); vlog_close(); return r; }
        return voxup_cli(argc, argv);
    }
    vlog_open(NULL, 0);
    r = gui(argc, argv);
    vlog_close();
    return r;
}
