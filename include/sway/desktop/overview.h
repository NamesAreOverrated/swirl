#ifndef _SWAY_OVERVIEW_H
#define _SWAY_OVERVIEW_H

#include <stdbool.h>
#include <xkbcommon/xkbcommon.h>

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

void overview_set_params(enum overview_scope scope,
		enum overview_action action, int content_flags);
void overview_toggle(void);
bool overview_handle_key(xkb_keysym_t sym);
void overview_handle_button(struct sway_seat *seat, uint32_t button,
		bool pressed);
void overview_handle_motion(struct sway_seat *seat);

#endif
