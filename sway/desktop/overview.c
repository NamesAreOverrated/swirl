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
  int w, h;
  float origin_x, origin_y;
};

static bool overview_active = false;

static struct {
  struct wlr_scene_rect *bg;
  struct wlr_scene_rect *hover_rect;
  struct wl_list thumbnails;
  int n_thumbnails;
  int digit_buf;
  int digit_count;
  struct sway_container *focus_con;
  struct sway_seat *seat;
  enum overview_scope scope;
  enum overview_action action;
} state;

static void overview_get_origin(struct sway_container *con,
                                struct sway_output *output,
                                struct sway_workspace *active_ws,
                                float *ox, float *oy);

static void overview_thumbnail_create(struct sway_container *con,
                                      struct sway_workspace *ws,
                                      struct sway_output *output,
                                      struct sway_workspace *active_ws,
                                      struct wlr_renderer *renderer,
                                      struct wlr_allocator *alloc,
                                      const struct wlr_drm_format *fmt,
                                      float scale, int bt, int idx) {
  if (!con->view->saved_buffer)
    return;
  struct wlr_buffer *saved_buf = con->view->saved_buffer;
  struct wlr_client_buffer *cb = wlr_client_buffer_get(saved_buf);
  if (!cb || !cb->texture) {
    sway_log(SWAY_INFO, "OVERVIEW:   skip idx=%d cb=%p tex=%p",
             idx, cb, cb ? cb->texture : NULL);
    return;
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
    return;
  }
  struct wlr_buffer *buf = wlr_swapchain_acquire(sc);
  if (!buf) {
    sway_log(SWAY_ERROR, "OVERVIEW:   swapchain acquire FAILED");
    wlr_swapchain_destroy(sc);
    return;
  }

  struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
      renderer, buf, &(struct wlr_buffer_pass_options){0});
  if (!pass) {
    sway_log(SWAY_ERROR, "OVERVIEW:   begin_buffer_pass FAILED");
    wlr_buffer_unlock(buf);
    wlr_swapchain_destroy(sc);
    return;
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
    return;
  }

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
  if (con->view) {
    if (con->view->saved_buffer) {
      (*idx)++;
      overview_thumbnail_create(con, ws, output, active_ws,
                                renderer, alloc, fmt, scale, bt, *idx);
    }
    return;
  }
  for (int i = 0; i < con->current.children->length; i++) {
    overview_collect(con->current.children->items[i],
                     ws, output, active_ws,
                     renderer, alloc, fmt, scale, bt, idx);
  }
}

static void overview_collect_minimized(struct sway_output *output,
                                       struct wlr_renderer *renderer,
                                       struct wlr_allocator *alloc,
                                       const struct wlr_drm_format *fmt,
                                       float scale, int bt, int *con_idx) {
  for (int i = 0; i < root->minimized->length; i++) {
    struct sway_container *con = root->minimized->items[i];
    if (con->view && con->view->saved_buffer) {
      (*con_idx)++;
      overview_thumbnail_create(con, NULL, output, NULL,
                                renderer, alloc, fmt, scale, bt, *con_idx);
    }
  }
}

bool overview_is_active(void) { return overview_active; }

void overview_set_params(enum overview_scope scope,
		enum overview_action action) {
	state.scope = scope;
	state.action = action;
}



static void overview_action_focus(struct overview_thumbnail *t) {
  if (t->ws != output_get_active_workspace(root->outputs->items[0])) {
    workspace_switch(t->ws);
  }
  seat_set_focus_container(state.seat, t->con);
}

static void overview_action_pull(struct overview_thumbnail *t) {
  struct sway_container *focus = state.focus_con;
  struct sway_workspace *active_ws = output_get_active_workspace(root->outputs->items[0]);
  struct sway_container *focus_col = container_toplevel_ancestor(focus);
  struct sway_container *target_col = container_toplevel_ancestor(t->con);
  if (focus_col != target_col) {
    int fi = list_find(active_ws->tiling, focus_col);
    if (fi >= 0) {
      workspace_pull_column(active_ws, target_col, fi + 1);
    }
  }
  seat_set_focus_container(state.seat, t->con);
}

