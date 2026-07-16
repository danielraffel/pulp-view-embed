// pulp_view_embed.cpp — implementation of the flat C embedding ABI.
//
// Wraps pulp::format::ViewBridge + pulp::view::PluginViewHost over a DesignIR
// materialized into a native view tree. The only Pulp surface that crosses the
// public boundary is opaque (PulpEmbedView*); everything else is POD/C.
//
// Lifetime/teardown ordering is the load-bearing detail: the PluginViewHost
// holds a reference to the root View (owned by ViewBridge) and runs a
// display-link/idle loop. So destroy() must, in order: clear host callbacks,
// destroy the host (stops the loop, drops the View&), then close the bridge
// (destroys the View), then drop processor/store.

#include "pulp_view_embed.h"
#include "pulp_view_embed_native.hpp"  // C++ native-view create entry

#include <pulp/format/processor.hpp>
#include <pulp/format/view_bridge.hpp>
#include <pulp/state/listener_token.hpp>
#include <pulp/state/parameter.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/design_frame_view.hpp>
#include <pulp/view/host_param_surface.hpp>  // HostActionSurface (view->host commands)
#include <pulp/view/text_editor.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/design_ir.hpp>
#include <pulp/view/inspector.hpp>   // ViewInspector::absolute_bounds (drag coords)
#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/scripted_ui.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>

// The two Processor facades, split out for readability (private to this target).
#include "embed_processors.hpp"
#include "frame_gate.hpp"  // embed_view_needs_frame — the opt-in tick dirty gate

#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

// Thread-local detail for creation failures (no handle exists yet).
thread_local std::string g_create_error;

// The two Processor facades live in embed_processors.hpp (split out for
// readability); pull them into this TU's unqualified scope so the create /
// teardown / query paths below name them as before.
using pulp::embed::shim::EmbedNativeViewProcessor;
using pulp::embed::shim::EmbedProcessor;
using pulp::embed::shim::EmbedScriptedProcessor;

// ── Parameter bridge ──────────────────────────────────────────────────────
//
// One ParamBinding per bindable control discovered in the design. The control
// is addressed across the C ABI by its string `key` (the design's pulpParamKey
// when present, else the widget id). Each binding owns a StateStore parameter
// (id == registration index + 1) whose name == key; the StateStore is the
// single source of truth, and the embed mirrors it both into the live widget
// and out to the host.
enum class ParamWidgetKind { knob, fader, toggle };

struct ParamBinding {
    std::string key;             // ABI identity (pulpParamKey or widget id)
    std::string widget_id;       // widget the param drives
    pulp::state::ParamID param_id = 0;
    ParamWidgetKind kind = ParamWidgetKind::knob;
    pulp::view::View* widget = nullptr;  // borrowed; owned by the view tree
    // Faithful-vector binding (v2): when >= 0, `widget` is the DesignFrameView
    // and the param drives its element at this index (knob OR choice control) via
    // its uniform element_value()/set_element_value() instead of a Knob/Fader/
    // Toggle. UI->host is event-driven through DesignFrameView::on_element_changed
    // (wired in build_param_bridge); set_element_value is silent so host pushes
    // don't echo back.
    int   frame_element_index = -1;

    // Richer metadata (ABI v5, pulp_embed_param_info). widget_kind is the design
    // control's real kind ("knob"/"fader"/"toggle"/"dropdown"/"tab_group"/
    // "stepper"); choice controls are discrete with option_count options.
    // default_norm is the imported default [0,1]. `name` is the design caption
    // (§2.1: IRInteractiveElement.label) — empty until the importer carries it,
    // in which case has_meta stays 0 and the host falls back to the key. unit/range
    // remain a later slice.
    std::string widget_kind = "knob";
    bool        is_discrete = false;
    int         option_count = 0;
    float       default_norm = 0.0f;
    std::string name;  // design caption (label); "" -> has_meta 0, fall back to key
};

// Backs the SDK's view-side host-action channel (View::host_actions()) with the
// ABI's host_action callback, so a view calling host_actions()->send_host_action(...)
// reaches the foreign host. Two producers land here: a native view sending
// directly (its author picks which of its buttons are host commands), and a
// DesignFrameView whose action routing is armed — which for an IMPORTED design is
// this shim's job, since that lane has no author to arm it (see build_param_bridge).
//
// The surface must OUTLIVE the open view (HostActionSurface's contract), so it is
// an owned member of PulpEmbedView rather than a local: the view tree is torn
// down in destroy() while the handle, and therefore this, is still alive. It
// holds the callback + ctx directly rather than a PulpEmbedView back-pointer,
// which keeps it independent of that (later-defined) type.
class EmbedHostActionSurface : public pulp::view::HostActionSurface {
public:
    // Re-pointed on every bridge build, so a reload re-installs the live
    // callback rather than stranding the surface on a stale one.
    void configure(PulpEmbedHostActionFn fn, void* ctx) { fn_ = fn; ctx_ = ctx; }

protected:
    bool do_send_host_action(std::string_view action, std::string_view args_json) override {
        if (!fn_) return false;
        // The C ABI takes NUL-terminated strings; a string_view carries no NUL
        // guarantee, so each argument is copied into one. The allocation is safe
        // here BY CONSTRUCTION: host actions fire from the UI thread's mouse
        // handler (never the audio thread, never paint — the SDK's
        // HostActionSurface call-context assert rejects a no-alloc/paint scope
        // outright), so this is off every real-time path.
        const std::string a(action);
        const std::string j(args_json);
        // The int return is diagnostic-only per the ABI contract — the caller
        // must not branch on it — so it is forwarded, never interpreted here.
        return fn_(ctx_, a.c_str(), j.empty() ? nullptr : j.c_str()) != 0;
    }

private:
    PulpEmbedHostActionFn fn_ = nullptr;
    void*                 ctx_ = nullptr;
};

// Backs the SDK's view-side host-parameter surface (View::host_params()).
//
// This exists for ONE answer the view cannot get any other way: the host
// parameter's value COUNT. DesignFrameView scales a choice control by
// param_step_count(key) - 1, and it reaches that only through host_params(). With
// no surface installed the frame falls back to the number of positions the
// control draws, so a 3-option control bound to a 6-value host parameter emits
// idx/2 instead of idx/5 and slams the host to the parameter's last value at the
// third position. The conversion happens INSIDE the frame (choice_to_norm), which
// runs before on_element_changed hands this shim an already-normalized float —
// so the shim cannot correct it after the fact, and the surface is the only seam.
//
// It is deliberately NOT a second write path. The bridge already funnels UI->host
// through store -> param_listener -> host.set_param, and DesignFrameView's own
// routing (route_changes_to_host_params) stays OFF, so the frame never calls
// set_param/begin_gesture/end_gesture here. Those overrides are still implemented
// honestly, via that same store funnel, so the surface tells the truth if the
// routing is ever enabled — and writing through the store keeps ONE funnel rather
// than a parallel one that would bypass the applying_host_change loop-break.
//
// Reads come from the PER-TICK SNAPSHOT, never from the host callbacks directly.
// That is this shim's standing architecture (see snapshot_host_param_surface):
// the paint path must not re-enter the host. It is load-bearing here because
// DesignFrameView::element_value() on a choice element resolves the value count,
// so has_param/param_step_count are reachable from paint — where a host round-trip
// would run arbitrary plugin code mid-render-traversal, and where the SDK's own
// call-context assert already fires.
//
// Like EmbedHostActionSurface it must OUTLIVE the open view, so it is an owned
// member of PulpEmbedView declared before `bridge`. It needs the snapshot and the
// store, so unlike that surface it does hold a back-pointer; the methods are
// therefore defined out-of-line once PulpEmbedView is complete.
class EmbedHostParamSurface : public pulp::view::HostParamSurface {
public:
    // Re-pointed on every bridge build for symmetry with the action surface; the
    // handle is stable, but a rebuild is the moment the snapshot/store behind it
    // are rebuilt too.
    void configure(PulpEmbedView* view) { view_ = view; }

protected:
    bool do_has_param(std::string_view key) override;
    double do_get_param(std::string_view key) override;
    void do_set_param(std::string_view key, double normalized) override;
    void do_begin_gesture(std::string_view key) override;
    void do_end_gesture(std::string_view key) override;
    std::string do_param_display_text(std::string_view key, double normalized) override;
    int do_param_step_count(std::string_view key) override;

private:
    // Resolve a key to its slot in params/the parallel snapshots, or npos.
    size_t slot_for(std::string_view key) const;

    PulpEmbedView* view_ = nullptr;
};

}  // namespace

// The opaque handle. Field order matters for teardown — see destroy().
struct PulpEmbedView {
    std::unique_ptr<pulp::format::Processor> processor;
    std::unique_ptr<pulp::state::StateStore> store;
    // Declared BEFORE `bridge` deliberately: the view tree (owned by `bridge`)
    // holds raw HostActionSurface* / HostParamSurface* pointers handed to it by
    // set_host_actions / set_host_params, and a surface must outlive the open
    // view. Reverse-declaration-order destruction therefore frees the tree first
    // and these second.
    EmbedHostActionSurface host_action_surface;
    EmbedHostParamSurface  host_param_surface;
    std::unique_ptr<pulp::format::ViewBridge> bridge;
    std::unique_ptr<pulp::view::PluginViewHost> host;
    PulpEmbedBackend backend = PULP_EMBED_BACKEND_UNKNOWN;
    pulp::format::ViewSize size_hints{};
    bool opened = false;     // notify_attached() has fired
    bool offscreen = false;  // created via pulp_embed_create_offscreen (no host)
    std::string last_error;

    // Widget captured at dispatch_mouse_down; drag/up replay against it until
    // up clears it (borrowed pointer, owned by the view tree).
    pulp::view::View* drag_target = nullptr;

    // ── periodic-repaint dirty gate (ABI v9) ──
    // OFF by default: pulp_embed_tick() repaints unconditionally, preserving the
    // historical always-repaint behaviour. When a host opts in via
    // pulp_embed_set_dirty_gate(true), tick repaints only when the view needs a
    // frame (embed_view_needs_frame) — a silent editor idles to 0 fps. Discrete
    // host/user changes are unaffected (they repaint on their own push path).
    bool dirty_gate = false;

    // ── host resource staging (ABI v3) ──
    // Temp dir holding host-served asset bytes (resolve_resource), written so the
    // existing on-disk render path loads them. Removed in destroy(). Empty when
    // the host served nothing.
    std::string staging_dir;

    // ── parameter bridge (ABI v2) ──
    PulpEmbedHostCallbacks host_cb{};         // copied from the desc; may be all-NULL
    void* host_ctx = nullptr;
    std::vector<ParamBinding> params;         // stable, registration-ordered
    std::unordered_map<std::string, size_t> key_to_index;
    // Monotonic generation of the KEY SET (pulp_embed_param_key_generation).
    // Bumped by every bridge rebuild and every runtime re-key, so a host can gate
    // its re-enumeration on an integer compare instead of re-reading every key
    // per tick. A re-key originates inside the view, so this is the only signal a
    // host gets that its cached key->parameter bindings went stale.
    uint64_t param_key_generation = 0;
    pulp::state::ListenerToken param_listener; // forwards store changes to host
    // Guard: true while applying a HOST-driven change so the store listener does
    // not bounce the value back out to host.set_param (feedback-loop break).
    bool applying_host_change = false;

    // ── text-field string bridge (ABI v6) ──
    // One entry per bindable text_field; `widget` is the DesignFrameView's overlay
    // TextEditor. key == the text_field's design key (source node id). Strings are
    // host/plugin STATE (not automatable). applying_host_string guards set_text
    // from echoing back to host.set_string.
    struct StringBinding {
        std::string key;
        pulp::view::TextEditor* widget = nullptr;
    };
    std::vector<StringBinding> strings;
    bool applying_host_string = false;

    // ── missing render-asset diagnostics (ABI v7) ──
    // Render-referenced assets (faithful svg_asset_id) that don't exist on disk
    // after asset-path resolution. Computed once at create; an authoritative
    // replacement for a consumer string-scanning the DesignIR JSON itself.
    std::vector<std::string> missing_assets;

    // ── host param-surface snapshot (ABI v8) ──
    // Snapshotted ONCE PER TICK (and at create/reload) from host.has_param /
    // host.param_display_text so the embedded view's paint path never calls back
    // into the host. Both are indexed parallel to `params`. has: -1 unknown (no
    // host has_param callback), else 0/1 membership. display: the host's
    // formatted value string ("500 ms"), empty when the host serves none.
    std::vector<int8_t>      param_has_snapshot;
    std::vector<std::string> param_display_snapshot;
    // The normalized value each param_display_snapshot entry was FORMATTED AT.
    // The text is only meaningful for that value, so this is what a request must
    // match — not the store's live value, which by definition moved on if it
    // disagrees. NaN = no text snapshotted (never matches any request).
    std::vector<double>      param_display_norm_snapshot;

    // ── host param step count (ABI v10) ──
    // Snapshotted with the v8 surface above and indexed parallel to `params`.
    // 0 = continuous or unknown (no host_param_steps callback, or the host
    // reports continuous); a positive entry is the host's step count. A design's
    // own option_count is NOT a substitute — only the host knows the real count.
    std::vector<int32_t> param_steps_snapshot;

