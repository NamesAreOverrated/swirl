#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-server-core.h>
#include "log.h"
#include "sway/server.h"
#include "sway/tree/animation.h"

// ── Helpers ─────────────────────────────────────────────────────────────────

static double ease_out_cubic(double t) {
	double m = t - 1.0;
	return m * m * m + 1.0;
}

static double spring_value_at(double t, double zeta, double k) {
	double w0 = sqrt(k);
	if (fabs(zeta - 1.0) < 1e-6) {
		return 1.0 - exp(-w0 * t) * (1.0 + w0 * t);
	}
	if (zeta < 1.0) {
		double wd = w0 * sqrt(1.0 - zeta * zeta);
		double A = zeta / sqrt(1.0 - zeta * zeta);
		return 1.0 - exp(-zeta * w0 * t) * (cos(wd * t) + A * sin(wd * t));
	}
	double b = w0 * sqrt(zeta * zeta - 1.0);
	double A = zeta / sqrt(zeta * zeta - 1.0);
	return 1.0 - exp(-zeta * w0 * t) * (cosh(b * t) + A * sinh(b * t));
}

static double spring_vel_at(double t, double zeta, double k) {
	double w0 = sqrt(k);
	if (fabs(zeta - 1.0) < 1e-6) {
		return t * w0 * w0 * exp(-w0 * t);
	}
	if (zeta < 1.0) {
		double wd = w0 * sqrt(1.0 - zeta * zeta);
		double A = zeta / sqrt(1.0 - zeta * zeta);
		double e = exp(-zeta * w0 * t);
		double c = cos(wd * t);
		double s = sin(wd * t);
		return e * (zeta * w0 * (c + A * s) + wd * (s - A * c));
	}
	double b = w0 * sqrt(zeta * zeta - 1.0);
	double A = zeta / sqrt(zeta * zeta - 1.0);
	double e = exp(-zeta * w0 * t);
	double ch = cosh(b * t);
	double sh = sinh(b * t);
	return e * (zeta * w0 * (ch + A * sh) - b * (sh + A * ch));
}

static bool spring_is_settled(double t, double zeta, double k, double eps) {
	if (t <= 0.0) return false;
	return fabs(spring_value_at(t, zeta, k) - 1.0) < eps
		&& fabs(spring_vel_at(t, zeta, k)) < eps;
}

static double sec_since(struct timespec *start) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - start->tv_sec)
		+ (now.tv_nsec - start->tv_nsec) / 1e9;
}

static double lerp(double a, double b, double t) {
	return a + (b - a) * t;
}

// ── Per-property interpolation ─────────────────────────────────────────────

static bool prop_done(struct timespec *start, struct sway_prop_config *cfg) {
	if (cfg->type == SWAY_ANIM_EASE) {
		double elapsed_ms = sec_since(start) * 1000.0;
		return elapsed_ms >= cfg->duration_ms;
	}
	double t = sec_since(start);
	return spring_is_settled(t, cfg->damping_ratio,
		cfg->stiffness, cfg->epsilon);
}

static double prop_interp(struct timespec *start, struct sway_prop_config *cfg) {
	if (cfg->type == SWAY_ANIM_EASE) {
		double elapsed = sec_since(start) * 1000.0;
		double t = fmin(elapsed / cfg->duration_ms, 1.0);
		return ease_out_cubic(t);
	}
	double t = sec_since(start);
	return fmax(spring_value_at(t, cfg->damping_ratio,
		cfg->stiffness), 0.0);
}

// ── Scale node capture ──────────────────────────────────────────────────────

enum { SWAY_SCALE_BUF, SWAY_SCALE_RECT };

struct scale_node {
	int type; // SWAY_SCALE_BUF or SWAY_SCALE_RECT
	struct wlr_scene_node *node;
	double orig_x, orig_y, orig_w, orig_h;
};

static void free_scale_nodes(struct scale_node **nodes, int *count, int *cap) {
	free(*nodes);
	*nodes = NULL;
	*count = 0;
	*cap = 0;
}

