# Quirks & Known Weirdness

Documenting deliberate hacks, fragile interactions, and recurring landmines
so future-us doesn't waste days re-learning them.

---

## 1. Height Fraction Recalculation in `viewport_arrange_windows`

`viewport_arrange_windows` recalculates each child's `pending.height` from
`height_fraction` before positioning. This was **added and removed 3 times**:

| Commit | Action | Why |
|--------|--------|-----|
| `396c982d` | Removed | "preserve pixel heights instead of redistributing via fractions (columns scroll naturally)" |
| `2ec1cb8f` | Re-added | "prevents 1-column overflow when sum(fractions)=1.0, fixing focus-scroll jitter" |
| `c5694771` | Removed | "Position only, no size recomputation from fractions" — column_release sizing broke |
| `5889c938` | Re-added | "recalculate child heights so children resize when workspace height changes (e.g. layer shell)" |
| `95fcb77f` | Removed | Right-click vertical resize — fraction recalc would reset drag-adjusted heights |
| *(current)* | Re-added | Fullscreen → unfullscreen leaves `pending.height = output->height`; fraction recalc resets it |

**Landmines:**
- Vertical resize (`tiled_resize_vertical_px`) adjusts `height_fraction` and
  `pending.height` directly — arrange firing afterward resets heights from
  fractions, potentially undoing the resize.
- `column_scroll_vert_to` can fight with fraction recalc if `height_fraction`
  doesn't sum to 1.0 across children, causing scroll jitter.

---

## 2. `container_fullscreen_disable` Only Restores `saved_*` for Floating

`container_fullscreen_workspace` saves `saved_x/y/width/height` for ALL
containers, but `container_fullscreen_disable` only restores them for floating:

```c
if (container_is_floating(con)) {
    con->pending.x = con->saved_x;
    // ...
}
```

