#include <stdlib.h>
#include <string.h>
#include "sway/commands.h"
#include "sway/desktop/overview.h"
#include "sway/desktop/transaction.h"
#include "sway/input/seat.h"
#include "sway/output.h"
#include "sway/tree/container.h"
#include "sway/tree/root.h"
#include "sway/tree/view.h"
#include "sway/tree/workspace.h"
#include "log.h"

static struct cmd_results *minimize_hide(struct sway_container *con) {
	if (!con) {
		return cmd_results_new(CMD_INVALID, "No container to minimize");
	}
	if (con->minimized) {
		return cmd_results_new(CMD_INVALID, "Container is already minimized");
	}
	// Minimize the whole top-level grouping (a column for tiled windows,
	// the container itself for a lone window / floating). This way hiding
	// any window in a tiled column hides the entire column.
	root_minimize_container(container_toplevel_ancestor(con));
	transaction_commit_dirty();
	return cmd_results_new(CMD_SUCCESS, NULL);
}

static struct cmd_results *minimize_show(void) {
	if (!root->minimized->length) {
		return cmd_results_new(CMD_INVALID, "Minimize pool is empty");
	}
	struct sway_container *con = root->minimized->items[0];
	root_minimized_show(con);
	transaction_commit_dirty();
	return cmd_results_new(CMD_SUCCESS, NULL);
}

static struct cmd_results *minimize_show_n(int n) {
	if (!root->minimized->length) {
		return cmd_results_new(CMD_INVALID, "Minimize pool is empty");
	}
	if (n < 1 || n > root->minimized->length) {
		return cmd_results_new(CMD_INVALID,
				"No such minimized window: %d (pool has %d)", n,
				root->minimized->length);
	}
	struct sway_container *con = root->minimized->items[n - 1];
	root_minimized_show(con);
	transaction_commit_dirty();
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_minimize(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if (argc == 0) {
		struct sway_seat *seat = input_manager_current_seat();
		struct sway_node *node = seat_get_focus(seat);
		if (!node || node->type != N_CONTAINER) {
			return cmd_results_new(CMD_INVALID, "No focusable container");
		}
		return minimize_hide(node->sway_container);
	}
	if ((error = checkarg(argc, "minimize", EXPECTED_AT_LEAST, 1))) {
		return error;
	}

	if (strcmp(argv[0], "hide") == 0) {
		if (config->handler_context.node_overridden) {
			return minimize_hide(config->handler_context.container);
		}
		struct sway_seat *seat = input_manager_current_seat();
		struct sway_node *node = seat_get_focus(seat);
		if (!node || node->type != N_CONTAINER) {
			return cmd_results_new(CMD_INVALID, "No focusable container");
		}
		return minimize_hide(node->sway_container);
	} else if (strcmp(argv[0], "show") == 0) {
		if (argc >= 2) {
			char *end;
			long n = strtol(argv[1], &end, 10);
			if (*end != '\0' || n < 1) {
				return cmd_results_new(CMD_INVALID,
						"Expected 'minimize show [<n>]'");
			}
			return minimize_show_n((int)n);
		}
		return minimize_show();
	} else if (strcmp(argv[0], "toggle") == 0) {
		struct sway_container *con =
			config->handler_context.node_overridden
				? config->handler_context.container
				: NULL;
		if (con && con->minimized) {
			root_minimized_show(con);
			transaction_commit_dirty();
			return cmd_results_new(CMD_SUCCESS, NULL);
		}
		if (con) {
			return minimize_hide(con);
		}
		struct sway_seat *seat = input_manager_current_seat();
		struct sway_node *node = seat_get_focus(seat);
		if (node && node->type == N_CONTAINER) {
			return minimize_hide(node->sway_container);
		}
		return minimize_show();
	} else if (strcmp(argv[0], "overview") == 0) {
		overview_set_params(OVERVIEW_MINIMIZED, OVERVIEW_RESTORE);
		overview_toggle();
		return cmd_results_new(CMD_SUCCESS, NULL);
	}

	return cmd_results_new(CMD_INVALID, "Expected 'minimize', 'minimize hide', "
			"'minimize show [<n>]', 'minimize overview' or 'minimize toggle'");
}
