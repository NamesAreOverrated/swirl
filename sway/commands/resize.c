#include "log.h"
#include "sway/commands.h"
#include "sway/desktop/transaction.h"
#include "sway/tree/arrange.h"
#include "sway/tree/layout.h"
#include "sway/tree/view.h"
#include "sway/tree/viewport.h"
#include "sway/tree/workspace.h"
#include "util.h"
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wlr/util/edges.h>

#define AXIS_HORIZONTAL (WLR_EDGE_LEFT | WLR_EDGE_RIGHT)
#define AXIS_VERTICAL (WLR_EDGE_TOP | WLR_EDGE_BOTTOM)

static uint32_t parse_resize_axis(const char *axis) {
  if (strcasecmp(axis, "width") == 0 || strcasecmp(axis, "horizontal") == 0) {
    return AXIS_HORIZONTAL;
  }
  if (strcasecmp(axis, "height") == 0 || strcasecmp(axis, "vertical") == 0) {
    return AXIS_VERTICAL;
  }
  if (strcasecmp(axis, "up") == 0) {
    return WLR_EDGE_TOP;
  }
  if (strcasecmp(axis, "down") == 0) {
    return WLR_EDGE_BOTTOM;
  }
  if (strcasecmp(axis, "left") == 0) {
    return WLR_EDGE_LEFT;
  }
  if (strcasecmp(axis, "right") == 0) {
    return WLR_EDGE_RIGHT;
  }
  return WLR_EDGE_NONE;
}

static bool is_horizontal(uint32_t axis) {
  return axis & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
}

struct sway_container *container_find_resize_parent(struct sway_container *con,
                                                    uint32_t axis) {
  enum sway_container_layout parallel_layout =
      is_horizontal(axis) ? L_HORIZ : L_VERT;
  bool allow_first = axis != WLR_EDGE_TOP && axis != WLR_EDGE_LEFT;
  bool allow_last = axis != WLR_EDGE_RIGHT && axis != WLR_EDGE_BOTTOM;

  while (con) {
    list_t *siblings = container_get_siblings(con);
    int index = container_sibling_index(con);
    if (container_parent_layout(con) == parallel_layout &&
        siblings->length > 1 && (allow_first || index > 0) &&
        (allow_last || index < siblings->length - 1)) {
      return con;
    }
    con = con->pending.parent;
  }

  return NULL;
}

