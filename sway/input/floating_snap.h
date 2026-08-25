#ifndef SWAY_INPUT_FLOATING_SNAP_H
#define SWAY_INPUT_FLOATING_SNAP_H

#include <stdbool.h>
#include "wlr/util/box.h"

struct sway_container;
struct sway_workspace;

#define FLOATING_SNAP_MAX_CANDS 64
#define FLOATING_SNAP_EDGE_STRIP 8

enum floating_snap_role {
	FLOATING_SNAP_LO,     // candidate matches our left/top edge
	FLOATING_SNAP_HI,     // candidate matches our right/bottom edge
	FLOATING_SNAP_CENTER  // candidate matches our center
};

// One snap candidate: an edge coordinate plus the rect to highlight when
// this candidate wins (the source window's matching edge strip).
struct floating_snap_cand {
	double edge;
	struct wlr_box hl;
	enum floating_snap_role role;
};

// Collect floater-edge candidates on `horizontal` (or vertical) axis from
// every other floating container on the workspace. Edges are offset by one
// inner gap and direction-tagged so a dragged window's compatible edge is
// unambiguous.
void floating_snap_collect(struct sway_container *skip,
		struct sway_workspace *ws, bool horizontal,
		struct floating_snap_cand *cands, int *n);

// Validity test: role must match the moving edge reference (left/top,
// right/bottom, or center), and the candidate's edge coordinate must be
// within `threshold` px of that reference.
bool floating_snap_cand_valid(const struct floating_snap_cand *c,
		enum floating_snap_role moving_role, double moving_edge_ref,
		int threshold);

#endif
