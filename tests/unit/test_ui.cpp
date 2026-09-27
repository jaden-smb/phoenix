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

PHX_TEST(ui_text_width_fixed_and_proportional) {
    BitmapFont fixed;
    fixed.advance = 6;
    CHECK_EQ(UI::text_width(fixed, "HELLO"), 30);
    CHECK_EQ(UI::text_width(fixed, "AB\nABCD"), 24);           // the widest line
    CHECK_EQ(UI::text_width(fixed, ""), 0);
    CHECK_EQ(UI::text_width(fixed, nullptr), 0);

    // a glyph table from ' ' (32): 'A' (33 slots in) 3 px, 'B' 5 px, space 2 px
    static FontGlyph table[40] = {};
    table[0].advance = 2;
    table['A' - 32].advance = 3; table['A' - 32].w = 2; table['A' - 32].h = 8;
    table['B' - 32].advance = 5; table['B' - 32].w = 4; table['B' - 32].h = 8;
    BitmapFont prop;
    prop.advance = 5; prop.glyphs = table; prop.glyph_count = 40;
    CHECK_EQ(UI::glyph_advance(prop, 'A'), 3);
    CHECK_EQ(UI::glyph_advance(prop, ' '), 2);
    CHECK_EQ(UI::glyph_advance(prop, 'z'), 5);                  // past the table: the header advance
    CHECK_EQ(UI::text_width(prop, "AB A"), 3 + 5 + 2 + 3);
}

PHX_TEST(ui_focus_ring_edge_cases) {
    CHECK_EQ(UI::ring_step(0, 0, +1), 0);   // empty ring -> 0, no div by zero
    CHECK_EQ(UI::ring_step(0, 1, +1), 0);   // single item -> stays
    CHECK_EQ(UI::ring_step(0, 1, -1), 0);
    CHECK_EQ(UI::ring_step(0, 3,  0), 0);   // no movement
    CHECK_EQ(UI::ring_step(5, 3, +1), 0);   // out-of-range focus normalizes (6 % 3)
}
