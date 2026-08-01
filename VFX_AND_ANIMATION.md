# VFX + Animation Architecture

This document describes the actual implementation of border rendering, drop
shadows, rounded corners, and scene-graph animation in the wlroots scene graph
and the GLES2 renderer, as of the current codebase.

VFX (corner radius, border, shadow) is modeled as a `wlr_scene_node_vfx` payload
attached to a plain `wlr_scene_rect` or `wlr_scene_buffer` node via
`node->vfx` — there is no dedicated VFX scene-node type anymore (the
`WLR_SCENE_NODE_VFX` type from earlier versions was folded into rects/buffers).
The GLES2 renderer draws it in a single SDF fragment pass; the Vulkan and pixman
renderers do not support it yet (see "Capability gate").

## Design Philosophy

### VFX vs Animation — two separate concerns

| Concern | Nature | Owner | Lifetime |
|---------|--------|-------|----------|
| Rounded corners, borders, shadows | Static per-node state | `wlr_scene_node_vfx` (via `wlr_scene_node_set_vfx`), a pointer on `wlr_scene_node` | Until explicitly changed |
| Position, opacity, scale transitions | Temporal tween | `wlr_scene_node_visual` + animation timer | Until tween completes |

VFX are **not** animations. A corner radius doesn't tween — it's a fixed property
of how the window looks, set once when the config or focus changes.

### The animation system lives in wlroots, not Sway

Sway previously had its own animation timer (`sway_anim_sync`) and per-node
state (`struct sway_anim`) in `sway/tree/animation.c`. That system was removed.
The new design lives entirely in wlroots:

1. **The renderer needs the data** — VFX state and visual-size overrides are
   consumed by the render pass in `scene_entry_render`. Having the state in
   wlroots avoids cross-layer callbacks.

2. **Zero-cost when idle** — `wlr_scene_node::visual` is NULL when no animation
   is active. The render pass checks `if (node->visual)` before accumulating
   offsets, scales, and opacity from ancestor nodes — the fast path runs
   unchanged when no node in the subtree has a visual override. The same holds
   for `node->vfx`: it is NULL on the thousands of nodes that never use VFX.

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

Set via `wlr_scene_node_set_vfx(node, &vfx)`. The struct is stored behind the
`node->vfx` pointer (`NULL` = no VFX), which lives on every `wlr_scene_node` —
so **any** node type can carry VFX state, but only rect and buffer nodes
consume it at render time.

A VFX payload is considered **active** when `scene_node_vfx_active(node)`
returns true:

```c
border.thickness[i] > 0 (any i)                  // border
|| (shadow.blur_sigma > 0 && shadow.opacity > 0  // shadow
    && shadow.color[3] > 0)
```

A payload with only `corner_radius` set is *not* "active" by this test.

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

### VFX on scene nodes

There is no dedicated VFX node type. A VFX rect is a plain `wlr_scene_rect`
whose `node->vfx` pointer holds the border/shadow/radius state:

```c
struct wlr_scene_rect *rect = wlr_scene_rect_create(parent, 0, 0, transparent);

struct wlr_scene_node_vfx vfx = {0};
vfx.border.thickness[0] = vfx.border.thickness[1] = 2; // top, right
vfx.border.thickness[2] = vfx.border.thickness[3] = 2; // bottom, left
memcpy(vfx.border.color, (float[]){ 0.5, 0.5, 0.5, 1.0 }, sizeof(vfx.border.color));
vfx.shadow.blur_sigma = 10.0f;
vfx.shadow.opacity = 0.8f;
vfx.shadow.color[3] = 1.0f;
wlr_scene_node_set_vfx(&rect->node, &vfx);
```

The rect's **bounds include the shadow expansion**. Sway sizes the node to
`width + 2*ext` by `height + 2*ext` and positions it at `-ext, -ext`, where
`ext = wlr_scene_vfx_shadow_extension(blur_sigma) = blur_sigma * 3.0f`. The
shader insets back by `ext` to find the container rect. This keeps the scene
node's bounding box large enough for both damage tracking and the shadow bleed.

