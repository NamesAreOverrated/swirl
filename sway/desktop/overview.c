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

struct overview_thumbnail {
	struct wl_list link;
	struct wlr_scene_buffer *sb;
	int w, h;
	float origin_y;
};

static bool overview_active = false;

static struct {
	struct wlr_scene_rect *bg;
	struct wl_list thumbnails;
	int n_thumbnails;
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
		int base_x, int base_y, float scale, int off_x, int off_y) {
	if (!node->enabled) {
		return;
	}
	int nx = base_x + node->x;

	if (node->type == WLR_SCENE_NODE_TREE) {
		struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
		struct wlr_scene_node *child;
		int ny = base_y + node->y;
		wl_list_for_each(child, &tree->children, link) {
			render_buffers(pass, renderer, child, nx, ny, scale, off_x, off_y);
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

		wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
			.texture = cb->texture,
			.dst_box = { .x = px - off_x, .y = py - off_y,
				.width = w, .height = h },
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
		struct overview_thumbnail *t, *tmp;
		wl_list_for_each_safe(t, tmp, &state.thumbnails, link) {
			wlr_scene_node_destroy(&t->sb->node);
			wl_list_remove(&t->link);
			free(t);
		}
		state.n_thumbnails = 0;
		if (state.bg) {
			wlr_scene_node_destroy(&state.bg->node);
			state.bg = NULL;
		}
		wlr_scene_node_set_enabled(&root->layers.overview->node, false);
		overview_active = false;
		return;
	}

	struct sway_workspace *ws = output_get_active_workspace(output);
	if (!ws) return;

	float scale = output->wlr_output->scale;
	struct wlr_renderer *renderer = output->wlr_output->renderer;
	struct wlr_allocator *alloc = output->wlr_output->allocator;
	const struct wlr_drm_format *fmt = NULL;
	if (output->wlr_output->swapchain) {
		fmt = &output->wlr_output->swapchain->format;
	}
	if (!fmt) return;

	int ow = (int)(output->wlr_output->width / scale);
	int oh = (int)(output->wlr_output->height / scale);
	int ox = (int)(output->scene_output->x);
	int oy = (int)(output->scene_output->y);

	state.bg = wlr_scene_rect_create(root->layers.overview,
		ow, oh, (float[4]){0.0, 0.0, 0.0, 0.6});
	if (state.bg) {
		wlr_scene_node_set_position(&state.bg->node, ox, oy);
	}

	wl_list_init(&state.thumbnails);
	state.n_thumbnails = 0;

	struct wlr_scene_node *container_node;
	wl_list_for_each(container_node, &ws->layers.tiling->children, link) {
		int min_x = INT_MAX, min_y = INT_MAX,
			max_x = INT_MIN, max_y = INT_MIN;
		collect_buffers(container_node, 0, 0, scale,
			&min_x, &min_y, &max_x, &max_y);
		if (max_x < min_x || max_y < min_y) continue;

		int cw = max_x - min_x;
		int ch = max_y - min_y;

		int bt = 0;
		if (config->border == B_PIXEL || config->border == B_NORMAL) {
			bt = (int)(config->border_thickness * scale);
		}
		int scw = cw + 2 * bt;
		int sch = ch + 2 * bt;

		struct wlr_swapchain *sc = wlr_swapchain_create(alloc, scw, sch, fmt);
		if (!sc) continue;
		struct wlr_buffer *buf = wlr_swapchain_acquire(sc);
		if (!buf) { wlr_swapchain_destroy(sc); continue; }

		struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
			renderer, buf, &(struct wlr_buffer_pass_options){0});
		if (!pass) {
			wlr_buffer_unlock(buf);
			wlr_swapchain_destroy(sc);
			continue;
		}

		wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
			.box = { .x = 0, .y = 0, .width = scw, .height = sch },
			.color = {0.05, 0.05, 0.1, 1},
		});

		if (bt > 0) {
			const float *col = config->border_colors.unfocused.border;
			wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
				.box = { .x = 0, .y = 0, .width = scw, .height = sch },
				.color = { col[0], col[1], col[2], col[3] },
			});
		}

		render_buffers(pass, renderer, container_node, 0, 0, scale,
			min_x - bt, min_y - bt);

		if (!wlr_render_pass_submit(pass)) {
			wlr_buffer_unlock(buf);
			wlr_swapchain_destroy(sc);
			continue;
		}

		struct overview_thumbnail *t = calloc(1, sizeof(*t));
		if (!t) { wlr_buffer_unlock(buf); wlr_swapchain_destroy(sc); continue; }
		t->w = scw;
		t->h = sch;
		t->origin_y = (float)min_y / scale;
		t->sb = wlr_scene_buffer_create(root->layers.overview, buf);
		wlr_buffer_unlock(buf);
		wlr_swapchain_destroy(sc);

		if (t->sb) {
			wl_list_insert(state.thumbnails.prev, &t->link);
			state.n_thumbnails++;
		} else {
			free(t);
		}
	}

	int n = state.n_thumbnails;
	if (n == 0) {
		wlr_scene_node_set_enabled(&root->layers.overview->node, true);
		overview_active = true;
		return;
	}

	float min_oy = INFINITY, max_bottom = 0, max_th = 0, sum_w = 0;
	struct overview_thumbnail *t;
	wl_list_for_each(t, &state.thumbnails, link) {
		float tw = (float)t->w / scale;
		float th = (float)t->h / scale;
		if (th > max_th) max_th = th;
		sum_w += tw;
		if (t->origin_y < min_oy) min_oy = t->origin_y;
		float bot = t->origin_y + th;
		if (bot > max_bottom) max_bottom = bot;
	}

	float gap = 8.0f;
	float avail_w = ow * 0.9f;
	float avail_h = oh * 0.85f;
	float denom = sum_w + gap * (n - 1);
	float fit = 1.0f;
	if (max_th > 0 && denom > 0) {
		fit = fminf(1.0f, fminf(avail_h / max_th, avail_w / denom));
	}
	float y_span = (max_bottom - min_oy) * fit;
	if (y_span > avail_h) {
		fit *= avail_h / y_span;
		y_span = avail_h;
	}

	float total_w = denom * fit;
	float start_x = ox + (ow - total_w) / 2.0f;
	float overview_oy = oy + (oh - y_span) / 2.0f;

	wl_list_for_each(t, &state.thumbnails, link) {
		float tw = (float)t->w / scale * fit;
		float th = (float)t->h / scale * fit;
		float ty = overview_oy + (t->origin_y - min_oy) * fit;

		wlr_scene_buffer_set_dest_size(t->sb, (int)tw, (int)th);
		wlr_scene_node_set_position(&t->sb->node,
			(int)start_x, (int)ty);
		start_x += tw + gap * fit;
	}

	wlr_scene_node_set_enabled(&root->layers.overview->node, true);
	overview_active = true;
}
