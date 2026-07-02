# Compatibility matrix

Three independently-versioned surfaces meet at the embed seam. This file is the
single place that pins which combinations are known-good and documents how an
adapter degrades when the runtime library predates a feature.

- **SDK version** — the Pulp SDK the shim (`libpulp_view_embed`) was **built
  against** (`find_package(Pulp CONFIG REQUIRED)`; `pulp-package.json` records
  the last-verified `pulp_commit`). Determines the internal `pulp::` surface the
  shim needs (e.g. `DesignFrameView::element_param_key`, the native-view mount).
- **Embed ABI** — `PULP_VIEW_EMBED_ABI_VERSION` in `include/pulp_view_embed.h`,
  the number a runtime library reports from `pulp_embed_abi_version()`. This is
  the ONLY version a foreign host must reason about at runtime.
- **Adapter tag** — the git tag / release of a downstream host adapter
  (`pulp-embed-juce`, a future iPlug2/SDL wrapper) that consumes the ABI.

## Matrix

| Embed ABI | Introduced in | Shim built vs SDK | Adapter tag (min) | New surface |
|-----------|---------------|-------------------|-------------------|-------------|
| v2 | pulp-view-embed 0.1.0 | Pulp ≥ 0.332.1 | juce v0.1 | host param bridge (`param_*`, `param_changed`) |
| v3 | 0.1.0 | Pulp ≥ 0.332.1 | juce v0.1 | `resolve_resource`, offscreen + `render_frame_rgba` |
| v5 | 0.1.0 | Pulp ≥ 0.332.1 | juce v0.1 | `param_info` metadata |
| v6 | 0.1.0 | Pulp ≥ 0.332.1 | juce v0.1 | text-field string bridge |
| v7 | 0.1.0 | Pulp ≥ 0.332.1 | juce v0.1 | missing-asset diagnostics |
| v8 | 0.1.0 (this release) | Pulp ≥ 0.550.0 | juce (dynamic-UI) | `has_param` / `param_display_text` snapshot, `host_action`; `dispatch_mouse_down/_drag/_up` |

> The "Shim built vs SDK" column is the SDK the shim needs at **build time**; a
> foreign host linking the prebuilt dylib never sees it. `pulp-package.json`'s
> `verification.pulp_commit` is authoritative for the last-verified pairing.
> ABI v4 (in-place `reload_bundle`) shipped as a function-only addition and is
> folded into the same 0.1.0 line.

## Version skew — the one rule an adapter must follow

The shim accepts a descriptor whose `abi_version` is **≤ the library's own
version** and gates every optional callback on `struct_size` (a smaller struct
from an older caller simply stops before the newer callbacks, which then read as
NULL). `check_desc` **rejects** an `abi_version` GREATER than the library
version — the shim cannot know a future layout. Therefore an adapter must:

```c
desc.abi_version = min(PULP_VIEW_EMBED_ABI_VERSION,  /* header it compiled with */
                       pulp_embed_abi_version());     /* runtime library reports */
desc.struct_size = sizeof(PulpEmbedDesc);
```

- **Header newer than runtime library** (adapter compiled against v8 headers,
  linked to a v7 dylib): clamp `abi_version` to 7. Because the desc struct only
  ever *grows* by tail-append, passing the full `sizeof(PulpEmbedDesc)` is safe —
  the v7 library reads only the prefix it understands and ignores the v8 tail.
  The adapter must then **degrade the dynamic-UI features gracefully**: skip
  wiring `has_param` / `param_display_text` / `host_action` (a v7 library never
  reads them), and do not call `pulp_embed_param_has` /
  `pulp_embed_param_display_text` / `pulp_embed_dispatch_mouse_down|drag|up` —
  they are absent from a v7 dylib's export table and will fail to resolve.
  Feature-detect by comparing `pulp_embed_abi_version()` to the ABI the feature
  was introduced in (see the matrix), NOT by dlsym alone.
- **Header older than runtime library** (adapter compiled against v6, linked to
  a v8 dylib): pass `abi_version = 6`, `struct_size = sizeof(PulpEmbedDesc)` for
  the v6 header. The v8 library accepts it and treats the absent v7/v8 tail as
  NULL. The adapter simply never uses the newer surface. This is the normal
  forward-compat path and needs no special handling.
- **Never hardcode `abi_version = PULP_VIEW_EMBED_ABI_VERSION`** without the
  `min()` against the runtime value — that is the one combination `check_desc`
  rejects (`PULP_EMBED_ERR_INVALID_ARG`) when the runtime library is older.

## Feature-availability quick reference

| Feature | Guard before use |
|---------|------------------|
| Host param bridge | `pulp_embed_abi_version() >= 2` |
| `resolve_resource`, offscreen | `>= 3` |
| `param_info` | `>= 5` |
| String bridge | `>= 6` |
| Missing-asset diagnostics | `>= 7` |
| `has_param` / `param_display_text` snapshot, `host_action`, mouse down/drag/up | `>= 8` |

Distribution + release mechanics (signing, notarization, the prebuilt tarball)
live in [DISTRIBUTING.md](DISTRIBUTING.md).
