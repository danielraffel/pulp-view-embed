// Control-geometry tests for pulp_embed_param_hit_point (ABI v11), driven
// through the REAL C ABI against a synthetic host.
//
// The verb turns a control index into the root-view point a pointer event must
// land on. Its whole reason to exist is that the point feeds
// pulp_embed_dispatch_mouse_down/drag/up, so the tests never stop at the
// returned numbers — they press at the point and assert the RIGHT control moved.
// A coordinate that looks plausible but misses reads exactly like a dead control,
// so "it hit what it named" is the property under test, at several window sizes.
//
// A native DesignFrameView (pulp_embed_create_from_view) is the fixture: it needs
// no design fixture on disk. Exit 0 = all pass.

#include "pulp_view_embed.h"
#include "pulp_view_embed_native.hpp"

#include <pulp/view/design_frame_view.hpp>

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

int g_fail = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

bool approx(double a, double b, double tol = 1e-3) { return std::fabs(a - b) < tol; }

// Records the gesture brackets and writes the embed pushes at the host, so a test
// can prove WHICH control a click landed on.
struct FakeHost {
    std::vector<std::pair<std::string, double>> sets;
    std::vector<std::string>                    begins;
    std::vector<std::string>                    ends;

    static FakeHost& of(void* ctx) { return *static_cast<FakeHost*>(ctx); }
    static void setParam(void* c, const char* k, double n) { of(c).sets.emplace_back(k, n); }
    static double getParam(void*, const char*) { return -1.0; }  // no host opinion
    static void begin(void* c, const char* k) { of(c).begins.emplace_back(k); }
    static void end(void* c, const char* k) { of(c).ends.emplace_back(k); }
};

// The design, in SVG coords. The panel is the full-bleed background rect, so the
// panel origin is (0,0) and the panel size IS the logical size — which makes the
// panel->view fit the identity at kW x kH, and every expected point below a
// hand-computed number rather than a re-derivation of the transform.
constexpr float kW = 240.0f, kH = 80.0f;
constexpr float kKnob0Cx = 60.0f, kKnob0Cy = 40.0f;
constexpr float kKnob1Cx = 180.0f, kKnob1Cy = 40.0f;
// The toggle is rect-hit rather than pivot-hit; its anchor is the rect center.
constexpr float kTogX = 10.0f, kTogY = 60.0f, kTogW = 20.0f, kTogH = 12.0f;
constexpr int   kTogIdx = 2;  // the toggle's element index in makeView()'s list

pulp::view::DesignFrameView* g_frame = nullptr;  // borrowed; owned by the view tree

std::unique_ptr<pulp::view::View> makeView() {
    using El = pulp::view::DesignFrameElement;
    auto knob = [](float cx, float cy, float value, std::string key) {
        El e;
        e.kind = El::Kind::knob;
        e.cx = cx; e.cy = cy; e.hit_radius = 18.0f;
        e.needle_d = "M0 0L0 -8";
        e.value = value;
        e.param_key = std::move(key);
        return e;
    };
    El tog;
    tog.kind = El::Kind::toggle;
    tog.x = kTogX; tog.y = kTogY; tog.w = kTogW; tog.h = kTogH;
    tog.value = 0.0f;
    tog.param_key = "bypass";

    std::vector<El> els{knob(kKnob0Cx, kKnob0Cy, 0.5f, "gain"),
                        knob(kKnob1Cx, kKnob1Cy, 0.25f, "tone"),
                        tog};
    const std::string svg =
        R"(<svg width="240" height="80" xmlns="http://www.w3.org/2000/svg">)"
        R"(<rect x="0" y="0" width="240" height="80" fill="#222"/></svg>)";
    auto view = std::make_unique<pulp::view::DesignFrameView>(svg, std::move(els));
    g_frame = view.get();
    return view;
}

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
    return d;
}

std::string paramKey(PulpEmbedView* v, int32_t i) {
    char buf[128] = {0};
    pulp_embed_param_key(v, i, buf, sizeof buf);
    return buf;
}

int indexOfKey(PulpEmbedView* v, const std::string& key) {
    for (int32_t i = 0; i < pulp_embed_param_count(v); ++i)
        if (paramKey(v, i) == key) return i;
    return -1;
}

PulpEmbedView* create(FakeHost& host, PulpEmbedDesc& d) {
    PulpEmbedView* v = nullptr;
    if (pulp::embed::pulp_embed_create_from_view(&d, &makeView, &v) != PULP_EMBED_OK || !v) {
        char err[512] = {0};
        pulp_embed_last_create_error(err, sizeof err);
        check(false, "create_from_view");
        std::printf("     create error: %s\n", err);
        return nullptr;
    }
    return v;
}

