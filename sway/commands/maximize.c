#include <stdlib.h>
#include <string.h>
#include "sway/commands.h"
#include "sway/desktop/transaction.h"
#include "sway/input/seat.h"
#include "sway/tree/container.h"
#include "log.h"

struct cmd_results *cmd_maximize(int argc, char **argv) {
	struct sway_container *con =
		config->handler_context.node_overridden
			? config->handler_context.container
			: NULL;
	if (!con) {
		struct sway_seat *seat = input_manager_current_seat();
		struct sway_node *node = seat_get_focus(seat);
		if (!node || node->type != N_CONTAINER) {
			return cmd_results_new(CMD_INVALID, "No focusable container");
		}
		con = node->sway_container;
	}

	sway_log(SWAY_DEBUG, "maximize: toggle con=%p", (void *)con);
	container_toggle_maximize(con);
	transaction_commit_dirty();
	return cmd_results_new(CMD_SUCCESS, NULL);
}