#include <string.h>
#include "sway/commands.h"
#include "sway/input/seat.h"
#include "sway/ipc-server.h"
#include "sway/tree/workspace.h"
#include "log.h"

struct cmd_results *cmd_default_float(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if (argc > 1
			|| (error = checkarg(argc, "default_float", EXPECTED_AT_MOST, 1))) {
		return error;
	}

	struct sway_workspace *ws = config->handler_context.workspace;
	if (!ws) {
		return cmd_results_new(CMD_INVALID,
				"No workspace in context for default_float");
	}

	if (argc == 0 || strcmp(argv[0], "toggle") == 0) {
		ws->default_float = !ws->default_float;
	} else if (strcmp(argv[0], "on") == 0) {
		ws->default_float = true;
	} else if (strcmp(argv[0], "off") == 0) {
		ws->default_float = false;
	} else {
		return cmd_results_new(CMD_INVALID,
				"Expected 'default_float', 'default_float on', "
				"'default_float off' or 'default_float toggle'");
	}

	sway_log(SWAY_DEBUG, "Workspace '%s' default_float=%d",
			ws->name, ws->default_float);
	ipc_event_workspace(ws, ws, "float");
	return cmd_results_new(CMD_SUCCESS, NULL);
}
