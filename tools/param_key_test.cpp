// Live-key + host-action + step-count tests for the embed's parameter bridge,
// driven through the REAL C ABI against a synthetic host.
//
// Covers three seams a foreign host depends on:
//   1. Runtime re-key (set_element_param_key) — a paged/tabbed control that
//      re-points an element at another host parameter must move BOTH directions
//      with it: UI->host writes/gestures and host->UI pushes.
//   2. The host-action channel — a view calling host_actions()->send_host_action
//      reaches the ABI's host_action callback.
//   3. The host step-count callback + its struct_size gating, and the surface it
//      backs: a choice control must scale by the HOST parameter's value count,
//      not by the number of positions it happens to draw.
//
// A native DesignFrameView (pulp_embed_create_from_view) is the fixture: it is
// the only lane that can re-key at runtime, and it needs no design fixture on
// disk. Exit 0 = all pass.

#include "pulp_view_embed.h"
#include "pulp_view_embed_native.hpp"

#include <pulp/view/design_frame_view.hpp>

#include <cmath>
#include <cstdio>
#include <cstddef>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

int g_fail = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

bool approx(double a, double b) { return std::fabs(a - b) < 1e-3; }

// Records everything the embed pushes at the host, and answers the host->view
// queries from scriptable state.
struct FakeHost {
    std::vector<std::pair<std::string, double>> sets;
    std::vector<std::string>                    begins;
    std::vector<std::string>                    ends;
    std::vector<std::pair<std::string, std::string>> actions;
    std::set<std::string>                       members;  // has_param answers
    std::map<std::string, int>                  steps;    // host_param_steps answers

    static FakeHost& of(void* ctx) { return *static_cast<FakeHost*>(ctx); }

    static void setParam(void* c, const char* k, double n) { of(c).sets.emplace_back(k, n); }
    static double getParam(void*, const char*) { return -1.0; }  // no host opinion
    static void begin(void* c, const char* k) { of(c).begins.emplace_back(k); }
    static void end(void* c, const char* k) { of(c).ends.emplace_back(k); }
    static int has(void* c, const char* k) { return of(c).members.count(k) ? 1 : 0; }
    static int action(void* c, const char* a, const char* j) {
        of(c).actions.emplace_back(a ? a : "", j ? j : "");
        return 1;
    }
    static int32_t stepsOf(void* c, const char* k) {
        const auto it = of(c).steps.find(k ? k : "");
        return it == of(c).steps.end() ? 0 : it->second;
    }
    // Echo the key back in the text so a test can prove WHICH key was resolved.
    static size_t displayText(void*, const char* k, double, char* buf, size_t cap) {
        const std::string s = std::string("disp:") + (k ? k : "");
        if (buf && cap) {
            const size_t n = s.size() < cap - 1 ? s.size() : cap - 1;
            std::memcpy(buf, s.data(), n);
            buf[n] = '\0';
        }
        return s.size();
    }
};

constexpr int   kRackIdx = 0;      // the element the tests re-key
constexpr float kW = 240.0f, kH = 80.0f;
// The action button's rect, in SVG coords. The SVG's intrinsic size matches the
// view's logical size, so the panel transform is identity and these are also the
// view-space coords a mouse dispatch must use.
constexpr float kActionX = 100.0f, kActionY = 4.0f, kActionW = 40.0f, kActionH = 20.0f;

pulp::view::DesignFrameView* g_frame = nullptr;  // borrowed; owned by the view tree

