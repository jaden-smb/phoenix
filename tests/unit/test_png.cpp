// tests/test_png.cpp — the asset pipeline's DEFLATE/inflate + PNG decoders. Exercises a real
// zlib stream (dynamic Huffman + LZ77 back-references), a hand-built stored block (no Huffman),
// a malformed stream (rejected, no crash), and a real 8-bit RGB PNG decoded to RGBA8. Tool
// headers are host-only/STL, pulled into the unit binary the same way test_lz uses lz_encode.h.
#include "phx_test.h"
#include "inflate.h"
#include "png.h"
#include "fixtures/png_fixtures.h"

#include <vector>
#include <string>

PHX_TEST(inflate_zlib_dynamic_huffman) {
    std::string expect;
    for (int i = 0; i < 8; ++i) expect += "the quick brown fox ";
    expect += "ABCABCABCABCABCABC";

    std::vector<uint8_t> out;
    CHECK(phxtool::inflate_zlib(kZlibStream, sizeof(kZlibStream), out));
    CHECK_EQ(out.size(), expect.size());
    bool same = out.size() == expect.size();
    for (size_t i = 0; same && i < out.size(); ++i) same &= (out[i] == uint8_t(expect[i]));
    CHECK(same);
}

PHX_TEST(inflate_raw_stored_block) {
    // One final stored block: header byte 0x01 (BFINAL=1, BTYPE=00), then LEN=2, NLEN=~2, "Hi".
    const uint8_t stored[] = { 0x01, 0x02, 0x00, 0xFD, 0xFF, 'H', 'i' };
    std::vector<uint8_t> out;
    CHECK(phxtool::detail::inflate_raw(stored, sizeof(stored), out));
    CHECK_EQ(out.size(), 2u);
    CHECK(out.size() == 2 && out[0] == 'H' && out[1] == 'i');
}

PHX_TEST(inflate_rejects_truncated) {
    // A valid zlib header but a truncated/garbage body -> false, no out-of-bounds read.
    const uint8_t bad[] = { 0x78, 0xda, 0xff };
    std::vector<uint8_t> out;
    CHECK(!phxtool::inflate_zlib(bad, sizeof(bad), out));
}

PHX_TEST(png_decode_rgb_8bit) {
    std::vector<uint32_t> px; uint16_t w = 0, h = 0;
    CHECK(phxtool::png_decode(kPng3x2, sizeof(kPng3x2), px, w, h));
    CHECK_EQ(w, 3u);
    CHECK_EQ(h, 2u);
    CHECK_EQ(px.size(), 6u);
    bool ok = px.size() == 6;
    for (int i = 0; ok && i < 6; ++i) ok &= (px[i] == kPng3x2RGBA[i]);
    CHECK(ok);
}

PHX_TEST(png_decode_rejects_non_png) {
    const uint8_t notpng[] = { 'n', 'o', 'p', 'e', 0, 0, 0, 0, 0, 0 };
    std::vector<uint32_t> px; uint16_t w = 0, h = 0;
    CHECK(!phxtool::png_decode(notpng, sizeof(notpng), px, w, h));
}
