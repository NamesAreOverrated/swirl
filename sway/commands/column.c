#include <stdlib.h>
#include <strings.h>
#include "sway/commands.h"
#include "sway/tree/column.h"
#include "sway/tree/container.h"
#include "sway/tree/workspace.h"
#include "sway/tree/arrange.h"
#include "sway/tree/layout.h"
#include "sway/tree/viewport.h"
#include "sway/input/seat.h"
#include "sway/desktop/transaction.h"
#include "log.h"

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

struct cmd_results *cmd_pop(int argc, char **argv) {
	struct sway_container *con = config->handler_context.container;
	if (!con || container_is_floating(con) ||
			con->pending.fullscreen_mode != FULLSCREEN_NONE) {
		return cmd_results_new(CMD_SUCCESS, NULL);
	}

	con = container_toplevel_ancestor(con);
	struct sway_workspace *ws = con->pending.workspace;
	if (!ws || ws->tiling->length < 2) {
		return cmd_results_new(CMD_SUCCESS, NULL);
	}

	int idx = list_find(ws->tiling, con);
	if (idx < 0) {
		return cmd_results_new(CMD_SUCCESS, NULL);
	}

	double col_w = con->pending.width;
	int gaps = ws->gaps_inner;

	sway_log(SWAY_DEBUG, "[pop] con=%p idx=%d col_w=%.1f gaps=%d n=%d",
		(void *)con, idx, col_w, gaps, ws->tiling->length);

	list_del(ws->tiling, idx);
	int focus_idx = viewport_grow_evenly(ws, idx, col_w);
	int insert_at = viewport_first_off_screen(ws, true);
	if (insert_at < 0) insert_at = ws->tiling->length;
	if (insert_at > ws->tiling->length) insert_at = ws->tiling->length;
	list_insert(ws->tiling, insert_at, con);

	sway_log(SWAY_DEBUG, "[pop] after: focus_idx=%d n=%d con_idx=%d",
		focus_idx, ws->tiling->length,
		list_find(ws->tiling, con));

	if (focus_idx >= 0 && focus_idx < ws->tiling->length) {
		struct sway_seat *seat = input_manager_current_seat();
		struct sway_container *target = ws->tiling->items[focus_idx];
		sway_log(SWAY_DEBUG, "[pop] focusing col at [%d] = %p (w=%.1f x=%.1f)",
			focus_idx, (void *)target, target->pending.width,
			target->pending.x);
		seat_set_focus_raw(seat, &target->node);
	} else {
		sway_log(SWAY_DEBUG, "[pop] focus_idx=%d out of range, no focus set",
			focus_idx);
	}

	arrange_workspace(ws);
	transaction_commit_dirty();
	return cmd_results_new(CMD_SUCCESS, NULL);
}