static void collect_scale_nodes_recurse(struct wlr_scene_node *parent_node,
		struct scale_node **nodes, int *count, int *cap) {
	if (parent_node->type != WLR_SCENE_NODE_TREE) {
		return;
	}
	struct wlr_scene_tree *tree = wlr_scene_tree_from_node(parent_node);
	struct wlr_scene_node *child;
	wl_list_for_each(child, &tree->children, link) {
		if (child->type == WLR_SCENE_NODE_TREE) {
			collect_scale_nodes_recurse(child, nodes, count, cap);
		} else if (child->type == WLR_SCENE_NODE_BUFFER) {
			struct wlr_scene_buffer *buf = wlr_scene_buffer_from_node(child);
			if (*count >= *cap) {
				int new_cap = *cap ? *cap * 2 : 16;
				struct scale_node *tmp = realloc(*nodes,
					new_cap * sizeof(struct scale_node));
				if (!tmp) return;
				*nodes = tmp;
				*cap = new_cap;
			}
			(*nodes)[*count].type = SWAY_SCALE_BUF;
			(*nodes)[*count].node = child;
			(*nodes)[*count].orig_x = child->x;
			(*nodes)[*count].orig_y = child->y;
			(*nodes)[*count].orig_w = buf->dst_width;
			(*nodes)[*count].orig_h = buf->dst_height;
			(*count)++;
		} else if (child->type == WLR_SCENE_NODE_RECT) {
			struct wlr_scene_rect *rect = wlr_scene_rect_from_node(child);
			if (*count >= *cap) {
				int new_cap = *cap ? *cap * 2 : 16;
				struct scale_node *tmp = realloc(*nodes,
					new_cap * sizeof(struct scale_node));
				if (!tmp) return;
				*nodes = tmp;
				*cap = new_cap;
			}
			(*nodes)[*count].type = SWAY_SCALE_RECT;
			(*nodes)[*count].node = child;
			(*nodes)[*count].orig_x = child->x;
			(*nodes)[*count].orig_y = child->y;
			(*nodes)[*count].orig_w = rect->width;
			(*nodes)[*count].orig_h = rect->height;
			(*count)++;
		}
	}
}

// ── Core data structures ────────────────────────────────────────────────────

struct sway_anim {
	struct wl_list link;
	struct wlr_scene_node *node;
	struct wl_listener node_destroy;

	bool pos_active;
	struct sway_prop_config pos_cfg;
	double pos_from_x, pos_from_y, pos_to_x, pos_to_y;
	struct timespec pos_start;

	bool scale_active;
	struct sway_prop_config scale_cfg;
	double scale_from_w, scale_from_h, scale_to_w, scale_to_h;
	struct timespec scale_start;
	struct scale_node *scale_nodes;
	int scale_count;
	int scale_cap;

	bool alpha_active;
	struct sway_prop_config alpha_cfg;
	double alpha_from, alpha_to;
	struct timespec alpha_start;
};

static struct wl_list animations;
static struct wl_event_source *timer;

// ── Forward declarations ────────────────────────────────────────────────────

static void finish_anim(struct sway_anim *anim);
static int on_anim_tick(void *data);
static void handle_node_destroy(struct wl_listener *listener, void *data);

// ── Find or create ──────────────────────────────────────────────────────────

static struct sway_anim *anim_get_or_create(struct wlr_scene_node *node) {
	struct sway_anim *anim;
	wl_list_for_each(anim, &animations, link) {
		if (anim->node == node) {
			return anim;
		}
	}
	anim = calloc(1, sizeof(*anim));
	if (!anim) {
		return NULL;
	}
	anim->node = node;
	wl_list_insert(&animations, &anim->link);
	anim->node_destroy.notify = handle_node_destroy;
	wl_signal_add(&node->events.destroy, &anim->node_destroy);
	if (!timer) {
		timer = wl_event_loop_add_timer(server.wl_event_loop,
			on_anim_tick, NULL);
		wl_event_source_timer_update(timer, 1);
	}
	return anim;
}

// ── Apply / sync ────────────────────────────────────────────────────────────

static void anim_apply_pos(struct sway_anim *anim) {
	double v = prop_interp(&anim->pos_start, &anim->pos_cfg);
	double x = lerp(anim->pos_from_x, anim->pos_to_x, v);
	double y = lerp(anim->pos_from_y, anim->pos_to_y, v);
	wlr_scene_node_set_position(anim->node, x, y);
}

static void anim_apply_scale(struct sway_anim *anim) {
	double progress = prop_interp(&anim->scale_start, &anim->scale_cfg);
	double scale_x = anim->scale_from_w > 0
		? lerp(1.0, anim->scale_to_w / anim->scale_from_w, progress) : 1.0;
	double scale_y = anim->scale_from_h > 0
		? lerp(1.0, anim->scale_to_h / anim->scale_from_h, progress) : 1.0;

	for (int i = 0; i < anim->scale_count; i++) {
		struct scale_node *sn = &anim->scale_nodes[i];
		double nx = sn->orig_x * scale_x;
		double ny = sn->orig_y * scale_y;
		double nw = sn->orig_w * scale_x;
		double nh = sn->orig_h * scale_y;

		wlr_scene_node_set_position(sn->node, nx, ny);

		if (sn->type == SWAY_SCALE_BUF) {
			wlr_scene_buffer_set_dest_size(
				wlr_scene_buffer_from_node(sn->node),
				(int)nw, (int)nh);
		} else {
			wlr_scene_rect_set_size(
				wlr_scene_rect_from_node(sn->node),
				(int)nw, (int)nh);
		}
	}
}

void sway_anim_sync(void) {
	struct sway_anim *anim;
	wl_list_for_each(anim, &animations, link) {
		if (anim->pos_active) {
			anim_apply_pos(anim);
		}
		if (anim->scale_active) {
			anim_apply_scale(anim);
		}
	}
}

