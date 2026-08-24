#include <stdlib.h>
#include "sway/commands.h"
#include "sway/config.h"
#include "log.h"

struct cmd_results *cmd_floating_snap(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "floating_snap", EXPECTED_AT_LEAST, 1))) {
		return error;
	}

	char *err;
	int val = (int)strtol(argv[0], &err, 10);
	if (*err) {
		return cmd_results_new(CMD_INVALID, "floating_snap int invalid");
	}
	if (val < 0 || val > 200) {
		return cmd_results_new(CMD_FAILURE, "floating_snap value out of bounds");
	}

	config->floating_snap_threshold = val;

	return cmd_results_new(CMD_SUCCESS, NULL);
}
