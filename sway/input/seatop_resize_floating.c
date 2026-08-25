#include <math.h>
#include <stdlib.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include "config.h"
#include "log.h"
#include "sway/desktop/transaction.h"
#include "sway/input/cursor.h"
#include "floating_snap.h"
#include "sway/input/seat.h"
#include "sway/tree/arrange.h"
#include "sway/tree/view.h"
#include "sway/tree/workspace.h"
#include "sway/tree/container.h"

struct seatop_resize_floating_event {
	struct sway_container *con;
	enum wlr_edges edge;
	bool preserve_ratio;
	double ref_lx, ref_ly;         // cursor's x/y at start of op
	double ref_width, ref_height;  // container's size at start of op
	double ref_con_lx, ref_con_ly; // container's x/y at start of op
	struct wlr_scene_rect *indicator_rect;
};

// One snap candidate: an edge coordinate plus the rect to highlight when
// this candidate wins (the source window's matching edge strip).
#define FLOATING_SNAP_MAX_CANDS 64

static void indicator_destroy(struct seatop_resize_floating_event *e) {
	if (e->indicator_rect) {
		wlr_scene_node_destroy(&e->indicator_rect->node);
		e->indicator_rect = NULL;
	}
}

static void indicator_show(struct seatop_resize_floating_event *e,
		const struct wlr_box *hl) {
	wlr_scene_node_set_enabled(&e->indicator_rect->node, true);
	wlr_scene_node_set_position(&e->indicator_rect->node, hl->x, hl->y);
	wlr_scene_rect_set_size(e->indicator_rect, hl->width, hl->height);
}

// Snap the moving edge(s) of the resize against nearby workspace/floater
// edges by adjusting the resulting width/height. Only the edges actually
// being dragged participate.
static void snap_floating_resize(struct seatop_resize_floating_event *e,
		struct sway_container *con, double *width, double *height) {
	int threshold = config->floating_snap_threshold;
	struct sway_workspace *ws = con->pending.workspace;
	sway_log(SWAY_DEBUG, "[FSNAP] resize con=%p id=%zu edge=%d "
		"ref_con=(%.0f,%.0f) ref_size=(%.0f,%.0f) pre=(%.0f,%.0f) thr=%d",
		(void *)con, con->node.id, e->edge,
		e->ref_con_lx, e->ref_con_ly,
		e->ref_width, e->ref_height, *width, *height, threshold);
	if (threshold <= 0 || !ws) {
		return;
	}

	bool horiz = e->edge & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
	bool vert = e->edge & (WLR_EDGE_TOP | WLR_EDGE_BOTTOM);

	const struct wlr_box *hl = NULL;

	if (horiz) {
		struct floating_snap_cand cands[FLOATING_SNAP_MAX_CANDS];
		int n = 0;
		floating_snap_collect(con, ws, true, cands, &n);
		// Absolute coordinate of the moving vertical edge.
		bool moving_hi = e->edge & WLR_EDGE_RIGHT;
		double moving = moving_hi
			? e->ref_con_lx + *width
			: e->ref_con_lx + e->ref_width - *width;
		sway_log(SWAY_DEBUG, "[FSNAP] resize-x moving=%.0f (hi=%d) n=%d",
			moving, moving_hi, n);
		for (int i = 0; i < n; i++) {
			if (cands[i].role != (moving_hi ? FLOATING_SNAP_HI : FLOATING_SNAP_LO)) {
				continue;
			}
			double d = cands[i].edge - moving;
			sway_log(SWAY_DEBUG, "[FSNAP]   test x edge=%.0f d=%.0f %s",
				cands[i].edge, d,
				fabs(d) <= threshold ? "IN-THRESHOLD" : "skip");
			if (fabs(d) <= threshold) {
				sway_log(SWAY_DEBUG, "[FSNAP] resize-x SNAP d=%.0f "
					"width %.0f -> %.0f", d, *width,
					moving_hi ? *width + d : *width - d);
				if (moving_hi) {
					*width += d;
				} else {
					*width -= d;
				}
				hl = &cands[i].hl;
				break;
			}
		}
	}

	if (vert) {
		struct floating_snap_cand cands[FLOATING_SNAP_MAX_CANDS];
		int n = 0;
		floating_snap_collect(con, ws, false, cands, &n);
		bool moving_hi = e->edge & WLR_EDGE_BOTTOM;
		double moving = moving_hi
			? e->ref_con_ly + *height
			: e->ref_con_ly + e->ref_height - *height;
		sway_log(SWAY_DEBUG, "[FSNAP] resize-y moving=%.0f (hi=%d) n=%d",
			moving, moving_hi, n);
		for (int i = 0; i < n; i++) {
			if (cands[i].role != (moving_hi ? FLOATING_SNAP_HI : FLOATING_SNAP_LO)) {
				continue;
			}
			double d = cands[i].edge - moving;
			sway_log(SWAY_DEBUG, "[FSNAP]   test y edge=%.0f d=%.0f %s",
				cands[i].edge, d,
				fabs(d) <= threshold ? "IN-THRESHOLD" : "skip");
			if (fabs(d) <= threshold) {
				sway_log(SWAY_DEBUG, "[FSNAP] resize-y SNAP d=%.0f "
					"height %.0f -> %.0f", d, *height,
					moving_hi ? *height + d : *height - d);
				if (moving_hi) {
					*height += d;
				} else {
					*height -= d;
				}
				hl = &cands[i].hl;
				break;
			}
		}
	}

	if (hl) {
		indicator_show(e, hl);
	} else {
		wlr_scene_node_set_enabled(&e->indicator_rect->node, false);
	}
}

