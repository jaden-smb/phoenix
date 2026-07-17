// tests/unit/test_viz.cpp — the precomputed visualization track (examples/miracle-player).
// Covers the two halves that must never disagree: the runtime side (viz.h — the zero-copy
// record decode + integer position->index mapping the ROM relies on for A/V lock) and the
// host bake side (tools/phxviz/analyze.h — that it emits a valid, self-consistent, and
// deterministic blob whose features actually respond to the signal).
#include "phx_test.h"
#include "viz.h"        // examples/miracle-player/src — runtime POD format
#include "analyze.h"    // tools/phxviz — host analysis (STL/float ok in a host unit test)

#include <vector>
#include <cstring>
#include <cmath>

using namespace miracle;

namespace {

// Build a minimal in-memory blob with N frames whose fields carry known values.
std::vector<uint8_t> make_blob(uint32_t frames, uint32_t hop, uint32_t rate) {
    std::vector<uint8_t> b(sizeof(VizHeader) + size_t(frames) * sizeof(VizFrame), 0);
    VizHeader h{};
    h.magic = kVizMagic; h.version = kVizVersion; h.band_count = kVizBands;
    h.frame_count = frames; h.device_rate = rate; h.hop_samples = hop;
    std::memcpy(b.data(), &h, sizeof(h));
    VizFrame* rec = reinterpret_cast<VizFrame*>(b.data() + sizeof(VizHeader));
    for (uint32_t f = 0; f < frames; ++f) { rec[f].rms = uint8_t(f & 0xFF); rec[f].beat = uint8_t(f & 1); }
    return b;
}

} // namespace

PHX_TEST(viz_validate_accepts_and_decodes) {
    auto b = make_blob(10, 303, 18157);
    const VizHeader* h = viz_validate(b.data(), uint32_t(b.size()));
    CHECK(h != nullptr);
    CHECK_EQ(h->frame_count, 10u);
    CHECK_EQ(h->hop_samples, 303u);
    CHECK_EQ(h->band_count, kVizBands);
    // Records decode by pure pointer arithmetic; the payload we stamped survives round-trip.
    CHECK_EQ(viz_frames(h)[7].rms, 7u);
    CHECK_EQ(viz_frames(h)[7].beat, 1u);
    CHECK_EQ(viz_frames(h)[8].beat, 0u);
}

PHX_TEST(viz_validate_rejects_bad) {
    auto b = make_blob(4, 303, 18157);
    CHECK(viz_validate(nullptr, 0) == nullptr);
    CHECK(viz_validate(b.data(), 3) == nullptr);                 // shorter than the header
    // Corrupt magic -> reject.
    auto bad = b; bad[0] ^= 0xFF;
    CHECK(viz_validate(bad.data(), uint32_t(bad.size())) == nullptr);
    // Header claims more frames than the buffer holds -> reject (bounds check).
    auto trunc = b; VizHeader hh{}; std::memcpy(&hh, trunc.data(), sizeof(hh));
    hh.frame_count = 9999; std::memcpy(trunc.data(), &hh, sizeof(hh));
    CHECK(viz_validate(trunc.data(), uint32_t(trunc.size())) == nullptr);
}

PHX_TEST(viz_index_mapping_is_integer_and_clamped) {
    auto b = make_blob(100, 300, 18000);
    const VizHeader* h = viz_validate(b.data(), uint32_t(b.size()));
    CHECK(h != nullptr);
    CHECK_EQ(viz_index_for_sample(h, 0), 0u);
    CHECK_EQ(viz_index_for_sample(h, 299), 0u);                 // still frame 0 (floor)
    CHECK_EQ(viz_index_for_sample(h, 300), 1u);
    CHECK_EQ(viz_index_for_sample(h, 301), 1u);
    CHECK_EQ(viz_index_for_sample(h, 3000), 10u);
    // Past the end clamps to the final record (never reads out of bounds).
    CHECK_EQ(viz_index_for_sample(h, 1000000), 99u);
    // frame_at agrees with frames()+index.
    CHECK(viz_frame_at(h, 3000) == viz_frames(h) + 10);
}

// A pure sine at a known frequency should light the band that contains it and leave a silent
// signal near zero — proving the analysis actually tracks the audio, not noise.
PHX_TEST(analyze_tracks_signal) {
    const uint32_t rate = 18157;
    const uint32_t n    = rate * 2;                            // 2 seconds
    std::vector<float> tone(n), silence(n, 0.0f);
    const double freq = 1000.0;
    for (uint32_t i = 0; i < n; ++i) tone[i] = 0.8f * float(std::sin(2.0 * M_PI * freq * i / rate));

    auto tb = phxviz::analyze(tone, rate);
    auto sb = phxviz::analyze(silence, rate);
    const VizHeader* th = viz_validate(tb.data(), uint32_t(tb.size()));
    const VizHeader* sh = viz_validate(sb.data(), uint32_t(sb.size()));
    CHECK(th != nullptr);
    CHECK(sh != nullptr);

    // hop = round(rate/60); frame_count ~ n/hop (2s * 60fps ~ 120 frames).
    CHECK_EQ(th->hop_samples, (rate + 30) / 60);
    CHECK(th->frame_count >= 118 && th->frame_count <= 122);

    // Steady-state (skip FFT warm-up frame 0): tone has real energy, silence is flat zero.
    const VizFrame& tf = viz_frames(th)[10];
    const VizFrame& sf = viz_frames(sh)[10];
    int tone_energy = 0; for (uint32_t b = 0; b < kVizBands; ++b) tone_energy += tf.bands[b];
    int sil_energy  = 0; for (uint32_t b = 0; b < kVizBands; ++b) sil_energy  += sf.bands[b];
    CHECK(tone_energy > 100);
    CHECK_EQ(sil_energy, 0);
    CHECK(tf.rms > 100);
    CHECK_EQ(sf.rms, 0u);
}

// The bake is a pure function of its input: identical bytes every run (determinism contract).
PHX_TEST(analyze_is_deterministic) {
    const uint32_t rate = 18157;
    std::vector<float> sig(rate);
    for (uint32_t i = 0; i < rate; ++i)
        sig[i] = 0.5f * float(std::sin(2.0 * M_PI * 220.0 * i / rate));
    auto a = phxviz::analyze(sig, rate);
    auto b = phxviz::analyze(sig, rate);
    CHECK_EQ(a.size(), b.size());
    CHECK(a == b);
}
