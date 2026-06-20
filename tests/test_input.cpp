// tests/test_input.cpp — semantic input: held/pressed/released edges + axis normalization.
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