static void handle_button(struct sway_seat *seat, uint32_t time_msec,
		struct wlr_input_device *device, uint32_t button,
		enum wl_pointer_button_state state) {
	struct seatop_resize_floating_event *e = seat->seatop_data;
	struct sway_container *con = e->con;

	if (seat->cursor->pressed_button_count == 0) {
		container_set_resizing(con, false);
		if (con->view) {
			// Collapse the reservation to the client's committed surface so the
			// final state can't leave a gap even if the client never commits
			// the last configure (e.g. a grid-aligned terminal).
			view_update_size(con->view);
		}
		arrange_container(con); // Send configure w/o resizing hint
		transaction_commit_dirty();
		seatop_begin_default(seat);
	}
}

static void handle_pointer_motion(struct sway_seat *seat, uint32_t time_msec) {
	struct seatop_resize_floating_event *e = seat->seatop_data;
	struct sway_container *con = e->con;
	enum wlr_edges edge = e->edge;
	struct sway_cursor *cursor = seat->cursor;

	// The amount the mouse has moved since the start of the resize operation
	// Positive is down/right
	double mouse_move_x = cursor->cursor->x - e->ref_lx;
	double mouse_move_y = cursor->cursor->y - e->ref_ly;

	if (edge == WLR_EDGE_TOP || edge == WLR_EDGE_BOTTOM) {
		mouse_move_x = 0;
	}
	if (edge == WLR_EDGE_LEFT || edge == WLR_EDGE_RIGHT) {
		mouse_move_y = 0;
	}

	double grow_width = edge & WLR_EDGE_LEFT ? -mouse_move_x : mouse_move_x;
	double grow_height = edge & WLR_EDGE_TOP ? -mouse_move_y : mouse_move_y;

	if (e->preserve_ratio) {
		double x_multiplier = grow_width / e->ref_width;
		double y_multiplier = grow_height / e->ref_height;
		double max_multiplier = fmax(x_multiplier, y_multiplier);
		grow_width = e->ref_width * max_multiplier;
		grow_height = e->ref_height * max_multiplier;
	}

	struct sway_container_state *state = &con->current;
	double border_width = 0.0;
	if (con->current.border == B_NORMAL || con->current.border == B_PIXEL) {
		border_width = state->border_thickness * 2;
	}
	double border_height = 0.0;
	if (con->current.border == B_NORMAL) {
		border_height += container_titlebar_height();
		border_height += state->border_thickness;
	} else if (con->current.border == B_PIXEL) {
		border_height += state->border_thickness * 2;
	}

	// Determine new width/height, and accommodate for floating min/max values
	double width = e->ref_width + grow_width;
	double height = e->ref_height + grow_height;
	int min_width, max_width, min_height, max_height;
	floating_calculate_constraints(&min_width, &max_width,
		&min_height, &max_height);
	width = fmin(width, max_width - border_width);
	width = fmax(width, min_width + border_width);
	width = fmax(width, 1);
	height = fmin(height, max_height - border_height);
	height = fmax(height, min_height + border_height);
	height = fmax(height, 1);

	// Apply the view's min/max size. `width`/`height` are container dims; the
	// helper clamps the content (borderless) size and we re-add the border.
	if (con->view) {
		double cw = width - border_width;
		double ch = height - border_height;
		container_clamp_content_size(con, &cw, &ch);
		width = cw + border_width;
		height = ch + border_height;
		width = fmax(width, 1);
		height = fmax(height, 1);
	}

	// Snap the dragged edge(s) against nearby workspace/floater edges before
	// deriving the growth vectors so position follows the adjusted size.
	snap_floating_resize(e, con, &width, &height);

	// Recalculate these, in case we hit a min/max limit
	grow_width = width - e->ref_width;
	grow_height = height - e->ref_height;

	// Determine grow x/y values - these are relative to the container's x/y at
	// the start of the resize operation.
	double grow_x = 0, grow_y = 0;
	if (edge & WLR_EDGE_LEFT) {
		grow_x = -grow_width;
	} else if (edge & WLR_EDGE_RIGHT) {
		grow_x = 0;
	} else {
		grow_x = -grow_width / 2;
	}
	if (edge & WLR_EDGE_TOP) {
		grow_y = -grow_height;
	} else if (edge & WLR_EDGE_BOTTOM) {
		grow_y = 0;
	} else {
		grow_y = -grow_height / 2;
	}

	// Determine the amounts we need to bump everything relative to the current
	// size.
	int relative_grow_width = width - con->pending.width;
	int relative_grow_height = height - con->pending.height;
	int relative_grow_x = (e->ref_con_lx + grow_x) - con->pending.x;
	int relative_grow_y = (e->ref_con_ly + grow_y) - con->pending.y;

	// Actually resize stuff
	con->pending.x += relative_grow_x;
	con->pending.y += relative_grow_y;
	con->pending.width += relative_grow_width;
	con->pending.height += relative_grow_height;

	con->pending.content_x += relative_grow_x;
	con->pending.content_y += relative_grow_y;
	con->pending.content_width += relative_grow_width;
	con->pending.content_height += relative_grow_height;

	arrange_container(con);
	transaction_commit_dirty();
}

