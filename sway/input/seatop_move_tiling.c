#include <limits.h>
#include <math.h>
#include <time.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/edges.h>
#include "log.h"
#include "sway/desktop/transaction.h"
#include "sway/input/cursor.h"
#include "sway/input/seat.h"
#include "sway/ipc-server.h"
#include "sway/output.h"
#include "sway/tree/arrange.h"
#include "sway/tree/container.h"
#include "sway/tree/node.h"
#include "sway/tree/view.h"
#include "sway/tree/workspace.h"

// Thickness of the dropzone when dragging to the edge of a layout container
#define DROP_LAYOUT_BORDER 30

// Thickness of indicator when dropping onto a titlebar.  This should be a
// multiple of 2.
#define DROP_SPLIT_INDICATOR 10

struct seatop_move_tiling_event {
	struct sway_container *con;
	struct sway_node *target_node;
	enum wlr_edges target_edge;
	double ref_lx, ref_ly; // cursor's x/y at start of op
	bool threshold_reached;
	bool split_target;
	bool insert_after_target;
	struct wlr_scene_rect *indicator_rect;
	struct wl_listener target_node_destroy; // Clears target_node if destroyed mid-drag
};

static void handle_end(struct sway_seat *seat) {
	struct seatop_move_tiling_event *e = seat->seatop_data;
	wl_list_remove(&e->target_node_destroy.link);
	wlr_scene_node_destroy(&e->indicator_rect->node);
	e->indicator_rect = NULL;
}

// Update target_node and its destruction listener
static void set_target_node(struct seatop_move_tiling_event *e, struct sway_node *node) {
	if (e->target_node == node) {
		return;
	}
	wl_list_remove(&e->target_node_destroy.link);
	e->target_node = node;
	if (node) {
		wl_signal_add(&node->events.destroy, &e->target_node_destroy);
	} else {
		wl_list_init(&e->target_node_destroy.link);
	}
}

static void handle_target_node_destroy(struct wl_listener *listener, void *data) {
	struct seatop_move_tiling_event *e =
		wl_container_of(listener, e, target_node_destroy);
	set_target_node(e, NULL);
}

static void handle_motion_prethreshold(struct sway_seat *seat) {
	struct seatop_move_tiling_event *e = seat->seatop_data;
	double cx = seat->cursor->cursor->x;
	double cy = seat->cursor->cursor->y;
	double sx = e->ref_lx;
	double sy = e->ref_ly;

	// Get the scaled threshold for the output. Even if the operation goes
	// across multiple outputs of varying scales, just use the scale for the
	// output that the cursor is currently on for simplicity.
	struct wlr_output *wlr_output = wlr_output_layout_output_at(
			root->output_layout, cx, cy);
	double output_scale = wlr_output ? wlr_output->scale : 1;
	double threshold = config->tiling_drag_threshold * output_scale;
	threshold *= threshold;

	// If the threshold has been exceeded, start the actual drag
	if ((cx - sx) * (cx - sx) + (cy - sy) * (cy - sy) > threshold) {
		wlr_scene_node_set_enabled(&e->indicator_rect->node, true);
		e->threshold_reached = true;
		cursor_set_image(seat->cursor, "grab", NULL);
	}
}

static void split_border(double pos, int offset, int len, int n_children,
		int avoid, int *out_pos, bool *out_after) {
	int region = 2 * n_children * (pos - offset) / len;
	// If the cursor is over the right side of a left-adjacent titlebar, or the
	// left side of a right-adjacent titlebar, it's position when dropped will
	// be the same.  To avoid this, shift the region for adjacent containers.
	if (avoid >= 0) {
		if (region == 2 * avoid - 1 || region == 2 * avoid) {
			region--;
		} else if (region == 2 * avoid + 1 || region == 2 * avoid + 2) {
			region++;
		}
	}

	int child_index = (region + 1) / 2;
	*out_after = region % 2;
	// When dropping at the beginning or end of a container, show the drop
	// region within the container boundary, otherwise show it on top of the
	// border between two titlebars.
	if (child_index == 0) {
		*out_pos = offset;
	} else if (child_index == n_children) {
		*out_pos = offset + len - DROP_SPLIT_INDICATOR;
	} else {
		*out_pos = offset + child_index * len / n_children -
			DROP_SPLIT_INDICATOR / 2;
	}
}

