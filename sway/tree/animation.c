#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdlib.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_scene.h>
#include "log.h"
#include "sway/server.h"
#include "sway/tree/animation.h"
#include "sway/tree/root.h"

// ── Helpers ─────────────────────────────────────────────────────────────────

static double spring_settle_time(double zeta, double k, double eps) {
	// Upper-bound estimate: time for spring to stay within eps forever
	double w0 = sqrt(k);
	double env = -log(eps) / (zeta * w0);
	return fmax(env * 2.0, 0.0);
}

// ── Timer ───────────────────────────────────────────────────────────────────

static struct wl_event_source *anim_timer;
static int on_anim_tick(void *data);

static void ensure_timer(void) {
	if (!anim_timer) {
		anim_timer = wl_event_loop_add_timer(server.wl_event_loop,
			on_anim_tick, NULL);
		wl_event_source_timer_update(anim_timer, 1);
	}
}

static int on_anim_tick(void *data) {
	sway_anim_sync();
	return 0;
}

// ── Public API ──────────────────────────────────────────────────────────────

void sway_anim_move(struct wlr_scene_node *node,
		double from_x, double from_y,
		double to_x, double to_y,
		struct sway_prop_config cfg) {
	(void)from_x;
	(void)from_y;

	// Lazily create animator on the root scene
	struct wlr_scene *scene = root ? root->root_scene : NULL;
	if (scene && !scene->animator) {
		wlr_scene_animator_create(scene);
	}

	if (!scene || !scene->animator) {
		wlr_scene_node_set_position(node, to_x, to_y);
		return;
	}

	uint32_t duration_ns;
	float (*easing)(float t);

	if (cfg.type == SWAY_ANIM_EASE) {
		duration_ns = (uint32_t)cfg.duration_ms * 1000000ULL;
		easing = wlr_scene_easing_ease_out_cubic;
	} else {
		// Spring: approximate as ease with settle time
		double settle_ms = spring_settle_time(
			cfg.damping_ratio, cfg.stiffness, cfg.epsilon) * 1000.0;
		duration_ns = (uint32_t)fmin(settle_ms * 1000000ULL,
			(double)UINT32_MAX);
		easing = NULL; // wlr_anim_spec allows NULL easing
	}

	struct wlr_anim_spec spec = {
		.duration_ns = duration_ns,
		.easing = easing,
		.done = NULL,
		.done_data = NULL,
	};

	wlr_scene_node_set_position_anim(node, (int)to_x, (int)to_y, &spec);
	ensure_timer();
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

void sway_anim_sync(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint64_t now_ns = (uint64_t)ts.tv_sec * 1000000000ULL +
		(uint64_t)ts.tv_nsec;

	bool active = false;

	// Tick all root-level scenes
	struct sway_root *r = root;
	if (r && r->root_scene && r->root_scene->animator) {
		wlr_scene_animator_tick(r->root_scene->animator, now_ns);
		active = r->root_scene->animator->active;
	}

	// Check animation scenes (view->image_capture_scene)
	// Currently none are animated

	if (active) {
		wl_event_source_timer_update(anim_timer, 16);
	} else if (anim_timer) {
		wl_event_source_remove(anim_timer);
		anim_timer = NULL;
	}
}

void sway_anim_init(struct wl_event_loop *loop) {
	(void)loop;
	anim_timer = NULL;
}