void tiled_resize_horizontal_px(struct sway_container *con,
                                       uint32_t edge, double delta_px) {
	struct sway_container *col = container_toplevel_ancestor(con);
	struct sway_workspace *ws = col->pending.workspace;
	if (!ws) {
		return;
	}

	double min_col_w = workspace_width_fraction(ws, config->min_column_width_fraction);

	sway_log(SWAY_DEBUG, "[resize] con=%p col=%p col_idx=%d col_x=%.0f col_w=%.0f ws_width=%d vp_x=%.0f",
		con, col, container_sibling_index(col),
		col->pending.x, col->pending.width, ws->width, ws->viewport_x);

	double new_w = fmax(min_col_w, fmin(col->pending.width + delta_px, ws->width));
	double real_delta = new_w - col->pending.width;
	sway_log(SWAY_DEBUG, "[resize] delta_px=%.0f new_w=%.0f real_delta=%.0f",
		delta_px, new_w, real_delta);

	if (real_delta == 0) {
		sway_log(SWAY_DEBUG, "[resize] real_delta == 0, early return");
		return;
	}

	int col_idx = container_sibling_index(col);
	if (col_idx < 0) {
		sway_log(SWAY_DEBUG, "[resize] col_idx < 0, arrange and return");
		arrange_workspace(ws);
		return;
	}
	int focus_idx = ws->focused_column_idx >= 0 ? ws->focused_column_idx : col_idx;
	sway_log(SWAY_DEBUG, "[resize] col_idx=%d raw_focused_column_idx=%d "
		"focus_idx=%d n_cols=%d vp=[%.0f, %.0f]",
		col_idx, ws->focused_column_idx, focus_idx, ws->tiling->length,
		ws->viewport_x, ws->viewport_x + ws->width);

	// Pre-change: scan visible columns and compute occupied width
	int candidates[32];
	double occupied;
	int n_candidates = viewport_scan_visible(ws, focus_idx, col_idx,
		candidates, 32, &occupied);
	sway_log(SWAY_DEBUG, "[resize] occupied=%.0f gap=%d n_candidates=%d",
		occupied, ws->gaps_inner, n_candidates);

	// Apply resize (always)
	col->pending.width = new_w;
	col->width_fraction = workspace_width_to_fraction(ws, new_w);
	node_set_dirty(&col->node);

	double remaining;
	if (real_delta < 0) {
		remaining = real_delta;
		sway_log(SWAY_DEBUG, "[resize] shrinking: remaining=%.0f (farthest grows to fill)", remaining);
	} else {
		remaining = fmax(0, occupied + real_delta - ws->width);
		sway_log(SWAY_DEBUG, "[resize] growing: occupied=%.0f real_delta=%.0f ws->width=%d remaining=%.0f",
			occupied, real_delta, ws->width, remaining);
	}

	// Edge-adjacent sibling absorbs first (mouse edge-drag)
	if (remaining != 0 && (edge & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT))) {
		int edge_sib = (edge & WLR_EDGE_LEFT) ? col_idx - 1 : col_idx + 1;
		for (int ci = 0; ci < n_candidates; ++ci) {
			if (candidates[ci] == edge_sib) {
				struct sway_container *c = ws->tiling->items[edge_sib];
				double orig = c->pending.width;
				double new_cw = fmax(min_col_w, orig - remaining);
				double absorbed = orig - new_cw;
				remaining -= absorbed;
				sway_log(SWAY_DEBUG, "[resize]   edge-sib col[%d]: orig=%.0f new=%.0f absorbed=%.0f remaining=%.0f",
					edge_sib, orig, new_cw, absorbed, remaining);
				c->pending.width = new_cw;
				c->width_fraction = workspace_width_to_fraction(ws, new_cw);
				node_set_dirty(&c->node);
				// Remove edge-sib from candidates
				for (int j = ci; j < n_candidates - 1; ++j)
					candidates[j] = candidates[j + 1];
				n_candidates--;
				break;
			}
		}
	}

	// Farthest-first absorption for non-edge-drag events (keyboard, mod+right-click)
	if (!(edge & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT)) && remaining != 0 && n_candidates > 0) {
		viewport_absorb_farthest(ws, candidates, n_candidates, focus_idx,
			&remaining, min_col_w);
	}

	if (remaining > 0) {
		sway_log(SWAY_DEBUG, "[resize]   cap growth: pull back by %.0f", remaining);
		col->pending.width -= remaining;
		col->width_fraction = workspace_width_to_fraction(ws, col->pending.width);
		node_set_dirty(&col->node);
	}

	sway_log(SWAY_DEBUG, "[resize] arrange_workspace (remaining=%.0f)", remaining);
	arrange_workspace(ws);
}

void tiled_resize_vertical_px(struct sway_container *con, uint32_t edge,
                                     double delta_px) {
  struct sway_workspace *ws = con->pending.workspace;
  if (!ws) {
    return;
  }

  list_t *siblings = container_get_siblings(con);
  int index = container_sibling_index(con);
  struct sway_container *sib = NULL;
  if ((edge & (WLR_EDGE_TOP | WLR_EDGE_BOTTOM)) && siblings &&
      siblings->length > 1) {
    int sib_idx = index + ((edge & WLR_EDGE_BOTTOM) ? 1 : -1);
    if (sib_idx >= 0 && sib_idx < siblings->length) {
      sib = siblings->items[sib_idx];
    }
  }

  double new_h = fmax(MIN_SANE_H, con->pending.height + delta_px);
  con->pending.height = new_h;
  con->height_fraction = workspace_height_to_fraction(ws, new_h);
  node_set_dirty(&con->node);

  if (sib) {
    double new_sh = fmax(MIN_SANE_H, sib->pending.height - delta_px);
    sib->pending.height = new_sh;
    sib->height_fraction = workspace_height_to_fraction(ws, new_sh);
  }

  arrange_workspace(ws);
}

