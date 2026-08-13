#define _POSIX_C_SOURCE 200809L
#include "sway/desktop/overview.h"
#include "log.h"
#include "sway/config.h"
#include "sway/desktop/transaction.h"
#include "sway/input/cursor.h"
#include "sway/input/seat.h"
#include "sway/output.h"
#include "sway/scene_descriptor.h"
#include "sway/server.h"
#include "sway/tree/arrange.h"
#include "sway/tree/container.h"
#include "sway/tree/root.h"
#include "sway/tree/workspace.h"
#include <cairo.h>
#include <drm_fourcc.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/pass.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>

struct overview_thumbnail {
  struct wl_list link;
  struct wlr_scene_buffer *sb;
  struct wlr_scene_buffer *badge_sb;
  struct sway_container *con;
  struct sway_workspace *ws;
  enum overview_action action;
  int w, h;
  float origin_x, origin_y;
};

static bool overview_active = false;

struct overview_divider {
  struct wlr_scene_rect *rect;
  struct wl_list link;
};

static struct {
  struct wlr_scene_rect *bg;
  struct wlr_scene_rect *hover_rect;
  struct wl_list thumbnails;
  struct wl_list dividers;
  int n_thumbnails;
  int digit_buf;
  int digit_count;
  struct sway_container *focus_con;
  struct sway_seat *seat;
  enum overview_scope scope;
  enum overview_action action;
  int content;
} state;

static void overview_get_origin(struct sway_container *con,
                                struct sway_output *output,
                                struct sway_workspace *active_ws,
                                float *ox, float *oy);

static bool overview_thumbnail_create_view(struct sway_container *con,
                                      struct sway_workspace *ws,
                                      struct sway_output *output,
                                      struct sway_workspace *active_ws,
                                      struct wlr_renderer *renderer,
                                      struct wlr_allocator *alloc,
                                      const struct wlr_drm_format *fmt,
                                      float scale, int bt, int idx,
                                      enum overview_action action);

static void overview_thumbnail_finalize(struct wlr_buffer *buf,
                                       struct wlr_swapchain *sc, int scw, int sch,
                                       struct sway_container *con,
                                       struct sway_workspace *ws,
                                       struct sway_output *output,
                                       struct sway_workspace *active_ws,
                                       struct wlr_renderer *renderer,
                                       struct wlr_allocator *alloc,
                                       const struct wlr_drm_format *fmt,
                                       float scale, int bt, int idx,
                                       enum overview_action action);

static bool overview_thumbnail_create_column(struct sway_container *con,
                                       struct sway_workspace *ws,
                                       struct sway_output *output,
                                       struct sway_workspace *active_ws,
                                       struct wlr_renderer *renderer,
                                       struct wlr_allocator *alloc,
                                       const struct wlr_drm_format *fmt,
                                       float scale, int bt, int idx,
                                       enum overview_action action);

static bool overview_thumbnail_create(struct sway_container *con,
                                      struct sway_workspace *ws,
                                      struct sway_output *output,
                                      struct sway_workspace *active_ws,
                                      struct wlr_renderer *renderer,
                                      struct wlr_allocator *alloc,
                                      const struct wlr_drm_format *fmt,
                                      float scale, int bt, int idx,
                                      enum overview_action action) {
  if (con->view) {
    if (!con->view->saved_buffer)
      return false;
    return overview_thumbnail_create_view(con, ws, output, active_ws,
        renderer, alloc, fmt, scale, bt, idx, action);
  }
  return overview_thumbnail_create_column(con, ws, output, active_ws,
      renderer, alloc, fmt, scale, bt, idx, action);
}

