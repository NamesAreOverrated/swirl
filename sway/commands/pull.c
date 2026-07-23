#include <stdlib.h>
#include <strings.h>
#include "sway/commands.h"
#include "sway/tree/container.h"
#include "sway/tree/workspace.h"
#include "sway/tree/arrange.h"
#include "sway/tree/viewport.h"
#include "sway/input/seat.h"
#include "sway/desktop/transaction.h"
#include "log.h"

struct cmd_results *cmd_pull(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "pull", EXPECTED_EQUAL_TO, 1))) {
		return error;
	}

	bool left;
	if (strcasecmp(argv[0], "left") == 0) {
		left = true;
	} else if (strcasecmp(argv[0], "right") == 0) {
		left = false;
	} else {
		return cmd_results_new(CMD_INVALID,
			"Usage: pull [left|right]");
	}

	struct sway_container *con = config->handler_context.container;
	if (!con || container_is_floating(con) ||
			con->pending.fullscreen_mode != FULLSCREEN_NONE) {
		return cmd_results_new(CMD_SUCCESS, NULL);
	}

	con = container_toplevel_ancestor(con);
	struct sway_workspace *ws = con->pending.workspace;
	if (!ws || ws->tiling->length < 2) {
		return cmd_results_new(CMD_SUCCESS, NULL);
	}

	int focus_idx = list_find(ws->tiling, con);
	if (focus_idx < 0) {
		return cmd_results_new(CMD_SUCCESS, NULL);
	}

	int target_idx = -1;
	if (left) {
		for (int i = focus_idx - 1; i >= 0; --i) {
			if (!viewport_column_is_visible(ws, i)) {
				target_idx = i;
				break;
			}
		}
	} else {
		for (int i = focus_idx + 1; i < ws->tiling->length; ++i) {
			if (!viewport_column_is_visible(ws, i)) {
				target_idx = i;
				break;
			}
		}
	}

	if (target_idx < 0) {
		return cmd_results_new(CMD_SUCCESS, NULL);
	}

	struct sway_container *target_col = ws->tiling->items[target_idx];
	int insert_idx = left ? focus_idx : focus_idx + 1;

	workspace_fit_new_column(ws, target_col, insert_idx);
	workspace_insert_column(ws, target_col, insert_idx);
	arrange_workspace(ws);
	transaction_commit_dirty();

	struct sway_seat *seat = input_manager_current_seat();
	seat_set_focus_raw(seat, &target_col->node);

	return cmd_results_new(CMD_SUCCESS, NULL);
}
