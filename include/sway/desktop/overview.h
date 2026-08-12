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

bool overview_is_active(void);
void overview_set_params(enum overview_scope scope,
		enum overview_action action);
void overview_toggle(void);
bool overview_handle_key(xkb_keysym_t sym);

#endif
