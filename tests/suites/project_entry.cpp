// tests/suites/project_entry.cpp — main() for `make project-check`'s headless runs of a game
// project: the engine's desktop entry, except that PHX_PROFILE=gba|psp|desktop picks the target
// profile (budgets + fixed resolution, phx/runtime/main.h) so the host runs the game exactly as
// the console entry would configure it. Linked with the null platform; never a shipping entry.
//
// Input is scripted like a player's: idle, A (past a title screen, or a jump), walking right with
// another A (a jump: the template's jump sound), then walking left. With PHX_EXPECT_AUDIO=1 the run fails unless the game queued a sound
// and the App's headless mix of it was audible (App::audio(): no device on the null platform).
//
// Budget runs (`make project-budget`): PHX_BUDGET_REPORT=file writes the run's budget report
// (phx/runtime/budget.h), and PHX_BUDGET_FRAMES=n repeats the scripted play (A, then running right
// with a jump every 40 frames, then a little left) for n frames.
#include "phx/runtime/main.h"
#include "phx/runtime/budget.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void phx_null_set_button_script(const uint32_t* masks, uint32_t n);

namespace {
constexpr uint32_t kA = 1u << 4, kLeft = 1u << 2, kRight = 1u << 3;   // canonical phx_button bits
uint32_t g_script[4096];
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
    const uint32_t host_only = target.screen_w == 240 && target.screen_h == 160 ? 240u * 160u * 4u + 64u : 0u;
    target.total_ram += host_only;

    for (uint32_t f = 0; f < 120; ++f)
        g_script[f] = f < 30 ? 0u : f < 60 ? (kRight | ((f == 30 || f == 45) ? kA : 0u)) : f < 90 ? kLeft : 0u;
    uint32_t script_n = 120;
    const char* report = std::getenv("PHX_BUDGET_REPORT");
    if (const char* fr = std::getenv("PHX_BUDGET_FRAMES"); fr && std::atoi(fr) > 120) {
        script_n = uint32_t(std::atoi(fr));
        if (script_n > 4096) script_n = 4096;
        for (uint32_t f = 120; f < script_n; ++f) {
            const uint32_t t = (f - 120) % 300;        // A (a title / a jump), run right jumping, step back
            g_script[f] = t == 0 ? kA : t < 260 ? (kRight | (t % 40 == 0 ? kA : 0u)) : kLeft;
        }
    }
    phx_null_set_button_script(g_script, script_n);

    phx::Game& game = phx::game_instance();
    phx::App app(phx::game_config(game, target));
    const int rc = app.run(&game);
    if (report && *report && !phx::write_budget_report(app, target, report, host_only))
        std::printf("[project][ERROR] cannot write the budget report %s\n", report);
    std::printf("[project] audio: %u sound(s) queued, headless peak %d\n",
                unsigned(app.audio().plays()), int(app.audio().peak()));
    const char* want = std::getenv("PHX_EXPECT_AUDIO");
    if (rc == 0 && want && *want == '1' && (app.audio().plays() == 0 || app.audio().peak() == 0)) {
        std::printf("[project][ERROR] the game made no sound\n");
        return 3;
    }
    return rc;
}
