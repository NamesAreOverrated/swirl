#include <stdlib.h>
#include <strings.h>
#include "sway/commands.h"
#include "sway/tree/column.h"
#include "sway/tree/container.h"
#include "sway/tree/workspace.h"
#include "sway/tree/arrange.h"
#include "sway/tree/layout.h"
#include "sway/input/seat.h"
#include "sway/desktop/transaction.h"
#include "log.h"

struct cmd_results *cmd_column_take(int argc, char **argv) {
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

	int ridx = list_find(ws->tiling, right);
	if (ridx >= 0) {
		list_del(ws->tiling, ridx);
	}
	container_reap_empty(right);

	node_set_dirty(&con->node);
	arrange_workspace(ws);
	transaction_commit_dirty();
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_column_release(int argc, char **argv) {
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
	} else if (children->length > 1) {
		start = children->length - 1;
	} else {
		return cmd_results_new(CMD_SUCCESS, NULL);
	}

	list_t *siblings = container_get_siblings(con);
	int cidx = list_find(siblings, con);

	struct sway_container *newcol = container_create(NULL);
	newcol->pending.layout = L_VERT;
	newcol->pending.workspace = ws;

	double col_width = workspace_get_new_column_width(ws);
	column_set_width_px(newcol, col_width);
	newcol->width_fraction = ws->width > 0
		? col_width / (ws->width - ws->current_gaps.left
			- ws->current_gaps.right)
		: layout_get_default_width(ws);

	for (int i = children->length - 1; i >= start; --i) {
		struct sway_container *child = children->items[i];
		container_detach(child);
		container_insert_child(newcol, child, 0);
	}

	list_insert(siblings, cidx + 1, newcol);
	newcol->pending.parent = NULL;

	if (con->pending.children->length == 1) {
		con->pending.layout = L_VERT;
	}

	node_set_dirty(&con->node);
	node_set_dirty(&newcol->node);
	arrange_workspace(ws);
	transaction_commit_dirty();
	return cmd_results_new(CMD_SUCCESS, NULL);
}
