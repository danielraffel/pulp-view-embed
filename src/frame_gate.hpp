/*
 * frame_gate.hpp — the predicate behind pulp_embed_tick's opt-in dirty gate.
 *
 * Library-private (not part of the flat C ABI, not installed). Kept in its own
 * header so it can be unit-tested against hand-built View trees without a live
 * host — see test/frame_gate_test.cpp.
 *
 * Why a gate at all: pulp_embed_tick() is the host's periodic idle pump (the
 * JUCE/iPlug2 timer calls it ~30 Hz). By default it repaints unconditionally.
 * But every DISCRETE change already repaints on its own — the host->view push
 * paths (pulp_embed_param_changed, set_string, the mouse dispatchers, reload)
 * each call host->repaint() directly. So the only reason a *periodic* tick must
 * paint is ONGOING animation. This predicate answers "is anything animating?"
 * so an opted-in host can skip the redundant per-tick repaint and let a silent
 * editor idle to 0 fps.
 */
#ifndef PULP_VIEW_EMBED_FRAME_GATE_HPP
#define PULP_VIEW_EMBED_FRAME_GATE_HPP

#include <cstddef>

#include <pulp/view/frame_clock.hpp>
#include <pulp/view/view.hpp>

// The SDK promoted the hosts' private view_needs_continuous_frames() walk to a
// shared pulp::view::needs_continuous_frames(). Prefer it when the pinned SDK
// exports it: it additionally catches widget-level animation (knob hover glow,
// time-driven shaders, running CSS animations) that is NOT a FrameClock
// subscriber. Older pinned SDKs fall back to the frame-clock/layout signals,
// which are complete for GPU-backed embeds (the host display-link independently
// repaints widget animation there) and the intended target of the gate.
#if __has_include(<pulp/view/continuous_frames.hpp>)
#include <pulp/view/continuous_frames.hpp>
#define PULP_EMBED_HAS_CONTINUOUS_FRAMES 1
#else
#define PULP_EMBED_HAS_CONTINUOUS_FRAMES 0
#endif

namespace pulp::embed {

// A pending layout pass anywhere in the tree needs a paint. View::layout_dirty()
// is set on the RECEIVER of invalidate_layout(), not propagated to the root, so a
// root-only check would miss a dirty descendant — walk the subtree.
inline bool subtree_layout_dirty(pulp::view::View* v) {
    if (!v) return false;
    if (v->layout_dirty()) return true;
    for (std::size_t i = 0; i < v->child_count(); ++i)
        if (subtree_layout_dirty(v->child_at(i))) return true;
    return false;
}

// True when `root`'s tree needs a repaint on THIS periodic tick. A null root
// returns true (unknown → paint; the gate must never freeze a view it can't
// reason about). `root` is the embed's root view (v->bridge->view()); the host
// installs+ticks a FrameClock on it, and View::frame_clock() walks up the parent
// chain, so this resolves the installed clock for any node in the tree.
inline bool embed_view_needs_frame(pulp::view::View* root) {
    if (!root) return true;
#if PULP_EMBED_HAS_CONTINUOUS_FRAMES
    if (pulp::view::needs_continuous_frames(root)) return true;
#endif
    if (auto* fc = root->frame_clock(); fc && fc->has_active_subscribers())
        return true;
    if (subtree_layout_dirty(root)) return true;
    return false;
}

}  // namespace pulp::embed

#endif  // PULP_VIEW_EMBED_FRAME_GATE_HPP
