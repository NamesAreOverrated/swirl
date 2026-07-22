# Swirl: Column Layout & Workspace Operations

## Philosophy

**Content comes to you, not you to it.**

The viewport is stable unless you explicitly move it. New content fits into the existing viewport where possible. Closing a window fills its slot from the pool without shifting existing columns. Manual resizing keeps all visible columns in view by adjusting the opposite side.

## Core Data

### On workspace (`sway_workspace`)
- `focused_column_idx: int` — index into `ws->tiling` for the focused column. Updated after every focus change in `seat_set_workspace_focus`. Enables O(1) lookups for leftmost, rightmost, and opposite columns.

### Config (`sway_config`)

```
default_column_width_fraction  0.5   (default)
min_column_width_fraction      0.2   (default)
```

`min_column_width_fraction` is the smallest a column can be after auto-sizing or auto-complementary resize. Manual resize can go below this.

## The Viewport Model

The workspace is a horizontal strip of columns. The viewport is a window into this strip. Columns can be:

- **In-view** — fully visible within the viewport
- **Pool** — scrolled off (right or left), invisible, accessible via goto or scale-and-pull

Edge-snap: after any focus change, if the focused column is out of view, the viewport snaps so it becomes fully visible. If focus is already in view, nothing happens. This is the only rule — checked after every focus change, regardless of what caused it.

## Operations

### Pop

Focused column → end of `ws->tiling` (pool tail). Remaining in-view columns **proportional-scale** to fill the viewport (relative size ratios preserved). Focus moves to the column that took the vacated slot (or leftmost if empty).

```
[A(500), B(300), C(400)] pop B
→ [A(550), C(550)], B(300)  (proportional fill)
```

### Swap

Via overview: pick target column by badge. Focused column exchanges position and width with target. No other columns resized. Focus moves to target.

```
[A(500), B(300), C(400)], D(500)  swap A ↔ D
→ [D(500), B(300), C(400)], A(500)
```

### Goto

Via overview (badge) or keybind. Edge-snap viewport so target column is visible. No layout change — columns keep their positions and widths.

### Scale and pull

Next pool column to the right of the focused column enters the working set. It inserts immediately right of the focused column. The opposite-side in-view column shrinks to make room.

Shrink priority: rightmost visible → leftmost visible → inward toward focus. If opposite is at min → next-opposite shrinks, etc. The new column takes the freed width. Focus stays on the focused column.

```
[A(400), B(300), C(300)], D(500)  focus on B, scale-and-pull
→ opposite of B is C. C shrinks from 300 to 150.
→ [A(400), B(300), D(500), C(150)]
```

If C is at min (200), check A next:

```
[A(400), B(300), C(200)], D(500)  focus on B, scale-and-pull
→ C at min, can't shrink. A shrinks from 400 to 200.
→ [A(200), B(300), D(500), C(200)]
```

### Scale to fit

Focused column expands to fill the remaining viewport width to its right. All columns to its right are pushed out of the viewport into the pool. Leftward columns keep their widths.

```
[A(200), B(300), C(300)]  focus on A, scale-to-fit
→ [A(700)], B(300), C(300)
```

### New window

New column created, width computed as:
1. Compute remaining visible space to right of rightmost in-view column
2. If remaining ≥ min → new column takes `min(max(remaining, min_fraction), default_fraction)`
3. If remaining < min → fallback to scale-and-pull behavior (shrink opposite, insert at min)

New column inserted after the focused column. View becomes wrapped in L_VERT column.

### Close window (last window in column)

Column closes. Remaining in-view columns shift left to close the gap. The first pool column slides in from the **rightmost position**, taking the closed column's width. If no pool column exists, the viewport has empty space on the right. Focus moves to the column that filled the gap, or leftmost remaining.

```
[A(400), B(300), C(300)], D(500)  close last window in B
→ column B removed. C shifts left by 300. D slides in from right at width 300.
→ [A(400), C(300), D(300)]
```

## Continuous Behaviors

