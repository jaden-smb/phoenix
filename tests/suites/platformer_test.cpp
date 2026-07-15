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
constexpr int FRAMES = 130;
const Rgba kClear = rgba(30, 30, 46);
const char* kSave = "build/platformer.sav";   // isolated save store for the round-trip test
int g_run1_score = -1, g_run1_deaths = -1;    // captured from run 1 -> asserted restored in run 2

// A thin verifier over the real game: same logic, plus assertions at teardown.
struct VerifyGame final : PlatformerGame {
    bool ok = false; int checks = 0, fail = 0;

    void on_stop(App& app) override {
        auto CK = [&](bool c, const char* m) { ++checks; if (!c) { ++fail; std::printf("    FAIL %s\n", m); } };
        g_run1_score = score(); g_run1_deaths = deaths();

        CK(score() > 0,            "collected at least one coin (score > 0)");
        CK(player_x() > 88,        "player walked past the coins into the enemy/spike zone");
        CK(depth() == 1,           "scene stack back to the level after pause+resume");
        CK(sfx_count() > 0,        "SFX triggered on jump/coin (baked WAV -> Sound -> mixer)");
        CK(audio_peak_level() > 0, "mixer produced non-silent output from the baked Sound assets");

        // Enemy/hazard mechanics (M5): the player took damage, i-frames kept it alive, and the
        // patroller was stomped. (Exact numbers are deterministic — identical on both tiers.)
        CK(health() < 3,           "player took damage from the enemy/spike (health < max)");
        CK(health() >= 1,          "i-frames kept a single brush from draining the whole bar");
        CK(enemies_killed() > 0,   "stomped the patroller (jump-on-top kill)");
        CK(hazard_tile_hits() == 0, "the golden walk-right path never touches the lava pit");

        // HUD text ("SCORE ...") drew near the top-left -> a non-background pixel there.
        phx_soft_fb fb = phx_gfx_soft_lock(app.platform()->gfx());
        bool hud = false;
        for (int y = 2; y < 10 && !hud; ++y)
            for (int x = 2; x < 48; ++x)
                if (fb.pixels[y * fb.w + x] != kClear) { hud = true; break; }
        CK(hud, "HUD text rendered to the framebuffer");

        ok = (fail == 0);
        std::printf("    score=%d  player_x=%d  on_ground=%d  depth=%u  sfx=%d  peak=%d  health=%d  killed=%d  deaths=%d\n",
                    score(), player_x(), player_on_ground(), depth(), sfx_count(), audio_peak_level(),
                    health(), enemies_killed(), deaths());
    }
};

// Run 2: boot a fresh game pointed at the same save store; on_start should restore run 1's score.
struct LoadGame final : PlatformerGame {
    bool ok = false; int checks = 0, fail = 0;
    void on_stop(App&) override {
        auto CK = [&](bool c, const char* m) { ++checks; if (!c) { ++fail; std::printf("    FAIL %s\n", m); } };
        CK(was_loaded(),               "a saved game was restored at boot (magic/version valid)");
        CK(score() == g_run1_score,    "restored score matches the saved run");
        CK(score() > 0,                "restored score is non-trivial");
        CK(deaths() == g_run1_deaths,  "restored death count matches the saved run");
        ok = (fail == 0);
        std::printf("    loaded=%d  score=%d (saved %d)  deaths=%d (saved %d)\n",
                    was_loaded(), score(), g_run1_score, deaths(), g_run1_deaths);
    }
};

// Run 3: walk LEFT into the lava pit at tile (0,10) — a HAZARD-flagged tile, not an entity.
// Proves the authored per-tile collision metadata drives real damage through
// PhysicsWorld::tile_flags_in(): the player falls in (lava is not solid), gets hurt (with
// i-frames), and the tile-hazard hit counter — which only tile flags can increment — moves.
struct LavaGame final : PlatformerGame {
    bool ok = false; int checks = 0, fail = 0;
    void on_stop(App&) override {
        auto CK = [&](bool c, const char* m) { ++checks; if (!c) { ++fail; std::printf("    FAIL %s\n", m); } };
        CK(hazard_tile_hits() >= 1, "a hazard-FLAGGED tile hurt the player (tile_flags_in)");
        CK(health() < 3,            "the lava actually cost health");
        CK(health() >= 1,           "i-frames metered the lava damage");
        CK(player_x() < 12,         "player is in the pit region (fell through non-solid lava)");
        ok = (fail == 0);
        std::printf("    lava_hits=%d  health=%d  player_x=%d\n",
                    hazard_tile_hits(), health(), player_x());
    }
};