    // Thread that created the view. pulp_embed_reload_bundle (which rebuilds views
    // + touches the GPU surface) must run here; a call from another thread is
    // rejected with PULP_EMBED_ERR_WRONG_THREAD. Captured at construction.
    std::thread::id creator_thread = std::this_thread::get_id();
};

namespace {

PulpEmbedResult set_err(PulpEmbedView* v, PulpEmbedResult r, std::string msg) {
    if (v) v->last_error = std::move(msg);
    return r;
}

// ── EmbedHostParamSurface ──────────────────────────────────────────────────
// Defined here rather than at the class body because every override reads
// PulpEmbedView, which is only complete above.

size_t EmbedHostParamSurface::slot_for(std::string_view key) const {
    if (!view_ || key.empty()) return static_cast<size_t>(-1);
    // key_to_index is keyed by std::string; the heterogeneous lookup this would
    // need is not enabled on it, so the view is copied into one. Off every
    // real-time path by the surface's own call-context contract (tick/update
    // only, never paint, never audio).
    const auto it = view_->key_to_index.find(std::string(key));
    if (it == view_->key_to_index.end()) return static_cast<size_t>(-1);
    // key_to_index is rebuilt FROM params, so this holds by construction; it is
    // asserted here anyway so every params[slot] below is unconditionally safe
    // rather than safe-by-argument.
    if (it->second >= view_->params.size()) return static_cast<size_t>(-1);
    return it->second;
}

bool EmbedHostParamSurface::do_has_param(std::string_view key) {
    const size_t slot = slot_for(key);
    if (slot == static_cast<size_t>(-1)) return false;  // not a design control
    if (slot >= view_->param_has_snapshot.size()) return false;
    const int8_t answer = view_->param_has_snapshot[slot];
    // -1 is "the host wired no has_param callback", NOT "unbound". The key is a
    // control the bridge bound, and the host has volunteered no opinion to the
    // contrary, so the binding stands. Reporting false here would be the quiet
    // failure this surface exists to remove: DesignFrameView gates the step-count
    // lookup on has_param, so a host that wires host_param_steps but not has_param
    // would silently keep scaling choice controls by what they draw.
    return answer != 0;
}

double EmbedHostParamSurface::do_get_param(std::string_view key) {
    const size_t slot = slot_for(key);
    if (slot == static_cast<size_t>(-1) || !view_->store) return 0.0;
    // The store mirrors the host's value (pulp_embed_param_changed pushes into
    // it), so this answers from local state rather than re-entering the host.
    return static_cast<double>(view_->store->get_normalized(view_->params[slot].param_id));
}

void EmbedHostParamSurface::do_set_param(std::string_view key, double normalized) {
    const size_t slot = slot_for(key);
    if (slot == static_cast<size_t>(-1) || !view_->store) return;
    // Through the store, not straight to host_cb.set_param: the store listener is
    // the bridge's single UI->host funnel and carries the applying_host_change
    // loop-break. A direct callback here would be a second, unguarded path.
    view_->store->set_normalized(view_->params[slot].param_id,
                                 static_cast<float>(normalized));
}

void EmbedHostParamSurface::do_begin_gesture(std::string_view key) {
    const size_t slot = slot_for(key);
    if (slot == static_cast<size_t>(-1) || !view_->store) return;
    view_->store->begin_gesture(view_->params[slot].param_id);
}

void EmbedHostParamSurface::do_end_gesture(std::string_view key) {
    const size_t slot = slot_for(key);
    if (slot == static_cast<size_t>(-1) || !view_->store) return;
    view_->store->end_gesture(view_->params[slot].param_id);
}

std::string EmbedHostParamSurface::do_param_display_text(std::string_view key,
                                                         double normalized) {
    // The snapshot holds the host's text for the value the parameter held at the
    // last tick, which is what a view asking to render "the current value" wants.
    // A request for the text of some OTHER normalized value cannot be served from
    // it, and answering it would mean calling the host's formatter — arbitrary
    // plugin code — off the snapshot cadence this shim is built around. Report the
    // cached text only when the request matches the value that text was FORMATTED
    // AT, which is the only value it describes.
    //
    // Matching against the snapshot's own value is load-bearing, not bookkeeping.
    // The live store is NOT the comparand: the sole caller passes get_param(),
    // which reads that same live store, so comparing against it always matches and
    // the guard degrades to a no-op that hands back whatever text the last tick
    // left. When the store moves between ticks the snapshot goes stale, and a
    // no-op guard would render the PREVIOUS value's text as though it were this
    // value's — a readout confidently displaying the wrong number.
    //
    // A mismatch returns empty, which the contract already defines as "host serves
    // none". The two are deliberately not distinguished: the surface returns a
    // std::string, so "" is the only "no answer" it has, and both cases mean the
    // same thing to a caller — no text is available for this value right now. The
    // next tick re-snapshots and the text returns.
    const size_t slot = slot_for(key);
    if (slot == static_cast<size_t>(-1)) return {};
    if (slot >= view_->param_display_snapshot.size()) return {};
    if (slot >= view_->param_display_norm_snapshot.size()) return {};
    const double snapped = view_->param_display_norm_snapshot[slot];
    // Written as a positive test so a NaN "nothing snapshotted" fails it: every
    // comparison against NaN is false, so the guard refuses rather than serving a
    // stale entry.
    if (!(std::fabs(snapped - normalized) <= 1e-6)) return {};
    return view_->param_display_snapshot[slot];
}

int EmbedHostParamSurface::do_param_step_count(std::string_view key) {
    const size_t slot = slot_for(key);
    // 0 is the contract's single "no index domain / cannot answer" answer, and
    // every miss collapses to it: an unknown key, a host that wired no
    // host_param_steps (or one gated out by an older struct_size), and a
    // genuinely continuous parameter. The snapshot already clamped a negative
    // host answer to 0, so a caller can never see a second don't-know value.
    if (slot == static_cast<size_t>(-1)) return 0;
    if (slot >= view_->param_steps_snapshot.size()) return 0;
    return static_cast<int>(view_->param_steps_snapshot[slot]);
}

// Validate + normalize the descriptor. Returns PULP_EMBED_OK or an error.
//
// abi_version is accepted when it is <= the library's version: a v1 caller hands
// a smaller struct (no host-bridge tail) and a v2 library reads that absent tail
// as all-NULL. A caller from the FUTURE (abi_version greater than ours) is
// rejected — we can't know its layout. struct_size gates how much of the desc
// we may read.
PulpEmbedResult check_desc(const PulpEmbedDesc* desc) {
    if (!desc) return PULP_EMBED_ERR_INVALID_ARG;
    if (desc->abi_version == 0 || desc->abi_version > PULP_VIEW_EMBED_ABI_VERSION)
        return PULP_EMBED_ERR_INVALID_ARG;
    if (desc->struct_size < sizeof(uint32_t) * 2) return PULP_EMBED_ERR_INVALID_ARG;
    if (desc->logical_width <= 0 || desc->logical_height <= 0) return PULP_EMBED_ERR_INVALID_ARG;
    return PULP_EMBED_OK;
}

// Copy the desc's host-callback block into v, gating EACH field on the caller's
// struct_size so older callers stay supported as the block grows:
//   - A v1 caller (struct stops before `host`) leaves host_cb all-NULL.
//   - A v2 caller carries the original five callbacks but not resolve_resource.
//   - A v3 caller carries resolve_resource too.
// We read up to whatever prefix of `host` the caller's struct_size covers.
void capture_host_callbacks(PulpEmbedView* v, const PulpEmbedDesc* desc) {
    v->host_ctx = desc->host_ctx;
    // Reachable bytes of the trailing `host` member, clamped to its real size.
    const size_t host_off = offsetof(PulpEmbedDesc, host);
    if (desc->struct_size <= host_off) return;  // no host block at all
    size_t avail = desc->struct_size - host_off;
    if (avail > sizeof(PulpEmbedHostCallbacks)) avail = sizeof(PulpEmbedHostCallbacks);
    // Copy only the prefix the caller actually provided; the rest stays NULL.
    std::memcpy(&v->host_cb, &desc->host, avail);
}

// Rebuild key_to_index from the CURRENT ParamBinding::key values.
//
// The map is DERIVED state: it is only ever a projection of ParamBinding::key,
// so any code path that changes a binding's key must update the binding FIRST
// and call this second. (Rebuilding it from unchanged binding keys is a no-op.)
//
// A param key must be unique within a view — the host->UI push resolves a key to
// a single element, so a duplicate makes one element permanently unreachable and
// silently misroutes host pushes to the other. pulp_view_embed_native.hpp
// declares that uniqueness as a caller contract; this records a violation in
// last_error instead of losing it, so a host that breaks the contract can see
// why its control went dead. The last binding wins (insertion order).
void rebuild_key_index(PulpEmbedView* v) {
    v->key_to_index.clear();
    std::string dupes;
    for (size_t i = 0; i < v->params.size(); ++i) {
        const auto& key = v->params[i].key;
        if (!v->key_to_index.insert_or_assign(key, i).second) {
            if (!dupes.empty()) dupes += ", ";
            dupes += key;
        }
    }
    if (!dupes.empty())
        v->last_error = "param key bound to more than one element: " + dupes +
                        " (keys must be unique per view; the last element wins)";
}

// Forward declarations — the param-bridge builders are defined further down
// (next to the host/session wiring) but the create paths above reference them.
void build_param_bridge(PulpEmbedView* v);
void build_string_bridge(PulpEmbedView* v);
void collect_missing_render_assets(PulpEmbedView* v);
void poll_host_meters(PulpEmbedView* v);
void snapshot_host_param_surface(PulpEmbedView* v);
void snapshot_host_param_at(PulpEmbedView* v, size_t i);

// ── host resource staging (resolve_resource, ABI v3) ───────────────────────
//
// The host can serve an asset's bytes by id via desc->host.resolve_resource.
// Rather than reach into every image/font/canvas decode site (which load by
// path through SkData::MakeFromFileName, NOT a hookable AssetManager), the shim
// stages served bytes to a temp dir and points the existing on-disk path at
// them. Disk fallback is automatic: when the callback returns NULL we leave the
// original path untouched, and the renderer loads it from the bundle/IR dir.

// Create (once) a unique temp dir for this view's staged resources. Returns the
// path, or empty on failure (callers then skip staging and fall back to disk).
std::string ensure_staging_dir(PulpEmbedView* v) {
    if (!v->staging_dir.empty()) return v->staging_dir;
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec);
    if (ec) return {};
    // A per-view dir keyed by address + clock keeps concurrent embeds isolated.
    const auto uniq = std::to_string(reinterpret_cast<uintptr_t>(v)) + "-" +
                      std::to_string(static_cast<unsigned long long>(
                          std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path dir = base / ("pulp-embed-res-" + uniq);
    fs::create_directories(dir, ec);
    if (ec) return {};
    v->staging_dir = dir.string();
    return v->staging_dir;
}

// Ask the host for `id`'s bytes; if served, write them under the staging dir at
// the SAME relative layout as `id` (so a path-based loader finds them) and
// return the absolute staged path. Returns empty when the host serves nothing
// (NULL) — the caller then keeps the disk path. `id` is the asset's design
// identifier (the path as written in ui.js / the manifest local_path).
std::string stage_host_resource(PulpEmbedView* v, const std::string& id) {
    if (!v || !v->host_cb.resolve_resource || id.empty()) return {};
    size_t len = 0;
    const uint8_t* bytes = v->host_cb.resolve_resource(v->host_ctx, id.c_str(), &len);
    if (!bytes || len == 0) return {};  // disk fallback

    const std::string dir = ensure_staging_dir(v);
    if (dir.empty()) return {};

    namespace fs = std::filesystem;
    // Preserve `id`'s relative shape under the staging root. An absolute id is
    // reduced to its filename so it still lands inside the staging dir.
    fs::path rel(id);
    if (rel.is_absolute()) rel = rel.filename();
    const fs::path out = fs::path(dir) / rel;
    std::error_code ec;
    fs::create_directories(out.parent_path(), ec);
    std::ofstream of(out, std::ios::binary | std::ios::trunc);
    if (!of) return {};
    of.write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(len));
    of.close();
    if (!of) return {};
    return out.lexically_normal().string();
}

// Scan a ui.js source for the asset paths it references (the path argument of
// setImageSource / setKnobSpriteStrip) and offer each to the host's
// resolve_resource. For every id the host serves, stage the bytes and record an
// id -> staged-absolute-path override. Returns the override list (empty when the
// host serves nothing or has no resolve_resource). The id offered to the host is
// the path EXACTLY as written in ui.js, matching the resolve_resource contract.
std::vector<std::pair<std::string, std::string>>
stage_bundle_resources(PulpEmbedView* v, const std::string& ui_js) {
    std::vector<std::pair<std::string, std::string>> overrides;
    if (!v || !v->host_cb.resolve_resource) return overrides;

    // Match the SECOND string argument (the path) of setImageSource(id, path)
    // and setKnobSpriteStrip(id, path, ...). Quotes may be ' or ".
    static const std::regex re(
        R"((?:setImageSource|setKnobSpriteStrip)\s*\(\s*['"][^'"]*['"]\s*,\s*['"]([^'"]+)['"])");
    std::unordered_map<std::string, std::string> seen;  // id -> staged (dedup)
    for (std::sregex_iterator it(ui_js.begin(), ui_js.end(), re), end; it != end; ++it) {
        const std::string id = (*it)[1].str();
        if (id.empty() || seen.count(id)) continue;
        std::string staged = stage_host_resource(v, id);
        seen[id] = staged;  // cache even empties so we don't re-offer
        if (!staged.empty()) overrides.emplace_back(id, staged);
    }
    return overrides;
}

// Rewrite relative asset/font local_paths to absolute against base_dir so the
// materializer can load rasterized images (e.g. the figma export's assets/*.png)
// regardless of the process CWD. DesignIR JSON stores local_path relative to the
// IR file; without this, ImageViews fail to load and the design renders without
// its bitmap content. No-op when base_dir is empty or the path is already absolute.
// `v` (may be NULL) carries the optional resolve_resource host callback: for
// each asset whose bytes the host serves (keyed by the asset's local_path as
// written in the IR), the served bytes are staged and local_path is pointed at
// the staged file BEFORE the disk-relative rewrite, so the host wins over disk.
void resolve_asset_paths(PulpEmbedView* v, pulp::view::DesignIR& ir,
                         const std::string& base_dir) {
    namespace fs = std::filesystem;
    const bool have_base = !base_dir.empty();
    const fs::path base(base_dir);
    for (auto& asset : ir.asset_manifest.assets) {
        if (!asset.local_path || asset.local_path->empty()) continue;
        // Host-served bytes take precedence over disk (id == the IR local_path).
        std::string staged = stage_host_resource(v, *asset.local_path);
        if (!staged.empty()) { asset.local_path = staged; continue; }
        // Disk fallback: resolve a relative path against the IR directory.
        fs::path p(*asset.local_path);
        if (have_base && p.is_relative())
            asset.local_path = (base / p).lexically_normal().string();
    }
    // Bundled fonts reference their file through the asset manifest (asset_id ->
    // IRAssetRef.local_path, resolved above); resolved_path, when set, is already
    // absolute. So no separate font-path rewrite is needed here.
}

// Shared create tail for every lane (DesignIR, ui.js bundle, native View): open
// the ViewBridge over v->processor, lay out the first frame, attach a
// PluginViewHost (or stay offscreen), build the host-param bridge, and apply the
// optional design viewport. On entry v->processor / v->offscreen must be set,
// host callbacks captured, and v->size_hints filled. Leaves v owned by the
// caller (no release) so a lane can run its own extra steps — e.g. the DesignIR
// lane's missing-asset scan — before publishing the handle. Returns OK or the
// specific failure code (g_create_error carries detail).
PulpEmbedResult open_and_bridge(PulpEmbedView* v, const PulpEmbedDesc* desc,
                                uint32_t w, uint32_t h) {
    v->store = std::make_unique<pulp::state::StateStore>();
    v->bridge = std::make_unique<pulp::format::ViewBridge>(*v->processor, *v->store);

    std::string err;
    if (!v->bridge->open(&err)) {
        g_create_error = "view open failed: " + err;
        return PULP_EMBED_ERR_VIEW_OPEN;
    }
    if (!v->bridge->view()) {
        g_create_error = "materialized view tree is empty";
        return PULP_EMBED_ERR_MATERIALIZE;
    }

    // A freshly built tree has no laid-out bounds; give the root the logical
    // size and run Yoga so the first frame paints (the host renders the tree but
    // does not itself lay out a guest view).
    if (auto* rv = v->bridge->view()) {
        rv->set_bounds({0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)});
        rv->layout_children();
    }

    if (!v->offscreen) {
        pulp::view::PluginViewHost::Options opts;
        opts.size = {w, h};
        opts.use_gpu = (desc->backend_pref != PULP_EMBED_BACKEND_PREF_CPU);
        v->host = pulp::view::PluginViewHost::create(*v->bridge->view(), opts);
        if (!v->host) {
            g_create_error = "no PluginViewHost (missing platform factory?)";
            return PULP_EMBED_ERR_HOST_CREATE;
        }
        v->backend = v->host->is_gpu_backed() ? PULP_EMBED_BACKEND_GPU
                                              : PULP_EMBED_BACKEND_CPU;
    } else {
        // Offscreen: no live host. Frames come from the deterministic renderer.
        v->backend = PULP_EMBED_BACKEND_CPU;
    }

    // Interactive parameter bridge (ABI v2): Knob/Fader/Toggle widgets (DesignIR
    // tree), DesignFrameView elements (faithful + native lanes), and text fields.
    build_param_bridge(v);

    if (!v->offscreen && desc->design_width > 0 && desc->design_height > 0) {
        v->host->set_design_viewport(static_cast<float>(desc->design_width),
                                     static_cast<float>(desc->design_height));
    }
    return PULP_EMBED_OK;
}

// Shared create path over an already-loaded JSON string. asset_base_dir is the
// directory relative asset paths resolve against (the IR file's dir, or
// desc->asset_base_path for the in-memory variant). When `offscreen` is true the
// view is built fully but no PluginViewHost is created — the host pulls frames
// via pulp_embed_render_frame_rgba instead of attaching a native child.
PulpEmbedResult create_from_json(const PulpEmbedDesc* desc,
                                 const std::string& json,
                                 const std::string& asset_base_dir,
                                 bool offscreen,
                                 PulpEmbedView** out_view) {
    if (out_view) *out_view = nullptr;
    g_create_error.clear();
    if (auto r = check_desc(desc); r != PULP_EMBED_OK) {
        g_create_error = "invalid descriptor";
        return r;
    }
    if (!out_view) return PULP_EMBED_ERR_INVALID_ARG;

    auto v = std::make_unique<PulpEmbedView>();
    v->offscreen = offscreen;
    // Capture host callbacks up front so resolve_resource can stage assets
    // BEFORE the materializer loads them from disk.
    capture_host_callbacks(v.get(), desc);

    pulp::view::DesignIR ir;
    try {
        ir = pulp::view::parse_design_ir_json(json);
    } catch (const std::exception& e) {
        g_create_error = std::string("DesignIR parse failed: ") + e.what();
        return PULP_EMBED_ERR_PARSE;
    }
    resolve_asset_paths(v.get(), ir, asset_base_dir);

    const auto w = static_cast<uint32_t>(desc->logical_width);
    const auto h = static_cast<uint32_t>(desc->logical_height);
    v->size_hints = pulp::format::view_size_from_design(w, h);

    v->processor = std::make_unique<EmbedProcessor>(std::move(ir), v->size_hints);

    if (auto r = open_and_bridge(v.get(), desc, w, h); r != PULP_EMBED_OK)
        return r;

    // Missing-asset diagnostics (ABI v7): walk the DesignIR asset manifest and
    // record any svg/image whose resolved local_path is absent on disk, so the
    // host preflight can query it authoritatively instead of re-parsing JSON.
    collect_missing_render_assets(v.get());

    *out_view = v.release();
    return PULP_EMBED_OK;
}

// Wire the host's live GpuSurface into the scripted-UI session's WidgetBridge
// and install the per-vsync idle pump. This is the load-bearing handoff for
// scripted/GPU content: without the surface, the JS-side navigator.gpu /
// canvas.getContext('webgpu') bridge returns mocks and any JS-rendered/canvas
// output is black (the threejs-bridge "gpu_surface MUST be passed to
// WidgetBridge" gotcha). For a native-widget-bridge design (rasterized images
// + skeuo knobs, no live 3D) the surface is harmless but correct to attach.
// Idempotent: a null surface (CPU host) is a safe detach.
void wire_scripted_session_to_host(PulpEmbedView* v) {
    if (!v || !v->bridge || !v->host) return;
    auto* scripted = v->bridge->scripted_ui();
    if (!scripted) return;
    scripted->attach_gpu_surface(v->host->gpu_surface());
    // Pump the session's poll() once per display-link tick so timers /
    // requestAnimationFrame / async results keep running while embedded.
    v->host->set_idle_callback([scripted]() {
        std::string err;
        scripted->poll(&err);
    });
}

// ── parameter bridge wiring ────────────────────────────────────────────────

// Recursively classify a view as a bindable control. Returns true + writes the
// kind when the view is a Knob / Fader / Toggle, false otherwise.
bool classify_bindable(pulp::view::View* w, ParamWidgetKind* out_kind) {
    if (dynamic_cast<pulp::view::Knob*>(w))   { *out_kind = ParamWidgetKind::knob;   return true; }
    if (dynamic_cast<pulp::view::Fader*>(w))  { *out_kind = ParamWidgetKind::fader;  return true; }
    if (dynamic_cast<pulp::view::Toggle*>(w)) { *out_kind = ParamWidgetKind::toggle; return true; }
    return false;
}

// Read a control's current normalized [0,1] value.
float widget_normalized(const ParamBinding& b) {
    if (b.frame_element_index >= 0) {  // faithful-vector SVG-patch knob
        float v = static_cast<pulp::view::DesignFrameView*>(b.widget)
                      ->element_value(b.frame_element_index);
        return v < 0.0f ? 0.0f : v;    // element_value returns -1 for bad index
    }
    switch (b.kind) {
        case ParamWidgetKind::knob:
            return static_cast<pulp::view::Knob*>(b.widget)->value();
        case ParamWidgetKind::fader:
            return static_cast<pulp::view::Fader*>(b.widget)->value();
        case ParamWidgetKind::toggle:
            return static_cast<pulp::view::Toggle*>(b.widget)->is_on() ? 1.0f : 0.0f;
    }
    return 0.0f;
}

// Apply a normalized [0,1] value to a control (programmatic; does NOT fire the
// widget's on_change — see widgets.cpp set_value / set_on, which only repaint).
void widget_set_normalized(const ParamBinding& b, float v) {
    if (b.frame_element_index >= 0) {  // faithful-vector SVG-patch knob
        static_cast<pulp::view::DesignFrameView*>(b.widget)
            ->set_element_value(b.frame_element_index, v);
        return;
    }
    switch (b.kind) {
        case ParamWidgetKind::knob:
            static_cast<pulp::view::Knob*>(b.widget)->set_value(v); break;
        case ParamWidgetKind::fader:
            static_cast<pulp::view::Fader*>(b.widget)->set_value(v); break;
        case ParamWidgetKind::toggle:
            static_cast<pulp::view::Toggle*>(b.widget)->set_on(v > 0.5f); break;
    }
}

// First DesignFrameView in the tree (the faithful-vector frame), or nullptr.
pulp::view::DesignFrameView* find_design_frame_view(pulp::view::View* v) {
    if (!v) return nullptr;
    if (auto* f = dynamic_cast<pulp::view::DesignFrameView*>(v)) return f;
    for (size_t i = 0; i < v->child_count(); ++i)
        if (auto* f = find_design_frame_view(v->child_at(i))) return f;
    return nullptr;
}

// Walk the view tree depth-first, collecting bindable controls in document
// order so the param index is stable and matches the design's reading order.
void collect_bindable(pulp::view::View* v, std::vector<ParamBinding>& out) {
    if (!v) return;
    ParamWidgetKind kind;
    if (!v->id().empty() && classify_bindable(v, &kind)) {
        ParamBinding b;
        b.widget_id = v->id();
        b.key = v->id();        // default key == widget id (no metadata in ui.js)
        b.kind = kind;
        b.widget = v;
        // Native-widget metadata (ABI v5): a toggle is a 2-option discrete; knob/
        // fader are continuous. default_norm is filled from the widget's seeded
        // value in the bind loop below.
        switch (kind) {
            case ParamWidgetKind::knob:   b.widget_kind = "knob";   break;
            case ParamWidgetKind::fader:  b.widget_kind = "fader";  break;
            case ParamWidgetKind::toggle:
                b.widget_kind = "toggle"; b.is_discrete = true; b.option_count = 2; break;
        }
        out.push_back(std::move(b));
    }
    for (size_t i = 0; i < v->child_count(); ++i)
        collect_bindable(v->child_at(i), out);
}

// Install the UI->host hooks on one control. Composes with (does not clobber)
// any on_change the WidgetBridge already installed for JS dispatch.
void wire_widget_to_host(PulpEmbedView* v, const ParamBinding& b) {
    auto* store = v->store.get();
    const pulp::state::ParamID pid = b.param_id;

    if (b.kind == ParamWidgetKind::knob || b.kind == ParamWidgetKind::fader) {
        auto prev_change = (b.kind == ParamWidgetKind::knob)
            ? static_cast<pulp::view::Knob*>(b.widget)->on_change
            : static_cast<pulp::view::Fader*>(b.widget)->on_change;

        auto on_change = [v, store, pid, prev_change](float val) {
            if (prev_change) prev_change(val);  // keep JS dispatch alive
            // UI-driven write: store is the source of truth, and its listener
            // forwards to host.set_param (applying_host_change is false here).
            store->set_normalized(pid, val);
        };
        auto on_begin = [store, pid]() { store->begin_gesture(pid); };
        auto on_end   = [store, pid]() { store->end_gesture(pid); };

        if (b.kind == ParamWidgetKind::knob) {
            auto* k = static_cast<pulp::view::Knob*>(b.widget);
            k->on_change = on_change;
            k->on_gesture_begin = on_begin;
            k->on_gesture_end = on_end;
        } else {
            auto* f = static_cast<pulp::view::Fader*>(b.widget);
            f->on_change = on_change;
            f->on_gesture_begin = on_begin;
            f->on_gesture_end = on_end;
        }
    } else {  // toggle — no gesture phases; click = begin/set/end atomically
        auto* t = static_cast<pulp::view::Toggle*>(b.widget);
        auto prev = t->on_toggle;
        t->on_toggle = [v, store, pid, prev](bool on) {
            if (prev) prev(on);
            store->begin_gesture(pid);
            store->set_normalized(pid, on ? 1.0f : 0.0f);
            store->end_gesture(pid);
        };
    }
}

// Build the parameter registry: discover bindable controls, register one
// StateStore param per control (name == key), seed it from the widget, wire the
// UI->host hooks, and install a single store listener that forwards UI-driven
// value changes to host.set_param. Gesture begin/end forward through
// StateStore::set_gesture_callbacks. Idempotent per view (called once at create).
void build_param_bridge(PulpEmbedView* v) {
    if (!v || !v->store || !v->bridge) return;
    auto* root = v->bridge->view();
    if (!root) return;

    // A rebuild (pulp_embed_reload_bundle) frees the old widget tree. Any drag
    // capture into it must be dropped here, or a subsequent dispatch_mouse_drag/
    // _up would deref a freed widget (heap-use-after-free in a hot-reload dev
    // loop). This is the common chokepoint for create AND reload; on first
    // create drag_target is already null, so clearing it is a harmless no-op.
    v->drag_target = nullptr;

    // Reload-safe: a rebuild (pulp_embed_reload_bundle) must reuse the StateStore
    // params that already exist for a key — the store has no remove API, and the
    // host binds by key. Snapshot key->param_id, drop the old widget bindings +
    // listener, then re-collect; existing keys reuse their store param, new keys
    // allocate a fresh id past the current max. (First call: prev is empty, so
    // ids are 1..N exactly as before.)
    std::unordered_map<std::string, pulp::state::ParamID> prev;
    pulp::state::ParamID max_id = 0;
    for (const auto& b : v->params) {
        prev[b.key] = b.param_id;
        if (b.param_id > max_id) max_id = b.param_id;
    }
    v->param_listener.reset();
    v->params.clear();
    v->key_to_index.clear();

    collect_bindable(root, v->params);

    // Faithful-vector lane (v2): DesignFrameView's elements (SVG-patch knobs +
    // native overlay dropdown/tab/stepper) aren't Knob/Fader/Toggle widgets, so
    // collect_bindable misses them. Append one binding per value-bearing element,
    // keyed by the importer's source node id, targeting the frame view by element
    // index (read/written via DesignFrameView's uniform element_value/
    // set_element_value). The frame's on_element_changed / gesture callbacks are
    // wired event-driven at the end of this function. (text_field is text, not a
    // normalized param — skipped, still in-view interactive.)
    if (auto* ep = dynamic_cast<EmbedProcessor*>(v->processor.get())) {
        if (auto* frame = find_design_frame_view(root)) {
            const auto keys = ep->faithful_element_keys();
            const auto metas = ep->faithful_element_metas();  // same order/size
            for (size_t k = 0; k < keys.size(); ++k) {
                ParamBinding b;
                b.key = keys[k].second;
                b.widget_id = keys[k].second;
                b.kind = ParamWidgetKind::knob;  // value carried via frame element
                b.widget = frame;
                b.frame_element_index = keys[k].first;
                // ABI v5 metadata from the IRInteractiveElement (kind/discrete/
                // option_count/default) — choice controls keep their real kind.
                if (k < metas.size()) {
                    b.widget_kind = metas[k].kind;
                    b.is_discrete = metas[k].is_discrete;
                    b.option_count = metas[k].option_count;
                    b.default_norm = metas[k].default_norm;
                    b.name = metas[k].label;  // §2.1: design caption -> param name
                }
                v->params.push_back(std::move(b));
            }
        }
    }

    // Native-view lane: a hand-built DesignFrameView (mounted via
    // pulp_embed_create_from_view) carries no DesignIR, so keys come off the LIVE
    // view — element_param_key(i) is the host param id the author declared. Bind
    // every value-bearing element with a non-empty key; everything downstream
    // (store param alloc, on_element_changed/gesture wiring, host->UI push) is
    // shared with the faithful lane via frame_element_index. Discreteness metadata
    // is reported coarsely (continuous) since the live view exposes no option
    // count; the bind itself is correct for choice controls because
    // element_value/set_element_value map the selection internally.
    if (dynamic_cast<EmbedNativeViewProcessor*>(v->processor.get())) {
        if (auto* frame = find_design_frame_view(root)) {
            using Kind = pulp::view::DesignFrameElement::Kind;
            for (int i = 0; i < frame->element_count(); ++i) {
                const std::string& key = frame->element_param_key(i);
                if (key.empty()) continue;
                const Kind kind = frame->element_kind(i);
                const char* wk = nullptr;
                switch (kind) {
                    case Kind::knob:      wk = "knob";      break;
                    case Kind::fader:     wk = "fader";     break;
                    case Kind::toggle:    wk = "toggle";    break;
                    case Kind::dropdown:  wk = "dropdown";  break;
                    case Kind::tab_group: wk = "tab_group"; break;
                    case Kind::stepper:   wk = "stepper";   break;
                    case Kind::xy_pad:    wk = "xy_pad";    break;
                    // text_field is a string (not a normalized param); momentary /
                    // swap / action / value_label / custom carry no host param.
                    default: continue;
                }
                ParamBinding b;
                b.key = key;
                b.widget_id = key;
                b.kind = ParamWidgetKind::knob;  // value carried via frame element
                b.widget = frame;
                b.frame_element_index = i;
                b.widget_kind = wk;
                b.default_norm = frame->element_value(i);  // imported default
                v->params.push_back(std::move(b));
            }
        }
    }

    for (size_t i = 0; i < v->params.size(); ++i) {
        auto& b = v->params[i];
        // Reuse the store param for a key that already existed (reload); else
        // allocate a fresh id past the current max and register it once.
        auto reused = prev.find(b.key);
        const bool is_new = (reused == prev.end());
        b.param_id = is_new ? ++max_id : reused->second;

        if (is_new) {
            pulp::state::ParamInfo info;
            info.id = b.param_id;
            // ParamInfo::name records the key the param was FIRST registered
            // under and is never rewritten — StateStore has no rename API, and a
            // runtime re-key (set_element_param_key) deliberately does not try to
            // fake one. This is cosmetic, not a bug: the store param is an
            // INTERNAL handle bound to the element, not to the host key. Every
            // key-addressed path (host->UI push, UI->host write, the gesture
            // brackets, the host surface snapshot) resolves through
            // ParamBinding::key / key_to_index, which the re-key handler keeps
            // current — so a re-keyed element routes correctly while its store
            // param keeps its original, purely-diagnostic name.
            info.name = b.key;
            info.range = pulp::state::ParamRange{0.0f, 1.0f, 0.0f, 0.0f};
            v->store->add_parameter(info);
        }

        // Seed: prefer the host's current value (automation/preset already set
        // before the editor opened), else the widget's imported default.
        float seed = widget_normalized(b);
        // Record the imported default (pre-host) for ABI v5 param_info. Faithful
        // elements already carry it from the IRInteractiveElement metadata.
        if (b.frame_element_index < 0)
            b.default_norm = seed;
        if (v->host_cb.get_param) {
            double hv = v->host_cb.get_param(v->host_ctx, b.key.c_str());
            if (hv >= 0.0 && hv <= 1.0) {
                seed = static_cast<float>(hv);
                widget_set_normalized(b, seed);
            }
        }
        v->store->set_normalized(b.param_id, seed);

        // Frame elements have no per-widget on_change; their UI->host forwarding
        // is wired event-driven via DesignFrameView::on_element_changed at the end
        // of this function. Other widgets wire their on_change here.
        if (b.frame_element_index < 0)
            wire_widget_to_host(v, b);
    }

    // Project the (now final) binding keys into the key->index registry. A
    // rebuild re-collects the key set wholesale (a reload can add/drop/reorder
    // keys), so it counts as a key-set change for a host's dirty gate.
    rebuild_key_index(v);
    ++v->param_key_generation;

    // Gesture begin/end forwarding (one set of callbacks for the whole store).
    if (v->host_cb.begin_gesture || v->host_cb.end_gesture) {
        PulpEmbedView* self = v;
        v->store->set_gesture_callbacks(
            [self](pulp::state::ParamID id) {
                if (!self->host_cb.begin_gesture) return;
                if (id == 0 || id > self->params.size()) return;
                self->host_cb.begin_gesture(self->host_ctx,
                                            self->params[id - 1].key.c_str());
            },
            [self](pulp::state::ParamID id) {
                if (!self->host_cb.end_gesture) return;
                if (id == 0 || id > self->params.size()) return;
                self->host_cb.end_gesture(self->host_ctx,
                                          self->params[id - 1].key.c_str());
            });
    }

    // Value-change forwarding: UI writes -> host.set_param. Suppressed while a
    // host-driven change is being applied (pulp_embed_param_changed).
    if (v->host_cb.set_param) {
        PulpEmbedView* self = v;
        v->param_listener = v->store->add_listener(
            [self](pulp::state::ParamID id, float /*denorm*/) {
                if (self->applying_host_change) return;          // break the loop
                if (!self->host_cb.set_param) return;
                if (id == 0 || id > self->params.size()) return;
                const auto& b = self->params[id - 1];
                self->host_cb.set_param(self->host_ctx, b.key.c_str(),
                                        self->store->get_normalized(id));
            },
            pulp::state::ListenerThread::Main);
    }

    // Faithful-vector lane (v2, event-driven): route DesignFrameView's USER
    // changes + gestures through the SAME store -> listener -> host path as every
    // other control — no per-tick poll. set_element_value is silent, so a
    // host-driven push (pulp_embed_param_changed) does NOT echo back through
    // on_element_changed. Covers knobs AND choice controls (dropdown/tab/stepper)
    // uniformly via the frame's element index.
    if (auto* frame = find_design_frame_view(root)) {
        PulpEmbedView* self = v;
        auto pid_for = [self](int idx) -> pulp::state::ParamID {
            for (const auto& b : self->params)
                if (b.frame_element_index == idx) return b.param_id;
            return 0;
        };
        frame->on_element_changed = [self, pid_for](int idx, float val) {
            if (self->applying_host_change) return;             // break the loop
            if (auto pid = pid_for(idx)) self->store->set_normalized(pid, val);
        };
        frame->on_gesture_begin = [self, pid_for](int idx) {
            if (auto pid = pid_for(idx)) self->store->begin_gesture(pid);
        };
        frame->on_gesture_end = [self, pid_for](int idx) {
            if (auto pid = pid_for(idx)) self->store->end_gesture(pid);
        };

        // Runtime re-key (paged rack / tabbed slot): set_element_param_key mutates
        // the element's param_key INSIDE the view. Every embed-side use of that
        // key is a COPY taken when this bridge was built, so without this handler
        // BOTH directions keep addressing the OLD host param:
        //   UI->host — the store listener and the gesture brackets read
        //              ParamBinding::key, so writes and begin/end land on the old
        //              parameter.
        //   host->UI — key_to_index still maps the old key to this element, so a
        //              push under the new key silently no-ops while a push under
        //              the old key moves the wrong control.
        // Refreshing key_to_index alone does NOT fix this: the map is derived from
        // ParamBinding::key, so it must be rebuilt AFTER the binding's key is
        // updated, never instead of it.
        frame->on_param_key_changed = [self](int idx, const std::string& key) {
            size_t slot = 0;
            bool bound = false;
            for (size_t i = 0; i < self->params.size(); ++i) {
                if (self->params[i].frame_element_index != idx) continue;
                // widget_id tracks key for a frame element (they are the same
                // string by construction in both the faithful and native lanes),
                // so it moves with it and pulp_embed_param_widget_id stays honest.
                self->params[i].key = key;
                self->params[i].widget_id = key;
                slot = i;
                bound = true;
                break;
            }
            // An element with no binding was not value-bearing (or carried an
            // empty key) when the bridge was built; re-keying it does not create a
            // binding — a host that needs one rebuilds the bridge (reload).
            if (!bound) return;
            rebuild_key_index(self);
            // Publish the change: a host's pump gates its re-enumeration on this
            // counter, and a re-key is driven from inside the view, so this is
            // the host's ONLY signal that its cached bindings just went stale.
            ++self->param_key_generation;
            // The host param-surface snapshot (membership / display text / step
            // count) is keyed off ParamBinding::key, so this element's slots now
            // describe the OLD parameter. Re-resolve just this one now rather
            // than leaving it wrong until the next tick — a re-key is a discrete
            // UI-thread event, and a stale membership answer is exactly the class
            // of bug this handler exists to close.
            if (slot < self->param_has_snapshot.size())
                snapshot_host_param_at(self, slot);
        };
    }

    // ABI v8 host_action (view->host): back the SDK's view-side host-action
    // channel with the captured C callback. A view reaches the host through
    // View::host_actions()->send_host_action(...) — either directly (a native
    // view) or via a DesignFrameView action button once route_actions_to_host is
    // armed.
    //
    // Installed only when the host wired the callback: with no host_action there
    // is nothing to forward, and leaving host_actions() null keeps the SDK's own
    // "no host action channel" path intact. Re-configured (not re-allocated) on
    // every build so a reload re-points the surface at the live callback and the
    // fresh view tree; the surface itself is an owned member, so it outlives the
    // view it is installed on, as its contract requires.
    if (v->host_cb.host_action) {
        v->host_action_surface.configure(v->host_cb.host_action, v->host_ctx);
        root->set_host_actions(&v->host_action_surface);
    } else {
        root->set_host_actions(nullptr);
    }

    // Arm the frame's action routing in the IMPORTED lane, and only there.
    //
    // The SDK defaults route_actions_to_host_ OFF, so a Kind::action button fires
    // on_action and nothing else. That default is right for a view whose author is
    // present: a native view (below) is constructed by a factory this shim calls,
    // and its author holds route_actions_to_host() and decides which of their
    // buttons are host commands versus view-internal (paging chevrons, tabs).
    //
    // An IMPORTED design has no such author. Its elements come from DesignIR — a
    // design tool emits an action button, and nothing between that JSON and here
    // can opt it in. Left at the default, an imported action button is inert: it
    // can never reach host_action, no matter what the host wires. This shim is the
    // only seam that exists, so it is the one that must arm it.
    //
    // The host's own opt-in is the gate, and it is the signal the ABI already
    // carries: wiring host_action IS the statement "route action buttons to me".
    // A host that wired nothing keeps the pre-existing inert behavior exactly, so
    // this can only turn a dead button live — never redirect a live one. Nothing
    // new is invented, and no ABI verb is needed to say what the callback says.
    if (dynamic_cast<EmbedProcessor*>(v->processor.get()))
        if (auto* frame = find_design_frame_view(root))
            frame->route_actions_to_host(v->host_cb.host_action != nullptr);

    // Host parameter surface: gives the view tree the HOST's value count per key,
    // so DesignFrameView scales a choice control by the parameter's domain rather
    // than by the number of positions it draws. Installed UNCONDITIONALLY, unlike
    // the action surface above — every answer it serves is snapshot-backed and
    // degrades on its own (an unwired host_param_steps reads 0 = "cannot answer",
    // which the frame handles and reports), and the param bridge always exists.
    // Gating on a callback would instead strip the view of has_param/get_param
    // for hosts that wire only some of the block.
    //
    // route_changes_to_host_params stays OFF (the SDK default): the store
    // listener is already the UI->host write funnel, so enabling it here would
    // write every edit twice.
    v->host_param_surface.configure(v);
    root->set_host_params(&v->host_param_surface);

    // ABI v6: text_field string bridge (separate from the numeric params above).
    build_string_bridge(v);

    // Seed the host param-surface snapshot (has_param / param_display_text, and
    // the v10 step count) so the getters are populated before the first tick.
    // Refreshed each tick.
    snapshot_host_param_surface(v);
}

// Build the text-field string bridge (ABI v6): discover bindable text_field
// overlay editors, seed each from host.get_string, and forward user edits to
// host.set_string. Strings are host/plugin state, not normalized params.
void build_string_bridge(PulpEmbedView* v) {
    v->strings.clear();
    if (!v || !v->bridge) return;
    auto* root = v->bridge->view();
    auto* ep = dynamic_cast<EmbedProcessor*>(v->processor.get());
    if (!root || !ep) return;  // string bridge is the faithful-vector lane today
    auto* frame = find_design_frame_view(root);
    if (!frame) return;

    for (auto& [idx, key] : ep->faithful_text_field_keys()) {
        auto* te = dynamic_cast<pulp::view::TextEditor*>(frame->overlay_widget(idx));
        if (!te) continue;
        // Seed from host state (preset recall) before wiring the change hook.
        if (v->host_cb.get_string) {
            char buf[2048] = {0};
            const int32_t n = v->host_cb.get_string(v->host_ctx, key.c_str(), buf,
                                                     static_cast<int32_t>(sizeof buf));
            if (n >= 0) {
                buf[sizeof buf - 1] = '\0';
                v->applying_host_string = true;
                te->set_text(buf);
                v->applying_host_string = false;
            }
        }
        // User edit -> host (suppressed while we apply a host-driven set_text).
        PulpEmbedView* self = v;
        const std::string k = key;
        te->on_change = [self, k](const std::string& text) {
            if (self->applying_host_string) return;
            if (self->host_cb.set_string)
                self->host_cb.set_string(self->host_ctx, k.c_str(), text.c_str());
        };
        v->strings.push_back({key, te});
    }
}

// Compute render-referenced assets that are missing on disk (ABI v7). Uses the
// parsed DesignIR + IRAssetManifest directly — authoritative, struct-based — so a
// consumer (e.g. pulp-embed-validate) doesn't string-scan the JSON. Today it
// covers the faithful-vector lane's frame SVG (svg_asset_id); the manifest's
// other entries are non-faithful fallback rasters the render never loads, so they
// are intentionally NOT flagged (avoids the false positive). local_paths were
// rewritten to absolute by resolve_asset_paths, so existence is a direct check.
// Bundle (scripted) views resolve assets through the session, not here -> empty.
void collect_svg_refs(const pulp::view::IRNode& n,
                      const pulp::view::IRAssetManifest& manifest,
                      PulpEmbedView* v) {
    namespace fs = std::filesystem;
    if (n.svg_asset_id) {
        if (const auto* a = manifest.resolve(*n.svg_asset_id)) {
            std::error_code ec;
            if (a->local_path && !a->local_path->empty() && !fs::exists(*a->local_path, ec))
                v->missing_assets.push_back(*a->local_path);
        }
    }
    for (const auto& c : n.children) collect_svg_refs(c, manifest, v);
}

void collect_missing_render_assets(PulpEmbedView* v) {
    v->missing_assets.clear();
    auto* ep = dynamic_cast<EmbedProcessor*>(v->processor.get());
    if (!ep) return;  // DesignIR lane only; bundle assets load via the scripted session
    const auto& ir = ep->ir();
    collect_svg_refs(ir.root, ir.asset_manifest, v);
}

// Poll host meters once (called from tick). Designs without meter widgets and
// hosts without a read_meters callback make this a no-op. The figma fixture has
// no meters, so this is currently a forwarding stub kept ready for meter-bearing
// designs (see report note).
void poll_host_meters(PulpEmbedView* v) {
    if (!v || !v->host_cb.read_meters) return;
    constexpr int kCap = pulp::view::MeterData::max_channels;
    float levels[kCap] = {};
    int n = v->host_cb.read_meters(v->host_ctx, levels, kCap);
    (void)n;
    // No meter widgets in the current importer JS bundle surface. When the
    // scripted bundle gains createMeter bindings, route `levels` through the
    // session's AudioBridge here.
}

// Re-resolve ONE param's host surface (membership / display text / step count)
// into the snapshot slots at `i`. The caller guarantees the slots are sized (see
// snapshot_host_param_surface) and that `i` is in range. Split out so a discrete
// re-key can refresh just the element that moved instead of re-walking every
// param — a paged rack re-keys a couple of elements out of a couple of hundred.
void snapshot_host_param_at(PulpEmbedView* v, size_t i) {
    const auto& b = v->params[i];
    // Reset to the "don't know" defaults first: a callback the host does not
    // wire must leave the documented answer, not a value from a previous key.
    v->param_has_snapshot[i] = -1;
    v->param_display_snapshot[i].clear();
    v->param_display_norm_snapshot[i] = std::numeric_limits<double>::quiet_NaN();
    v->param_steps_snapshot[i] = 0;

    if (v->host_cb.has_param)
        v->param_has_snapshot[i] = static_cast<int8_t>(
            v->host_cb.has_param(v->host_ctx, b.key.c_str()) ? 1 : 0);

    if (v->host_cb.host_param_steps) {
        const int32_t steps = v->host_cb.host_param_steps(v->host_ctx, b.key.c_str());
        // Clamp a negative to the documented 0: the ABI has exactly one "don't
        // know" answer, and a host must not be able to invent another.
        v->param_steps_snapshot[i] = steps > 0 ? steps : 0;
    }

    if (v->host_cb.param_display_text) {
        const double norm =
            v->store ? static_cast<double>(v->store->get_normalized(b.param_id))
                     : 0.0;
        // Record what the text below is formatted at BEFORE the callback runs, so
        // the pairing holds on every exit from this block — including the len == 0
        // early return, where the entry stays empty and the value is irrelevant.
        v->param_display_norm_snapshot[i] = norm;
        char buf[256];
        const size_t len = v->host_cb.param_display_text(v->host_ctx, b.key.c_str(),
                                                         norm, buf, sizeof buf);
        if (len == 0) return;                       // no display text for this key
        if (len < sizeof buf) {
            v->param_display_snapshot[i].assign(buf, len);
        } else {
            // Host reports a string longer than our stack buffer — re-query with
            // an exact-sized heap buffer (two-call sizing contract).
            std::string big(len + 1, '\0');
            size_t got = v->host_cb.param_display_text(
                v->host_ctx, b.key.c_str(), norm, big.data(), big.size());
            if (got > len) got = len;
            v->param_display_snapshot[i].assign(big.data(), got);
        }
    }
}

// Snapshot the host param surface (ABI v8 membership/display text + the v10 step
// count) into local state. Called at create / reload (from build_param_bridge)
// and every tick. The embedded view's paint path reads the cache
// (pulp_embed_param_has / pulp_embed_param_display_text / pulp_embed_param_steps)
// — it MUST NOT re-enter the host per frame, so the one host round-trip happens
// here, at tick, not in paint. Sizing is always kept in lockstep with `params` so
// the getters index safely even when no callback is wired (all -1/empty/0).
void snapshot_host_param_surface(PulpEmbedView* v) {
    if (!v) return;
    const size_t n = v->params.size();
    v->param_has_snapshot.assign(n, static_cast<int8_t>(-1));
    v->param_display_snapshot.assign(n, std::string());
    // NaN = nothing snapshotted, so an unwired param_display_text leaves every
    // entry unable to match any request — the correct "no text" answer.
    v->param_display_norm_snapshot.assign(n, std::numeric_limits<double>::quiet_NaN());
    // 0 = continuous/unknown, so an unwired host_param_steps leaves every entry
    // at the correct "no step divisor" answer.
    v->param_steps_snapshot.assign(n, 0);
    if (!v->host_cb.has_param && !v->host_cb.param_display_text &&
        !v->host_cb.host_param_steps)
        return;
    for (size_t i = 0; i < n; ++i) snapshot_host_param_at(v, i);
}

// Shared create path for the high-fidelity scripted-UI bundle. bundle_dir must
// contain ui.js; asset paths inside resolve absolute or relative to bundle_dir.
// When `offscreen` is true no PluginViewHost is created (the host pulls frames).
PulpEmbedResult create_from_bundle(const PulpEmbedDesc* desc,
                                   const std::string& bundle_dir,
                                   bool offscreen,
                                   PulpEmbedView** out_view) {
    if (out_view) *out_view = nullptr;
    g_create_error.clear();
    if (auto r = check_desc(desc); r != PULP_EMBED_OK) {
        g_create_error = "invalid descriptor";
        return r;
    }
    if (!out_view) return PULP_EMBED_ERR_INVALID_ARG;

    namespace fs = std::filesystem;
    const fs::path script = fs::path(bundle_dir) / "ui.js";
    if (!fs::exists(script)) {
        g_create_error = "bundle missing ui.js: " + script.string();
        return PULP_EMBED_ERR_PARSE;
    }

    auto v = std::make_unique<PulpEmbedView>();
    v->offscreen = offscreen;
    // Capture host callbacks up front so resolve_resource can stage bundle
    // assets BEFORE the script runs and the renderer loads them.
    capture_host_callbacks(v.get(), desc);

    const auto w = static_cast<uint32_t>(desc->logical_width);
    const auto h = static_cast<uint32_t>(desc->logical_height);
    v->size_hints = pulp::format::view_size_from_design(w, h);

    auto proc = std::make_unique<EmbedScriptedProcessor>(script, fs::path(bundle_dir), v->size_hints);

    // Resource session (ABI v3): offer each asset path in ui.js to the host's
    // resolve_resource; stage the bytes it serves and install id -> staged-path
    // overrides the JS resolver consults before the bundle-dir fallback.
    if (v->host_cb.resolve_resource) {
        std::ifstream uin(script, std::ios::binary);
        if (uin) {
            std::ostringstream us;
            us << uin.rdbuf();
            proc->set_asset_overrides(stage_bundle_resources(v.get(), us.str()));
        }
    }

    try {
        proc->load_or_throw();
    } catch (const std::exception& e) {
        g_create_error = std::string("bundle load failed: ") + e.what();
        return PULP_EMBED_ERR_VIEW_OPEN;
    }
    v->processor = std::move(proc);
    v->store = std::make_unique<pulp::state::StateStore>();
    v->bridge = std::make_unique<pulp::format::ViewBridge>(*v->processor, *v->store);

    std::string err;
    if (!v->bridge->open(&err)) {
        g_create_error = "view open failed: " + err;
        return PULP_EMBED_ERR_VIEW_OPEN;
    }
    if (!v->bridge->view()) {
        g_create_error = "scripted view tree is empty";
        return PULP_EMBED_ERR_MATERIALIZE;
    }

    if (auto* rv = v->bridge->view()) {
        rv->set_bounds({0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)});
        rv->layout_children();
    }