// ── the point is where the control is drawn, and pressing it grabs THAT one ───
void testHitPointLocatesEachControl() {
    FakeHost host;
    PulpEmbedDesc d = makeDesc(host);
    PulpEmbedView* v = create(host, d);
    if (!v) return;

    const int gain = indexOfKey(v, "gain");
    const int tone = indexOfKey(v, "tone");
    const int byp  = indexOfKey(v, "bypass");
    check(gain >= 0 && tone >= 0 && byp >= 0, "all three controls are bindable params");
    if (gain < 0 || tone < 0 || byp < 0) { pulp_embed_destroy(v); return; }

    double x = 0, y = 0;
    // A knob's anchor is its pivot; the fit is the identity here, so the pivot IS
    // the view point.
    check(pulp_embed_param_hit_point(v, gain, &x, &y) == PULP_EMBED_OK,
          "hit_point succeeds for a knob");
    check(approx(x, kKnob0Cx) && approx(y, kKnob0Cy), "knob hit point is its pivot");

    check(pulp_embed_param_hit_point(v, tone, &x, &y) == PULP_EMBED_OK,
          "hit_point succeeds for the second knob");
    check(approx(x, kKnob1Cx) && approx(y, kKnob1Cy),
          "each knob reports its OWN pivot, not a shared origin");

    // A rect-hit control's anchor is its rect center, NOT the pivot fields it
    // never set — reporting (0,0) here would miss the control entirely.
    check(pulp_embed_param_hit_point(v, byp, &x, &y) == PULP_EMBED_OK,
          "hit_point succeeds for a rect-hit toggle");
    check(approx(x, kTogX + kTogW / 2) && approx(y, kTogY + kTogH / 2),
          "toggle hit point is its rect center");

    // The point is only worth anything if it actually lands: press each knob and
    // assert the gesture bracketed under THAT knob's key. This is what a direct
    // widget poke cannot prove — it never runs the hit-test.
    pulp_embed_param_hit_point(v, gain, &x, &y);
    host.begins.clear();
    pulp_embed_dispatch_mouse_down(v, x, y);
    pulp_embed_dispatch_mouse_up(v, x, y);
    check(host.begins.size() == 1 && host.begins[0] == "gain",
          "pressing the gain hit point grabbed gain");

    pulp_embed_param_hit_point(v, tone, &x, &y);
    host.begins.clear();
    pulp_embed_dispatch_mouse_down(v, x, y);
    pulp_embed_dispatch_mouse_up(v, x, y);
    check(host.begins.size() == 1 && host.begins[0] == "tone",
          "pressing the tone hit point grabbed tone, not the nearer-to-origin knob");

    pulp_embed_destroy(v);
}

// ── a real drag from the point moves the value and brackets the gesture ───────
void testDragFromHitPointMovesTheControl() {
    FakeHost host;
    PulpEmbedDesc d = makeDesc(host);
    PulpEmbedView* v = create(host, d);
    if (!v) return;

    const int gain = indexOfKey(v, "gain");
    double x = 0, y = 0;
    check(pulp_embed_param_hit_point(v, gain, &x, &y) == PULP_EMBED_OK, "hit_point for the drag");

    const double before = pulp_embed_param_value(v, gain);
    pulp_embed_dispatch_mouse_down(v, x, y);
    pulp_embed_dispatch_mouse_drag(v, x, y - 30);  // up = increase
    pulp_embed_dispatch_mouse_up(v, x, y - 30);
    const double after = pulp_embed_param_value(v, gain);

    check(after > before, "dragging up from the hit point raised the value");
    check(!host.sets.empty() && host.sets.back().first == "gain",
          "the drag forwarded set_param under the control's key");
    check(approx(host.sets.back().second, after),
          "the value forwarded to the host is the value the control holds");
    check(host.begins.size() == 1 && host.begins[0] == "gain" &&
          host.ends.size() == 1 && host.ends[0] == "gain",
          "the drag bracketed exactly one begin/end gesture on the key");

    pulp_embed_destroy(v);
}

// ── the point follows the live layout, so it keeps hitting after a resize ─────
void testHitPointTracksLayout() {
    FakeHost host;
    PulpEmbedDesc d = makeDesc(host);
    PulpEmbedView* v = create(host, d);
    if (!v) return;

    const int gain = indexOfKey(v, "gain");
    double x = 0, y = 0;

    // Uniform 2x, same aspect: no letterbox, everything doubles.
    check(pulp_embed_resize(v, 480, 160, 1.0f) == PULP_EMBED_OK, "resize to 2x");
    check(pulp_embed_param_hit_point(v, gain, &x, &y) == PULP_EMBED_OK, "hit_point at 2x");
    check(approx(x, kKnob0Cx * 2) && approx(y, kKnob0Cy * 2), "hit point scaled with the view");

    host.begins.clear();
    pulp_embed_dispatch_mouse_down(v, x, y);
    pulp_embed_dispatch_mouse_up(v, x, y);
    check(host.begins.size() == 1 && host.begins[0] == "gain", "the 2x point still grabs gain");

    // Taller than the panel aspect: uniform fit scale = min(240/240, 160/80) = 1,
    // centered, so the design letterboxes with oy = (160-80)/2 = 40 and the knob
    // slides DOWN by 40 while x stays put. A caller that assumed the design fills
    // the window would miss here.
    check(pulp_embed_resize(v, 240, 160, 1.0f) == PULP_EMBED_OK, "resize to a letterboxed aspect");
    check(pulp_embed_param_hit_point(v, gain, &x, &y) == PULP_EMBED_OK, "hit_point when letterboxed");
    check(approx(x, kKnob0Cx) && approx(y, kKnob0Cy + 40),
          "hit point followed the letterbox offset");

    host.begins.clear();
    pulp_embed_dispatch_mouse_down(v, x, y);
    pulp_embed_dispatch_mouse_up(v, x, y);
    check(host.begins.size() == 1 && host.begins[0] == "gain",
          "the letterboxed point still grabs gain");

    pulp_embed_destroy(v);
}

