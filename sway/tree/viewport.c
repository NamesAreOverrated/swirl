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

	ws->viewport_x = round(edge_snap_horiz(active->pending.x,
		active->pending.width,
		ws->viewport_x, area_width, max_x));
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
	double old_vp_x = ws->viewport_x;
	ws->viewport_x = round(edge_snap_horiz(col->pending.x,
		col->pending.width,
		ws->viewport_x, area_w, max_x));
	sway_log(SWAY_DEBUG, "[focus_vp] vp_x: %.4f -> %.4f (delta=%.4f) "
		"col[%d] x=%.4f w=%.4f total=%.4f area_w=%.4f max_x=%.4f",
		old_vp_x, ws->viewport_x, ws->viewport_x - old_vp_x,
		list_find(ws->tiling, col), col->pending.x,
		col->pending.width, total_w, area_w, max_x);
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
			double new_w = remaining;
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