std::unique_ptr<pulp::view::View> makeView() {
    using El = pulp::view::DesignFrameElement;
    auto knob = [](float cx, float value, std::string key) {
        El e;
        e.kind = El::Kind::knob;
        e.cx = cx; e.cy = 40.0f; e.hit_radius = 18.0f;
        e.needle_d = "M0 0L0 -8";
        e.value = value;
        e.param_key = std::move(key);
        return e;
    };
    El act;
    act.kind = El::Kind::action;
    act.action = "page_next";
    act.x = kActionX; act.y = kActionY; act.w = kActionW; act.h = kActionH;

    std::vector<El> els{knob(60.0f, 0.5f, "slot0.gain"),
                        knob(180.0f, 0.25f, "other"),
                        act};
    const std::string svg =
        R"(<svg width="240" height="80" xmlns="http://www.w3.org/2000/svg">)"
        R"(<rect x="0" y="0" width="240" height="80" fill="#222"/></svg>)";
    auto view = std::make_unique<pulp::view::DesignFrameView>(svg, std::move(els));
    // Opt the action button into the host channel — OFF by default in the SDK.
    view->route_actions_to_host(true);
    g_frame = view.get();
    return view;
}

// ── choice fixture: a 3-position control over a 6-value host parameter ───────
// Deliberately a SEPARATE view from makeView(): the tests above pin element
// indices and param_count against that one, and a choice control's whole point
// here is that its own option count must NOT be what scales it.
// The rect spans the whole frame, and the SVG's intrinsic size matches the
// view's logical size (kW x kH) so the panel transform is identity — SVG coords
// are also the view coords a mouse dispatch takes. A smaller SVG here would be
// scaled to the logical size and every click would land on the wrong tab.
constexpr float kChoiceX = 0.0f, kChoiceY = 0.0f, kChoiceW = kW, kChoiceH = kH;
constexpr int   kChoiceOptions = 3;

pulp::view::DesignFrameView* g_choice_frame = nullptr;  // borrowed; owned by the tree

std::unique_ptr<pulp::view::View> makeChoiceView() {
    using El = pulp::view::DesignFrameElement;
    El tabs;
    tabs.kind = El::Kind::tab_group;
    tabs.x = kChoiceX; tabs.y = kChoiceY; tabs.w = kChoiceW; tabs.h = kChoiceH;
    tabs.options = {"A", "B", "C"};   // 3 drawn positions
    tabs.selected_index = 0;
    tabs.param_key = "mode";          // ...bound to a 6-value host parameter

    const std::string svg =
        R"(<svg width="240" height="80" xmlns="http://www.w3.org/2000/svg">)"
        R"(<rect x="0" y="0" width="240" height="80" fill="#222"/></svg>)";
    auto view = std::make_unique<pulp::view::DesignFrameView>(svg, std::vector<El>{tabs});
    g_choice_frame = view.get();
    return view;
}

// Click the center of tab `index`. Goes through the real ABI mouse dispatch, so
// this exercises the same path a user tap does: DesignTabGroup hit-test ->
// on_select -> the frame's choice_to_norm -> on_element_changed -> store ->
// listener -> host.set_param.
void clickTab(PulpEmbedView* v, int index) {
    const double cell = kChoiceW / static_cast<double>(kChoiceOptions);
    const double cx = kChoiceX + cell * (static_cast<double>(index) + 0.5);
    const double cy = kChoiceY + kChoiceH / 2.0;
    pulp_embed_dispatch_mouse_down(v, cx, cy);
    pulp_embed_dispatch_mouse_up(v, cx, cy);
}

// Fill a descriptor wired to `host`. `struct_size`/`abi_version` are left at the
// current ABI; a caller simulating an older host overrides them.
PulpEmbedDesc makeDesc(FakeHost& host) {
    PulpEmbedDesc d{};
    d.struct_size = sizeof(PulpEmbedDesc);
    d.abi_version = PULP_VIEW_EMBED_ABI_VERSION;
    d.logical_width = static_cast<int32_t>(kW);
    d.logical_height = static_cast<int32_t>(kH);
    d.scale_factor = 1.0f;
    d.backend_pref = PULP_EMBED_BACKEND_PREF_CPU;
    d.host_ctx = &host;
    d.host.set_param = &FakeHost::setParam;
    d.host.get_param = &FakeHost::getParam;
    d.host.begin_gesture = &FakeHost::begin;
    d.host.end_gesture = &FakeHost::end;
    d.host.has_param = &FakeHost::has;
    d.host.param_display_text = &FakeHost::displayText;
    d.host.host_action = &FakeHost::action;
    d.host.host_param_steps = &FakeHost::stepsOf;
    return d;
}

