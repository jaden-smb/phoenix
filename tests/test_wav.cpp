// tests/test_wav.cpp — the asset pipeline's WAV (RIFF/PCM) decoder. Builds WAV byte streams in
// memory (WAV is trivial to author) and checks: 16-bit mono passthrough, 16-bit stereo downmix,
// 8-bit unsigned -> signed-16 conversion, and rejection of non-RIFF data. Host-only tool header.
#include "phx_test.h"
#include "wav.h"

#include <vector>
#include <cstdint>

namespace {
void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); }
void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i))); }
void tag(std::vector<uint8_t>& v, const char* t) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(t[i])); }

std::vector<uint8_t> make_wav(uint16_t channels, uint16_t bits, uint32_t rate, const std::vector<uint8_t>& pcm) {
    const uint32_t block = channels * (bits / 8);
    std::vector<uint8_t> w;
    tag(w, "RIFF"); put32(w, uint32_t(36 + pcm.size())); tag(w, "WAVE");
    tag(w, "fmt "); put32(w, 16); put16(w, 1); put16(w, channels);
    put32(w, rate); put32(w, rate * block); put16(w, uint16_t(block)); put16(w, bits);
    tag(w, "data"); put32(w, uint32_t(pcm.size()));
    w.insert(w.end(), pcm.begin(), pcm.end());
    return w;
}
void s16(std::vector<uint8_t>& v, int16_t x) { put16(v, uint16_t(x)); }
} // namespace

using namespace phxtool;

PHX_TEST(wav_16bit_mono_passthrough) {
    std::vector<uint8_t> pcm;
    s16(pcm, 1000); s16(pcm, -2000); s16(pcm, 3000);
    auto wav = make_wav(1, 16, 22050, pcm);
    std::vector<int16_t> mono; uint32_t rate = 0;
    CHECK(wav_decode(wav.data(), wav.size(), mono, rate));
    CHECK_EQ(rate, 22050u);
    CHECK_EQ(mono.size(), 3u);
    CHECK(mono.size() == 3 && mono[0] == 1000 && mono[1] == -2000 && mono[2] == 3000);
}

PHX_TEST(wav_16bit_stereo_downmix) {
    std::vector<uint8_t> pcm;
    s16(pcm, 100); s16(pcm, 200);     // frame 0: L=100 R=200 -> 150
    s16(pcm, 300); s16(pcm, -100);    // frame 1: L=300 R=-100 -> 100
    auto wav = make_wav(2, 16, 44100, pcm);
    std::vector<int16_t> mono; uint32_t rate = 0;
    CHECK(wav_decode(wav.data(), wav.size(), mono, rate));
    CHECK_EQ(mono.size(), 2u);
    CHECK(mono.size() == 2 && mono[0] == 150 && mono[1] == 100);
}

PHX_TEST(wav_8bit_mono_converts_to_signed16) {
    std::vector<uint8_t> pcm = { 128, 255, 0 };   // mid, max, min (unsigned)
    auto wav = make_wav(1, 8, 8000, pcm);
    std::vector<int16_t> mono; uint32_t rate = 0;
    CHECK(wav_decode(wav.data(), wav.size(), mono, rate));
    CHECK_EQ(rate, 8000u);
    CHECK_EQ(mono.size(), 3u);
    CHECK(mono.size() == 3 && mono[0] == 0 && mono[1] == int16_t(127 << 8) && mono[2] == int16_t(-32768));
}

PHX_TEST(wav_rejects_non_riff) {
    const uint8_t junk[16] = { 'n', 'o', 'p', 'e' };
    std::vector<int16_t> mono; uint32_t rate = 0;
    CHECK(!wav_decode(junk, sizeof(junk), mono, rate));
}
