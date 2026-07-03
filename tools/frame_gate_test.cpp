// Unit test for embed_view_needs_frame() — the predicate behind
// pulp_embed_tick()'s opt-in dirty gate (pulp_embed_set_dirty_gate). Pure
// logic over a hand-built View tree, no host required. Exit 0 = all pass.

#include "../src/frame_gate.hpp"

#include <pulp/view/frame_clock.hpp>
#include <pulp/view/view.hpp>

#include <cstdio>

using pulp::embed::embed_view_needs_frame;
using pulp::view::FrameClock;
using pulp::view::View;

static int g_fail = 0;
static void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

int main() {
    // A null root must never freeze — unknown state paints.
    check(embed_view_needs_frame(nullptr), "null root -> needs frame");

    // A fresh, clockless, static tree is idle: nothing to animate.
    {
        View root;
        check(!embed_view_needs_frame(&root), "clockless idle tree -> no frame");
    }

    // A clock with no subscribers is still idle.
    {
        View root;
        FrameClock clock;
        root.set_frame_clock(&clock);
        check(!embed_view_needs_frame(&root),
              "clock, zero subscribers -> no frame");
        root.set_frame_clock(nullptr);
    }

    // A live FrameClock subscriber (a scripted rAF, a CSS animation, a
    // meter/scalar source) keeps frames flowing.
    {
        View root;
        FrameClock clock;
        root.set_frame_clock(&clock);
        const int id = clock.subscribe([](float) { return true; });
        check(embed_view_needs_frame(&root),
              "active subscriber -> needs frame");
        clock.unsubscribe(id);
        check(!embed_view_needs_frame(&root),
              "after unsubscribe -> no frame");
        root.set_frame_clock(nullptr);
    }

    // The clock resolves through the parent chain: the gate can be asked about
    // any node, not just the root the host installed the clock on.
    {
        View root;
        FrameClock clock;
        root.set_frame_clock(&clock);
        auto child = std::make_unique<View>();
        View* c = child.get();
        root.add_child(std::move(child));
        const int id = clock.subscribe([](float) { return true; });
        check(embed_view_needs_frame(c),
              "descendant resolves installed clock -> needs frame");
        clock.unsubscribe(id);
        root.set_frame_clock(nullptr);
    }

    // A pending layout pass needs a paint even with no clock/animation.
    {
        View root;
        root.invalidate_layout();
        check(embed_view_needs_frame(&root), "layout dirty -> needs frame");
    }

    // invalidate_layout marks only the receiver, so a dirty DESCENDANT (not the
    // root) must still be caught by the subtree walk.
    {
        View root;
        auto child = std::make_unique<View>();
        View* c = child.get();
        root.add_child(std::move(child));
        c->invalidate_layout();
        check(embed_view_needs_frame(&root),
              "descendant layout dirty -> needs frame");
    }

    std::printf(g_fail ? "\nFAILED (%d)\n" : "\nPASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