// Rendered, scene-space box of a node. Unlike node_get_box (pending geometry,
// which is speculatively re-arranged during a drag while the scene stays
// committed), this reflects what is actually on screen under the cursor.
static void node_get_screen_box(struct sway_node *node, struct wlr_box *box) {
	if (node->type == N_CONTAINER) {
		container_get_screen_box(node->sway_container, box);
	} else {
		node_get_box(node, box);
	}
}

static bool split_titlebar(struct sway_node *node, struct sway_container *avoid,
		struct wlr_cursor *cursor, struct wlr_box *title_box, bool *after) {
	struct sway_container *con = node->sway_container;
	struct sway_node *parent = con->pending.parent ?
		&con->pending.parent->node : NULL;
	int title_height = container_titlebar_height();
	struct wlr_box box;
	int n_children, avoid_index;
	enum sway_container_layout layout =
		parent ? node_get_layout(parent) : L_NONE;
	if (layout == L_TABBED || layout == L_STACKED) {
		node_get_screen_box(parent, &box);
		n_children = node_get_children(parent)->length;
		avoid_index = list_find(node_get_children(parent), avoid);
	} else {
		node_get_screen_box(node, &box);
		n_children = 1;
		avoid_index = -1;
	}
	if (layout == L_STACKED && cursor->y < box.y + title_height * n_children) {
		// Drop into stacked titlebars.
		title_box->width = box.width;
		title_box->height = DROP_SPLIT_INDICATOR;
		title_box->x = box.x;
		split_border(cursor->y, box.y, title_height * n_children,
			n_children, avoid_index, &title_box->y, after);
		return true;
	} else if (layout == L_VERT && cursor->y < box.y + title_height) {
		// Drop into a vertical column's window titlebar: the drop inserts the
		// window above/below the target, so show a horizontal divider spanning
		// the whole column width and derive `after` from the cursor's vertical
		// position (within this window's titlebar).
		struct wlr_box col_box = box;
		if (con->pending.parent) {
			container_get_screen_box(con->pending.parent, &col_box);
		}
		title_box->width = col_box.width;
		title_box->height = DROP_SPLIT_INDICATOR;
		title_box->x = col_box.x;
		split_border(cursor->y, box.y, title_height, n_children,
			avoid_index, &title_box->y, after);
		return true;
	} else if (layout != L_STACKED && cursor->y < box.y + title_height) {
		// Drop into side-by-side titlebars.
		title_box->width = DROP_SPLIT_INDICATOR;
		title_box->height = title_height;
		title_box->y = box.y;
		split_border(cursor->x, box.x, box.width, n_children,
			avoid_index, &title_box->x, after);
		return true;
	}
	return false;
}

static void update_indicator(struct seatop_move_tiling_event *e, struct wlr_box *box) {
	sway_log(SWAY_DEBUG, "DRAG: indicator box=%d,%d %dx%d target=%s edge=%d",
		box->x, box->y, box->width, box->height,
		e->target_node ? node_type_to_str(e->target_node->type) : "none",
		e->target_edge);
	wlr_scene_node_set_enabled(&e->indicator_rect->node, true);
	wlr_scene_node_set_position(&e->indicator_rect->node, box->x, box->y);
	wlr_scene_rect_set_size(e->indicator_rect, box->width, box->height);
}

