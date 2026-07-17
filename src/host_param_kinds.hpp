// host_param_kinds.hpp — the ONE definition of which design controls carry a
// host parameter, and what each reports as its widget kind. PRIVATE to the
// pulp_view_embed library target (not in the install set).
//
// Both binding lanes resolve through this table:
//   - the imported lane, over a DesignIR element's InteractiveElementKind
//     (EmbedProcessor::faithful_element_keys / faithful_element_metas), and
//   - the native lane, over a live view's DesignFrameElement::Kind
//     (build_param_bridge, for a host-supplied compiled view).
// They used to answer the question separately, and drifted: the imported lane
// excluded only text_field and let every other non-value kind fall through a
// catch-all `default:` into a continuous knob. One home, one answer.
#ifndef PULP_VIEW_EMBED_HOST_PARAM_KINDS_HPP
#define PULP_VIEW_EMBED_HOST_PARAM_KINDS_HPP

#include <pulp/view/design_frame_view.hpp>
#include <pulp/view/design_ir.hpp>

namespace pulp::embed::shim {

// What a control kind contributes to the host parameter surface.
// `name` is the reported widget kind; a null `name` means the kind carries no
// host parameter at all and must not be bound.
struct HostParamKind {
    const char* name = nullptr;
    bool discrete = false;
};

// The table. A kind is bindable only if it has a normalized value that persists
// between interactions — that is what a host parameter is.
//
// The kinds that carry none, and why:
//   text_field   a string, not a normalized value; bridged separately through
//                the text-field entry points.
//   value_label  a read-only readout the design paints.
//   swap         a frame-swap link: a click switches page, and nothing is
//                carried between clicks.
//   action       a command button: a click fires a named action, which the host
//                maps to its own state.
//   momentary    a press pulse. It does report 0/1 while held, but it is an
//                event, not a value a host persists or automates.
//   custom       a registered native control. The design declares no value
//                domain for it, so there is nothing for a parameter to scale by.
//
// The runtime agrees with all but momentary: DesignFrameView::element_value()
// answers with its "no normalized value" sentinel for text_field, value_label,
// swap, action, and an unregistered custom. Binding any of them anyway publishes
// a host parameter that can never be read or written — the phantom this table
// exists to prevent — and shifts the index of every real parameter behind it.
constexpr HostParamKind host_param_kind(pulp::view::DesignFrameElement::Kind k) {
    using K = pulp::view::DesignFrameElement::Kind;
    switch (k) {
        case K::knob:       return {"knob", false};
        case K::fader:      return {"fader", false};
        case K::toggle:     return {"toggle", false};
        case K::xy_pad:     return {"xy_pad", false};
        case K::dropdown:   return {"dropdown", true};
        case K::tab_group:  return {"tab_group", true};
        case K::stepper:    return {"stepper", true};
        case K::text_field:
        case K::value_label:
        case K::swap:
        case K::action:
        case K::momentary:
        case K::custom:
            return {};
    }
    return {};
}

// A DesignIR element's kind as the frame kind the materializer gives it, so the
// imported lane asks the table above rather than keeping a second copy of it.
// Exhaustive and deliberately default-less, here and in the table above: a
// control kind added to either enum trips -Wswitch (on by default) and gets
// classified on purpose. A silent catch-all is exactly what bound the non-value
// kinds as knobs. Should one slip through anyway, both switches fall back to a
// NON-binding answer, so the failure mode is a missing parameter — visibly
// absent — rather than a phantom one that looks real and can never be driven.
constexpr pulp::view::DesignFrameElement::Kind to_frame_kind(
    pulp::view::InteractiveElementKind k) {
    using I = pulp::view::InteractiveElementKind;
    using K = pulp::view::DesignFrameElement::Kind;
    switch (k) {
        case I::knob:        return K::knob;
        case I::fader:       return K::fader;
        case I::toggle:      return K::toggle;
        case I::dropdown:    return K::dropdown;
        case I::text_field:  return K::text_field;
        case I::tab_group:   return K::tab_group;
        case I::stepper:     return K::stepper;
        case I::swap:        return K::swap;
        case I::action:      return K::action;
        case I::xy_pad:      return K::xy_pad;
        case I::value_label: return K::value_label;
        case I::custom:      return K::custom;
    }
    return K::custom;
}

constexpr HostParamKind host_param_kind(pulp::view::InteractiveElementKind k) {
    return host_param_kind(to_frame_kind(k));
}

}  // namespace pulp::embed::shim

#endif  // PULP_VIEW_EMBED_HOST_PARAM_KINDS_HPP
