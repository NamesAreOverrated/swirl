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

// Room a column can still grow into before hitting its view's declared
// maximum width (DBL_MAX when unconstrained).
static double col_width_headroom(struct sway_container *col, double width) {
	double cm_w = DBL_MIN, cmax_w = DBL_MAX, cm_h = DBL_MIN, cmax_h = DBL_MAX;
	container_get_size_constraints(col, &cm_w, &cmax_w, &cm_h, &cmax_h);
	return cmax_w != DBL_MAX ? fmax(0, cmax_w - width) : DBL_MAX;
}

void workspace_arrange_columns(struct sway_workspace *ws,
		struct wlr_box *parent) {
	if (!ws->tiling || ws->tiling->length == 0) {
		return;
	}

	int gaps = ws->gaps_inner;
	double usable_h = parent->height;

	// Pixel-faithful layout: each column's pending.width is the authority
	// (written by workspace_fit_new_column, viewport_absorb_farthest, the
	// resize primitives, ...). Fractions are derived state, re-synced below
	// to match whatever gets rendered, so every width_fraction consumer
	// (move-between-workspaces, swap, resize deltas, ipc) sees reality and
	// the next arrange reproduces this layout exactly.
	int n = ws->tiling->length;
	double total_w = fmax(0, parent->width - gaps * (n - 1));

	// Containers are placed at their on-screen (output-global) position:
	// ws->x/y is the tiling origin (already includes outer gaps), so column
	// coordinates start there and every consumer (xwayland geometry, input
	// mapping, ipc, focus/warp math) can take pending coords at face value.
	double origin_x = ws->x;
	double origin_y = ws->y;

	double *w = malloc(sizeof(double) * n);
	bool *locked = calloc(n, sizeof(bool));
	if (!w || !locked) {
		free(w);
		free(locked);
		return;
	}

	double sum = 0;
	for (int i = 0; i < n; ++i) {
		struct sway_container *col = ws->tiling->items[i];
		// Unified clamp: honors the view's min/max (and config min width) so
		// the resize primitives and the arrange pass enforce identical rules.
		w[i] = container_clamp_tiled_width(col, col->pending.width, total_w);
		sum += w[i];
	}

	if (sum > total_w && sum > 0) {
		// Overflow (stale state, e.g. the output shrank): shrink
		// proportionally, floored at each view's minimum by the clamp. Real
		// opens never land here: fit_new_column + viewport_absorb_farthest
		// already make the row fit exactly.
		double scale = total_w / sum;
		for (int i = 0; i < n; ++i) {
			w[i] = container_clamp_tiled_width(ws->tiling->items[i],
					w[i] * scale, total_w);
		}
	} else if (sum < total_w - 0.5) {
		// Underflow (sparse row, e.g. a fresh workspace): grow to fill
		// proportionally to the current widths, capped by each view's
		// declared maximum size.
		double spare = total_w - sum;
		while (spare > 0.5) {
			double base = 0, room = 0;
			for (int i = 0; i < n; ++i) {
				if (locked[i]) continue;
				base += w[i];
				room += col_width_headroom(ws->tiling->items[i], w[i]);
			}
			if (room <= 0.5) break;
			double taken = 0;
			for (int i = 0; i < n; ++i) {
				if (locked[i] || spare - taken <= 0.5) continue;
				double headroom =
					col_width_headroom(ws->tiling->items[i], w[i]);
				if (headroom <= 0.5) {
					locked[i] = true;
					continue;
				}
				double share = base > 0 ? spare * w[i] / base : spare / n;
				double grow = fmin(fmin(share, headroom), spare - taken);
				w[i] += grow;
				taken += grow;
			}
			if (taken <= 0.5) break;
			spare -= taken;
		}
	}

	// Integer-exact placement: cumulative rounding keeps every column at
	// round() of its ideal geometry so the whole row lands within half a
	// pixel of the parent edge without a dedicated last-column fill.
	double pos = 0;
	long prev_edge = 0;
	for (int i = 0; i < n; ++i) {
		struct sway_container *col = ws->tiling->items[i];

		pos += w[i];
		long edge = lround(pos);
		col->pending.x = origin_x + prev_edge;
		col->pending.y = origin_y;
		col->pending.height = usable_h;
		col->pending.width = edge - prev_edge;

		// Persist what rendered so the next arrange is a fixed point.
		col->width_fraction = workspace_width_to_fraction(ws,
				col->pending.width);

		node_set_dirty(&col->node);

		if (!col->view && col->pending.children) {
			if (col->pending.layout == L_VERT) {
				viewport_arrange_windows(col);
			} else {
				arrange_container(col);
			}
		}

		prev_edge = edge + gaps;
		pos += gaps;
	}

	free(w);
	free(locked);
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
	// Space at the insertion point after column `start_index`, assuming the
	// columns to its left keep their widths. Structural on purpose: with
	// horizontal scrolling gone every tiling column is in the viewport, so
	// stale pre-arrange geometry must never influence sizing decisions.
	int gaps = ws->gaps_inner;
	int start = start_index < 0 ? ws->tiling->length - 1 : start_index;
	double free = ws->width;
	for (int i = 0; i <= start && i < ws->tiling->length; ++i) {
		struct sway_container *col = ws->tiling->items[i];
		free -= col->pending.width + gaps;
	}
	return free > 0 ? free : 0;
}

int viewport_scan_visible(struct sway_workspace *ws, int focus_idx,
		int exclude_idx, bool exclude_occupied, int *candidates,
		int max_cand, double *out_occupied) {
	// No scrolling means every tiling column is in the viewport; occupancy
	// is computed structurally from the list instead of from (possibly
	// stale, pre-arrange) column geometry. Candidate order is irrelevant:
	// viewport_absorb_farthest re-sorts by distance from focus on each pick.
	double sum = 0;
	int count = 0;
	for (int i = 0; i < ws->tiling->length; ++i) {
		if (exclude_occupied && i == exclude_idx) {
			continue;
		}
		struct sway_container *col = ws->tiling->items[i];
		sum += col->pending.width;
		count++;
	}

	int n = 0;
	for (int i = 0; i < ws->tiling->length && n < max_cand; ++i) {
		if (i != exclude_idx)
			candidates[n++] = i;
	}

	*out_occupied = count > 1 ? sum + ws->gaps_inner * (count - 1) : sum;
	return n;
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
	// No scrolling: the whole tiling list is always visible.
	if (ws->tiling->length > 0) {
		*start = 0;
		*end = ws->tiling->length - 1;
	} else {
		*start = -1;
		*end = -1;
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

	// 2. Grow remaining columns (up to default_w each)
	{
		for (int i = col_idx; i < ws->tiling->length && remaining > 1; ++i) {
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

	// 3. Give anything left over to the smallest column.
	if (remaining > 1 && vs >= 0) {
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

	if (remaining > 0 && remaining < 0.5) {
		remaining = 0;
	}

	int ret;
	if (col_idx - 1 >= 0) ret = col_idx - 1;
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
