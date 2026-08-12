#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>
#include "log.h"
#include "sway/server.h"
#include "sway/desktop/transaction.h"
#include "sway/config.h"
#include "sway/input/seat.h"
#include "sway/output.h"
#include <wlr/types/wlr_scene_animation.h>
#include "sway/tree/arrange.h"
#include "sway/tree/container.h"
#include "sway/tree/layout.h"
#include "sway/tree/view.h"
#include "sway/tree/viewport.h"
#include "sway/tree/workspace.h"
#include "sway/commands.h"
#include "sway/tree/column.h"

void workspace_arrange_columns(struct sway_workspace *ws,
		struct wlr_box *parent) {
	if (!ws->tiling || ws->tiling->length == 0) {
		return;
	}

	int gaps = ws->gaps_inner;
	double usable_h = parent->height;

	// Fit-to-width: normalize width fractions so columns always fill the
	// parent width (no overflow past the workspace boundary). Ratios between
	// columns are preserved.
	int n = ws->tiling->length;
	double total_frac = 0;
	for (int i = 0; i < n; ++i) {
		struct sway_container *col = ws->tiling->items[i];
		total_frac += col->width_fraction > 0 ? col->width_fraction : 1.0;
	}
	if (total_frac <= 0) {
		total_frac = n;
	}
	double child_total_width = fmax(0, parent->width - gaps * (n - 1));

	double x = 0;
	for (int i = 0; i < n; ++i) {
		struct sway_container *col = ws->tiling->items[i];

		double frac = col->width_fraction > 0 ? col->width_fraction : 1.0;
		col->pending.x = x;
		col->pending.y = 0;
		col->pending.height = usable_h;
		if (i < n - 1) {
			col->pending.width = round(frac / total_frac * child_total_width);
		} else {
			col->pending.width = fmax(0, parent->width - x);
		}
		node_set_dirty(&col->node);

		if (!col->view && col->pending.children) {
			if (col->pending.layout == L_VERT) {
				viewport_arrange_windows(col);
			} else {
				arrange_container(col);
			}
		}

		x += col->pending.width + gaps;
	}
}

void viewport_arrange_windows(struct sway_container *col) {
	if (!col || col->view || !col->pending.children
			|| col->pending.children->length == 0) {
		return;
	}

	struct sway_workspace *ws = col->pending.workspace;
	double gap = ws ? ws->gaps_inner : 0;
	int n = col->pending.children->length;

	// Fit-to-height: normalize height fractions so the windows always fill the
	// column height (no vertical overflow/scroll). Ratios are preserved.
	double total_hf = 0;
	for (int i = 0; i < n; ++i) {
		struct sway_container *child = col->pending.children->items[i];
		total_hf += child->height_fraction > 0 ? child->height_fraction : 1.0;
	}
	if (total_hf <= 0) {
		total_hf = n;
	}
	double usable_h = fmax(0, col->pending.height - gap * (n - 1));

	double y = 0;
	for (int i = 0; i < n; ++i) {
		struct sway_container *child = col->pending.children->items[i];

		double hf = child->height_fraction > 0 ? child->height_fraction : 1.0;
		if (i < n - 1) {
			child->pending.height = round(hf / total_hf * usable_h);
		} else {
			child->pending.height = fmax(0, col->pending.height - y);
		}
		child->pending.x = 0;
		child->pending.y = y;
		child->pending.width = col->pending.width;
		y += child->pending.height + gap;
		node_set_dirty(&child->node);

		if (child->view) {
			view_autoconfigure(child->view);
		}
	}
}

void viewport_compute_offset(struct sway_workspace *ws,
		struct sway_container *active, double area_width,
		double area_height) {
	// Horizontal scrolling has been removed: columns always fit within the
	// workspace width, so the viewport is fixed at (0,0).
	ws->viewport_x = 0;
	ws->viewport_y = 0;
}