static bool overview_thumbnail_create_view(struct sway_container *con,
                                      struct sway_workspace *ws,
                                      struct sway_output *output,
                                      struct sway_workspace *active_ws,
                                      struct wlr_renderer *renderer,
                                      struct wlr_allocator *alloc,
                                      const struct wlr_drm_format *fmt,
                                      float scale, int bt, int idx,
                                      enum overview_action action) {
  if (!con->view->saved_buffer)
    return false;
  struct wlr_buffer *saved_buf = con->view->saved_buffer;
  struct wlr_client_buffer *cb = wlr_client_buffer_get(saved_buf);
  if (!cb || !cb->texture) {
    sway_log(SWAY_INFO, "OVERVIEW:   skip idx=%d cb=%p tex=%p",
             idx, cb, cb ? cb->texture : NULL);
    return false;
  }

  int bw = saved_buf->width;
  int bh = saved_buf->height;
  int scw = bw + 2 * bt;
  int sch = bh + 2 * bt;

  sway_log(SWAY_INFO, "OVERVIEW:   thumbnail idx=%d buf=%dx%d card=%dx%d",
           idx, bw, bh, scw, sch);

  struct wlr_swapchain *sc = wlr_swapchain_create(alloc, scw, sch, fmt);
  if (!sc) {
    sway_log(SWAY_ERROR, "OVERVIEW:   swapchain create FAILED");
    return false;
  }
  struct wlr_buffer *buf = wlr_swapchain_acquire(sc);
  if (!buf) {
    sway_log(SWAY_ERROR, "OVERVIEW:   swapchain acquire FAILED");
    wlr_swapchain_destroy(sc);
    return false;
  }

  struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
      renderer, buf, &(struct wlr_buffer_pass_options){0});
  if (!pass) {
    sway_log(SWAY_ERROR, "OVERVIEW:   begin_buffer_pass FAILED");
    wlr_buffer_unlock(buf);
    wlr_swapchain_destroy(sc);
    return false;
  }

  wlr_render_pass_add_rect(
      pass, &(struct wlr_render_rect_options){
                .box = {.x = 0, .y = 0, .width = scw, .height = sch},
                .color = {0.05, 0.05, 0.1, 1},
            });

  if (bt > 0) {
    float col[4] = {
        config->border_colors.unfocused.border[0],
        config->border_colors.unfocused.border[1],
        config->border_colors.unfocused.border[2],
        config->border_colors.unfocused.border[3],
    };
    wlr_render_pass_add_rect(
        pass, &(struct wlr_render_rect_options){
                  .box = {.x = 0, .y = 0, .width = scw, .height = sch},
                  .color = {col[0], col[1], col[2], col[3]},
              });
  }

  float alpha = 1.0f;
  wlr_render_pass_add_texture(
      pass, &(struct wlr_render_texture_options){
                .texture = cb->texture,
                .dst_box = {.x = bt, .y = bt,
                            .width = bw, .height = bh},
                .transform = WL_OUTPUT_TRANSFORM_NORMAL,
                .alpha = &alpha,
            });

  if (!wlr_render_pass_submit(pass)) {
    sway_log(SWAY_ERROR, "OVERVIEW:   render pass submit FAILED");
    wlr_buffer_unlock(buf);
    wlr_swapchain_destroy(sc);
    return false;
  }

  overview_thumbnail_finalize(buf, sc, scw, sch, con, ws, output,
      active_ws, renderer, alloc, fmt, scale, bt, idx, action);
  return true;
}

static void overview_thumbnail_finalize(struct wlr_buffer *buf,
      struct wlr_swapchain *sc, int scw, int sch,
      struct sway_container *con, struct sway_workspace *ws,
      struct sway_output *output, struct sway_workspace *active_ws,
      struct wlr_renderer *renderer, struct wlr_allocator *alloc,
      const struct wlr_drm_format *fmt, float scale, int bt, int idx,
      enum overview_action action) {
  struct overview_thumbnail *t = calloc(1, sizeof(*t));
  if (!t) {
    sway_log(SWAY_ERROR, "OVERVIEW:   thumb alloc FAILED");
    wlr_buffer_unlock(buf);
    wlr_swapchain_destroy(sc);
    return;
  }
  t->w = scw;
  t->h = sch;
  overview_get_origin(con, output, active_ws, &t->origin_x, &t->origin_y);
  t->con = con;
  t->ws = ws;
  t->action = action;

  int badge_w = (int)(48 * scale);
  int badge_h = (int)(48 * scale);
  struct wlr_swapchain *badge_sc =
      wlr_swapchain_create(alloc, badge_w, badge_h, fmt);
  struct wlr_buffer *badge_buf = NULL;
  if (badge_sc) {
    badge_buf = wlr_swapchain_acquire(badge_sc);
  }
  if (badge_buf) {
    struct wlr_render_pass *badge_pass = wlr_renderer_begin_buffer_pass(
        renderer, badge_buf, &(struct wlr_buffer_pass_options){0});
    if (badge_pass) {
      char label[16];
      snprintf(label, sizeof(label), "%02d", idx);

      cairo_surface_t *surf =
          cairo_image_surface_create(CAIRO_FORMAT_ARGB32, badge_w, badge_h);
      cairo_t *cr = cairo_create(surf);

      cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
      cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.7);
      cairo_rectangle(cr, 0, 0, badge_w, badge_h);
      cairo_fill(cr);

      cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
      cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                             CAIRO_FONT_WEIGHT_BOLD);
      cairo_set_font_size(cr, 32 * scale);

      cairo_text_extents_t ext;
      cairo_text_extents(cr, label, &ext);
      cairo_move_to(cr, (badge_w - ext.width) / 2.0 - ext.x_bearing,
                    (badge_h - ext.height) / 2.0 - ext.y_bearing);
      cairo_show_text(cr, label);
      cairo_destroy(cr);

      struct wlr_texture *tex = wlr_texture_from_pixels(
          renderer, DRM_FORMAT_ARGB8888, cairo_image_surface_get_stride(surf),
          badge_w, badge_h, cairo_image_surface_get_data(surf));
      cairo_surface_destroy(surf);

      if (tex) {
        float alpha = 1.0f;
        wlr_render_pass_add_texture(
            badge_pass, &(struct wlr_render_texture_options){
                            .texture = tex,
                            .dst_box = {.x = 0,
                                        .y = 0,
                                        .width = badge_w,
                                        .height = badge_h},
                            .transform = WL_OUTPUT_TRANSFORM_NORMAL,
                            .alpha = &alpha,
                        });
        wlr_texture_destroy(tex);
      }
      wlr_render_pass_submit(badge_pass);
    }
    t->badge_sb = wlr_scene_buffer_create(root->layers.overview, badge_buf);
    wlr_buffer_unlock(badge_buf);
  } else {
    t->badge_sb = NULL;
  }
  if (badge_sc)
    wlr_swapchain_destroy(badge_sc);

  t->sb = wlr_scene_buffer_create(root->layers.overview, buf);
  wlr_buffer_unlock(buf);
  wlr_swapchain_destroy(sc);

  if (t->sb) {
    wl_list_insert(state.thumbnails.prev, &t->link);
    state.n_thumbnails++;
  } else {
    if (t->badge_sb)
      wlr_scene_node_destroy(&t->badge_sb->node);
    free(t);
  }
}

