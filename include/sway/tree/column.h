#ifndef _SWAY_COLUMN_H
#define _SWAY_COLUMN_H

#include "sway/tree/container.h"
#include "sway/tree/workspace.h"

void column_set_width_px(struct sway_container *col, double width_px);

int column_remove(struct sway_container *col, bool grow_neighbors);

static inline void column_set_width_fraction(struct sway_container *col,
		double fraction) {
	struct sway_workspace *ws = col->pending.workspace;
	if (ws && ws->width > 0) {
		double usable = ws->width - ws->current_gaps.left
			- ws->current_gaps.right;
		column_set_width_px(col, fraction * usable);
	}
}

static inline void window_set_height_px(struct sway_container *win,
		double height_px) {
	win->pending.height = height_px;
	struct sway_workspace *ws = win->pending.workspace;
	if (ws) {
		double usable = ws->height - ws->current_gaps.top
			- ws->current_gaps.bottom;
		win->height_fraction = usable > 0 ? height_px / usable : 0;
	}
}

static inline void window_set_height_fraction(struct sway_container *win,
		double fraction) {
	struct sway_workspace *ws = win->pending.workspace;
	if (ws) {
		double usable = ws->height - ws->current_gaps.top
			- ws->current_gaps.bottom;
		window_set_height_px(win, fraction * usable);
	}
}

#endif
