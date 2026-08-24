#ifndef SWAY_INPUT_FLOATING_SNAP_H
#define SWAY_INPUT_FLOATING_SNAP_H

#include <stdbool.h>
#include "wlr/util/box.h"

struct sway_container;
struct sway_workspace;

// Set during zone commits to prevent container_set_geometry_from_content
// from overwriting zone geometry with client-driven dimensions.

#define FLOATING_SNAP_MAX_CANDS 64
#define FLOATING_SNAP_EDGE_STRIP 8

// One snap candidate: an edge coordinate plus the rect to highlight when
// this candidate wins (the source window's matching edge strip), and the
// source window's perpendicular span for AABB validity checking.
struct floating_snap_cand {
	double edge;
	struct wlr_box hl;
	bool for_hi; // true: matches the dragged window's right/bottom edge
	double p_lo, p_hi;
};

// Collect floater-edge candidates on `horizontal` (or vertical) axis from
// every other floating container on the workspace. Edges are offset by one
// inner gap and direction-tagged so a dragged window's compatible edge is
// unambiguous.
void floating_snap_collect(struct sway_container *skip,
		struct sway_workspace *ws, bool horizontal,
		struct floating_snap_cand *cands, int *n);

// Validity test for one candidate during a drag/resize: the candidate's
// direction must match the moving edge (right/bottom vs left/top), its edge
// must sit within `threshold` px of the moving edge coordinate, and the two
// windows' perpendicular spans must actually overlap.
bool floating_snap_cand_valid(const struct floating_snap_cand *c,
		bool moving_hi, double moving_edge,
		double p_lo, double p_hi, int threshold);

#endif