struct overview_col_view {
  struct wlr_client_buffer *cb;
  int w, h;
};

static void overview_collect_column_views(struct sway_container *c,
    struct overview_col_view *out, int *n, int max) {
  if (*n >= max)
    return;
  if (c->view) {
    if (c->view->saved_buffer) {
      struct wlr_client_buffer *cb =
          wlr_client_buffer_get(c->view->saved_buffer);
      if (cb && cb->texture) {
        out[*n].cb = cb;
        out[*n].w = c->view->saved_buffer->width;
        out[*n].h = c->view->saved_buffer->height;
        (*n)++;
      }
    }
    return;
  }
  for (int i = 0; i < c->current.children->length; i++) {
    overview_collect_column_views(c->current.children->items[i],
        out, n, max);
  }
}

static bool overview_thumbnail_create_column(struct sway_container *con,
                                      struct sway_workspace *ws,
                                      struct sway_output *output,
                                      struct sway_workspace *active_ws,
                                      struct wlr_renderer *renderer,
                                      struct wlr_allocator *alloc,
                                      const struct wlr_drm_format *fmt,
                                      float scale, int bt, int idx,
                                      enum overview_action action) {
  #define MAX_COL_VIEW 64
  struct overview_col_view cv[MAX_COL_VIEW];
  int n = 0;
  overview_collect_column_views(con, cv, &n, MAX_COL_VIEW);
  if (n == 0)
    return false;

  int pad = bt;
  int gap = bt;
  int inner_w = 0;
  int comp_h = 2 * pad;
  for (int i = 0; i < n; i++) {
    if (cv[i].w > inner_w)
      inner_w = cv[i].w;
    comp_h += cv[i].h;
    if (i + 1 < n)
      comp_h += gap;
  }
  int comp_w = inner_w + 2 * pad;
  if (comp_w <= 0 || comp_h <= 0)
    return false;

  struct wlr_swapchain *sc = wlr_swapchain_create(alloc, comp_w, comp_h, fmt);
  if (!sc)
    return false;
  struct wlr_buffer *buf = wlr_swapchain_acquire(sc);
  if (!buf) {
    wlr_swapchain_destroy(sc);
    return false;
  }

  struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
      renderer, buf, &(struct wlr_buffer_pass_options){0});
  if (!pass) {
    wlr_swapchain_destroy(sc);
    wlr_buffer_unlock(buf);
    return false;
  }

  wlr_render_pass_add_rect(
      pass, &(struct wlr_render_rect_options){
                .box = {.x = 0, .y = 0, .width = comp_w, .height = comp_h},
                .color = {0.05, 0.05, 0.1, 1},
            });

  if (bt > 0) {
    float col[4] = {
        config->border_colors.unfocused.border[0],
        config->border_colors.unfocused.border[1],
        config->border_colors.unfocused.border[2],
        config->border_colors.unfocused.border[3],
    };
    wlr_render_pass_add_rect(
        pass, &(struct wlr_render_rect_options){
                  .box = {.x = 0, .y = 0, .width = comp_w, .height = comp_h},
                  .color = {col[0], col[1], col[2], col[3]},
              });
  }

  int y = pad;
  for (int i = 0; i < n; i++) {
    int x = pad + (inner_w - cv[i].w) / 2;
    float alpha = 1.0f;
    wlr_render_pass_add_texture(
        pass, &(struct wlr_render_texture_options){
                  .texture = cv[i].cb->texture,
                  .dst_box = {.x = x, .y = y,
                              .width = cv[i].w, .height = cv[i].h},
                  .transform = WL_OUTPUT_TRANSFORM_NORMAL,
                  .alpha = &alpha,
                });
    y += cv[i].h + gap;
  }

  if (!wlr_render_pass_submit(pass)) {
    wlr_swapchain_destroy(sc);
    wlr_buffer_unlock(buf);
    return false;
  }

  overview_thumbnail_finalize(buf, sc, comp_w, comp_h, con, ws, output,
      active_ws, renderer, alloc, fmt, scale, bt, idx, action);
  return true;
}

