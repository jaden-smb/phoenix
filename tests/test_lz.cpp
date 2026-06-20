// tests/test_lz.cpp — the bundle compression codec. Verifies the host encoder
// (tools/phxpack/lz_encode.h) and the runtime decoder (phx/resource/lz.h) round-trip
// exactly across input shapes — runs (RLE via overlap), repeated structure, random
// (incompressible), empty — and that the decoder rejects hostile/corrupt input without
// reading out of bounds. The decoder is the only half that ships to a console.
#include "phx_test.h"
#include "phx/resource/lz.h"
#include "lz_encode.h"

#include <vector>
#include <cstdint>
#include <cstring>

using namespace phx;

namespace {
// Encode then decode; check the result is byte-identical to the input. Returns the
// compressed size so tests can also assert compressibility where expected.
uint32_t roundtrip(const std::vector<uint8_t>& in) {
    std::vector<uint8_t> packed;
    phxtool::lz_encode(in.data(), uint32_t(in.size()), packed);
    std::vector<uint8_t> out(in.size());
    uint32_t got = lz_decode(packed.data(), uint32_t(packed.size()),
                             out.data(), uint32_t(out.size()));
    CHECK_EQ(got, uint32_t(in.size()));
    bool same = true;
    for (size_t i = 0; i < in.size(); ++i) same &= (out[i] == in[i]);
    CHECK(same);
    return uint32_t(packed.size());
}
} // namespace

PHX_TEST(lz_roundtrip_runs) {
    std::vector<uint8_t> in(1000, 0xAB);                 // one long run
    uint32_t packed = roundtrip(in);
    CHECK(packed < in.size() / 4);                       // runs compress hard (overlap match)
}

PHX_TEST(lz_roundtrip_repeated_structure) {
    std::vector<uint8_t> in;
    for (int i = 0; i < 256; ++i) { in.push_back(1); in.push_back(2); in.push_back(3); in.push_back(4); }
    uint32_t packed = roundtrip(in);
    CHECK(packed < in.size());                           // periodic data compresses
}

PHX_TEST(lz_roundtrip_incompressible) {
    std::vector<uint8_t> in;
    uint32_t s = 0x12345678u;                            // xorshift -> high-entropy bytes
    for (int i = 0; i < 512; ++i) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; in.push_back(uint8_t(s)); }
    roundtrip(in);                                       // must still round-trip exactly
}

PHX_TEST(lz_roundtrip_text) {
    const char* t = "the quick brown fox the quick brown fox jumps over the lazy dog the the the";
    std::vector<uint8_t> in(t, t + std::strlen(t));
    uint32_t packed = roundtrip(in);
    CHECK(packed < in.size());                           // repeated words compress
}

PHX_TEST(lz_roundtrip_empty) {
    std::vector<uint8_t> in;
    std::vector<uint8_t> packed;
    phxtool::lz_encode(in.data(), 0, packed);
    CHECK_EQ(packed.size(), size_t(0));                  // nothing in, nothing out
    CHECK_EQ(lz_decode(packed.data(), 0, nullptr, 0), 0u);
}

PHX_TEST(lz_roundtrip_single_byte) {
    std::vector<uint8_t> in{ 0x7F };
    roundtrip(in);
}

PHX_TEST(lz_decode_rejects_input_underrun) {
    // A control byte promising 8 literals but no payload bytes -> 0, no OOB read.
    const uint8_t bad[] = { 0x00 };
    uint8_t out[8];
    CHECK_EQ(lz_decode(bad, sizeof(bad), out, sizeof(out)), 0u);
}

PHX_TEST(lz_decode_rejects_bad_backref) {
    // control=0x80 -> first item is a match; displacement 1 but output is still empty.
    const uint8_t bad[] = { 0x80, 0x00, 0x00 };          // len=3, disp=1, but di==0 -> disp>di
    uint8_t out[16];
    CHECK_EQ(lz_decode(bad, sizeof(bad), out, sizeof(out)), 0u);
}

PHX_TEST(lz_decode_respects_capacity) {
    // All-distinct bytes -> the stream is pure literals; decoding into a buffer smaller than
    // the input fills exactly the cap and stops, never overrunning.
    std::vector<uint8_t> in; for (int i = 0; i < 100; ++i) in.push_back(uint8_t(i));
    std::vector<uint8_t> packed;
    phxtool::lz_encode(in.data(), uint32_t(in.size()), packed);
    uint8_t out[10];
    uint32_t got = lz_decode(packed.data(), uint32_t(packed.size()), out, sizeof(out));
    CHECK_EQ(got, 10u);
    bool same = true; for (int i = 0; i < 10; ++i) same &= (out[i] == uint8_t(i));
    CHECK(same);
}

PHX_TEST(lz_decode_rejects_overrunning_match) {
    // A match whose length would write past the output buffer is corruption -> rejected
    // (the real decompressor always knows the exact usize, so this only fires on bad data).
    std::vector<uint8_t> in(100, 0x5A);                  // compresses to literal + matches
    std::vector<uint8_t> packed;
    phxtool::lz_encode(in.data(), uint32_t(in.size()), packed);
    uint8_t out[5];                                      // too small for the first match
    CHECK_EQ(lz_decode(packed.data(), uint32_t(packed.size()), out, sizeof(out)), 0u);
}
