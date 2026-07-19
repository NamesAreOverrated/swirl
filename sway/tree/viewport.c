#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>
#include "log.h"
#include "sway/desktop/transaction.h"
#include "sway/input/seat.h"
#include "sway/output.h"
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
	double usable_w = parent->width;
	double usable_h = parent->height;

	int n = ws->tiling->length;
	double gaps_total = n > 1 ? gaps * (n - 1) : 0;
	double content_w = usable_w - gaps_total;

	double x = 0;
	for (int i = 0; i < ws->tiling->length; ++i) {
		struct sway_container *col = ws->tiling->items[i];

		double col_w = round(content_w * col->width_fraction);
		double col_h = usable_h;

		if (col_w < 10 || col_h < 10) {
			col_w = 0;
			col_h = 0;
		}

		col->pending.x = x;
		col->pending.y = 0;
		col->pending.width = col_w;
		col->pending.height = col_h;
		node_set_dirty(&col->node);

		if (!col->view && col->pending.children) {
			viewport_arrange_windows(col);
		}

		x += col_w + gaps;
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
	double gaps_total = n > 1 ? gap * (n - 1) : 0;
	double content_h = col->pending.height - gaps_total;

	double y = 0;
	for (int i = 0; i < col->pending.children->length; ++i) {
		struct sway_container *child = col->pending.children->items[i];

		double child_h = round(content_h * child->height_fraction);
		if (child_h < 10) {
			child_h = 10;
		}

		child->pending.x = 0;
		child->pending.y = y;
		child->pending.width = col->pending.width;
		child->pending.height = child_h;
		y += child_h + gap;
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
	col->pending.scroll_y = edge_snap_vert(win->pending.y,
		win->pending.height,
		col->pending.scroll_y, area_h, max_y);
	node_set_dirty(&col->node);
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
