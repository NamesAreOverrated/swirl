#include "sway/commands.h"
#include "sway/config.h"
#include "sway/tree/container.h"
#include "sway/tree/root.h"
#include "log.h"

static void update_container_corner_radius(struct sway_container *con, void *data) {
	container_update(con);
}

struct cmd_results *cmd_corner_radius(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "corner_radius", EXPECTED_AT_LEAST, 1))) {
		return error;
	}

	char *err;
	int val = (int)strtol(argv[0], &err, 10);
	if (*err) {
		return cmd_results_new(CMD_INVALID, "corner_radius int invalid");
	}
	if (val < 0 || val > 100) {
		return cmd_results_new(CMD_FAILURE, "corner_radius value out of bounds");
	}

	config->corner_radius = val;
	root_for_each_container(update_container_corner_radius, NULL);

	return cmd_results_new(CMD_SUCCESS, NULL);
}