// Run 4: the OPTIONS/remap flow, end to end through the real UI focus ring — pause →
// OPTIONS → rebind JUMP via the "press a button" capture (raw_pressed) to physical R →
// BACK → resume → jump with R → pause again (which saves the remapped layout, v2).
struct RemapGame final : PlatformerGame {
    bool ok = false; int checks = 0, fail = 0;
    void on_stop(App& app) override {
        auto CK = [&](bool c, const char* m) { ++checks; if (!c) { ++fail; std::printf("    FAIL %s\n", m); } };
        CK(app.input_map().physical[uint32_t(Button::A)] == uint8_t(Button::R),
           "options UI rebound JUMP (logical A) to physical R");
        CK(!player_on_ground(),    "the rebound button actually jumps (airborne at pause)");
        CK(depth() == 2,           "frozen in the second pause (level + pause)");
        ok = (fail == 0);
        std::printf("    map[A]=%u  on_ground=%d  depth=%u\n",
                    app.input_map().physical[uint32_t(Button::A)], player_on_ground(), depth());
    }
};

// Run 5: a fresh boot restores the remapped layout from the save store.
struct RemapLoadGame final : PlatformerGame {
    bool ok = false; int checks = 0, fail = 0;
    void on_stop(App& app) override {
        auto CK = [&](bool c, const char* m) { ++checks; if (!c) { ++fail; std::printf("    FAIL %s\n", m); } };
        CK(was_loaded(),           "the v2 save was restored at boot");
        CK(app.input_map().physical[uint32_t(Button::A)] == uint8_t(Button::R),
           "the control remap survived the power cycle");
        ok = (fail == 0);
        std::printf("    loaded=%d  map[A]=%u\n", was_loaded(),
                    app.input_map().physical[uint32_t(Button::A)]);
    }
};
} // namespace

static Config make_cfg() {
    Config cfg = Config::from_defaults();
    cfg.sim_hz = 60; cfg.title = "platformer";
    cfg.width = 128; cfg.height = 96;
    cfg.total_ram = 32u << 20; cfg.frame_scratch = 256u << 10; cfg.max_entities = 1024;
    return cfg;
}