static void tiled_resize_horizontal_frac(struct sway_container *con,
                                         double delta_frac) {
  struct sway_container *col = container_toplevel_ancestor(con);
  struct sway_workspace *ws = col->pending.workspace;
  if (!ws) {
    return;
  }
  double new_frac = col->width_fraction + delta_frac;
  new_frac = fmax(0.05, fmin(1.0, new_frac));
  double delta_px = workspace_width_fraction(ws, new_frac) - col->pending.width;
  tiled_resize_horizontal_px(con, WLR_EDGE_NONE, delta_px);
}

static void tiled_resize_vertical_frac(struct sway_container *con,
                                       double delta_frac) {
  struct sway_workspace *ws = con->pending.workspace;
  if (!ws) {
    return;
  }
  double new_frac = con->height_fraction + delta_frac;
  new_frac = fmax(0.05, fmin(1.0, new_frac));
  double delta_px =
      workspace_height_fraction(ws, new_frac) - con->pending.height;
  tiled_resize_vertical_px(con, WLR_EDGE_NONE, delta_px);
}

void container_resize_tiled(struct sway_container *con, uint32_t axis,
                            int amount) {
  if (!con || container_is_scratchpad_hidden_or_child(con)) {
    return;
  }
  if (is_horizontal(axis)) {
    tiled_resize_horizontal_px(con, axis, amount);
  } else {
    tiled_resize_vertical_px(con, axis, amount);
  }
}

/**
 * Implement `resize <grow|shrink>` for a floating container.
 */
static struct cmd_results *
resize_adjust_floating(uint32_t axis, struct movement_amount *amount) {
  struct sway_container *con = config->handler_context.container;
  int grow_width = 0, grow_height = 0;

  if (is_horizontal(axis)) {
    grow_width = amount->amount;
  } else {
    grow_height = amount->amount;
  }

  // Make sure we're not adjusting beyond floating min/max size
  int min_width, max_width, min_height, max_height;
  floating_calculate_constraints(&min_width, &max_width, &min_height,
                                 &max_height);
  if (con->pending.width + grow_width < min_width) {
    grow_width = min_width - con->pending.width;
  } else if (con->pending.width + grow_width > max_width) {
    grow_width = max_width - con->pending.width;
  }
  if (con->pending.height + grow_height < min_height) {
    grow_height = min_height - con->pending.height;
  } else if (con->pending.height + grow_height > max_height) {
    grow_height = max_height - con->pending.height;
  }
  int grow_x = 0, grow_y = 0;

  if (axis == AXIS_HORIZONTAL) {
    grow_x = -grow_width / 2;
  } else if (axis == AXIS_VERTICAL) {
    grow_y = -grow_height / 2;
  } else if (axis == WLR_EDGE_TOP) {
    grow_y = -grow_height;
  } else if (axis == WLR_EDGE_LEFT) {
    grow_x = -grow_width;
  }
  if (grow_width == 0 && grow_height == 0) {
    return cmd_results_new(CMD_INVALID, "Cannot resize any further");
  }
  con->pending.x += grow_x;
  con->pending.y += grow_y;
  con->pending.width += grow_width;
  con->pending.height += grow_height;

  con->pending.content_x += grow_x;
  con->pending.content_y += grow_y;
  con->pending.content_width += grow_width;
  con->pending.content_height += grow_height;

  arrange_container(con);

  return cmd_results_new(CMD_SUCCESS, NULL);
}

/**
 * Implement `resize <grow|shrink>` for a tiled container.
 */
