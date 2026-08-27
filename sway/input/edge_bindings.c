#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <wlr/util/edges.h>
#include "sway/input/edge_bindings.h"
#include "sway/input/input-manager.h"
#include "sway/input/cursor.h"
#include "sway/config.h"
#include "sway/desktop/launcher.h"
#include "sway/output.h"
#include "sway/tree/root.h"
#include "sway/tree/workspace.h"
#include "log.h"

struct edge_binding_state {
	bool was_inside;
};

static struct edge_binding_state *states = NULL;
static int state_count = 0;

static void ensure_states(int count) {
	if (count <= state_count) return;
	struct edge_binding_state *next = realloc(states,
			sizeof(*states) * count);
	if (!next) return;
	memset(next + state_count, 0,
			sizeof(*next) * (count - state_count));
	states = next;
	state_count = count;
}

void edge_bindings_reset(void) {
	free(states);
	states = NULL;
	state_count = 0;
}

static void compute_edge_zone(struct wlr_box *zone,
		const struct wlr_box *output, uint32_t edges, int range) {
	zone->x = output->x;
	zone->y = output->y;
	zone->width = output->width;
	zone->height = output->height;

	if (edges & WLR_EDGE_LEFT) {
		if (zone->width > range) zone->width = range;
	}
	if (edges & WLR_EDGE_TOP) {
		if (zone->height > range) zone->height = range;
	}
	if (edges & WLR_EDGE_RIGHT) {
		int w = range;
		int new_x = output->x + output->width - w;
		if (new_x > zone->x) {
			zone->width -= new_x - zone->x;
			zone->x = new_x;
			if (zone->width > w) zone->width = w;
		}
	}
	if (edges & WLR_EDGE_BOTTOM) {
		int h = range;
		int new_y = output->y + output->height - h;
		if (new_y > zone->y) {
			zone->height -= new_y - zone->y;
			zone->y = new_y;
			if (zone->height > h) zone->height = h;
		}
	}
}

static void edge_binding_exec(const char *cmd) {
	struct launcher_ctx *ctx = launcher_ctx_create_internal();
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		if (ctx) {
			const char *token = launcher_ctx_get_token_name(ctx);
			setenv("XDG_ACTIVATION_TOKEN", token, 1);
			setenv("DESKTOP_STARTUP_ID", token, 1);
		}
		close(STDIN_FILENO);
		close(STDOUT_FILENO);
		close(STDERR_FILENO);
		execlp("sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	} else if (pid < 0) {
		launcher_ctx_destroy(ctx);
	} else if (ctx) {
		ctx->pid = pid;
	}
}

void edge_bindings_check(double lx, double ly) {
	if (!config || !config->edge_bindings ||
			!config->edge_bindings->length) {
		return;
	}

	int count = config->edge_bindings->length;
	ensure_states(count);

	// Find output containing the cursor
	struct sway_output *output = NULL;
	for (int i = 0; i < root->outputs->length; ++i) {
		struct sway_output *o = root->outputs->items[i];
		struct wlr_box ob;
		output_get_box(o, &ob);
		if (lx >= ob.x && lx < ob.x + ob.width &&
				ly >= ob.y && ly < ob.y + ob.height) {
			output = o;
			break;
		}
	}
	if (!output) return;

	struct wlr_box output_box;
	output_get_box(output, &output_box);

	// Get focused workspace for filtering
	struct sway_seat *seat = input_manager_current_seat();
	struct sway_workspace *focused_ws = seat_get_focused_workspace(seat);

	for (int i = 0; i < count; ++i) {
		struct edge_binding_config *cfg =
			config->edge_bindings->items[i];
		struct edge_binding_state *st = &states[i];

		// Workspace filter
		if (!cfg->is_global) {
			bool ws_match = focused_ws &&
				cfg->workspace_name &&
				strcmp(focused_ws->name, cfg->workspace_name) == 0;
			if (!ws_match) {
				// Leaving a scoped workspace while inside its zone:
				// fire the exit command before resetting state.
				if (st->was_inside && cfg->fire_on_exit) {
					sway_log(SWAY_DEBUG,
						"[EDGE] ws-switch exit id=%d cmd=%s",
						i, cfg->command);
					edge_binding_exec(cfg->command);
				}
				st->was_inside = false;
				continue;
			}
		}

		// Compute zone rect
		struct wlr_box zone;
		compute_edge_zone(&zone, &output_box, cfg->edges, cfg->range);

		// Point-in-rect check
		bool inside = lx >= zone.x &&
			lx < zone.x + zone.width &&
			ly >= zone.y && ly < zone.y + zone.height;

		// Motion gate: suppress during drags/resizes if configured
		struct sway_seat *cur_seat = input_manager_current_seat();
		int pressed = cur_seat && cur_seat->cursor ?
			cur_seat->cursor->pressed_button_count > 0 : 0;
		if (cfg->motion_gate == -1 && pressed) {
			st->was_inside = inside;
			continue;
		}
		if (cfg->motion_gate == 1 && !pressed) {
			st->was_inside = inside;
			continue;
		}

		// Transition detection
		if (inside && !st->was_inside) {
			if (!cfg->fire_on_exit) {
				sway_log(SWAY_DEBUG, "[EDGE] enter id=%d cmd=%s",
					i, cfg->command);
				edge_binding_exec(cfg->command);
			}
		} else if (!inside && st->was_inside) {
			if (cfg->fire_on_exit) {
				sway_log(SWAY_DEBUG, "[EDGE] exit id=%d cmd=%s",
					i, cfg->command);
				edge_binding_exec(cfg->command);
			}
		}

		st->was_inside = inside;
	}
}
