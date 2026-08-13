#include <stdlib.h>
#include "sway/commands.h"
#include "sway/config.h"
#include "sway/desktop/transaction.h"
#include "sway/tree/arrange.h"
#include "sway/tree/column.h"
#include "sway/tree/container.h"
#include "sway/tree/viewport.h"
#include "sway/tree/workspace.h"

struct cmd_results *cmd_take(int argc, char **argv) {
	struct sway_container *con = config->handler_context.container;
	if (!con || container_is_floating(con) ||
			con->pending.fullscreen_mode != FULLSCREEN_NONE) {
		return cmd_results_new(CMD_SUCCESS, NULL);
	}

	con = container_toplevel_ancestor(con);
	struct sway_workspace *ws = con->pending.workspace;
	if (!ws) {
		return cmd_results_new(CMD_SUCCESS, NULL);
	}

	int idx = list_find(ws->tiling, con);
	if (idx < 0 || idx + 1 >= ws->tiling->length) {
		return cmd_results_new(CMD_SUCCESS, NULL);
	}

	struct sway_container *right = ws->tiling->items[idx + 1];

	if (right->pending.children) {
		while (right->pending.children->length > 0) {
			struct sway_container *child = right->pending.children->items[0];
			container_detach(child);
			container_add_child(con, child);
		}
	} else {
		container_detach(right);
		container_add_child(con, right);
	}

	double freed = right->pending.width;
	(void)column_remove(right, false);
	viewport_grow_evenly(ws, idx, freed);

	node_set_dirty(&con->node);
	arrange_workspace(ws);
	transaction_commit_dirty();
	return cmd_results_new(CMD_SUCCESS, NULL);
}