    if (!offscreen) {
        pulp::view::PluginViewHost::Options opts;
        opts.size = {w, h};
        opts.use_gpu = (desc->backend_pref != PULP_EMBED_BACKEND_PREF_CPU);
        v->host = pulp::view::PluginViewHost::create(*v->bridge->view(), opts);
        if (!v->host) {
            g_create_error = "no PluginViewHost (missing platform factory?)";
            return PULP_EMBED_ERR_HOST_CREATE;
        }
        v->backend = v->host->is_gpu_backed() ? PULP_EMBED_BACKEND_GPU
                                              : PULP_EMBED_BACKEND_CPU;

        // Load-bearing for scripted/GPU fidelity — see wire_scripted_session_to_host.
        wire_scripted_session_to_host(v.get());
    } else {
        // Offscreen: no live host / display-link; frames come from the
        // deterministic renderer. The scripted session still loaded + ran, so
        // poll() once so timers/rAF/async asset loads settle before first pull.
        v->backend = PULP_EMBED_BACKEND_CPU;
        if (auto* scripted = v->bridge->scripted_ui()) {
            std::string perr;
            scripted->poll(&perr);
        }
    }

    // Interactive parameter bridge (ABI v2): discover the design's controls,
    // register them in the StateStore, and wire UI<->host param + gesture flow.
    build_param_bridge(v.get());