std::string paramKey(PulpEmbedView* v, int32_t i) {
    char buf[128] = {0};
    pulp_embed_param_key(v, i, buf, sizeof buf);
    return buf;
}

std::string displayAt(PulpEmbedView* v, int32_t i) {
    char buf[128] = {0};
    pulp_embed_param_display_text(v, i, buf, sizeof buf);
    return buf;
}

// ── the re-key round trip: both directions must follow the new key ───────────
void testRekey() {
    FakeHost host;
    host.members = {"slot0.gain", "slot1.gain", "other"};
    PulpEmbedDesc d = makeDesc(host);
    PulpEmbedView* v = nullptr;
    if (pulp::embed::pulp_embed_create_from_view(&d, &makeView, &v) != PULP_EMBED_OK || !v) {
        char err[512] = {0};
        pulp_embed_last_create_error(err, sizeof err);
        check(false, "create_from_view (re-key fixture)");
        std::printf("     create error: %s\n", err);
        return;
    }

    check(pulp_embed_param_count(v) == 2, "action element is not a bindable param");
    check(paramKey(v, kRackIdx) == "slot0.gain", "element 0 starts on slot0.gain");

    g_frame->set_element_param_key(kRackIdx, "slot1.gain");

    // (c) enumeration + the host-surface snapshots report the NEW key.
    check(paramKey(v, kRackIdx) == "slot1.gain", "param_key reports the new key");
    check(pulp_embed_param_has(v, "slot1.gain") == 1,
          "param_has resolves the new key");
    check(pulp_embed_param_has(v, "slot0.gain") == -1,
          "param_has reports the old key as no longer a control");
    check(displayAt(v, kRackIdx) == "disp:slot1.gain",
          "display-text snapshot re-resolved under the new key");

    // (a) host->UI: the new key moves element 0; the old key is an inert no-op.
    check(pulp_embed_param_changed(v, "slot1.gain", 0.9) == PULP_EMBED_OK,
          "push under the new key returns OK");
    check(approx(pulp_embed_param_value(v, kRackIdx), 0.9),
          "push under the new key moved element 0");
    check(pulp_embed_param_changed(v, "slot0.gain", 0.1) == PULP_EMBED_OK,
          "push under the stale key returns OK (blind-push contract)");
    check(approx(pulp_embed_param_value(v, kRackIdx), 0.9),
          "push under the stale key did NOT move element 0");

    // (b) UI->host: a drag forwards set_param + gesture brackets under the NEW key.
    host.sets.clear(); host.begins.clear(); host.ends.clear();
    check(pulp_embed_simulate_param_drag(v, kRackIdx, 0.3) == PULP_EMBED_OK,
          "simulate_param_drag on the re-keyed element");
    const bool setsOk = !host.sets.empty() &&
                        host.sets.back().first == "slot1.gain" &&
                        approx(host.sets.back().second, 0.3);
    check(setsOk, "UI drag forwarded set_param under the new key");
    check(!host.begins.empty() && host.begins.back() == "slot1.gain",
          "begin_gesture bracketed under the new key");
    check(!host.ends.empty() && host.ends.back() == "slot1.gain",
          "end_gesture bracketed under the new key");
    bool anyStale = false;
    for (const auto& s : host.sets) if (s.first == "slot0.gain") anyStale = true;
    check(!anyStale, "no write escaped to the stale key");

    pulp_embed_destroy(v);
}

