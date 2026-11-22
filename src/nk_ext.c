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

struct nk_rect nk_ext_wrap_label(struct nk_context *ctx, const char *t, struct nk_color c, int w) {
    const struct nk_user_font *f = ctx->style.font; float tw = f->width(f->userdata, f->height, t, (int)strlen(t)) * 1.15f;
    int lines = w > 40 ? (int)(tw / (float)(w - 8)) + 1 : 2; struct nk_rect b;
    if (lines > 8) lines = 8;
    nk_layout_row_dynamic(ctx, 18.0f * (float)lines + 2, 1); b = nk_widget_bounds(ctx); nk_label_colored_wrap(ctx, t, c);
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
    if ((r & NK_EXT_SPLIT_HOVER) && tip) nk_tooltip(ctx, tip);
    return r;
}