double workspace_view_remaining_width(struct sway_workspace *ws, int start_index) {
	int gaps = ws->gaps_inner;
	double vp = ws->viewport_x;
	double vp_end = vp + ws->width;
	int start = start_index < 0 ? ws->tiling->length - 1 : start_index;
	for (int i = start; i >= 0; --i) {
		struct sway_container *col = ws->tiling->items[i];
		if (col->pending.x + col->pending.width + gaps < vp) {
			break;
		}
		if (col->pending.x > vp_end) {
			continue;
		}
		return vp_end - (col->pending.x + col->pending.width + gaps);
	}
	return ws->width;
}

void handle_focus_viewport(struct sway_seat *seat,
		struct sway_container *container) {
	// Vertical scrolling has been removed; columns always fit their windows,
	// so there is nothing to scroll into view on focus.
	(void)seat;
	(void)container;
}int viewport_scan_visible(struct sway_workspace *ws, int focus_idx,
		int exclude_idx, bool exclude_occupied, int *candidates,
		int max_cand, double *out_occupied) {
	sway_log(SWAY_DEBUG, "[FLOAT | viewport_scan_visible] ws=%p focus_idx=%d "
		"exclude_idx=%d exclude_occupied=%d tiling_len=%d vp_x=%.1f "
		"ws->width=%d", ws, focus_idx, exclude_idx, exclude_occupied,
		ws->tiling->length, ws->viewport_x, ws->width);

	for (int i = 0; i < ws->tiling->length; i++) {
		struct sway_container *c = ws->tiling->items[i];
		double x_end = c->pending.x + c->pending.width;
		double vp_end = ws->viewport_x + ws->width;
		bool vis = c->pending.x >= ws->viewport_x - 0.5
			&& x_end <= vp_end + 0.5;
		sway_log(SWAY_DEBUG, "[FLOAT | viewport_scan_visible]   col[%d]: %p "
			"x=%.1f w=%.1f x_end=%.1f vp_end=%.1f vis=%d",
			i, c, c->pending.x, c->pending.width, x_end, vp_end, vis);
	}

	if (!viewport_column_is_visible(ws, focus_idx)) {
		int orig = focus_idx;
		for (int i = focus_idx - 1; i >= 0; --i) {
			if (viewport_column_is_visible(ws, i)) {
				focus_idx = i;
				break;
			}
		}
		if (focus_idx == orig) {
			for (int i = focus_idx + 1; i < ws->tiling->length; ++i) {
				if (viewport_column_is_visible(ws, i)) {
					focus_idx = i;
					break;
				}
			}
		}
		if (focus_idx != orig) {
			sway_log(SWAY_DEBUG, "[FLOAT | viewport_scan_visible] "
				"focus_idx %d off-screen, adjusted to %d", orig, focus_idx);
		}
	}

	double sum = 0;
	int n = 0;
	int total_vis = 1;

	struct sway_container *fc = ws->tiling->items[focus_idx];
	if (exclude_occupied && focus_idx == exclude_idx) {
		total_vis--;
		sway_log(SWAY_DEBUG, "[FLOAT | viewport_scan_visible]   focus[%d] EXCLUDED",
			focus_idx);
	} else {
		sum += fc->pending.width;
		sway_log(SWAY_DEBUG, "[FLOAT | viewport_scan_visible]   focus[%d]: "
			"w=%.1f added", focus_idx, fc->pending.width);
	}

	for (int i = focus_idx + 1; i < ws->tiling->length; ++i) {
		if (!viewport_column_is_visible(ws, i)) {
			sway_log(SWAY_DEBUG, "[FLOAT | viewport_scan_visible]   right[%d]: "
				"NOT visible, break", i);
			break;
		}
		struct sway_container *c = ws->tiling->items[i];
		if (exclude_occupied && i == exclude_idx) {
			total_vis--;
			sway_log(SWAY_DEBUG, "[FLOAT | viewport_scan_visible]   right[%d] "
				"EXCLUDED", i);
		} else {
			total_vis++;
			sum += c->pending.width;
			sway_log(SWAY_DEBUG, "[FLOAT | viewport_scan_visible]   right[%d]: "
				"w=%.1f added", i, c->pending.width);
		}
		if (i != exclude_idx && n < max_cand)
			candidates[n++] = i;
	}

	for (int i = focus_idx - 1; i >= 0; --i) {
		if (!viewport_column_is_visible(ws, i)) {
			sway_log(SWAY_DEBUG, "[FLOAT | viewport_scan_visible]   left[%d]: "
				"NOT visible, break", i);
			break;
		}
		struct sway_container *c = ws->tiling->items[i];
		if (exclude_occupied && i == exclude_idx) {
			total_vis--;
			sway_log(SWAY_DEBUG, "[FLOAT | viewport_scan_visible]   left[%d] "
				"EXCLUDED", i);
		} else {
			total_vis++;
			sum += c->pending.width;
			sway_log(SWAY_DEBUG, "[FLOAT | viewport_scan_visible]   left[%d]: "
				"w=%.1f added", i, c->pending.width);
		}
		if (i != exclude_idx && n < max_cand)
			candidates[n++] = i;
	}

	if (exclude_idx != focus_idx && n < max_cand) {
		candidates[n++] = focus_idx;
	}

	*out_occupied = sum + ws->gaps_inner * (total_vis - 1);
	sway_log(SWAY_DEBUG, "[FLOAT | viewport_scan_visible] result: "
		"occupied=%.1f (sum=%.1f + gaps*%d) n_vis=%d n_candidates=%d",
		*out_occupied, sum, total_vis - 1, total_vis, n);
	return n;
}

