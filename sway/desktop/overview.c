#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <drm_fourcc.h>
#include <stdbool.h>
#include <stdlib.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/pass.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include "log.h"
#include "sway/config.h"
#include "sway/desktop/overview.h"
#include "sway/output.h"
#include "sway/server.h"
#include "sway/tree/container.h"
#include "sway/tree/root.h"
#include "sway/tree/workspace.h"

static bool overview_active = false;

static struct {
	struct wlr_scene_rect *bg;
	struct wlr_scene_buffer *scene_buffer;
} state;

bool overview_is_active(void) {
	return overview_active;
}

static void collect_buffers(struct wlr_scene_node *node,
		int base_x, int base_y, float scale,
		int *min_x, int *min_y, int *max_x, int *max_y) {
	if (!node->enabled) {
		return;
	}
	int nx = base_x + node->x;

	if (node->type == WLR_SCENE_NODE_TREE) {
		struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
		struct wlr_scene_node *child;
		int ny = base_y + node->y;
		wl_list_for_each(child, &tree->children, link) {
			collect_buffers(child, nx, ny, scale, min_x, min_y, max_x, max_y);
		}
		return;
	}

	if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_buffer *sb = wlr_scene_buffer_from_node(node);
		if (!sb->buffer) return;
		if (!wlr_client_buffer_get(sb->buffer)) return;
		int w = sb->buffer->width;
		int h = sb->buffer->height;
		if (sb->dst_width > 0) w = (int)(sb->dst_width * scale);
		if (sb->dst_height > 0) h = (int)(sb->dst_height * scale);
		int px = (int)(nx * scale);
		int py = (int)((base_y + node->y) * scale);
		if (px < *min_x) *min_x = px;
		if (py < *min_y) *min_y = py;
		if (px + w > *max_x) *max_x = px + w;
		if (py + h > *max_y) *max_y = py + h;
		return;
	}
}

static void render_buffers(struct wlr_render_pass *pass,
		struct wlr_renderer *renderer, struct wlr_scene_node *node,
		int base_x, int base_y, float scale) {
	if (!node->enabled) {
		return;
	}
	int nx = base_x + node->x;

	if (node->type == WLR_SCENE_NODE_TREE) {
		struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
		struct wlr_scene_node *child;
		int ny = base_y + node->y;
		wl_list_for_each(child, &tree->children, link) {
			render_buffers(pass, renderer, child, nx, ny, scale);
		}
		return;
	}

	if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_buffer *sb = wlr_scene_buffer_from_node(node);
		if (!sb->buffer) return;
		struct wlr_client_buffer *cb = wlr_client_buffer_get(sb->buffer);
		if (!cb || !cb->texture) return;
		int px = (int)(nx * scale);
		int py = (int)((base_y + node->y) * scale);
		int w = sb->buffer->width;
		int h = sb->buffer->height;
		if (sb->dst_width > 0) w = (int)(sb->dst_width * scale);
		if (sb->dst_height > 0) h = (int)(sb->dst_height * scale);

		if (config->border != B_NONE) {
			int bt = (int)(config->border_thickness * scale);
			if (bt > 0) {
				const float *col = config->border_colors.unfocused.border;
				wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
					.box = { .x = px - bt, .y = py - bt,
						.width = w + 2 * bt, .height = h + 2 * bt },
					.color = { col[0], col[1], col[2], col[3] },
				});
			}
		}

		wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
			.texture = cb->texture,
			.dst_box = { .x = px, .y = py, .width = w, .height = h },
			.transform = WL_OUTPUT_TRANSFORM_NORMAL,
			.alpha = &sb->opacity,
		});
		return;
	}
}

