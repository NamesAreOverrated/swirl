#ifndef _SWAY_ANIMATION_H
#define _SWAY_ANIMATION_H

/*
 * ANIMATION SYSTEM
 *
 * DATA-DRIVEN:  Operates purely on wlr_scene_node pointers.
 *               Knows nothing about containers, views, columns, or workspaces.
 *
 * NEVER RESTART: All calls to sway_anim_move append a new entry to the global
 *                queue.  Entries play sequentially per-node — when the oldest
 *                entry for a node finishes, the next queued entry activates
 *                automatically.  No existing entry is ever mutated.
 *
 * NEVER CANCEL:  Once queued, an animation runs to completion (spring
 *                settles or ease finishes).  There is no cancel API.
 *
 * QUEUE MODEL:   The caller always provides from_x/from_y (typically the
 *                scene node's current visual position).  For queued entries,
 *                from_x/from_y is updated on activation to the then-current
 *                visual position, chaining seamlessly.
 *
 * The animation timer fires at ~60 Hz while any animations are active.
 * When the list is empty the timer is disabled (zero idle cost).
 */

#include <stdbool.h>
#include <wlr/types/wlr_scene.h>

struct sway_anim;
struct wl_event_loop;

enum sway_anim_type {
	SWAY_ANIM_EASE,
	SWAY_ANIM_SPRING,
};

struct sway_anim_config {
	enum sway_anim_type type;
	int duration_ms;
	double damping_ratio; // spring only (0.1–10.0)
	double stiffness;     // spring only
	double epsilon;       // spring settling threshold
};

// Queue a position animation.  Always appends — never mutates existing entries.
void sway_anim_move(struct wlr_scene_node *node,
	double from_x, double from_y,
	double to_x, double to_y,
	struct sway_anim_config cfg);

// TODO: Queue a scale animation.  Needs offscreen rendering.
void sway_anim_scale(struct wlr_scene_node *node,
	double from, double to, struct sway_anim_config cfg);

// TODO: Queue an alpha animation.  Needs offscreen rendering.
void sway_anim_alpha(struct wlr_scene_node *node,
	double from, double to, struct sway_anim_config cfg);

// Override all animated node positions in the scene tree.
// Called after the transaction arrange pass.  Skips queued entries.
void sway_anim_sync(void);

// Initialize the animation system.
void sway_anim_init(struct wl_event_loop *loop);

#endif
