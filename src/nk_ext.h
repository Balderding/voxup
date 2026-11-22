/* nk_ext.h: voxup's own additions to Nuklear (third_party/nuklear.h stays untouched, so a Nuklear update is a file
   copy). Include after nuklear.h. */
#ifndef NK_EXT_H
#define NK_EXT_H

/* a row of n widgets: w[i] > 0 a fixed width, 0 a share of what is left, so the row fits any panel width */
void nk_ext_row(struct nk_context *ctx, float h, int n, const int *w);

/* a label that wraps, as tall as its words need in a panel w wide (at most 8 lines); returns its place, for clicks */
struct nk_rect nk_ext_wrap_label(struct nk_context *ctx, const char *text, struct nk_color c, int w);

/* a divider between two panels, in a row slot of its own: a thin line, lit (hot) while hovered or dragged (active).
   Returns NK_EXT_SPLIT_* flags; the caller keeps the drag (where it started, which panel). */
enum { NK_EXT_SPLIT_HOVER = 1, NK_EXT_SPLIT_PRESS = 2, NK_EXT_SPLIT_DOUBLE = 4 };
int nk_ext_splitter(struct nk_context *ctx, int active, struct nk_color hot, struct nk_color line, const char *tip);

#endif
