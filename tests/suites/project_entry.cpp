// tests/suites/project_entry.cpp — main() for `make project-check`'s headless runs of a game
// project: the engine's desktop entry, except that PHX_PROFILE=gba|psp|desktop picks the target
// profile (budgets + fixed resolution, phx/runtime/main.h) so the host runs the game exactly as
// the console entry would configure it. Linked with the null platform; never a shipping entry.
//
// Input is scripted like a player's: idle, a tap of A (the template's jump sound) while walking
// right, then walking left. With PHX_EXPECT_AUDIO=1 the run fails unless the game queued a sound
// and the App's headless mix of it was audible (App::audio(): no device on the null platform).
#include "phx/runtime/main.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void phx_null_set_button_script(const uint32_t* masks, uint32_t n);

namespace {
constexpr uint32_t kA = 1u << 4, kLeft = 1u << 2, kRight = 1u << 3;   // canonical phx_button bits
uint32_t g_script[120];
} // namespace

int main() {
    const char* p = std::getenv("PHX_PROFILE");
    phx::TargetProfile target = !p                     ? phx::kTargetDesktop
                              : !std::strcmp(p, "gba") ? phx::kTargetGba
                              : !std::strcmp(p, "psp") ? phx::kTargetPsp
                                                       : phx::kTargetDesktop;
    // Off hardware the GBA PPU backend composes each frame into a 240x160 RGBA buffer carved from
    // the arena; on the console that image is VRAM, which costs the arena nothing. Grant exactly
    // that, so everything else (ECS, bundle, tiles, audio, the game) runs at the real GBA budget.
    if (target.screen_w == 240 && target.screen_h == 160) target.total_ram += 240u * 160u * 4u + 64u;

    for (uint32_t f = 0; f < 120; ++f)
        g_script[f] = f < 30 ? 0u : f < 60 ? (kRight | (f == 30 ? kA : 0u)) : f < 90 ? kLeft : 0u;
    phx_null_set_button_script(g_script, 120);

    phx::Game& game = phx::game_instance();
    phx::App app(phx::game_config(game, target));
    const int rc = app.run(&game);
    std::printf("[project] audio: %u sound(s) queued, headless peak %d\n",
                unsigned(app.audio().plays()), int(app.audio().peak()));
    const char* want = std::getenv("PHX_EXPECT_AUDIO");
    if (rc == 0 && want && *want == '1' && (app.audio().plays() == 0 || app.audio().peak() == 0)) {
        std::printf("[project][ERROR] the game made no sound\n");
        return 3;
    }
    return rc;
}