    if (!offscreen && desc->design_width > 0 && desc->design_height > 0) {
        v->host->set_design_viewport(static_cast<float>(desc->design_width),
                                     static_cast<float>(desc->design_height));
    }

    *out_view = v.release();
    return PULP_EMBED_OK;
}

}  // namespace

// ── native-view create (C++ surface; see pulp_view_embed_native.hpp) ─────────
// Mirrors create_from_json's shape but skips the DesignIR parse / asset resolve:
// the root tree comes straight from the host factory. The shared open_and_bridge
// tail then opens the ViewBridge, attaches the host, and (for a DesignFrameView
// with param_key'd elements) wires the same string-key host bridge.
namespace pulp::embed {

PulpEmbedResult pulp_embed_create_from_view(const PulpEmbedDesc* desc,
                                            NativeViewFactory factory,
                                            PulpEmbedView** out_view) {
    if (out_view) *out_view = nullptr;
    g_create_error.clear();
    if (auto r = check_desc(desc); r != PULP_EMBED_OK) {
        g_create_error = "invalid descriptor";
        return r;
    }
    if (!out_view) return PULP_EMBED_ERR_INVALID_ARG;
    if (!factory) {
        g_create_error = "null native-view factory";
        return PULP_EMBED_ERR_INVALID_ARG;
    }

    auto v = std::make_unique<PulpEmbedView>();
    v->offscreen = false;  // windowed: Pulp owns a child view in the host window
    capture_host_callbacks(v.get(), desc);

    const auto w = static_cast<uint32_t>(desc->logical_width);
    const auto h = static_cast<uint32_t>(desc->logical_height);
    v->size_hints = pulp::format::view_size_from_design(w, h);

    v->processor =
        std::make_unique<EmbedNativeViewProcessor>(std::move(factory), v->size_hints);

    if (auto r = open_and_bridge(v.get(), desc, w, h); r != PULP_EMBED_OK)
        return r;

    *out_view = v.release();
    return PULP_EMBED_OK;
}

}  // namespace pulp::embed