bool viewport_column_is_visible(struct sway_workspace *ws, int col_idx) {
	struct sway_container *c = ws->tiling->items[col_idx];
	double vp = ws->viewport_x;
	double vp_end = vp + ws->width;
	return c->pending.x >= vp - 0.5
		&& c->pending.x + c->pending.width <= vp_end + 0.5;
}

void viewport_absorb_farthest(struct sway_workspace *ws,
		int *candidates, int n_candidates, int focus_idx,
		double *remaining, double min_col_w) {
	for (int k = 0; k < n_candidates && *remaining != 0; ++k) {
		int farthest = -1, farthest_dist = -1;
		for (int ci = 0; ci < n_candidates; ++ci) {
			if (candidates[ci] < 0) continue;
			int dist = abs(candidates[ci] - focus_idx);
			if (dist > farthest_dist || (dist == farthest_dist && candidates[ci] > farthest)) {
				farthest_dist = dist;
				farthest = ci;
			}
		}
		if (farthest < 0) break;
		int idx = candidates[farthest];
		candidates[farthest] = -1;  // mark used
		struct sway_container *c = ws->tiling->items[idx];
		double orig = c->pending.width;
		double new_cw = fmax(min_col_w, orig - *remaining);
		double absorbed = orig - new_cw;
		sway_log(SWAY_DEBUG, "[absorb]   col[%d]: %.4f -> %.4f (-%.4f) "
			"remaining=%.4f", idx, orig, new_cw, absorbed,
			*remaining - absorbed);
		*remaining -= absorbed;
		c->pending.width = new_cw;
		c->width_fraction = workspace_width_to_fraction(ws, new_cw);
		node_set_dirty(&c->node);
	}
}

void viewport_visible_range(struct sway_workspace *ws, int *start, int *end) {
	*start = -1;
	*end = -1;
	for (int i = 0; i < ws->tiling->length; ++i) {
		if (viewport_column_is_visible(ws, i)) {
			if (*start < 0) *start = i;
			*end = i;
		} else if (*start >= 0) {
			break;
		}
	}
}

