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

// Remappable controls (docs/10 §1): for each LOGICAL Button the game reads, the PHYSICAL
// button (bit position in phx_input_raw::buttons, i.e. the platform's canonical layout)
// that feeds it. Plain POD — identity by default (zero setup), save-file friendly (persist
// it through the platform save seam to remember a player's layout), and applied centrally
// in InputState::update() so every backend and both scalar tiers get remapping for free.
// Stick→dpad synthesis works on the RAW axis units (int16), so it is integer-deterministic.
struct InputMap {
    uint8_t physical[uint32_t(Button::Count)] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
    uint8_t stick_to_dpad  = 1;       // left stick also drives Up/Down/Left/Right (pads on PC/PSP)
    uint8_t pad_[1]        = { 0 };
    int16_t stick_deadzone = 16384;   // raw axis units the stick must exceed (~50%)

    // Feed logical `l` from physical `p` (e.g. remap(Button::A, Button::B) to swap confirm).
    void remap(Button l, Button p) { physical[uint32_t(l)] = uint8_t(p); }
    void reset() { *this = InputMap{}; }
};

// Frame-stable input. update() is called once per frame, before the fixed-step sim,
// so every sub-step sees a consistent snapshot (determinism).
struct InputState {
    uint32_t held     = 0;     // bitmask over LOGICAL Button (after `map`)
    uint32_t pressed  = 0;     // edge: down this frame, up last frame
    uint32_t released = 0;     // edge: up this frame, down last frame
    uint32_t raw_held    = 0;  // bitmask over PHYSICAL buttons (before `map`)
    uint32_t raw_pressed = 0;  // physical edge — what a rebind UI's "press any button"
                               // capture reads, since logical edges are already remapped
    vec2     lstick   {};
    vec2     rstick   {};
    vec2     pointer  {};
    bool     pointer_down = false;
    InputMap map;              // the active remap (identity by default)

    void update(const phx_input_raw& raw) {
        raw_pressed = raw.buttons & ~raw_held;
        raw_held    = raw.buttons;
        uint32_t now = 0;
        for (uint32_t b = 0; b < uint32_t(Button::Count); ++b)
            if (raw.buttons & (1u << (map.physical[b] & 31u))) now |= 1u << b;   // &31: no UB
                                                               // on out-of-range saved maps
        if (map.stick_to_dpad) {   // integer thresholds on the raw axis: tier-exact
            const int16_t dz = map.stick_deadzone;
            if (raw.axis[0] <= int16_t(-dz)) now |= button_bit(Button::Left);
            if (raw.axis[0] >= dz)           now |= button_bit(Button::Right);
            if (raw.axis[1] <= int16_t(-dz)) now |= button_bit(Button::Up);
            if (raw.axis[1] >= dz)           now |= button_bit(Button::Down);
        }
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
