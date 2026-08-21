#include <stdlib.h>
#include "sway/tree/column.h"
#include "sway/tree/container.h"
#include "sway/tree/viewport.h"

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
