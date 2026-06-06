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

## What works (v1, macOS)

- `pulp_embed_create_from_design_json[_str]` → parse DesignIR → materialize a
  native view tree → open a `ViewBridge` → create a `PluginViewHost`.
- `pulp_embed_attach` to a host `NSView*` (gated on a real attach via the
  `try_attach_to_parent`/`is_attached` seam, so a failed attach never fires the
  view-opened lifecycle).
- `resize`, `tick`, `repaint`, `size_hints`, `active_backend` (GPU/CPU report).
- `pulp_embed_render_png` — deterministic headless Skia raster (thumbnails/tests).
- `pulp_embed_capture_png` — live GPU back-buffer capture.
- Strict C error model: `PulpEmbedResult` everywhere, `last_error` /
  `last_create_error`, idempotent NULL-safe `destroy` with correct teardown order.

The smoke (`examples/macos-nsview-smoke`) drives the M1.1–M1.5 gates and renders
the bundled fixtures, including the real "VST Style" Figma frame.

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
examples/macos-nsview-smoke/smoke.mm # AppKit host + M1.1–M1.5 gates
fixtures/                            # synthetic + Figma "VST Style" DesignIR
```

Framework adapters (JUCE, iPlug2) live in their own repos and depend only on
`pulp_view_embed.h`.

## License

MIT (matches Pulp).
