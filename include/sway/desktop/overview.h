#ifndef _SWAY_OVERVIEW_H
#define _SWAY_OVERVIEW_H

#include <stdbool.h>
#include <xkbcommon/xkbcommon.h>
#include "list.h"

enum overview_scope {
	OVERVIEW_CURRENT,
	OVERVIEW_ALL,
	OVERVIEW_MINIMIZED,
};

enum overview_action {
	OVERVIEW_FOCUS,
	OVERVIEW_PULL,
	OVERVIEW_SWAP,
	OVERVIEW_RESTORE,
};

enum overview_content {
	OVERVIEW_CONTENT_TILED     = 1 << 0,
	OVERVIEW_CONTENT_FLOATING  = 1 << 1,
	OVERVIEW_CONTENT_MINIMIZED = 1 << 2,
};

bool overview_is_active(void);

struct sway_seat;
struct sway_container;

void overview_pull_container(struct sway_container *target,
		struct sway_seat *seat);
void overview_swap_container(struct sway_container *focus_top,
		struct sway_container *target, struct sway_seat *seat);

// A single eligible overview target: the top-level container plus the content
// type it was gathered as (so clients can route actions / label it).
struct overview_target {
	struct sway_container *con;
	enum overview_content type; // bit from OVERVIEW_CONTENT_*
};

// Gather the containers the overview would present for the given scope/action,
// applying the same content filtering (and the swap focus-type override) as the
// graphical overview. `content_flags` of 0 means "use the overview defaults".
// Fills `out` with heap-allocated `struct overview_target` entries.
void overview_collect_targets(list_t *out, enum overview_scope scope,
		enum overview_action action, int content_flags,
		struct sway_seat *seat);

void overview_set_params(enum overview_scope scope,
		enum overview_action action, int content_flags);
void overview_toggle(void);
bool overview_handle_key(xkb_keysym_t sym);
bool overview_handle_key_release(xkb_keysym_t sym);
void overview_handle_button(struct sway_seat *seat, uint32_t button,
		bool pressed);
void overview_handle_motion(struct sway_seat *seat);

#endif
