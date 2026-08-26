#include <math.h>
#include "floating_snap.h"
#include "log.h"
#include "sway/tree/container.h"
#include "sway/tree/workspace.h"

static void add_cand(struct floating_snap_cand *cands, int *n, double edge,
		const struct wlr_box *hl, enum floating_snap_role role) {
	if (*n < FLOATING_SNAP_MAX_CANDS) {
		cands[*n].edge = edge;
		cands[*n].hl = *hl;
		cands[*n].role = role;
		(*n)++;
	}
}

void floating_snap_collect(struct sway_container *skip,
		struct sway_workspace *ws, bool horizontal,
		const struct wlr_box *ws_box,
		struct floating_snap_cand *cands, int *n) {
	int gap = ws->gaps_inner;

	// Workspace usable-area edge candidates (ws_box is already gap-inset)
	struct wlr_box edge_hl = {0}; // zero-size: no indicator for ws edges
	{
		double lx = ws_box->x;
		double rx = ws_box->x + ws_box->width;
		double ty = ws_box->y;
		double by = ws_box->y + ws_box->height;
		sway_log(SWAY_DEBUG, "[FSNAP] ws-edge cands axis=%s "
			"L=%.0f R=%.0f T=%.0f B=%.0f",
			horizontal ? "x" : "y", lx, rx, ty, by);
		if (horizontal) {
			add_cand(cands, n, lx, &edge_hl, FLOATING_SNAP_LO);
			add_cand(cands, n, rx, &edge_hl, FLOATING_SNAP_HI);
		} else {
			add_cand(cands, n, ty, &edge_hl, FLOATING_SNAP_LO);
			add_cand(cands, n, by, &edge_hl, FLOATING_SNAP_HI);
		}
	}

	for (int i = 0; i < ws->floating->length; i++) {
		struct sway_container *f = ws->floating->items[i];
		if (f == skip || f->minimized) {
			continue;
		}
		struct wlr_box fb = {
			f->pending.x, f->pending.y,
			f->pending.width, f->pending.height,
		};
		sway_log(SWAY_DEBUG, "[FSNAP] floater con=%p id=%zu "
			"box={%d,%d,%d,%d}", (void *)f, f->node.id,
			fb.x, fb.y, fb.width, fb.height);
		if (horizontal) {
			struct wlr_box hl = { fb.x, fb.y,
				FLOATING_SNAP_EDGE_STRIP, fb.height };
			add_cand(cands, n, (double)fb.x - gap, &hl,
				FLOATING_SNAP_HI);
			sway_log(SWAY_DEBUG, "[FSNAP] cand axis=x id=%zu "
				"edge=%.0f LO-stack hl={%d,%d,%d,%d}",
				f->node.id, (double)fb.x - gap,
				hl.x, hl.y, hl.width, hl.height);

			struct wlr_box hl2 = { fb.x + fb.width
				- FLOATING_SNAP_EDGE_STRIP, fb.y,
				FLOATING_SNAP_EDGE_STRIP, fb.height };
			add_cand(cands, n, (double)(fb.x + fb.width) + gap, &hl2,
				FLOATING_SNAP_LO);
			sway_log(SWAY_DEBUG, "[FSNAP] cand axis=x id=%zu "
				"edge=%.0f HI-stack hl={%d,%d,%d,%d}",
				f->node.id, (double)(fb.x + fb.width) + gap,
				hl2.x, hl2.y, hl2.width, hl2.height);

			add_cand(cands, n, (double)fb.x, &hl,
				FLOATING_SNAP_LO);
			sway_log(SWAY_DEBUG, "[FSNAP] cand axis=x id=%zu "
				"edge=%.0f align-L hl={%d,%d,%d,%d}",
				f->node.id, (double)fb.x,
				hl.x, hl.y, hl.width, hl.height);

			add_cand(cands, n, (double)(fb.x + fb.width), &hl2,
				FLOATING_SNAP_HI);
			sway_log(SWAY_DEBUG, "[FSNAP] cand axis=x id=%zu "
				"edge=%.0f align-R hl={%d,%d,%d,%d}",
				f->node.id, (double)(fb.x + fb.width),
				hl2.x, hl2.y, hl2.width, hl2.height);

			double fcenter = fb.x + fb.width / 2.0;
			struct wlr_box chl = { (int)(fcenter - FLOATING_SNAP_EDGE_STRIP / 2.0),
				fb.y, FLOATING_SNAP_EDGE_STRIP, fb.height };
			add_cand(cands, n, fcenter, &chl,
				FLOATING_SNAP_CENTER);
			sway_log(SWAY_DEBUG, "[FSNAP] cand axis=x id=%zu "
				"edge=%.0f CENTER hl={%d,%d,%d,%d}",
				f->node.id, fcenter,
				chl.x, chl.y, chl.width, chl.height);
		} else {
			struct wlr_box hl = { fb.x, fb.y, fb.width,
				FLOATING_SNAP_EDGE_STRIP };
			add_cand(cands, n, (double)fb.y - gap, &hl,
				FLOATING_SNAP_HI);
			sway_log(SWAY_DEBUG, "[FSNAP] cand axis=y id=%zu "
				"edge=%.0f HI-stack hl={%d,%d,%d,%d}",
				f->node.id, (double)fb.y - gap,
				hl.x, hl.y, hl.width, hl.height);

			struct wlr_box hl2 = { fb.x,
				fb.y + fb.height - FLOATING_SNAP_EDGE_STRIP,
				fb.width, FLOATING_SNAP_EDGE_STRIP };
			add_cand(cands, n, (double)(fb.y + fb.height) + gap, &hl2,
				FLOATING_SNAP_LO);
			sway_log(SWAY_DEBUG, "[FSNAP] cand axis=y id=%zu "
				"edge=%.0f LO-stack hl={%d,%d,%d,%d}",
				f->node.id, (double)(fb.y + fb.height) + gap,
				hl2.x, hl2.y, hl2.width, hl2.height);

			add_cand(cands, n, (double)fb.y, &hl,
				FLOATING_SNAP_LO);
			sway_log(SWAY_DEBUG, "[FSNAP] cand axis=y id=%zu "
				"edge=%.0f align-T hl={%d,%d,%d,%d}",
				f->node.id, (double)fb.y,
				hl.x, hl.y, hl.width, hl.height);

			add_cand(cands, n, (double)(fb.y + fb.height), &hl2,
				FLOATING_SNAP_HI);
			sway_log(SWAY_DEBUG, "[FSNAP] cand axis=y id=%zu "
				"edge=%.0f align-B hl={%d,%d,%d,%d}",
				f->node.id, (double)(fb.y + fb.height),
				hl2.x, hl2.y, hl2.width, hl2.height);

			double fcenter = fb.y + fb.height / 2.0;
			struct wlr_box chl = { fb.x,
				(int)(fcenter - FLOATING_SNAP_EDGE_STRIP / 2.0),
				fb.width, FLOATING_SNAP_EDGE_STRIP };
			add_cand(cands, n, fcenter, &chl,
				FLOATING_SNAP_CENTER);
			sway_log(SWAY_DEBUG, "[FSNAP] cand axis=y id=%zu "
				"edge=%.0f CENTER hl={%d,%d,%d,%d}",
				f->node.id, fcenter,
				chl.x, chl.y, chl.width, chl.height);
		}
	}
}

bool floating_snap_cand_valid(const struct floating_snap_cand *c,
		enum floating_snap_role moving_role, double moving_edge_ref,
		int threshold) {
	if (c->role != moving_role) {
		return false;
	}
	int eff_thr = threshold;
	if (c->role == FLOATING_SNAP_CENTER)
		eff_thr = threshold * 2;
	return fabs(c->edge - moving_edge_ref) <= eff_thr;
}
