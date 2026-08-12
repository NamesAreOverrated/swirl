#ifndef SWAY_XWAYLAND_H
#define SWAY_XWAYLAND_H

#include <wlr/xwayland.h>
#include <xcb/xproto.h>

enum atom_name {
	NET_WM_WINDOW_TYPE_NORMAL,
	NET_WM_WINDOW_TYPE_DIALOG,
	NET_WM_WINDOW_TYPE_UTILITY,
	NET_WM_WINDOW_TYPE_TOOLBAR,
	NET_WM_WINDOW_TYPE_SPLASH,
	NET_WM_WINDOW_TYPE_MENU,
	NET_WM_WINDOW_TYPE_DROPDOWN_MENU,
	NET_WM_WINDOW_TYPE_POPUP_MENU,
	NET_WM_WINDOW_TYPE_TOOLTIP,
	NET_WM_WINDOW_TYPE_NOTIFICATION,
	NET_WM_STATE_MODAL,
	ATOM_LAST,
};

struct sway_xwayland {
	struct wlr_xwayland *wlr_xwayland;
	struct wlr_xcursor_manager *xcursor_manager;

	xcb_atom_t atoms[ATOM_LAST];
};

void handle_xwayland_ready(struct wl_listener *listener, void *data);

struct sway_view;
// Re-sync an XWayland view's configured geometry (content_x/content_y and the
// underlying xsurface->x/y) with its live rendered scene position. Needed
// because XWayland reconstructs pointer coordinates from that geometry, which
// otherwise goes stale while the surface is being animated/resized.
void xwayland_update_geometry(struct sway_view *view);
// Animation-completion callback: re-sync every XWayland view in a column once
// its slide settles, so the cached geometry matches the final position.
void xwayland_sync_column_geometry_done(void *data);

#endif
