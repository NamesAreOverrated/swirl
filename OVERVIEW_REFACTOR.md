# Overview Mode Refactor: Scene-Graph Thumbnails

Replace the offscreen-render pipeline (`collect_buffers` + `render_buffers` +
swapchain per container) with scene-graph nodes. Each thumbnail becomes a small
`wlr_scene_tree` slot with rect and buffer children — no render passes needed.

## Goal

Same visual output as today (dark card + border + frozen content per thumbnail
in a horizontal grid, with number badges), but faster and simpler:

- **No offscreen rendering** — no swapchain acquire, no render pass, no texture
  upload per container. Scene graph compositor handles all blending.
- **No scene tree recursion** — `collect_buffers`/`render_buffers` deleted.
  Content comes from `view->saved_buffer` (a `struct wlr_buffer *` ref that the
  view already maintains on each commit).
- **Same grid layout math** — thumbnails still sized via `fit` scaling, still
  centered in the output, still numbered 01–99.

## Implementation

### Already Done (commit `0e82cc53`)

- `view.h` — added `struct wlr_buffer *saved_buffer` to `sway_view`
- `view.c` — `view_update_saved_buffer()`: drops old ref, takes new ref from
  `view->surface->buffer` on each commit. Init/cleanup in `view_init`/`view_destroy`.
- `xdg_shell.c` / `xwayland.c` — both call `view_update_saved_buffer()` at end
  of `handle_commit`.
- `overview.c` — `collect_buffers()`/`render_buffers()` removed, swapchain
  acquire/render-pass removed for content. Badge still uses swapchain (cairo text).
  _But_: thumbnail is currently a bare `sb` on the overview layer — no card, no
  border, no slot tree. This plan fixes that.

### Changes to `overview.c`

#### Overview Thumbnail Struct

Add `struct wlr_scene_tree *slot` to the existing struct:

```c
struct overview_thumbnail {
    struct wl_list link;
    struct wlr_scene_tree *slot;   // scene tree holding card + border + content
    struct wlr_scene_buffer *sb;   // content buffer (child of slot)
    struct wlr_scene_rect *card;   // card background rect
    struct wlr_scene_rect *border; // border rect (NULL if border_thickness==0)
    struct wlr_scene_buffer *badge_sb;
    struct sway_container *con;
    int w, h;                      // thumbnail size = bw + 2*bt (card + border)
    float origin_y;                // con->pending.y (for grid fit)
};
```

#### Activation (inside `overview_toggle`)

For each tiled container with a view and `saved_buffer`:

```c
int bt = 0;
if (config->border == B_PIXEL || config->border == B_NORMAL)
    bt = (int)(config->border_thickness * scale);

struct wlr_buffer *buf = con->view->saved_buffer;
int bw = buf->width;
int bh = buf->height;
int card_w = bw + 2 * bt;
int card_h = bh + 2 * bt;

struct overview_thumbnail *t = calloc(1, sizeof(*t));
if (!t) continue;
t->w = card_w;
t->h = card_h;
t->origin_y = con->pending.y;
t->con = con;

// Slot tree: children are card → border → content → badge
t->slot = wlr_scene_tree_create(root->layers.overview);
if (!t->slot) { free(t); continue; }

// Card background (init dummy size; real size set in grid loop)
t->card = wlr_scene_rect_create(t->slot, card_w, card_h,
                                (float[4]){0.05, 0.05, 0.1, 1});

// Border rect (on top of card)
t->border = NULL;
if (bt > 0) {
    float *bc = config->border_colors.unfocused.border;
    t->border = wlr_scene_rect_create(t->slot, card_w, card_h,
                                      (float[4]){bc[0], bc[1], bc[2], bc[3]});
}

// Content buffer (on top of border; sized in grid loop)
t->sb = wlr_scene_buffer_create(t->slot, buf);
if (!t->sb) { wlr_scene_node_destroy(&t->slot->node); free(t); continue; }
wlr_scene_node_set_position(&t->sb->node, 0, 0);

// Badge overlay (child of slot, same swapchain/cairo as before)
t->badge_sb = NULL;
// ... badge creation code, last arg t->slot instead of root->layers.overview
```

Card/border/content NOT sized yet — sizing happens in grid loop using
`wlr_scene_rect_set_size` and `wlr_scene_buffer_set_dest_size`.

#### Grid Layout

```c
wl_list_for_each(t, &state.thumbnails, link) {
    float tw = (float)t->w / scale * fit;    // on-screen card width
    float th = (float)t->h / scale * fit;    // on-screen card height
    float ty = overview_oy + (t->origin_y - min_oy) * fit;
    float bt_s = (float)bt / scale * fit;    // border in on-screen

    // Position slot
    wlr_scene_node_set_position(&t->slot->node, (int)start_x, (int)ty);

    // Size card background + border rects to on-screen size
    wlr_scene_rect_set_size(t->card, (int)tw, (int)th);
    if (t->border) wlr_scene_rect_set_size(t->border, (int)tw, (int)th);

    // Content buffer: size = card minus 2×border, position = border offset
    int cw = (int)(tw - 2 * bt_s);
    int ch = (int)(th - 2 * bt_s);
    wlr_scene_buffer_set_dest_size(t->sb, cw, ch);
    wlr_scene_node_set_position(&t->sb->node, (int)bt_s, (int)bt_s);

    // Badge relative to slot
    if (t->badge_sb) {
        int bsz = (int)(48 * fit);
        if (bsz < 28) bsz = 28;
        int bpad = (int)(2 * fit);
        if (bpad < 1) bpad = 1;
        wlr_scene_buffer_set_dest_size(t->badge_sb, bsz, bsz);
        double scroll_y = t->con ? t->con->current.scroll_y : 0;
        wlr_scene_node_set_position(&t->badge_sb->node, bpad,
                                    bpad + (int)(scroll_y * fit));
        wlr_scene_node_raise_to_top(&t->badge_sb->node);
    }

    start_x += tw + gap * fit;
}
```

#### Deactivation (replace lines 118–125)

Destroy slot trees instead of individual children:

```c
wl_list_for_each_safe(t, tmp, &state.thumbnails, link) {
    wlr_scene_node_destroy(&t->slot->node);  // cascades to card, border, sb, badge
    wl_list_remove(&t->link);
    free(t);
}
```

No need to manually destroy `t->sb`, `t->badge_sb` — slot tree destroy handles
all descendants.

## Files Touched

| File | Change |
|------|--------|
| `sway/desktop/overview.c` | Add `slot` field to struct; wrap thumbnail in slot tree; position slot node |

Not touching `view.h`, `view.c`, `xdg_shell.c`, `xwayland.c` — already done.
