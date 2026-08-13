#define _POSIX_C_SOURCE 200809L
#include <cairo.h>
#include <drm_fourcc.h>
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
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include "log.h"
#include "sway/config.h"
#include "sway/desktop/overview.h"
#include "sway/desktop/overview_private.h"
#include "sway/output.h"
#include "sway/scene_descriptor.h"
#include "sway/server.h"
#include "sway/tree/container.h"
#include "sway/tree/root.h"
#include "sway/tree/view.h"
#include "sway/tree/workspace.h"

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
    wl_list_insert(overview_state.thumbnails.prev, &t->link);
    overview_state.n_thumbnails++;
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

static void overview_collect(struct sway_container *con,
                             struct sway_workspace *ws,
                             struct sway_output *output,
                             struct sway_workspace *active_ws,
                             struct wlr_renderer *renderer,
                             struct wlr_allocator *alloc,
                             const struct wlr_drm_format *fmt,
                             float scale, int bt, int *idx) {
  // One tile per top-level container (column or floating window). The next
  // thumbnail always gets *idx + 1 and the counter only advances on success,
  // so failed renders never leave a numbered gap.
  int prev = *idx;
  if (con->view) {
    if (con->view->saved_buffer &&
        overview_thumbnail_create(con, ws, output, active_ws,
                                  renderer, alloc, fmt, scale, bt, prev + 1,
                                  overview_state.action)) {
      *idx = prev + 1;
    }
    return;
  }
  if (overview_thumbnail_create(con, ws, output, active_ws,
                                renderer, alloc, fmt, scale, bt, prev + 1,
                                overview_state.action)) {
    *idx = prev + 1;
  }
}

void overview_collect_workspace(struct sway_workspace *ws,
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

void overview_collect_minimized(struct sway_output *output,
                                struct wlr_renderer *renderer,
                                struct wlr_allocator *alloc,
                                const struct wlr_drm_format *fmt,
                                float scale, int bt, int *con_idx) {
  // Dedicated minimize overview: show every minimized window across all
  // workspaces (a global catch-all). Restoration is handled by
  // overview_action_restore, which adds the window back to the focused
  // workspace. Reuses the per-workspace walker restricted to the pool.
  for (int oi = 0; oi < root->outputs->length; oi++) {
    struct sway_output *o = root->outputs->items[oi];
    for (int wi = 0; wi < o->workspaces->length; wi++) {
      overview_collect_workspace(o->workspaces->items[wi], output, NULL,
                                 renderer, alloc, fmt, scale, bt, con_idx,
                                 OVERVIEW_CONTENT_MINIMIZED);
    }
  }
}