extern "C" {

uint32_t pulp_embed_abi_version(void) { return PULP_VIEW_EMBED_ABI_VERSION; }

PulpEmbedResult pulp_embed_create_from_design_json(const PulpEmbedDesc* desc,
                                                   const char* path,
                                                   PulpEmbedView** out_view) {
    try {
        if (out_view) *out_view = nullptr;
        if (!path) { g_create_error = "null path"; return PULP_EMBED_ERR_INVALID_ARG; }
        std::ifstream f(path, std::ios::binary);
        if (!f) { g_create_error = std::string("cannot open ") + path; return PULP_EMBED_ERR_PARSE; }
        std::ostringstream ss;
        ss << f.rdbuf();
        // Relative asset paths in the IR resolve against the IR file's directory.
        const std::string base_dir = std::filesystem::path(path).parent_path().string();
        return create_from_json(desc, ss.str(), base_dir, /*offscreen=*/false, out_view);
    } catch (const std::exception& e) {
        g_create_error = std::string("internal: ") + e.what();
        return PULP_EMBED_ERR_INTERNAL;
    } catch (...) {
        g_create_error = "internal: unknown exception";
        return PULP_EMBED_ERR_INTERNAL;
    }
}

PulpEmbedResult pulp_embed_create_from_design_json_str(const PulpEmbedDesc* desc,
                                                       const char* json,
                                                       size_t json_len,
                                                       PulpEmbedView** out_view) {
    try {
        if (out_view) *out_view = nullptr;
        if (!json) { g_create_error = "null json"; return PULP_EMBED_ERR_INVALID_ARG; }
        const std::string base_dir =
            (desc && desc->asset_base_path) ? std::string(desc->asset_base_path) : std::string();
        return create_from_json(desc, std::string(json, json_len), base_dir, /*offscreen=*/false, out_view);
    } catch (const std::exception& e) {
        g_create_error = std::string("internal: ") + e.what();
        return PULP_EMBED_ERR_INTERNAL;
    } catch (...) {
        g_create_error = "internal: unknown exception";
        return PULP_EMBED_ERR_INTERNAL;
    }
}

PulpEmbedResult pulp_embed_create_from_ui_bundle(const PulpEmbedDesc* desc,
                                                 const char* bundle_dir,
                                                 PulpEmbedView** out_view) {
    try {
        if (out_view) *out_view = nullptr;
        if (!bundle_dir) { g_create_error = "null bundle_dir"; return PULP_EMBED_ERR_INVALID_ARG; }
        return create_from_bundle(desc, std::string(bundle_dir), /*offscreen=*/false, out_view);
    } catch (const std::exception& e) {
        g_create_error = std::string("internal: ") + e.what();
        return PULP_EMBED_ERR_INTERNAL;
    } catch (...) {
        g_create_error = "internal: unknown exception";
        return PULP_EMBED_ERR_INTERNAL;
    }
}

PulpEmbedResult pulp_embed_create_offscreen(const PulpEmbedDesc* desc,
                                            const char* source,
                                            int32_t from_bundle,
                                            PulpEmbedView** out_view) {
    try {
        if (out_view) *out_view = nullptr;
        if (!source) { g_create_error = "null source"; return PULP_EMBED_ERR_INVALID_ARG; }
        if (from_bundle) {
            return create_from_bundle(desc, std::string(source), /*offscreen=*/true, out_view);
        }
        std::ifstream f(source, std::ios::binary);
        if (!f) { g_create_error = std::string("cannot open ") + source; return PULP_EMBED_ERR_PARSE; }
        std::ostringstream ss;
        ss << f.rdbuf();
        const std::string base_dir = std::filesystem::path(source).parent_path().string();
        return create_from_json(desc, ss.str(), base_dir, /*offscreen=*/true, out_view);
    } catch (const std::exception& e) {
        g_create_error = std::string("internal: ") + e.what();
        return PULP_EMBED_ERR_INTERNAL;
    } catch (...) {
        g_create_error = "internal: unknown exception";
        return PULP_EMBED_ERR_INTERNAL;
    }
}

PulpEmbedResult pulp_embed_render_frame_rgba(PulpEmbedView* v, int32_t width,
                                             int32_t height, float scale,
                                             uint8_t* out, size_t cap, int32_t* w,
                                             int32_t* h, int32_t* stride) {
    if (!v || !v->bridge || width <= 0 || height <= 0)
        return PULP_EMBED_ERR_INVALID_ARG;
    try {
        auto* rv = v->bridge->view();
        if (!rv) return set_err(v, PULP_EMBED_ERR_MATERIALIZE, "no view");
        rv->set_bounds({0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)});
        rv->layout_children();

        uint32_t pw = 0, ph = 0;
        std::vector<uint8_t> rgba = pulp::view::render_to_rgba(
            *rv, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
            scale > 0.0f ? scale : 1.0f, &pw, &ph);
        if (rgba.empty() || pw == 0 || ph == 0)
            return set_err(v, PULP_EMBED_ERR_UNSUPPORTED,
                           "render_to_rgba produced no pixels (no Skia backend?)");

        const int32_t row = static_cast<int32_t>(pw) * 4;
        if (w) *w = static_cast<int32_t>(pw);
        if (h) *h = static_cast<int32_t>(ph);
        if (stride) *stride = row;
        if (!out) return PULP_EMBED_OK;  // sizing query
        if (cap < rgba.size()) return PULP_EMBED_ERR_BUFFER_TOO_SMALL;
        std::memcpy(out, rgba.data(), rgba.size());
        return PULP_EMBED_OK;
    } catch (const std::exception& e) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, e.what());
    } catch (...) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, "render_frame_rgba threw");
    }
}

