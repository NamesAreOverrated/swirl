# VFX + Animation Architecture

This document describes the actual implementation of border rendering, drop
shadows, rounded corners, and scene-graph animation in the wlroots GLES2 shader
pipeline, as of the current codebase.

## Design Philosophy

### VFX vs Animation — two separate concerns

| Concern | Nature | Owner | Lifetime |
|---------|--------|-------|----------|
| Rounded corners, borders, shadows | Static shader uniforms | `wlr_scene_node_vfx` (via `wlr_scene_node_set_vfx`) | Until explicitly changed |
| Position, opacity, scale transitions | Temporal tween | `wlr_scene_node_visual` + animation timer | Until tween completes |

VFX are **not** animations. A corner radius doesn't tween — it's a fixed property
of how the window looks, set once when the config or focus changes.

### The animation system lives in wlroots, not Sway

Sway previously had its own animation timer (`sway_anim_sync`) and per-node
state (`struct sway_anim`) in `sway/tree/animation.c`. That system was removed.
The new design lives entirely in wlroots:

1. **The renderer needs the data** — VFX uniforms and visual-size overrides are
   consumed by the render pass in `scene_entry_render`. Having the state in
   wlroots avoids cross-layer callbacks.

2. **Zero-cost when idle** — `wlr_scene_node::visual` is NULL when no animation
   is active. The render pass checks `if (node->visual)` before accumulating
   offsets, scales, and opacity from ancestor nodes — the fast path runs
   unchanged when no node in the subtree has a visual override.

3. **No special cases in Sway** — Sway calls `wlr_scene_animate_position()` or
   `wlr_scene_animate()` and the animation "just works". No per-animation
   bookkeeping in Sway code beyond the initial call.

### The animation system is timer-driven, not per-frame synced

The old `sway_anim` system required an explicit `sway_anim_sync()` call from
`arrange_output` after every transaction arrange pass. The new system is fully
self-contained:

```
on_anim_tick (wl_event_source_timer, 1ms init, 16ms thereafter)
  └── for each active animation:
        ├── compute interpolation value
        ├── if done: snap to final, remove animation, fire callback
        └── if running: apply interpolated state to node->x/y or node->visual
```

No per-frame sync call needed. The timer reschedules itself at 16ms intervals
while any animation remains active, and disables itself when idle.

### Override API — new functions alongside unchanged setters

wlroots' existing `wlr_scene_node_set_position()`, `wlr_scene_rect_set_size()`,
`wlr_scene_buffer_set_dest_size()`, etc. are **completely unchanged**.

Animation is started via explicit functions:
- `wlr_scene_animate()` — animates `node->visual` fields (scale, opacity, offset)
- `wlr_scene_animate_position()` — animates `node->x/y` directly via
  `wlr_scene_node_set_position` on each tick

Both functions support **retargeting**: if an animation of the same type
(position or visual) already exists on the same node, it is reused — the "from"
state is snapshotted from the current node state, the "to" state is updated,
and the timer resets. This prevents fighting animations when multiple
transactions arrive in quick succession.

---

## Data Structures

### Visual override (temporal, written by animation system)

```c
struct wlr_scene_node_visual {
    float x, y;            // position offset from node->x, node->y
    float width, height;   // 0 = use real node size (absolute override)
    float scale_x, scale_y; // 1.0 = normal (relative multiplier)
    float opacity;         // 1.0 = opaque
};
```

Set via `wlr_scene_node_set_visual(node, &vis)`, cleared via
`wlr_scene_node_clear_visual(node)`.

### VFX (static, per-node appearance)

```c
struct wlr_scene_node_vfx {
    float corner_radius[4];          // tl, tr, br, bl (0 = square)
    struct {
        float thickness[4];           // top, right, bottom, left (0 = no border)
        float color[4];              // premultiplied RGBA
    } border;
    struct {
        float blur_sigma;            // 0 = no shadow
        float opacity;               // 0.0 – 1.0
        float color[4];              // premultiplied RGBA
    } shadow;
};
```

