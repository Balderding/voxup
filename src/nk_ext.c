/* nk_ext.c: voxup's own additions to Nuklear (see nk_ext.h). Written for voxup, 2026-11-22. */
#include <string.h>
#include "nk_conf.h"
#include "third_party/nuklear.h"
#include "nk_ext.h"

void nk_ext_row(struct nk_context *ctx, float h, int n, const int *w) {
    int i;
    nk_layout_row_template_begin(ctx, h);
    for (i = 0; i < n; i++) { if (w[i] > 0) nk_layout_row_template_push_static(ctx, (float)w[i]); else nk_layout_row_template_push_dynamic(ctx); }
    nk_layout_row_template_end(ctx);
}

/* how many lines Nuklear's wrap makes of t in a line w wide: words (cut at spaces) as long as they fit */
static int wrap_lines(const struct nk_user_font *f, const char *t, float w) {
    int lines = 1, ns = 0; float x = 0, sp = f->width(f->userdata, f->height, " ", 1); const char *p = t;
    while (*p) {      /* each word with the spaces before it (all of them: "   " counts three) */
        const char *e = p; float ww;
        while (*e && *e != ' ') e++;
        ww = f->width(f->userdata, f->height, p, (int)(e - p));
        if (x > 0 && x + ns * sp + ww > w) { lines++; x = ww; } else x += (x > 0 ? ns * sp : 0) + ww;
        while (x > w && ww > w) { lines++; x -= w; ww -= w; }       /* a word longer than the line */
        p = e; ns = 0; while (*p == ' ') { p++; ns++; }
    }
    return lines;
}
struct nk_rect nk_ext_wrap_label(struct nk_context *ctx, const char *t, struct nk_color c, int w) {
    /* the width Nuklear wraps in: the panel's own row width (not the caller's guess, w is the fallback), less the
       text padding; the height its wrap needs: each line f->height + 2 x padding, and the row's padding (review 3, P3-6) */
    const struct nk_user_font *f = ctx->style.font; struct nk_rect b; float pw = ctx->current && ctx->current->layout ? ctx->current->layout->bounds.w : 0, px = ctx->style.text.padding.x, py = ctx->style.text.padding.y, lh;
    int lines;
    if (pw < 40) pw = (float)w;
    lines = pw > 40 ? wrap_lines(f, t, pw - 2 * px) : 2;
    if (lines > 8) lines = 8;
    lh = f->height + 2 * py;
    nk_layout_row_dynamic(ctx, lh * (float)lines + 3 * py + 1, 1); b = nk_widget_bounds(ctx); nk_label_colored_wrap(ctx, t, c);
    return b;
}

int nk_ext_splitter(struct nk_context *ctx, int active, struct nk_color hot, struct nk_color line, const char *tip) {
    struct nk_rect b; struct nk_input *in = &ctx->input; enum nk_widget_layout_states ws = nk_widget(&b, ctx); int r = 0, lit;
    if (ws == NK_WIDGET_INVALID) return 0;
    if (nk_input_is_mouse_hovering_rect(in, b)) r |= NK_EXT_SPLIT_HOVER;
    if (ws == NK_WIDGET_VALID && nk_input_is_mouse_click_down_in_rect(in, NK_BUTTON_DOUBLE, b, nk_true)) r |= NK_EXT_SPLIT_DOUBLE;
    else if (ws == NK_WIDGET_VALID && nk_input_is_mouse_click_down_in_rect(in, NK_BUTTON_LEFT, b, nk_true)) r |= NK_EXT_SPLIT_PRESS;
    lit = (r & NK_EXT_SPLIT_HOVER) || active;
    nk_fill_rect(nk_window_get_canvas(ctx), nk_rect(b.x + b.w / 2 - 1, b.y + 4, lit ? 3.0f : 2.0f, b.h - 8), 0, lit ? hot : line);
    if ((r & NK_EXT_SPLIT_HOVER) && tip) nk_ext_tooltip(ctx, tip);
    return r;
}

static unsigned TIP_NOW, TIP_T0, TIP_KEY, TIP_FRAME, TIP_LAST; static int TIP_WAIT;
void nk_ext_frame(unsigned now_ms) { TIP_NOW = now_ms; TIP_FRAME++; TIP_WAIT = 0; }
void nk_ext_tooltip(struct nk_context *ctx, const char *text) {
    unsigned k = 5381; const char *p;
    for (p = text; *p; p++) k = k * 33 + (unsigned char)*p;
    k ^= (unsigned)(int)ctx->input.mouse.pos.x / 8 * 7919u ^ (unsigned)(int)ctx->input.mouse.pos.y / 8 * 104729u;   /* the same thing, roughly the same place */
    if (k != TIP_KEY || TIP_LAST + 1 < TIP_FRAME) { TIP_KEY = k; TIP_T0 = TIP_NOW; }       /* new, or not asked for last frame: start waiting */
    TIP_LAST = TIP_FRAME;
    if (TIP_NOW - TIP_T0 >= NK_EXT_TIP_MS) nk_tooltip(ctx, text); else TIP_WAIT = 1;
}
int nk_ext_tip_waiting(void) { return TIP_WAIT; }