Sway helpers that set VFX state live in `sway/tree/container.c`:
- `container_set_border` — builds the `vfx` struct and calls
  `wlr_scene_node_set_vfx` (container.c:292)
- `container_set_geometry` — positions/sizes the rect around the container
  (container.c:321-323)
- `wlr_scene_rect_set_corner_radius` / `wlr_scene_buffer_set_corner_radius`
  allocate `node->vfx` lazily when only rounding is needed (wlr_scene.c:826, 840)

### Scene tree structure (per container)

```
container->scene_tree
├── container->title_bar.tree
│     └── background rects, text — managed by sway
├── container->border.tree
│     ├── container->border.vfx      ← wlr_scene_rect carrying node->vfx
│     │     bounds = container size + 2*ext (shadow expansion)
│     │     shader draws (inset by ext):
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

The VFX rect replaces the old approach of four individual border rects
(`border.{top,bottom,left,right}`). The border is drawn by the fragment shader
as an inset rounded stroke. Shadow is rendered as a Gaussian-blurred rounded
rect behind the border.

### Fragment shader (quad.frag)

The GLES2 shader (`render/gles2/shaders/quad.frag`) receives a rect box,
corner radius, border thickness, and shadow parameters. It computes:

1. `corner_sdf()` — an SDF for a rounded rect, and `corner_alpha()` — its
   `smoothstep(fwidth())` anti-aliased alpha.
2. **Shadow** — `exp(-d²/2σ²)` Gaussian falloff of the rounded-rect SDF,
   clipped out of the inner (border-inset) rect, multiplied by
   `shadow.opacity * shadow_color.a`.
3. **Border rim** — outer rounded rect minus inner rounded rect, drawn at the
   per-edge thickness.
4. **Plain fallback** — `color * corner_alpha` when no border and no shadow.

The center of the VFX rect is transparent (zero alpha), allowing the view
content (rendered behind it in the scene tree z-order) to show through.

The border color, shadow color, and opacity are premultiplied; GLES2 passes
them through in sRGB space and the framebuffer is treated as sRGB, so no
conversion is done in the GLES2 shader.

### Capability gate (renderer support)

VFX support is gated by `renderer->features.vfx` (`include/wlr/render/wlr_renderer.h`).
Only the GLES2 renderer sets it (`render/gles2/renderer.c`). On renderers where
it is unset (Vulkan, pixman):

- `scene_entry_render` **skips** rect/buffer nodes with an *active* VFX payload
  entirely — nothing is drawn (no garbage, but no border/shadow either).
- `corner_radius` on plain rects, single-pixel buffers, and textures is ignored
  (the `node->vfx && features.vfx` guard in `scene_entry_render`).

This is the documented fallback behavior today; the Vulkan Porting Guide below
is the task list to close the gap.

### Opaque region

`scene_node_opaque_region()` reports **zero opaque area** for any node with an
active VFX payload (wlr_scene.c:269, 278). The rounded corners, inner cutout,
and shadow make the whole node potentially transparent, so content behind it
must not be culled.

Note the corner-only case: a node with only `corner_radius` set is not "active"
per `scene_node_vfx_active`, so a fully-opaque rect or buffer with rounding
still reports its full bounds as opaque. Rounded corners at the edges of an
opaque buffer can therefore be over-culled; the opaque-region conservative
checks (alpha == 1, buffer_is_opaque) are the only mitigations today.

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

The VFX rect's position and size are also adjusted by visual state
(`scene_entry_render_vfx`, wlr_scene.c:1529):
- `x_rel += vis_off_x` (offset from accumulated visual.x/y)
- `vw *= vis_scale_x` (scale VFX width by accumulated scale)
- `vh *= vis_scale_y`
- `x_rel += (original_vw - vw) / 2` (center-shrink like dst_box)
- Shadow opacity: `shadow.opacity * vis_alpha`
- Border thickness and corner radius are scaled by the output scale and
  rounded with `roundf()` to match the content's pixel grid (see Quirk 6).

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

The pixman renderer (software fallback) does **not** support VFX. It never sets
`features.vfx`, so the capability gate applies exactly as it does on Vulkan:

- Rect/buffer nodes with an active VFX payload are skipped in
  `scene_entry_render` — no border, no shadow, no rounded corners.
- `corner_radius` on plain rects, single-pixel buffers, and textures is
  ignored.

There is no pixman-side SDF, mask, or blur logic for VFX. `scene_entry_render_vfx`
is only reached when `features.vfx` is set, which only the GLES2 renderer does.
The visual-override fields (scale, opacity, offset) are the only VFX-adjacent
features pixman honors, via its existing region/mask composition.

---

## Opaque region and damage tracking

Rounded corners make parts of a node transparent that would otherwise be
opaque. Nodes with an active VFX payload report zero opaque area (see "Opaque
region" above) so content behind them is never culled incorrectly.

Damage tracking itself (`scene_node_update()`) is unchanged — the entire node
bounding box is damaged when VFX or visual state changes. Because the VFX rect
bounds include the shadow expansion, the shadow bleed is naturally covered by
the node's own damage region.

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

### 2. Opaque region must zero-out any node with an active VFX payload

`scene_node_opaque_region()` must report zero opaque area for nodes carrying an
active VFX payload — otherwise the node's full bounds are marked fully opaque
and content behind it gets culled during visibility accumulation.

**Fix (implemented):** `scene_node_opaque_region` early-returns for both rect
(wlr_scene.c:269) and buffer (wlr_scene.c:278) nodes when
`scene_node_vfx_active(node)` is true. This replaced the old
`WLR_SCENE_NODE_VFX` branch that existed before the VFX type was folded into
rects.

### 3. Blend mode must stay PREMULTIPLIED when a border is present

With `WLR_RENDER_BLEND_MODE_NONE`, the rect clears the framebuffer instead of
blending. A VFX rect draws with per-pixel alpha (its center is transparent), so
NONE would overwrite the framebuffer with the flat color — or with transparent
black — instead of letting content show through. The old override
(`color->a == 1.0` → NONE) had the same problem for fully-opaque borders.

**Fix (implemented in GLES2):** `render_pass_add_rect`
(`render/gles2/pass.c:279-284`) forces `WLR_RENDER_BLEND_MODE_PREMULTIPLIED`
whenever any `border_thickness` is non-zero; otherwise it uses
`options->blend_mode` directly. The Vulkan port must replicate this (see the
Vulkan Porting Guide, task 5).

### 4. `fwidth()` requires GL_OES_standard_derivatives on GLES2

GLES2 does not support `fwidth()` without explicit extension enable:
```glsl
#extension GL_OES_standard_derivatives : enable
```
The Vulkan equivalent is the optional `fragmentShaderDerivatives` device
feature (see the Vulkan Porting Guide, task 1).

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

**Fix:** Multiply thickness and radius values by `scale` in
`scene_entry_render_vfx`, and use `roundf()` on each border edge offset to match
the content's rounding exactly, eliminating sub-pixel gaps:

```c
bt_left = roundf((x_rel + left_logical) * s) - roundf(x_rel * s);
bt_top  = roundf((y_rel + top_logical) * s) - roundf(y_rel * s);
bt_right = roundf((x_rel + vw) * s) - roundf((x_rel + vw - right_logical) * s);
bt_bottom = roundf((y_rel + vh) * s) - roundf((y_rel + vh - bottom_logical) * s);
```

### 7. `WL_OUTPUT_TRANSFORM_FLIPPED_180` projection matrix

The GLES2 render pass creates its projection matrix with
`WL_OUTPUT_TRANSFORM_FLIPPED_180` (render/gles2/pass.c). This does not actually
flip the Y axis — `gl_FragCoord.xy` and the box coordinates are in the same
orientation (Y = 0 at top, increasing downward). No Y-coordinate adjustment is
needed in shaders.

### 8. `scene_node_invisible` must be extended for every new VFX effect

`scene_node_invisible()` (wlr_scene.c:2113) skips a rect only when its color is
fully transparent **and** no VFX is active:
`rect->color[3] == 0.f && !scene_node_vfx_active(node)`. Since sway's VFX rect
is transparent but has an active payload, it stays in the render list. Adding a
new effect (e.g., glow, inner shadow) requires extending `scene_node_vfx_active`
so the node is not skipped when the effect is active.

### 9. `wlr_scene_animation_cancel` calls `wlr_scene_node_set_visual` for all animations

`wlr_scene_animation_cancel()` unconditionally calls
`wlr_scene_node_set_visual(anim->node, &anim->to)`, even for position
animations where `anim->to` is a visual struct that was never initialized
(zeroed from calloc). For position animations, this would reset the visual
state to all zeros (scale=0, opacity=0). This function is not currently used
by any caller.

### 10. `quad.frag` has explicit paths for VFX and plain rects

`render_pass_add_rect()` serves both plain `wlr_scene_rect` nodes and VFX
border/shadow nodes via the same `quad.frag` shader. Early versions had no
VFX-free fallback: the `else` branch left `u_box` at `(0,0,0,0)`, making
`corner_alpha()` always return 0, and the blend-mode override produced
transparent black instead of the requested color.

**Fix (implemented):** `render_pass_add_rect` now always sets `u_box` to the
actual rect and resets all VFX uniforms to 0 for plain rects (gles2/pass.c:312).
`quad.frag` now early-returns after the shadow/border paths and ends with a
plain `color * corner_alpha` fallback — with `corner_radius = 0`,
`corner_alpha = 1`, so plain rects draw their color unchanged.

### 11. VFX rect position/size must extend into parent-managed title bar area

When a tabbed or stacked parent manages title bars, each child's VFX rect
(border/shadow) must extend upward into the title bar area so corners and
shadows cover the title bar background. The child's scene tree is offset below
the title bars, but the VFX rect needs to know how much space to extend into.

**Fix:** In `arrange_container`, when `title_bar` is false (parent-managed),
compute `title_ext` from the parent's layout:
`container_titlebar_height()` for tabbed, `N * container_titlebar_height()`
for stacked. The VFX rect's position is shifted up by `title_ext` and its
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

---

## Vulkan Porting Guide

The Vulkan renderer currently ignores all VFX fields. It never sets
`features.vfx`, so the capability gate (see "Capability gate") skips
VFX-active nodes and ignores `corner_radius`. This section is the concrete
task list to add VFX support to Vulkan.

### Data flow (already renderer-agnostic)

The scene graph populates `wlr_render_rect_options` / `wlr_render_texture_options`
in `scene_entry_render()` and passes them to `wlr_render_pass_add_rect()` /
`wlr_render_pass_add_texture()`. Both backends receive the same structs —
Vulkan just doesn't read the extra fields yet.

```c
// include/wlr/render/pass.h
struct wlr_render_rect_options {
    struct wlr_box box;                  // full VFX bounds, shadow expansion included
    struct wlr_render_color color;       // = border color for VFX rects
    const pixman_region32_t *clip;
    enum wlr_render_blend_mode blend_mode;

