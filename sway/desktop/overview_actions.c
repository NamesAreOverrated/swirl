#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdbool.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include "list.h"
#include "log.h"
#include "sway/desktop/overview.h"
#include "sway/desktop/overview_private.h"
#include "sway/desktop/transaction.h"
#include "sway/input/cursor.h"
#include "sway/input/seat.h"
#include "sway/output.h"
#include "sway/tree/arrange.h"
#include "sway/tree/container.h"
#include "sway/tree/root.h"
#include "sway/tree/workspace.h"

static void overview_action_focus(struct overview_thumbnail *t) {
  if (!t || !t->con)
    return;
  if (t->ws && t->ws != seat_get_focused_workspace(overview_state.seat)) {
    workspace_switch(t->ws);
  }
  // Resolve the target after any workspace switch so we never hold a
  // reference across a switch that may have reaped the old workspace.
  struct sway_container *target = container_toplevel_ancestor(t->con);
  seat_set_focus_container(overview_state.seat, target);
  transaction_commit_dirty();
}

// Core pull logic, shared by the overview and the `pull` command so both
// stay in sync (pointer-placement for floating, column pull for tiled). The
// `pull` command targets the focused workspace; the overview click path
// targets the workspace displayed under the overview (see
// overview_pull_container_to).
void overview_pull_container(struct sway_container *target,
        struct sway_seat *seat) {
  if (!target || !seat)
    return;
  struct sway_workspace *active_ws = seat_get_focused_workspace(seat);
  if (!active_ws)
    return;
  overview_pull_container_to(target, seat, active_ws);
}

void overview_pull_container_to(struct sway_container *target,
        struct sway_seat *seat, struct sway_workspace *dest_ws) {
  if (!target || !seat || !dest_ws)
    return;
  target = container_toplevel_ancestor(target);

  if (container_is_floating(target)) {
    if (container_is_scratchpad_hidden(target)) {
      root_scratchpad_show(target);
      transaction_commit_dirty();
      return;
    }
    if (target->pending.fullscreen_mode != FULLSCREEN_NONE) {
      container_set_fullscreen(target, FULLSCREEN_NONE);
    }
    struct sway_workspace *old_ws = target->pending.workspace;
    if (old_ws && old_ws != dest_ws) {
      struct sway_output *old_output = old_ws->output;
      container_detach(target);
      workspace_add_floating(dest_ws, target);
      if (old_output != dest_ws->output) {
        struct wlr_box old_box, new_box;
        workspace_get_box(old_ws, &old_box);
        workspace_get_box(dest_ws, &new_box);
        floating_fix_coordinates(target, &old_box, &new_box);
      }
      arrange_workspace(old_ws);
      arrange_workspace(dest_ws);
    }
    // Pull the floating window to the pointer position (centered, clamped).
    struct wlr_cursor *cursor = seat->cursor->cursor;
    double lx = cursor->x - target->pending.width / 2;
    double ly = cursor->y - target->pending.height / 2;
    struct wlr_output *output =
        wlr_output_layout_output_at(root->output_layout, cursor->x, cursor->y);
    if (output) {
      struct wlr_box box;
      wlr_output_layout_get_box(root->output_layout, output, &box);
      lx = fmax(lx, box.x);
      ly = fmax(ly, box.y);
      if (lx + target->pending.width > box.x + box.width)
        lx = box.x + box.width - target->pending.width;
      if (ly + target->pending.height > box.y + box.height)
        ly = box.y + box.height - target->pending.height;
    }
    container_floating_move_to(target, lx, ly);
    seat_set_focus_container(seat, target);
    container_raise_floating(target);
    transaction_commit_dirty();
    return;
  }

  struct sway_container *focus = seat_get_focused_container(seat);
  struct sway_container *target_col = container_toplevel_ancestor(target);
  if (focus) {
    struct sway_container *focus_col = container_toplevel_ancestor(focus);
    if (focus_col != target_col) {
      int fi = list_find(dest_ws->tiling, focus_col);
      if (fi >= 0) {
        workspace_pull_column(dest_ws, target_col, fi + 1);
      } else {
        // The focused container is not a tiling column of the destination
        // (e.g. wlroots focus is parked elsewhere while the overview covers
        // another workspace). Fall back to the focused column index,
        // otherwise append.
        int col = dest_ws->focused_column_idx;
        if (col < 0 || col > dest_ws->tiling->length) {
          col = dest_ws->tiling->length;
        }
        workspace_pull_column(dest_ws, target_col, col);
      }
    }
  }
  seat_set_focus_container(seat, target);
  transaction_commit_dirty();
}

static void overview_action_pull(struct overview_thumbnail *t) {
  if (!t || !t->con)
    return;
  struct sway_workspace *dest = overview_action_current_ws(overview_state.seat);
  if (!dest)
    return;
  overview_pull_container_to(t->con, overview_state.seat, dest);
}

// Core swap logic, shared by the overview and the `swap` command. Only swaps
// within the same type (tiled<->tiled, floating<->floating).
void overview_swap_container(struct sway_container *focus_top,
        struct sway_container *target, struct sway_seat *seat) {
  if (!focus_top || !target || focus_top == target)
    return;
  bool focus_float = container_is_floating(focus_top);
  bool target_float = container_is_floating(target);
  if (focus_float != target_float)
    return;
  if (focus_float) {
    workspace_swap_floating(focus_top, target);
  } else {
    workspace_swap_columns(focus_top, target);
  }
  seat_set_focus_raw(seat, &target->node);
  struct sway_workspace *ws_a = focus_top->pending.workspace;
  struct sway_workspace *ws_b = target->pending.workspace;
  arrange_workspace(ws_a);
  if (ws_b != ws_a) arrange_workspace(ws_b);
  transaction_commit_dirty();
}

static void overview_action_swap(struct overview_thumbnail *t) {
  if (!t || !t->con)
    return;
  struct sway_container *focus = overview_state.focus_con;
  if (!focus) {
    seat_set_focus_container(overview_state.seat,
        container_toplevel_ancestor(t->con));
    return;
  }
  overview_swap_container(container_toplevel_ancestor(focus),
      container_toplevel_ancestor(t->con), overview_state.seat);
}

static void overview_action_restore(struct overview_thumbnail *t) {
  if (!t->con)
    return;
  // Restore onto the workspace currently displayed under the overview (like
  // pull/focus/swap), falling back to the focused workspace.
  struct sway_workspace *dest = overview_action_current_ws(overview_state.seat);
  if (dest) {
    workspace_minimized_show_on(t->con, dest);
  } else {
    workspace_minimized_show(t->con);
  }
  transaction_commit_dirty();
}

void overview_dispatch_action(struct overview_thumbnail *t) {
  switch (t->action) {
    case OVERVIEW_FOCUS:
      overview_action_focus(t);
      break;
    case OVERVIEW_PULL:
      overview_action_pull(t);
      break;
    case OVERVIEW_SWAP:
      overview_action_swap(t);
      break;
    case OVERVIEW_RESTORE:
      overview_action_restore(t);
      break;
  }
}