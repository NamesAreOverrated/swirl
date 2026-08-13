#ifndef _SWAY_COLUMN_H
#define _SWAY_COLUMN_H

#include "sway/tree/container.h"
#include "sway/tree/layout.h"
#include "sway/tree/workspace.h"

void column_set_width_px(struct sway_container *col, double width_px);

double col_local_x(struct sway_workspace *ws,
		const struct sway_container *col);

int column_remove(struct sway_container *col, bool grow_neighbors);

static inline void column_set_width_fraction(struct sway_container *col,
		double fraction) {
	col->width_fraction = fraction;
	struct sway_workspace *ws = col->pending.workspace;
	if (ws) {
		col->pending.width = workspace_width_fraction(ws, fraction);
	}
}

static inline void window_set_height_px(struct sway_container *win,
		double height_px) {
	win->pending.height = height_px;
	struct sway_workspace *ws = win->pending.workspace;
	if (ws) {
		win->height_fraction = workspace_height_to_fraction(ws, height_px);
	}
}

static inline void window_set_height_fraction(struct sway_container *win,
		double fraction) {
	win->height_fraction = fraction;
	struct sway_workspace *ws = win->pending.workspace;
	if (ws) {
		win->pending.height = workspace_height_fraction(ws, fraction);
	}
}

#endif
