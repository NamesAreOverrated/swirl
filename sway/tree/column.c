#include <stdlib.h>
#include "log.h"
#include "sway/tree/column.h"
#include "sway/tree/container.h"
#include "sway/tree/viewport.h"

// Column pending.x is output-global (see workspace_arrange_columns), but
// visibility/fit math is tile-local (columns start at the workspace origin).
// Convert a column's position to tile-local coordinates.
double col_local_x(struct sway_workspace *ws,
		const struct sway_container *col) {
	return col->pending.x - ws->x;
}

int column_remove(struct sway_container *col, bool grow_neighbors) {
	struct sway_workspace *ws = col->pending.workspace;
	if (!ws) {
		container_reap_empty(col);
		return -1;
	}

	int idx = list_find(ws->tiling, col);
	double col_w = col->pending.width;

	sway_log(SWAY_DEBUG, "[FLOAT | column_remove] col=%p idx=%d "
		"tiling_len=%d grow_neighbors=%d", col, idx,
		ws->tiling->length, grow_neighbors);

	container_reap_empty(col);

	int focus_idx = -1;
	if (idx >= 0 && grow_neighbors && ws->tiling->length > 0) {
		double freed = col_w;
		sway_log(SWAY_DEBUG, "[FLOAT | column_remove] "
			"viewport_grow_to_fill: idx=%d freed=%.0f tiling_len=%d",
			idx, freed, ws->tiling->length);
		focus_idx = viewport_grow_to_fill(ws, idx, freed);
	}

	sway_log(SWAY_DEBUG, "[FLOAT | column_remove] return focus_idx=%d "
		"tiling_len=%d", focus_idx, ws->tiling->length);
	return focus_idx;
}