int viewport_grow_to_fill(struct sway_workspace *ws, int col_idx,
		double freed_width) {
  if (!ws || ws->tiling->length == 0 || freed_width <= 1) {
    return -1;
  }

  freed_width = fmin(freed_width + ws->gaps_inner, ws->width);

  double remaining = freed_width;
  double default_w = workspace_width_fraction(ws,
      config->default_column_width_fraction);
  double min_w = workspace_width_fraction(ws,
      config->min_column_width_fraction);

  int vs, ve;
  viewport_visible_range(ws, &vs, &ve);
  sway_log(SWAY_DEBUG, "[grow] col_idx=%d freed=%.1f vis=[%d,%d] n=%d "
		"default_w=%.1f min_w=%.1f",
		col_idx, freed_width, vs, ve, ws->tiling->length, default_w, min_w);

	for (int i = 0; i < ws->tiling->length; ++i) {
		struct sway_container *c = ws->tiling->items[i];
		bool vis = viewport_column_is_visible(ws, i);
		sway_log(SWAY_DEBUG, "[grow]   before: col[%d] x=%.1f w=%.1f vis=%d",
			i, c->pending.x, c->pending.width, vis);
	}

	// 1. Grow left visible neighbor up to default_w
	if (col_idx - 1 >= 0 && col_idx - 1 >= vs && remaining > 1) {
		struct sway_container *c = ws->tiling->items[col_idx - 1];
		double orig = c->pending.width;
		double room = fmax(0, default_w - orig);
		double give = fmin(room, remaining);
		sway_log(SWAY_DEBUG, "[grow]   step1 left col[%d]: %.4f -> %.4f (+%.4f) "
			"room=%.4f rem=%.4f", col_idx - 1, orig, orig + give, give,
			room, remaining - give);
		if (give > 1) {
			c->pending.width += give;
			c->width_fraction = workspace_width_to_fraction(ws, c->pending.width);
			node_set_dirty(&c->node);
			remaining -= give;
		} else {
			sway_log(SWAY_DEBUG, "[grow]   step1 left col[%d]: give=%.4f too "
				"small, skipped", col_idx - 1, give);
		}
	} else {
		sway_log(SWAY_DEBUG, "[grow]   step1 left: skipped (idx=%d vs=%d "
			"rem=%.4f)", col_idx - 1, vs, remaining);
	}

	// 2. Grow visible right neighbors (up to default_w each)
	{
		int grown = 0;
		for (int i = col_idx; i < ws->tiling->length && remaining > 1; ++i) {
			if (!viewport_column_is_visible(ws, i)) {
				sway_log(SWAY_DEBUG, "[grow]   step2 stop at col[%d] (not vis)", i);
				break;
			}
			struct sway_container *c = ws->tiling->items[i];
			double orig = c->pending.width;
			double room = fmax(0, default_w - orig);
			double give = fmin(room, remaining);
			sway_log(SWAY_DEBUG, "[grow]   step2 col[%d]: %.4f -> %.4f (+%.4f) "
				"room=%.4f rem=%.4f", i, orig, orig + give, give,
				room, remaining - give);
			if (give > 1) {
				c->pending.width += give;
				c->width_fraction = workspace_width_to_fraction(ws, c->pending.width);
				node_set_dirty(&c->node);
				remaining -= give;
				++grown;
			} else {
				sway_log(SWAY_DEBUG, "[grow]   step2 col[%d]: give=%.4f too "
					"small, skipped", i, give);
			}
		}
		sway_log(SWAY_DEBUG, "[grow]   step2 done: grown=%d remaining=%.4f",
			grown, remaining);
	}

	// 3. Handle slider (first off-screen column at or after col_idx)
	bool slider_resized = false;
	sway_log(SWAY_DEBUG, "[grow]   step3 start: remaining=%.4f", remaining);
	if (remaining > 1) {
		int slider_idx = -1;
		for (int i = col_idx; i < ws->tiling->length; ++i) {
			if (!viewport_column_is_visible(ws, i)) {
				slider_idx = i;
				break;
			}
		}
		sway_log(SWAY_DEBUG, "[grow]   step3: slider_idx=%d remaining=%.4f "
			"min_w=%.4f", slider_idx, remaining, min_w);

		if (slider_idx >= 0 && remaining >= min_w) {
			double orig = ((struct sway_container *)ws->tiling->items[slider_idx])->pending.width;
			double new_w = remaining - ws->gaps_inner;
			if (new_w < min_w) new_w = min_w;
			sway_log(SWAY_DEBUG, "[grow]   step3: slider[%d]: %.4f -> %.4f "
				"(delta=%.4f)", slider_idx, orig, new_w, new_w - orig);
			struct sway_container *slider = ws->tiling->items[slider_idx];
			slider->pending.width = new_w;
			slider->width_fraction = workspace_width_to_fraction(ws, new_w);
			node_set_dirty(&slider->node);
			remaining = 0;
			slider_resized = true;
		}
		if (remaining > 1) {
			if (vs >= 0) {
				struct sway_container **items = (struct sway_container **)ws->tiling->items;
				double min_vw = items[vs]->pending.width;
				int smallest = vs;
				for (int i = vs; i <= ve; ++i) {
					double w = items[i]->pending.width;
					if (w < min_vw) {
						min_vw = w;
						smallest = i;
					}
				}
				double orig = items[smallest]->pending.width;
				sway_log(SWAY_DEBUG, "[grow]   step3: dump to vis col[%d]: %.4f -> "
					"%.4f (+%.4f)", smallest, orig, orig + remaining, remaining);
				items[smallest]->pending.width += remaining;
				items[smallest]->width_fraction =
					workspace_width_to_fraction(ws, items[smallest]->pending.width);
				node_set_dirty(&items[smallest]->node);
				remaining = 0;
			} else {
				sway_log(SWAY_DEBUG, "[grow]   step3: no target, remaining=%.4f "
					"lost", remaining);
			}
		}
	}

	if (remaining > 0 && remaining < 0.5) {
		sway_log(SWAY_DEBUG, "[grow]   cleanup: zeroing sub-pixel remaining=%.4f",
			remaining);
		remaining = 0;
	}

	for (int i = 0; i < ws->tiling->length; ++i) {
		struct sway_container *c = ws->tiling->items[i];
		bool vis = viewport_column_is_visible(ws, i);
		sway_log(SWAY_DEBUG, "[grow]   after: col[%d] x=%.4f w=%.4f vis=%d",
			i, c->pending.x, c->pending.width, vis);
	}

	int ret;
	if (col_idx < ws->tiling->length && slider_resized) ret = col_idx;
	else if (col_idx - 1 >= 0) ret = col_idx - 1;
	else ret = -1;
	sway_log(SWAY_DEBUG, "[grow]   return focus_idx=%d", ret);
	return ret;
}

