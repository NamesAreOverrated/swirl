#define _POSIX_C_SOURCE 200809L
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include "list.h"
#include "log.h"
#include "sway/config.h"
#include "sway/desktop/overview.h"
#include "sway/desktop/overview_private.h"
#include "sway/input/cursor.h"
#include "sway/input/seat.h"
#include "sway/output.h"
#include "sway/server.h"
#include "sway/tree/container.h"
#include "sway/tree/root.h"
#include "sway/tree/workspace.h"

struct overview_state overview_state;

static bool overview_active = false;

bool overview_is_active(void) { return overview_active; }

// Single source of truth for which content a scope/action combination shows.
// Used by the graphical overview (set_params + toggle) and by the IPC target
// gatherer so the two paths can never drift apart.
static int overview_resolve_content(enum overview_scope scope,
		enum overview_action action, int content_flags,
		struct sway_seat *seat) {
	// Default to everything for non-minimized scopes when no flags supplied.
	if (content_flags == 0 && scope != OVERVIEW_MINIMIZED) {
		content_flags = OVERVIEW_CONTENT_TILED | OVERVIEW_CONTENT_FLOATING |
			OVERVIEW_CONTENT_MINIMIZED;
	}
	// Nothing but the minimize pool for a minimized scope.
	if (scope == OVERVIEW_MINIMIZED) {
		content_flags = OVERVIEW_CONTENT_MINIMIZED;
	}
	// Swap only offers same-type live partners (floating XOR tiled) so the
	// picker presents valid swap targets.
	if (action == OVERVIEW_SWAP && seat) {
		struct sway_container *focus = seat_get_focused_container(seat);
		if (focus) {
			content_flags =
				container_is_floating(container_toplevel_ancestor(focus))
				? OVERVIEW_CONTENT_FLOATING : OVERVIEW_CONTENT_TILED;
		}
	}
	// Minimized (parked, workspace-less) containers can only be restored:
	// pulling/focusing a parked container misclassifies it (NULL workspace)
	// and breaks the pool invariant (workspace_swap_columns/pull_column).
	if (action != OVERVIEW_RESTORE) {
		content_flags &= ~OVERVIEW_CONTENT_MINIMIZED;
	}
	return content_flags;
}

void overview_set_params(enum overview_scope scope,
		enum overview_action action, int content_flags) {
	overview_state.scope = scope;
	overview_state.action = action;
	overview_state.content = overview_resolve_content(scope, action,
			content_flags, input_manager_current_seat());
}

// Lightweight counterpart to overview_collect_workspace / overview_collect_minimized:
// instead of building rendered thumbnails, gather the eligible top-level
// containers (with their content type) so non-graphical clients (e.g. the
// cm-swirl script) can present the exact same candidate set the overview would.
static void overview_collect_targets_ws(list_t *out,
		struct sway_workspace *ws, int content) {
	if (content & OVERVIEW_CONTENT_TILED) {
		for (int i = 0; i < ws->tiling->length; i++) {
			struct overview_target *t = malloc(sizeof(*t));
			t->con = ws->tiling->items[i];
			t->type = OVERVIEW_CONTENT_TILED;
			list_add(out, t);
		}
	}
	if (content & OVERVIEW_CONTENT_FLOATING) {
		for (int i = 0; i < ws->floating->length; i++) {
			struct overview_target *t = malloc(sizeof(*t));
			t->con = ws->floating->items[i];
			t->type = OVERVIEW_CONTENT_FLOATING;
			list_add(out, t);
		}
	}
	if (content & OVERVIEW_CONTENT_MINIMIZED) {
		for (int i = 0; i < ws->minimized->length; i++) {
			struct overview_target *t = malloc(sizeof(*t));
			t->con = ws->minimized->items[i];
			t->type = OVERVIEW_CONTENT_MINIMIZED;
			list_add(out, t);
		}
	}
}

void overview_collect_targets(list_t *out, enum overview_scope scope,
		enum overview_action action, int content_flags,
		struct sway_seat *seat) {
	if (!out)
		return;

	content_flags = overview_resolve_content(scope, action, content_flags, seat);

	if (scope == OVERVIEW_CURRENT) {
		struct sway_output *output = root->outputs->length
			? root->outputs->items[0] : NULL;
		struct sway_workspace *ws = output
			? output_get_active_workspace(output) : NULL;
		if (ws)
			overview_collect_targets_ws(out, ws, content_flags);
	} else {
		for (int oi = 0; oi < root->outputs->length; oi++) {
			struct sway_output *o = root->outputs->items[oi];
			for (int i = 0; i < o->workspaces->length; i++) {
				overview_collect_targets_ws(out, o->workspaces->items[i],
					content_flags);
			}
		}
	}
}

static struct overview_thumbnail *overview_thumbnail_at(double x, double y) {
  struct overview_thumbnail *t;
  wl_list_for_each(t, &overview_state.thumbnails, link) {
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
    overview_dispatch_action(t);
    overview_toggle();
  }
}

