# Shader-based VFX + Animation Architecture

This document describes the design for moving border rendering, drop shadows,
rounded corners, and all spatial animations (move, resize, opacity) into the
wlroots GLES2 shader pipeline, replacing the existing CPU-side scene-node
approaches.

## Design Philosophy

### VFX vs Animation — two separate concerns

| Concern | Nature | Owner | Lifetime |
|---------|--------|-------|----------|
| Rounded corners, borders, shadows | Static shader uniforms | `wlr_scene_node::vfx` | Until explicitly changed |
| Position, size, opacity transitions | Temporal tween | `wlr_scene_node::visual` + animation system | Until tween completes |

VFX are **not** animations. A corner radius doesn't tween — it's a fixed property
of how the window looks, set once when the config or focus changes.

### The animation system lives in wlroots, not Sway

Sway currently has its own animation timer (`sway_anim_sync`) and per-node state
(`struct sway_anim`) in `sway/tree/animation.c`. The new design moves this into
wlroots for three reasons:

1. **The renderer needs the data** — VFX uniforms and visual-size overrides are
   consumed by the GLES2 shader in `scene_entry_render`. Having the state in
   wlroots avoids cross-layer callbacks to fetch it.

2. **Zero-cost when idle** — `wlr_scene_node::visual` is NULL when no animation
   is active. The render pass checks `if (visual && visual->width > 0)` before
   binding the scaled-texcoord shader path — otherwise the fast path runs
   unchanged.

3. **No special cases in Sway** — Sway calls `wlr_scene_buffer_set_dest_size_anim()`
   and the animation "just works". No per-animation bookkeeping in Sway code.

### Parallel API — never modify existing setters

wlroots' existing `wlr_scene_buffer_set_dest_size()`, `wlr_scene_rect_set_size()`,
`wlr_scene_node_set_position()`, and `wlr_scene_buffer_set_opacity()` are
**completely unchanged**. This preserves easy upstream merging.

New `_anim` variants are added alongside:
- `wlr_scene_buffer_set_dest_size_anim(buf, w, h, spec)`
- `wlr_scene_node_set_position_anim(node, x, y, spec)`
- `wlr_scene_node_set_opacity_anim(node, a, spec)`

These set the **logical** (real) value immediately (for input targeting), and
register a tween on the node's `visual` struct that the shader reads.

### Sway driving vs wlroots driving animations

Sway still drives the frame clock (via `sway_anim_sync`), but it only calls
`wlr_scene_animator_tick(animator, now_ns)`. The animation system handles
all easing, completion, and uniform writing internally. If any animation is
active, `wlr_scene_animator::request_frame` fires, and Sway schedules a
wlr_output frame.

```
Sway                   wlroots
  │                      │
  │── tick(now) ──────▶  animator
  │                      ├── tween each active anim
  │                      ├── write node->visual
  │                      ├── if done: fire callback, remove
  │                      └── emit request_frame if still active
  │◀── request_frame ────┘
  │── render frame ────▶  scene_entry_render()
  │                       ├── read node->visual → shader uniforms
  │                       ├── read node->vfx → corner/border/shadow
  │                       └── draw
```

---

## Data Structures

### `wlr_scene_node` additions

```c
// ── VFX (static, per-node appearance) ──────────────────────────────────────

struct wlr_scene_node_vfx {
    float corner_radius[4];         // tl, tr, br, bl (0 = square)
    struct {
        float thickness[4];          // top, right, bottom, left (0 = no border)
        float color[4];             // premultiplied RGBA
    } border;
    struct {
        float blur_sigma;           // 0 = no shadow
        float opacity;              // 0.0 – 1.0
        float color[4];             // premultiplied RGBA
    } shadow;
};

// ── Animation / visual override (written by animator, read by renderer) ───

struct wlr_scene_node_visual {
    float x, y;                     // tweened visual position offset
    float width, height;            // tweened visual size (0 = use real)
    float opacity;                  // tweened opacity (0.0 – 1.0)
};

// Extended node struct:

struct wlr_scene_node {
    enum wlr_scene_node_type type;
    struct wlr_scene_tree *parent;
    struct wl_list link;
    bool enabled;
    int x, y;
    struct { struct wl_signal destroy; } events;
    void *data;
    struct wlr_addon_set addons;
    struct { pixman_region32_t visible; } WLR_PRIVATE;

    // NEW:
    struct wlr_scene_node_vfx *vfx;         // NULL = no VFX
    struct wlr_scene_node_visual *visual;   // NULL = no active animation
};
```