size_t pulp_embed_last_create_error(char* buf, size_t cap) {
    const auto& s = g_create_error;
    if (buf && cap) {
        const size_t n = s.size() < cap - 1 ? s.size() : cap - 1;
        for (size_t i = 0; i < n; ++i) buf[i] = s[i];
        buf[n] = '\0';
    }
    return s.size();
}

void* pulp_embed_native_handle(PulpEmbedView* v) {
    if (!v || !v->host) return nullptr;
    try { return v->host->native_handle(); } catch (...) { return nullptr; }
}

PulpEmbedResult pulp_embed_notify_attached(PulpEmbedView* v) {
    if (!v || !v->host || !v->bridge) return PULP_EMBED_ERR_INVALID_ARG;
    try {
        if (!v->host->is_attached())
            return set_err(v, PULP_EMBED_ERR_ATTACH, "child not parented; cannot notify_attached");
        if (!v->opened) { v->bridge->notify_attached(); v->opened = true; }
        return PULP_EMBED_OK;
    } catch (const std::exception& e) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, e.what());
    } catch (...) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, "notify_attached threw");
    }
}

PulpEmbedResult pulp_embed_attach(PulpEmbedView* v, void* parent) {
    if (!v || !v->host || !v->bridge) return PULP_EMBED_ERR_INVALID_ARG;
    try {
        if (v->host->is_attached()) return PULP_EMBED_OK;  // idempotent
        if (!v->host->try_attach_to_parent(parent))
            return set_err(v, PULP_EMBED_ERR_ATTACH, "attach_to_parent did not take");
        if (!v->opened) { v->bridge->notify_attached(); v->opened = true; }
        return PULP_EMBED_OK;
    } catch (const std::exception& e) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, e.what());
    } catch (...) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, "unknown exception");
    }
}

PulpEmbedResult pulp_embed_detach(PulpEmbedView* v) {
    if (!v || !v->host) return PULP_EMBED_ERR_INVALID_ARG;
    try {
        v->host->detach();
        return PULP_EMBED_OK;
    } catch (...) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, "detach threw");
    }
}

PulpEmbedResult pulp_embed_resize(PulpEmbedView* v, int32_t w, int32_t h, float scale) {
    if (!v || !v->host || !v->bridge || w <= 0 || h <= 0) return PULP_EMBED_ERR_INVALID_ARG;
    // `scale` is validated but advisory for the windowed/native embed path:
    // PluginViewHost has no backing-scale setter — the attached NSWindow's
    // backingScaleFactor drives surface DPI. Only the explicit deterministic
    // capture APIs (pulp_embed_render_png / pulp_embed_render_frame_rgba) honor
    // a caller-supplied scale. We still reject a non-finite/non-positive scale
    // so a host bug surfaces here instead of producing a degenerate surface.
    if (!(scale > 0.0f) || !std::isfinite(scale)) return PULP_EMBED_ERR_INVALID_ARG;
    try {
        v->host->set_size(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
        if (auto* rv = v->bridge->view()) {
            rv->set_bounds({0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)});
            rv->layout_children();
        }
        v->bridge->resize(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
        return PULP_EMBED_OK;
    } catch (...) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, "resize threw");
    }
}

PulpEmbedResult pulp_embed_tick(PulpEmbedView* v) {
    if (!v || !v->host) return PULP_EMBED_ERR_INVALID_ARG;
    try {
        // Drain host param writes queued from the audio thread, then pull the
        // latest meter levels. (Faithful-vector control changes are forwarded
        // event-driven via DesignFrameView::on_element_changed, not polled here.)
        if (v->store) v->store->pump_listeners();
        poll_host_meters(v);
        // Refresh the host param-surface snapshot (membership + display text +
        // step count) ONCE here so the subsequent repaint reads cached state and
        // never re-enters the host from paint. Runs every tick regardless of the
        // gate — it is a cheap host-state refresh, not a paint.
        snapshot_host_param_surface(v);
        // Repaint every tick by default. When the dirty gate is opted in (ABI v9),
        // repaint only when the tree is animating — discrete changes already
        // repainted on their own push path, so skipping an idle tick loses nothing
        // but frames.
        if (!v->dirty_gate ||
            pulp::embed::embed_view_needs_frame(v->bridge ? v->bridge->view() : nullptr))
            v->host->repaint();
        return PULP_EMBED_OK;
    } catch (...) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, "tick threw");
    }
}

PulpEmbedResult pulp_embed_repaint(PulpEmbedView* v) {
    if (!v || !v->host) return PULP_EMBED_ERR_INVALID_ARG;
    try {
        v->host->repaint();
        return PULP_EMBED_OK;
    } catch (...) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, "repaint threw");
    }
}

PulpEmbedResult pulp_embed_set_dirty_gate(PulpEmbedView* v, int32_t enabled) {
    if (!v) return PULP_EMBED_ERR_INVALID_ARG;
    v->dirty_gate = (enabled != 0);
    return PULP_EMBED_OK;
}