Set via `wlr_scene_node_set_vfx(node, &vfx)`.

### Animation types

```c
enum wlr_easing {
    WLR_EASING_LINEAR,
    WLR_EASING_EASE_OUT_CUBIC,
    WLR_EASING_SPRING,
};

struct wlr_scene_anim_spec {
    enum wlr_easing easing;
    double duration_ms;         // ease, linear only
    double damping_ratio;       // spring only (0.1–10.0)
    double stiffness;           // spring only
    double epsilon;             // spring settling threshold
};

struct wlr_scene_animation {
    struct wl_list link;                     // wlr_scene_animator.animations
    struct wlr_scene_node *node;
    bool position;                           // true = animating x/y, false = visual
    struct wlr_scene_anim_spec spec;
    union {
        struct { struct wlr_scene_node_visual from, to; };  // visual case
        struct { double pos_from_x, pos_from_y, pos_to_x, pos_to_y; }; // position case
    };
    struct timespec start_time;
    struct wl_listener node_destroy;
    void (*done)(void *data);
    void *done_data;
};

struct wlr_scene_animator {
    struct wl_list animations;
    struct wl_event_source *timer;
    struct wl_signal request_frame;
};
```

### Key design decisions

**Why separate `vfx` and `visual`?**
- `vfx` is set once per config/focus change — the compositor writes it, the
  renderer reads it every frame. It never tweens.
- `visual` is written by the animation system every tick. Separating the two
  means VFX updates don't route through the animation system at all.

**Why a pointer (`*vfx`, `*visual`) instead of embedding?**
- Zero-cost when not in use: every non-animated, non-VFX node has NULL for both.
  The wlroots scene tree has thousands of nodes that will never use VFX or
  animation.
- Embedding would add ~140 bytes per node regardless of need.

**Why position and visual in one struct instead of per-property?**
- Simpler API: one animation entry per node per type (position or visual),
  rather than one per property (x, y, scale_x, scale_y, opacity, offset).
- All visual fields are applied atomically in a single `wlr_scene_node_set_visual`
  call per tick.

---

## VFX Rendering

### VFX node type

`WLR_SCENE_NODE_VFX` is a scene node that draws border decorations (outline,
drop shadow, corner radius) in a single fragment shader pass.

```c
struct wlr_scene_vfx {
    struct wlr_scene_node node;
    int width, height;
};

struct wlr_scene_vfx *wlr_scene_vfx_create(
    struct wlr_scene_tree *parent, int width, int height);
void wlr_scene_vfx_set_size(struct wlr_scene_vfx *vfx, int w, int h);
```

### Scene tree structure (per container)

```
container->scene_tree
├── container->title_bar.tree
│     └── background rects, text — managed by sway
├── container->border.tree
│     ├── container->border.vfx      ← WLR_SCENE_NODE_VFX
│     │     shader draws:
│     │       1. Drop shadow (behind, blurred rounded rect)
│     │       2. Border (inset outline, corner-radius clipped)
│     │       → rest of the box is transparent (content shows through)
│     └── container->content_tree
│           └── view->scene_tree
│                 ├── view->saved_surface_tree (frozen buffer during resize)
│                 ├── view->output_handler (WLR_SCENE_NODE_BUFFER)
│                 └── view->content_tree
│                       └── xdg_surface tree (wlr_scene_buffer)
```

The VFX node replaces the old approach of four individual border rects
(`border.{top,bottom,left,right}`). The border is drawn by the fragment shader
as an inset rounded stroke. Shadow is rendered as a Gaussian-blurred rounded
rect behind the border.

### Fragment shader (vfx.frag)

The VFX shader receives uniforms for corner radius, border thickness/color, and
shadow parameters. It computes:
1. Shadow — expanded rounded rect with Gaussian blur convolution
2. Border — outer rounded rect minus inner rounded rect, drawn at given thickness

The center of the VFX node is transparent (zero alpha), allowing the view
content (rendered behind it in the scene tree z-order) to show through.

### Opaque region

A VFX node with active border or shadow reports zero opaque area — the rounded
corners and inner cutout make the entire node potentially transparent. Nodes
behind a VFX node should not be culled.

