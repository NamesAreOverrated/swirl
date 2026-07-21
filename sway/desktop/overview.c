#define _POSIX_C_SOURCE 200809L
#include <cairo.h>
#include <math.h>
#include <drm_fourcc.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/pass.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include "log.h"
#include "sway/config.h"
#include "sway/desktop/overview.h"
#include "sway/input/seat.h"
#include "sway/output.h"
#include "sway/scene_descriptor.h"
#include "sway/server.h"
#include "sway/tree/arrange.h"
#include "sway/tree/container.h"
#include "sway/tree/root.h"
#include "sway/tree/workspace.h"

struct overview_thumbnail {
	struct wl_list link;
	struct wlr_scene_buffer *sb;
	struct wlr_scene_buffer *badge_sb;
	struct sway_container *con;
	int w, h;
	float origin_y;
};

static bool overview_active = false;

static struct {
	struct wlr_scene_rect *bg;
	struct wl_list thumbnails;
	int n_thumbnails;
	int digit_buf;
	int digit_count;
	struct sway_container *focus_con;
	struct sway_seat *seat;
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

bool overview_handle_key(xkb_keysym_t sym) {
	if (!overview_active) return false;

	if (sym >= XKB_KEY_0 && sym <= XKB_KEY_9) {
		state.digit_buf = state.digit_buf * 10 + (sym - XKB_KEY_0);
		state.digit_count++;
		if (state.digit_count >= 2) {
			int idx = state.digit_buf;
			struct sway_container *focus = state.focus_con;
			state.digit_buf = 0;
			state.digit_count = 0;
			if (idx >= 1 && idx <= state.n_thumbnails && focus) {
				struct overview_thumbnail *t;
				int i = 0;
				wl_list_for_each(t, &state.thumbnails, link) {
					if (i == idx - 1) break;
					i++;
				}
				if (t && t->con && t->con != focus) {
					struct sway_container *focus_col =
						container_toplevel_ancestor(focus);
					struct sway_container *target_col =
						container_toplevel_ancestor(t->con);
					if (focus_col != target_col) {
						struct sway_workspace *ws =
							target_col->pending.workspace;
						int fi = list_find(ws->tiling, focus_col);
						if (fi >= 0) {
							workspace_insert_column(ws,
								target_col, fi + 1);
							arrange_workspace(ws);
						}
					}
					seat_set_focus_container(state.seat, t->con);
				}
			}
			overview_toggle();
		}
		return true;
	}

	if (sym == XKB_KEY_Escape) {
		overview_toggle();
		return true;
	}

	if (sym == XKB_KEY_BackSpace && state.digit_count > 0) {
		state.digit_buf /= 10;
		state.digit_count--;
		return true;
	}

	state.digit_buf = 0;
	state.digit_count = 0;
	return false;
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
			wlr_scene_node_destroy(&t->badge_sb->node);
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

	state.seat = input_manager_current_seat();
	state.focus_con = seat_get_focused_container(state.seat);
	state.digit_buf = 0;
	state.digit_count = 0;

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
	int con_idx = 0;
	wl_list_for_each(container_node, &ws->layers.tiling->children, link) {
		int min_x = INT_MAX, min_y = INT_MAX,
			max_x = INT_MIN, max_y = INT_MIN;
		collect_buffers(container_node, 0, 0, scale,
			&min_x, &min_y, &max_x, &max_y);
		if (max_x < min_x || max_y < min_y) continue;
		con_idx++;

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
			float col[4] = {
				config->border_colors.unfocused.border[0],
				config->border_colors.unfocused.border[1],
				config->border_colors.unfocused.border[2],
				config->border_colors.unfocused.border[3],
			};
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
		t->con = scene_descriptor_try_get(container_node,
			SWAY_SCENE_DESC_CONTAINER);

		// Create badge overlay (separate small buffer, not scaled with thumbnail)
		int badge_w = (int)(26 * scale);
		int badge_h = (int)(26 * scale);
		struct wlr_swapchain *badge_sc = wlr_swapchain_create(alloc,
			badge_w, badge_h, fmt);
		struct wlr_buffer *badge_buf = NULL;
		if (badge_sc) {
			badge_buf = wlr_swapchain_acquire(badge_sc);
		}
		if (badge_buf) {
			struct wlr_render_pass *badge_pass = wlr_renderer_begin_buffer_pass(
				renderer, badge_buf, &(struct wlr_buffer_pass_options){0});
			if (badge_pass) {
				char label[16];
				snprintf(label, sizeof(label), "%02d", con_idx);

				cairo_surface_t *surf = cairo_image_surface_create(
					CAIRO_FORMAT_ARGB32, badge_w, badge_h);
				cairo_t *cr = cairo_create(surf);

				cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
				cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.7);
				cairo_rectangle(cr, 0, 0, badge_w, badge_h);
				cairo_fill(cr);

				cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
				cairo_select_font_face(cr, "sans-serif",
					CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
				cairo_set_font_size(cr, 14 * scale);

				cairo_text_extents_t ext;
				cairo_text_extents(cr, label, &ext);
				cairo_move_to(cr,
					(badge_w - ext.width) / 2.0 - ext.x_bearing,
					(badge_h - ext.height) / 2.0 - ext.y_bearing);
				cairo_show_text(cr, label);
				cairo_destroy(cr);

				struct wlr_texture *tex = wlr_texture_from_pixels(renderer,
					DRM_FORMAT_ARGB8888,
					cairo_image_surface_get_stride(surf),
					badge_w, badge_h,
					cairo_image_surface_get_data(surf));
				cairo_surface_destroy(surf);

				if (tex) {
					float alpha = 1.0f;
					wlr_render_pass_add_texture(badge_pass,
						&(struct wlr_render_texture_options){
						.texture = tex,
						.dst_box = { .x = 0, .y = 0,
							.width = badge_w, .height = badge_h },
						.transform = WL_OUTPUT_TRANSFORM_NORMAL,
						.alpha = &alpha,
					});
					wlr_texture_destroy(tex);
				}
				wlr_render_pass_submit(badge_pass);
			}
			t->badge_sb = wlr_scene_buffer_create(
				root->layers.overview, badge_buf);
			wlr_buffer_unlock(badge_buf);
		} else {
			t->badge_sb = NULL;
		}
		if (badge_sc) wlr_swapchain_destroy(badge_sc);

		t->sb = wlr_scene_buffer_create(root->layers.overview, buf);
		wlr_buffer_unlock(buf);
		wlr_swapchain_destroy(sc);

		if (t->sb) {
			wl_list_insert(state.thumbnails.prev, &t->link);
			state.n_thumbnails++;
		} else {
			if (t->badge_sb) wlr_scene_node_destroy(&t->badge_sb->node);
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

		if (t->badge_sb) {
			int bsz = (int)(26 * fit);
			if (bsz < 18) bsz = 18;
			int bpad = (int)(2 * fit);
			if (bpad < 1) bpad = 1;
			wlr_scene_buffer_set_dest_size(t->badge_sb, bsz, bsz);
			wlr_scene_node_set_position(&t->badge_sb->node,
				(int)start_x + bpad, (int)ty + bpad);
			wlr_scene_node_raise_to_top(&t->badge_sb->node);
		}

		start_x += tw + gap * fit;
	}

	wlr_scene_node_set_enabled(&root->layers.overview->node, true);
	overview_active = true;
}