Tiled containers' `pending.height` stays at `output->height` after unfullscreen.
The height-fraction loop in `viewport_arrange_windows` (see #1) works around
this by recalculating from fractions, but if that loop is removed, unfullscreen
will overflow.

A proper fix would be to unconditionally restore `saved_width/height` for
all containers, but that hasn't been tested.

---

## 3. `workspace_arrange_columns` Preserves `col->pending.width`

Unlike upstream `apply_horiz_layout` which recalculates every child's width
from `width_fraction`, `workspace_arrange_columns` only sets x/y/height and
preserves whatever `pending.width` the column already has.

This was deliberate (`c5694771`): "Position only, no size recomputation from
fractions. Existing pending.width/height preserved."

It means column widths are set by growth/absorption/pop operations via
`viewport_grow_to_fill`, resize via `tiled_resize_horizontal_px`, and new-window
fitting — never by the arrange pass itself.

---

## 4. `viewport_grow_to_fill` 3-Step Distribution

When a column is removed (kill/pop), freed width (`col_w`) gets `+ ws->gaps_inner`
on entry (line 442) to account for the gap that collapses when two gaps become
one. Then redistributed in 3 steps:

1. **Left visible neighbor** up to `default_w`
2. **Visible right neighbors** (including focus column) up to `default_w`
3. **Slider** (first off-screen column at or after removed index) gets
   `remaining - ws->gaps_inner` (line 532) — the gap is subtracted back because
   the slider's positional gap is already implicit. If slider doesn't exist or
   `remaining < min_w`, remaining dumps to smallest visible column.

The `slider_resized` flag determines focus return: if slider was actually
resized, focus stays at `col_idx`; otherwise falls back to `col_idx - 1`.

This evolved organically through multiple rewrites and `focused_column_idx`
must be non-stale when this runs.

---

## 5. `focused_column_idx` Must Always Be Fresh

`ws->focused_column_idx` is used for O(1) column lookup in resize absorption
and new-window fitting. If stale, it can cause OOB reads in
`tiled_resize_horizontal_px`.

`seat_set_focus_raw` unconditionally calls `workspace_update_focused_column_idx`
at the end. This was patched in `7ea0b6e3` to handle the case where
`seat_set_workspace_focus` early-returns on `last_focus == node`.

The old workspace's `focused_column_idx` is reset to -1 on focus switch
(`seat.c:1254`).

---

## 6. Freed Space Adds `+ gaps_inner` Then Gap Is Subtracted Back

Callers (`column_remove`, `cmd_pop`) pass `col_w` (no gap). Both
`viewport_grow_to_fill:442` and `workspace_even_freed:1599` add
`+ ws->gaps_inner` to account for the collapsed gap when two gaps become one
(middle-column removal). The slider path in `viewport_grow_to_fill` subtracts
it back (`- ws->gaps_inner` at line 532) since the slider's positional gap is
already implicit. `workspace_even_freed`'s `viewport_grow_evenly` distributes
the inflated freed_width evenly, so each visible column gets a share of the
collapsed gap — no subtraction needed there.

---

## 7. No `list_del` in `column_remove`

`column_remove` doesn't call `list_del` on the column — instead,
`container_reap_empty` → `container_detach` already removes the container
from `ws->tiling`. Calling `list_del` separately would double-remove.

---

## 8. Auto-Complementary Resize (Farthest-First Absorption)

`tiled_resize_horizontal_px` uses farthest-first absorption instead of
adjusting a direct sibling. The delta is taken from the most distant visible
column so the viewport stays full.

- Mouse edge-drag: adjacent sibling absorbs first
- Keyboard / mod+right-click: farthest visible column absorbs
- Capped so growth doesn't exceed viewport width

Vertical resize (`tiled_resize_vertical_px`) is deliberately simple — direct
sibling adjustment, no absorption. The asymmetry is by design.

---

## 9. Fraction Functions Subtract `gaps_inner`

All `layout.c` fraction helpers subtract `ws->gaps_inner` from the workspace
dimension:

```c
workspace_width_fraction(ws, f)  → (ws->width - ws->gaps_inner) * f
workspace_height_fraction(ws, f) → fraction >= 1.0 ? ws->height : (ws->height - ws->gaps_inner) * f
```

When `fraction >= 1.0`, height returns full `ws->height` (no gap subtraction)
so a single child fills the entire column. This is intentional — gaps are
positional overlay, not part of the fractional space.

---

## 10. Column Visibility 0.5px Epsilon

`viewport_column_is_visible` uses `vp - 0.5` / `vp_end + 0.5` bounds. The
0.5px epsilon avoids flicker when a column sits exactly at the viewport edge
and floating-point rounding toggles visibility each frame.

---

## 11. Close-Slide: Slider/Pool Column

When a column is killed, `viewport_grow_to_fill` looks for the first
off-screen column *at or after* the removed index. If found and
`remaining >= min_w`, it becomes a visible "slider" sliding into the freed
viewport space. The width is set to `remaining` (clamped to `min_w`).

This only slides in a column that already exists — it doesn't create new
slots from nothing.

---

## 12. `handle_seat_node_destroy` Focus Restoration

The seat-side destroy handler (`seat.c:234-325`) has special focus management
for the close-slide cycle:

- If destroyed node was scratchpad: `seat_set_focus(seat, NULL)` first to
  force workspace IPC event
- If next focus == current focus: uses `seat_send_focus` to avoid redundant
  events
- The "setting focus_inactive" branch restores the focus stack by setting
  focus_inactive to the new node, then restoring original focus on top via
  `seat_set_raw_focus`

This prevents premature viewport scroll that would otherwise happen during
the close-destroy cycle.

---

## 13. `sway_anim_alpha` Is a Stub

`sway_anim_alpha` in `animation.c:245` does nothing. The comment says
"requires offscreen rendering or custom shader". Opacity animation is
not implemented.

---

## 14. Fixed `candidates[32]` Array

`viewport_scan_visible` uses `int candidates[32]` as a fixed cap. If more
than 32 columns are visible simultaneously (extremely unlikely with a
typical viewport), absorption will silently miss the tail.

---

## 15. Viewport Scroll Animation

`column_scroll_vert_to` animates the `content_tree` node Y position with
`damping_ratio=1.0, stiffness=1200, epsilon=0.001` spring parameters.
This is non-upstream and runs via `sway_anim_move`, which queues per-node
animation entries.

---

## 16. `container_toplevel_ancestor` Everywhere

Since views are wrapped inside L_VERT column containers, every operation
that needs the column navigates up `pending.parent` via
`container_toplevel_ancestor`. Used in 23+ sites. If the parent chain is
ever deeper than expected (e.g. nested splits inside a column), this could
return the wrong container.

---

## 17. `workspace_even_freed` Fallback: Use `freed_width`, Assign Leftovers

The fallback (`workspace.c:1627-1655`) runs when `viewport_grow_evenly`
returns -1 (no visible columns after removal). Previously it iterated with
`remaining = ws->width` and never assigned freed width — it only subtracted
column widths and returned `start` unchanged. Fixed to:

- Use `remaining = freed_width` (not `ws->width`)
- When a column's `pending.width > remaining`, clip it and return its index
  so `cmd_pop`'s `insert_at = focus_idx + 1` places the popped column after it
- After the loop, assign any leftover (positive `remaining`) to the last
  iterated column (the first slider)

---

## 18. Floating→Tiling Insert: Left-Edge + Workspace-Relative

`container_set_floating` (tiling→floating, line 1035) finds the insert index
by comparing the floating window's `pending.x` against column positions.
Two bugs fixed:

- **Left-edge comparison**: Previously used right-edge (`c->pending.x +
  c->pending.width + gaps`), causing any window inside a full-width column's
  span to always match at `idx = 0`. Changed to compare against `c->pending.x`
  (left edge) so the window is inserted before the first column it's left of.
- **Workspace-relative**: Floating `pending.x` is output-absolute (`ws->x +
  offset`), while column positions in `workspace_arrange_columns` start at 0
  (workspace-relative). Now subtracts `ws->x` before comparison.

---

## 19. `workspace_swap_columns` Must Swap `pending.width`

`workspace_arrange_columns` (viewport.c:84) sets `col->pending.x` by
iterating and accumulating `col->pending.width + gaps`. It does NOT
recompute `pending.width` from `width_fraction`. So swapping only
`width_fraction` had no visible effect — columns kept their old pixel widths.
Both branches (same-ws, cross-ws) now also swap `pending.width`.

---

## 20. Swap Commit Ordering: Arrange Then Commit

`workspace_swap_columns` previously called `transaction_commit_dirty()` at the
end, but with stale `pending.width`/`pending.x` (only `width_fraction` was
swapped, not pixel geometry). Then `overview_action_swap` called
`arrange_workspace` after — which fixed geometry from the new fractions — but
no second commit followed. Fixed:

- `workspace_swap_columns` no longer commits (removed `transaction_commit_dirty`)
- `overview_action_swap` calls `transaction_commit_dirty()` after arrange
- `cmd_swap` already had its own commit after arrange — unaffected
- `cmd_swap` column path uses `seat_set_focus_raw` instead of
  `seat_set_focus_container` to avoid viewport scroll on swap

---

## 21. Focus NULL-Guards in `seat.c`

Three NULL-deref fixes in sway/input/seat.c:

- **`seat_send_unfocus`**: `seat_set_workspace_focus` with `node == NULL` called
  `seat_send_unfocus(last_focus, seat)` without checking `last_focus` — crashed
  when a rofi/foot overlay appeared before anything was focused.
- **`handle_seat_node_destroy`**: Focus restoration branch accessed `focus->type`
  without checking `focus` for NULL.
- **Auto-descent fallback**: When `seat_get_focus_inactive_view` returned NULL
  (no views in a container), the code fell through to access an empty children
  list. Now returns early.

---

## 22. `seat_set_workspace_focus` Auto-Descends from Non-Views

When `seat_set_workspace_focus` receives a non-view container (column, split),
it redirects focus via `seat_get_focus_inactive_view` to the most recently
focused view within that subtree. If no view is found, returns early (no focus
change). This prevents focus from landing on a column container, which has no
rendered representation and would leave the user with no visible focus indicator.

---

## 23. `cmd_release` Insert Position

`cmd_release` (column.c:102) inserts the new column at `cidx + 1` in the
siblings list (`con`'s position + 1). This places the split-off column
immediately to the right of the original column. Works correctly because the
original column is still in the tiling list at this point (it hasn't been
detached or removed).

---

## 24. Gap Double-Counting: `+ gaps_inner` + Slider Subtraction

When a column is destroyed (window close), `freed_width` gets `+ ws->gaps_inner`
(layout.c helpers, viewport.c:442). The slider (first off-screen column at or
after the removed index) would receive this inflated width, getting one extra
gap of space. Fixed at viewport.c:532 by subtracting `ws->gaps_inner` from the
slider's new width — the gap is already accounted for by the slider's
position in the workspace.
