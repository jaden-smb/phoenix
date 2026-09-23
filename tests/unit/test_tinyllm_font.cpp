// tests/unit/test_tinyllm_font.cpp — the tinyllm text console's glyph mapping.
//
// This file exists because of a bug that shipped past every other test: `font_cell()` returned
// the atlas cell index directly, but the engine's tilemap convention is that cell value 0 means
// EMPTY and value v selects tileset tile v-1 (soft_renderer.cpp and gba_ppu.cpp agree on this).
// The console therefore rendered every character as the one before it in the atlas — invisible
// to a test that only counts non-empty cells, and invisible on the host because nobody reads the
// framebuffer back as text. So: pin the mapping against the actual baked atlas pixels.
#include "../phx_test.h"
#include "../../examples/tinyllm/src/text_font.h"
#include "../../examples/tinyllm/src/bake.h"

#include <cstring>
#include <vector>

using namespace tinyllm;

namespace {

// Does the 8x8 cell at atlas tile index `tile` hold the glyph bitmap for `ch`?
bool atlas_tile_matches(const uint32_t* px, int tile, char ch, bool amber) {
    const int g = int(static_cast<unsigned char>(ch)) - 32;
    if (g < 0 || g >= kFontGlyphs) return false;
    const int cx = (tile % kFontCols) * 8;
    const int cy = (tile / kFontCols) * 8;
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            const bool lit = px[(cy + y) * kFontAtlasW + (cx + x)] != 0u;
            const bool want = (y < 7) && (x >= 1) && (x <= 5) &&
                              ((kFont5x7[g][y] & (1u << (4 - (x - 1)))) != 0);
            if (lit != want) return false;
        }
    }
    // ...and in the right colour bank.
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            const uint32_t p = px[(cy + y) * kFontAtlasW + (cx + x)];
            if (!p) continue;
            const bool is_amber = p == pack_rgba(255, 190, 90);
            if (is_amber != amber) return false;
        }
    return true;
}

} // namespace

PHX_TEST(tinyllm_font_cell_matches_the_baked_atlas) {
    std::vector<uint32_t> px(size_t(kFontAtlasW) * kFontAtlasH);
    build_font_atlas(px.data());

    // Every printable character, in both colour banks, must land on the atlas tile that actually
    // holds its glyph — after the cell-value-to-tile-index shift the renderer applies.
    for (int a = 32; a < 127; ++a) {
        const char ch = char(a);
        for (int bank = 0; bank < 2; ++bank) {
            const uint16_t cell = font_cell(ch, bank != 0);
            CHECK(cell != 0);                              // 0 would render as EMPTY, not a glyph
            const int tile = int(cell) - 1;                // the renderer's mapping
            if (!atlas_tile_matches(px.data(), tile, ch, bank != 0)) {
                std::printf("      FAIL '%c' (%d) bank %d -> cell %u -> tile %d\n",
                            ch, a, bank, unsigned(cell), tile);
                CHECK(false);
            } else {
                CHECK(true);
            }
        }
    }
}

PHX_TEST(tinyllm_font_banks_are_distinct_and_in_range) {
    // The two banks must not overlap, and every cell must index a real atlas tile.
    const int tiles = (kFontAtlasW / 8) * (kFontAtlasH / 8);
    for (int a = 32; a < 127; ++a) {
        const uint16_t w = font_cell(char(a), false);
        const uint16_t m = font_cell(char(a), true);
        CHECK(w != m);
        CHECK(int(w) - 1 < tiles);
        CHECK(int(m) - 1 < tiles);
        CHECK(int(m) - int(w) == kFontBankB);
    }
    CHECK_EQ(tiles, 2 * kFontGlyphs);
}

PHX_TEST(tinyllm_font_char_round_trips) {
    for (int a = 32; a < 127; ++a) {
        CHECK_EQ(int(font_char(font_cell(char(a), false))), a);
        CHECK_EQ(int(font_char(font_cell(char(a), true))), a);
    }
    CHECK_EQ(int(font_char(0)), int(' '));           // the empty cell reads back as a space
    // Out-of-range bytes fall back to the hollow box rather than indexing off the atlas.
    CHECK_EQ(int(font_cell(char(200))), kFontGlyphs);
    CHECK_EQ(int(font_cell(char(1))),   kFontGlyphs);
}