int viewport_grow_evenly(struct sway_workspace *ws, int col_idx,
		double freed_width) {
	if (!ws || ws->tiling->length == 0 || freed_width <= 1) {
		sway_log(SWAY_DEBUG, "[FLOAT | viewport_grow_evenly] early return -1: "
			"tiling_len=%d freed_width=%.1f",
			ws ? ws->tiling->length : -1, freed_width);
		return -1;
	}

	int vs, ve;
	viewport_visible_range(ws, &vs, &ve);
	if (vs < 0 || ve < vs) {
		sway_log(SWAY_DEBUG, "[FLOAT | viewport_grow_evenly] no visible range: "
			"vs=%d ve=%d", vs, ve);
		return -1;
	}

	int n_vis = ve - vs + 1;
	double give = freed_width / n_vis;
	double given = 0;

	sway_log(SWAY_DEBUG, "[FLOAT | viewport_grow_evenly] ws=%p col_idx=%d "
		"freed_width=%.1f vs=%d ve=%d n_vis=%d give=%.2f",
		ws, col_idx, freed_width, vs, ve, n_vis, give);

	for (int i = 0; i < ws->tiling->length; i++) {
		struct sway_container *c = ws->tiling->items[i];
		sway_log(SWAY_DEBUG, "[FLOAT | viewport_grow_evenly]   before[%d]: %p "
			"x=%.1f w=%.1f", i, c, c->pending.x, c->pending.width);
	}

	for (int i = vs; i <= ve; ++i) {
		struct sway_container *c = ws->tiling->items[i];
		double add = (i == ve) ? (freed_width - given) : give;
		double new_w = fmin(c->pending.width + add, ws->width);
		sway_log(SWAY_DEBUG, "[FLOAT | viewport_grow_evenly]   col[%d]: "
			"w=%.1f + %.1f = %.1f (capped at ws->width=%d)",
			i, c->pending.width, add, new_w, ws->width);
		c->pending.width = new_w;
		c->width_fraction = workspace_width_to_fraction(ws, new_w);
		node_set_dirty(&c->node);
		given += add;
	}

	sway_log(SWAY_DEBUG, "[FLOAT | viewport_grow_evenly] return ve=%d "
		"tiling_len=%d", ve, ws->tiling->length);
	return ve;
}