static struct cmd_results *resize_adjust_tiled(uint32_t axis,
                                               struct movement_amount *amount) {
  struct sway_container *current = config->handler_context.container;

  if (container_is_scratchpad_hidden_or_child(current)) {
    return cmd_results_new(CMD_FAILURE,
                           "Cannot resize a hidden scratchpad container");
  }

  double sign = amount->amount > 0 ? 1.0 : -1.0;

  if (is_horizontal(axis)) {
    tiled_resize_horizontal_frac(current, sign * 0.1);
  } else {
    tiled_resize_vertical_frac(current, sign * 0.1);
  }
  return cmd_results_new(CMD_SUCCESS, NULL);
}

/**
 * Implement `resize set` for a tiled container.
 */
static struct cmd_results *resize_set_tiled(struct sway_container *con,
                                            struct movement_amount *width,
                                            struct movement_amount *height) {

  if (container_is_scratchpad_hidden_or_child(con)) {
    return cmd_results_new(CMD_FAILURE,
                           "Cannot resize a hidden scratchpad container");
  }

  if (width->amount) {
    if (width->unit == MOVEMENT_UNIT_PPT ||
        width->unit == MOVEMENT_UNIT_DEFAULT) {
      // Convert to px
      struct sway_container *parent = con->pending.parent;
      while (parent && parent->pending.layout != L_HORIZ) {
        parent = parent->pending.parent;
      }
      if (parent) {
        width->amount = parent->pending.width * width->amount / 100;
      } else {
        width->amount = con->pending.workspace->width * width->amount / 100;
      }
      width->unit = MOVEMENT_UNIT_PX;
    }
    if (width->unit == MOVEMENT_UNIT_PX) {
      container_resize_tiled(con, AXIS_HORIZONTAL,
                             width->amount - con->pending.width);
    }
  }

  if (height->amount) {
    if (height->unit == MOVEMENT_UNIT_PPT ||
        height->unit == MOVEMENT_UNIT_DEFAULT) {
      // Convert to px
      struct sway_container *parent = con->pending.parent;
      while (parent && parent->pending.layout != L_VERT) {
        parent = parent->pending.parent;
      }
      if (parent) {
        height->amount = parent->pending.height * height->amount / 100;
      } else {
        height->amount = con->pending.workspace->height * height->amount / 100;
      }
      height->unit = MOVEMENT_UNIT_PX;
    }
    if (height->unit == MOVEMENT_UNIT_PX) {
      container_resize_tiled(con, AXIS_VERTICAL,
                             height->amount - con->pending.height);
    }
  }

  return cmd_results_new(CMD_SUCCESS, NULL);
}

/**
 * Implement `resize set` for a floating container.
 */
static struct cmd_results *resize_set_floating(struct sway_container *con,
                                               struct movement_amount *width,
                                               struct movement_amount *height) {
  int min_width, max_width, min_height, max_height, grow_width = 0,
                                                    grow_height = 0;
  floating_calculate_constraints(&min_width, &max_width, &min_height,
                                 &max_height);

  if (width->amount) {
    switch (width->unit) {
    case MOVEMENT_UNIT_PPT:
      if (container_is_scratchpad_hidden(con)) {
        return cmd_results_new(
            CMD_FAILURE, "Cannot resize a hidden scratchpad container by ppt");
      }
      // Convert to px
      width->amount = con->pending.workspace->width * width->amount / 100;
      width->unit = MOVEMENT_UNIT_PX;
      // Falls through
    case MOVEMENT_UNIT_PX:
    case MOVEMENT_UNIT_DEFAULT:
      width->amount = fmax(min_width, fmin(width->amount, max_width));
      grow_width = width->amount - con->pending.width;
      con->pending.x -= grow_width / 2;
      con->pending.width = width->amount;
      break;
    case MOVEMENT_UNIT_INVALID:
      sway_assert(false, "invalid width unit");
      break;
    }
  }

  if (height->amount) {
    switch (height->unit) {
    case MOVEMENT_UNIT_PPT:
      if (container_is_scratchpad_hidden(con)) {
        return cmd_results_new(
            CMD_FAILURE, "Cannot resize a hidden scratchpad container by ppt");
      }
      // Convert to px
      height->amount = con->pending.workspace->height * height->amount / 100;
      height->unit = MOVEMENT_UNIT_PX;
      // Falls through
    case MOVEMENT_UNIT_PX:
    case MOVEMENT_UNIT_DEFAULT:
      height->amount = fmax(min_height, fmin(height->amount, max_height));
      grow_height = height->amount - con->pending.height;
      con->pending.y -= grow_height / 2;
      con->pending.height = height->amount;
      break;
    case MOVEMENT_UNIT_INVALID:
      sway_assert(false, "invalid height unit");
      break;
    }
  }

  con->pending.content_x -= grow_width / 2;
  con->pending.content_y -= grow_height / 2;
  con->pending.content_width += grow_width;
  con->pending.content_height += grow_height;

  arrange_container(con);

  return cmd_results_new(CMD_SUCCESS, NULL);
}

