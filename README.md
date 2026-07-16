# pulp-view-embed

A flat **C ABI** for embedding a [Pulp](https://github.com/danielraffel/pulp)-imported
frontend (e.g. a design imported from Figma) as a rendered child view inside a
**foreign C++ host** — JUCE, iPlug2, or a bespoke shell — without the host
linking Pulp's C++ ABI.

> Status: **experiment**. macOS is working end to end: high-fidelity render,
> an interactive host↔view parameter bridge (a dragged control moves a host
> parameter; host automation moves the control), a host resource-resolution
> callback, and an offscreen/texture render mode. Not for production yet.

## Status / what works / known limitations / roadmap

**What works (macOS, end to end):**

- Two create paths — high-fidelity importer JS bundle (rasterized images,
  skeuomorphic knobs, glass) and lightweight DesignIR — both opening a
  `ViewBridge` + `PluginViewHost` (GPU Dawn/Skia, CPU fallback).
- Lifecycle: pulp-parents (`attach`) and host-parents (`native_handle` +
  `notify_attached`); `resize` (with validated DPI scale), `tick`, `repaint`,
  `size_hints`, `active_backend`.
- **Interactive parameters (ABI v3):** controls bind bidirectionally to the
  host's parameters by **string key** (UI drag → host gesture/set; host
  automation → control, no feedback loop).
- **Plugin formats:** consumed by real VST3 / AU / CLAP plugins via the JUCE and
  iPlug2 adapter repos (the editor IS the embedded design).
- **Offscreen / texture render mode**, **`resolve_resource`** host-asset
  callback, **font** resolution + portable bundling, and a relocatable
  shared-library + tarball **packaging** story (`DISTRIBUTING.md`).
- Deterministic headless Skia render (`render_png` / `render_frame_rgba`) +
  live GPU back-buffer capture (`capture_png`).
- **Faithful-vector native render (v2):** a `faithful_svg` DesignIR renders the
  frame's own SVG with native SVG-patch knobs (no JS engine) + native overlay
  controls (dropdown / tab group / stepper / text field).
