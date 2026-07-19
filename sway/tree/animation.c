#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-server-core.h>
#include "log.h"
#include "sway/server.h"
#include "sway/tree/animation.h"

/*
 * DATA-DRIVEN  -> operates on wlr_scene_node *, knows nothing about sway types
 * NEVER RESTART -> existing animation only updates to_x/to_y, no snap
 * NEVER CANCEL  -> no cancel API, always runs to completion
 */

struct sway_anim {
	struct wl_list link;
	struct wlr_scene_node *node;
	enum sway_anim_type type;
	bool queued;

	double from_x, from_y, to_x, to_y;

	bool scale_active;
	double from_scale, to_scale;

	bool alpha_active;
	double from_alpha, to_alpha;

	int duration_ms;

	double damping_ratio;
	double stiffness;
	double epsilon;

	struct timespec start;

	void (*on_done)(void *data);
	void *data;

	struct wl_listener node_destroy;
};

static struct wl_list animations;
static struct wl_event_source *timer;

// ── Easing curves ───────────────────────────────────────────────────────────

static double ease_out_cubic(double t) {
	double m = t - 1.0;
	return m * m * m + 1.0;
}

// ── Spring math ─────────────────────────────────────────────────────────────

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

// ── Helpers ─────────────────────────────────────────────────────────────────

static double sec_since(struct timespec *start) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - start->tv_sec)
		+ (now.tv_nsec - start->tv_nsec) / 1e9;
}

static double lerp(double a, double b, double t) {
	return a + (b - a) * t;
}

// True if a newer entry for the same node is closer to the tail (prev).
// Oldest entry is active; all newer ones are queued behind it.
static bool has_older_for_node(struct sway_anim *anim) {
	struct wl_list *n = anim->link.next;
	while (n != &animations) {
		struct sway_anim *a = wl_container_of(n, a, link);
		if (a->node == anim->node) {
			return true;
		}
		n = n->next;
	}
	return false;
}

static void activate_anim(struct sway_anim *anim) {
	anim->from_x = anim->node->x;
	anim->from_y = anim->node->y;
	anim->queued = false;
	clock_gettime(CLOCK_MONOTONIC, &anim->start);
}

// ── Apply / sync ────────────────────────────────────────────────────────────

static void anim_apply(struct sway_anim *anim) {
	double v;

	if (anim->type == SWAY_ANIM_EASE) {
		double elapsed = sec_since(&anim->start) * 1000.0;
		double t = elapsed / anim->duration_ms;
		t = fmin(t, 1.0);
		v = ease_out_cubic(t);
	} else {
		double t = sec_since(&anim->start);
		v = fmax(spring_value_at(t, anim->damping_ratio,
			anim->stiffness), 0.0);
	}

	double x = lerp(anim->from_x, anim->to_x, v);
	double y = lerp(anim->from_y, anim->to_y, v);
	wlr_scene_node_set_position(anim->node, x, y);
}

// ── Timer ───────────────────────────────────────────────────────────────────

static void finish_anim(struct sway_anim *anim) {
	wlr_scene_node_set_position(anim->node,
		anim->to_x, anim->to_y);
	wl_list_remove(&anim->link);
	wl_list_remove(&anim->node_destroy.link);
	if (anim->on_done) {
		anim->on_done(anim->data);
	}
	free(anim);
}

static int on_anim_tick(void *data) {
	struct sway_anim *anim, *tmp;
	bool running = false;

	wl_list_for_each_safe(anim, tmp, &animations, link) {
		if (anim->queued) {
			continue;
		}

		if (anim->type == SWAY_ANIM_EASE) {
			double elapsed = sec_since(&anim->start) * 1000.0;
			if (elapsed >= anim->duration_ms) {
				finish_anim(anim);
				continue;
			}
		} else {
			double t = sec_since(&anim->start);
			if (spring_is_settled(t, anim->damping_ratio,
					anim->stiffness, anim->epsilon)) {
				finish_anim(anim);
				continue;
			}
		}
		anim_apply(anim);
		running = true;
	}

	// Activate queued entries whose older sibling just finished
	wl_list_for_each_safe(anim, tmp, &animations, link) {
		if (anim->queued && !has_older_for_node(anim)) {
			activate_anim(anim);
			running = true;
		}
	}

	if (running) {
		wl_event_source_timer_update(timer, 16);
		return 0;
	}
	timer = NULL;
	return 0;
}

// ── Node destruction ────────────────────────────────────────────────────────

static void handle_node_destroy(struct wl_listener *listener, void *data) {
	struct sway_anim *anim = wl_container_of(listener, anim, node_destroy);
	wl_list_remove(&anim->link);
	wl_list_remove(&anim->node_destroy.link);
	free(anim);
}

// ── Public API ──────────────────────────────────────────────────────────────

void sway_anim_move(struct wlr_scene_node *node,
		double from_x, double from_y,
		double to_x, double to_y,
		struct sway_anim_config cfg) {
	struct sway_anim *anim = calloc(1, sizeof(*anim));
	if (!anim) {
		return;
	}
	anim->node = node;
	anim->type = cfg.type;
	anim->from_x = from_x;
	anim->from_y = from_y;
	anim->to_x = to_x;
	anim->to_y = to_y;
	anim->duration_ms = cfg.duration_ms;
	anim->damping_ratio = cfg.damping_ratio;
	anim->stiffness = cfg.stiffness;
	anim->epsilon = cfg.epsilon;

	// Mark queued if a non-queued entry for the same node exists
	anim->queued = has_older_for_node(anim);

	anim->node_destroy.notify = handle_node_destroy;
	wl_signal_add(&node->events.destroy, &anim->node_destroy);

	wl_list_insert(&animations, &anim->link);

	if (!anim->queued) {
		wlr_scene_node_set_position(node, from_x, from_y);
		clock_gettime(CLOCK_MONOTONIC, &anim->start);
	}

	if (!timer) {
		timer = wl_event_loop_add_timer(server.wl_event_loop,
			on_anim_tick, NULL);
		wl_event_source_timer_update(timer, 1);
	}
}

void sway_anim_scale(struct wlr_scene_node *node,
		double from, double to, struct sway_anim_config cfg) {
	(void)node;
	(void)from;
	(void)to;
	(void)cfg;
	// TODO: requires offscreen rendering or custom shader
}

void sway_anim_alpha(struct wlr_scene_node *node,
		double from, double to, struct sway_anim_config cfg) {
	(void)node;
	(void)from;
	(void)to;
	(void)cfg;
	// TODO: requires offscreen rendering or custom shader
}

void sway_anim_sync(void) {
	struct sway_anim *anim;
	wl_list_for_each(anim, &animations, link) {
		if (anim->queued) {
			continue;
		}
		anim_apply(anim);
	}
}

void sway_anim_init(struct wl_event_loop *loop) {
	(void)loop;
	wl_list_init(&animations);
}
