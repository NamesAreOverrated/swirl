#include <wlr/types/wlr_cursor.h>
#include <wlr/util/edges.h>
#include "sway/commands.h"
#include "sway/desktop/transaction.h"
#include "sway/input/cursor.h"
#include "sway/input/seat.h"
#include "sway/tree/arrange.h"
#include "sway/tree/container.h"
#include "sway/tree/view.h"

struct seatop_resize_tiling_event {
	struct sway_container *con;    // leaf container

	// con, or ancestor of con which will be resized horizontally/vertically
	struct sway_container *h_con;
	struct sway_container *v_con;

	enum wlr_edges edge;
	enum wlr_edges edge_x, edge_y;
	double ref_lx, ref_ly;         // cursor's x/y at start of op
	double h_con_orig_width;       // width of the horizontal ancestor at start
	double v_con_orig_height;      // height of the vertical ancestor at start
};

static void handle_button(struct sway_seat *seat, uint32_t time_msec,
		struct wlr_input_device *device, uint32_t button,
		enum wl_pointer_button_state state) {
	struct seatop_resize_tiling_event *e = seat->seatop_data;

	if (seat->cursor->pressed_button_count == 0) {
		if (e->h_con) {
			container_set_resizing(e->h_con, false);
			if (e->h_con->pending.parent) {
				arrange_container(e->h_con->pending.parent);
			} else {
				arrange_workspace(e->h_con->pending.workspace);
			}
		}
		if (e->v_con) {
			container_set_resizing(e->v_con, false);
			if (e->v_con->pending.parent) {
				arrange_container(e->v_con->pending.parent);
			} else {
				arrange_workspace(e->v_con->pending.workspace);
			}
		}
		transaction_commit_dirty();
		seatop_begin_default(seat);
	}
}

static void handle_pointer_motion(struct sway_seat *seat, uint32_t time_msec) {
	struct seatop_resize_tiling_event *e = seat->seatop_data;
	int amount_x = 0;
	int amount_y = 0;
	int moved_x = seat->cursor->cursor->x - e->ref_lx;
	int moved_y = seat->cursor->cursor->y - e->ref_ly;

	if (e->h_con) {
		if (e->edge & WLR_EDGE_LEFT) {
			amount_x = (e->h_con_orig_width - moved_x) - e->h_con->pending.width;
		} else if (e->edge & WLR_EDGE_RIGHT) {
			amount_x = (e->h_con_orig_width + moved_x) - e->h_con->pending.width;
		}
	}
	if (e->v_con) {
		if (e->edge & WLR_EDGE_TOP) {
			amount_y = (e->v_con_orig_height - moved_y) - e->v_con->pending.height;
		} else if (e->edge & WLR_EDGE_BOTTOM) {
			amount_y = (e->v_con_orig_height + moved_y) - e->v_con->pending.height;
		}
	}

	// Combined edges (e.g. LEFT|TOP) mean $mod+right-click — skip sibling.
	// Single edge (e.g. WLR_EDGE_RIGHT) means border drag — adjust sibling.
	uint32_t edge_x = (e->edge & (WLR_EDGE_TOP | WLR_EDGE_BOTTOM)) ?
		WLR_EDGE_NONE : e->edge_x;
	uint32_t edge_y = (e->edge & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT)) ?
		WLR_EDGE_NONE : e->edge_y;

	if (amount_x != 0) {
		if (edge_x != WLR_EDGE_NONE) {
			container_resize_tiled(e->h_con, edge_x, amount_x);
		} else {
			tiled_resize_horizontal_px(e->h_con, WLR_EDGE_NONE, amount_x);
		}
	}
	if (amount_y != 0) {
		if (edge_y != WLR_EDGE_NONE) {
			container_resize_tiled(e->v_con, edge_y, amount_y);
		} else {
			tiled_resize_vertical_px(e->v_con, WLR_EDGE_NONE, amount_y);
		}
	}
	transaction_commit_dirty();
}

static void handle_unref(struct sway_seat *seat, struct sway_container *con) {
	struct seatop_resize_tiling_event *e = seat->seatop_data;
	if (e->con == con) {
		seatop_begin_default(seat);
	}
}

static const struct sway_seatop_impl seatop_impl = {
	.button = handle_button,
	.pointer_motion = handle_pointer_motion,
	.unref = handle_unref,
};

void seatop_begin_resize_tiling(struct sway_seat *seat,
		struct sway_container *con, enum wlr_edges edge) {
	seatop_end(seat);

	struct seatop_resize_tiling_event *e =
		calloc(1, sizeof(struct seatop_resize_tiling_event));
	if (!e) {
		return;
	}
	e->con = con;
	e->edge = edge;

	e->ref_lx = seat->cursor->cursor->x;
	e->ref_ly = seat->cursor->cursor->y;

	bool is_mod_rt = (edge & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT)) &&
		(edge & (WLR_EDGE_TOP | WLR_EDGE_BOTTOM));

	if (edge & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT)) {
		e->edge_x = edge & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
		if (is_mod_rt) {
			e->h_con = container_toplevel_ancestor(e->con);
		} else {
			e->h_con = container_find_resize_parent(e->con, e->edge_x);
		}

		if (e->h_con) {
			container_set_resizing(e->h_con, true);
			e->h_con_orig_width = e->h_con->pending.width;
		}
	}
	if (edge & (WLR_EDGE_TOP | WLR_EDGE_BOTTOM)) {
		e->edge_y = edge & (WLR_EDGE_TOP | WLR_EDGE_BOTTOM);
		if (is_mod_rt) {
			e->v_con = e->con;
		} else {
			e->v_con = container_find_resize_parent(e->con, e->edge_y);
		}

		if (e->v_con) {
			container_set_resizing(e->v_con, true);
			e->v_con_orig_height = e->v_con->pending.height;
		}
	}

	seat->seatop_impl = &seatop_impl;
	seat->seatop_data = e;

	transaction_commit_dirty();
	wlr_seat_pointer_notify_clear_focus(seat->wlr_seat);
}
