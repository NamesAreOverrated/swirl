#define _POSIX_C_SOURCE 200809L
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>
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

	// Containers are placed at their on-screen (output-global) position:
	// ws->x/y is the tiling origin (already includes outer gaps), so column
	// coordinates start there and every consumer (xwayland geometry, input
	// mapping, ipc, focus/warp math) can take pending coords at face value.
	double origin_x = ws->x;
	double origin_y = ws->y;

	// Reserve each column's requested minimum width first, then distribute
	// the remaining space in proportion to the columns' width fractions. This
	// keeps every column at or above its view's minimum while still filling
	// the workspace exactly, so the post-hoc clamp never has to mutate
	// width_fraction (which previously caused a ratcheting overflow).
	double *cmin = malloc(sizeof(double) * n);
	double reserved = 0;
	for (int i = 0; i < n; ++i) {
		struct sway_container *col = ws->tiling->items[i];
		cmin[i] = container_clamp_tiled_width_min(col);
		reserved += cmin[i];
	}
	double remainder = child_total_width - reserved;

	double x = 0;
	for (int i = 0; i < n; ++i) {
		struct sway_container *col = ws->tiling->items[i];

		double frac = col->width_fraction > 0 ? col->width_fraction : 1.0;
		col->pending.x = origin_x + x;
		col->pending.y = origin_y;
		col->pending.height = usable_h;
		double w = (remainder >= 0)
				? cmin[i] + remainder * (frac / total_frac)
				: cmin[i];
		if (i < n - 1) {
			col->pending.width = round(w);
		} else {
			col->pending.width = fmax(0, parent->width - x);
		}
		// Unified clamp: honors the view's min/max (and config min width) so
		// the resize primitives and the arrange pass enforce identical rules.
		// width_fraction is left as the user-intent value (not rewritten).
		col->pending.width = container_clamp_tiled_width(col,
				col->pending.width, parent->width);
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
	free(cmin);
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

	// Split the usable column height by height fraction first, so a fraction
	// is a true absolute-px share and a window can be driven all the way down
	// to its view minimum (the old reserve-then-split rendered a floor of
	// cmin + a fraction share, which stranded windows above their real min).
	// Each child is then clamped to [min, max] and the resulting slack or
	// demand is redistributed proportionally among the flexible children. If
	// the mins alone overflow the column, every child is pinned to its
	// minimum (stable vertical overflow, no scroll).
	double *heights = malloc(sizeof(double) * n);
	double *cmin_h = malloc(sizeof(double) * n);
	double *hi_h = malloc(sizeof(double) * n);
	double reserved_h = 0;
	for (int i = 0; i < n; ++i) {
		struct sway_container *child = col->pending.children->items[i];
		cmin_h[i] = container_clamp_tiled_height_min(child);
		hi_h[i] = container_clamp_tiled_height(child, usable_h, usable_h);
		reserved_h += cmin_h[i];
	}

	if (reserved_h > usable_h) {
		for (int i = 0; i < n; ++i) {
			heights[i] = cmin_h[i];
		}
	} else {
		double leftover = usable_h;
		for (int i = 0; i < n; ++i) {
			struct sway_container *child = col->pending.children->items[i];
			double hf = child->height_fraction > 0 ? child->height_fraction : 1.0;
			double raw = usable_h * (hf / total_hf);
			heights[i] = container_clamp_tiled_height(child, raw, usable_h);
			leftover -= heights[i];
		}
		// Redistribute: children that are not pinned to min/max absorb or shed
		// the difference, weighted by their fractions. Two passes cover the
		// realistic case where only real view min/max constraints clamp.
		for (int pass = 0; pass < 2 && leftover != 0; ++pass) {
			double flex_sum = 0;
			for (int i = 0; i < n; ++i) {
				struct sway_container *child = col->pending.children->items[i];
				double hf = child->height_fraction > 0 ? child->height_fraction : 1.0;
				bool flexible = (leftover > 0 && heights[i] < hi_h[i]) ||
					(leftover < 0 && heights[i] > cmin_h[i]);
				if (flexible) {
					flex_sum += hf;
				}
			}
			if (flex_sum <= 0) {
				break;
			}
			for (int i = 0; i < n; ++i) {
				struct sway_container *child = col->pending.children->items[i];
				double hf = child->height_fraction > 0 ? child->height_fraction : 1.0;
				if (leftover > 0 && heights[i] >= hi_h[i]) {
					continue;
				}
				if (leftover < 0 && heights[i] <= cmin_h[i]) {
					continue;
				}
				double want = heights[i] + leftover * (hf / flex_sum);
				double next = container_clamp_tiled_height(child, want, usable_h);
				leftover -= next - heights[i];
				heights[i] = next;
			}
		}
		// Exact fill: dump any sub-pixel remainder on the last child.
		if (leftover != 0) {
			struct sway_container *last = col->pending.children->items[n - 1];
			double next = container_clamp_tiled_height(last,
				heights[n - 1] + leftover, usable_h);
			heights[n - 1] = next;
		}
	}

	double y = 0;
	for (int i = 0; i < n; ++i) {
		struct sway_container *child = col->pending.children->items[i];

		child->pending.height = heights[i];
		child->pending.x = col->pending.x;
		child->pending.y = col->pending.y + y;
		child->pending.width = col->pending.width;
		// Do NOT re-derive height_fraction here: the stored fraction is the
		// authoritative user-intent value (set by the resize primitive) and
		// the distribute above already honors min/max via the unified
		// container_clamp_tiled_height() clamp. Re-deriving from pixels mixed
		// a reserved-min part with a distributed part using the full
		// workspace height as the denominator, which was lossy and drifted
		// the ratio on every arrange pass (and only cancelled out at exactly
		// 0.5/0.5).
		y += heights[i] + gap;
		node_set_dirty(&child->node);

		if (child->view) {
			view_autoconfigure(child->view);
		}
	}
	free(heights);
	free(cmin_h);
	free(hi_h);
}