### Auto-complementary resize

When the user manually changes a column's width, the farthest fully-visible column absorbs the delta so the viewport stays full. Only **fully visible** columns participate — partially visible columns are invisible (their space counts as free). Tiebreaker: if two columns are equidistant from focus, the larger index absorbs first.

#### Growing (delta > 0)

1. Sum widths of all fully-visible columns
2. If `sum + delta ≤ viewport_width` → grow without absorption (free space exists)
3. If not enough free space → find farthest fully-visible column. It shrinks by `delta - free` (capped at min_width). If still not enough, next farthest, etc.

#### Shrinking (delta < 0)

1. The farthest fully-visible column grows by `|delta|`
2. If capped at min_width, next farthest absorbs the remainder

#### Examples

```
[A(500), B(300), C(200)]  focus=1, grow B 300→500 (+200)
→ Fully visible: A, B, C. Sum = 1000. No free space.
→ Farthest from B: A (idx 0, distance 1), then C (idx 2, distance 1).
→ Tiebreaker: C first. C(200) − 200 = 0 < min → skipped.
→ A(500) − 200 = 300 ≥ min → A shrinks to 300.
→ [A(300), B(500), C(200)]
```

```
[A(500), B(300), C(200)]  focus=1, shrink B 300→150 (-150)
→ Farthest from B: C (idx 2, distance 1), then A (idx 0, distance 1).
→ Tiebreaker: C first. C(200) + 150 = 350. Done.
→ [A(500), B(150), C(350)]
```

```
[1(0.5 partially), 2(0.3), 3(0.5)]  focus=2, grow 3 0.5→0.6 (+0.1)
→ Fully visible: 2, 3. Sum = 0.8. Free = 0.2.
→ d(0.1) ≤ free(0.2) → no absorption. Just grow.
→ [1(0.5 partially), 2(0.3), 3(0.6)]
```

This replaces the current sibling-based resize in `tiled_resize_horizontal_px`.

### Vertical scroll

Per-column `scroll_y`, unchanged from current implementation. Horizontal viewport is unaffected by vertical scrolling.

## Overview

### Display
- All columns shown as scaled thumbnails in a grid
- Thumbnails reference existing `wlr_scene_buffer` nodes + `wlr_scene_buffer_set_dest_size()` — no snapshot render passes
- Badges overlaid via Cairo (unchanged)
- Semi-transparent background dims the scene behind

### Interaction
- Type two-digit badge number to select target
- Operations from overview: **swap**, **goto**
- Escape dismisses without action

### Performance target
- Instant open/close regardless of window count
- No blocking render passes — the scene graph already has the textures

## Animation

### Approach
- **Compute-before-animate**: all positions, sizes, and widths are finalized in the pending state before any visual change begins
- One smooth spring animation per operation (view does not show intermediate layout states)

### Spring config
- Fast, subtle
- `damping_ratio = 1.0`, `stiffness = 1200`, `epsilon = 0.001` (current defaults)

### Properties animated
- Column position (x) — columns slide to their new positions
- Column width — columns expand/contract smoothly
- Viewport offset — edge-snap scrolls smoothly to align columns

## Implementation Order

### Phase 1: Infrastructure
1. `focused_column_idx` on workspace, set on focus-change events
2. Config: `default_column_width_fraction`, `min_column_width_fraction`
3. Auto-complementary resize in `tiled_resize_horizontal_px`

### Phase 2: Layout operations
4. Pop — `workspace_pop_focused(ws)`
5. Scale to fit — `workspace_scale_to_fit(ws)`
6. Scale and pull — `workspace_scale_and_pull(ws)`

### Phase 3: Selection & navigation
7. Overview refactor — buffer references, not snapshots
8. Swap — `workspace_swap_columns(focused, target)`
9. Goto — edge-snap viewport to target column

### Phase 4: Behavioral
10. Close-window slide-in — detect last-window-in-column, trigger pool column insertion
11. New-window sizing — [min, default] fitting with scale-and-pull fallback