// ── a control nothing can hit reports no point, rather than a plausible one ───
// Every case above asks for the point of a control that IS reachable. This one
// asks for a control that is not: a disabled element is skipped by the view's
// hit-tester, so NO point reaches it. The verb's contract is to say so — a
// coordinate that misses is indistinguishable from a dead control, which is the
// failure the verb exists to prevent.
//
// This is also the tripwire for which SDK the shim actually linked against. The
// accessor only refuses here because it round-trips its candidate point through
// the real hit-tester before promising it; an SDK build whose accessor merely
// computes the anchor returns a point for a control nothing can reach, and this
// test goes red. That makes a wrong-SDK link a loud failure rather than a suite
// that passes while validating geometry the shipped SDK does not enforce.
void testUnhittableControlReportsNoPoint() {
    FakeHost host;
    PulpEmbedDesc d = makeDesc(host);
    PulpEmbedView* v = create(host, d);
    if (!v) return;
    if (!g_frame) {
        check(false, "the design frame was captured");
        pulp_embed_destroy(v);
        return;
    }

    const int bypass = indexOfKey(v, "bypass");
    double x = 0, y = 0;
    check(pulp_embed_param_hit_point(v, bypass, &x, &y) == PULP_EMBED_OK,
          "the toggle reports a hit point while enabled");

    // Disable it: the hit-tester now skips it, so nothing can land on it.
    g_frame->set_element_enabled(kTogIdx, false);

    // Prove the premise before pinning the refusal — the point that DID hit the
    // toggle must now reach nothing, or "no point exists" would be an assumption
    // rather than a fact this fixture established.
    host.begins.clear();
    pulp_embed_dispatch_mouse_down(v, x, y);
    pulp_embed_dispatch_mouse_up(v, x, y);
    check(host.begins.empty(), "pressing a disabled control grabs nothing");

    double rx = -7, ry = -9;
    const PulpEmbedResult r = pulp_embed_param_hit_point(v, bypass, &rx, &ry);
    check(r == PULP_EMBED_ERR_UNSUPPORTED, "a control nothing can hit reports no point");
    if (r == PULP_EMBED_OK)
        std::printf("     the SDK's element_hit_point answered (%.3f, %.3f) for a control"
                    " nothing can hit: it is not proving the anchor against the"
                    " hit-tester\n", rx, ry);
    check(approx(rx, -7) && approx(ry, -9), "the refused call left the out-params untouched");

    // Re-enabling restores the point: the refusal tracks live state rather than
    // latching the control off after one miss.
    g_frame->set_element_enabled(kTogIdx, true);
    check(pulp_embed_param_hit_point(v, bypass, &rx, &ry) == PULP_EMBED_OK,
          "re-enabling the control restores its hit point");

    pulp_embed_destroy(v);
}

// ── bad addressing is refused, never answered with a plausible point ──────────
void testInvalidArgs() {
    FakeHost host;
    PulpEmbedDesc d = makeDesc(host);
    PulpEmbedView* v = create(host, d);
    if (!v) return;

    double x = -7, y = -9;
    check(pulp_embed_param_hit_point(nullptr, 0, &x, &y) == PULP_EMBED_ERR_INVALID_ARG,
          "NULL view is refused");
    check(pulp_embed_param_hit_point(v, 0, nullptr, &y) == PULP_EMBED_ERR_INVALID_ARG,
          "NULL out_x is refused");
    check(pulp_embed_param_hit_point(v, 0, &x, nullptr) == PULP_EMBED_ERR_INVALID_ARG,
          "NULL out_y is refused");
    check(pulp_embed_param_hit_point(v, -1, &x, &y) == PULP_EMBED_ERR_INVALID_ARG,
          "a negative index is refused");
    check(pulp_embed_param_hit_point(v, 999, &x, &y) == PULP_EMBED_ERR_INVALID_ARG,
          "an out-of-range index is refused");
    check(approx(x, -7) && approx(y, -9), "a refused call leaves the out-params untouched");

    pulp_embed_destroy(v);
}

}  // namespace

int main() {
    testHitPointLocatesEachControl();
    testDragFromHitPointMovesTheControl();
    testHitPointTracksLayout();
    testUnhittableControlReportsNoPoint();
    testInvalidArgs();
    std::printf("%s\n", g_fail == 0 ? "hit-point test: all pass"
                                    : "hit-point test: FAILURES");
    return g_fail == 0 ? 0 : 1;
}
