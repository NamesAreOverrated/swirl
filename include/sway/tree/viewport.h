#ifndef _SWAY_VIEWPORT_H
#define _SWAY_VIEWPORT_H

struct sway_workspace;
struct sway_container;
struct sway_seat;
struct wlr_box;

void workspace_arrange_columns(struct sway_workspace *ws,
		struct wlr_box *parent);

void viewport_arrange_windows(struct sway_container *col);

void viewport_compute_offset(struct sway_workspace *ws,
		struct sway_container *active, double area_width, double area_height);

void column_scroll_vert_to(struct sway_container *col,
		struct sway_container *win, double area_h);

void handle_focus_viewport(struct sway_seat *seat,
		struct sway_container *container);

#endif
