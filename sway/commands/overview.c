#include <stddef.h>
#include <strings.h>
#include "sway/commands.h"
#include "sway/config.h"
#include "sway/desktop/overview.h"

struct cmd_results *cmd_overview(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "overview", EXPECTED_EQUAL_TO, 2))) {
		return error;
	}

	enum overview_scope scope;
	if (strcasecmp(argv[0], "current") == 0) {
		scope = OVERVIEW_CURRENT;
	} else if (strcasecmp(argv[0], "all") == 0) {
		scope = OVERVIEW_ALL;
	} else {
		return cmd_results_new(CMD_INVALID, "Invalid scope: %s", argv[0]);
	}

	enum overview_action action;
	if (strcasecmp(argv[1], "focus") == 0) {
		action = OVERVIEW_FOCUS;
	} else if (strcasecmp(argv[1], "pull") == 0) {
		action = OVERVIEW_PULL;
	} else if (strcasecmp(argv[1], "swap") == 0) {
		action = OVERVIEW_SWAP;
	} else {
		return cmd_results_new(CMD_INVALID, "Invalid action: %s", argv[1]);
	}

	overview_set_params(scope, action);
	overview_toggle();
	return cmd_results_new(CMD_SUCCESS, NULL);
}
