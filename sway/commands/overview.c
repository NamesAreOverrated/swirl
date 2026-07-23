#include <stddef.h>
#include <strings.h>
#include "sway/commands.h"
#include "sway/config.h"
#include "sway/desktop/overview.h"

struct cmd_results *cmd_overview(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "overview", EXPECTED_AT_MOST, 1))) {
		return error;
	}
	if (argc == 1 && strcasecmp(argv[0], "all") == 0) {
		overview_toggle_all();
	} else {
		overview_toggle();
	}
	return cmd_results_new(CMD_SUCCESS, NULL);
}
