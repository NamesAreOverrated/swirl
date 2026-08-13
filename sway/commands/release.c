#include <stdlib.h>
#include "sway/commands.h"
#include "sway/config.h"
#include "sway/desktop/transaction.h"
#include "sway/input/seat.h"
#include "sway/tree/arrange.h"
#include "sway/tree/container.h"
#include "sway/tree/workspace.h"
#include "log.h"

struct cmd_results *cmd_release(int argc, char **argv) {
	struct sway_container *con = config->handler_context.container;
	if (!con || container_is_floating(con) ||
			con->pending.fullscreen_mode != FULLSCREEN_NONE) {
		return cmd_results_new(CMD_SUCCESS, NULL);
	}

	con = container_toplevel_ancestor(con);
	struct sway_workspace *ws = con->pending.workspace;
	if (!ws || !con->pending.children ||
			con->pending.children->length < 2) {
		return cmd_results_new(CMD_SUCCESS, NULL);
	}

	list_t *children = con->pending.children;
	int split_idx = -1;
	struct sway_container *focus = seat_get_focused_container(
			input_manager_current_seat());
	if (focus) {
		split_idx = list_find(children, focus);
	}

	int start;
	if (split_idx >= 0 && split_idx < children->length - 1) {
		start = split_idx + 1;
	} else {
		return cmd_results_new(CMD_SUCCESS, NULL);
	}

	list_t *siblings = container_get_siblings(con);
	int cidx = list_find(siblings, con);

	struct sway_container *newcol = container_create(NULL);
	newcol->pending.layout = L_VERT;
	newcol->pending.workspace = ws;

	workspace_fit_new_column(ws, newcol, cidx + 1);

	for (int i = children->length - 1; i >= start; --i) {
		struct sway_container *child = children->items[i];
		container_detach(child);
		container_insert_child(newcol, child, 0);
	}

	sway_log(SWAY_DEBUG, "[FLOAT | cmd_release] before insert: "
		"tiling_len=%d cidx=%d siblings=%p", siblings->length, cidx,
		siblings);
	list_insert(siblings, cidx + 1, newcol);
	newcol->pending.parent = NULL;
	sway_log(SWAY_DEBUG, "[FLOAT | cmd_release] after insert: "
		"tiling_len=%d newcol_at=%d", siblings->length,
		list_find(siblings, newcol));

	if (con->pending.children->length == 1) {
		con->pending.layout = L_VERT;
	}

	node_set_dirty(&con->node);
	node_set_dirty(&newcol->node);
	arrange_workspace(ws);
	transaction_commit_dirty();
	return cmd_results_new(CMD_SUCCESS, NULL);
}
