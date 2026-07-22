#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "sway/commands.h"
#include "sway/config.h"
#include "log.h"

struct cmd_results *cmd_column_width(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "column_width", EXPECTED_AT_LEAST, 2))) {
		return error;
	}

	char *err;
	float val = strtof(argv[1], &err);
	if (*err) {
		return cmd_results_new(CMD_INVALID,
			"Expected `column_width default|min <fraction>'");
	}

	if (strcmp(argv[0], "default") == 0) {
		config->default_column_width_fraction = val;
	} else if (strcmp(argv[0], "min") == 0) {
		config->min_column_width_fraction = val;
	} else {
		return cmd_results_new(CMD_INVALID,
			"Expected `column_width default|min <fraction>'");
	}

	return cmd_results_new(CMD_SUCCESS, NULL);
}
