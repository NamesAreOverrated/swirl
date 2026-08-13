#include "sway/tree/layout.h"
#include "sway/tree/column.h"
#include "sway/config.h"
#include "sway/output.h"
#include <wlr/types/wlr_scene.h>
#include <math.h>

double layout_get_default_width(struct sway_workspace *workspace) {
	return config->default_column_width_fraction;
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

double workspace_width_to_fraction(struct sway_workspace *ws, double pixel_width) {
	double divisor = ws->width - ws->gaps_inner;
	return divisor > 0 ? pixel_width / divisor : 0;
}

double workspace_height_to_fraction(struct sway_workspace *ws, double pixel_height) {
	double divisor = ws->height - ws->gaps_inner;
	return divisor > 0 ? pixel_height / divisor : 0;
}

double workspace_clamp_column_width(struct sway_workspace *ws, double pixel_width) {
	double default_w = workspace_width_fraction(ws, config->default_column_width_fraction);
	double min_w = workspace_width_fraction(ws, config->min_column_width_fraction);
	if (pixel_width >= default_w || pixel_width < min_w) {
		return default_w;
	}
	return pixel_width;
}

void column_set_width_px(struct sway_container *col, double width_px) {
	col->pending.width = width_px;
	struct sway_workspace *ws = col->pending.workspace;
	if (ws && ws->width > 0) {
		col->width_fraction = workspace_width_to_fraction(ws, width_px);
	}
}
