#include <math.h>
#include <stdlib.h>
#include <wlr/types/wlr_cursor.h>
#include "config.h"
#include "log.h"
#include "sway/desktop/transaction.h"
#include "sway/input/cursor.h"
#include "floating_snap.h"
#include "sway/input/seat.h"
#include "sway/tree/arrange.h"
#include "sway/tree/container.h"
#include "sway/tree/workspace.h"

#define ZONE_STRIP 10

struct seatop_move_floating_event {
	struct sway_container *con;
	double dx, dy; // cursor offset in container
	struct wlr_scene_rect *indicator_rect;
	// Aero-snap zone under the cursor: while active, the indicator previews
	// it and release commits position+size to it.
	struct wlr_box zone;
	bool zone_active;
};

struct snap_result {
	bool hit;
	double shift;
	struct wlr_box hl;
};

static struct snap_result snap_axis(double lo, double hi,
		double p_lo, double p_hi,
		const struct floating_snap_cand *cands, int n, double threshold) {
	struct snap_result res = {0};
	sway_log(SWAY_DEBUG, "[FSNAP] scan axis lo=%.0f hi=%.0f "
		"perp=[%.0f..%.0f] n=%d thr=%d",
		lo, hi, p_lo, p_hi, n, (int)threshold);
	for (int i = 0; i < n; i++) {
		// Direction-tagged: a candidate only matches the dragged edge it
		// was generated for. AABB check: the source window's perpendicular
		// span must actually overlap ours (± threshold slack) — this is
		// what picks the BOTTOM window when we're beside it, not the top.
		bool perp_ok = cands[i].p_lo < p_hi + threshold &&
			cands[i].p_hi > p_lo - threshold;
		double v = cands[i].for_hi ? hi : lo;
		double d = cands[i].edge - v;
		double mag = fabs(d);
		sway_log(SWAY_DEBUG, "[FSNAP]   test edge=%.0f role=%s "
			"perp=[%.0f..%.0f] d=%.0f %s%s", cands[i].edge,
			cands[i].for_hi ? "hi" : "lo",
			cands[i].p_lo, cands[i].p_hi, d,
			perp_ok ? "perp-OK " : "PERP-REJECT ",
			mag <= threshold ? "IN-THRESHOLD " : "skip ");
		if (!perp_ok) {
			continue;
		}
		if (mag <= threshold && (!res.hit || mag < fabs(res.shift))) {
			res.hit = true;
			res.shift = d;
			res.hl = cands[i].hl;
		}
	}
	if (res.hit) {
		sway_log(SWAY_DEBUG, "[FSNAP] axis HIT shift=%.0f hl={%d,%d,%d,%d}",
			res.shift, res.hl.x, res.hl.y, res.hl.width, res.hl.height);
	} else {
		sway_log(SWAY_DEBUG, "[FSNAP] axis MISS");
	}
	return res;
}

static void indicator_hide(struct seatop_move_floating_event *e) {
	if (e->indicator_rect) {
		wlr_scene_node_set_enabled(&e->indicator_rect->node, false);
	}
}

static void indicator_show(struct seatop_move_floating_event *e,
		const struct wlr_box *hl) {
	wlr_scene_node_set_enabled(&e->indicator_rect->node, true);
	wlr_scene_node_set_position(&e->indicator_rect->node, hl->x, hl->y);
	wlr_scene_rect_set_size(e->indicator_rect, hl->width, hl->height);
}

