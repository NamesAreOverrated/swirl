#include <math.h>
#include "floating_snap.h"
#include "log.h"
#include "sway/tree/container.h"
#include "sway/tree/workspace.h"


static void add_cand(struct floating_snap_cand *cands, int *n, double edge,
		const struct wlr_box *hl, bool for_hi,
		double p_lo, double p_hi) {
	if (*n < FLOATING_SNAP_MAX_CANDS) {
		cands[*n].edge = edge;
		cands[*n].hl = *hl;
		cands[*n].for_hi = for_hi;
		cands[*n].p_lo = p_lo;
		cands[*n].p_hi = p_hi;
		(*n)++;
	}
}

void floating_snap_collect(struct sway_container *skip,
		struct sway_workspace *ws, bool horizontal,
		struct floating_snap_cand *cands, int *n) {
	int gap = ws->gaps_inner;
	for (int i = 0; i < ws->floating->length; i++) {
		struct sway_container *f = ws->floating->items[i];
		if (f == skip || f->minimized) {
			continue;
		}
		if (container_is_scratchpad_hidden(f)) {
			sway_log(SWAY_DEBUG, "[FSNAP] cand SKIP hidden-scratchpad "
				"con=%p id=%zu", (void *)f, f->node.id);
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
			double p_lo = fb.y, p_hi = fb.y + fb.height;
			struct wlr_box hl = { fb.x, fb.y,
				FLOATING_SNAP_EDGE_STRIP, fb.height };
			add_cand(cands, n, (double)fb.x - gap, &hl, true, p_lo, p_hi);
			sway_log(SWAY_DEBUG, "[FSNAP] cand axis=x id=%zu edge=%.0f "
				"for_hi hl={%d,%d,%d,%d}", f->node.id,
				(double)fb.x - gap,
				hl.x, hl.y, hl.width, hl.height);
			hl.x = fb.x + fb.width - FLOATING_SNAP_EDGE_STRIP;
			add_cand(cands, n, (double)(fb.x + fb.width) + gap, &hl,
				false, p_lo, p_hi);
			sway_log(SWAY_DEBUG, "[FSNAP] cand axis=x id=%zu edge=%.0f "
				"for_lo hl={%d,%d,%d,%d}", f->node.id,
				(double)(fb.x + fb.width) + gap,
				hl.x, hl.y, hl.width, hl.height);
		} else {
			double p_lo = fb.x, p_hi = fb.x + fb.width;
			struct wlr_box hl = { fb.x, fb.y, fb.width,
				FLOATING_SNAP_EDGE_STRIP };
			add_cand(cands, n, (double)fb.y - gap, &hl, true, p_lo, p_hi);
			sway_log(SWAY_DEBUG, "[FSNAP] cand axis=y id=%zu edge=%.0f "
				"for_hi hl={%d,%d,%d,%d}", f->node.id,
				(double)fb.y - gap,
				hl.x, hl.y, hl.width, hl.height);
			hl.y = fb.y + fb.height - FLOATING_SNAP_EDGE_STRIP;
			add_cand(cands, n, (double)(fb.y + fb.height) + gap, &hl,
				false, p_lo, p_hi);
			sway_log(SWAY_DEBUG, "[FSNAP] cand axis=y id=%zu edge=%.0f "
				"for_lo hl={%d,%d,%d,%d}", f->node.id,
				(double)(fb.y + fb.height) + gap,
				hl.x, hl.y, hl.width, hl.height);
		}
	}
}

bool floating_snap_cand_valid(const struct floating_snap_cand *c,
		bool moving_hi, double moving_edge,
		double p_lo, double p_hi, int threshold) {
	if (c->for_hi != moving_hi) {
		return false;
	}
	// AABB: the source window's perpendicular span must actually overlap
	// ours (± threshold slack).
	if (!(c->p_lo < p_hi + threshold && c->p_hi > p_lo - threshold)) {
		return false;
	}
	return fabs(c->edge - moving_edge) <= threshold;
}