### Animation types

```c
enum wlr_anim_property {
    WLR_ANIM_VISUAL_X,
    WLR_ANIM_VISUAL_Y,
    WLR_ANIM_VISUAL_WIDTH,
    WLR_ANIM_VISUAL_HEIGHT,
    WLR_ANIM_VISUAL_OPACITY,
};

struct wlr_anim_spec {
    uint32_t duration_ns;       // 0 = instant (no animation)
    float (*easing)(float t);   // NULL = ease_out_cubic
    void (*done)(void *data);   // completion callback (may be NULL)
    void *done_data;
};

struct wlr_scene_animation {
    struct wlr_scene_node *node;
    enum wlr_anim_property prop;
    double from, to;
    uint32_t start_time_ns, duration_ns;
    float (*easing)(float t);
    void (*done)(void *data);
    void *done_data;
    struct wl_list link;        // wlr_scene_animator.animations
};

struct wlr_scene_animator {
    struct wlr_scene *scene;
    struct wl_list animations;  // wlr_scene_animation.link
    bool active;                // cached for fast check
    struct wl_signal request_frame;
};
```

### Key design decisions

**Why separate `vfx` and `visual`?**
- `vfx` is set once per config/focus change — the compositor writes it, the
  renderer reads it every frame. It never tweens.
- `visual` is written by the animation system every tick. Separating the two
  means VFX updates don't route through the animation system at all, and the
  animation system doesn't need to know about corner radii or shadow config.

**Why a pointer (`*vfx`, `*visual`) instead of embedding?**
- Zero-cost when not in use: every non-animated, non-VFX node has NULL for both.
  The wlroots scene tree has thousands of nodes (subsurfaces, drag icons, etc.)
  that will never use VFX or animation.
- Embedding would add 140+ bytes per node (two structs) regardless of need.

---

## VFX Rendering: `WLR_SCENE_NODE_VFX`

### New node type

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
├── container->vfx_node      ← NEW WLR_SCENE_NODE_VFX
│     shader draws:
│       1. Drop shadow (behind, blurred rounded rect)
│       2. Border (inset outline, corner-radius clipped)
│       → rest of the box is transparent (content shows through)
├── container->title_bar.tree
│     └── background rects, text — corner_radius applied
├── container->content_tree
│     └── view->scene_tree
│           └── wlr_scene_buffer — corner_radius clips content
```

No more `container->border.{top,bottom,left,right}` — the four individual scene
rects are replaced by one VFX node. The border is drawn by the fragment shader
as an inset rounded stroke, eliminating all the edge-positioning math in
`arrange_container`/`transaction.c`.

### Fragment shader (vfx.frag)

```glsl
// vfx.frag — draws shadow + border for WLR_SCENE_NODE_VFX

uniform vec2  u_size;           // VFX node logical size
uniform vec4  u_corner_radius;  // tl, tr, br, bl
uniform vec4  u_border_thickness; // t, r, b, l
uniform vec4  u_border_color;
uniform float u_shadow_blur;
uniform float u_shadow_opacity;
uniform vec4  u_shadow_color;

// From common SDF function:
float corner_alpha(vec2 frag, vec2 pos, vec2 size, vec4 radii);

void main() {
    vec2 frag = gl_FragCoord.xy;
    vec4 r = u_corner_radius;

    // 1. Shadow — draw Gaussian-blurred rounded rect behind
    if (u_shadow_blur > 0.0 && u_shadow_opacity > 0.0) {
        float shadow_alpha = corner_alpha(frag,
            pos - u_shadow_blur,          // expanded bounds
            u_size + 2.0 * u_shadow_blur, // expanded bounds
            r + u_shadow_blur)            // expanded corner
            * u_shadow_opacity;
        gl_FragColor = u_shadow_color * shadow_alpha;
    }

    // 2. Border — outer rounded rect minus inner rounded rect
    float outer_alpha = corner_alpha(frag, pos, u_size, r);
    vec2 inner_pos = pos + u_border_thickness.tl; // inset from top-left
    vec2 inner_size = u_size - u_border_thickness.tl
                              - u_border_thickness.br; // inset from all sides
    vec4 inner_r = max(r - u_border_thickness.tl, 0.0);
    float inner_alpha = 1.0 - corner_alpha(frag, inner_pos, inner_size, inner_r);

    float border_alpha = outer_alpha * inner_alpha;
    gl_FragColor = u_border_color * border_alpha;
}
```

The VFX node sits at the container's bounds. Content sits behind it (in the
scene tree z-order under the VFX node) and is clipped by a separate
`wlr_scene_buffer_set_corner_radius()` call on the content buffer node.

### Why a separate node type instead of merging VFX into every node?

Every buffer and rect would carry VFX uniforms (border, shadow, corner) even
though most don't use them. A dedicated VFX node is drawn once per container
instead of duplicating the logic across the content buffer node and the 4 old
border rect nodes. It also keeps the scene tree structure clear: "this node is
purely decorative."

---

## Animation rendering: `wlr_scene_node_visual`

When `node->visual` is non-NULL and `visual->width > 0`, the render pass
overrides the node's destination size with the tweened visual size:

```c
// In scene_entry_render() — after scene_node_get_size():