static void overview_get_origin(struct sway_container *con,
                                struct sway_output *output,
                                struct sway_workspace *active_ws,
                                float *ox, float *oy) {
  struct wlr_box sbox;
  container_get_screen_box(con, &sbox);
  *ox = sbox.x;
  *oy = sbox.y;
}

static void overview_collect(struct sway_container *con,
                             struct sway_workspace *ws,
                             struct sway_output *output,
                             struct sway_workspace *active_ws,
                             struct wlr_renderer *renderer,
                             struct wlr_allocator *alloc,
                             const struct wlr_drm_format *fmt,
                             float scale, int bt, int *idx) {
  // One tile per top-level container (column or floating window).
  if (!con->view) {
    if (overview_thumbnail_create(con, ws, output, active_ws,
                                  renderer, alloc, fmt, scale, bt, *idx + 1,
                                  state.action)) {
      *idx = *idx + 1;
    }
    return;
  }
  if (con->view->saved_buffer) {
    (*idx)++;
    overview_thumbnail_create(con, ws, output, active_ws,
                              renderer, alloc, fmt, scale, bt, *idx,
                              state.action);
  }
}

static void overview_collect_minimized(struct sway_output *output,
                                       struct wlr_renderer *renderer,
                                       struct wlr_allocator *alloc,
                                       const struct wlr_drm_format *fmt,
                                       float scale, int bt, int *con_idx) {
  // Dedicated minimize overview: show every minimized window across all
  // workspaces (a global catch-all). Restoring is handled by
  // overview_action_restore, which adds the window back to the focused
  // workspace. Pass the source workspace so t->ws is set for focus switching.
  for (int oi = 0; oi < root->outputs->length; oi++) {
    struct sway_output *o = root->outputs->items[oi];
    for (int wi = 0; wi < o->workspaces->length; wi++) {
      struct sway_workspace *ws = o->workspaces->items[wi];
      for (int i = 0; i < ws->minimized->length; i++) {
        struct sway_container *con = ws->minimized->items[i];
        // Columns (no view) are composited from their child windows; windows
        // (with a view) are rendered directly. overview_thumbnail_create
        // returns false when there is nothing to show, so only count
        // successful tiles.
        int prev = *con_idx;
        if (overview_thumbnail_create(con, ws, output, NULL,
                                      renderer, alloc, fmt, scale, bt,
                                      prev + 1, OVERVIEW_RESTORE)) {
          *con_idx = prev + 1;
        }
      }
    }
  }
}

bool overview_is_active(void) { return overview_active; }

void overview_set_params(enum overview_scope scope,
		enum overview_action action, int content_flags) {
	state.scope = scope;
	state.action = action;
	// Default to showing everything for non-minimized scopes when no flags
	// are supplied.
	if (content_flags == 0 && scope != OVERVIEW_MINIMIZED) {
		content_flags = OVERVIEW_CONTENT_TILED | OVERVIEW_CONTENT_FLOATING |
			OVERVIEW_CONTENT_MINIMIZED;
	}
	state.content = content_flags;
}



static void overview_action_focus(struct overview_thumbnail *t) {
  if (!t || !t->con)
    return;
  struct sway_container *target = container_toplevel_ancestor(t->con);
  if (t->ws && t->ws != output_get_active_workspace(root->outputs->items[0])) {
    workspace_switch(t->ws);
  }
  seat_set_focus_container(state.seat, target);
  transaction_commit_dirty();
}