static void finalize_move(struct sway_seat *seat) {
	struct seatop_move_floating_event *e = seat->seatop_data;
	struct sway_container *con = e->con;
	sway_log(SWAY_DEBUG, "[FSNAP] finalize zone=%d zone_rect={%d,%d,%d,%d} "
		"pending={%.0f,%.0f,%.0f,%.0f}",
		e->zone_active, e->zone.x, e->zone.y,
		e->zone.width, e->zone.height,
		con->pending.x, con->pending.y,
		con->pending.width, con->pending.height);

	if (e->zone_active) {
		// Aero-snap commit: take over the previewed zone.
		const struct wlr_box *z = &e->zone;
		int rel_x = z->x - con->pending.x;
		int rel_y = z->y - con->pending.y;
		int rel_w = z->width - con->pending.width;
		int rel_h = z->height - con->pending.height;

		con->pending.x += rel_x;
		con->pending.y += rel_y;
		con->pending.width += rel_w;
		con->pending.height += rel_h;
		con->pending.content_x += rel_x;
		con->pending.content_y += rel_y;
		con->pending.content_width += rel_w;
		con->pending.content_height += rel_h;

		arrange_container(con);

		// Bottom-anchored zones: pin our bottom edge to the workspace
		// bottom so decoration shrinkage can't leave us visually short.
		struct sway_workspace *ws = con->pending.workspace;
		if (ws) {
			struct wlr_box ws_box;
			workspace_get_box(ws, &ws_box);
			int ws_bottom = ws_box.y + ws_box.height;
			if (e->zone.y + e->zone.height == ws_bottom) {
				con->pending.y = ws_bottom - con->pending.height;
				arrange_container(con);
			}
		}

	} else {
		// We "move" the container to its own location
		// so it discovers its output again.
		container_floating_move_to(con, con->pending.x, con->pending.y);
	}
	con->node.dragging = false;
	transaction_commit_dirty();
	sway_log(SWAY_DEBUG, "[FSNAP] post-commit id=%zu "
		"pending={%d,%d,%d,%d}",
		con->node.id,
		(int)con->pending.x, (int)con->pending.y,
		(int)con->pending.width, (int)con->pending.height);
	sway_log(SWAY_DEBUG, "[FSNAP] watching for geometry changes...");

	seatop_begin_default(seat);
}

