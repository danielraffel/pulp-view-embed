# pulp-view-embed

A flat **C ABI** for embedding a [Pulp](https://github.com/danielraffel/pulp)-imported
frontend (e.g. a design imported from Figma) as a rendered child view inside a
**foreign C++ host** — JUCE, iPlug2, or a bespoke shell — without the host
linking Pulp's C++ ABI.

> Status: **experiment**. Render-only v1 (macOS) is working end to end. Not for
> production yet. See `planning/2026-06-06-foreign-host-embedding-revised-plan.md`
> in the Pulp repo for the roadmap.

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

The smoke (`examples/macos-nsview-smoke`) drives the M1.1–M1.7 gates: synthetic +
Figma "VST Style" DesignIR, GPU attach/capture, CPU backend, 100× teardown, and
the high-fidelity bundle render.

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