// ── the host-action channel reaches the C callback ───────────────────────────
void testHostAction() {
    FakeHost host;
    PulpEmbedDesc d = makeDesc(host);
    PulpEmbedView* v = nullptr;
    if (pulp::embed::pulp_embed_create_from_view(&d, &makeView, &v) != PULP_EMBED_OK || !v) {
        check(false, "create_from_view (host-action fixture)");
        return;
    }

    const double cx = kActionX + kActionW / 2.0;
    const double cy = kActionY + kActionH / 2.0;
    pulp_embed_dispatch_mouse_down(v, cx, cy);
    pulp_embed_dispatch_mouse_up(v, cx, cy);

    check(host.actions.size() == 1, "action click fired exactly one host_action");
    if (!host.actions.empty()) {
        check(host.actions[0].first == "page_next", "host_action carried the action name");
        check(host.actions[0].second == "{}", "host_action carried the args payload");
    }

    // Reload-safety proxy: rebuilding the bridge must re-point the surface at the
    // live callback rather than strand it. The native lane has no reload_bundle,
    // so the equivalent proof is that a SECOND view over the same factory wires
    // its own surface and still reaches its own host.
    FakeHost host2;
    PulpEmbedDesc d2 = makeDesc(host2);
    PulpEmbedView* v2 = nullptr;
    if (pulp::embed::pulp_embed_create_from_view(&d2, &makeView, &v2) == PULP_EMBED_OK && v2) {
        pulp_embed_dispatch_mouse_down(v2, cx, cy);
        pulp_embed_dispatch_mouse_up(v2, cx, cy);
        check(host2.actions.size() == 1 && host.actions.size() == 1,
              "a second view routes actions to its own host, not the first");
        pulp_embed_destroy(v2);
    } else {
        check(false, "create_from_view (second host-action view)");
    }

    pulp_embed_destroy(v);
}

// ── a host with no host_action wired leaves the SDK channel unwired ──────────
void testHostActionAbsent() {
    FakeHost host;
    PulpEmbedDesc d = makeDesc(host);
    d.host.host_action = nullptr;
    PulpEmbedView* v = nullptr;
    if (pulp::embed::pulp_embed_create_from_view(&d, &makeView, &v) != PULP_EMBED_OK || !v) {
        check(false, "create_from_view (no host_action)");
        return;
    }
    const double cx = kActionX + kActionW / 2.0;
    const double cy = kActionY + kActionH / 2.0;
    pulp_embed_dispatch_mouse_down(v, cx, cy);
    pulp_embed_dispatch_mouse_up(v, cx, cy);
    check(host.actions.empty(), "no host_action callback -> no action forwarded");
    pulp_embed_destroy(v);
}

// ── step count: reported per key, and struct_size-gated ──────────────────────
void testStepCount() {
    FakeHost host;
    host.steps["slot0.gain"] = 6;  // a 6-step host param behind a knob
    PulpEmbedDesc d = makeDesc(host);
    PulpEmbedView* v = nullptr;
    if (pulp::embed::pulp_embed_create_from_view(&d, &makeView, &v) != PULP_EMBED_OK || !v) {
        check(false, "create_from_view (step-count fixture)");
        return;
    }
    check(pulp_embed_param_steps(v, "slot0.gain") == 6,
          "a 6-step host param reports 6");
    check(pulp_embed_param_steps(v, "other") == 0,
          "a continuous host param reports 0");
    check(pulp_embed_param_steps(v, "nonexistent") == 0,
          "an unknown key reports 0 (continuous/unknown)");
    check(pulp_embed_param_steps(nullptr, "slot0.gain") == 0, "NULL view reports 0");
    check(pulp_embed_param_steps(v, nullptr) == 0, "NULL key reports 0");

    // A re-key must re-resolve the step count under the new key.
    host.steps["slot1.gain"] = 3;
    g_frame->set_element_param_key(kRackIdx, "slot1.gain");
    check(pulp_embed_param_steps(v, "slot1.gain") == 3,
          "step count re-resolves under a new key");
    pulp_embed_destroy(v);
}

