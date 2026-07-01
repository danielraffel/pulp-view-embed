/*
 * pulp_view_embed_native.hpp — C++ entry for embedding a HAND-BUILT native Pulp
 * View (not an importer-generated design) in a foreign C++ host.
 *
 * This is a deliberately C++-ONLY surface, separate from the flat C ABI in
 * pulp_view_embed.h. The C ABI's contract is "no Pulp C++ type crosses the
 * boundary" so it can be dlopen'd by any host; it can therefore only mount
 * designs described by POD inputs (a DesignIR JSON path / a ui.js bundle dir).
 * A compiled View is inherently a C++ object, so mounting one requires the host
 * to be C++ and to link this library in the SAME Pulp toolchain (the JUCE /
 * iPlug2 adapters already do — they statically link pulp_view_embed). Keeping
 * this factory out of the C header preserves the C ABI's dlopen guarantee while
 * giving in-toolchain C++ hosts the "bring your own View" lane.
 *
 * Binding: if the factory's tree contains a pulp::view::DesignFrameView whose
 * elements carry a non-empty DesignFrameElement::param_key, those elements bind
 * to host parameters through the SAME string-key↔host bridge the importer lanes
 * use (PulpEmbedDesc::host callbacks + pulp_embed_param_changed). The element's
 * param_key is the host parameter id; no extra glue is needed. Elements with an
 * empty param_key stay interactive but unbound. Two notes on the binding: each
 * param_key should be UNIQUE within the view (the host→UI push resolves a key to
 * a single element); and an xy_pad binds its X axis only (the param is the X
 * value — a 2-axis control needs two params, one per axis). The created view is enumerable
 * and host-pushable exactly like an importer-created one (pulp_embed_param_count
 * / _key / _info, pulp_embed_param_changed, pulp_embed_simulate_param_drag).
 *
 * Threading / ownership: identical to the C create functions — call on the
 * host UI thread; the caller owns the returned handle and destroys it with
 * pulp_embed_destroy().
 */
#ifndef PULP_VIEW_EMBED_NATIVE_HPP
#define PULP_VIEW_EMBED_NATIVE_HPP

#include "pulp_view_embed.h"  // PulpEmbedDesc / PulpEmbedResult / PulpEmbedView

#include <pulp/view/view.hpp>

#include <functional>
#include <memory>

namespace pulp::embed {

// Builds the root View tree once, when the embed opens. Return the owning
// unique_ptr; returning nullptr is reported as PULP_EMBED_ERR_MATERIALIZE
// (same as an empty design). Runs on the host UI thread, synchronously, during
// pulp_embed_create_from_view(). Typically `return std::make_unique<MyEditor>();`
// where MyEditor is a pulp::view::DesignFrameView subclass.
using NativeViewFactory = std::function<std::unique_ptr<pulp::view::View>()>;

// Mount a hand-built native View in the embed (windowed: Pulp owns a child view
// in the host's parent window, exactly like pulp_embed_create_from_design_json).
// On PULP_EMBED_OK *out_view receives the handle; otherwise *out_view is set to
// NULL and pulp_embed_last_create_error() carries detail. `desc` is the same
// descriptor the C create functions take (size, scale, backend_pref, host
// callbacks); the design-import-only fields (asset_base_path) are ignored.
PulpEmbedResult pulp_embed_create_from_view(const PulpEmbedDesc* desc,
                                            NativeViewFactory factory,
                                            PulpEmbedView** out_view);

}  // namespace pulp::embed

#endif  // PULP_VIEW_EMBED_NATIVE_HPP
