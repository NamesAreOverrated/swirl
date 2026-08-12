#ifndef _SWAY_VIEWPORT_H
#define _SWAY_VIEWPORT_H

struct sway_workspace;
struct sway_container;
struct sway_seat;
struct wlr_box;
struct cmd_results;

void workspace_arrange_columns(struct sway_workspace *ws,
		struct wlr_box *parent);

void viewport_arrange_windows(struct sway_container *col);

void viewport_compute_offset(struct sway_workspace *ws,
		struct sway_container *active, double area_width, double area_height);

void handle_focus_viewport(struct sway_seat *seat,
		struct sway_container *container);

int viewport_scan_visible(struct sway_workspace *ws, int focus_idx,
		int exclude_idx, bool exclude_occupied, int *candidates,
		int max_cand, double *out_occupied);

bool viewport_column_is_visible(struct sway_workspace *ws, int col_idx);

void viewport_absorb_farthest(struct sway_workspace *ws,
		int *candidates, int n_candidates, int focus_idx,
		double *remaining, double min_col_w);

int viewport_grow_to_fill(struct sway_workspace *ws, int col_idx,
		double freed_width);
int viewport_grow_evenly(struct sway_workspace *ws, int col_idx,
		double freed_width);

void viewport_visible_range(struct sway_workspace *ws, int *start, int *end);

struct cmd_results *cmd_evenh(int argc, char **argv);
struct cmd_results *cmd_evenv(int argc, char **argv);

#endif