void overview_handle_motion(struct sway_seat *seat) {
  if (!overview_is_active() || !overview_state.hover_rect)
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
    wlr_scene_rect_set_size(overview_state.hover_rect, dw + 2 * pad, dh + 2 * pad);
    wlr_scene_node_set_position(&overview_state.hover_rect->node, tx - pad, ty - pad);
    wlr_scene_node_set_enabled(&overview_state.hover_rect->node, true);
  } else {
    wlr_scene_node_set_enabled(&overview_state.hover_rect->node, false);
  }
}

bool overview_handle_key(xkb_keysym_t sym) {
  if (!overview_active)
    return false;

  if (sym >= XKB_KEY_0 && sym <= XKB_KEY_9) {
    overview_state.digit_buf = overview_state.digit_buf * 10 + (sym - XKB_KEY_0);
    overview_state.digit_count++;
    if (overview_state.digit_count >= 2) {
      int idx = overview_state.digit_buf;
      struct sway_container *focus = overview_state.focus_con;
      overview_state.digit_buf = 0;
      overview_state.digit_count = 0;
      if (idx >= 1 && idx <= overview_state.n_thumbnails && focus) {
        struct overview_thumbnail *t;
        int i = 0;
        wl_list_for_each(t, &overview_state.thumbnails, link) {
          if (i == idx - 1)
            break;
          i++;
        }
        if (t && t->con && t->con != focus) {
          overview_dispatch_action(t);
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

  if (sym == XKB_KEY_BackSpace && overview_state.digit_count > 0) {
    overview_state.digit_buf /= 10;
    overview_state.digit_count--;
    return true;
  }

  overview_state.digit_buf = 0;
  overview_state.digit_count = 0;
  return false;
}

static void overview_layout_grid(struct sway_output *output) {
  int n = overview_state.n_thumbnails;
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
  wl_list_for_each_safe(d, dtmp, &overview_state.dividers, link) {
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
  wl_list_for_each(t, &overview_state.thumbnails, link) {
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
        wl_list_insert(&overview_state.dividers, &nd->link);
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
  wl_list_for_each_safe(t, tmp, &overview_state.thumbnails, link) {
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
  wl_list_for_each_safe(d, dtmp, &overview_state.dividers, link) {
    wlr_scene_node_destroy(&d->rect->node);
    wl_list_remove(&d->link);
    free(d);
  }
  overview_state.n_thumbnails = 0;
  if (overview_state.bg) {
    wlr_scene_node_destroy(&overview_state.bg->node);
    overview_state.bg = NULL;
  }
  if (overview_state.hover_rect) {
    wlr_scene_node_destroy(&overview_state.hover_rect->node);
    overview_state.hover_rect = NULL;
  }
  wlr_scene_node_set_enabled(&root->layers.overview->node, false);
  overview_active = false;
}

static bool overview_setup(struct sway_output *output) {
  overview_state.seat = input_manager_current_seat();
  overview_state.focus_con = seat_get_focused_container(overview_state.seat);
  overview_state.digit_buf = 0;
  overview_state.digit_count = 0;

  float scale = output->wlr_output->scale;
  int ow = (int)(output->wlr_output->width / scale);
  int oh = (int)(output->wlr_output->height / scale);
  int ox = (int)(output->scene_output->x);
  int oy = (int)(output->scene_output->y);

  // For the "all" overview, dim every monitor so the whole desktop reads as
  // overview mode; otherwise dim only the (primary) output we render on.
  if (overview_state.scope == OVERVIEW_ALL && root->outputs->length > 1) {
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

  overview_state.bg = wlr_scene_rect_create(root->layers.overview, ow, oh,
                                    (float[4]){0.0, 0.0, 0.0, 0.6});
  if (overview_state.bg) {
    wlr_scene_node_set_position(&overview_state.bg->node, ox, oy);
  }

  // Highlight ring shown behind the hovered thumbnail (created before the
  // thumbnails so it sits beneath them and reads as a border outline).
  overview_state.hover_rect = wlr_scene_rect_create(root->layers.overview, 0, 0,
      config->border_colors.focused.border);
  if (overview_state.hover_rect) {
    wlr_scene_node_set_enabled(&overview_state.hover_rect->node, false);
  }

  wl_list_init(&overview_state.thumbnails);
  wl_list_init(&overview_state.dividers);
  overview_state.n_thumbnails = 0;
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
  if (!fmt) {
    // overview_setup already created the dim background + hover ring on the
    // overlay layer; undo it so a future toggle doesn't leak them.
    overview_teardown();
    return;
  }

  int bt = 0;
  if (config->border == B_PIXEL || config->border == B_NORMAL) {
    bt = (int)(config->border_thickness * scale);
  }

  int con_idx = 0;
  // State content was resolved by overview_set_params (scope/action defaults,
  // swap focus-type override, minimized excluded from pull/focus).
  int content = overview_state.content;
  if (overview_state.scope == OVERVIEW_MINIMIZED) {
    overview_collect_minimized(output, renderer, alloc, fmt,
                               scale, bt, &con_idx);
    overview_layout_grid(output);
  } else if (overview_state.scope == OVERVIEW_ALL) {
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