int main() {
    const char* bundle = "build/platformer.phxp";
    if (!bake_platformer_assets(bundle)) { std::printf("PLATFORMER FAIL (bake)\n"); return 1; }
    std::remove(kSave);                                     // start from a clean save store

    // --- Run 1: a full playthrough that also saves on pause -----------------
    // Scripted controller (per-frame button masks): press Start, then walk right the whole way
    // while hopping, so the player collects coins, reaches the patroller + spike, and lands on
    // the enemy from above (a stomp). Pause near the end saves; resume continues.
    static uint32_t script[FRAMES];
    for (auto& s : script) s = 0;
    script[1] = button_bit(Button::Start);                  // title -> level (replace)
    for (int f = 3; f < 120; ++f) script[f] = button_bit(Button::Right);   // walk right
    for (int f = 6; f < 118; f += 20) script[f] |= button_bit(Button::A);  // hop (jump when grounded)
    script[124] = button_bit(Button::Start);                // pause -> save
    script[127] = button_bit(Button::Start);                // resume

    phx_null_set_step_ns(1000000000ull / 60);
    phx_null_set_max_frames(FRAMES);
    phx_null_set_button_script(script, FRAMES);

    int rc1;
    { App app(make_cfg()); VerifyGame game; game.bundle = bundle; game.save_path = kSave;
      rc1 = app.run(&game);
      std::printf("\nplatformer_test run1: rc=%d  %d checks, %d failures\n", rc1, game.checks, game.fail);
      if (rc1 != 0 || !game.ok) { std::printf("PLATFORMER FAIL\n\n"); return 1; } }

    // --- Run 2: a fresh boot that should restore the saved run --------------
    static uint32_t idle[8] = {0};                          // no input: stay on the title, just load
    phx_null_set_max_frames(8);
    phx_null_set_button_script(idle, 8);

    int rc2;
    { App app(make_cfg()); LoadGame game; game.bundle = bundle; game.save_path = kSave;
      rc2 = app.run(&game);
      std::printf("platformer_test run2: rc=%d  %d checks, %d failures\n", rc2, game.checks, game.fail);
      if (rc2 != 0 || !game.ok) { std::printf("PLATFORMER FAIL\n\n"); return 1; } }

    // --- Run 3: the tile-hazard proof — walk left into the lava pit -----------
    constexpr int LAVA_FRAMES = 100;
    static uint32_t lava_script[LAVA_FRAMES];
    for (auto& s : lava_script) s = 0;
    lava_script[1] = button_bit(Button::Start);             // title -> level
    for (int f = 3; f < LAVA_FRAMES; ++f) lava_script[f] = button_bit(Button::Left);
    phx_null_set_max_frames(LAVA_FRAMES);
    phx_null_set_button_script(lava_script, LAVA_FRAMES);

    int rc3;
    { App app(make_cfg()); LavaGame game; game.bundle = bundle; game.save_path = kSave;
      rc3 = app.run(&game);
      std::printf("platformer_test run3: rc=%d  %d checks, %d failures\n", rc3, game.checks, game.fail);
      if (rc3 != 0 || !game.ok) { std::printf("PLATFORMER FAIL\n\n"); return 1; } }

    // --- Run 4: the options/remap flow through the real pause menu UI ---------
    // pause -> Down to OPTIONS -> A -> A on JUMP row (listen) -> press R (capture) ->
    // 4x Down to BACK -> R (confirm: A is now fed by R) -> Start resumes -> R jumps ->
    // Start pauses again (saving the remapped layout).
    constexpr int REMAP_FRAMES = 40;
    static uint32_t remap_script[REMAP_FRAMES];
    for (auto& s : remap_script) s = 0;
    remap_script[1]  = button_bit(Button::Start);   // title -> level
    remap_script[3]  = button_bit(Button::Start);   // level -> pause (saves identity map)
    remap_script[5]  = button_bit(Button::Down);    // focus RESUME -> OPTIONS
    remap_script[7]  = button_bit(Button::A);       // enter options
    remap_script[10] = button_bit(Button::A);       // JUMP row -> listening
    remap_script[12] = button_bit(Button::R);       // captured: JUMP := physical R
    remap_script[14] = button_bit(Button::Down);    // JUMP -> PAUSE
    remap_script[16] = button_bit(Button::Down);    // PAUSE -> DZ
    remap_script[18] = button_bit(Button::Down);    // DZ -> RESET
    remap_script[20] = button_bit(Button::Down);    // RESET -> BACK
    remap_script[22] = button_bit(Button::R);       // confirm BACK (logical A == phys R now)
    remap_script[24] = button_bit(Button::Start);   // pause -> level
    remap_script[27] = button_bit(Button::R);       // JUMP with the rebound button
    remap_script[29] = button_bit(Button::Start);   // pause again -> SAVE the remap
    phx_null_set_max_frames(REMAP_FRAMES);
    phx_null_set_button_script(remap_script, REMAP_FRAMES);

    int rc4;
    { App app(make_cfg()); RemapGame game; game.bundle = bundle; game.save_path = kSave;
      rc4 = app.run(&game);
      std::printf("platformer_test run4: rc=%d  %d checks, %d failures\n", rc4, game.checks, game.fail);
      if (rc4 != 0 || !game.ok) { std::printf("PLATFORMER FAIL\n\n"); return 1; } }

    // --- Run 5: the remapped layout survives a "power cycle" ------------------
    static uint32_t idle5[8] = {0};
    phx_null_set_max_frames(8);
    phx_null_set_button_script(idle5, 8);

    int rc5;
    { App app(make_cfg()); RemapLoadGame game; game.bundle = bundle; game.save_path = kSave;
      rc5 = app.run(&game);
      std::printf("platformer_test run5: rc=%d  %d checks, %d failures\n", rc5, game.checks, game.fail);
      if (rc5 != 0 || !game.ok) { std::printf("PLATFORMER FAIL\n\n"); return 1; } }

    std::printf("PLATFORMER PASS\n\n");
    return 0;
}