---

## Visual Override Rendering

When any ancestor node has a non-NULL `visual` pointer, `scene_entry_render()`
walks up the parent chain and accumulates:

```c
struct wlr_scene_node *p = node;
while (p) {
    if (p->visual) {
        if (p->visual->width > 0) {
            dst_box.width = p->visual->width;     // absolute override
        }
        if (p->visual->height > 0) {
            dst_box.height = p->visual->height;    // absolute override
        }
        if (p->visual->scale_x > 0) {
            vis_scale_x *= p->visual->scale_x;     // accumulated scale
        }
        if (p->visual->scale_y > 0) {
            vis_scale_y *= p->visual->scale_y;
        }
        vis_off_x += p->visual->x;                 // accumulated offset
        vis_off_y += p->visual->y;
        dst_box.x += (int)p->visual->x;
        dst_box.y += (int)p->visual->y;
        vis_alpha *= p->visual->opacity;            // accumulated opacity
    }
    p = p->parent ? &p->parent->node : NULL;
}
```

After accumulation, if the total scale differs from 1.0, auto-centering is
applied:

```c
if (vis_scale_x != 1.0f || vis_scale_y != 1.0f) {
    int sw = (int)(dst_box.width * vis_scale_x);
    int sh = (int)(dst_box.height * vis_scale_y);
    dst_box.x += (dst_box.width - sw) / 2;  // center-shrink
    dst_box.y += (dst_box.height - sh) / 2;
    dst_box.width = sw > 0 ? sw : 1;
    dst_box.height = sh > 0 ? sh : 1;
}
```

Opacity is multiplied into each entry type's color alpha:
- Rects: `scene_rect->color[3] * vis_alpha`
- Single-pixel buffers: `color_alpha * buffer_opacity * vis_alpha`
- Texture buffers: `buffer_opacity * vis_alpha`
- VFX border: `border.color[3] * vis_alpha`
- VFX shadow: `shadow.opacity * vis_alpha`

The VFX node's position and size are also adjusted by visual state:
- `x_rel += vis_off_x` (offset from accumulated visual.x/y)
- `vw *= vis_scale_x` (scale VFX width by accumulated scale)
- `vh *= vis_scale_y`
- `x_rel += (original_vw - vw) / 2` (center-shrink like dst_box)
- Shadow opacity: `shadow.opacity * vis_alpha`

---

## Animation System

### Timer tick

`on_anim_tick` runs from `wl_event_source_timer`:

```
on_anim_tick:
  for each animation:
    if prop_done(&anim->start_time, &anim->spec):
      if anim->position:
        snap node->x,y to pos_to_x,pos_to_y
      else:
        snap node->visual to anim->to
      fire anim->done callback if set
      remove + free animation
    else:
      compute interpolation value v
      if anim->position:
        node->x = lerp(pos_from_x, pos_to_x, v)
        node->y = lerp(pos_from_y, pos_to_y, v)
      else:
        per-field lerp of from→to for visual
        wlr_scene_node_set_visual(node, &interpolated)
  if any animation running:
    reschedule timer at 16ms
  else:
    disable timer
```

### Retargeting

Both `wlr_scene_animate()` and `wlr_scene_animate_position()` iterate the
animation list looking for a matching entry (`node == node && position flag
matches`). If found:
1. The "from" state is snapshotted from the current node state (current
   `node->x/y` for position, current `node->visual` for visual)
2. The "to" state is updated to the new target
3. The spec (easing, duration) is overwritten
4. The start time is reset to now
5. The timer is restarted at 1ms

If not found, a new animation entry is allocated and inserted.

### Opacity-only animation

Since `wlr_scene_animate()` animates all visual fields (scale, offset, opacity)
simultaneously, an opacity-only fade is achieved by setting `from` and `to`
with default values for fields that should not change:

```c
struct wlr_scene_node_visual from = { .opacity = 1.0f, .scale_x = 1.0f, .scale_y = 1.0f };
wlr_scene_node_set_visual(node, &from);

struct wlr_scene_node_visual to = { .opacity = 0.0f, .scale_x = 1.0f, .scale_y = 1.0f };
wlr_scene_animate(animator, node, &to, &spec, done_cb, done_data);
```

The `retarget` path snapshots the current `node->visual` as "from", so
subsequent calls naturally continue from wherever the current animation is.

### Node destruction

When a node with an active animation is destroyed, the `node_destroy` listener
fires, removes the animation from the list, and frees it. The done callback is
**not** invoked on node destruction — callers that need cleanup should listen
for the node's destroy event separately.

---

## Sway Integration

### What is currently animated

| Animation | Trigger | Mechanism | Duration | Easing |
|-----------|---------|-----------|----------|--------|
| **Open** (view_map) | View mapped | `wlr_scene_animate` on `container->scene_tree->node`: scale 0.88→1, opacity 0→1 | 150ms | ease_out_cubic |
| **Position** (transaction) | Container x,y changes ≥10px | `wlr_scene_animate_position` on `container->scene_tree->node` | Spring (k=1200, ζ=1.0, ε=0.001) | spring |
| **Tiling layer scroll** (arrange_output) | workspace viewport changes ≥10px | `wlr_scene_animate_position` on `child->layers.tiling->node` | Spring (same) | spring |
| **Column scroll** (column_scroll_vert_to) | User scrolls column | `wlr_scene_animate_position` on `col->content_tree->node` | Spring (same) | spring |

### What is NOT animated

| Operation | Behavior |
|-----------|----------|
| **Resize** (container width/height change) | Snaps immediately — no scale or visual animation |
| **Close** (container destruction) | Snaps immediately — no fade-out |
| **Opacity** | Not animated — `sway_anim_alpha` was a stub, no replacement |
| **Saved buffer** | Removed immediately after transaction — no crossfade |

### Position thresholds

Position animations only fire when the Manhattan distance `|dx| + |dy| ≥ 10px`.
Below this threshold, the node position snaps immediately without animation.
This prevents micro-wobbles from rounding or small layout adjustments.

### Size changes

Resize animation was attempted via two approaches and then removed:

1. **Visual scale** (`wlr_scene_animate` on scene_tree with scale_x/scale_y):
   The renderer's auto-centering shifts each child node differently based on
   its intrinsic size (background rects = new_size, saved buffer = old_size),
   causing visible misalignment between the animated parts.

2. **Crossfade** (`wlr_scene_animate` on saved_surface_tree with opacity 1→0):
   Required deferred cleanup of the saved buffer via a done callback, which
   introduced use-after-free crashes when the view was destroyed mid-animation.

Both were reverted. The saved buffer is now removed immediately after
transaction commit (original behavior).

### Open animation

When a view is mapped (`view_map` in `sway/tree/view.c`), a visual override
is applied to `container->scene_tree->node`:

```c
struct wlr_scene_node_visual init = {
    .scale_x = 0.88f, .scale_y = 0.88f,
    .opacity = 0.0f,
};
wlr_scene_node_set_visual(node, &init);

struct wlr_scene_node_visual to = {
    .scale_x = 1.0f, .scale_y = 1.0f,
    .opacity = 1.0f,
};
wlr_scene_animate(animator, node, &to, spec_150ms_ease_out_cubic, NULL, NULL);
```

The renderer's auto-centering handles the centering offset during the scale
animation. All descendant nodes (rects, buffers, VFX) inherit the accumulated
scale and opacity, creating a smooth zoom-in + fade-in effect.

Both the open animation and position animations can be active simultaneously
on different nodes without conflict (one is a visual animation on the
container's scene_tree, the other is a position animation on the same or
different node). Retargeting is type-aware: visual animations only retarget
other visual animations, and position animations only retarget other position
animations.

---

## Pixman fallback

The pixman renderer (software fallback) doesn't support custom fragment
shaders. VFX features (rounded corners, borders, shadows) use pixman region
operations:

| Feature | Pixman approach |
|---------|----------------|
| Corner radius | Render normally, then composite corner cutouts using `pixman_image_create_solid_fill()` + `pixman_image_composite32()` with a rounded mask |
| Borders | Render the outer rect, then punch out the inner rect using pixman region subtract |
| Shadow | Expand the rect by `blur_sigma`, render the shadow color with a pixman gaussian-blur convolution |
| Visual scale | Uses `pixman_image_set_transform()` with bilinear scale (exists in pixman pass) |
| Visual opacity | Already handled by the `mask` parameter in `pixman_image_composite32()` |

The pixman backend falls behind GLES2 quality-wise (no smoothstep for
anti-aliased corners), but this is the same limitation as any software
fallback.

---

## Opaque region and damage tracking

Rounded corners make parts of a node transparent that would otherwise be
opaque. For VFX nodes, the entire node reports zero opaque area because both
the inner cutout and rounded corners create transparency. This prevents
incorrect occlusion culling behind the VFX node.

Damage tracking itself (`scene_node_update()`) is unchanged — the entire node
bounding box is damaged when VFX or visual state changes.

---

## Legacy code removed

The old `sway/tree/animation.c` + `animation.h` (251 + 55 lines) was fully
deleted. It provided:

- `sway_anim_move()` — predecessor to `wlr_scene_animate_position()`
- `sway_anim_alpha()` — no-op stub for opacity animation
- `sway_anim_sync()` — per-frame sync call from `arrange_output`
- `sway_anim_init()` — initialization, replaced by `wlr_scene_animator_create()`

Call sites migrated:
- `transaction.c` `apply_container_state` → `wlr_scene_animate_position()`
- `transaction.c` `arrange_output` → `wlr_scene_animate_position()` on tiling layer
- `viewport.c` `column_scroll_vert_to` → `wlr_scene_animate_position()`
- `server.c` `sway_anim_init` → `wlr_scene_animator_create()`
- `resize.c` — include removed, no direct animation call

The new animation code lives in:
- `subprojects/wlroots/types/scene/animation.c` — engine + API
- `subprojects/wlroots/include/wlr/types/wlr_scene_animation.h` — header

---

## Quirks & Findings

### 1. Auto-centering causes misalignment under parent scale

When `visual.scale` is set on a tree node, the renderer applies auto-centering
to each descendant entry independently: `dst_box.x += (width - width*scale) / 2`.
Entries with different intrinsic sizes (e.g., background rect at new_size vs.
saved buffer at old_size) get different auto-centering shifts, causing visible
misalignment between siblings. This is why size animation via parent `visual.scale`
was removed.

### 2. `scene_node_opaque_region` has no VFX case

`scene_node_opaque_region()` needed a `WLR_SCENE_NODE_VFX` branch to report
zero opaque area. Without it, the VFX node's entire bounds were marked as fully
opaque, causing content behind it to be culled during visibility accumulation.

**Fix:** `} else if (node->type == WLR_SCENE_NODE_VFX) { return; }`

### 3. Blend mode forced to NONE when uniform `color->a == 1.0`

`render_pass_add_rect()` overrides blend mode: `color->a == 1.0` → NONE.
For a VFX border with opaque paint (alpha=1.0), the shader computes per-pixel
alpha (center is transparent). With NONE blend, `gl_FragColor = (0,0,0,0)`
overwrites the framebuffer with black instead of letting content show through.

**Fix:** When any border thickness is non-zero, force
`WLR_RENDER_BLEND_MODE_PREMULTIPLIED`.

### 4. `fwidth()` requires GL_OES_standard_derivatives on GLES2

GLES2 does not support `fwidth()` without explicit extension enable:
```glsl
#extension GL_OES_standard_derivatives : enable
```

### 5. wlroots as a standalone git repo inside sway

`subprojects/wlroots/` has its own `.git/` directory — it is not a git
submodule. Both repos need separate commits:
```bash
cd subprojects/wlroots && git commit -m "..."
cd ../.. && git commit -m "..."
```

### 6. HiDPI border/inner rect coordinate-space mismatch