if (node->visual && node->visual->width > 0) {
    dst_box.width = node->visual->width;
    dst_box.height = node->visual->height;
}
if (node->visual && (node->visual->x != 0 || node->visual->y != 0)) {
    dst_box.x += node->visual->x;
    dst_box.y += node->visual->y;
}
```

### For textures (resize animation)

The fragment shader receives `u_visual_size` and scales texture coordinates:

```glsl
// tex_rgba.frag (modified)
uniform vec2 u_visual_size;      // tweened, 0 = use real

void main() {
    vec2 size = u_visual_size.x > 0.0 ? u_visual_size : u_real_size;
    vec2 uv = (gl_FragCoord.xy - u_pos) / size;
    gl_FragColor = texture2D(tex, uv) * alpha;
}
```

This means during a resize animation, the old buffer content is smoothly scaled
to the intermediate visual size. No waiting for a new client buffer — the shader
scales whatever the client last sent. When the animation completes and a new
correctly-sized buffer arrives, `u_visual_size` matches the real size and the
effect is invisible.

### For opacity

```c
// In scene_entry_render():
float opacity = scene_buffer->opacity;
if (node->visual) {
    opacity *= node->visual->opacity;
}
```

Opacity is simply multiplied — the existing `.alpha` field in the render
pass already handles this.

### For position (move animation)

The VFX node's border/shadow follow the container's visual position
automatically because the VFX node is a child of the container's scene tree.
When `wlr_scene_node_set_position_anim()` tweens the container's visual x/y,
the VFX node (positioned at 0,0 relative to parent) moves with it.

---

## Pixman fallback

The pixman renderer (software fallback when no GPU is available) doesn't
support custom fragment shaders. For each VFX/animation feature:

| Feature | Pixman approach |
|---------|----------------|
| Corner radius | Render normally, then composite corner cutouts using `pixman_image_create_solid_fill()` + `pixman_image_composite32()` with a rounded mask |
| Borders | Render the outer rect, then punch out the inner rect using pixman region subtract |
| Shadow | Expand the rect by `blur_sigma`, render the shadow color with a pixman gaussian-blur convolution |
| Visual size scaling | Use `pixman_image_set_transform()` with a bilinear scale (already exists in pixman pass) |
| Visual opacity | Already handled by the `mask` parameter in `pixman_image_composite32()` |

The pixman backend falls behind GLES2 quality-wise (no smoothstep for
anti-aliased corners), but this is the same limitation as the current pixman
path — it's a software fallback.

---

## Opaque region and damage tracking

Rounded corners make parts of a node transparent that would otherwise be
opaque. This matters for occlusion culling: wlroots uses the opaque region to
skip rendering nodes hidden behind opaque ones.

For each node with `corner_radius > 0`:

```c
// In scene_node_opaque_region():
if (node->vfx && node->vfx->corner_radius) {
    pixman_region32_t corner_squares = create_corner_square_region(
        node->vfx->corner_radius, x, y, width, height);
    pixman_region32_subtract(opaque, opaque, &corner_squares);
    pixman_region32_fini(&corner_squares);
}
```

This subtracts the four corner squares (where the rounded rect fades to
transparent) from the opaque region. The center of the window remains opaque,
so nodes behind the center are still culled correctly.

Damage tracking itself (`scene_node_update()`) is unchanged — the entire node
bounding box is damaged when VFX or visual state changes, the same as any
other property change.

---

## Sway integration

### Config

```c
// sway/config.h additions
struct sway_config {
    int corner_radius;              // 0 = square
    bool shadow_enabled;
    int shadow_blur_radius;
    float shadow_opacity;
    struct wlr_render_color shadow_color;
    // ... current fields ...
};
```

### Command parsing

New commands: `corner_radius`, `shadows`, `shadow_blur_radius`, `shadow_color`,
`shadow_opacity`.

```c
// sway/commands/corner_radius.c
struct cmd_results *cmd_corner_radius(int argc, char **argv) {
    config->corner_radius = atoi(argv[0]);
    // Apply to all existing containers:
    container_for_each(container_apply_vfx, NULL);
    arrange_root();
    return cmd_results_new(CMD_SUCCESS, NULL);
}
```

### Container creation

```c
// sway/tree/container.c — replaces 4 border rects with one VFX node
struct sway_container *container_create(enum sway_container_layout layout) {
    // ...
    // Before:
    c->border.top = alloc_rect_node(c->border.tree, &failed);
    c->border.bottom = alloc_rect_node(c->border.tree, &failed);
    c->border.left = alloc_rect_node(c->border.tree, &failed);
    c->border.right = alloc_rect_node(c->border.tree, &failed);

