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

	container_reap_empty(col);

	int focus_idx = -1;
	if (idx >= 0 && grow_neighbors && ws->tiling->length > 0) {
		focus_idx = viewport_grow_to_fill(ws, idx, col_w);
	}

	return focus_idx;
}
