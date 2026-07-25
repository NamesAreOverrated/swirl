#include <string.h>
#include <stdlib.h>
#include "sway/commands.h"
#include "util.h"

struct cmd_results *cmd_env(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "env", EXPECTED_AT_LEAST, 2))) {
		return error;
	}

	setenv(argv[0], argv[1], true);

	return cmd_results_new(CMD_SUCCESS, NULL);
}
