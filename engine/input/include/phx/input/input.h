// phx/input/input.h — turns the platform's phx_input_raw snapshot into semantic, frame-
// stable state with edge detection. The canonical Button order is defined ONCE here, so
// `Button::A` means the same thing on every backend (each backend fills phx_input_raw
// using these bit positions). See docs/10 §1.
#ifndef PHX_INPUT_INPUT_H
#define PHX_INPUT_INPUT_H

#include "phx/core/types.h"
#include "phx/core/math.h"
#include "phx/platform/platform.h"     // phx_input_raw

namespace phx {

// Canonical button order — the bit index into phx_input_raw::buttons.
enum class Button : uint8_t {
    Up, Down, Left, Right,
    A, B, X, Y,
    L, R,
    Start, Select,
    Count
};

inline constexpr uint32_t button_bit(Button b) { return 1u << uint32_t(b); }

// Normalize a raw axis (-32768..32767) to scalar [-1, 1]. Tier-agnostic.
inline scalar axis_norm(int16_t a) {
#if PHX_CAPS_HAS_FLOAT_HW
    return scalar(a) * (1.0f / 32768.0f);
#else
    return fixed16::from_raw(int32_t(a) * 2);   // a/32768 in Q16.16 == a*2 raw
#endif
}

// Frame-stable input. update() is called once per frame, before the fixed-step sim,
// so every sub-step sees a consistent snapshot (determinism).
struct InputState {
    uint32_t held     = 0;     // bitmask over Button
    uint32_t pressed  = 0;     // edge: down this frame, up last frame
    uint32_t released = 0;     // edge: up this frame, down last frame
    vec2     lstick   {};
    vec2     rstick   {};
    vec2     pointer  {};
    bool     pointer_down = false;

    void update(const phx_input_raw& raw) {
        uint32_t now = raw.buttons;
        pressed  = now & ~held;
        released = ~now & held;
        held     = now;
        lstick   = { axis_norm(raw.axis[0]), axis_norm(raw.axis[1]) };
        rstick   = { axis_norm(raw.axis[2]), axis_norm(raw.axis[3]) };
        pointer  = { s_from_int(raw.pointer_x), s_from_int(raw.pointer_y) };
        pointer_down = raw.pointer_down != 0;
    }

    bool down(Button b) const { return (held     & button_bit(b)) != 0; }
    bool just(Button b) const { return (pressed  & button_bit(b)) != 0; }  // edge press
    bool up(Button b)   const { return (released & button_bit(b)) != 0; }  // edge release
    bool any() const { return held != 0; }
};

} // namespace phx
#endif // PHX_INPUT_INPUT_H