struct cmd_results *cmd_evenh(int argc, char **argv) {
	struct sway_container *con = config->handler_context.container;
	if (!con) {
		return cmd_results_new(CMD_SUCCESS, NULL);
	}
	con = container_toplevel_ancestor(con);
	struct sway_workspace *ws = con->pending.workspace;
	if (!ws || ws->tiling->length == 0) {
		return cmd_results_new(CMD_SUCCESS, NULL);
	}

	double vp = ws->viewport_x;
	double vp_end = vp + ws->width;

	int *visible = malloc(ws->tiling->length * sizeof(int));
	if (!visible) {
		return cmd_results_new(CMD_FAILURE, "allocation failed");
	}
	int n = 0;
	for (int i = 0; i < ws->tiling->length; ++i) {
		struct sway_container *c = ws->tiling->items[i];
		if (c->pending.x + c->pending.width > vp - 0.5 &&
				c->pending.x < vp_end + 0.5) {
			visible[n++] = i;
		}
	}

	if (n < 2) {
		free(visible);
		return cmd_results_new(CMD_SUCCESS, NULL);
	}

	int gaps = ws->gaps_inner;
	double usable = ws->width - (n - 1) * gaps;
	double new_w = usable / n;

	for (int i = 0; i < n; ++i) {
		struct sway_container *c = ws->tiling->items[visible[i]];
		column_set_width_px(c, new_w);
		node_set_dirty(&c->node);
	}
	free(visible);

	arrange_workspace(ws);
	transaction_commit_dirty();
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_evenv(int argc, char **argv) {
	struct sway_container *con = config->handler_context.container;
	if (!con) {
		return cmd_results_new(CMD_SUCCESS, NULL);
	}

	con = container_toplevel_ancestor(con);
	if (con->pending.layout != L_VERT || !con->pending.children ||
			con->pending.children->length < 2) {
		return cmd_results_new(CMD_SUCCESS, NULL);
	}

	struct sway_workspace *ws = con->pending.workspace;
	if (!ws) {
		return cmd_results_new(CMD_SUCCESS, NULL);
	}

	int gaps = ws->gaps_inner;
	int n = con->pending.children->length;

	if (n < 2) {
		return cmd_results_new(CMD_SUCCESS, NULL);
	}

	double usable = con->pending.height - (n - 1) * gaps;
	double new_h = usable / n;

	for (int i = 0; i < n; ++i) {
		struct sway_container *child = con->pending.children->items[i];
		window_set_height_px(child, new_h);
		node_set_dirty(&child->node);
	}

	arrange_workspace(ws);
	transaction_commit_dirty();
	return cmd_results_new(CMD_SUCCESS, NULL);
}
