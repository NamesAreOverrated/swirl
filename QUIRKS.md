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

When a column is removed (kill/pop), freed width is redistributed in 3 steps:

1. **Left visible neighbor** up to `default_w`
2. **Visible right neighbors** (including focus column) up to `default_w`
3. **Slider** (first off-screen column at or after removed index) gets the rest.
   If slider doesn't exist or `remaining < min_w`, remaining dumps to smallest
   visible column.

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

## 6. Freed Space = `col_w` (No `+ gaps_inner`)

In `column_remove` and `cmd_column_pop`, the freed width is `col_w` without
adding `gaps_inner`. This was a fix for 8px drift on kill/pop — the gap
is positional overlay, not part of the column's reclaimable space.

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