- **Dev hot-reload (v2):** `PULP_EMBED_HOT_RELOAD=1` live-reloads the bundle while
  a host editor is open — edit `ui.js`, save, see it (values preserved). Off by
  default. See **[Editing & hot-reload](#editing--hot-reload-the-dev-loop--no-re-import-per-tweak)**.
- Smoke gates M1.1–M1.11 (create/attach/teardown stress, hi-fi bundle, param
  bridge, resolve_resource, offscreen, and a resize/scale/DPI stress sweep).

**Platforms:** macOS (primary) and **Linux/X11 (proven 2026-06-08)**. On Linux the
Pulp SDK builds + `cmake --install`s, `pulp-view-embed` builds against it, the
preflight renders headless, the iPlug2 adapter's tests pass, and the **live X11
host attaches into a real X11 window and renders GPU-backed** (Dawn → Vulkan
surface from the X11 window; software Vulkan under Xvfb). See `examples/
linux-x11-smoke`. **Windows is blocked** on a prebuilt Windows Skia archive (Pulp
builds GPU-OFF on Windows until one exists; the embed needs Skia).

**Known limitations:**

- **Linux SDK consumers need ICU + fontconfig at link** (system ICU, and
  `-lfontconfig` *after* libskia.a). The installed `PulpConfig` re-finds ICU and
  exposes `pulp_link_fontconfig_after_skia()`; call it on each Skia-linking exe on
  Linux (the examples/adapters do). macOS needs neither.
- The Linux X11 host opens its own connection to the same `$DISPLAY` — fine for
  same-server hosts (the common case); host-supplied `Display*` (cross-Display) is
  a follow-up seam.
- Requires an installed Pulp SDK on `CMAKE_PREFIX_PATH` (the static build cannot
  stand alone; the shared-lib dist is the foreign-host path).
- Requires an installed Pulp SDK on `CMAKE_PREFIX_PATH` (the static build cannot
  stand alone; the shared-lib dist is the foreign-host path).
- `pulp_embed_resize`'s `scale` is validated but advisory for the windowed embed
  (the host NSWindow drives backing DPI); only the capture APIs honor it.
- Zero-copy GPU compositing (IOSurface/MTLTexture handle) is deferred — the
  offscreen path returns CPU RGBA today.

**Resolved design questions** (from the foreign-host-embedding plan):

- *Event-loop tick* — borrowed from the host: the host's display-link (GPU) plus
  a host timer drive `pulp_embed_tick`; the shim runs no loop of its own.
- *Parameter model* — string-key based, which works for both JUCE
  `AudioProcessorParameter` and iPlug2 `IParam` (the host maps its own
  parameters onto the design's keys once at create time).

**Roadmap:** Linux is proven (above) — next a prebuilt Linux SDK tarball +
`pulp add` foreign-host packaging; Windows once a prebuilt Windows Skia archive
exists; zero-copy GPU compositing (watchlist — no consumer yet).

## Why

The host owns the native parent window; Pulp owns a child view and renders into
it. Only opaque handles, POD structs, and result codes cross the boundary
(`include/pulp_view_embed.h`) — no Pulp C++ type, exception, or STL object. The
shim wraps `pulp::format::ViewBridge` + `pulp::view::PluginViewHost` internally.

## Two render paths

| Entry point | Fidelity | Use it for |
|---|---|---|
| `pulp_embed_create_from_ui_bundle(desc, bundle_dir, out)` | **High — pixel-identical to the Pulp importer's own render** (rasterized images, skeuomorphic knobs, glass panels). Renders the importer's `--emit js` bundle through the scripted-UI pipeline (`ScriptedUiSession` + `WidgetBridge`). | Faithfully reproducing a Figma/imported design. **Recommended.** |
| `pulp_embed_create_from_design_json[_str](desc, …, out)` | Lightweight — flat native widgets, drops rasterized images. | A fast, dependency-light approximation. |

Generate the bundle with the importer (its `--emit js` output is `ui.js` + `assets/`):

```bash
pulp import-design --from figma-plugin --file scene.pulp.json --emit js --output bundle/ui.js
pulp_embed_create_from_ui_bundle(&desc, "bundle", &view);   # renders that bundle
```

The importer pre-resolves each asset to an **absolute** filesystem path in
`ui.js` (`setImageSource` / `registerFont` / `setKnobSpriteStrip`). For a
**portable** bundle that loads against its own dir at runtime, rewrite those to
the bundle-relative `assets/<file>` form after import (the embed's path-resolver
preamble resolves the relative form against the bundle dir):

```bash
perl -0pi -e "s{(['\"])(?:/[^'\"]*?/)?assets/([^'\"]+)\1}{\${1}assets/\${2}\${1}}g" bundle/ui.js
```

The committed `fixtures/figma-vst-style/bundle/ui.js` uses this relative form;
the `embed-smoke` ctest reads the first `setImageSource` path as bundle-relative,
so a freshly imported bundle must be portabilized before it round-trips.

## Editing & hot-reload (the dev loop — no re-import per tweak)

Re-importing from Figma/Claude is only for pulling *fresh* design changes. For
day-to-day tweaking the design is just **data on disk** (the bundle's `ui.js` +
`assets/`, or the DesignIR JSON), so you edit that and reload — you do not touch
C++.

### Live hot-reload while a host editor is open

Opt-in, **off by default** so it never ships in a release build. How to use:

1. **Launch the host with the dev flag set.** Any host that embeds via this ABI
   (JUCE/iPlug2 plugin, the standalone, your own app):
   ```bash
   PULP_EMBED_HOT_RELOAD=1 open "Pulp Embed (JUCE).app"
   # or for a plugin, set it in the environment the DAW inherits, or:
   #   PULP_EMBED_HOT_RELOAD=1 /Applications/REAPER.app/Contents/MacOS/REAPER
   ```
2. **Open the plugin/app editor** so the embedded design is on screen.
3. **Edit the bundle's `ui.js`** (or `theme.json`) in your editor and **save**.
4. The open editor **reloads live within a frame or two, preserving widget
   values** — no DAW reload, no re-import, no recompile.

Under the hood: the flag flips on Pulp's `ScriptedUiSession` `HotReloader` (a
background file watcher whose reload is applied on the UI thread via the
`poll()` the embed already pumps every display-link tick). Turning the flag off
(the default) uses the plain load path with no watcher thread.

Notes:
- Dev hot-reload loads the bundle's `ui.js` **directly** (so the file watcher
  watches the file you edit). Use the importer's default **absolute** asset
  paths for the dev loop; the portabilized relative-path bundle uses the
  production asset-resolver wrapper (which the watcher can't see).
- It's gated on `PULP_EMBED_HOT_RELOAD` and uses a background file watcher only
  in that mode — production builds run the plain load path, no watcher thread.
- What's safe to hand-edit in `ui.js`: colors, positions/sizes, text, swap an
  asset in `assets/`. Pulp's `lock_to_source` / `jsx_lock` keep anchored
  hand-edits reconcilable across a later real re-import; `live_constant_editor`
  is the basis for a constrained numeric dev-tweak UI.
- **Programmatic reload (ABI v4):** `pulp_embed_reload_bundle(view, bundle_dir)`
  reloads in place — `bundle_dir = NULL` reloads the current bundle (pick up
  edits), or pass a new dir to swap bundles. Same `PulpEmbedView*`, attach, and
  host callbacks; the param list is rebuilt by key (re-enumerate
  `pulp_embed_param_*` after). Probe-first **last-good** (a broken edit keeps the
  running UI). UI-thread-only (`PULP_EMBED_ERR_WRONG_THREAD` otherwise); scripted
  bundle path only. This is the on-demand counterpart to the file-watched
  `PULP_EMBED_HOT_RELOAD` dev flag — drive it from an in-host editor or a
  debounced adapter file-watcher.
- **Adapter file-watchers (shipped):** `PulpEmbedComponent` (JUCE) and
  `PulpEmbedEditor` (iPlug2) auto-arm a debounced `ui.js` mtime watcher when
  `PULP_EMBED_HOT_RELOAD` is set (on their existing timer) → `reload_bundle` on
  save. `enableBundleHotReload(bool)` forces it.

**Longer-horizon (need a toolchain / infra / bigger design, not quick niceties):**
- Linux/X11 host — **verified** (SDK + embed + adapter build, headless render +
  live X11 attach with GPU/Vulkan; see `examples/linux-x11-smoke`). Windows host —
  blocked on a prebuilt Windows Skia archive (Pulp builds GPU-OFF on Windows).
- `pulp add`-style packaged distribution — needs the core package-registry infra.
- Zero-copy GPU compositing — the offscreen path currently does a CPU RGBA
  readback; eliminating it is a render-pipeline change.
- `text_field` host binding — the ABI parameter model is normalized floats; a
  text control would need a string-parameter ABI surface (it stays in-view
  interactive today).

## What you actually get (plain-English FAQ)

**What is embedded?** A single rendered child view — the imported design,
drawn by Pulp — parented inside your host's window. Your host code talks to it
only through the flat C header `pulp_view_embed.h` (opaque handle + POD structs
+ result codes). You attach it, size it, tick it, and (ABI v3) bind its controls
to your host parameters. That's the whole surface.

**Does it pull in Skia/Dawn, or just C++?** It links the **installed Pulp SDK
statically**, and the SDK brings **Skia (2D GPU rasterizer)** and **Dawn
(WebGPU)** with it transitively (via `Pulp::render`). So your plugin/app binary
grows by the Pulp SDK + Skia + Dawn (tens of MB). You do *not* add Skia/Dawn to
your own build files — `find_package(pulp_view_embed CONFIG)` pulls them in
behind the C ABI. None of those C++ types leak into your translation units; you
include one C header.

**GPU or CPU?** GPU is the goal and the default when available: Dawn (Metal on
macOS) + Skia Graphite render the view, and the host composites Pulp's
layer-hosting Metal view. If the GPU/Skia stack isn't present, it falls back to
a CPU raster path (macOS CoreGraphics) — correct, just no GPU effects and some
image-compositing limits. Both standalone demos run on GPU; the captures in the
adapter repos are live GPU back-buffer reads.

**Is there a JavaScript engine in my binary?** Only on the **high-fidelity
path**. `pulp_embed_create_from_ui_bundle` runs the importer's `--emit js`
bundle through Pulp's scripted-UI pipeline, which uses Pulp's bundled JS engine
(QuickJS by default) — that's what makes it pixel-identical to the importer.
The **DesignIR path** (`pulp_embed_create_from_design_json`) builds native
Pulp widgets with **no JS engine** — lighter, but it drops rasterized images
and fancy effects. Pick per the "Two render paths" table above.

**What parts of Pulp end up in my code?** Functionally: the view/layout engine
(Yoga flex+grid), the canvas + text shaping, the render stack (Skia/Dawn), and —
on the high-fi path — the JS scripted-UI runtime. All static, all behind the C
ABI. Your source only ever sees `pulp_view_embed.h`.

**How do I change the UX later?** You don't edit C++. You re-run the importer on
an updated design (or hand-edit the emitted `ui.js` / DesignIR JSON) and ship the
new bundle — the embed renders whatever bundle you point it at. To make controls
*do* things, bind them to your host parameters by string key through the ABI v3
param bridge (a dragged knob writes your param; host automation pushes values
back). This is the big difference from hand-coding a JUCE/iPlug editor: the
visual design lives in the bundle, not in your C++.

## Supported design imports + roadmap

The embed is **source-agnostic**: it consumes the Pulp importer's *output*
(`--emit ir-json` or `--emit js`), not the design tool directly. So anything
`pulp import-design` can import, the embed can render. Today the importer
supports:

| Source | `--from` | Through the embed? |
|---|---|---|
| Figma (plugin export / REST) | `figma-plugin`, `figma` | ✅ yes (the bundled fixture is a real Figma frame) |
| Claude Design (manual HTML export) | `claude` | ✅ yes — `--emit js` → high-fi bundle |
| Stitch | `stitch` | ✅ yes |
| v0 | `v0` | ✅ yes |
| Pencil | `pencil` | ✅ yes |
| React Native | `react-native` | ✅ yes |
| design.md / JSX runtime | `designmd`, `jsx` | ✅ yes (jsx is experimental) |

What is **not** supported is anything the importer itself can't represent —
Pulp's layout engine is **flex + grid only** (Yoga / React-Native parity), so
designs that depend on CSS block flow, floats, tables, multi-column, or print
pagination are out of scope **by design**, not a missing feature (see Pulp's
`docs/reference/layout-model.md`). Fidelity also tracks the importer: the
high-fi path matches the importer's own render exactly, so any remaining gap
(e.g. a specific knob style) is an importer-workstream item, not an embed
limitation.

**Roadmap:** the source list above is already the importer's; new sources land
in `pulp import-design` upstream and the embed picks them up for free (no embed
change needed). Embed-specific roadmap items:

- **v2 — native vector knobs that match the source design.** The importer
  recently gained a *faithful-vector* render mode (Plan B: pulp #3465
  `DesignFrameView`, #3466 typed IR, #3469/#3470 REST + Figma-plugin emit
  faithful-vector frames, #3309 synthesized SVG paths, #3436/#3444 native knob
  fidelity, #3460/#3453 baked-indicator cleanup). Wiring the embed's DesignIR
  path to that mode gives **native (no-JS) vector knobs** that match the
  original, instead of today's choice between the JS bundle (high-fi but carries
  the JS runtime) and the flat `build_native_view_tree` fallback (drops detail).
- **v2 — interactive controls (search boxes, selects, dropdowns).** The importer
  now emits real interactive fields (pulp #3451: search-box field + dropdown
  precision + control positioning). Surface those through the embed + the ABI v3
  input/param bridge so they behave like they do in the Pulp import preview.
- Linux/X11 host parity — verified (build + headless render + live X11 attach);
  Windows blocked on a prebuilt Windows Skia archive.
- `pulp add`-style packaged distribution.
- Zero-copy GPU compositing (the offscreen path currently does a CPU RGBA readback).

## What works (v1, macOS)

- Both create paths above → open a `ViewBridge` → create a `PluginViewHost` (GPU
  Dawn/Skia, CPU fallback).
- `pulp_embed_attach` to a host `NSView*` (gated on a real attach via the
  `try_attach_to_parent`/`is_attached` seam, so a failed attach never fires the
  view-opened lifecycle), plus host-parents mode (`native_handle` +
  `notify_attached`) for JUCE `NSViewComponent` / iPlug2.
- `resize`, `tick`, `repaint`, `size_hints`, `active_backend` (GPU/CPU report).
- `pulp_embed_render_png` — deterministic headless Skia raster (thumbnails/tests).
- `pulp_embed_capture_png` — live GPU back-buffer capture.
- Strict C error model: `PulpEmbedResult` everywhere, `last_error` /
  `last_create_error`, idempotent NULL-safe `destroy` with correct teardown order.
- **Interactive parameter bridge (ABI v2)** — the design's controls are wired
  bidirectionally to the host's parameters:
  - `PulpEmbedDesc.host` carries the host callbacks (`set_param`, `get_param`,
    `begin_gesture`, `end_gesture`, `read_meters`), each passed the host's
    `host_ctx`.
  - Parameters are addressed by **string key** — the design's `pulpParamKey`
    when present, else the control's widget id. Enumerate them with
    `pulp_embed_param_count` / `pulp_embed_param_key` / `pulp_embed_param_widget_id`
    / `pulp_embed_param_value` and map each key to a host parameter once at
    create time.
  - UI → host: a dragged knob/fader fires `begin_gesture` → `set_param`(s) →
    `end_gesture` (normalized [0,1]); a toggle click fires begin/set/end
    atomically.
  - host → UI: `pulp_embed_param_changed(view, key, normalized)` pushes
    automation/preset values into the control and repaints — without echoing
    back to `set_param` (no feedback loop).
  - Mouse/keyboard already reach the controls through the GPU host's native
    child view (`plugin_view_host_mac.mm` forwards `mouseDown:`/`Dragged:`/`Up:`
    to the same widget handlers the bridge hooks); no extra forwarding needed.
  - `pulp_embed_simulate_param_drag(view, index, target)` drives a control
    through its real interaction path for headless host testing.

### Host resource resolution (ABI v3)

A host can serve a design's assets from memory (an in-memory store, an
encrypted bundle, a project file) instead of from disk by supplying
`PulpEmbedDesc.host.resolve_resource`:

```c
const uint8_t* resolve_resource(void* host_ctx, const char* id, size_t* out_len);
```

The shim offers every asset the design references to this callback **before**
falling back to disk. Return borrowed bytes (valid until creation returns) +
write the byte count, or `NULL` to fall back to loading `id` from disk. The
asset `id` is the path as written in the design:

- bundle path (`pulp_embed_create_from_ui_bundle`): the path in `ui.js`, e.g.
  `assets/<hash>.png`.
- DesignIR path (`pulp_embed_create_from_design_json[_str]`): the manifest
  asset's `local_path`.

Bytes the host serves are staged to a temp file (removed on `destroy`) so the
existing on-disk render path picks them up — the host may free its buffer as
soon as the callback returns. (Image draws decode straight through Skia by
path, so staging is how the host's bytes reach every consumer uniformly.)

### Offscreen / texture render mode (ABI v3)

For a host that composites Pulp's output itself — no Pulp-owned child NSView —
create an offscreen view and pull finished frames on demand:

```c
PulpEmbedResult pulp_embed_create_offscreen(const PulpEmbedDesc* desc,
                                            const char* source,    // IR path or bundle dir
                                            int32_t from_bundle,   // 0 = DesignIR, 1 = bundle
                                            PulpEmbedView** out_view);

PulpEmbedResult pulp_embed_render_frame_rgba(PulpEmbedView* view,
                                             int32_t width, int32_t height, float scale,
                                             uint8_t* out, size_t cap,
                                             int32_t* w, int32_t* h, int32_t* stride);
```

The offscreen view is built through the same materializer / scripted-UI
pipeline, parameter bridge, and `resolve_resource` staging as the windowed
paths — it just has no parent window and no display-link. `render_frame_rgba`
hands back a CPU-readable **RGBA8** frame (R,G,B,A byte order, premultiplied
alpha, sRGB, top-to-bottom, `stride == w * 4`), pixel dimensions
`width*scale × height*scale`, via Pulp's deterministic headless Skia renderer —
so offscreen output matches the windowed embed's high-fidelity render exactly
(the M1.10 gate asserts a 0.00000 pixel diff vs the windowed render). Two-call
sizing pattern: pass `out=NULL` to learn `*w/*h/*stride`, then call again with
`cap >= *stride * *h`. `render_frame_rgba` also works on a windowed view (it
renders deterministically rather than reading a live back buffer — use
`pulp_embed_capture_png` for the live host surface). A zero-copy GPU
texture/IOSurface handle is deferred (see "Scoped out").

The smoke (`examples/macos-nsview-smoke`) drives the M1.1–M1.10 gates: synthetic
+ Figma "VST Style" DesignIR, GPU attach/capture, CPU backend, 100× teardown,
the high-fidelity bundle render, the bidirectional parameter bridge, the
resolve_resource host callback (same-bytes parity + different-bytes control),
and the offscreen render mode (matches the windowed render).

### Idle repaint gate (ABI v9)

`pulp_embed_tick()` repaints every call by default (the historical behaviour).
A host on a 30 Hz timer therefore repaints 30×/s even when nothing on screen is
moving. `pulp_embed_set_dirty_gate(view, 1)` opts into a smarter tick: it repaints
only when the view is actually animating, so a silent editor idles to 0 fps.

```c
pulp_embed_set_dirty_gate(view, 1);   // opt in; pass 0 to restore always-repaint
```

Under the gate, a tick repaints when any of these hold: a `FrameClock`
subscriber is live (a scripted `requestAnimationFrame`/timer, a running CSS
animation, or a meter/scalar value source), a layout pass is pending, or — on
SDKs that export `pulp::view::needs_continuous_frames` — any widget-level
animation (knob hover glow, a time-driven shader). **Discrete changes are never
gated**: the host→view push paths (`pulp_embed_param_changed`, `set_string`, the
mouse dispatchers, `reload`) each repaint on their own, and `pulp_embed_repaint`
always forces a paint.

The one thing to check before enabling it: your live indicators (meters, scopes)
must redraw off a `FrameClock`-backed source rather than relying on the
unconditional per-tick repaint — otherwise they'd stop updating while the editor
is otherwise idle. The predicate is proven by `tools/frame_gate_test.cpp`
(`ctest -R embed-frame-gate`).

### Host step count + live keys (ABI v10)

A design cannot know a host parameter's discreteness: a radio drawn with 3
visible options may be bound to a 6-step parameter, and a control that derives
its value from the number of options it draws addresses the wrong steps. The
host is the authority, so it is asked — `host.host_param_steps(key)`, read back
off the per-tick snapshot (never from paint):

```c
int32_t steps = pulp_embed_param_steps(view, "lfo_waveform");  /* 6 */
/* 0 = CONTINUOUS or UNKNOWN — one answer, deliberately indistinguishable. It
   also covers "no host_param_steps callback" and "not a design control", so
   treat 0 as "do not use a step divisor". A positive value is a step COUNT, not
   a divisor. */
```

A control can also be **re-keyed at run time** (`set_element_param_key` — a paged
rack, a tabbed slot). That is driven from inside the view, so a host has no way
to notice on its own; `pulp_embed_param_key_generation(view)` is the signal. It
bumps on every re-key and every bridge rebuild, so a host gates its
re-enumeration on an integer compare instead of re-reading the whole key set
every tick:

```c
uint64_t gen = pulp_embed_param_key_generation(view);
if (gen != cached_gen) { cached_gen = gen; /* re-enumerate param_count/_key */ }
```

Both are proven by `tools/param_key_test.cpp` (`ctest -R embed-param-key`),
which also asserts that a re-key carries the UI→host writes/gestures and the
host→UI pushes to the new key together.

### Control geometry (ABI v11)

`dispatch_mouse_down/_drag/_up` take root-view coordinates, so a host that wants
to drive a control it can name had no way to aim them: a knob's hit anchor, the
panel crop origin, and the panel→view fit are all private to the view.
`pulp_embed_param_hit_point` closes that — it is the missing half of the dispatch
family, turning an index into the point a pointer must land on.

```c
double x, y;
if (pulp_embed_param_hit_point(view, index, &x, &y) == PULP_EMBED_OK) {
    pulp_embed_dispatch_mouse_down(view, x, y);   /* hit-tests + captures */
    pulp_embed_dispatch_mouse_drag(view, x, y - 20);
    pulp_embed_dispatch_mouse_up(view, x, y - 20);
}
/* PULP_EMBED_ERR_UNSUPPORTED = this control has no locatable geometry (not on a
   design frame, or not laid out yet). No fallback point is invented: a wrong
   coordinate would miss and read as a dead control. Re-read after a resize —
   the point tracks the live layout. */
```

This exists so a drive can keep the **real** gesture path. The older
`pulp_embed_simulate_param_drag` reaches past hit-testing and drives the widget
directly — useful as a bridge-plumbing probe, but it cannot catch a regression in
hit-testing or event routing because it never runs them. Prefer composing
`param_hit_point` + the dispatchers, and **measure** the control's response with
`pulp_embed_param_value` rather than assuming its drag law (the law is per-kind
and is the view's business). Proven by `tools/hit_point_test.cpp`
(`ctest -R embed-hit-point`), which presses each reported point and asserts the
named control — not a neighbour — took the gesture, at a 1:1 fit, a uniform
scale, and a letterboxed aspect.

### Scoped out (this round)

- **Zero-copy GPU compositing** (`IOSurface` / `MTLTexture` handle): deferred.
  Codex's call — ship CPU RGBA readback first; a GPU handle needs a defined
  ownership/synchronization/resize-invalidation contract and a way to advance
  the offscreen scripted/GPU path without a display link.
- Non-macOS offscreen RGBA: `render_to_rgba` is macOS-only for now (the
  non-Apple screenshot stub has no portable raw-pixel producer).

## Build

Requires an **installed Pulp SDK** (built from the embedding seam branch, which
adds the `PluginViewHost` attach-observability seam):

```bash
# In a Pulp checkout on the seam branch:
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPULP_BUILD_TESTS=OFF -DPULP_BUILD_EXAMPLES=OFF
cmake --build build -j
cmake --install build --prefix /path/to/pulp-sdk-install

# Here:
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/path/to/pulp-sdk-install
cmake --build build -j
ctest --test-dir build --output-on-failure   # runs the macOS embed smoke
```

`-DPULP_VIEW_EMBED_SHARED=ON` builds `libpulp_view_embed.dylib` (a stable ABI a
foreign host links without seeing Pulp C++ symbols); default is a static lib.

## Preflight validation (`pulp-embed-validate`)

A **build-time preflight** for a design bundle, run BEFORE it's embedded into a
plugin. It validates the design *at the seam a foreign host consumes it* —
something the plugin-binary validators (`auval` / `pluginval` / `clap-validator`,
and JUCE's `pluginval`) structurally can't: they validate the assembled
`.vst3`/`.component`/`.clap` and understand nothing about a Pulp design. This
complements them; it does not replace them.

```bash
pulp-embed-validate <bundle-dir | design.ir.json> \
  [--design-w N --design-h N] [--scale F] \
  [--host-keys k1,k2,...]   # report bound vs visual-only controls + dangling host keys
  [--out render.png] [--golden ref.png]
```

It checks: the design **parses + materializes** through the embed ABI; every
bindable control **key is non-empty and unique** (keys collide in the host
bridge otherwise); every **render-referenced asset resolves** on disk (the
faithful lane's `svg_asset_id` and fonts, or a bundle's `ui.js` asset literals —
unreferenced fallback rasters are noted, not failed, so it won't false-positive
on a faithful design); and the **deterministic Skia render is non-blank** (with
an optional byte-exact `--golden` compare). Pure C++ / headless (no window or GPU
back-buffer), so it runs in CI and is portable to the Win/Linux hosts. Exit 0 =
all pass, 1 = a check failed, 2 = usage. Wired as the `embed-validate-faithful`
ctest. With `--host-keys` (your adapter's param keys — the same key→param map the
JUCE/iPlug2 binding uses) it reports which design controls actually bind vs stay
visual-only, and host keys with no matching control.

## Distribution

To ship this to a foreign host that does **not** build Pulp from source, see
[`DISTRIBUTING.md`](DISTRIBUTING.md): the cargo-like package manifest
(`pulp-package.json`), the relocatable shared-library dist with a
`find_package(pulp_view_embed)` config and a symbol surface pinned to the C ABI,
the published-SDK tarball recipe (`tools/package-sdk.sh`), and the
codesign/notarize steps.

## Layout

```
include/pulp_view_embed.h            # the only header a foreign host includes
src/pulp_view_embed.cpp              # extern "C" shim over ViewBridge/PluginViewHost
examples/macos-nsview-smoke/         # AppKit host: smoke gates + bundle_render harness
fixtures/figma-vst-style/            # Figma "VST Style": DesignIR + bundle/ (ui.js + assets)
fixtures/synthetic/                  # tiny hand-authored DesignIR
```

Framework adapters (JUCE, iPlug2) live in their own repos and depend only on
`pulp_view_embed.h`.

## License

MIT (matches Pulp).