    // After:
    c->vfx_node = wlr_scene_vfx_create(c->scene_tree, 0, 0);
    // (vfx_node stores corner/border/shadow state in node->vfx)
}
```

### Transaction apply

```c
// sway/desktop/transaction.c — arrange_container():

// BEFORE: 20 lines of individual border rect sizing + view offset
wlr_scene_rect_set_size(con->border.top, width, border_top);
wlr_scene_rect_set_size(con->border.bottom, width, border_bottom);
wlr_scene_rect_set_size(con->border.left, border_left, vert_border_height);
wlr_scene_rect_set_size(con->border.right, border_right, vert_border_height);
wlr_scene_node_set_position(&con->border.top->node, 0, 0);
wlr_scene_node_set_position(&con->border.bottom->node, 0, ...);
wlr_scene_node_set_position(&con->border.left->node, 0, border_top);
wlr_scene_node_set_position(&con->border.right->node, ...);
wlr_scene_node_set_position(&con->view->scene_tree->node, border_left, border_top);
wlr_scene_node_set_position(&con->view->output_handler->node, -border_left, -border_top);

// AFTER: one VFX node + corner_radius on content
if (con->vfx_node) {
    wlr_scene_vfx_set_size(con->vfx_node, width, height);
    wlr_scene_vfx_set_border_color(con->vfx_node, state->border_color);
    con->vfx_node->node.vfx->corner_radius = config->corner_radius;
    con->vfx_node->node.vfx->shadow = { config->shadow_blur, ... };
}
// Content at (0,0) — borders are purely visual
wlr_scene_node_set_position(&con->view->scene_tree->node, 0, 0);
// Corner radius on content buffer
if (con->view && con->view->content_buffer) {
    wlr_scene_buffer_set_corner_radius(con->view->content_buffer,
        config->corner_radius);
}
```

### Focus change

```c
// sway/tree/container.c — container_set_border_colors() (called on focus):
wlr_scene_vfx_set_border_color(con->vfx_node, focused ? colors->focused : colors->unfocused);
```

### Animation

```c
// sway/tree/animation.c — sway_anim_move() is now a thin wrapper:
void sway_anim_move(struct wlr_scene_node *node,
        double from_x, double from_y, double to_x, double to_y,
        struct sway_prop_config cfg) {
    (void)from_x; (void)from_y; // from is handled internally
    struct wlr_anim_spec spec = {
        .duration_ns = cfg.type == SWAY_ANIM_SPRING
            ? spring_settle_time(cfg.damping_ratio, cfg.stiffness, cfg.epsilon)
            : cfg.duration_ms * 1000000,
        .easing = cfg.type == SWAY_ANIM_EASE ? ease_out_cubic : NULL,
        .done = NULL,
    };
    wlr_scene_node_set_position_anim(node, to_x, to_y, &spec);
}