static void handle_motion_postthreshold(struct sway_seat *seat) {
	struct seatop_move_tiling_event *e = seat->seatop_data;
	e->split_target = false;
	struct wlr_surface *surface = NULL;
	double sx, sy;
	struct sway_cursor *cursor = seat->cursor;

	struct sway_node *node = node_at_coords(seat,
			cursor->cursor->x, cursor->cursor->y, &surface, &sx, &sy);

	if (!node) {
		sway_log(SWAY_DEBUG, "DRAG: no node under cursor");
		set_target_node(e, NULL);
		e->target_edge = WLR_EDGE_NONE;
		wlr_scene_node_set_enabled(&e->indicator_rect->node, false);
		return;
	}

	if (node->type == N_WORKSPACE) {
		sway_log(SWAY_DEBUG, "DRAG: empty workspace");
		set_target_node(e, node);
		e->target_edge = WLR_EDGE_NONE;

		struct wlr_box drop_box;
		workspace_get_box(node->sway_workspace, &drop_box);
		update_indicator(e, &drop_box);
		return;
	}

	// Deny moving within own workspace if this is the only child
	struct sway_container *con = node->sway_container;
	if (workspace_num_tiling_views(e->con->pending.workspace) == 1 &&
			con->pending.workspace == e->con->pending.workspace) {
		sway_log(SWAY_DEBUG, "DRAG: only child in workspace, deny");
		set_target_node(e, NULL);
		e->target_edge = WLR_EDGE_NONE;
		wlr_scene_node_set_enabled(&e->indicator_rect->node, false);
		return;
	}

	// A floating window is not a valid tiling drop target: deny it so the drop
	// cancels instead of converting the dragged window to floating (which had
	// no visual cue and was surprising). Also avoids routing through the
	// tiling insert paths below, which NULL-deref on a leaf view's missing
	// children list.
	if (container_is_floating_or_child(con)) {
		sway_log(SWAY_DEBUG, "DRAG: floating target, deny");
		set_target_node(e, NULL);
		e->target_edge = WLR_EDGE_NONE;
		wlr_scene_node_set_enabled(&e->indicator_rect->node, false);
		return;
	}

	// Starting point for indicator geometry; split_titlebar and the surface
	// divider overwrite it with rendered scene-space boxes, which are directly
	// comparable to the cursor position.
	struct wlr_box drop_box = {
		.x = con->pending.content_x,
		.y = con->pending.content_y,
		.width = con->pending.content_width,
		.height = con->pending.content_height,
	};

	// Check if the cursor is over a titlebar only if the destination
	// container is not a descendant of the source container.
	if (!surface && !container_has_ancestor(con, e->con) &&
			split_titlebar(node, e->con, cursor->cursor,
				&drop_box, &e->insert_after_target)) {
		if (con == e->con) {
			sway_log(SWAY_DEBUG, "DRAG: titlebar drop on self, deny");
			set_target_node(e, NULL);
			e->target_edge = WLR_EDGE_NONE;
			wlr_scene_node_set_enabled(&e->indicator_rect->node, false);
			return;
		}
		e->split_target = true;
		sway_log(SWAY_DEBUG, "DRAG: titlebar drop, insert_after_target=%d",
			e->insert_after_target);
		set_target_node(e, node);
		e->target_edge = WLR_EDGE_NONE;
		update_indicator(e, &drop_box);
		return;
	}

	// Traverse the ancestors, trying to find a layout container perpendicular
	// to the edge. Eg. close to the top or bottom of a horiz layout. The
	// thresholds come from the hovered window's rendered scene box
	// (container_get_screen_box), never from pending layout coords (the
	// dragged copy animates in pending-space while its scene geometry stays
	// put).
	struct wlr_box win_box;
	node_get_screen_box(node, &win_box);
	int thresh_top = win_box.y + DROP_LAYOUT_BORDER;
	int thresh_bottom = win_box.y + win_box.height - DROP_LAYOUT_BORDER;
	int thresh_left = win_box.x + DROP_LAYOUT_BORDER;
	int thresh_right = win_box.x + win_box.width - DROP_LAYOUT_BORDER;
	sway_log(SWAY_DEBUG, "DRAG: thresh t=%d b=%d l=%d r=%d cur=(%.0f,%.0f)",
		thresh_top, thresh_bottom, thresh_left, thresh_right,
		cursor->cursor->x, cursor->cursor->y);
	while (con) {
		enum wlr_edges edge = WLR_EDGE_NONE;
		enum sway_container_layout layout = container_parent_layout(con);
		sway_log(SWAY_DEBUG, "DRAG: ancestor loop con=%p view=%d layout=%d",
			(void*)con, !!con->view, layout);
		struct wlr_box box;
		node_get_screen_box(node_get_parent(&con->node), &box);
		int p_x = box.x, p_y = box.y, p_w = box.width, p_h = box.height;
		if (layout == L_HORIZ || layout == L_TABBED) {
			if (cursor->cursor->y < thresh_top) {
				edge = WLR_EDGE_TOP;
				if (thresh_top < p_y) thresh_top = p_y;
				box.y = thresh_top - DROP_LAYOUT_BORDER;
				if (box.y < p_y) box.y = p_y;
				box.height = p_y + p_h - box.y;
				if (box.height > DROP_LAYOUT_BORDER) box.height = DROP_LAYOUT_BORDER;
			} else if (cursor->cursor->y > thresh_bottom) {
				edge = WLR_EDGE_BOTTOM;
				if (thresh_bottom > p_y + p_h) thresh_bottom = p_y + p_h;
				box.y = thresh_bottom;
				box.height = p_y + p_h - box.y;
				if (box.height > DROP_LAYOUT_BORDER) box.height = DROP_LAYOUT_BORDER;
				if (box.height <= 0) {
					box.y = p_y + p_h - DROP_LAYOUT_BORDER;
					box.height = DROP_LAYOUT_BORDER;
				}
			}
		} else if (layout == L_VERT || layout == L_STACKED) {
			if (cursor->cursor->x < thresh_left) {
				edge = WLR_EDGE_LEFT;
				if (thresh_left < p_x) thresh_left = p_x;
				box.x = thresh_left - DROP_LAYOUT_BORDER;
				if (box.x < p_x) box.x = p_x;
				box.width = p_x + p_w - box.x;
				if (box.width > DROP_LAYOUT_BORDER) box.width = DROP_LAYOUT_BORDER;
			} else if (cursor->cursor->x > thresh_right) {
				edge = WLR_EDGE_RIGHT;
				if (thresh_right > p_x + p_w) thresh_right = p_x + p_w;
				box.x = thresh_right;
				box.width = p_x + p_w - box.x;
				if (box.width > DROP_LAYOUT_BORDER) box.width = DROP_LAYOUT_BORDER;
				if (box.width <= 0) {
					box.x = p_x + p_w - DROP_LAYOUT_BORDER;
					box.width = DROP_LAYOUT_BORDER;
				}
			}
		}
		if (edge) {
			struct sway_node *parent = node_get_parent(&con->node);
			sway_log(SWAY_DEBUG, "DRAG: ancestor edge=%d parent_type=%d",
				edge, parent ? (int)parent->type : -1);
			set_target_node(e, parent);
			if (e->target_node && (e->target_node == &e->con->node ||
					node_has_ancestor(e->target_node, &e->con->node))) {
				sway_log(SWAY_DEBUG, "DRAG: target is self/ancestor, bumping to con's parent");
				set_target_node(e, node_get_parent(&e->con->node));
			}
			e->target_edge = edge;

			// Anchor workspace-edge strips flush against the workspace's true
			// edges. Without this, the strip is positioned off the hovered
			// window's thresholds and floats mid-screen instead of marking
			// where the new column actually lands.
			if (e->target_node && e->target_node->type == N_WORKSPACE) {
				struct wlr_box ws_box;
				workspace_get_box(e->target_node->sway_workspace, &ws_box);
				switch (e->target_edge) {
				case WLR_EDGE_TOP:
					box.x = ws_box.x;
					box.y = ws_box.y;
					box.width = ws_box.width;
					box.height = DROP_LAYOUT_BORDER;
					break;
				case WLR_EDGE_BOTTOM:
					box.x = ws_box.x;
					box.y = ws_box.y + ws_box.height - DROP_LAYOUT_BORDER;
					box.width = ws_box.width;
					box.height = DROP_LAYOUT_BORDER;
					break;
				case WLR_EDGE_LEFT:
					box.x = ws_box.x;
					box.y = ws_box.y;
					box.width = DROP_LAYOUT_BORDER;
					box.height = ws_box.height;
					break;
				case WLR_EDGE_RIGHT:
					box.x = ws_box.x + ws_box.width - DROP_LAYOUT_BORDER;
					box.y = ws_box.y;
					box.width = DROP_LAYOUT_BORDER;
					box.height = ws_box.height;
					break;
				default:
					break;
				}
			}

			update_indicator(e, &box);
			return;
		}
		con = con->pending.parent;
		if (!con) {
			break;
		}
		if (!con->pending.parent && con->pending.layout == L_VERT) {
			sway_log(SWAY_DEBUG, "DRAG: column boundary reached, stopping ancestor walk");
			break;
		}
	}

	// Use the hovered view - but we must be over the actual surface
	con = node->sway_container;
	if (!con->view || !con->view->surface || node == &e->con->node
			|| node_has_ancestor(node, &e->con->node)) {
		sway_log(SWAY_DEBUG, "DRAG: surface fallback skip (no view/self/ancestor)");
		set_target_node(e, NULL);
		e->target_edge = WLR_EDGE_NONE;
		return;
	}

	// All surfaces are leaves, so the cursor is over the hovered window's
	// content. Derive a divider edge from the cursor's position inside that
	// window; the divider spans the window's column, read via
	// container_get_screen_box so it stays glued to the rendered scene
	// geometry even while the dragged copy animates in pending-space.
	enum sway_container_layout layout = container_parent_layout(con);
	if (layout == L_HORIZ) {
		// Nested horizontal split: horizontal divider.
		if (cursor->cursor->x < win_box.x + win_box.width / 2) {
			e->target_edge = WLR_EDGE_LEFT;
		} else {
			e->target_edge = WLR_EDGE_RIGHT;
		}
	} else if (cursor->cursor->y < win_box.y + win_box.height / 2) {
		e->target_edge = WLR_EDGE_TOP;
	} else {
		e->target_edge = WLR_EDGE_BOTTOM;
	}

	struct sway_container *col = container_toplevel_ancestor(con);
	struct wlr_box col_box;
	container_get_screen_box(col, &col_box);
	set_target_node(e, node);

	if (e->target_edge == WLR_EDGE_TOP) {
		drop_box.x = col_box.x;
		drop_box.y = win_box.y;
		drop_box.width = col_box.width;
		drop_box.height = DROP_SPLIT_INDICATOR;
	} else if (e->target_edge == WLR_EDGE_BOTTOM) {
		drop_box.x = col_box.x;
		drop_box.y = win_box.y + win_box.height;
		drop_box.width = col_box.width;
		drop_box.height = DROP_SPLIT_INDICATOR;
	} else if (e->target_edge == WLR_EDGE_LEFT) {
		drop_box.x = win_box.x;
		drop_box.y = col_box.y;
		drop_box.width = DROP_SPLIT_INDICATOR;
		drop_box.height = col_box.height;
	} else {
		drop_box.x = win_box.x + win_box.width;
		drop_box.y = col_box.y;
		drop_box.width = DROP_SPLIT_INDICATOR;
		drop_box.height = col_box.height;
	}
	sway_log(SWAY_DEBUG,
		"DRAG: surface divider edge=%d target=%d,%d %dx%d win=%d,%d %dx%d cur=(%.0f,%.0f)",
		e->target_edge, col_box.x, col_box.y, col_box.width, col_box.height,
		win_box.x, win_box.y, win_box.width, win_box.height,
		cursor->cursor->x, cursor->cursor->y);
	update_indicator(e, &drop_box);

}

