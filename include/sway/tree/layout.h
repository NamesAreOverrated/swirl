#ifndef _SWAY_LAYOUT_H
#define _SWAY_LAYOUT_H

#include "sway/tree/workspace.h"

double layout_get_default_width(struct sway_workspace *workspace);
double workspace_get_new_column_width(struct sway_workspace *ws);
double workspace_width_fraction(struct sway_workspace *ws, double fraction);
double workspace_height_fraction(struct sway_workspace *ws, double fraction);

#endif