static void handle_unref(struct sway_seat *seat, struct sway_container *con) {
	struct seatop_resize_floating_event *e = seat->seatop_data;
	if (e->con == con) {
		indicator_destroy(e);
		seatop_begin_default(seat);
	}
}

static void handle_end(struct sway_seat *seat) {
	struct seatop_resize_floating_event *e = seat->seatop_data;
	// seatop_end() frees seatop_data after this returns; only tear down
	// the indicator here.
	indicator_destroy(e);
}

static const struct sway_seatop_impl seatop_impl = {
	.button = handle_button,
	.pointer_motion = handle_pointer_motion,
	.unref = handle_unref,
	.end = handle_end,
};

void seatop_begin_resize_floating(struct sway_seat *seat,
		struct sway_container *con, enum wlr_edges edge) {
	seatop_end(seat);

	struct seatop_resize_floating_event *e =
		calloc(1, sizeof(struct seatop_resize_floating_event));
	if (!e) {
		return;
	}
	e->con = con;

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
	wlr_scene_node_set_enabled(&e->indicator_rect->node, false);

	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat->wlr_seat);
	e->preserve_ratio = keyboard &&
		(wlr_keyboard_get_modifiers(keyboard) & WLR_MODIFIER_SHIFT);

	e->edge = edge == WLR_EDGE_NONE ? WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT : edge;
	e->ref_lx = seat->cursor->cursor->x;
	e->ref_ly = seat->cursor->cursor->y;
	e->ref_con_lx = con->pending.x;
	e->ref_con_ly = con->pending.y;
	e->ref_width = con->pending.width;
	e->ref_height = con->pending.height;

	seat->seatop_impl = &seatop_impl;
	seat->seatop_data = e;

	container_set_resizing(con, true);
	container_raise_floating(con);
	transaction_commit_dirty();

	const char *image = edge == WLR_EDGE_NONE ?
		"se-resize" : wlr_xcursor_get_resize_name(edge);
	cursor_set_image(seat->cursor, image, NULL);
	wlr_seat_pointer_notify_clear_focus(seat->wlr_seat);
}
