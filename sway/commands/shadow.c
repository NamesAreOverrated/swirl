#include <string.h>
#include <strings.h>
#include "sway/commands.h"
#include "sway/config.h"
#include "sway/tree/container.h"
#include "sway/tree/root.h"
#include "util.h"
#include "log.h"

static void update_shadow(struct sway_container *con, void *data) {
	container_update(con);
}

struct cmd_results *cmd_shadow(int argc, char **argv) {
	struct cmd_results *error = NULL;
	if ((error = checkarg(argc, "shadow", EXPECTED_AT_LEAST, 2))) {
		return error;
	}

	if (strcmp(argv[0], "enabled") == 0) {
		config->shadow_enabled = parse_boolean(argv[1], true);
	} else if (strcmp(argv[0], "blur") == 0) {
		char *err;
		int val = (int)strtol(argv[1], &err, 10);
		if (*err || val < 0) {
			return cmd_results_new(CMD_INVALID, "shadow blur int invalid");
		}
		config->shadow_blur_radius = val;
	} else if (strcmp(argv[0], "offset") == 0) {
		char *err;
		int val = (int)strtol(argv[1], &err, 10);
		if (*err || val < 0) {
			return cmd_results_new(CMD_INVALID, "shadow offset int invalid");
		}
		config->shadow_offset = val;
	} else if (strcmp(argv[0], "opacity") == 0) {
		char *err;
		float val = strtof(argv[1], &err);
		if (*err || val < 0.0f || val > 1.0f) {
			return cmd_results_new(CMD_INVALID, "shadow opacity float invalid");
		}
		config->shadow_opacity = val;
	} else if (strcmp(argv[0], "color") == 0) {
		uint32_t color;
		if (!parse_color(argv[1], &color)) {
			return cmd_results_new(CMD_INVALID, "Invalid shadow color %s", argv[1]);
		}
		color_to_rgba(config->shadow_color, color);
	} else {
		return cmd_results_new(CMD_INVALID, "Expected `shadow enabled|blur|offset|opacity|color'");
	}

	if (config->active) {
		root_for_each_container(update_shadow, NULL);
	}

	return cmd_results_new(CMD_SUCCESS, NULL);
}