static void handle_pointer_motion(struct sway_seat *seat, uint32_t time_msec) {
	struct seatop_move_tiling_event *e = seat->seatop_data;
	if (e->threshold_reached) {
		handle_motion_postthreshold(seat);
	} else {
		handle_motion_prethreshold(seat);
	}
	transaction_commit_dirty();
}

static void finalize_move(struct sway_seat *seat) {
	struct seatop_move_tiling_event *e = seat->seatop_data;

	if (!e->target_node) {
		sway_log(SWAY_DEBUG, "DRAG: finalize no target");
		seatop_begin_default(seat);
		return;
	}

	struct sway_container *con = e->con;
	struct sway_workspace *old_ws = con->pending.workspace;
	struct sway_node *target_node = e->target_node;
	struct sway_workspace *new_ws = target_node->type == N_WORKSPACE ?
		target_node->sway_workspace :
		target_node->sway_container->pending.workspace;

	sway_log(SWAY_DEBUG, "DRAG: finalize con=%p target_type=%d target_edge=%d split=%d",
		(void*)con, target_node->type, e->target_edge, e->split_target);

	if (target_node->type == N_WORKSPACE) {
		sway_log(SWAY_DEBUG, "DRAG: finalize -> workspace_add_tiling (workspace drop)");
		con = workspace_add_tiling(new_ws, con);
	} else {
		struct sway_container *target = target_node->sway_container;

		// Defensive: a floating target can never be a tiling insert target.
		// handle_motion_postthreshold denies floating windows, but if any
		// path set one, cancel rather than NULL-deref in workspace_insert_window.
		if (container_is_floating_or_child(target)) {
			sway_log(SWAY_DEBUG, "DRAG: finalize floating target, cancel");
			seatop_begin_default(seat);
			return;
		}

		sway_log(SWAY_DEBUG, "DRAG: finalize -> workspace_insert_window target=%p", (void*)target);
		if (e->split_target) {
			sway_log(SWAY_DEBUG, "DRAG: titlebar finalize target=%p after=%d",
				(void*)target, e->insert_after_target);
			workspace_insert_window(new_ws, con, target,
				WLR_EDGE_TOP, e->insert_after_target);
		} else {
			int after;
			if (e->target_edge == WLR_EDGE_NONE) {
				struct sway_container *t = target_node->sway_container;
				after = seat->cursor->cursor->y
					>= t->pending.y + t->pending.height / 2;
			} else {
				after = e->target_edge != WLR_EDGE_TOP
					&& e->target_edge != WLR_EDGE_LEFT;
			}
			workspace_insert_window(new_ws, con, target,
				e->target_edge, after);
		}
		ipc_event_window(con, "move");
	}

	arrange_workspace(old_ws);
	if (new_ws != old_ws) {
		arrange_workspace(new_ws);
	}

	transaction_commit_dirty();
	seatop_begin_default(seat);
}

