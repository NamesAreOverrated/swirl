# Scroll Column/Viewport Port — Architecture Analysis

This document captures the full architectural analysis for porting the [scroll](https://github.com/scroll-org/scroll) fork's column-based tiling layout into standard sway, using **standard wlroots** (no custom scene callbacks, no animation).

## Reference Codebases

| Repo | Path |
|------|------|
| Scroll fork | `/home/ln/Projects/scroll` |
| Standard sway | `/home/ln/Projects/sway` |

---

## 1. Architecture Overview

### Standard sway's two-phase layout system

**Phase 1 — Calculation (`sway/tree/arrange.c`)**
- Called during event handling (view map, resize, focus change, etc.)
- Sets `container->pending.x/y/width/height` — the DESIRED layout
- Also sets `pending.width_fraction`, `height_fraction` for proportional sizing
- These `pending` values are later copied into transaction instructions

**Phase 2 — Application (`sway/desktop/transaction.c`)**
- Transaction copies `pending` → instruction → `current` state
- `arrange_output()` → `arrange_workspace_tiling()` → `arrange_children()`
- Positions scene nodes using `current.x/y/width/height`
- Also arranges title bars, borders, and enables/disables nodes

### Scene hierarchy (standard sway)

```
root->layers.tiling
  └── ws->layers.tiling  [positioned at gaps+area.x, gaps+area.y]
      └── container->scene_tree  [positioned at current.x, current.y]
          ├── title_bar.tree
          │   ├── border (4 rects)
          │   └── background (5 rects)
          ├── border.tree
          │   ├── border top/bottom/left/right rects  (view only)
          │   └── content_tree
          │       └── view->scene_tree  [or child containers]
          └── (other nodes)
```

### Scroll fork's column hierarchy

```
root->layers.tiling
  └── ws->layers.tiling  [positioned at gaps+area.x - viewport_x, gaps+area.y]
      └── column (container, no view, L_VERT layout, no decoration)
          └── scene_tree [positioned at col_x, col_y]
              ├── content_tree
              │   └── window (container, has view, full decoration)
              │       └── scene_tree [positioned at 0, y_off]
              │           ├── decoration.tree
              │           │   ├── title_bar.tree
              │           │   ├── decoration.full  (wlr_scene_decoration)
              │           │   ├── content_tree
              │           │   │   └── view->scene_tree
              │           │   └── output_handler
              │           └── jump.tree
              └── (more windows...)
      └── (more columns...)
```

---

## 2. Key Differences

| Aspect | Standard Sway | Scroll Fork |
|--------|--------------|-------------|
| **Tiling children** | Views or split containers | Columns only (L_VERT, no view) |
| **Window grouping** | Split containers (L_HORIZ/L_VERT) | Columns contain windows (vertical stack) |
| **Decoration** | Title bar + border on every container | Decoration only on leaf view containers |
| **Layout algorithm** | Proportional width_fraction / height_fraction | Column-based with resizable widths |
| **Viewport** | N/A | `ws->viewport_x` shifts the tiling layer node |
| **Transactions** | Full commit/apply cycle | `transaction_commit_dirty` is a no-op |
| **Scene positioning** | Transaction apply → `wlr_scene_node_set_position` | Direct calls in `workspace_arrange_columns` |
| **Damage tracking** | Correct (old + new position via standard wlroots) | Broken (new position only — custom wlroots) |

---

## 3. Column Architecture in Detail

### Container roles

| Role | Creation | `view` | `children` | `layout` | Has decoration? |
|------|----------|--------|------------|----------|-----------------|
| Column | `container_create(NULL)` | NULL | List of windows | `L_VERT` | No |
| Window | `container_create(view)` | Set | NULL | `L_NONE` | Yes (title bar, border) |

### Column width management

From scroll fork (`sway/tree/layout.c`):

```c
// Compute pixel width from default fraction
double workspace_get_new_column_width(struct sway_workspace *ws) {
    double default_fraction = layout_get_default_width(ws);
    double usable = ws->width - ws->current_gaps.left - ws->current_gaps.right;
    double sum = 0;
    for (each column in ws->tiling) {
        sum += column->width_fraction;
    }
    double remaining = 1.0 - sum;
    if (remaining >= default_fraction || remaining < 0.2) {
        return default_fraction * usable;
    }
    return remaining * usable;
}

// Set column width in pixels
void column_set_width_px(struct sway_container *col, double width_px) {
    col->pending.width = width_px;
    // Also update fraction
}

// Inline helpers (scroll fork include/sway/tree/column.h):
static inline void column_set_width_fraction(struct sway_container *col, double fraction);
static inline void window_set_height_px(struct sway_container *win, double height_px);
static inline void window_set_height_fraction(struct sway_container *win, double fraction);
```

### Window height within column

Windows within a column stack vertically. Each window's height is proportional:

```c
double workspace_height_fraction(struct sway_workspace *ws, double fraction) {
    return fraction * (ws->height - ws->current_gaps.top - ws->current_gaps.bottom);
}

// In view_map:
container->pending.height = workspace_height_fraction(ws, 1.0);
container->height_fraction = 1.0;
```

### Column layout algorithm (`workspace_arrange_columns`)

From scroll fork `sway/tree/viewport.c:79`:

```
workspace_arrange_columns(ws, start_index):
    x = gaps->left                               // relative to ws origin
    for each column in ws->tiling[start_index:]:
        reparent column->scene_tree → ws->layers.tiling
        col_x = x
        col_y = ws->y + gaps->top
        
        set column->col_x, col_y, pending.x, pending.y
        position column->scene_tree at (col_x, col_y)
        
        if column has children:
            viewport_arrange_windows(column)     // stack children vertically
        
        x += column->pending.width + 2 * gaps    // accumulate
```

### Window arrangement within column (`viewport_arrange_windows`)

From scroll fork `sway/tree/viewport.c:182`:

```
viewport_arrange_windows(column):
    y = 0
    for each window in column->children:
        set window->pending.x, pending.y, pending.width, pending.height
        position window->scene_tree at (0, y)    // relative to column
        y += window->pending.height + 2 * gaps
```

### Viewport centering (`viewport_compute_offset`)

From scroll fork `sway/tree/viewport.c:220`:

```
viewport_compute_offset(ws, active, area_width, area_height):
    if no active container: return
    
    center_x = active->col_x + active->pending.width / 2.0
    target_x = center_x - area_width / 2.0
    
    total_width = sum(column widths + gaps)
    max_x = max(0, total_width - area_width)
    target_x = clamp(target_x, 0, max_x)
    
    ws->viewport_y = 0
    ws->viewport_x = target_x                        // immediate positioning
```

(The scroll fork has spring animation logic here too — omitted for port.)

---

## 4. Port Strategy — Two-Phase Integration

### Phase 1 (`sway/tree/arrange.c`)

Replace the `arrange_children(ws->tiling, ...)` call in `arrange_workspace()`:

```c
void arrange_workspace(struct sway_workspace *workspace) {
    // ... existing workspace_box / gaps computation (unchanged) ...

    // REPLACE: arrange_children(ws->tiling, ws->layout, &workspace_box, ...)
    // WITH:
    workspace_arrange_columns(workspace, &workspace_box);

    // NEW: compute viewport offset
    struct sway_seat *seat = input_manager_current_seat();
    struct sway_container *focus = seat_get_focus_inactive_tiling(seat, workspace);
    // Climb to column (direct tiling child)
    while (focus && focus->pending.parent) {
        focus = focus->pending.parent;
    }
    if (focus && workspace_is_visible(workspace)) {
        viewport_compute_offset(workspace, focus,
            workspace_box.width, workspace_box.height);
    } else {
        workspace->viewport_x = 0;
        workspace->viewport_y = 0;
    }

    // ... arrange_floating (unchanged) ...
}
```

`workspace_arrange_columns()` sets:
- Each column's `pending.x/y/width/height` (workspace-relative positions, starting at 0)
- Each window's `pending.x/y/width/height` (column-relative positions)
- Uses `width_fraction` / `height_fraction` for proportional sizing

### Phase 2 (`sway/desktop/transaction.c`)

#### `arrange_workspace_tiling()` — REPLACED

```c
static void arrange_workspace_tiling(struct sway_workspace *ws,
        int width, int height) {
    for (int i = 0; i < ws->current.tiling->length; i++) {
        struct sway_container *col = ws->current.tiling->items[i];

        // Position column scene node relative to tiling layer
        wlr_scene_node_set_position(&col->scene_tree->node,
            col->current.x, col->current.y);
        wlr_scene_node_reparent(&col->scene_tree->node, ws->layers.tiling);
        wlr_scene_node_set_enabled(&col->scene_tree->node, true);

        // Existing arrange_container handles column's children (windows)
        // via arrange_children(L_VERT, ...) — no custom code needed
        if (col->current.children && col->current.children->length > 0) {
            arrange_container(col, col->current.width,
                col->current.height, false, ws->gaps_inner);
        }
    }
}
```

#### `arrange_output()` — Add viewport shift

```c
wlr_scene_node_set_position(&child->layers.tiling->node,
    gaps->left + area->x - child->current.viewport_x,   // NEW
    gaps->top + area->y - child->current.viewport_y);   // NEW
```

#### `copy_workspace_state()` — Copy viewport

```c
state->viewport_x = ws->viewport_x;
state->viewport_y = ws->viewport_y;
```

### Scene hierarchy (post-port)

```
root->layers.tiling
  └── ws->layers.tiling  [pos: gaps+area.x - viewport_x, gaps+area.y]
      └── column->scene_tree  [pos: current.x, current.y]
          └── column->content_tree
              └── window->scene_tree  [pos: 0, off — set by arrange_children(L_VERT)]
                  ├── window->title_bar.*
                  ├── window->border.*
                  └── window->content_tree
                      └── view->scene_tree
```

---

## 5. What Reuses Existing Code (transaction.c)

| Function | How it's used for columns |
|----------|--------------------------|
| `arrange_container(con, ...)` | Called on each column → detects non-view → calls `arrange_children(L_VERT, ...)` on column's children |
| `arrange_children(L_VERT, ...)` | Positions window scene trees at `(0, off)` within column's `content_tree` |
| `arrange_container(win, ...)` | Called on each window → positions title bar, border rects, reparents view scene tree |
| `disable_container(con)` | Handles disabling column/window during fullscreen or inactive workspace |
| `apply_container_state` | `memcpy` copies `pending` → `current` for windows and columns |
| `apply_workspace_state` | `memcpy` handles `viewport_x/y` via `sway_workspace_state` |
| `copy_container_state` | `memcpy` from `container->pending` (includes `width`, `height`, `width_fraction`) |

**Key insight**: Standard sway's `arrange_container` + `arrange_children` in transaction.c already handle arbitrary container hierarchies correctly. A column is just a non-view L_VERT container whose children happen to be view containers. No custom Phase 2 scene positioning code is needed beyond iterating columns.

---

## 6. What Does NOT Reuse

| Old sway code | Reason |
|--------------|--------|
| `arrange.c:apply_horiz_layout` | Replaced by `workspace_arrange_columns` |
| `arrange.c:apply_vert_layout` | Not applicable — window stacking uses L_VERT via columns |
| `arrange.c:arrange_children` for `ws->tiling` | Replaced — columns are not L_HORIZ/L_VERT/STACKED/TABBED |
| `transaction.c:arrange_workspace_tiling` (old) | Replaced — iterates columns instead of calling `arrange_children` on tiling |
| `workspace.c:workspace_add_tiling` `container_split` | Bypassed — columns and windows are added via direct `list_add` or `container_add_child` |

---

## 7. View Mapping Flow (`sway/tree/view.c`)

In `view_map()`, after `container_create(view)`:

```c
// Create column wrapper
struct sway_container *col = container_create(NULL);
col->pending.workspace = ws;
col->pending.layout = L_VERT;

// Add view container as child of column
container_add_child(col, view->container);

// Set initial sizes
column_set_width_px(col, workspace_get_new_column_width(ws));
col->width_fraction = ws->width > 0
    ? col->pending.width / (ws->width - ...)
    : layout_get_default_width(ws);

view->container->pending.height = workspace_height_fraction(ws, 1.0);
view->container->height_fraction = 1.0;

// Add column to workspace (bypassing workspace_add_tiling's container_split)
list_add(ws->tiling, col);
```

---

## 8. Container Changes

### `container_create(view == NULL)` (columns)

Standard sway's `container_create` always allocates:
- `title_bar.tree` (with 4 border rects + 5 background rects) — **11 scene nodes**
- `border.tree` with `content_tree` as child

For columns (and any non-view container), these are unused. The port should conditionally skip them:

```c
// Only for view containers:
c->title_bar.tree = alloc_scene_tree(c->scene_tree, &failed);
// ... 11 scene nodes ...

// For all containers:
c->border.tree = alloc_scene_tree(c->scene_tree, &failed);
c->content_tree = alloc_scene_tree(c->border.tree, &failed);

// Border rects only for view containers:
if (view) {
    c->border.top = alloc_rect_node(c->border.tree, &failed);
    // ...
}
```

For columns, `content_tree` should be a direct child of `scene_tree` instead of `border.tree`:

```c
if (view) {
    c->content_tree = alloc_scene_tree(c->border.tree, &failed);
} else {
    c->content_tree = alloc_scene_tree(c->scene_tree, &failed);
}
```

### No new container fields needed

- `col_x`/`col_y` → Use `pending.x`/`pending.y` (workspace-relative)
- `width_fraction` — already exists in `sway_container_state`
- `height_fraction` — already exists in `sway_container_state`
- `spring_x`/`spring_y` — deferred (animation)

---

## 9. Workspace Changes

### New fields

```c
struct sway_workspace_state {
    // ... existing fields ...
    double viewport_x, viewport_y;  // NEW
};

struct sway_workspace {
    // ... existing fields ...
    double viewport_x, viewport_y;  // NEW — written by arrange_workspace, read by copy_workspace_state
};
```

### `copy_workspace_state`

```c
state->viewport_x = ws->viewport_x;
state->viewport_y = ws->viewport_y;
```

### `apply_workspace_state`

Already handled via `memcpy(&ws->current, state, sizeof(struct sway_workspace_state))`.

---

## 10. Files to Create

| File | Contents |
|------|----------|
| `include/sway/tree/viewport.h` | Declares `workspace_arrange_columns`, `viewport_arrange_windows`, `viewport_compute_offset` |
| `sway/tree/viewport.c` | Implementation of the three functions above (no spring/animation code) |
| `include/sway/tree/column.h` | Inline helpers: `column_set_width_px`, `column_set_width_fraction`, `window_set_height_px`, `window_set_height_fraction` |
| `include/sway/tree/layout.h` (minimal) | `layout_get_default_width`, `workspace_width_fraction`, `workspace_height_fraction` declarations |
| `sway/tree/layout.c` (minimal) | `layout_get_default_width`, `workspace_width_fraction`, `workspace_height_fraction`, `workspace_get_new_column_width` |

## 11. Files to Modify

| File | Change |
|------|--------|
| `include/sway/tree/workspace.h` | Add `viewport_x, viewport_y` to `sway_workspace_state` and `sway_workspace` |
| `sway/tree/container.c` | `container_create`: skip `title_bar`, `border.tree` nodes for non-view; make `content_tree` direct child of `scene_tree` for non-view |
| `sway/tree/arrange.c` | `arrange_workspace`: replace `arrange_children` with `workspace_arrange_columns` + `viewport_compute_offset` |
| `sway/desktop/transaction.c` | `copy_workspace_state`: add `viewport_x/y`; `arrange_workspace_tiling`: iterate columns; `arrange_output`: shift tiling layer by `-viewport_x` |
| `sway/tree/view.c` | `view_map`: wrap in column, add column to `ws->tiling` directly |
| `sway/tree/workspace.c` | Initialize `viewport_x/y` in `workspace_create` |

---

## 12. Deferred (Not Ported Yet)

| Feature | Reason |
|---------|--------|
| Spring animation (`spring_x`, `spring_y`, `viewport_animating`) | Requires per-frame callback — deferred |
| Overview mode / scale (`ws->scale`, `layout_scale_set`) | Not needed for basic layout |
| Scroll gestures (`layout_scroll_begin/update/end`) | Input handling — deferred |
| Trails, selections, pins, toggle_size | Scroll-specific UX features |
| Custom `scene_setup_decoration` | Keep standard sway's title bar + border |
| CSD / shadow nodes | Keep standard sway's approach |
| `workspace_wrap_children` / `workspace_squash` | Column architecture doesn't use these |

---

## 13. Potential Pitfalls

1. **`container_split` in `workspace_add_tiling`**: Must avoid this when adding columns. Use `workspace_insert_tiling_direct` or `list_add` directly.

2. **`container_reap_empty`**: When a column loses all its windows, it must be reaped and removed from `ws->tiling`. The scroll fork's `container_reap_empty` handles this.

3. **Focus climbing**: `seat_get_focus_inactive_tiling` climbs to the top-level tiling child. In column architecture, this is the column. `copy_workspace_state` already does this via the parent climbing loop.

4. **Container movement between workspaces**: `layout_move_container_to_workspace` moves the entire column (or wraps a bare view in a new column). This needs to be ported or adapted.

5. **`current.x/y` vs `pending.x/y`**: Standard sway convention is that `pending.x/y` is output-absolute. For columns, we use workspace-relative coordinates (0-based from start of tiling area). This is fine as long as Phase 1 and Phase 2 agree on the convention, but may affect code that reads `pending.x/y` for other purposes (IPC, focus, etc.).

6. **Multiple windows per column**: When adding a window to an existing column, it gets positioned vertically. Standard sway's `arrange_children(L_VERT, ...)` in transaction.c reads `child->current.height` (set from Phase 1's `pending.height`). As long as `workspace_arrange_columns` → `viewport_arrange_windows` sets `pending.height` correctly, this works.

---

## 14. Transaction Data Flow Summary

```
Event (view_map, resize, focus_change)
  │
  ▼
arrange_workspace()                              [sway/tree/arrange.c]
  ├── workspace_arrange_columns()
  │     sets col->pending.{x,y,width,height}
  │     sets win->pending.{x,y,width,height}
  └── viewport_compute_offset()
        sets ws->viewport_x
  │
  ▼
transaction_commit()
  ├── copy_workspace_state()
  │     reads ws->viewport_x  →  instruction->workspace_state.viewport_x
  └── copy_container_state()
        memcpy col->pending → instruction->container_state
        memcpy win->pending → instruction->container_state
  │
  ▼
transaction_apply()
  ├── apply_workspace_state()
  │     memcpy → ws->current.viewport_x
  ├── apply_container_state()
  │     memcpy → col->current, win->current
  └── arrange_output()
        ├── ws->layers.tiling pos -= ws->current.viewport_x
        └── arrange_workspace_tiling()
              ├── col->scene_tree pos = col->current.{x,y}
              └── arrange_container(col)
                    └── arrange_children(L_VERT, win)
                          └── win->scene_tree pos = (0, y_off)
                                └── arrange_container(win)
                                      view->scene_tree → win->content_tree
```