static void overview_action_swap(struct overview_thumbnail *t) {
  struct sway_container *focus = state.focus_con;
  struct sway_container *focus_col = container_toplevel_ancestor(focus);
  struct sway_container *target_col = container_toplevel_ancestor(t->con);
  if (focus_col != target_col) {
    workspace_swap_columns(focus_col, target_col);
    seat_set_focus_raw(state.seat, &t->con->node);
    struct sway_workspace *ws_a = focus_col->pending.workspace;
    struct sway_workspace *ws_b = target_col->pending.workspace;
    arrange_workspace(ws_a);
    if (ws_b != ws_a) arrange_workspace(ws_b);
    transaction_commit_dirty();
  } else {
    seat_set_focus_container(state.seat, t->con);
  }
}

static void overview_action_restore(struct overview_thumbnail *t) {
  if (!t->con)
    return;
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
    overview_action_restore(t);
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
          switch (state.action) {
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
                                       float scale, int bt, int *con_idx) {
  struct wlr_scene_node *top_node;
  wl_list_for_each(top_node, &ws->layers.tiling->children, link) {
    struct sway_container *top = scene_descriptor_try_get(top_node,
        SWAY_SCENE_DESC_CONTAINER);
    if (top) {
      overview_collect(top, ws, output, active_ws,
                       renderer, alloc, fmt, scale, bt, con_idx);
    }
  }
}

static void overview_layout_and_enable(struct sway_output *output,
                                       bool multi_ws) {
  struct overview_thumbnail *t;
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

  #define MAX_WS 64
  struct {
    struct sway_workspace *ws;
    float min_y, max_y, y_offset;
  } ws_info[MAX_WS];
  int n_ws = 0;

  if (multi_ws) {
    wl_list_for_each(t, &state.thumbnails, link) {
      int wi;
      for (wi = 0; wi < n_ws; wi++) {
        if (ws_info[wi].ws == t->ws) break;
      }
      if (wi == n_ws) {
        if (n_ws >= MAX_WS) continue;
        ws_info[wi].ws = t->ws;
        ws_info[wi].min_y = INFINITY;
        ws_info[wi].max_y = -INFINITY;
        n_ws++;
      }
      float th = (float)t->h / scale;
      if (t->origin_y < ws_info[wi].min_y) ws_info[wi].min_y = t->origin_y;
      if (t->origin_y + th > ws_info[wi].max_y) ws_info[wi].max_y = t->origin_y + th;
    }

    float ws_gap = 16.0f;
    float cur = 0;
    for (int wi = 0; wi < n_ws; wi++) {
      ws_info[wi].y_offset = cur;
      cur += ws_info[wi].max_y - ws_info[wi].min_y + ws_gap;
    }
  }

  float min_x = INFINITY, min_y = INFINITY, max_x = -INFINITY, max_y = -INFINITY;
  wl_list_for_each(t, &state.thumbnails, link) {
    float tx = t->origin_x;
    float tw = (float)t->w / scale;
    float th = (float)t->h / scale;
    if (tx < min_x) min_x = tx;

    float oy = t->origin_y;
    if (multi_ws) {
      for (int wi = 0; wi < n_ws; wi++) {
        if (ws_info[wi].ws == t->ws) {
          oy = ws_info[wi].y_offset + (t->origin_y - ws_info[wi].min_y);
          break;
        }
      }
    }
    if (oy < min_y) min_y = oy;
    if (tx + tw > max_x) max_x = tx + tw;
    if (oy + th > max_y) max_y = oy + th;
  }

  float src_w = max_x - min_x;
  float src_h = max_y - min_y;
  if (src_w <= 0 || src_h <= 0) return;
  float avail_w = (float)ow * 0.9f;
  float avail_h = (float)oh * 0.85f;
  float fit = fminf(1.0f, fminf(avail_w / src_w, avail_h / src_h));
  float dst_w = src_w * fit;
  float dst_h = src_h * fit;
  float base_x = ox + (ow - dst_w) / 2.0f;
  float base_y = oy + (oh - dst_h) / 2.0f;

  wl_list_for_each(t, &state.thumbnails, link) {
    float oy = t->origin_y;
    if (multi_ws) {
      for (int wi = 0; wi < n_ws; wi++) {
        if (ws_info[wi].ws == t->ws) {
          oy = ws_info[wi].y_offset + (t->origin_y - ws_info[wi].min_y);
          break;
        }
      }
    }
    float tw = (float)t->w / scale * fit;
    float th = (float)t->h / scale * fit;
    float tx = base_x + (t->origin_x - min_x) * fit;
    float ty = base_y + (oy - min_y) * fit;

    wlr_scene_buffer_set_dest_size(t->sb, (int)tw, (int)th);
    wlr_scene_node_set_position(&t->sb->node, (int)tx, (int)ty);

    if (t->badge_sb) {
      int bsz = (int)(48 * fit);
      if (bsz < 28) bsz = 28;
      int bpad = (int)(2 * fit);
      if (bpad < 1) bpad = 1;
      wlr_scene_buffer_set_dest_size(t->badge_sb, bsz, bsz);
      wlr_scene_node_set_position(&t->badge_sb->node, (int)tx + bpad,
                                  (int)ty + bpad);
      wlr_scene_node_raise_to_top(&t->badge_sb->node);
    }
  }

  wlr_scene_node_set_enabled(&root->layers.overview->node, true);
  overview_active = true;
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

  int cols = (int)ceilf(sqrtf((float)n));
  int rows = (int)ceilf((float)n / (float)cols);
  float avail_w = ow * 0.9f;
  float avail_h = oh * 0.85f;
  float cell_w = avail_w / (float)cols;
  float cell_h = avail_h / (float)rows;
  float base_x = ox + (ow - avail_w) / 2.0f;
  float base_y = oy + (oh - avail_h) / 2.0f;

  int i = 0;
  struct overview_thumbnail *t;
  wl_list_for_each(t, &state.thumbnails, link) {
    int r = i / cols;
    int c = i % cols;
    float tw = (float)t->w / scale;
    float th = (float)t->h / scale;
    float fit = fminf(cell_w / tw, cell_h / th);
    tw = tw * fit;
    th = th * fit;
    float cx = base_x + (float)c * cell_w + (cell_w - tw) / 2.0f;
    float cy = base_y + (float)r * cell_h + (cell_h - th) / 2.0f;

    wlr_scene_buffer_set_dest_size(t->sb, (int)tw, (int)th);
    wlr_scene_node_set_position(&t->sb->node, (int)cx, (int)cy);

    if (t->badge_sb) {
      int bsz = (int)(48 * fit);
      if (bsz < 28) bsz = 28;
      int bpad = (int)(2 * fit);
      if (bpad < 1) bpad = 1;
      wlr_scene_buffer_set_dest_size(t->badge_sb, bsz, bsz);
      wlr_scene_node_set_position(&t->badge_sb->node, (int)cx + bpad,
                                  (int)cy + bpad);
      wlr_scene_node_raise_to_top(&t->badge_sb->node);
    }
    i++;
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
  if (state.scope == OVERVIEW_MINIMIZED) {
    overview_collect_minimized(output, renderer, alloc, fmt,
                               scale, bt, &con_idx);
    overview_layout_grid(output);
  } else if (state.scope == OVERVIEW_ALL) {
    for (int i = 0; i < output->workspaces->length; i++) {
      struct sway_workspace *ws = output->workspaces->items[i];
      overview_collect_workspace(ws, output, active_ws,
                                 renderer, alloc, fmt, scale, bt, &con_idx);
    }
    overview_layout_and_enable(output, true);
  } else {
    overview_collect_workspace(active_ws, output, active_ws,
                               renderer, alloc, fmt, scale, bt, &con_idx);
    overview_layout_and_enable(output, false);
  }
}