void sway_anim_sync(void) {
    uint64_t now_ns = /* clock_gettime monotonic */;
    struct wlr_scene_animator *anim = scene_node_get_root(some_node)->animator;
    wlr_scene_animator_tick(anim, now_ns);
}
```

### Column resize animation

```c
// sway/tree/column.h:
static inline void column_set_width_px(struct sway_container *col, double width_px) {
    struct sway_container *siblings = container_get_siblings(col);
    // ... existing logic ...
    wlr_scene_buffer_set_dest_size_anim(col->content_tree,
        width_px, col->pending.height,
        &(struct wlr_anim_spec){ .duration_ns = 200000000, .easing = ease_out_cubic });
}
```

---

## Migration from current Sway animation system

| Current (`sway/tree/animation.c`) | New |
|-----------------------------------|-----|
| `struct sway_anim` per node | `struct wlr_scene_animation` per property |
| `wl_list animations` in Sway | `wlr_scene_animator::animations` in wlroots |
| `sway_anim_move(node, from, to, cfg)` | `wlr_scene_node_set_position_anim(node, to_x, to_y, &spec)` |
| `sway_anim_alpha(node, from, to, cfg)` (stub — no-op) | `wlr_scene_node_set_opacity_anim(node, to, &spec)` — works via visual->opacity |
| `sway_anim_scale` (removed previously) | `wlr_scene_buffer_set_dest_size_anim(buf, w, h, &spec)` |
| `sway_anim_sync()` calls `wlr_scene_node_set_position()` | `sway_anim_sync()` calls `wlr_scene_animator_tick(anim, now)` |
| Timer-driven via `wl_event_source_add_timer` | Same — but calls `wlr_scene_animator_tick()` instead of tweening directly |
| Node-destroy listener cleaning up `struct sway_anim` | wlr_scene_node_destroy() cancels all animations on the node |

---

## Upstream merge compatibility

All wlroots changes are **additive**:
- New fields at the end of existing structs (or as pointer members)
- New functions alongside existing ones (`_anim` variants, `*_vfx_*`)
- New node type added to the enum (doesn't break existing switch statements
  that don't handle it — they'll hit the `default:` or `assert(false)` path)

The only modifications to existing files are:
- `wlr_scene.h` — struct field additions + new function declarations
- `wlr_scene.c` — `scene_entry_render` gets new branches (VFX dispatch,
  visual override check) and opaque region adjustments
- `pass.h` — `corner_radius` field in options structs
- `gles2/pass.c` — uniform binding for new shader paths
- `gles2/shaders/quad.frag` / `tex_rgba.frag` / `tex_rgbx.frag` — uniform
  declarations + VFX/visual coordinate branching

None of these delete or refactor existing code paths. The non-VFX, non-animated
fast path is identical to upstream: `if (!node->vfx && !node->visual) { /* same */
}`.

---

## Implementation order

1. Add VFX fields + `WLR_SCENE_NODE_VFX` type to wlroots (initially invisible)
2. Add `corner_radius` to render pass options + GLES2 shaders
3. Add `wlr_scene_rect_set_corner_radius()` + `wlr_scene_buffer_set_corner_radius()`
3. Replace border rects in Sway with `wlr_scene_vfx` node
4. Add shadow rendering to VFX shader
5. Add `wlr_scene_node_visual` + animation infrastructure
6. Add `_anim` API setters
7. Migrate Sway `sway_anim_move` → `wlr_scene_node_set_position_anim`
8. Add column resize animation via `wlr_scene_buffer_set_dest_size_anim`
9. Add pixman fallback rendering for VFX + visual overrides
10. Remove dead code: `container->border.{top,bottom,left,right}` in Sway,
    old `sway_anim_scale` remnants, `sway_anim_alpha` stub

---

## Quirks & Findings

### 1. `scene_node_opaque_region` has no VFX case

`scene_opaque_region()` (called per-node during the visibility/update pass)
lacked a `WLR_SCENE_NODE_VFX` branch and fell through to the default, which
set the **entire VFX node bounds as fully opaque**. Since the VFX node is a
sibling drawn above the content, its "opaque" area was subtracted from the
shared visibility accumulator *before* the content buffer was processed,
causing the content's `node->visible` to be empty (culled).

**Fix:** Add `} else if (node->type == WLR_SCENE_NODE_VFX) { return; }` to
report zero opaque area.

### 2. Blend mode forced to NONE when uniform `color->a == 1.0`

`render_pass_add_rect()` in `render/gles2/pass.c` overrides the blend mode:
```c
color->a == 1.0 ? WLR_RENDER_BLEND_MODE_NONE : options->blend_mode;
```
For a VFX border frame, the border color's alpha is typically 1.0 (fully opaque
border paint). This forces `WLR_RENDER_BLEND_MODE_NONE`, but the shader
computes per-pixel alpha — the center is `color * 0 = (0,0,0,0)`. With NONE
blend mode, `gl_FragColor = (0,0,0,0)` is written directly, overwriting the
framebuffer with black instead of letting content show through.

**Fix:** Check `border_thickness` before the blend-mode decision — when any
border thickness is non-zero, force `WLR_RENDER_BLEND_MODE_PREMULTIPLIED`.

### 3. Shader border offset direction (sign error)

The original shader computed the inner (content) rect offset as:
```glsl
vec2 inner_pos = pos + vec2(bt[3], bt[0]); // BUG: should be subtraction
```
`pos` is the fragment position relative to the VFX node's top-left. The inner
rect starts at `(bt_left, bt_top)` in VFX coordinates. But `corner_alpha()`
interprets its first argument as coordinates *within* the inner rect's own
space (where `(0,0)` is the inner rect's origin). Adding the offset pushed
the inner rect's *perceived* origin `2×` inward, so the top and left border
areas were treated as inside the inner rect and made transparent.

**Fix:** `inner_pos = pos - vec2(bt[3], bt[0])` — subtracting shifts the
coordinate space so VFX `(bt_left, bt_top)` maps to `(0, 0)` in inner-rect
space.

### 4. `fwidth()` requires GL_OES_standard_derivatives on GLES2

GLES2 does not support `fwidth()` without an explicit extension enable:
```glsl
#extension GL_OES_standard_derivatives : enable
```
Without it, the shader silently fails to compile on some drivers (notably
Mesa software rasterizer) and produces aliased/non-existent corner
smoothing on others.

### 5. wlroots as a standalone git repo inside sway

`subprojects/wlroots/` has its own `.git/` directory — **it is not a git
submodule**. Both repos need separate commits:
```bash
cd subprojects/wlroots && git commit -m "..."
cd ../.. && git commit -m "..."
```
The meson build system picks up whatever is on disk. Rebuilding after
wlroots changes requires recompiling wlroots, but ninja handles this
automatically.

### 7. HiDPI border/inner rect coordinate-space mismatch

The VFX shader's `pos` and `size` are in **buffer coordinates** (after scaling
by output scale via `transform_output_box`), but `u_border_thickness`,
`u_corner_radius`, and `u_inner_corner_radius` are in **logical pixels** (never
scaled). On HiDPI displays (scale > 1), the inner rect's position and size in the
shader are smaller than the content area, leaving a visible gap that scales with
border width × (scale − 1).

**Initial fix:** Multiply thickness and radius values by `data->scale` in
`scene_entry_render()` before passing them to `wlr_render_rect_options`.

**Precision fix (sub-pixel gaps remain):** The content node's buffer position is
computed by `transform_output_box()` using `round(logical_pos × scale)`, but the
VFX inner rect offset was `border_thickness × scale` (raw, unrounded). At
non-integer scene positions or when `border_thickness × scale` has a fractional
part that rounds differently from the content position, a ±1 pixel gap appears.

**Final fix:** Compute each buffer-precise border offset using `roundf()` to
match the content's rounding exactly:

```c
bt_left = roundf((x_rel + left_logical) × s) - roundf(x_rel × s);
bt_top  = roundf((y_rel + top_logical) × s) - roundf(y_rel × s);
// right side: offset from VFX right edge
bt_right = roundf((x_rel + vw) × s) - roundf((x_rel + vw - right_logical) × s);
// bottom side: offset from VFX bottom edge
bt_bottom = roundf((y_rel + vh) × s) - roundf((y_rel + vh - bottom_logical) × s);
```

This ensures `inner_pos = (gl_FragCoord.xy − u_box.xy) − bt` lands exactly at
the content's buffer origin, eliminating all sub-pixel gaps.

### 8. `WL_OUTPUT_TRANSFORM_FLIPPED_180` projection matrix

The GLES2 render pass creates its projection matrix with
`WL_OUTPUT_TRANSFORM_FLIPPED_180`, not `WL_OUTPUT_TRANSFORM_NORMAL`:
```c
matrix_projection(pass->projection_matrix, wlr_buffer->width, wlr_buffer->height,
    WL_OUTPUT_TRANSFORM_FLIPPED_180);
```
This does **not** actually flip the Y axis (`mat[4] = 2/height`, positive),
so the box coordinates in output space and `gl_FragCoord.xy` are in the same
orientation — both have Y = 0 at the top and Y increasing downward. The
shader's `pos = gl_FragCoord.xy - u_box.xy` works correctly without any
Y-coordinate adjustment.
