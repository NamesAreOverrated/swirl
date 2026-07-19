#ifndef _SWAY_ANIMATION_H
#define _SWAY_ANIMATION_H

/*
 * ANIMATION SYSTEM
 *
 * DATA-DRIVEN:  Operates purely on wlr_scene_node pointers.
 *               One entry per node — retargeting overwrites fields.
 *
 * NEVER CANCEL:  No cancel API.  An entry finishes when all active
 *                properties have settled (spring) or expired (ease).
 *
 * Each animation property (pos, scale, alpha) has its own config
 * and start time.  Calling the function again always restarts
 * that property's timer.
 */

#include <stdbool.h>
#include <wlr/types/wlr_scene.h>

struct wl_event_loop;

enum sway_anim_type {
	SWAY_ANIM_EASE,
	SWAY_ANIM_SPRING,
};

struct sway_prop_config {
	enum sway_anim_type type;
	int duration_ms;
	double damping_ratio; // spring only (0.1–10.0)
	double stiffness;     // spring only
	double epsilon;       // spring settling threshold
};

// Queue or retarget a position animation.
// from_x/from_y should be the current visual position (typically node->x,y).
// If the node already has an entry, restarts the pos timer.
void sway_anim_move(struct wlr_scene_node *node,
	double from_x, double from_y,
	double to_x, double to_y,
	struct sway_prop_config cfg);

// TODO: Queue a scale animation.  Needs offscreen rendering.
void sway_anim_scale(struct wlr_scene_node *node,
	double from, double to,
	struct sway_prop_config cfg);

// TODO: Queue an alpha animation.  Needs offscreen rendering.
void sway_anim_alpha(struct wlr_scene_node *node,
	double from, double to,
	struct sway_prop_config cfg);

// Override animated node positions after the transaction arrange pass.
void sway_anim_sync(void);

// Initialize the animation system (called once at server startup).
void sway_anim_init(struct wl_event_loop *loop);

#endif
