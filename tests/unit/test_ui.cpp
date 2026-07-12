// tests/test_ui.cpp — pure UI logic: the focus-ring step wraps correctly and is bounded.
// (Drawing primitives are exercised end-to-end by the headless `make ui` integration test.)
#include "phx_test.h"
#include "phx/ui/ui.h"

using namespace phx;

PHX_TEST(ui_focus_ring_wraps) {
    CHECK_EQ(UI::ring_step(0, 3, +1), 1);
    CHECK_EQ(UI::ring_step(1, 3, +1), 2);
    CHECK_EQ(UI::ring_step(2, 3, +1), 0);   // wrap forward
    CHECK_EQ(UI::ring_step(0, 3, -1), 2);   // wrap backward
    CHECK_EQ(UI::ring_step(2, 3, -1), 1);
}

PHX_TEST(ui_focus_ring_edge_cases) {
    CHECK_EQ(UI::ring_step(0, 0, +1), 0);   // empty ring -> 0, no div by zero
    CHECK_EQ(UI::ring_step(0, 1, +1), 0);   // single item -> stays
    CHECK_EQ(UI::ring_step(0, 1, -1), 0);
    CHECK_EQ(UI::ring_step(0, 3,  0), 0);   // no movement
    CHECK_EQ(UI::ring_step(5, 3, +1), 0);   // out-of-range focus normalizes (6 % 3)
}
