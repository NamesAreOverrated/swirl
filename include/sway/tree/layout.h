#ifndef _SWAY_LAYOUT_H
#define _SWAY_LAYOUT_H

#include "sway/tree/workspace.h"

double layout_get_default_width(struct sway_workspace *workspace);
double workspace_width_fraction(struct sway_workspace *ws, double fraction);
double workspace_height_fraction(struct sway_workspace *ws, double fraction);
double workspace_view_remaining_width(struct sway_workspace *ws, int start_index);
double column_view_remaining_height(struct sway_container *col, int start_index);
double workspace_clamp_column_width(struct sway_workspace *ws, double pixel_width);
double workspace_width_to_fraction(struct sway_workspace *ws, double pixel_width);
double workspace_height_to_fraction(struct sway_workspace *ws, double pixel_height);

#endif