/**
 * resize set <args>
 *
 * args: [width] <width> [px|ppt]
 *     : height <height> [px|ppt]
 *     : [width] <width> [px|ppt] [height] <height> [px|ppt]
 */
static struct cmd_results *cmd_resize_set(int argc, char **argv) {
  struct cmd_results *error;
  if ((error = checkarg(argc, "resize", EXPECTED_AT_LEAST, 1))) {
    return error;
  }
  const char usage[] =
      "Expected 'resize set [width] <width> [px|ppt]' or "
      "'resize set height <height> [px|ppt]' or "
      "'resize set [width] <width> [px|ppt] [height] <height> [px|ppt]'";

  // Width
  struct movement_amount width = {0};
  if (argc >= 2 && !strcmp(argv[0], "width") && strcmp(argv[1], "height")) {
    argc--;
    argv++;
  }
  if (strcmp(argv[0], "height")) {
    int num_consumed_args = parse_movement_amount(argc, argv, &width);
    argc -= num_consumed_args;
    argv += num_consumed_args;
    if (width.unit == MOVEMENT_UNIT_INVALID) {
      return cmd_results_new(CMD_INVALID, "%s", usage);
    }
  }

  // Height
  struct movement_amount height = {0};
  if (argc) {
    if (argc >= 2 && !strcmp(argv[0], "height")) {
      argc--;
      argv++;
    }
    int num_consumed_args = parse_movement_amount(argc, argv, &height);
    if (argc > num_consumed_args) {
      return cmd_results_new(CMD_INVALID, "%s", usage);
    }
    if (height.unit == MOVEMENT_UNIT_INVALID) {
      return cmd_results_new(CMD_INVALID, "%s", usage);
    }
  }

  // If 0, don't resize that dimension
  struct sway_container *con = config->handler_context.container;
  if (width.amount <= 0) {
    width.amount = con->pending.width;
  }
  if (height.amount <= 0) {
    height.amount = con->pending.height;
  }

  if (container_is_floating(con)) {
    return resize_set_floating(con, &width, &height);
  }
  return resize_set_tiled(con, &width, &height);
}

/**
 * resize <grow|shrink> <args>
 *
 * args: <direction>
 * args: <direction> <amount> <unit>
 * args: <direction> <amount> <unit> or <amount> <other_unit>
 */