static void overview_action_pull(struct overview_thumbnail *t) {
  if (!t || !t->con)
    return;
  struct sway_container *target = container_toplevel_ancestor(t->con);
  struct sway_workspace *active_ws = output_get_active_workspace(root->outputs->items[0]);
  if (container_is_floating(target)) {
    // Floating windows aren't columns: relocate the window to the focused
    // workspace (where your focus/cursor is).
    struct sway_workspace *src = target->pending.workspace;
    if (src != active_ws) {
      container_detach(target);
      workspace_add_floating(active_ws, target);
    }
    seat_set_focus_container(state.seat, target);
    transaction_commit_dirty();
    return;
  }
  struct sway_container *target_col = target;
  struct sway_container *focus = state.focus_con;
  if (!focus) {
    seat_set_focus_container(state.seat, target_col);
    transaction_commit_dirty();
    return;
  }
  struct sway_container *focus_col = container_toplevel_ancestor(focus);
  if (focus_col != target_col) {
    int fi = list_find(active_ws->tiling, focus_col);
    if (fi >= 0) {
      workspace_pull_column(active_ws, target_col, fi + 1);
    }
  }
  seat_set_focus_container(state.seat, target_col);
  transaction_commit_dirty();
}

static void overview_action_swap(struct overview_thumbnail *t) {
  if (!t || !t->con)
    return;
  struct sway_container *focus = state.focus_con;
  if (!focus) {
    seat_set_focus_container(state.seat, container_toplevel_ancestor(t->con));
    return;
  }
  struct sway_container *focus_top = container_toplevel_ancestor(focus);
  struct sway_container *target = container_toplevel_ancestor(t->con);
  // Only swap within the same type (tiled<->tiled, floating<->floating).
  // The overview already filters by focus type in swap mode; this guards
  // against any mismatch (and clicking the focused tile itself).
  if (focus_top == target) {
    return;
  }
  bool focus_float = container_is_floating(focus_top);
  bool target_float = container_is_floating(target);
  if (focus_float != target_float) {
    return;
  }
  if (focus_float) {
    workspace_swap_floating(focus_top, target);
  } else {
    workspace_swap_columns(focus_top, target);
  }
  seat_set_focus_raw(state.seat, &target->node);
  struct sway_workspace *ws_a = focus_top->pending.workspace;
  struct sway_workspace *ws_b = target->pending.workspace;
  arrange_workspace(ws_a);
  if (ws_b != ws_a) arrange_workspace(ws_b);
  transaction_commit_dirty();
}

static void overview_action_restore(struct overview_thumbnail *t) {
  if (!t->con)
    return;
  // Remove from its workspace pool and add to the focused workspace (just
  // like pull/focus/swap).
  root_minimized_show(t->con);
  transaction_commit_dirty();
}

static struct overview_thumbnail *overview_thumbnail_at(double x, double y) {
  struct overview_thumbnail *t;
  wl_list_for_each(t, &state.thumbnails, link) {
    if (!t->sb)
      continue;
    int tx, ty;
    wlr_scene_node_coords(&t->sb->node, &tx, &ty);
    int dw = t->sb->dst_width;
    int dh = t->sb->dst_height;
    if (x >= tx && x <= tx + dw && y >= ty && y <= ty + dh)
      return t;
  }
  return NULL;
}

void overview_handle_button(struct sway_seat *seat, uint32_t button,
    bool pressed) {
  if (!pressed)
    return;
  if (button == BTN_RIGHT) {
    overview_toggle();
    return;
  }
  if (button != BTN_LEFT)
    return;
  double cx = seat->cursor->cursor->x;
  double cy = seat->cursor->cursor->y;
  struct overview_thumbnail *t = overview_thumbnail_at(cx, cy);
  if (t) {
    switch (t->action) {
      case OVERVIEW_FOCUS:
        overview_action_focus(t);
        break;
      case OVERVIEW_PULL:
        overview_action_pull(t);
        break;
      case OVERVIEW_SWAP:
        overview_action_swap(t);
        break;
      case OVERVIEW_RESTORE:
        overview_action_restore(t);
        break;
    }
    overview_toggle();
  }
}

void overview_handle_motion(struct sway_seat *seat) {
  if (!overview_is_active() || !state.hover_rect)
    return;
  double cx = seat->cursor->cursor->x;
  double cy = seat->cursor->cursor->y;
  struct overview_thumbnail *t = overview_thumbnail_at(cx, cy);
  if (t) {
    int tx, ty;
    wlr_scene_node_coords(&t->sb->node, &tx, &ty);
    int dw = t->sb->dst_width;
    int dh = t->sb->dst_height;
    int pad = 4;
    wlr_scene_rect_set_size(state.hover_rect, dw + 2 * pad, dh + 2 * pad);
    wlr_scene_node_set_position(&state.hover_rect->node, tx - pad, ty - pad);
    wlr_scene_node_set_enabled(&state.hover_rect->node, true);
  } else {
    wlr_scene_node_set_enabled(&state.hover_rect->node, false);
  }
}

