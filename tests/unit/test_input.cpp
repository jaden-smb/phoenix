// tests/test_input.cpp — semantic input: held/pressed/released edges, axis normalization,
// button remapping, and integer stick→dpad synthesis.
#include "phx_test.h"
#include "phx/input/input.h"

using namespace phx;

static phx_input_raw raw_with(uint32_t buttons) {
    phx_input_raw r{};
    r.buttons = buttons;
    r.pointer_x = -1; r.pointer_y = -1;
    return r;
}

PHX_TEST(input_press_edge) {
    InputState in;
    in.update(raw_with(0));                          // nothing
    CHECK(!in.down(Button::A));

    in.update(raw_with(button_bit(Button::A)));      // A goes down
    CHECK(in.down(Button::A));
    CHECK(in.just(Button::A));                        // press edge this frame
    CHECK(!in.up(Button::A));

    in.update(raw_with(button_bit(Button::A)));      // A still held
    CHECK(in.down(Button::A));
    CHECK(!in.just(Button::A));                       // no longer an edge
}

PHX_TEST(input_release_edge) {
    InputState in;
    in.update(raw_with(button_bit(Button::Start)));
    in.update(raw_with(0));                           // Start released
    CHECK(!in.down(Button::Start));
    CHECK(in.up(Button::Start));                      // release edge
    in.update(raw_with(0));
    CHECK(!in.up(Button::Start));
}

PHX_TEST(input_multiple_buttons) {
    InputState in;
    uint32_t both = button_bit(Button::Left) | button_bit(Button::B);
    in.update(raw_with(both));
    CHECK(in.down(Button::Left));
    CHECK(in.down(Button::B));
    CHECK(!in.down(Button::Right));
    CHECK(in.any());
}

PHX_TEST(input_axis_normalization) {
    InputState in;
    phx_input_raw r{};
    r.axis[0] = 32767;     // ~ +1
    r.axis[1] = -32768;    // == -1
    in.update(r);
    CHECK_NEAR(s_to_double(in.lstick.x),  1.0, 0.001);
    CHECK_NEAR(s_to_double(in.lstick.y), -1.0, 0.001);
}

PHX_TEST(input_remap_swaps_buttons) {
    InputState in;
    in.map.remap(Button::A, Button::B);       // logical A fed by physical B
    in.map.remap(Button::B, Button::A);       // and vice versa (swapped confirm/cancel)

    in.update(raw_with(button_bit(Button::A)));   // PHYSICAL A pressed
    CHECK(in.down(Button::B));                    // ...reads as logical B
    CHECK(!in.down(Button::A));
    CHECK(in.just(Button::B));                    // edges are computed on the LOGICAL mask

    in.map.reset();                                // identity restores 1:1
    in.update(raw_with(button_bit(Button::A)));
    CHECK(in.down(Button::A));
    CHECK(!in.down(Button::B));
}

PHX_TEST(input_raw_edges_for_rebind_capture) {
    InputState in;
    in.map.remap(Button::A, Button::R);           // logical A fed by physical R

    in.update(raw_with(button_bit(Button::R)));   // PHYSICAL R pressed
    CHECK(in.just(Button::A));                    // logical view is remapped...
    CHECK(in.raw_pressed == button_bit(Button::R));   // ...raw view is NOT (rebind capture)
    CHECK(in.raw_held    == button_bit(Button::R));

    in.update(raw_with(button_bit(Button::R)));   // held: no raw edge
    CHECK(in.raw_pressed == 0);
    CHECK(in.raw_held    == button_bit(Button::R));

    in.update(raw_with(0));
    CHECK(in.raw_held == 0);

    // an out-of-range physical index in a (corrupt) saved map must not be UB — the shift
    // masks to a valid bit position (200 & 31 == 8, i.e. physical L here, which is up)
    in.map.physical[uint32_t(Button::A)] = 200;
    in.update(raw_with(button_bit(Button::A)));
    CHECK(!in.down(Button::A));
}

PHX_TEST(input_stick_synthesizes_dpad) {
    InputState in;
    phx_input_raw r{};
    r.axis[0] = 30000;                          // stick hard right
    r.axis[1] = -30000;                         // and up
    in.update(r);
    CHECK(in.down(Button::Right));
    CHECK(in.down(Button::Up));
    CHECK(!in.down(Button::Left));
    CHECK(in.just(Button::Right));              // synthesis feeds the same edge detection

    r.axis[0] = 8000; r.axis[1] = -8000;        // inside the default deadzone (16384)
    in.update(r);
    CHECK(!in.down(Button::Right));
    CHECK(in.up(Button::Right));                // release edge as the stick recenters

    in.map.stick_to_dpad = 0;                   // synthesis can be disabled entirely
    r.axis[0] = 30000;
    in.update(r);
    CHECK(!in.down(Button::Right));
    CHECK_NEAR(s_to_double(in.lstick.x), 30000.0 / 32768.0, 0.001);   // raw stick still exposed
}