double workspace_view_remaining_width(struct sway_workspace *ws, int start_index) {
	int gaps = ws->gaps_inner;
	double vp = ws->viewport_x;
	double vp_end = vp + ws->width;
	int start = start_index < 0 ? ws->tiling->length - 1 : start_index;
	for (int i = start; i >= 0; --i) {
		struct sway_container *col = ws->tiling->items[i];
		double col_x = col_local_x(ws, col);
		if (col_x + col->pending.width + gaps < vp) {
			break;
		}
		if (col_x > vp_end) {
			continue;
		}
		return vp_end - (col_x + col->pending.width + gaps);
	}
	return ws->width;
}

int viewport_scan_visible(struct sway_workspace *ws, int focus_idx,
		int exclude_idx, bool exclude_occupied, int *candidates,
		int max_cand, double *out_occupied) {
	if (focus_idx < 0 || focus_idx >= ws->tiling->length) {
		*out_occupied = 0;
		return 0;
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
	}

	double sum = 0;
	int n = 0;
	int total_vis = 1;

	struct sway_container *fc = ws->tiling->items[focus_idx];
	if (exclude_occupied && focus_idx == exclude_idx) {
		total_vis--;
	} else {
		sum += fc->pending.width;
	}

	for (int i = focus_idx + 1; i < ws->tiling->length; ++i) {
		if (!viewport_column_is_visible(ws, i)) {
			break;
		}
		struct sway_container *c = ws->tiling->items[i];
		if (exclude_occupied && i == exclude_idx) {
			total_vis--;
		} else {
			total_vis++;
			sum += c->pending.width;
		}
		if (i != exclude_idx && n < max_cand)
			candidates[n++] = i;
	}

	for (int i = focus_idx - 1; i >= 0; --i) {
		if (!viewport_column_is_visible(ws, i)) {
			break;
		}
		struct sway_container *c = ws->tiling->items[i];
		if (exclude_occupied && i == exclude_idx) {
			total_vis--;
		} else {
			total_vis++;
			sum += c->pending.width;
		}
		if (i != exclude_idx && n < max_cand)
			candidates[n++] = i;
	}

	if (exclude_idx != focus_idx && n < max_cand) {
		candidates[n++] = focus_idx;
	}

	*out_occupied = sum + ws->gaps_inner * (total_vis - 1);
	return n;
}

