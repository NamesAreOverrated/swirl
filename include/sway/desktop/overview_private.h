#ifndef _SWAY_OVERVIEW_PRIVATE_H
#define _SWAY_OVERVIEW_PRIVATE_H

#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_scene.h>
#include "sway/desktop/overview.h"

struct sway_container;
struct sway_output;
struct sway_seat;
struct sway_workspace;

// One mounted thumbnail: a scene buffer holding the rendered snapshot plus an
// optional digit badge. Created (and destroyed) while the overview is shown.
// Lifecycle is owned by overview.c (teardown); rendering happens in
// overview_thumbnail.c and actions in overview_actions.c.
struct overview_thumbnail {
  struct wl_list link;
  struct wlr_scene_buffer *sb;
  struct wlr_scene_buffer *badge_sb;
  struct sway_container *con;
  struct sway_workspace *ws;
  enum overview_action action;
  int w, h;
};

struct overview_divider {
  struct wlr_scene_rect *rect;
  struct wl_list link;
};

// Shared overview render/input/hit-test state. Set up every toggle by
// overview_toggle; consumed by the thumbnail renderers, the action handlers
// and the input handlers.
struct overview_state {
  struct wlr_scene_rect *bg;
  struct wlr_scene_rect *hover_rect;
  struct wl_list thumbnails;
  struct wl_list dividers;
  int n_thumbnails;
  int digit_buf;
  int digit_count;
  struct sway_container *focus_con;
  struct sway_seat *seat;
  enum overview_scope scope;
  enum overview_action action;
  int content;
};

extern struct overview_state overview_state;

// Render-side collection: snapshot every eligible container for a workspace
// (or every minimized window across all workspaces) into thumbnails mounted
// on the overlay layer. `con_idx` carries the running digit counter.
void overview_collect_workspace(struct sway_workspace *ws,
        struct sway_output *output, struct sway_workspace *active_ws,
        struct wlr_renderer *renderer, struct wlr_allocator *alloc,
        const struct wlr_drm_format *fmt, float scale, int bt, int *con_idx,
        int content);
void overview_collect_minimized(struct sway_output *output,
        struct wlr_renderer *renderer, struct wlr_allocator *alloc,
        const struct wlr_drm_format *fmt, float scale, int bt, int *con_idx);

// Action side: run the stored action for a picked thumbnail. Used by the
// button and digit-key handlers in overview.c.
void overview_dispatch_action(struct overview_thumbnail *t);

#endif