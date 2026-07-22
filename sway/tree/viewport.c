#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>
#include "log.h"
#include "sway/desktop/transaction.h"
#include "sway/config.h"
#include "sway/input/seat.h"
#include "sway/output.h"
#include "sway/tree/animation.h"
#include "sway/tree/container.h"
#include "sway/tree/layout.h"
#include "sway/tree/view.h"
#include "sway/tree/viewport.h"
#include "sway/tree/workspace.h"

static bool fully_visible(double x, double w, double vp, double vpl) {
	return x >= vp && x + w <= vp + vpl;
}

static double total_extent_h(list_t *tiling, int gaps) {
	double total = 0;
	for (int i = 0; i < tiling->length; ++i) {
		struct sway_container *c = tiling->items[i];
		total += c->pending.width;
		if (i < tiling->length - 1) {
			total += gaps;
		}
	}
	return total;
}

static double total_extent_v(list_t *children, int gaps) {
	double total = 0;
	for (int i = 0; i < children->length; ++i) {
		struct sway_container *c = children->items[i];
		total += c->pending.height;
		if (i < children->length - 1) {
			total += gaps;
		}
	}
	return total;
}

static double edge_snap_horiz(double con_x, double con_w, double vp,
		double area_w, double max_x) {
	if (fully_visible(con_x, con_w, vp, area_w)) {
		return vp;
	}
	double target = con_x < vp ? con_x : con_x + con_w - area_w;
	target = target < 0.0 ? 0.0 : target;
	target = target > max_x ? max_x : target;
	return target;
}

static double edge_snap_vert(double con_y, double con_h, double vp,
		double area_h, double max_y) {
	if (fully_visible(con_y, con_h, vp, area_h)) {
		return vp;
	}
	double target = con_y < vp ? con_y : con_y + con_h - area_h;
	target = target < 0.0 ? 0.0 : target;
	target = target > max_y ? max_y : target;
	return target;
}