static void indicator_destroy(struct seatop_move_floating_event *e) {
	if (e->indicator_rect) {
		wlr_scene_node_destroy(&e->indicator_rect->node);
		e->indicator_rect = NULL;
	}
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

static void handle_pointer_motion(struct sway_seat *seat, uint32_t time_msec) {
	struct seatop_move_floating_event *e = seat->seatop_data;
	struct wlr_cursor *cursor = seat->cursor->cursor;

	double nx = cursor->x - e->dx;
	double ny = cursor->y - e->dy;

	int threshold = config->floating_snap_threshold;
	struct sway_workspace *ws = e->con->pending.workspace;
	sway_log(SWAY_DEBUG, "[FSNAP] motion id=%zu box={%.0f,%.0f,%.0f,%.0f} "
		"cursor=(%.0f,%.0f) raw=(%.0f,%.0f) thr=%d ws=%s",
		e->con->node.id,
		(double)e->con->pending.x, (double)e->con->pending.y,
		(double)e->con->pending.width, (double)e->con->pending.height,
		cursor->x, cursor->y, nx, ny, threshold,
		ws ? ws->name : "none");

	// Aero-snap zones: cursor in an outer strip/corner of the usable area
	// previews a target region; release commits position+size.
	e->zone_active = false;
	if (ws) {
		struct wlr_box ws_box;
		workspace_get_box(ws, &ws_box);
		int zx = ws_box.x, zy = ws_box.y;
		int zw = ws_box.width, zh = ws_box.height;
		int hw = zw / 2, hh = zh / 2;
		bool left = cursor->x <= zx + ZONE_STRIP;
		bool right = cursor->x >= zx + zw - ZONE_STRIP;
		bool top = cursor->y <= zy + ZONE_STRIP;
		bool bottom = cursor->y >= zy + zh - ZONE_STRIP;

		if (left && top) {
			e->zone = (struct wlr_box){ zx, zy, hw, hh };
			e->zone_active = true;
		} else if (right && top) {
			e->zone = (struct wlr_box){ zx + zw - hw, zy, zw - hw, hh };
			e->zone_active = true;
		} else if (left && bottom) {
			e->zone = (struct wlr_box){ zx, zy + zh - hh, hw, zh - hh };
			e->zone_active = true;
		} else if (right && bottom) {
			e->zone = (struct wlr_box){
				zx + zw - hw, zy + zh - hh, zw - hw, zh - hh };
			e->zone_active = true;
		} else if (left) {
			e->zone = (struct wlr_box){ zx, zy, hw, zh };
			e->zone_active = true;
		} else if (right) {
			e->zone = (struct wlr_box){ zx + zw - hw, zy, zw - hw, zh };
			e->zone_active = true;
		} else if (top) {
			e->zone = (struct wlr_box){ zx, zy, zw, zh }; // maximize-equivalent
			e->zone_active = true;
		} else if (bottom) {
			// Height-fill: keep the window's own width and x.
			e->zone = (struct wlr_box){
				e->con->pending.x, zy, e->con->pending.width, zh };
			e->zone_active = true;
		}

		if (e->zone_active) {
			sway_log(SWAY_DEBUG, "[FSNAP] zone HIT rect={%d,%d,%d,%d}",
				e->zone.x, e->zone.y, e->zone.width, e->zone.height);
			indicator_show(e, &e->zone);
		} else {
			sway_log(SWAY_DEBUG, "[FSNAP] zone miss (L=%d R=%d T=%d B=%d)",
				left, right, top, bottom);
		}
	}

	if (!e->zone_active && threshold > 0 && ws && !e->con->minimized) {
		struct floating_snap_cand cx[FLOATING_SNAP_MAX_CANDS], cy[FLOATING_SNAP_MAX_CANDS];
		int ncx = 0, ncy = 0;
		floating_snap_collect(e->con, ws, true, cx, &ncx);
		floating_snap_collect(e->con, ws, false, cy, &ncy);

		struct snap_result rx = snap_axis(nx,
				nx + e->con->pending.width,
				ny, ny + e->con->pending.height,
				cx, ncx, threshold);
		struct snap_result ry = snap_axis(ny,
				ny + e->con->pending.height,
				nx, nx + e->con->pending.width,
				cy, ncy, threshold);

		if (rx.hit) {
			nx += rx.shift;
		}
		if (ry.hit) {
			ny += ry.shift;
		}

		if (rx.hit) {
			indicator_show(e, &rx.hl);
		} else if (ry.hit) {
			indicator_show(e, &ry.hl);
		} else {
			indicator_hide(e);
		}
	}

	sway_log(SWAY_DEBUG, "[FSNAP] apply final=(%.0f,%.0f) zone=%d",
		nx, ny, e->zone_active);
	container_floating_move_to(e->con, nx, ny);
	transaction_commit_dirty();
}

static void handle_unref(struct sway_seat *seat, struct sway_container *con) {
	struct seatop_move_floating_event *e = seat->seatop_data;
	if (e->con == con) {
		e->con->node.dragging = false;
		indicator_destroy(e);
		seatop_begin_default(seat);
	}
}

static void handle_end(struct sway_seat *seat) {
	struct seatop_move_floating_event *e = seat->seatop_data;
	// seatop_end() frees seatop_data after this returns; only tear down
	// the indicator here.
	indicator_destroy(e);
}

static const struct sway_seatop_impl seatop_impl = {
	.button = handle_button,
	.pointer_motion = handle_pointer_motion,
	.tablet_tool_tip = handle_tablet_tool_tip,
	.unref = handle_unref,
	.end = handle_end,
};

void seatop_begin_move_floating(struct sway_seat *seat,
		struct sway_container *con) {
	seatop_end(seat);

	struct sway_cursor *cursor = seat->cursor;
	struct seatop_move_floating_event *e =
		calloc(1, sizeof(struct seatop_move_floating_event));
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
	wlr_scene_node_set_enabled(&e->indicator_rect->node, false);

	e->con = con;
	con->node.dragging = true;
	e->dx = cursor->cursor->x - con->pending.x;
	e->dy = cursor->cursor->y - con->pending.y;

	seat->seatop_impl = &seatop_impl;
	seat->seatop_data = e;

	container_raise_floating(con);
	transaction_commit_dirty();

	cursor_set_image(cursor, "grab", NULL);
	wlr_seat_pointer_notify_clear_focus(seat->wlr_seat);
}