static void handle_button(struct sway_seat *seat, uint32_t time_msec,
		struct wlr_input_device *device, uint32_t button,
		enum wl_pointer_button_state state) {
	if (seat->cursor->pressed_button_count == 0) {
		finalize_move(seat);
	}
}

static void handle_tablet_tool_tip(struct sway_seat *seat,
		struct sway_tablet_tool *tool, uint32_t time_msec,
		enum wlr_tablet_tool_tip_state state) {
	if (state == WLR_TABLET_TOOL_TIP_UP) {
		finalize_move(seat);
	}
}

static void handle_unref(struct sway_seat *seat, struct sway_container *con) {
	struct seatop_move_tiling_event *e = seat->seatop_data;
	if (e->target_node == &con->node) { // Drop target
		set_target_node(e, NULL);
	}
	if (e->con == con) { // The container being moved
		seatop_begin_default(seat);
	}
}

static const struct sway_seatop_impl seatop_impl = {
	.button = handle_button,
	.pointer_motion = handle_pointer_motion,
	.tablet_tool_tip = handle_tablet_tool_tip,
	.unref = handle_unref,
	.end = handle_end,
};

void seatop_begin_move_tiling_threshold(struct sway_seat *seat,
		struct sway_container *con) {
	seatop_end(seat);

	struct seatop_move_tiling_event *e =
		calloc(1, sizeof(struct seatop_move_tiling_event));
	if (!e) {
		return;
	}

	const float *indicator = config->border_colors.focused.indicator;
	float color[4] = {
		indicator[0] * .5,
		indicator[1] * .5,
		indicator[2] * .5,
		indicator[3] * .5,
	};
	e->indicator_rect = wlr_scene_rect_create(seat->scene_tree, 0, 0, color);
	if (!e->indicator_rect) {
		free(e);
		return;
	}

	e->con = con;
	e->ref_lx = seat->cursor->cursor->x;
	e->ref_ly = seat->cursor->cursor->y;
	wl_list_init(&e->target_node_destroy.link);
	e->target_node_destroy.notify = handle_target_node_destroy;

	seat->seatop_impl = &seatop_impl;
	seat->seatop_data = e;

	container_raise_floating(con);
	transaction_commit_dirty();
	wlr_seat_pointer_notify_clear_focus(seat->wlr_seat);
}

void seatop_begin_move_tiling(struct sway_seat *seat,
		struct sway_container *con) {
	seatop_begin_move_tiling_threshold(seat, con);
	struct seatop_move_tiling_event *e = seat->seatop_data;
	if (e) {
		e->threshold_reached = true;
		cursor_set_image(seat->cursor, "grab", NULL);
	}
}