static struct cmd_results *cmd_resize_adjust(int argc, char **argv,
                                             int multiplier) {
  const char usage[] = "Expected 'resize grow|shrink <direction> "
                       "[<amount> px|ppt [or <amount> px|ppt]]'";
  uint32_t axis = parse_resize_axis(*argv);
  if (axis == WLR_EDGE_NONE) {
    return cmd_results_new(CMD_INVALID, "%s", usage);
  }
  --argc;
  ++argv;

  // First amount
  struct movement_amount first_amount;
  if (argc) {
    int num_consumed_args = parse_movement_amount(argc, argv, &first_amount);
    argc -= num_consumed_args;
    argv += num_consumed_args;
    if (first_amount.unit == MOVEMENT_UNIT_INVALID) {
      return cmd_results_new(CMD_INVALID, "%s", usage);
    }
  } else {
    first_amount.amount = 10;
    first_amount.unit = MOVEMENT_UNIT_DEFAULT;
  }

  // "or"
  if (argc) {
    if (strcmp(*argv, "or") != 0) {
      return cmd_results_new(CMD_INVALID, "%s", usage);
    }
    --argc;
    ++argv;
  }

  // Second amount
  struct movement_amount second_amount;
  if (argc) {
    int num_consumed_args = parse_movement_amount(argc, argv, &second_amount);
    if (argc > num_consumed_args) {
      return cmd_results_new(CMD_INVALID, "%s", usage);
    }
    if (second_amount.unit == MOVEMENT_UNIT_INVALID) {
      return cmd_results_new(CMD_INVALID, "%s", usage);
    }
  } else {
    second_amount.amount = 0;
    second_amount.unit = MOVEMENT_UNIT_INVALID;
  }

  first_amount.amount *= multiplier;
  second_amount.amount *= multiplier;

  struct sway_container *con = config->handler_context.container;
  if (container_is_floating(con)) {
    // Floating containers can only resize in px. Choose an amount which
    // uses px, with fallback to an amount that specified no unit.
    if (first_amount.unit == MOVEMENT_UNIT_PX) {
      return resize_adjust_floating(axis, &first_amount);
    } else if (second_amount.unit == MOVEMENT_UNIT_PX) {
      return resize_adjust_floating(axis, &second_amount);
    } else if (first_amount.unit == MOVEMENT_UNIT_DEFAULT) {
      return resize_adjust_floating(axis, &first_amount);
    } else if (second_amount.unit == MOVEMENT_UNIT_DEFAULT) {
      return resize_adjust_floating(axis, &second_amount);
    } else {
      return cmd_results_new(CMD_INVALID,
                             "Floating containers cannot use ppt measurements");
    }
  }

  // For tiling, prefer ppt -> default -> px
  if (first_amount.unit == MOVEMENT_UNIT_PPT) {
    return resize_adjust_tiled(axis, &first_amount);
  } else if (second_amount.unit == MOVEMENT_UNIT_PPT) {
    return resize_adjust_tiled(axis, &second_amount);
  } else if (first_amount.unit == MOVEMENT_UNIT_DEFAULT) {
    return resize_adjust_tiled(axis, &first_amount);
  } else if (second_amount.unit == MOVEMENT_UNIT_DEFAULT) {
    return resize_adjust_tiled(axis, &second_amount);
  } else {
    return resize_adjust_tiled(axis, &first_amount);
  }
}

struct cmd_results *cmd_resize(int argc, char **argv) {
  if (!root->outputs->length) {
    return cmd_results_new(
        CMD_INVALID,
        "Can't run this command while there's no outputs connected.");
  }
  struct sway_container *current = config->handler_context.container;
  if (!current) {
    return cmd_results_new(CMD_INVALID, "Cannot resize nothing");
  }

  struct cmd_results *error;
  if ((error = checkarg(argc, "resize", EXPECTED_AT_LEAST, 2))) {
    return error;
  }

  if (strcasecmp(argv[0], "set") == 0) {
    return cmd_resize_set(argc - 1, &argv[1]);
  }
  if (strcasecmp(argv[0], "grow") == 0) {
    return cmd_resize_adjust(argc - 1, &argv[1], 1);
  }
  if (strcasecmp(argv[0], "shrink") == 0) {
    return cmd_resize_adjust(argc - 1, &argv[1], -1);
  }

  const char usage[] = "Expected 'resize <shrink|grow> "
                       "<width|height|up|down|left|right> [<amount>] [px|ppt]'";

  return cmd_results_new(CMD_INVALID, "%s", usage);
}
