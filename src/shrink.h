/* shrink.h: voxup's last step, a model's drawing made into the game's sprite, every step with its own settings.

   The pipeline, in order (every stage has its own settings, ShrinkOpt below):
     1 cut-out      the drawing's background (its corners' colour, or plain magenta) made clear
     2 before       contrast / brightness / saturation, smoothing, sharpening, on the drawing at full size
     3 fit          the picture's place in the sprite: the original's own box (scaled to the size), the whole sprite, or
                    stretched; a margin kept free
     4 shrink       one of many methods (classic filters, majority/median, DPID 2016, SAID 2025, superpixels 2012)
     5 pinholes     a clear pixel inside the shape filled from its neighbours
     6 colours      the original's colours, the original's with shades, the model's own, or N colours found in it;
                    RGB, Lab or weighted RGB distance; dithering
     7 rim          the model's own dark rim peeled off
     8 clean-up     lone pixels, doubled outline corners ("jaggies")
     9 outline      none, black (side by side, or corners too), or selective (a darker shade of the colour beside it)
   With the defaults (shrink_defaults) the result is pixel for pixel pick.c's to32: proved by
   `voxup --shrinkcheck DIR`. */
#pragma once

enum { SZ_16, SZ_32, SZ_CUSTOM };
enum { FIT_BOX, FIT_FILL, FIT_STRETCH };
enum { SM_NONE, SM_MEDIAN, SM_BILATERAL };
enum { M_LANCZOS, M_NEAREST, M_BOX, M_BILINEAR, M_BICUBIC, M_MITCHELL, M_GAUSS, M_MEDIAN, M_MAJORITY, M_DPID, M_SAID, M_SUPERPIXEL,
       M_TWOSTEP, M_GRID, M_COUNT };
enum { C_ORIGINAL, C_SHADES, C_OWN, C_ASDRAWN, C_KMEANS, C_MEDIANCUT, C_WU, C_PALETTE };
enum { PAL_FILE, PAL_EGA, PAL_PICO8, PAL_GAMEBOY, PAL_C64, PAL_SET, PAL_COUNT };     /* C_PALETTE: which palette (B8) */
enum { D_RGB, D_LAB, D_REDMEAN };
enum { DI_NONE, DI_FLOYD, DI_ATKINSON, DI_BAYER };
enum { O_NONE, O_BLACK4, O_BLACK8, O_SELECTIVE, O_AUTO };   /* O_AUTO: black only when the original has an outline */

typedef struct {
    int size, cw, ch;              /* SZ_*; custom width and height (1 to 128) */
    int fit, margin;               /* FIT_*; pixels kept free at the sprite's edge */
    int bg_dist, magenta, alpha_cut;   /* cut-out: distance from the corners' colour, plain magenta too, coverage (of 255) kept */
    int contrast, brightness, saturation, smooth, sharpen;   /* before: -100..100 each, SM_*, sharpen 0..300 % */
    int method;                    /* M_* */
    int dpid_lambda;               /* DPID's lambda x 100 (0..300; 100 = 1.0) */
    int said_gamma, said_alpha, said_beta;   /* SAID's sharpening and texture weights x 100 */
    int sp_m, sp_iter;             /* superpixels: compactness (1..100), passes */
    int pinholes;                  /* passes, 0..3 */
    int colours, ncol, dist;       /* C_*; colours found (2..32); D_* */
    int dither, dither_amt;        /* DI_*; strength % */
    int black;                     /* a colour darker than this on every channel is made black (0 = off) */
    int pink;                      /* model's own colours: pink-tinted pixels (the background bleeding in) still snapped */
    int peel;                      /* rim layers peeled, 0..4 */
    int orphans, jaggies;          /* lone pixels taken by their neighbours; doubled outline corners removed */
    int edgestray, kmpp, palid;    /* ...; palid: PAL_* for C_PALETTE */           /* B7: an edge pixel with 6+ of its 8 neighbours empty goes; k-means++ seeds (else farthest-first) */
    int outline;                   /* O_* */
    int othick;                    /* the outline's thickness, 1..4 pixels (1 standard) */
    int edge;                      /* without an outline: 0 rough (as shrunk), 1 smooth (the silhouette's steps evened
                                      out with whole pixels: no see-through pixels, not anti-aliasing) */
    /* 2026-11-21, all off by default (so the defaults stay to32's): */
    int flood;                     /* cut-out: the background also taken by a flood from the border through near-
                                      background pixels (the pink fringe and the model's shadow go, inside holes stay) */
    int linear;                    /* shrink in linear light (thin bright and dark edges stay, not muddied) */
    int silhouette;                /* the shape decided on its own: half-covered pixels kept, thin lines kept (a pixel
                                      covered more than its two neighbours across it) */
    int regions;                   /* colours by region: each patch of like colour takes one palette colour by vote (Lab) */
    int notch, blocks;             /* after the outline: black bites into the body filled; 2 x 2 black blocks broken */
} ShrinkOpt;

void shrink_defaults(ShrinkOpt *o);
void shrink_size(const ShrinkOpt *o, int ow, int oh, int *W, int *H);   /* the sprite's size for an original of ow x oh */
/* drw: RGB dw x dh; orig: the sprite RGBA ow x oh. out: RGBA W x H (shrink_size), the caller's buffer (128 x 128 x 4 is
   always enough). 0 when there is nothing to cut out */
int shrink(const unsigned char *drw, int dw, int dh, const unsigned char *orig, int ow, int oh, const ShrinkOpt *o, unsigned char *out);
const char *shrink_describe(const ShrinkOpt *o, char *buf, int n);       /* one line: what differs from the defaults */
int shrink_set(ShrinkOpt *o, const char *key, int value);               /* by name (pick.ini, command line); 0 = no such */
int shrink_get(const ShrinkOpt *o, const char *key, int *value);
int orig_outlined(const unsigned char *rgba, int w, int h);              /* black fences the colours in */
extern const char *const SHRINK_KEYS[];                                  /* every setting's name, NULL-ended */
extern const char *const METHOD_NAMES[M_COUNT];
/* the last grid snap (M_GRID): the drawing's own pixel cell found, x and y, in its pixels, and how clearly (1 to 3;
   0 = no grid found, Majority was used) */
void shrink_grid_info(double *cell_x, double *cell_y, int *stars);
/* the palette C_PALETTE maps to (RGB triples, at most 256); set by the caller before shrinking (B8) */
void shrink_set_palette(const unsigned char *rgb, int n);