The VFX shader's `pos` and `size` are in buffer coordinates (after scaling by
output scale), but `border_thickness` and `corner_radius` are in logical pixels.
On HiDPI (scale > 1), the inner rect appears smaller than the content area.

**Fix:** Multiply thickness and radius values by `scale` in `scene_entry_render`,
and use `roundf()` on each border edge offset to match the content's rounding
exactly, eliminating sub-pixel gaps:

```c
bt_left = roundf((x_rel + left_logical) * s) - roundf(x_rel * s);
bt_top  = roundf((y_rel + top_logical) * s) - roundf(y_rel * s);
bt_right = roundf((x_rel + vw) * s) - roundf((x_rel + vw - right_logical) * s);
bt_bottom = roundf((y_rel + vh) * s) - roundf((y_rel + vh - bottom_logical) * s);
```

### 7. `WL_OUTPUT_TRANSFORM_FLIPPED_180` projection matrix

The GLES2 render pass creates its projection matrix with
`WL_OUTPUT_TRANSFORM_FLIPPED_180`. This does not actually flip the Y axis —
`gl_FragCoord.xy` and the box coordinates are in the same orientation (Y = 0 at
top, increasing downward). No Y-coordinate adjustment is needed in shaders.

### 8. `scene_node_invisible` must be extended for every new VFX effect

`scene_node_invisible()` checks whether a VFX node has active border _or_
shadow. Adding a new effect (e.g., glow, inner shadow) requires extending this
check so the node is not skipped during render-list construction when the
effect is active.

### 9. `wlr_scene_animation_cancel` calls `wlr_scene_node_set_visual` for all animations

`wlr_scene_animation_cancel()` unconditionally calls
`wlr_scene_node_set_visual(anim->node, &anim->to)`, even for position
animations where `anim->to` is a visual struct that was never initialized
(zeroed from calloc). For position animations, this would reset the visual
state to all zeros (scale=0, opacity=0). This function is not currently used
by any caller.

### 10. `quad.frag` has no VFX-free fallback path

`wlr_render_pass_add_rect()` serves both plain `wlr_scene_rect` nodes and VFX
border/shadow nodes via the same `quad.frag` shader. The VFX uniforms (border
thickness, shadow, box) are only populated in the VFX branch of
`render_pass_add_rect()` — the `else` branch was setting `u_box` to
`(0,0,0,0)`, making `corner_alpha()` always return 0. Combined with the
blend-mode override (`color->a == 1.0` → `NONE`), non-VFX rects produced
transparent black instead of the requested color.

**Fix:** Added an `else` branch that sets `u_box` to the actual rect and
resets all VFX uniforms to 0. Replaced the blend-mode override with
`options->blend_mode` directly (callers already provide the correct mode).
`quad.frag` now early-returns after the VFX border path and uses a
`color * corner_alpha` fallback for plain rects.

### 11. VFX node position/size must extend into parent-managed title bar area

When a tabbed or stacked parent manages title bars, each child's VFX node
(border/shadow) must extend upward into the title bar area so corners and
shadows cover the title bar background. The child's scene tree is offset below
the title bars, but the VFX node needs to know how much space to extend into.

**Fix:** In `arrange_container`, when `title_bar` is false (parent-managed),
compute `title_ext` from the parent's layout:
`container_titlebar_height()` for tabbed, `N * container_titlebar_height()`
for stacked. The VFX node's position is shifted up by `title_ext` and its
height is increased by `title_ext`. `hide_lone_tab` is handled transparently
because it sets `title_bar = true` (self-managed), which skips the extension
entirely.

### 12. `container_titlebar_height()` returns fixed value, not hide_lone_tab-adjusted

`container_titlebar_height()` always returns the standard title bar height
regardless of `hide_lone_tab`. When computing VFX title extension from the
parent, using this value for a hide_lone_tab scenario would extend into
non-existent space. This is avoided by keying off the `title_bar` bool —
hide_lone_tab sets `title_bar_height = 0`, which makes
`title_bar_height == 0` evaluate to `true`, meaning the container is
self-managed and no extension is applied.