void workspace_arrange_columns(struct sway_workspace *ws,
		struct wlr_box *parent) {
	if (!ws->tiling || ws->tiling->length == 0) {
		return;
	}

	int gaps = ws->gaps_inner;
	double usable_h = parent->height;

	double x = 0;
	for (int i = 0; i < ws->tiling->length; ++i) {
		struct sway_container *col = ws->tiling->items[i];

		col->pending.x = x;
		col->pending.y = 0;
		col->pending.height = usable_h;
		node_set_dirty(&col->node);

		if (!col->view && col->pending.children) {
			viewport_arrange_windows(col);
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

	double y = 0;
	for (int i = 0; i < col->pending.children->length; ++i) {
		struct sway_container *child = col->pending.children->items[i];

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
	if (!active || active->node.destroying) {
		ws->viewport_x = 0;
		ws->viewport_y = 0;
		return;
	}

	int gaps = ws->gaps_inner;
	double total_w = total_extent_h(ws->tiling, gaps);
	double max_x = total_w > area_width ? total_w - area_width : 0.0;

	ws->viewport_x = edge_snap_horiz(active->pending.x,
		active->pending.width,
		ws->viewport_x, area_width, max_x);
	ws->viewport_y = 0;
}

void column_scroll_vert_to(struct sway_container *col,
		struct sway_container *win, double area_h) {
	if (!col || col->view || !col->pending.children) {
		return;
	}
	if (!win || win == col || list_find(col->pending.children, win) == -1) {
		col->pending.scroll_y = 0;
		node_set_dirty(&col->node);
		return;
	}
	int gaps = col->pending.workspace
		? col->pending.workspace->gaps_inner : 0;
	double total_h = total_extent_v(col->pending.children, gaps);
	double max_y = total_h > area_h ? total_h - area_h : 0.0;

	double old_scroll_y = col->pending.scroll_y;
	double new_scroll_y = edge_snap_vert(win->pending.y,
		win->pending.height,
		old_scroll_y, area_h, max_y);
	col->pending.scroll_y = new_scroll_y;

	if (col->content_tree) {
		double from_y = col->content_tree->node.y;
		struct sway_prop_config cfg = {
			.type = SWAY_ANIM_SPRING,
			.damping_ratio = 1.0,
			.stiffness = 1200.0,
			.epsilon = 0.001,
		};
		sway_anim_move(&col->content_tree->node,
			0, from_y,
			0, -new_scroll_y,
			cfg);
	}

	node_set_dirty(&col->node);
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

double column_view_remaining_height(struct sway_container *col, int start_index) {
	int gap = col->pending.workspace ? col->pending.workspace->gaps_inner : 0;
	double scroll_y = col->pending.scroll_y;
	double vp_end = scroll_y + col->pending.height;
	int start = start_index < 0 ? col->pending.children->length - 1 : start_index;
	for (int i = start; i >= 0; --i) {
		struct sway_container *child = col->pending.children->items[i];
		if (child->pending.y + child->pending.height + gap < scroll_y) {
			break;
		}
		if (child->pending.y > vp_end) {
			continue;
		}
		return vp_end - (child->pending.y + child->pending.height + gap);
	}
	return col->pending.height;
}

void handle_focus_viewport(struct sway_seat *seat,
		struct sway_container *container) {
	if (!container || container_is_floating(container)) {
		return;
	}
	struct sway_workspace *ws = container->pending.workspace;
	if (!ws || !ws->tiling || ws->tiling->length == 0) {
		return;
	}

	struct sway_container *col = container;
	while (col->pending.parent) {
		col = col->pending.parent;
	}
	if (list_find(ws->tiling, col) == -1) {
		return;
	}

	int gaps = ws->gaps_inner;
	double area_w = ws->width;
	double area_h = ws->height;

	// Horizontal — edge-snap column into viewport
	double total_w = total_extent_h(ws->tiling, gaps);
	double max_x = total_w > area_w ? total_w - area_w : 0.0;
	ws->viewport_x = edge_snap_horiz(col->pending.x,
		col->pending.width,
		ws->viewport_x, area_w, max_x);
	ws->viewport_y = 0;

	// Vertical — edge-snap focused window into column viewport
	column_scroll_vert_to(col, col != container ? container : NULL, area_h);

	node_set_dirty(&ws->node);
	node_set_dirty(&col->node);
	transaction_commit_dirty();
}

int viewport_scan_visible(struct sway_workspace *ws, int focus_idx,
		int exclude_idx, int *candidates, int max_cand, double *out_occupied) {
	double sum = 0;
	int n = 0;
	int total_vis = 1;

	struct sway_container *fc = ws->tiling->items[focus_idx];
	sum += fc->pending.width;

	for (int i = focus_idx + 1; i < ws->tiling->length; ++i) {
		if (!viewport_column_is_visible(ws, i)) break;
		struct sway_container *c = ws->tiling->items[i];
		total_vis++;
		sum += c->pending.width;
		if (i != exclude_idx && n < max_cand)
			candidates[n++] = i;
	}

	for (int i = focus_idx - 1; i >= 0; --i) {
		if (!viewport_column_is_visible(ws, i)) break;
		struct sway_container *c = ws->tiling->items[i];
		total_vis++;
		sum += c->pending.width;
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
		*remaining -= absorbed;
		c->pending.width = new_cw;
		c->width_fraction = workspace_width_to_fraction(ws, new_cw);
		node_set_dirty(&c->node);
	}
}

int viewport_grow_to_fill(struct sway_workspace *ws, int col_idx,
		double freed_width) {
	if (!ws || ws->tiling->length == 0 || freed_width <= 1) {
		return -1;
	}

	double remaining = freed_width;
	double half = remaining / 2;

	sway_log(SWAY_DEBUG, "[close-slide] col_idx=%d freed_width=%.0f "
		"n_cols=%d", col_idx, freed_width, ws->tiling->length);

	for (int i = 0; i < ws->tiling->length; ++i) {
		struct sway_container *c = ws->tiling->items[i];
		bool vis = viewport_column_is_visible(ws, i);
		sway_log(SWAY_DEBUG, "[close-slide]   before: col[%d] x=%.0f w=%.0f "
			"vis=%d vp=[%.0f, %.0f]", i, c->pending.x, c->pending.width,
			vis, ws->viewport_x, ws->viewport_x + ws->width);
	}

	sway_log(SWAY_DEBUG, "[close-slide]   left_check: idx=%d exists=%d vis=%d",
		col_idx - 1, col_idx - 1 >= 0,
		col_idx - 1 >= 0 ? viewport_column_is_visible(ws, col_idx - 1) : 0);
	sway_log(SWAY_DEBUG, "[close-slide]   right_check: idx=%d exists=%d vis=%d",
		col_idx, col_idx < ws->tiling->length,
		col_idx < ws->tiling->length ? viewport_column_is_visible(ws, col_idx) : 0);

	double default_w = workspace_width_fraction(ws,
			config->default_column_width_fraction);

	// Left neighbor gets up to default_w (half share)
	if (col_idx - 1 >= 0 && half > 1
			&& viewport_column_is_visible(ws, col_idx - 1)) {
		struct sway_container *c = ws->tiling->items[col_idx - 1];
		double room = fmax(0, default_w - c->pending.width);
		double give = fmin(room, half);
		sway_log(SWAY_DEBUG, "[close-slide]   left col %d: "
			"w=%.0f + %.0f (room=%.0f) = %.0f", col_idx - 1,
			c->pending.width, give, room, c->pending.width + give);
		c->pending.width += give;
		c->width_fraction = workspace_width_to_fraction(ws, c->pending.width);
		node_set_dirty(&c->node);
		remaining -= give;
	}

	// Right neighbor gets up to default_w (remaining share)
	if (col_idx < ws->tiling->length && remaining > 1
			&& viewport_column_is_visible(ws, col_idx)) {
		struct sway_container *c = ws->tiling->items[col_idx];
		double room = fmax(0, default_w - c->pending.width);
		double give = fmin(room, remaining);
		sway_log(SWAY_DEBUG, "[close-slide]   right col %d: "
			"w=%.0f + %.0f (room=%.0f) = %.0f", col_idx,
			c->pending.width, give, room, c->pending.width + give);
		c->pending.width += give;
		c->width_fraction = workspace_width_to_fraction(ws, c->pending.width);
		node_set_dirty(&c->node);
		remaining -= give;
	}

	// Left spillover (if right was off-screen or saturated)
	if (remaining > 1 && col_idx - 1 >= 0
			&& viewport_column_is_visible(ws, col_idx - 1)) {
		struct sway_container *c = ws->tiling->items[col_idx - 1];
		double room = fmax(0, default_w - c->pending.width);
		double give = fmin(room, remaining);
		sway_log(SWAY_DEBUG, "[close-slide]   left spillover: +%.0f (room=%.0f)", give, room);
		c->pending.width += give;
		c->width_fraction = workspace_width_to_fraction(ws, c->pending.width);
		node_set_dirty(&c->node);
		remaining -= give;
	}

	for (int i = 0; i < ws->tiling->length; ++i) {
		struct sway_container *c = ws->tiling->items[i];
		bool vis = viewport_column_is_visible(ws, i);
		sway_log(SWAY_DEBUG, "[close-slide]   after: col[%d] x=%.0f w=%.0f "
			"vis=%d", i, c->pending.x, c->pending.width, vis);
	}

	sway_log(SWAY_DEBUG, "[close-slide] remaining=%.0f", remaining);

	if (col_idx < ws->tiling->length) return col_idx;
	if (col_idx - 1 >= 0) return col_idx - 1;
	return -1;
}
