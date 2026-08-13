#include <stdlib.h>
#include <strings.h>
#include "sway/commands.h"
#include "sway/desktop/overview.h"
#include "sway/tree/container.h"
#include "sway/tree/root.h"
#include "sway/tree/workspace.h"
#include "sway/tree/arrange.h"
#include "sway/tree/viewport.h"
#include "sway/input/seat.h"
#include "sway/desktop/transaction.h"
#include "log.h"

struct cmd_results *cmd_pull(int argc, char **argv) {
	// 0 args: [con_id=X] pull — delegate to the shared overview pull logic
	// so the script and the graphical overview stay in sync.
	if (argc == 0) {
		struct sway_container *target = config->handler_context.container;
		if (!target) {
			return cmd_results_new(CMD_SUCCESS, NULL);
		}
		overview_pull_container(target, input_manager_current_seat());
		return cmd_results_new(CMD_SUCCESS, NULL);
	}

	return cmd_results_new(CMD_INVALID, "Usage: pull");
}