PulpEmbedResult pulp_embed_reload_bundle(PulpEmbedView* v, const char* bundle_dir) {
    if (!v) return PULP_EMBED_ERR_INVALID_ARG;
    // Rebuilds views + touches the GPU surface — must run on the creator thread.
    if (std::this_thread::get_id() != v->creator_thread)
        return set_err(v, PULP_EMBED_ERR_WRONG_THREAD,
                       "reload must be called on the thread that created the view");
    auto* sp = dynamic_cast<EmbedScriptedProcessor*>(v->processor.get());
    if (!sp)
        return set_err(v, PULP_EMBED_ERR_UNSUPPORTED,
                       "reload is supported only for the scripted bundle path "
                       "(create_from_ui_bundle)");
    try {
        std::string err;
        const std::filesystem::path dir =
            bundle_dir ? std::filesystem::path(bundle_dir) : std::filesystem::path();
        // probe-first / last-good lives in ScriptedUiSession::reload_from: on
        // failure the running UI is untouched and we report the error.
        if (!sp->reload(dir, &err))
            return set_err(v, PULP_EMBED_ERR_INTERNAL,
                           err.empty() ? "reload failed (last-good UI kept)" : err);
        // The widget tree was rebuilt — rebuild the param bridge (reuses store
        // params by key; resets the old bindings/listener internally).
        build_param_bridge(v);
        if (v->host) v->host->repaint();
        return PULP_EMBED_OK;
    } catch (const std::exception& e) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, e.what());
    } catch (...) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, "reload threw");
    }
}

PulpEmbedResult pulp_embed_size_hints(PulpEmbedView* v, PulpEmbedSizeHints* out) {
    if (!v || !out) return PULP_EMBED_ERR_INVALID_ARG;
    const auto& s = v->size_hints;
    out->preferred_width = static_cast<int32_t>(s.preferred_width);
    out->preferred_height = static_cast<int32_t>(s.preferred_height);
    out->min_width = static_cast<int32_t>(s.min_width);
    out->min_height = static_cast<int32_t>(s.min_height);
    out->max_width = static_cast<int32_t>(s.max_width);
    out->max_height = static_cast<int32_t>(s.max_height);
    out->aspect_ratio = static_cast<float>(s.aspect_ratio);
    // Derived honestly from the synthesized min/max: resizable iff there is
    // any room to resize — an unbounded max (0), or max strictly greater than min
    // in either axis. A locked design (min == max in both axes) reports 0. Today
    // view_size_from_design synthesizes min = preferred*2/3, max = preferred*2, so
    // this is effectively always 1; it is computed rather than hardcoded so a
    // future fixed-size design honestly reports 0. (Design-DECLARED min/max/
    // resizable, when the importer carries it, needs a versioned
    // pulp_embed_size_hints2() — this struct has no struct_size and cannot grow;
    // see the PulpEmbedSizeHints header doc.)
    const bool w_range = (s.max_width == 0) || (s.max_width > s.min_width);
    const bool h_range = (s.max_height == 0) || (s.max_height > s.min_height);
    out->resizable = (w_range || h_range) ? 1 : 0;
    return PULP_EMBED_OK;
}

int32_t pulp_embed_active_backend(PulpEmbedView* v) {
    return v ? static_cast<int32_t>(v->backend) : PULP_EMBED_BACKEND_UNKNOWN;
}

// ── parameter bridge (ABI v2) ──────────────────────────────────────────────

namespace {
// Copy `s` into buf (NUL-terminated, truncated to cap); return s.size().
size_t copy_str(const std::string& s, char* buf, size_t cap) {
    if (buf && cap) {
        const size_t n = s.size() < cap - 1 ? s.size() : cap - 1;
        std::memcpy(buf, s.data(), n);
        buf[n] = '\0';
    }
    return s.size();
}
}  // namespace

int32_t pulp_embed_param_count(PulpEmbedView* v) {
    return v ? static_cast<int32_t>(v->params.size()) : 0;
}

size_t pulp_embed_param_key(PulpEmbedView* v, int32_t index, char* buf, size_t cap) {
    if (!v || index < 0 || static_cast<size_t>(index) >= v->params.size()) {
        if (buf && cap) buf[0] = '\0';
        return 0;
    }
    return copy_str(v->params[static_cast<size_t>(index)].key, buf, cap);
}

size_t pulp_embed_param_widget_id(PulpEmbedView* v, int32_t index, char* buf, size_t cap) {
    if (!v || index < 0 || static_cast<size_t>(index) >= v->params.size()) {
        if (buf && cap) buf[0] = '\0';
        return 0;
    }
    return copy_str(v->params[static_cast<size_t>(index)].widget_id, buf, cap);
}

double pulp_embed_param_value(PulpEmbedView* v, int32_t index) {
    if (!v || !v->store || index < 0 ||
        static_cast<size_t>(index) >= v->params.size())
        return -1.0;
    return static_cast<double>(
        v->store->get_normalized(v->params[static_cast<size_t>(index)].param_id));
}

PulpEmbedResult pulp_embed_param_info(PulpEmbedView* v, int32_t index,
                                      PulpEmbedParamInfo* out) {
    if (out) *out = PulpEmbedParamInfo{};  // zero-fill so partial reads are safe
    if (!v || !out || index < 0 || static_cast<size_t>(index) >= v->params.size())
        return PULP_EMBED_ERR_INVALID_ARG;
    const auto& b = v->params[static_cast<size_t>(index)];
    auto copy = [](char* dst, size_t cap, const std::string& s) {
        const size_t n = s.size() < cap - 1 ? s.size() : cap - 1;
        std::memcpy(dst, s.data(), n);
        dst[n] = '\0';
    };
    copy(out->widget_kind, sizeof out->widget_kind, b.widget_kind);
    out->is_discrete = b.is_discrete ? 1 : 0;
    out->option_count = b.option_count;
    out->default_norm = static_cast<double>(b.default_norm);
    // §2.1: `name` is the design caption (IRInteractiveElement.label) when the
    // importer carried one — has_meta then signals the host to prefer it over the
    // key. unit/range remain a later slice (still uncarried).
    copy(out->name, sizeof out->name, b.name);
    out->unit[0] = '\0';
    out->has_range = 0;
    out->min_value = 0.0;
    out->max_value = 0.0;
    // step_count: a discrete control's option count is its step count.
    out->step_count = b.is_discrete ? b.option_count : 0;
    out->has_meta = b.name.empty() ? 0 : 1;
    return PULP_EMBED_OK;
}

// ── host param-surface snapshot getters (ABI v8) ───────────────────────────
// Both read the per-tick snapshot (snapshot_host_param_surface); neither calls
// back into the host, so they are safe from a paint/layout path.

int32_t pulp_embed_param_has(PulpEmbedView* v, const char* key) {
    if (!v || !key) return -1;
    auto it = v->key_to_index.find(key);
    if (it == v->key_to_index.end()) return -1;          // not a design control
    if (it->second >= v->param_has_snapshot.size()) return -1;
    return static_cast<int32_t>(v->param_has_snapshot[it->second]);  // -1 / 0 / 1
}

uint64_t pulp_embed_param_key_generation(PulpEmbedView* v) {
    return v ? v->param_key_generation : 0;
}

int32_t pulp_embed_param_steps(PulpEmbedView* v, const char* key) {
    // Every don't-know case collapses to 0 ("continuous or unknown") per the ABI
    // contract: NULL view/key, a key that is not a design control, a host with no
    // host_param_steps callback, and a genuinely continuous parameter are all
    // indistinguishable to the caller by design.
    if (!v || !key) return 0;
    auto it = v->key_to_index.find(key);
    if (it == v->key_to_index.end()) return 0;
    if (it->second >= v->param_steps_snapshot.size()) return 0;
    return v->param_steps_snapshot[it->second];
}

size_t pulp_embed_param_display_text(PulpEmbedView* v, int32_t index,
                                     char* buf, size_t cap) {
    if (!v || index < 0 ||
        static_cast<size_t>(index) >= v->param_display_snapshot.size()) {
        if (buf && cap) buf[0] = '\0';
        return 0;
    }
    return copy_str(v->param_display_snapshot[static_cast<size_t>(index)], buf, cap);
}

PulpEmbedResult pulp_embed_simulate_param_drag(PulpEmbedView* v, int32_t index, double target) {
    if (!v || index < 0 || static_cast<size_t>(index) >= v->params.size())
        return PULP_EMBED_ERR_INVALID_ARG;
    try {
        auto& b = v->params[static_cast<size_t>(index)];
        auto* w = b.widget;
        if (!w) return set_err(v, PULP_EMBED_ERR_INVALID_ARG, "param widget gone");
        const float tgt = static_cast<float>(target < 0.0 ? 0.0 : (target > 1.0 ? 1.0 : target));

        if (b.frame_element_index >= 0) {
            // Faithful-vector lane: `widget` is the DesignFrameView, not a
            // Knob/Fader/Toggle, so the widget-cast paths below don't apply.
            // Drive the SAME begin -> change -> end callbacks a real drag on the
            // frame fires (wired in build_param_bridge), so UI->host forwarding
            // and the visual both move. set_element_value updates the visual
            // silently; on_element_changed drives the store -> host.set_param path.
            auto* frame = static_cast<pulp::view::DesignFrameView*>(w);
            const int el = b.frame_element_index;
            if (frame->on_gesture_begin) frame->on_gesture_begin(el);
            frame->set_element_value(el, tgt);
            if (frame->on_element_changed) frame->on_element_changed(el, tgt);
            if (frame->on_gesture_end) frame->on_gesture_end(el);
        } else if (b.kind == ParamWidgetKind::knob) {
            // Knob drag is delta-based: down records start value at start_y;
            // drag up by (target-cur)*150 px reaches the target (widgets.cpp).
            auto* k = static_cast<pulp::view::Knob*>(w);
            const float cur = k->value();
            const float y0 = 1000.0f;
            k->on_mouse_down({0.0f, y0});                       // fires gesture_begin
            k->on_mouse_drag({0.0f, y0 - (tgt - cur) * 150.0f}); // fires on_change
            k->on_mouse_up({0.0f, y0 - (tgt - cur) * 150.0f});   // fires gesture_end
        } else if (b.kind == ParamWidgetKind::fader) {
            // Fader maps local position to value over its bounds.
            auto* f = static_cast<pulp::view::Fader*>(w);
            const auto lb = f->local_bounds();
            // Vertical default: value = 1 - y/height. Compute the target y.
            const float yh = lb.height > 0 ? lb.height : 150.0f;
            const float y = (1.0f - tgt) * yh;
            f->on_mouse_down({lb.width * 0.5f, y});  // begin + set
            f->on_mouse_drag({lb.width * 0.5f, y});  // change
            f->on_mouse_up({lb.width * 0.5f, y});    // end
        } else {  // toggle
            auto* t = static_cast<pulp::view::Toggle*>(w);
            const bool want = tgt > 0.5f;
            if (t->is_on() != want) t->on_mouse_down({0.0f, 0.0f});  // flips + fires on_toggle
        }
        if (v->host) v->host->repaint();
        return PULP_EMBED_OK;
    } catch (const std::exception& e) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, e.what());
    } catch (...) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, "simulate_param_drag threw");
    }
}

PulpEmbedResult pulp_embed_param_changed(PulpEmbedView* v, const char* key, double normalized) {
    if (!v || !key || !v->store) return PULP_EMBED_ERR_INVALID_ARG;
    try {
        auto it = v->key_to_index.find(key);
        if (it == v->key_to_index.end()) return PULP_EMBED_OK;  // unknown key: no-op
        auto& b = v->params[it->second];

        const float val = static_cast<float>(
            normalized < 0.0 ? 0.0 : (normalized > 1.0 ? 1.0 : normalized));

        // Suppress the store listener's host-forward so a host-driven change
        // does not echo back to host.set_param (feedback-loop break).
        v->applying_host_change = true;
        v->store->set_normalized(b.param_id, val);
        v->applying_host_change = false;

        // Mirror into the live widget (set_value/set_on and DesignFrameView::
        // set_element_value are silent — no on_change / on_element_changed — so
        // this stays a one-way host->view push) and repaint.
        if (b.widget) widget_set_normalized(b, val);
        if (v->host) v->host->repaint();
        return PULP_EMBED_OK;
    } catch (const std::exception& e) {
        v->applying_host_change = false;
        return set_err(v, PULP_EMBED_ERR_INTERNAL, e.what());
    } catch (...) {
        v->applying_host_change = false;
        return set_err(v, PULP_EMBED_ERR_INTERNAL, "param_changed threw");
    }
}

// ── text-field string bridge (ABI v6) — reuses the file's copy_str() helper ──
int32_t pulp_embed_string_param_count(PulpEmbedView* v) {
    return v ? static_cast<int32_t>(v->strings.size()) : 0;
}

size_t pulp_embed_string_param_key(PulpEmbedView* v, int32_t index, char* buf, size_t cap) {
    if (!v || index < 0 || static_cast<size_t>(index) >= v->strings.size()) {
        if (buf && cap > 0) buf[0] = '\0';
        return 0;
    }
    return copy_str(v->strings[static_cast<size_t>(index)].key, buf, cap);
}

size_t pulp_embed_get_string(PulpEmbedView* v, const char* key, char* buf, size_t cap) {
    if (buf && cap > 0) buf[0] = '\0';
    if (!v || !key) return 0;
    for (const auto& s : v->strings)
        if (s.key == key && s.widget) return copy_str(s.widget->text(), buf, cap);
    return 0;
}

