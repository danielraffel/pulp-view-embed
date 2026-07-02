# Overlay & resize (foreign-host embedding)

How the Pulp-owned child view coexists with a foreign host's own UI (JUCE,
iPlug2, a bespoke shell) and how to resize it correctly. This is P0.6 of the
JUCE-port accelerator: the two things that most often surprise a host author
are **z-order** (what can draw on top of the embed) and **resize** (who owns the
window geometry and who owns the letterbox math).

Verified on macOS (arm64). The z-order section is AppKit-specific; the resize
recipe is platform-independent (the letterbox math lives in Pulp).

---

## 1. Z-order: the embed is a heavyweight native view

`pulp_embed_native_handle()` returns a real `NSView*` (a Pulp-owned child that
Pulp renders into via a GPU surface or CPU raster). When you insert it into your
host hierarchy — `juce::NSViewComponent::setView`, iPlug2 `IGraphics`, or a raw
`addSubview:` — it becomes a **heavyweight** sibling in the native view tree.

Consequences you must design around:

- **The embed covers any lightweight host UI beneath it.** JUCE's own
  components (its resize grip, tooltips, popup menus, combo-box dropdowns) are
  *lightweight* — they paint into JUCE's single backing `NSView`. A heavyweight
  child `NSView` (the embed) always composits **above** every lightweight JUCE
  component regardless of JUCE's internal z-order. A JUCE component can never
  appear to overlay the embed.
- **Corollary — put host chrome outside the embed's rect.** Anything the host
  must draw on top (a title bar, a resize grip, a "bypass" ribbon, a modal
  scrim) has to live in a region the embed does **not** cover, OR be promoted to
  its own heavyweight `NSView` layered above the embed. Do not expect a
  lightweight JUCE overlay to win.
- **Menus / tooltips.** Host-drawn tooltips and menus that are lightweight will
  be hidden behind the embed. Native (windowed) menus — `NSMenu` popups, real
  child windows — are separate windows and are unaffected. Prefer native popups
  for anything that must appear over the embed, or route the interaction into
  the design itself (the embed's own `dropdown` / `tab_group` / `stepper`
  overlays are drawn by Pulp inside the embed and are always visible).
- **Interaction routing still flows through the ABI.** The embed only sees the
  pointer events the host forwards: hover via `pulp_embed_dispatch_mouse_move` /
  `_exit`, and press/drag/release via `pulp_embed_dispatch_mouse_down` /
  `_drag` / `_up`. If the host consumes a gesture for its own chrome, don't
  forward it; if it should drive an embedded control, forward it in root-view
  logical pixels.

---

## 2. Two supported resize strategies — pick ONE

The embed does not resize your window; the **host** owns the window geometry.
The embed owns only the paint transform inside whatever bounds you give it
(`pulp_embed_resize`). There are exactly two supported strategies.

### (A) Host-window-resizable + locked aspect (recommended for design imports)

The host makes its window resizable but **constrains the drag to the design's
aspect ratio**, then forwards the new bounds to `pulp_embed_resize`. The embed
does the letterbox/uniform-scale math internally (see §4), so the design fills
the window edge-to-edge at any size on the locked aspect with no dark bars.

- Query the aspect + range once from `pulp_embed_size_hints`:
  `preferred_*`, `min_*`, `max_*`, `aspect_ratio`, and the derived `resizable`
  flag (1 when there is a resize range; 0 for a locked design — see the
  `PulpEmbedSizeHints` header doc).
- Apply `aspect_ratio` as the window's content aspect constraint
  (`NSWindow.contentAspectRatio` on macOS; the equivalent min/max-with-ratio on
  other hosts) and clamp to `[min, max]`.
- On every live resize, call `pulp_embed_resize(view, w, h, scale)` with the new
  **logical** width/height.

### (B) Fixed size

The host makes its window non-resizable and sizes it to `preferred_width` ×
`preferred_height`. Call `pulp_embed_resize` once after attach (or rely on the
create size). `size_hints.resizable == 0` designs (future locked designs) should
use this path.

**Do not mix them.** A freely-resizable window with no aspect constraint will
letterbox (the embed centers the design and fills the leftover with the design's
background) — legal, but usually not what you want for a plugin editor.

---

## 3. Lifecycle & teardown order

The embed's lifecycle must stay balanced (open fires once, close fires once).
Ordering matters because the `PluginViewHost` holds a reference to the root
`View` and runs a display-link/idle loop.

**Attach** (pick one mode, never mix per view):

- Mode A (Pulp parents): `pulp_embed_attach(view, parent_nsview)` — Pulp inserts
  its child and fires view-opened on a confirmed attach.
- Mode B (host parents): `pulp_embed_native_handle(view)` → insert into your
  hierarchy → `pulp_embed_notify_attached(view)` (fires view-opened iff the
  child is actually in a native hierarchy).

**Teardown** (reverse order, and the load-bearing detail):

1. Null your wrapper's reference to the native handle **first** (do not release
   it — Pulp owns it). On JUCE, `NSViewComponent::setView(nullptr)` before
   destroying the embed.
2. `pulp_embed_detach(view)` — balances the attach, fires view-closed if it had
   opened.
3. `pulp_embed_destroy(view)` — clears host callbacks, destroys the host (stops
   the render loop, drops the `View&`), closes the bridge (destroys the `View`),
   drops processor/store. NULL-safe; call exactly once.

Destroying while the native handle is still installed in the host hierarchy is
the classic use-after-free: the host still points at a freed `NSView`. Always
detach the wrapper reference before `pulp_embed_destroy`.

## 3a. Reopen-while-zoomed

Plugin editors are opened and closed repeatedly, sometimes while the host window
is zoomed/maximized or at a non-default size:

- The embed handle does **not** persist across `pulp_embed_destroy`. A reopen
  creates a fresh view; re-enumerate params (`pulp_embed_param_*`) and re-seed
  host state (`pulp_embed_param_changed` / `pulp_embed_set_string`) after
  `notify_attached`.
- Re-apply the current window size with `pulp_embed_resize` immediately after
  attach so the first painted frame matches the (possibly zoomed) window rather
  than the create-time logical size.
- The aspect constraint (strategy A) must be re-applied on the new window each
  reopen — it is host-window state, not embed state.
- `size_hints` is stable across reopen for the same design, so cache the
  aspect/range from the first open if you prefer.

---

## 4. Recommended resize recipe

The host constrains the window; the embed already does the letterbox math via
`WindowHost::compute_design_viewport_transform` (the same uniform-fit transform
the design-tool host uses). You do **not** compute scale yourself.

```
on host window will-resize (proposed size S):
    S = clamp(S, size_hints.min, size_hints.max)
    if size_hints.aspect_ratio > 0:
        S = snap S to aspect_ratio        # e.g. NSWindow.contentAspectRatio
    return S                              # host honors the constraint

on host window did-resize (final logical size w x h, backing scale k):
    pulp_embed_resize(view, w, h, k)      # embed re-fits the design into w x h
    # k is advisory for the windowed path — the backing-store DPI comes from the
    # host NSWindow; pass it so DPI-aware hosts can hint a change. Only the
    # deterministic capture APIs (render_png / render_frame_rgba) honor a
    # caller-supplied scale for pixel density.
```

Notes:

- Pin the design viewport at create by setting `desc.design_width` /
  `desc.design_height` to the imported design size. Without it the tree renders
  at native size off-surface (dark fill past the design surface) — the
  documented imported-design host gotcha.
- Because the fit is uniform and centered, an aspect-locked window (strategy A)
  never letterboxes; a free-aspect window (unsupported mix) will.
- The letterbox fill is the design's own background color, not black.