    float corner_radius[4];        // tl, tr, br, bl — zero = square
    float border_thickness[4];     // top, right, bottom, left — zero = filled
    float shadow_blur_sigma;       // zero = no shadow
    float shadow_opacity;          // 0.0–1.0, multiplied by shadow_color.a
    struct wlr_render_color shadow_color; // premultiplied RGBA
};
```

`wlr_render_texture_options` has `float corner_radius[4]` too (for
corner-clipped textures — no border/shadow needed).

### 0. Design overview

The GLES2 backend renders VFX in a single SDF fragment pass over clip-rect
instances. The Vulkan backend already uses the same instanced-clip-rect model
in `render_pass_add_rect` (render/vulkan/pass.c) and `render_pass_add_texture`.
The port is therefore mechanical:

- Reuse the GLES2 SDF math in `quad.frag`.
- Keep per-draw appearance data in **push constants** — the idiomatic vehicle
  for small per-draw uniforms, and the Vulkan renderer has no uniform-buffer
  plumbing for draws.
- The VFX appearance fields (56 bytes) fit the existing fragment push-constant
  range `[48, 120)` with room to spare.
- The rect box and corner radius must **not** go in push constants (that would
  blow the 128-byte budget). Pass them through the **instance vertex stream** as
  flat varyings instead. This is the one structural change.

**Alternatives considered and rejected:**
- *Uniform buffer (UBO) / descriptor set* — the renderer has no descriptor
  machinery for per-draw data; every draw would need descriptor-pool allocation
  and binding. Push constants are the right tool for ≤128 bytes of frequently
  changing data.
- *box in vertex push constants* — shifts every fragment push offset in the
  shared pipeline layout (the texture frag block sits at 48 and would move),
  touches all existing `vkCmdPushConstants` calls and the `static_assert`.
- *Query `maxPushConstantsSize` and set `features.vfx = true` only if ≥ 136* —
  couples a feature flag to a hardware capability that the instance-stream
  approach sidesteps entirely, and leaves the texture corner-radius case over
  budget anyway.

### 1. Enable `fragmentShaderDerivatives` (or use fixed-width AA)

GLES2's SDF uses `fwidth()` (`GL_OES_standard_derivatives`). In Vulkan,
`fwidth()`/`dFdx`/`dFdy` require the optional `fragmentShaderDerivatives`
device feature, which `vulkan_device_create` (render/vulkan/vulkan.c) does not
currently enable — it enables no `VkPhysicalDeviceFeatures` at all.

Two options:

```c
// (a) Preferred — matches GLES2 pixel-for-pixel.
VkPhysicalDeviceFeatures2 feats = {
    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
    .features.fragmentShaderDerivatives = VK_TRUE,
    // pNext chains with the existing samplerYcbcr/sync2/timeline features
};
```

If the device does not support `fragmentShaderDerivatives`, fall back to a
fixed-width AA band instead of `fwidth()`:

```glsl
float ca = clamp(0.5 - d / 0.75, 0.0, 1.0); // ~0.75px AA band
```

Prefer (a); the fixed-width variant needs no feature and is fully portable.

### 2. Pass box + corner radius through the instance stream

The vertex input layout (`instance_vert_binding` / `instance_vert_attr` in
render/vulkan/renderer.c) currently has one `vec4` per instance (the normalized
clip rect). Widen it to three `vec4`s — `inst_rect`, `inst_box`, `inst_corner`
— and use the **same** layout for both the rect and texture pipelines:

```c
static const VkVertexInputBindingDescription instance_vert_binding = {
    .binding = 0,
    .stride = sizeof(float) * 12,        // was 4
    .inputRate = VK_VERTEX_INPUT_RATE_INSTANCE,
};
static const VkVertexInputAttributeDescription instance_vert_attrs[] = {
    { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 0  },  // inst_rect
    { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 16 },  // inst_box
    { .location = 2, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 32 },  // inst_corner
};
```

`common.vert` forwards them as flat varyings (the existing `uv` output stays,
the texture pipeline still consumes it):

```glsl
layout(location = 1) in vec4 inst_box;
layout(location = 2) in vec4 inst_corner;
flat out vec4 v_box;
flat out vec4 v_corner;
// ...
v_box = inst_box;
v_corner = inst_corner;
```

Every draw writes the full 12 floats per clip rect:
- `inst_rect` — the normalized clip rect (as today)
- `inst_box` — `options->box` / `dst_box` in buffer coordinates
- `inst_corner` — `options->corner_radius` (all zero for non-VFX draws)

For plain rects `inst_corner = {0,0,0,0}`, so `corner_alpha = 1` and the shared
shader's fallback path draws the flat color — no special casing.

### 3. `quad.frag` — port the SDF

Replace the flat-color passthrough with the GLES2 SDF
(render/gles2/shaders/quad.frag): `corner_sdf`, `corner_alpha`, the shadow
layer (Gaussian falloff clipped out of the inner rect), the border rim
(outer minus inner), and the plain `color * corner_alpha` fallback.

### 4. Fragment push constants — 56 bytes, fits `[48, 120)`

The pipeline layout's fragment range is already `[48, 120)` (`init_tex_layouts`,
render/vulkan/renderer.c), so no layout change and no `static_assert` change.

```glsl
layout(push_constant) uniform Vfx {
    layout(offset = 48) vec4 color;            // premultiplied linear RGBA
    layout(offset = 64) vec4 border_thickness; // top, right, bottom, left
    layout(offset = 80) vec2 shadow;           // x = blur_sigma, y = opacity
    layout(offset = 88) vec4 shadow_color;     // premultiplied linear RGBA
} vfx;
```

`shadow.x`'s extension is computed in-shader as `ext = shadow.x * 3.0`, matching
`wlr_scene_vfx_shadow_extension` (`blur_sigma * 3.0`). The scene passes the
already-expanded box in `inst_box`; the shader computes the container rect as
`inst_box` inset by `ext` — same as the GLES2 `u_shadow.w` mechanism.

### 5. `render_pass_add_rect`

- **Force `WLR_RENDER_BLEND_MODE_PREMULTIPLIED`** whenever any
  `border_thickness` is non-zero, matching the GLES2 fix (Quirk 3) —
  otherwise the NONE path (`vkCmdClearAttachments`) would clear instead of
  blend.
- **Convert colors to linear premultiplied** before pushing: `color`,
  `border_color`, and `shadow_color` must go through `color_to_linear_premult`
  (the existing pattern at pass.c:647-652), because the Vulkan pipeline
  computes in linear space and the render pass applies sRGB encoding on output.
  GLES2 works in sRGB and does not convert — the two backends legitimately
  diverge here.
- **Do not expand the box.** The scene already sizes the VFX rect to include
  the shadow extension; the shader insets by `ext`. Expanding again would
  misalign the border with the content.
- Replace the current 16-byte color push (pass.c:718-720) with the full 56-byte
  block from task 4.

### 6. Texture corner radius

`wlr_render_texture_options.corner_radius` is also ignored. With the instance
stream from task 2, the box and corner radius already arrive as `v_box` /
`v_corner`, so `texture.frag` needs no push-constant change:

```glsl
// uv already occupies location 0; box/corner match common.vert outputs
layout(location = 1) flat in vec4 v_box;
layout(location = 2) flat in vec4 v_corner;
// ...
float ca = corner_alpha(gl_FragCoord.xy - v_box.xy, v_box.zw, v_corner);
out_color *= ca;
```

No border or shadow is needed for textures.

### 7. Flip the capability flag

Once tasks 1-6 land, set `features.vfx` in `vulkan_renderer_create_for_device`
(render/vulkan/renderer.c, alongside the other feature assignments):

```c
renderer->wlr_renderer.features.vfx = true;
```

Until this is done, the scene keeps skipping VFX on Vulkan — which is the
current, safe fallback.

### 8. Capability gate (current fallback — already implemented)

`renderer->features.vfx` (include/wlr/render/wlr_renderer.h) is set true only by
the GLES2 renderer (render/gles2/renderer.c). On Vulkan and pixman:

- `scene_entry_render` breaks out for rect/buffer nodes with an active VFX
  payload (wlr_scene.c:1661, 1694) — nothing is drawn, no garbage.
- `corner_radius` on plain rects, single-pixel buffers, and textures is ignored
  (wlr_scene.c:1682, 1716, 1770).

This is the documented fallback behavior today. Completing tasks 1-7 removes the
gap. Do not change the gate itself — it is the correct mechanism for renderers
that genuinely cannot do SDF-style VFX.