void overview_toggle(void) {
	struct sway_output *output = NULL;
	if (root && root->outputs && root->outputs->length > 0) {
		output = root->outputs->items[0];
	}
	if (!output) {
		sway_log(SWAY_ERROR, "overview: no output");
		return;
	}

	if (overview_active) {
		if (state.bg) {
			wlr_scene_node_destroy(&state.bg->node);
			state.bg = NULL;
		}
		if (state.scene_buffer) {
			wlr_scene_node_destroy(&state.scene_buffer->node);
			state.scene_buffer = NULL;
		}
		wlr_scene_node_set_enabled(&root->layers.overview->node, false);
		overview_active = false;
		return;
	}

	struct sway_workspace *ws = output_get_active_workspace(output);
	if (!ws) return;

	float scale = output->wlr_output->scale;
	int min_x = INT_MAX, min_y = INT_MAX, max_x = INT_MIN, max_y = INT_MIN;
	struct wlr_scene_node *child;
	wl_list_for_each(child, &ws->layers.tiling->children, link) {
		collect_buffers(child, 0, 0, scale, &min_x, &min_y, &max_x, &max_y);
	}
	if (max_x < min_x || max_y < min_y) {
		sway_log(SWAY_DEBUG, "overview: no buffers found");
		return;
	}
	int ws_w = max_x - min_x;
	int ws_h = max_y - min_y;
	sway_log(SWAY_INFO, "overview: bounds %dx%d (phys), scale %.2f", ws_w, ws_h, scale);

	struct wlr_renderer *renderer = output->wlr_output->renderer;
	struct wlr_allocator *alloc = output->wlr_output->allocator;

	const struct wlr_drm_format *fmt = NULL;
	if (output->wlr_output->swapchain) {
		fmt = &output->wlr_output->swapchain->format;
	}
	if (!fmt) return;

	struct wlr_swapchain *swapchain = wlr_swapchain_create(alloc, ws_w, ws_h, fmt);
	if (!swapchain) return;

	struct wlr_buffer *buffer = wlr_swapchain_acquire(swapchain);
	if (!buffer) { wlr_swapchain_destroy(swapchain); return; }

	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
		renderer, buffer, &(struct wlr_buffer_pass_options){0});
	if (!pass) {
		wlr_buffer_unlock(buffer);
		wlr_swapchain_destroy(swapchain);
		return;
	}

	wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
		.box = { .x = 0, .y = 0, .width = ws_w, .height = ws_h },
		.color = {0.05, 0.05, 0.1, 1},
	});

	wl_list_for_each(child, &ws->layers.tiling->children, link) {
		render_buffers(pass, renderer, child, 0, 0, scale);
	}

	if (!wlr_render_pass_submit(pass)) {
		wlr_buffer_unlock(buffer);
		wlr_swapchain_destroy(swapchain);
		return;
	}

	int ow = (int)(output->wlr_output->width / scale);
	int oh = (int)(output->wlr_output->height / scale);
	int ox = (int)(output->scene_output->x);
	int oy = (int)(output->scene_output->y);

	state.bg = wlr_scene_rect_create(root->layers.overview,
		ow, oh, (float[4]){0.0, 0.0, 0.0, 0.6});
	if (state.bg) {
		wlr_scene_node_set_position(&state.bg->node, ox, oy);
	}

	int tw = (int)(ws_w / scale);
	int th = (int)(ws_h / scale);
	float max_w = ow * 0.7f;
	float max_h = oh * 0.7f;
	if (tw > max_w || th > max_h) {
		float fit = fminf(max_w / tw, max_h / th);
		tw = (int)(tw * fit);
		th = (int)(th * fit);
	}
	if (tw < 1) tw = 1;
	if (th < 1) th = 1;

	state.scene_buffer = wlr_scene_buffer_create(root->layers.overview, buffer);
	if (state.scene_buffer) {
		wlr_scene_buffer_set_dest_size(state.scene_buffer, tw, th);
		wlr_scene_node_set_position(&state.scene_buffer->node,
			ox + ow/2 - tw/2, oy + oh/2 - th/2);
	}

	wlr_buffer_unlock(buffer);
	wlr_swapchain_destroy(swapchain);

	wlr_scene_node_set_enabled(&root->layers.overview->node, true);
	overview_active = true;
}
