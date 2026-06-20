// tests/platformer_test.cpp — the M1 CAPSTONE: a headless playthrough of the example game,
// assembling EVERY engine system (resource bundle, ECS, input, physics, animation, scenes,
// UI, render) under the real fixed-step loop. A scripted controller presses Start (title →
// level), walks right collecting coins, jumps, pauses, and resumes; assertions read game
// state + the framebuffer in on_stop (before the App frees its memory).
#include "components.h"            // examples/platformer/src
#include "bake.h"                  // host bundle baker
#include "phx/platform/platform.h"
#include "phx/platform/gfx_soft.h"

#include <cstdio>

extern "C" void phx_null_set_step_ns(uint64_t);
extern "C" void phx_null_set_max_frames(uint64_t);
extern "C" void phx_null_set_button_script(const uint32_t*, uint32_t);

using namespace phx;
using namespace game;

namespace {
constexpr int FRAMES = 56;
const Rgba kClear = rgba(30, 30, 46);

// A thin verifier over the real game: same logic, plus assertions at teardown.
struct VerifyGame final : PlatformerGame {
    bool ok = false; int checks = 0, fail = 0;

    void on_stop(App& app) override {
        auto CK = [&](bool c, const char* m) { ++checks; if (!c) { ++fail; std::printf("    FAIL %s\n", m); } };

        CK(score() > 0,            "collected at least one coin (score > 0)");
        CK(player_x() > 16,        "player moved right from spawn x=16 (from the Tiled spawn)");
        CK(player_on_ground(),     "player is resting on the tile floor at the end");
        CK(depth() == 1,           "scene stack back to the level after pause+resume");
        CK(sfx_count() > 0,        "SFX triggered on jump/coin (baked WAV -> Sound -> mixer)");
        CK(audio_peak_level() > 0, "mixer produced non-silent output from the baked Sound assets");

        // HUD text ("SCORE ...") drew near the top-left -> a non-background pixel there.
        phx_soft_fb fb = phx_gfx_soft_lock(app.platform()->gfx());
        bool hud = false;
        for (int y = 2; y < 10 && !hud; ++y)
            for (int x = 2; x < 48; ++x)
                if (fb.pixels[y * fb.w + x] != kClear) { hud = true; break; }
        CK(hud, "HUD text rendered to the framebuffer");

        ok = (fail == 0);
        std::printf("    score=%d  player_x=%d  on_ground=%d  scene_depth=%u  sfx=%d  audio_peak=%d\n",
                    score(), player_x(), player_on_ground(), depth(), sfx_count(), audio_peak_level());
    }
};
} // namespace

int main() {
    const char* bundle = "build/platformer.phxp";
    if (!bake_platformer_assets(bundle)) { std::printf("PLATFORMER FAIL (bake)\n"); return 1; }

    // Scripted controller (per-frame button masks).
    static uint32_t script[FRAMES];
    for (auto& s : script) s = 0;
    script[1]  = button_bit(Button::Start);                 // title -> level (replace)
    for (int f = 3; f < 40; ++f) script[f] = button_bit(Button::Right);   // walk right
    script[6] |= button_bit(Button::A);                     // jump mid-stride
    script[42] = button_bit(Button::Start);                 // pause
    script[48] = button_bit(Button::Start);                 // resume

    phx_null_set_step_ns(1000000000ull / 60);
    phx_null_set_max_frames(FRAMES);
    phx_null_set_button_script(script, FRAMES);

    Config cfg = Config::from_defaults();
    cfg.sim_hz = 60; cfg.title = "platformer";
    cfg.width = 128; cfg.height = 96;
    cfg.total_ram = 32u << 20; cfg.frame_scratch = 256u << 10; cfg.max_entities = 1024;

    App app(cfg);
    VerifyGame game;
    game.bundle = bundle;
    int rc = app.run(&game);

    std::printf("\nplatformer_test: rc=%d  %d checks, %d failures\n", rc, game.checks, game.fail);
    std::printf((rc == 0 && game.ok) ? "PLATFORMER PASS\n\n" : "PLATFORMER FAIL\n\n");
    return (rc == 0 && game.ok) ? 0 : 1;
}
