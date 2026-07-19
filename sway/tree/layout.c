#include "sway/tree/layout.h"
#include "sway/tree/column.h"
#include "sway/output.h"
#include <wlr/types/wlr_scene.h>
#include <math.h>

#define DEFAULT_COLUMN_WIDTH_FRACTION 0.5

double layout_get_default_width(struct sway_workspace *workspace) {
	return DEFAULT_COLUMN_WIDTH_FRACTION;
}

double workspace_width_fraction(struct sway_workspace *ws, double fraction) {
	if (fraction >= 1.0) {
		return ws->width;
	}
	return (ws->width - ws->gaps_inner) * fraction;
}

double workspace_height_fraction(struct sway_workspace *ws, double fraction) {
	if (fraction >= 1.0) {
		return ws->height;
	}
	return (ws->height - ws->gaps_inner) * fraction;
}

static bool col_visible(struct sway_container *col, double vp, double vp_end, int gaps) {
	return col->pending.x + col->pending.width + gaps > vp
		&& col->pending.x < vp_end;
}

double workspace_get_new_column_width(struct sway_workspace *ws) {
	double default_fraction = DEFAULT_COLUMN_WIDTH_FRACTION;
	double usable = ws->width;
	double vp = ws->viewport_x;
	double vp_end = ws->viewport_x + usable;
	int gaps = ws->gaps_inner;
	double sum = 0;
	for (int i = 0; i < ws->tiling->length; ++i) {
		struct sway_container *con = ws->tiling->items[i];
		if (col_visible(con, vp, vp_end, gaps)) {
			sum += con->width_fraction;
		}
	}
	double remaining = 1.0 - sum;
	if (remaining >= default_fraction || remaining < 0.2) {
		return workspace_width_fraction(ws, default_fraction);
	}
	return workspace_width_fraction(ws, remaining);
}

void column_set_width_px(struct sway_container *col, double width_px) {
	col->pending.width = width_px;
	struct sway_workspace *ws = col->pending.workspace;
	if (ws) {
		col->width_fraction = ws->width > 0 ? width_px / ws->width : 0;
	}
}