bool overview_handle_key(xkb_keysym_t sym) {
  if (!overview_active)
    return false;

  if (sym >= XKB_KEY_0 && sym <= XKB_KEY_9) {
    state.digit_buf = state.digit_buf * 10 + (sym - XKB_KEY_0);
    state.digit_count++;
    if (state.digit_count >= 2) {
      int idx = state.digit_buf;
      struct sway_container *focus = state.focus_con;
      state.digit_buf = 0;
      state.digit_count = 0;
      if (idx >= 1 && idx <= state.n_thumbnails && focus) {
        struct overview_thumbnail *t;
        int i = 0;
        wl_list_for_each(t, &state.thumbnails, link) {
          if (i == idx - 1)
            break;
          i++;
        }
        if (t && t->con && t->con != focus) {
          switch (t->action) {
            case OVERVIEW_FOCUS:
              overview_action_focus(t);
              break;
            case OVERVIEW_PULL:
              overview_action_pull(t);
              break;
            case OVERVIEW_SWAP:
              overview_action_swap(t);
              break;
            case OVERVIEW_RESTORE:
              overview_action_restore(t);
              break;
          }
        }
      }
      overview_toggle();
    }
    return true;
  }

  if (sym == XKB_KEY_Escape) {
    overview_toggle();
    return true;
  }

  if (sym == XKB_KEY_BackSpace && state.digit_count > 0) {
    state.digit_buf /= 10;
    state.digit_count--;
    return true;
  }

  state.digit_buf = 0;
  state.digit_count = 0;
  return false;
}

static void overview_collect_workspace(struct sway_workspace *ws,
                                       struct sway_output *output,
                                       struct sway_workspace *active_ws,
                                       struct wlr_renderer *renderer,
                                       struct wlr_allocator *alloc,
                                       const struct wlr_drm_format *fmt,
                                       float scale, int bt, int *con_idx,
                                       int content) {
  struct wlr_scene_node *top_node;
  // Tiled columns.
  if (content & OVERVIEW_CONTENT_TILED) {
    wl_list_for_each(top_node, &ws->layers.tiling->children, link) {
      struct sway_container *top = scene_descriptor_try_get(top_node,
          SWAY_SCENE_DESC_CONTAINER);
      if (top) {
        overview_collect(top, ws, output, active_ws,
                         renderer, alloc, fmt, scale, bt, con_idx);
      }
    }
  }
  // Floating windows.
  if (content & OVERVIEW_CONTENT_FLOATING) {
    wl_list_for_each(top_node, &ws->layers.floating->children, link) {
      struct sway_container *top = scene_descriptor_try_get(top_node,
          SWAY_SCENE_DESC_CONTAINER);
      if (top) {
        overview_collect(top, ws, output, active_ws,
                         renderer, alloc, fmt, scale, bt, con_idx);
      }
    }
  }
  // Minimized windows parked in this workspace's pool.
  if (content & OVERVIEW_CONTENT_MINIMIZED) {
    for (int i = 0; i < ws->minimized->length; i++) {
      struct sway_container *con = ws->minimized->items[i];
      int prev = *con_idx;
      if (overview_thumbnail_create(con, ws, output, active_ws,
                                    renderer, alloc, fmt, scale, bt,
                                    prev + 1, OVERVIEW_RESTORE)) {
        *con_idx = prev + 1;
      }
    }
  }
}

