#include <stddef.h>
#include "sway/commands.h"
#include "sway/config.h"
#include "sway/desktop/overview.h"

struct cmd_results *cmd_overview(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "overview", EXPECTED_EQUAL_TO, 0))) {
		return error;
	}
	overview_toggle();
	return cmd_results_new(CMD_SUCCESS, NULL);
}