// ── the motivating case: a 3-option control on a 6-value host parameter ──────
// The control draws 3 positions; the parameter has 6 values. The third position
// is value index 2 of 6, so it must emit 2/5 = 0.4. Scaling by what the control
// draws yields 2/2 = 1.0 and slams the host to the parameter's LAST value —
// the defect a consumer was working around with a per-control denominator
// override. This is the end-to-end proof, through the embed's own ABI.
void testChoiceScalesByHostStepCount() {
    FakeHost host;
    host.members = {"mode"};
    host.steps["mode"] = 6;  // the host's parameter carries 6 values
    PulpEmbedDesc d = makeDesc(host);
    PulpEmbedView* v = nullptr;
    if (pulp::embed::pulp_embed_create_from_view(&d, &makeChoiceView, &v) != PULP_EMBED_OK || !v) {
        char err[512] = {0};
        pulp_embed_last_create_error(err, sizeof err);
        check(false, "create_from_view (choice fixture)");
        std::printf("     create error: %s\n", err);
        return;
    }

    check(pulp_embed_param_steps(v, "mode") == 6,
          "the 6-value host param reports 6 through the snapshot");

    host.sets.clear();
    clickTab(v, 2);  // the third drawn position = value index 2 of 6

    const bool emitted = !host.sets.empty() && host.sets.back().first == "mode";
    check(emitted, "clicking the third position wrote to the host under 'mode'");
    if (emitted) {
        const double got = host.sets.back().second;
        check(approx(got, 2.0 / 5.0),
              "a 3-option control on a 6-value param emits idx/5, not idx/2");
        // Name the specific regression: dividing by the drawn option count.
        check(!approx(got, 1.0),
              "the third position does NOT slam the host to the param's last value");
        if (!approx(got, 2.0 / 5.0)) std::printf("     emitted %.6f, expected %.6f\n", got, 2.0 / 5.0);
    }

    // Index 1 of 6 is 1/5 = 0.2 (the buggy divisor would give 1/2 = 0.5).
    host.sets.clear();
    clickTab(v, 1);
    check(!host.sets.empty() && approx(host.sets.back().second, 1.0 / 5.0),
          "the second position emits 1/5");

    // Index 0 pins to 0.0 under either divisor — it must still hold.
    host.sets.clear();
    clickTab(v, 0);
    check(!host.sets.empty() && approx(host.sets.back().second, 0.0),
          "the first position emits 0");

    pulp_embed_destroy(v);
}

// A continuous host parameter reports 0 ("no index domain"). The surface must
// never divide by it: the control keeps its own positions as the only domain
// available, and the guess is reported rather than silently absorbed.
void testChoiceOnContinuousHostParam() {
    FakeHost host;
    host.members = {"mode"};
    // No host.steps entry -> stepsOf answers 0 = continuous/unknown.
    PulpEmbedDesc d = makeDesc(host);
    PulpEmbedView* v = nullptr;
    if (pulp::embed::pulp_embed_create_from_view(&d, &makeChoiceView, &v) != PULP_EMBED_OK || !v) {
        check(false, "create_from_view (continuous-param fixture)");
        return;
    }
    check(pulp_embed_param_steps(v, "mode") == 0, "a continuous host param reports 0");

    host.sets.clear();
    clickTab(v, 2);
    // Documented fallback: 0 is not a divisor, so the element's own 3 positions
    // are used — 2/2 = 1.0. Correct HERE (the param has no index domain), and
    // reported via the SDK's scale-mismatch diagnostic rather than assumed.
    check(!host.sets.empty() && approx(host.sets.back().second, 1.0),
          "a 0 step count falls back to the control's own option count");
    pulp_embed_destroy(v);
}

// The struct_size gate still holds with the surface installed: a host block that
// stops before host_param_steps must degrade to "cannot answer" (0) — never read
// the absent tail and never produce a garbage divisor.
void testChoiceUnderPreviousAbiStructSize() {
    FakeHost host;
    host.members = {"mode"};
    host.steps["mode"] = 6;
    PulpEmbedDesc d = makeDesc(host);
    d.abi_version = PULP_VIEW_EMBED_ABI_VERSION - 1u;
    d.struct_size = static_cast<uint32_t>(offsetof(PulpEmbedDesc, host) +
                                          offsetof(PulpEmbedHostCallbacks, host_param_steps));
    PulpEmbedView* v = nullptr;
    if (pulp::embed::pulp_embed_create_from_view(&d, &makeChoiceView, &v) != PULP_EMBED_OK || !v) {
        check(false, "a previous-ABI-sized desc is accepted with a choice control");
        return;
    }
    check(pulp_embed_param_steps(v, "mode") == 0,
          "the gated-out host_param_steps reads as 0, not garbage");
    host.sets.clear();
    clickTab(v, 2);
    // Degrades to the control's own count — the pre-existing behavior, not a
    // crash and not a divisor invented from unread memory.
    check(!host.sets.empty() && approx(host.sets.back().second, 1.0),
          "an older host degrades to the option count without crashing");
    pulp_embed_destroy(v);
}