PulpEmbedResult pulp_embed_set_string(PulpEmbedView* v, const char* key, const char* utf8) {
    if (!v || !key) return PULP_EMBED_ERR_INVALID_ARG;
    try {
        bool updated = false;
        for (auto& s : v->strings) {
            if (s.key != key || !s.widget) continue;
            // Apply without echoing back through TextEditor::on_change -> set_string.
            v->applying_host_string = true;
            s.widget->set_text(utf8 ? utf8 : "");
            v->applying_host_string = false;
            updated = true;
            break;
        }
        // Repaint on a real change so the pushed text shows this frame. Without
        // this the display relied on the next unconditional tick — which the
        // opt-in dirty gate can skip. Discrete push, so it repaints on its own.
        if (updated && v->host) v->host->repaint();
        return PULP_EMBED_OK;  // unknown key tolerated (blind-push), like param_changed
    } catch (const std::exception& e) {
        v->applying_host_string = false;
        return set_err(v, PULP_EMBED_ERR_INTERNAL, e.what());
    } catch (...) {
        v->applying_host_string = false;
        return set_err(v, PULP_EMBED_ERR_INTERNAL, "set_string threw");
    }
}

PulpEmbedResult pulp_embed_simulate_text_input(PulpEmbedView* v, int32_t index, const char* utf8) {
    if (!v || index < 0 || static_cast<size_t>(index) >= v->strings.size())
        return PULP_EMBED_ERR_INVALID_ARG;
    try {
        auto* te = v->strings[static_cast<size_t>(index)].widget;
        if (!te) return set_err(v, PULP_EMBED_ERR_INVALID_ARG, "string widget gone");
        // Real edit path (NOT guarded) — set_text fires on_change -> host.set_string,
        // exactly as a user typing would.
        te->set_text(utf8 ? utf8 : "");
        if (v->host) v->host->repaint();
        return PULP_EMBED_OK;
    } catch (const std::exception& e) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, e.what());
    } catch (...) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, "simulate_text_input threw");
    }
}

// Host -> view hover dispatch (no ABI bump — additive function).
//
// Embedded plugins host Pulp inside a JUCE/iPlug2/SDL component that owns
// its platform mouse-move events; pulp-view-embed itself never sees those
// events. Without forwarding, `View::set_hovered` is never called from any
// non-test path → `on_hover_enter` (wired by registerHover) never fires →
// JS `onMouseEnter` handlers stay silent. Host adapters override their own
// mouseMove and forward (x,y) here in root-view coords; the shim defers to
// `View::simulate_hover`, which performs the same hit-test + set_hovered
// hop a native Pulp window does on real mouse moves.
//
// Symmetric with the existing pulp_embed_simulate_* family. Named
// `dispatch_*` rather than `simulate_*` because the source IS a real
// pointer, not a synthetic test event.
PulpEmbedResult pulp_embed_dispatch_mouse_move(PulpEmbedView* v, double x, double y) {
    if (!v || !v->bridge) return PULP_EMBED_ERR_INVALID_ARG;
    try {
        auto* root = v->bridge->view();
        if (!root) return PULP_EMBED_ERR_INVALID_ARG;
        root->simulate_hover(pulp::view::Point{static_cast<float>(x),
                                                static_cast<float>(y)});
        if (v->host) v->host->repaint();
        return PULP_EMBED_OK;
    } catch (const std::exception& e) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, e.what());
    } catch (...) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, "dispatch_mouse_move threw");
    }
}

PulpEmbedResult pulp_embed_dispatch_mouse_exit(PulpEmbedView* v) {
    if (!v || !v->bridge) return PULP_EMBED_ERR_INVALID_ARG;
    try {
        auto* root = v->bridge->view();
        if (!root) return PULP_EMBED_ERR_INVALID_ARG;
        // Pass an out-of-bounds point so hit_test returns null and any
        // currently-hovered view clears (matches platform behaviour when
        // the pointer leaves the window).
        root->simulate_hover(pulp::view::Point{-1.0f, -1.0f});
        if (v->host) v->host->repaint();
        return PULP_EMBED_OK;
    } catch (const std::exception& e) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, e.what());
    } catch (...) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, "dispatch_mouse_exit threw");
    }
}

// Press / drag / release. hit_test the root at the (root-space) point, then
// dispatch to the target widget in ITS local coords (root point minus the
// widget's absolute origin). down captures the target on the view; drag/up
// replay against it. This is the minimal subset of the native plugin-view-host
// mouse path (no focus/bubbling) — enough to make knobs/faders/buttons drag.
static pulp::view::Point local_for(pulp::view::View* target, double x, double y) {
    const auto ab = pulp::view::ViewInspector::absolute_bounds(*target);
    return pulp::view::Point{static_cast<float>(x) - ab.x, static_cast<float>(y) - ab.y};
}

PulpEmbedResult pulp_embed_dispatch_mouse_down(PulpEmbedView* v, double x, double y) {
    if (!v || !v->bridge) return PULP_EMBED_ERR_INVALID_ARG;
    try {
        auto* root = v->bridge->view();
        if (!root) return PULP_EMBED_ERR_INVALID_ARG;
        v->drag_target = root->hit_test(pulp::view::Point{static_cast<float>(x),
                                                          static_cast<float>(y)});
        if (v->drag_target) {
            v->drag_target->on_mouse_down(local_for(v->drag_target, x, y));
            if (v->host) v->host->repaint();
        }
        return PULP_EMBED_OK;
    } catch (const std::exception& e) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, e.what());
    } catch (...) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, "dispatch_mouse_down threw");
    }
}

PulpEmbedResult pulp_embed_dispatch_mouse_drag(PulpEmbedView* v, double x, double y) {
    if (!v || !v->bridge) return PULP_EMBED_ERR_INVALID_ARG;
    try {
        if (!v->drag_target) return PULP_EMBED_OK;
        v->drag_target->on_mouse_drag(local_for(v->drag_target, x, y));
        if (v->host) v->host->repaint();
        return PULP_EMBED_OK;
    } catch (const std::exception& e) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, e.what());
    } catch (...) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, "dispatch_mouse_drag threw");
    }
}

PulpEmbedResult pulp_embed_dispatch_mouse_up(PulpEmbedView* v, double x, double y) {
    if (!v || !v->bridge) return PULP_EMBED_ERR_INVALID_ARG;
    try {
        if (v->drag_target) {
            v->drag_target->on_mouse_up(local_for(v->drag_target, x, y));
            if (v->host) v->host->repaint();
        }
        v->drag_target = nullptr;
        return PULP_EMBED_OK;
    } catch (const std::exception& e) {
        v->drag_target = nullptr;
        return set_err(v, PULP_EMBED_ERR_INTERNAL, e.what());
    } catch (...) {
        v->drag_target = nullptr;
        return set_err(v, PULP_EMBED_ERR_INTERNAL, "dispatch_mouse_up threw");
    }
}

PulpEmbedResult pulp_embed_param_hit_point(PulpEmbedView* v, int32_t index,
                                           double* out_x, double* out_y) {
    if (!v || !out_x || !out_y || index < 0 ||
        static_cast<size_t>(index) >= v->params.size())
        return PULP_EMBED_ERR_INVALID_ARG;
    try {
        auto& b = v->params[static_cast<size_t>(index)];
        // Only the design-frame lane can locate a control: its elements carry hit
        // geometry the view can map. A plain Knob/Fader/Toggle tree has no such
        // per-element anchor here, so report the capability as absent rather than
        // guessing a point that would miss.
        if (b.frame_element_index < 0 || !b.widget)
            return set_err(v, PULP_EMBED_ERR_UNSUPPORTED,
                           "control has no locatable geometry (not a design frame)");
        auto* frame = static_cast<pulp::view::DesignFrameView*>(b.widget);
        pulp::view::Point local{};
        if (!frame->element_hit_point(b.frame_element_index, local))
            return set_err(v, PULP_EMBED_ERR_UNSUPPORTED,
                           "element has no hit point (view not laid out?)");
        // Element-local -> root, the space the dispatchers hit-test in.
        const auto ab = pulp::view::ViewInspector::absolute_bounds(*frame);
        *out_x = static_cast<double>(ab.x + local.x);
        *out_y = static_cast<double>(ab.y + local.y);
        return PULP_EMBED_OK;
    } catch (const std::exception& e) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, e.what());
    } catch (...) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, "param_hit_point threw");
    }
}

PulpEmbedResult pulp_embed_capture_png(PulpEmbedView* v, uint8_t* out,
                                       size_t cap, size_t* out_len) {
    if (!v || !v->host || !out_len) return PULP_EMBED_ERR_INVALID_ARG;
    try {
        std::vector<uint8_t> png = v->host->capture_back_buffer_png();
        *out_len = png.size();
        if (png.empty())
            return set_err(v, PULP_EMBED_ERR_UNSUPPORTED, "no back-buffer capture (CPU host?)");
        if (!out) return PULP_EMBED_OK;  // sizing query
        if (cap < png.size()) return PULP_EMBED_ERR_BUFFER_TOO_SMALL;
        for (size_t i = 0; i < png.size(); ++i) out[i] = png[i];
        return PULP_EMBED_OK;
    } catch (const std::exception& e) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, e.what());
    } catch (...) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, "capture threw");
    }
}

PulpEmbedResult pulp_embed_render_png(PulpEmbedView* v, int32_t w, int32_t h,
                                      float scale, uint8_t* out, size_t cap,
                                      size_t* out_len) {
    if (!v || !v->bridge || !out_len || w <= 0 || h <= 0) return PULP_EMBED_ERR_INVALID_ARG;
    try {
        auto* rv = v->bridge->view();
        if (!rv) return set_err(v, PULP_EMBED_ERR_MATERIALIZE, "no view");
        rv->set_bounds({0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)});
        rv->layout_children();
        std::vector<uint8_t> png = pulp::view::render_to_png(
            *rv, static_cast<uint32_t>(w), static_cast<uint32_t>(h),
            scale > 0.0f ? scale : 1.0f, pulp::view::ScreenshotBackend::skia);
        *out_len = png.size();
        if (png.empty())
            return set_err(v, PULP_EMBED_ERR_UNSUPPORTED, "render_to_png produced no bytes");
        if (!out) return PULP_EMBED_OK;  // sizing query
        if (cap < png.size()) return PULP_EMBED_ERR_BUFFER_TOO_SMALL;
        for (size_t i = 0; i < png.size(); ++i) out[i] = png[i];
        return PULP_EMBED_OK;
    } catch (const std::exception& e) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, e.what());
    } catch (...) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, "render_png threw");
    }
}

int32_t pulp_embed_missing_asset_count(PulpEmbedView* v) {
    if (!v) return 0;
    return static_cast<int32_t>(v->missing_assets.size());
}

size_t pulp_embed_missing_asset(PulpEmbedView* v, int32_t index, char* buf, size_t cap) {
    if (!v || index < 0 || static_cast<size_t>(index) >= v->missing_assets.size()) {
        if (buf && cap > 0) buf[0] = '\0';
        return 0;
    }
    return copy_str(v->missing_assets[static_cast<size_t>(index)], buf, cap);
}

size_t pulp_embed_last_error(PulpEmbedView* v, char* buf, size_t cap) {
    if (!v) return 0;
    const auto& s = v->last_error;
    if (buf && cap) {
        const size_t n = s.size() < cap - 1 ? s.size() : cap - 1;
        for (size_t i = 0; i < n; ++i) buf[i] = s[i];
        buf[n] = '\0';
    }
    return s.size();
}

void pulp_embed_destroy(PulpEmbedView* v) {
    if (!v) return;
    try {
        // Teardown order: stop the host's loop/callbacks and drop it (it holds
        // a View& into the bridge-owned tree), THEN close the bridge (destroys
        // the view + fires on_view_closed iff opened), THEN drop proc/store.
        if (v->host) {
            v->host->set_idle_callback(nullptr);
            v->host->set_resize_callback(nullptr);
            v->host->detach();
            v->host.reset();
        }
        // For the high-fidelity scripted path the processor owns the
        // ScriptedUiSession (+ its WidgetBridge), which holds a View& into the
        // bridge-owned root. Destroy that session BEFORE the bridge frees the
        // root, or the WidgetBridge destructor touches freed memory. The
        // DesignIR/native path has no session and this is a no-op.
        if (auto* sp = dynamic_cast<EmbedScriptedProcessor*>(v->processor.get())) {
            sp->release_session();
        }
        if (v->bridge) { v->bridge->close(); v->bridge.reset(); }
        v->processor.reset();
        // Drop the param-bridge subscriptions (which capture `v`) BEFORE the
        // store they target is destroyed. The widgets the param bindings borrow
        // are already gone with the bridge above; null them defensively.
        v->param_listener.reset();
        if (v->store) v->store->set_gesture_callbacks(nullptr, nullptr);
        for (auto& b : v->params) b.widget = nullptr;
        v->store.reset();
        // Remove the host-resource staging dir (resolve_resource), if any. The
        // renderer is gone, so the staged files are no longer referenced.
        if (!v->staging_dir.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(v->staging_dir, ec);
        }
    } catch (...) {
        // swallow — destroy must not throw across the C boundary
    }
    delete v;
}

}  // extern "C"
