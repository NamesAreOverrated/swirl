#include <stdlib.h>
#include <strings.h>
#include "sway/commands.h"
#include "sway/tree/container.h"
#include "sway/tree/root.h"
#include "sway/tree/workspace.h"
#include "sway/tree/arrange.h"
#include "sway/tree/viewport.h"
#include "sway/input/seat.h"
#include "sway/desktop/transaction.h"
#include "log.h"

struct cmd_results *cmd_pull(int argc, char **argv) {
	// 0 args: [con_id=X] pull — criteria-based overview-style pull
	if (argc == 0) {
		struct sway_container *target = config->handler_context.container;
		if (!target) {
			return cmd_results_new(CMD_SUCCESS, NULL);
		}

		struct sway_seat *seat = input_manager_current_seat();
		struct sway_workspace *active_ws = seat_get_focused_workspace(seat);
		if (!active_ws) {
			return cmd_results_new(CMD_SUCCESS, NULL);
		}

		if (container_is_floating(target)) {
			if (container_is_scratchpad_hidden(target)) {
				root_scratchpad_show(target);
				transaction_commit_dirty();
				return cmd_results_new(CMD_SUCCESS, NULL);
			}
			if (target->pending.fullscreen_mode != FULLSCREEN_NONE) {
				container_set_fullscreen(target, FULLSCREEN_NONE);
			}
			struct sway_workspace *old_ws = target->pending.workspace;
			if (old_ws && old_ws != active_ws) {
				struct sway_output *old_output = old_ws->output;
				container_detach(target);
				workspace_add_floating(active_ws, target);
				if (old_output != active_ws->output) {
					struct wlr_box old_box, new_box;
					workspace_get_box(old_ws, &old_box);
					workspace_get_box(active_ws, &new_box);
					floating_fix_coordinates(target, &old_box, &new_box);
				}
				arrange_workspace(old_ws);
				arrange_workspace(active_ws);
			}
			seat_set_focus_container(seat, target);
			container_raise_floating(target);
			transaction_commit_dirty();
			return cmd_results_new(CMD_SUCCESS, NULL);
		}

		struct sway_container *focus = seat_get_focused_container(seat);
		if (!focus) {
			seat_set_focus_container(seat, target);
			transaction_commit_dirty();
			return cmd_results_new(CMD_SUCCESS, NULL);
		}

		struct sway_container *focus_col = container_toplevel_ancestor(focus);
		struct sway_container *target_col = container_toplevel_ancestor(target);

		if (focus_col != target_col) {
			int fi = list_find(active_ws->tiling, focus_col);
			if (fi >= 0)
				workspace_pull_column(active_ws, target_col, fi + 1);
		}
		seat_set_focus_container(seat, target);
		transaction_commit_dirty();
		return cmd_results_new(CMD_SUCCESS, NULL);
	}

	return cmd_results_new(CMD_INVALID, "Usage: pull");
}
