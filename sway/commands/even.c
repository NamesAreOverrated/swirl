#include <stdlib.h>
#include "sway/commands.h"
#include "sway/config.h"
#include "sway/desktop/transaction.h"
#include "sway/tree/arrange.h"
#include "sway/tree/column.h"
#include "sway/tree/container.h"
#include "sway/tree/workspace.h"

struct cmd_results *cmd_evenh(int argc, char **argv) {
	struct sway_container *con = config->handler_context.container;
	if (!con) {
		return cmd_results_new(CMD_SUCCESS, NULL);
	}
	con = container_toplevel_ancestor(con);
	struct sway_workspace *ws = con->pending.workspace;
	if (!ws || ws->tiling->length == 0) {
		return cmd_results_new(CMD_SUCCESS, NULL);
	}

	double vp = ws->viewport_x;
	double vp_end = vp + ws->width;

	int *visible = malloc(ws->tiling->length * sizeof(int));
	if (!visible) {
		return cmd_results_new(CMD_FAILURE, "allocation failed");
	}
	int n = 0;
	for (int i = 0; i < ws->tiling->length; ++i) {
		struct sway_container *c = ws->tiling->items[i];
		double col_x = col_local_x(ws, c);
		if (col_x + c->pending.width > vp - 0.5 &&
				col_x < vp_end + 0.5) {
			visible[n++] = i;
		}
	}

	if (n < 2) {
		free(visible);
		return cmd_results_new(CMD_SUCCESS, NULL);
	}

	int gaps = ws->gaps_inner;
	double usable = ws->width - (n - 1) * gaps;
	double new_w = usable / n;

	for (int i = 0; i < n; ++i) {
		struct sway_container *c = ws->tiling->items[visible[i]];
		column_set_width_px(c, new_w);
		node_set_dirty(&c->node);
	}
	free(visible);

	arrange_workspace(ws);
	transaction_commit_dirty();
	return cmd_results_new(CMD_SUCCESS, NULL);
}

struct cmd_results *cmd_evenv(int argc, char **argv) {
	struct sway_container *con = config->handler_context.container;
	if (!con) {
		return cmd_results_new(CMD_SUCCESS, NULL);
	}

	con = container_toplevel_ancestor(con);
	if (con->pending.layout != L_VERT || !con->pending.children ||
			con->pending.children->length < 2) {
		return cmd_results_new(CMD_SUCCESS, NULL);
	}

	struct sway_workspace *ws = con->pending.workspace;
	if (!ws) {
		return cmd_results_new(CMD_SUCCESS, NULL);
	}

	int gaps = ws->gaps_inner;
	int n = con->pending.children->length;

	if (n < 2) {
		return cmd_results_new(CMD_SUCCESS, NULL);
	}

	double usable = con->pending.height - (n - 1) * gaps;
	double new_h = usable / n;

	for (int i = 0; i < n; ++i) {
		struct sway_container *child = con->pending.children->items[i];
		window_set_height_px(child, new_h);
		node_set_dirty(&child->node);
	}

	arrange_workspace(ws);
	transaction_commit_dirty();
	return cmd_results_new(CMD_SUCCESS, NULL);
}
