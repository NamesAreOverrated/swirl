#include <stdlib.h>
#include "log.h"
#include "sway/tree/column.h"
#include "sway/tree/container.h"
#include "sway/tree/viewport.h"

void column_remove(struct sway_container *col, bool grow_neighbors) {
	struct sway_workspace *ws = col->pending.workspace;
	if (!ws) {
		container_reap_empty(col);
		return;
	}

	int idx = list_find(ws->tiling, col);
	double col_w = col->pending.width;

	container_reap_empty(col);

	if (grow_neighbors && idx >= 0 && ws->tiling->length > 0) {
		double freed = col_w + ws->gaps_inner;
		sway_log(SWAY_DEBUG, "[column_remove] idx=%d freed=%.0f grow=%d",
			idx, freed, grow_neighbors);
		viewport_grow_to_fill(ws, idx, freed);
	}
}