// A negative step count is not a second "don't know" answer — it clamps to 0.
void testNegativeStepsClamp() {
    FakeHost host;
    host.steps["slot0.gain"] = -5;
    PulpEmbedDesc d = makeDesc(host);
    PulpEmbedView* v = nullptr;
    if (pulp::embed::pulp_embed_create_from_view(&d, &makeView, &v) != PULP_EMBED_OK || !v) {
        check(false, "create_from_view (negative-steps fixture)");
        return;
    }
    check(pulp_embed_param_steps(v, "slot0.gain") == 0,
          "a negative host step count clamps to 0");
    pulp_embed_destroy(v);
}

// A caller built against the PREVIOUS ABI hands a desc that stops before
// host_param_steps. The shim must accept it and read the absent tail as NULL —
// even though this test deliberately leaves a live pointer in the (larger)
// struct it actually passes, which only struct_size gating can hide.
void testPreviousAbiStructSizeGating() {
    FakeHost host;
    host.steps["slot0.gain"] = 6;
    PulpEmbedDesc d = makeDesc(host);
    d.abi_version = PULP_VIEW_EMBED_ABI_VERSION - 1u;
    d.struct_size = static_cast<uint32_t>(offsetof(PulpEmbedDesc, host) +
                                          offsetof(PulpEmbedHostCallbacks, host_param_steps));

    PulpEmbedView* v = nullptr;
    if (pulp::embed::pulp_embed_create_from_view(&d, &makeView, &v) != PULP_EMBED_OK || !v) {
        char err[512] = {0};
        pulp_embed_last_create_error(err, sizeof err);
        check(false, "a previous-ABI-sized desc is still accepted");
        std::printf("     create error: %s\n", err);
        return;
    }
    check(true, "a previous-ABI-sized desc is still accepted");
    check(pulp_embed_param_count(v) == 2,
          "the older desc's param bridge still binds");
    check(pulp_embed_param_steps(v, "slot0.gain") == 0,
          "host_param_steps past the caller's struct_size is not read");
    // The callbacks INSIDE the older caller's struct must still be captured.
    check(pulp_embed_param_has(v, "other") == 0,
          "has_param inside the older struct is still wired");
    pulp_embed_destroy(v);
}

// A duplicate key is a contract violation, not a silent last-writer-wins.
void testDuplicateKeyIsReported() {
    FakeHost host;
    PulpEmbedDesc d = makeDesc(host);
    PulpEmbedView* v = nullptr;
    if (pulp::embed::pulp_embed_create_from_view(&d, &makeView, &v) != PULP_EMBED_OK || !v) {
        check(false, "create_from_view (duplicate-key fixture)");
        return;
    }
    // Re-key element 0 onto element 1's key.
    g_frame->set_element_param_key(kRackIdx, "other");
    char err[512] = {0};
    pulp_embed_last_error(v, err, sizeof err);
    check(std::string(err).find("other") != std::string::npos,
          "a duplicate param key is reported in last_error");
    pulp_embed_destroy(v);
}

}  // namespace

int main() {
    testRekey();
    testHostAction();
    testHostActionAbsent();
    testStepCount();
    testNegativeStepsClamp();
    testPreviousAbiStructSizeGating();
    testChoiceScalesByHostStepCount();
    testChoiceOnContinuousHostParam();
    testChoiceUnderPreviousAbiStructSize();
    testDuplicateKeyIsReported();
    std::printf("%s\n", g_fail == 0 ? "param-key test: all pass"
                                    : "param-key test: FAILURES");
    return g_fail == 0 ? 0 : 1;
}