static void overview_layout_grid(struct sway_output *output) {
  int n = state.n_thumbnails;
  float scale = output->wlr_output->scale;
  int ow = (int)(output->wlr_output->width / scale);
  int oh = (int)(output->wlr_output->height / scale);
  int ox = (int)(output->scene_output->x);
  int oy = (int)(output->scene_output->y);

  if (n == 0) {
    wlr_scene_node_set_enabled(&root->layers.overview->node, true);
    overview_active = true;
    return;
  }

  /* Clear any dividers left from a previous layout pass. */
  struct overview_divider *d, *dtmp;
  wl_list_for_each_safe(d, dtmp, &state.dividers, link) {
    wlr_scene_node_destroy(&d->rect->node);
    wl_list_remove(&d->link);
    free(d);
  }

  /* Bucket thumbnails by workspace, preserving collection order. Within a
   * workspace the order is tiled -> floating -> minimized. */
  #define MAX_SEC 256
  struct overview_section {
    struct sway_workspace *ws;
    struct overview_thumbnail **tiles;
    int count;
  } secs[MAX_SEC];
  int n_sec = 0;

  struct overview_thumbnail *t;
  wl_list_for_each(t, &state.thumbnails, link) {
    int si = -1;
    for (int i = 0; i < n_sec; i++) {
      if (secs[i].ws == t->ws) {
        si = i;
        break;
      }
    }
    if (si == -1) {
      if (n_sec >= MAX_SEC) continue;
      si = n_sec++;
      secs[si].ws = t->ws;
      secs[si].count = 0;
      secs[si].tiles = NULL;
    }
    secs[si].tiles = realloc(secs[si].tiles,
        sizeof(struct overview_thumbnail *) * (secs[si].count + 1));
    secs[si].tiles[secs[si].count++] = t;
  }

  float avail_w = ow * 0.9f;
  float avail_h = oh * 0.85f;
  float base_x = ox + (ow - avail_w) / 2.0f;
  float base_y = oy + (oh - avail_h) / 2.0f;
  float tile_gap = 16.0f;

  float sec_gap = 24.0f;
  float slice_h = avail_h;
  if (n_sec > 1) {
    slice_h = (avail_h - sec_gap * (n_sec - 1)) / (float)n_sec;
    if (slice_h < 1) slice_h = avail_h;
  }

  for (int si = 0; si < n_sec; si++) {
    struct overview_section *sec = &secs[si];
    int sn = sec->count;
    if (sn == 0) {
      free(sec->tiles);
      continue;
    }
    int cols = (int)ceilf(sqrtf((float)sn));
    int rows = (int)ceilf((float)sn / (float)cols);
    float cell_w = avail_w / (float)cols;
    float cell_h = slice_h / (float)rows;
    float sec_y = base_y + (float)si * (slice_h + sec_gap);

    for (int i = 0; i < sn; i++) {
      struct overview_thumbnail *tt = sec->tiles[i];
      int r = i / cols;
      int c = i % cols;
      float tw = (float)tt->w / scale;
      float th = (float)tt->h / scale;
      float fit = fminf((cell_w - tile_gap) / tw, (cell_h - tile_gap) / th);
      tw = tw * fit;
      th = th * fit;
      float cx = base_x + (float)c * cell_w + (cell_w - tw) / 2.0f;
      float cy = sec_y + (float)r * cell_h + (cell_h - th) / 2.0f;

      wlr_scene_buffer_set_dest_size(tt->sb, (int)tw, (int)th);
      wlr_scene_node_set_position(&tt->sb->node, (int)cx, (int)cy);

      if (tt->badge_sb) {
        int bsz = (int)(48 * fit);
        if (bsz < 28) bsz = 28;
        int bpad = (int)(2 * fit);
        if (bpad < 1) bpad = 1;
        wlr_scene_buffer_set_dest_size(tt->badge_sb, bsz, bsz);
        wlr_scene_node_set_position(&tt->badge_sb->node,
            (int)cx + bpad, (int)cy + bpad);
        wlr_scene_node_raise_to_top(&tt->badge_sb->node);
      }
    }
    free(sec->tiles);
  }

  /* Thin divider lines between workspace sections. */
  for (int si = 0; si + 1 < n_sec; si++) {
    float dy = base_y + (si + 1) * (slice_h + sec_gap) - sec_gap / 2.0f;
    struct wlr_scene_rect *r = wlr_scene_rect_create(
        root->layers.overview, (int)avail_w, 2,
        (float[4]){0.55f, 0.55f, 0.55f, 0.5f});
    if (r) {
      wlr_scene_node_set_position(&r->node, (int)base_x, (int)(dy - 1));
      struct overview_divider *nd = calloc(1, sizeof(*nd));
      if (nd) {
        nd->rect = r;
        wl_list_insert(&state.dividers, &nd->link);
      } else {
        wlr_scene_node_destroy(&r->node);
      }
    }
  }

  wlr_scene_node_set_enabled(&root->layers.overview->node, true);
  overview_active = true;
}


static void overview_teardown(void) {
  struct overview_thumbnail *t, *tmp;
  wl_list_for_each_safe(t, tmp, &state.thumbnails, link) {
    if (t->sb) {
      wlr_scene_node_destroy(&t->sb->node);
    }
    if (t->badge_sb) {
      wlr_scene_node_destroy(&t->badge_sb->node);
    }
    wl_list_remove(&t->link);
    free(t);
  }
  struct overview_divider *d, *dtmp;
  wl_list_for_each_safe(d, dtmp, &state.dividers, link) {
    wlr_scene_node_destroy(&d->rect->node);
    wl_list_remove(&d->link);
    free(d);
  }
  state.n_thumbnails = 0;
  if (state.bg) {
    wlr_scene_node_destroy(&state.bg->node);
    state.bg = NULL;
  }
  if (state.hover_rect) {
    wlr_scene_node_destroy(&state.hover_rect->node);
    state.hover_rect = NULL;
  }
  wlr_scene_node_set_enabled(&root->layers.overview->node, false);
  overview_active = false;
}

