#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wlr/util/edges.h>
#include "sway/commands.h"
#include "sway/config.h"
#include "log.h"
#include "stringop.h"

static struct cmd_results *parse_edge_binding(int argc, char **argv,
		bool fire_on_exit) {
	if (argc < 4) {
		return cmd_results_new(CMD_INVALID,
			"Expected: exec-range[-exit] [motion|no-motion] "
			"<scope> <edge> <range> <cmd...>");
	}
	int argi = 0;

	// Optional motion gate keyword
	int motion_gate = 0; // 0 = always
	if (strcmp(argv[argi], "motion") == 0) {
		motion_gate = 1;
		argi++;
	} else if (strcmp(argv[argi], "no-motion") == 0) {
		motion_gate = -1;
		argi++;
	}

	if (argc - argi < 3) {
		return cmd_results_new(CMD_INVALID, "Missing scope/edge/range/cmd");
	}

	// Scope
	bool is_global = false;
	char *workspace_name = NULL;
	if (strcmp(argv[argi], "global") == 0) {
		is_global = true;
	} else {
		workspace_name = strdup(argv[argi]);
	}
	argi++;

	// Edge spec: one or more of left/top/right/bottom joined by '+'
	uint32_t edges = 0;
	char *edge_str = strdup(argv[argi]);
	char *saveptr;
	char *token = strtok_r(edge_str, "+", &saveptr);
	while (token) {
		if (strcasecmp(token, "left") == 0)
			edges |= WLR_EDGE_LEFT;
		else if (strcasecmp(token, "top") == 0)
			edges |= WLR_EDGE_TOP;
		else if (strcasecmp(token, "right") == 0)
			edges |= WLR_EDGE_RIGHT;
		else if (strcasecmp(token, "bottom") == 0)
			edges |= WLR_EDGE_BOTTOM;
		token = strtok_r(NULL, "+", &saveptr);
	}
	free(edge_str);

	if (!edges) {
		free(workspace_name);
		return cmd_results_new(CMD_INVALID, "No valid edge specified");
	}
	argi++;

	// Range
	int range = atoi(argv[argi]);
	if (range < 1) {
		free(workspace_name);
		return cmd_results_new(CMD_INVALID, "Range must be >= 1");
	}
	argi++;

	// Command: join remaining args
	char *command = join_args(argv + argi, argc - argi);
	if (!command || !*command) {
		free(workspace_name);
		return cmd_results_new(CMD_INVALID, "Missing command");
	}

	struct edge_binding_config *binding = calloc(1, sizeof(*binding));
	binding->edges = edges;
	binding->range = range;
	binding->command = command;
	binding->fire_on_exit = fire_on_exit;
	binding->is_global = is_global;
	binding->workspace_name = workspace_name;
	binding->motion_gate = motion_gate;

	list_add(config->edge_bindings, binding);

	sway_log(SWAY_DEBUG, "[EDGE] binding edges=0x%x range=%d ws=%s "
		"gate=%d exit=%d cmd=%s",
		edges, range, is_global ? "global" : workspace_name,
		motion_gate, fire_on_exit, command);

	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_exec_range(int argc, char **argv) {
	return parse_edge_binding(argc, argv, false);
}

struct cmd_results *cmd_exec_range_exit(int argc, char **argv) {
	return parse_edge_binding(argc, argv, true);
}