bool viewport_column_is_visible(struct sway_workspace *ws, int col_idx) {
	if (col_idx < 0 || col_idx >= ws->tiling->length) {
		return false;
	}
	struct sway_container *c = ws->tiling->items[col_idx];
	double col_x = col_local_x(ws, c);
	double vp = ws->viewport_x;
	double vp_end = vp + ws->width;
	return col_x >= vp - 0.5
		&& col_x + c->pending.width <= vp_end + 0.5;
}

void viewport_absorb_farthest(struct sway_workspace *ws,
		int *candidates, int n_candidates, int focus_idx,
		double *remaining) {
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
		double new_cw = container_clamp_tiled_width(c,
				orig - *remaining, ws->width);
		double absorbed = orig - new_cw;
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

	// 1. Grow left visible neighbor up to default_w
	if (col_idx - 1 >= 0 && col_idx - 1 >= vs && remaining > 1) {
		struct sway_container *c = ws->tiling->items[col_idx - 1];
		double orig = c->pending.width;
		double room = fmax(0, default_w - orig);
		double give = fmin(room, remaining);
		if (give > 1) {
			c->pending.width += give;
			c->width_fraction = workspace_width_to_fraction(ws, c->pending.width);
			node_set_dirty(&c->node);
			remaining -= give;
		}
	}

	// 2. Grow visible right neighbors (up to default_w each)
	{
		for (int i = col_idx; i < ws->tiling->length && remaining > 1; ++i) {
			if (!viewport_column_is_visible(ws, i)) {
				break;
			}
			struct sway_container *c = ws->tiling->items[i];
			double orig = c->pending.width;
			double room = fmax(0, default_w - orig);
			double give = fmin(room, remaining);
			if (give > 1) {
				c->pending.width += give;
				c->width_fraction = workspace_width_to_fraction(ws, c->pending.width);
				node_set_dirty(&c->node);
				remaining -= give;
			}
		}
	}

	// 3. Handle slider (first off-screen column at or after col_idx)
	bool slider_resized = false;
	if (remaining > 1) {
		int slider_idx = -1;
		for (int i = col_idx; i < ws->tiling->length; ++i) {
			if (!viewport_column_is_visible(ws, i)) {
				slider_idx = i;
				break;
			}
		}

		if (slider_idx >= 0 && remaining >= min_w) {
			double new_w = remaining - ws->gaps_inner;
			if (new_w < min_w) new_w = min_w;
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
				items[smallest]->pending.width += remaining;
				items[smallest]->width_fraction =
					workspace_width_to_fraction(ws, items[smallest]->pending.width);
				node_set_dirty(&items[smallest]->node);
				remaining = 0;
			}
		}
	}

	if (remaining > 0 && remaining < 0.5) {
		remaining = 0;
	}

	int ret;
	if (col_idx < ws->tiling->length && slider_resized) ret = col_idx;
	else if (col_idx - 1 >= 0) ret = col_idx - 1;
	else ret = -1;
	return ret;
}

int viewport_grow_evenly(struct sway_workspace *ws, int col_idx,
		double freed_width) {
	if (!ws || ws->tiling->length == 0 || freed_width <= 1) {
		return -1;
	}

	int vs, ve;
	viewport_visible_range(ws, &vs, &ve);
	if (vs < 0 || ve < vs) {
		return -1;
	}

	int n_vis = ve - vs + 1;
	double give = freed_width / n_vis;
	double given = 0;

	for (int i = vs; i <= ve; ++i) {
		struct sway_container *c = ws->tiling->items[i];
		double add = (i == ve) ? (freed_width - given) : give;
		double new_w = fmin(c->pending.width + add, ws->width);
		c->pending.width = new_w;
		c->width_fraction = workspace_width_to_fraction(ws, new_w);
		node_set_dirty(&c->node);
		given += add;
	}

	return ve;
}