static bool overview_setup(struct sway_output *output) {
  state.seat = input_manager_current_seat();
  state.focus_con = seat_get_focused_container(state.seat);
  state.digit_buf = 0;
  state.digit_count = 0;

  float scale = output->wlr_output->scale;
  int ow = (int)(output->wlr_output->width / scale);
  int oh = (int)(output->wlr_output->height / scale);
  int ox = (int)(output->scene_output->x);
  int oy = (int)(output->scene_output->y);

  // For the "all" overview, dim every monitor so the whole desktop reads as
  // overview mode; otherwise dim only the (primary) output we render on.
  if (state.scope == OVERVIEW_ALL && root->outputs->length > 1) {
    float min_x = INFINITY, min_y = INFINITY;
    float max_x = -INFINITY, max_y = -INFINITY;
    for (int oi = 0; oi < root->outputs->length; oi++) {
      struct sway_output *o = root->outputs->items[oi];
      float osc = o->wlr_output->scale;
      int oow = (int)(o->wlr_output->width / osc);
      int ooh = (int)(o->wlr_output->height / osc);
      int oox = (int)(o->scene_output->x);
      int ooy = (int)(o->scene_output->y);
      if (oox < min_x) min_x = oox;
      if (ooy < min_y) min_y = ooy;
      if (oox + oow > max_x) max_x = oox + oow;
      if (ooy + ooh > max_y) max_y = ooy + ooh;
    }
    ox = (int)min_x;
    oy = (int)min_y;
    ow = (int)(max_x - min_x);
    oh = (int)(max_y - min_y);
  }

  state.bg = wlr_scene_rect_create(root->layers.overview, ow, oh,
                                    (float[4]){0.0, 0.0, 0.0, 0.6});
  if (state.bg) {
    wlr_scene_node_set_position(&state.bg->node, ox, oy);
  }

  // Highlight ring shown behind the hovered thumbnail (created before the
  // thumbnails so it sits beneath them and reads as a border outline).
  state.hover_rect = wlr_scene_rect_create(root->layers.overview, 0, 0,
      config->border_colors.focused.border);
  if (state.hover_rect) {
    wlr_scene_node_set_enabled(&state.hover_rect->node, false);
  }

  wl_list_init(&state.thumbnails);
  wl_list_init(&state.dividers);
  state.n_thumbnails = 0;
  return true;
}

void overview_toggle(void) {
  struct sway_output *output = NULL;
  if (root && root->outputs && root->outputs->length > 0) {
    output = root->outputs->items[0];
  }
  if (!output) return;

  if (overview_active) { overview_teardown(); return; }

  struct sway_workspace *active_ws = output_get_active_workspace(output);
  if (!active_ws) return;

  if (!overview_setup(output)) return;

  float scale = output->wlr_output->scale;
  struct wlr_renderer *renderer = output->wlr_output->renderer;
  struct wlr_allocator *alloc = output->wlr_output->allocator;
  const struct wlr_drm_format *fmt = output->wlr_output->swapchain
      ? &output->wlr_output->swapchain->format : NULL;
  if (!fmt) return;

  int bt = 0;
  if (config->border == B_PIXEL || config->border == B_NORMAL) {
    bt = (int)(config->border_thickness * scale);
  }

  int con_idx = 0;
  int content = state.content;
  // In swap mode, only show tiles matching the focused container's type so the
  // overview presents valid swap partners (tiled-only / floating-only).
  if (state.action == OVERVIEW_SWAP && state.focus_con) {
    content = container_is_floating(container_toplevel_ancestor(state.focus_con))
        ? OVERVIEW_CONTENT_FLOATING : OVERVIEW_CONTENT_TILED;
  }
  if (state.scope == OVERVIEW_MINIMIZED) {
    overview_collect_minimized(output, renderer, alloc, fmt,
                               scale, bt, &con_idx);
    overview_layout_grid(output);
  } else if (state.scope == OVERVIEW_ALL) {
    for (int oi = 0; oi < root->outputs->length; oi++) {
      struct sway_output *o = root->outputs->items[oi];
      float osc = o->wlr_output->scale;
      for (int i = 0; i < o->workspaces->length; i++) {
        struct sway_workspace *ws = o->workspaces->items[i];
        overview_collect_workspace(ws, o, active_ws,
                                   renderer, alloc, fmt, osc, bt, &con_idx,
                                   content);
      }
    }
    overview_layout_grid(output);
  } else {
    overview_collect_workspace(active_ws, output, active_ws,
                               renderer, alloc, fmt, scale, bt, &con_idx,
                               content);
    overview_layout_grid(output);
  }
}