// ── Timer ───────────────────────────────────────────────────────────────────

static void finish_anim(struct sway_anim *anim) {
	if (anim->pos_active) {
		wlr_scene_node_set_position(anim->node,
			anim->pos_to_x, anim->pos_to_y);
	}
	if (anim->scale_active) {
		anim_apply_scale(anim);
	}
	wl_list_remove(&anim->link);
	wl_list_remove(&anim->node_destroy.link);
	free_scale_nodes(&anim->scale_nodes,
		&anim->scale_count, &anim->scale_cap);
	free(anim);
}

static int on_anim_tick(void *data) {
	struct sway_anim *anim, *tmp;
	bool running = false;

	wl_list_for_each_safe(anim, tmp, &animations, link) {
		bool all_done = true;

		if (anim->pos_active) {
			if (prop_done(&anim->pos_start, &anim->pos_cfg)) {
				wlr_scene_node_set_position(anim->node,
					anim->pos_to_x, anim->pos_to_y);
				anim->pos_active = false;
			} else {
				anim_apply_pos(anim);
				all_done = false;
			}
		}

		if (anim->scale_active) {
			if (prop_done(&anim->scale_start, &anim->scale_cfg)) {
				// set final target sizes
				double scale_x = anim->scale_from_w > 0
					? anim->scale_to_w / anim->scale_from_w : 1.0;
				double scale_y = anim->scale_from_h > 0
					? anim->scale_to_h / anim->scale_from_h : 1.0;
				for (int i = 0; i < anim->scale_count; i++) {
					struct scale_node *sn = &anim->scale_nodes[i];
					double nx = sn->orig_x * scale_x;
					double ny = sn->orig_y * scale_y;
					double nw = sn->orig_w * scale_x;
					double nh = sn->orig_h * scale_y;
					wlr_scene_node_set_position(sn->node, nx, ny);
					if (sn->type == SWAY_SCALE_BUF) {
						wlr_scene_buffer_set_dest_size(
							wlr_scene_buffer_from_node(sn->node),
							(int)nw, (int)nh);
					} else {
						wlr_scene_rect_set_size(
							wlr_scene_rect_from_node(sn->node),
							(int)nw, (int)nh);
					}
				}
				anim->scale_active = false;
			} else {
				anim_apply_scale(anim);
				all_done = false;
			}
		}

		if (all_done) {
			finish_anim(anim);
		} else {
			running = true;
		}
	}

	if (running) {
		wl_event_source_timer_update(timer, 16);
	} else {
		timer = NULL;
	}
	return 0;
}

// ── Node destruction ────────────────────────────────────────────────────────

static void handle_node_destroy(struct wl_listener *listener, void *data) {
	struct sway_anim *anim = wl_container_of(listener, anim, node_destroy);
	wl_list_remove(&anim->link);
	wl_list_remove(&anim->node_destroy.link);
	free_scale_nodes(&anim->scale_nodes,
		&anim->scale_count, &anim->scale_cap);
	free(anim);
}

// ── Public API ──────────────────────────────────────────────────────────────

void sway_anim_move(struct wlr_scene_node *node,
		double from_x, double from_y,
		double to_x, double to_y,
		struct sway_prop_config cfg) {
	struct sway_anim *anim = anim_get_or_create(node);
	if (!anim) {
		return;
	}

	anim->pos_from_x = from_x;
	anim->pos_from_y = from_y;
	anim->pos_to_x = to_x;
	anim->pos_to_y = to_y;
	anim->pos_cfg = cfg;
	anim->pos_active = true;
	clock_gettime(CLOCK_MONOTONIC, &anim->pos_start);
}

void sway_anim_scale(struct wlr_scene_node *node,
		double from_w, double from_h,
		double to_w, double to_h,
		struct sway_prop_config cfg) {
	struct sway_anim *anim = anim_get_or_create(node);
	if (!anim) {
		return;
	}

	// Only animate if both from dimensions are positive
	if (from_w <= 0 || from_h <= 0) {
		return;
	}

	anim->scale_from_w = from_w;
	anim->scale_from_h = from_h;
	anim->scale_to_w = to_w;
	anim->scale_to_h = to_h;
	anim->scale_cfg = cfg;

	// Re-capture child nodes (handles retarget)
	free_scale_nodes(&anim->scale_nodes,
		&anim->scale_count, &anim->scale_cap);
	collect_scale_nodes_recurse(node,
		&anim->scale_nodes, &anim->scale_count, &anim->scale_cap);

	anim->scale_active = true;
	clock_gettime(CLOCK_MONOTONIC, &anim->scale_start);
}

void sway_anim_alpha(struct wlr_scene_node *node,
		double from, double to,
		struct sway_prop_config cfg) {
	(void)node;
	(void)from;
	(void)to;
	(void)cfg;
	// TODO: requires offscreen rendering or custom shader
}

void sway_anim_init(struct wl_event_loop *loop) {
	(void)loop;
	wl_list_init(&animations);
}
