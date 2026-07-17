// tests/suites/miracle_test.cpp — a headless, deterministic verification of the miracle-player
// music visualizer (examples/miracle-player) under the real fixed-step loop: bundle bake+mount,
// the streamed-PCM mixer path, the precomputed viz track driving the visuals, seeking, and loop.
// The whole design is A/V lock, so the load-bearing assertion is that the viz record index is a
// pure integer function of the audio samples consumed — identical on both scalar tiers.
//
// A synthetic two-tone is baked (no giant song needed); every counter is deterministic, so
// `make determinism` runs this on BOTH scalar tiers and diffs the output byte-for-byte.
#include "../../examples/miracle-player/src/player.h"
#include "../../examples/miracle-player/src/bake.h"
#include "phx/platform/platform.h"
#include "phx/platform/gfx_soft.h"

#include <cstdio>

extern "C" void phx_null_set_step_ns(uint64_t);
extern "C" void phx_null_set_max_frames(uint64_t);
extern "C" void phx_null_set_button_script(const uint32_t*, uint32_t);

using namespace phx;
using namespace miracle;

namespace {
const char* kBundle = "build/miracle_test.phxp";
int g_fail = 0;
void CK(bool c, const char* m) { if (!c) { ++g_fail; std::printf("    FAIL %s\n", m); } }

constexpr uint32_t kRate = 22050;             // the synthetic tone's rate (tier 2 keeps it)
constexpr uint32_t kAdvance = kRate / 60;     // samples the headless callback consumes per frame

Config make_cfg() {
    Config cfg = Config::from_defaults();
    cfg.sim_hz = 60; cfg.title = "miracle";
    cfg.width = 240; cfg.height = 160;
    cfg.total_ram = 32u << 20; cfg.frame_scratch = 256u << 10; cfg.max_entities = 64;
    return cfg;
}

template <uint32_t FRAMES>
void run_app(MiracleGame& game, void (*fill_script)(uint32_t*)) {
    static uint32_t script[FRAMES];
    for (auto& s : script) s = 0;
    script[1] = button_bit(Button::Start);        // dismiss the intro on frame 1
    fill_script(script);
    phx_null_set_step_ns(1000000000ull / 60);
    phx_null_set_max_frames(FRAMES);
    phx_null_set_button_script(script, FRAMES);
    App app(make_cfg());
    game.bundle = kBundle;
    game.mixer_rate = kRate;
    game.device_audio = false;
    app.run(&game);
}

// Run 1: plain playthrough. A/V lock is exact (no seeks), and the track plays to the outro.
struct PlayGame final : MiracleGame {
    void on_stop(App&) override {
        const uint64_t pos = samples_played.load();
        std::printf("    run1 phase=%d play_frames=%u samples=%llu total=%llu peak=%d viz_active=%u idx=%u\n",
                    int(current_phase()), play_frames, (unsigned long long)pos,
                    (unsigned long long)total_samples, audio_peak, viz_active_frames, viz_index());
        CK(uint64_t(play_frames) * kAdvance == pos, "run1: samples == play_frames * rate/60 (locked, no seek)");
        CK(viz && viz_index_for_sample(viz, pos) == viz_index(), "run1: viz index derives purely from samples");
        CK(current_phase() == Phase::Outro, "run1: reached the outro after the track ended");
        CK(pos >= total_samples, "run1: consumed the whole song");
        CK(total_samples == uint64_t(kRate) * 3, "run1: song is the 3 s synthetic tone");
        CK(audio_peak > 1000, "run1: mixer produced non-silent output");
        CK(viz_active_frames > 30, "run1: spectrum bars reacted across most playing frames");
    }
};

// Run 2: seek forward. RIGHT (+5 s) on a 3 s track jumps near the end, so the outro arrives in
// far fewer frames than a full playthrough — proving the stream cursor AND the position both
// jumped, and the viz index still derives from the (now advanced) sample cursor.
struct SeekGame final : MiracleGame {
    void on_stop(App&) override {
        const uint64_t pos = samples_played.load();
        std::printf("    run2 phase=%d play_frames=%u samples=%llu idx=%u\n",
                    int(current_phase()), play_frames, (unsigned long long)pos, viz_index());
        CK(current_phase() == Phase::Outro, "run2: forward seek reached the outro");
        CK(play_frames < 90, "run2: reached the end far sooner than a full playthrough (seek jumped)");
        CK(pos >= total_samples, "run2: position jumped forward to the end");
        CK(viz && viz_index_for_sample(viz, position_samples()) == viz_index(), "run2: viz still locked to samples");
    }
};

// Run 3: loop. With loop enabled the track never stops — it re-seeks to 0 at the end — so after
// well past one track length the app is still Playing and still producing sound.
struct LoopGame final : MiracleGame {
    void on_stop(App&) override {
        std::printf("    run3 phase=%d play_frames=%u samples=%llu peak=%d\n",
                    int(current_phase()), play_frames, (unsigned long long)samples_played.load(), audio_peak);
        CK(current_phase() == Phase::Playing, "run3: loop kept playing (never reached the outro)");
        CK(play_frames > 300, "run3: kept advancing across more than one track length");
        CK(samples_played.load() < total_samples, "run3: sample cursor wrapped (loop re-seek to 0)");
        CK(audio_peak > 1000, "run3: still non-silent after looping");
    }
};

int run() {
    if (!bake_miracle_assets(kBundle, 2, nullptr)) { std::printf("MIRACLE FAIL (bake)\n"); return 1; }

    { PlayGame g; run_app<260>(g, [](uint32_t*) {}); }
    { SeekGame g; run_app<200>(g, [](uint32_t* s) { s[30] = button_bit(Button::Right); }); }
    { LoopGame g; g.loop = true; run_app<400>(g, [](uint32_t*) {}); }
    return 0;
}
} // namespace

int main() {
    if (run() != 0) return 1;
    if (g_fail) { std::printf("MIRACLE FAIL (%d checks)\n", g_fail); return 1; }
    std::printf("MIRACLE PASS\n");
    return 0;
}
