// phx/runtime/main.h — the engine-owned program entry for games. A game names its Game type
// once, at file scope in one of its source files:
//
//     struct MyGame final : phx::Game {
//         void on_configure(phx::Config& cfg) override { cfg.title = "My Game"; cfg.width = 240; cfg.height = 160; }
//         ...
//     };
//     PHX_GAME(MyGame);
//
// and never writes main(). Each target's main() is engine code (engine/runtime/src/entry/
// <target>_main.cpp), linked by the project build (`make game | game-gba | game-psp`): it does
// the target's boot chores (a console registers the bundle linked into the ROM/EBOOT, the GBA
// hands the display to the PPU, the PSP declares its module and heap) and then calls run_game()
// with the target's profile. That is how one game source builds for every target with no
// #ifdef: what differs per target lives here and in the entries, not in the game.
#ifndef PHX_RUNTIME_MAIN_H
#define PHX_RUNTIME_MAIN_H

#include "phx/runtime/app.h"

#include <type_traits>

namespace phx {

// What a target imposes on a game at boot.
struct TargetProfile {
    const char* name;
    // Budgets the target is proven with; 0 keeps Config::from_defaults() (the capability tier).
    // They are applied BEFORE Game::on_configure, so a game may still change them.
    uint32_t total_ram;
    uint32_t frame_scratch;
    uint32_t max_entities;
    // > 0: the only resolution the target renders (applied AFTER on_configure).
    int32_t  screen_w;
    int32_t  screen_h;
};

// PC (SDL window): the capability tier's budgets, any resolution.
inline constexpr TargetProfile kTargetDesktop{ "desktop", 0, 0, 0, 0, 0 };
// GBA on the native PPU: a 160 KB EWRAM arena (the rest of EWRAM holds the PPU's streamed
// tiles and the stack's neighbours), always the 240x160 LCD.
inline constexpr TargetProfile kTargetGba{ "gba", 160u << 10, 4u << 10, 256, 240, 160 };
// PSP (software renderer, 2x-scaled when the resolution fits): a 4 MB arena out of the
// EBOOT's 16 MB heap; any resolution up to 480x272.
inline constexpr TargetProfile kTargetPsp{ "psp", 4u << 20, 64u << 10, 1024, 0, 0 };

// The boot Config for `game` on `target`: the tier defaults, then the target's budgets, then
// Game::on_configure, then the target's fixed resolution (if it has one).
Config game_config(Game& game, const TargetProfile& target);

// game_config() + App::run(): boots the engine, runs the game until it quits, tears down.
int run_game(Game& game, const TargetProfile& target);

// The game named by PHX_GAME. Defined by the game, called by the entries.
Game& game_instance();

// Host builds only (engine/runtime/src/component_schema.cpp): write the program's reflected
// components (phx/ecs/reflect.h: names, fields, types, C++ defaults) as JSON to `path`. The
// desktop entry does this, and exits, when the environment names a file in PHX_DUMP_COMPONENTS.
bool write_component_schema(const char* path);

} // namespace phx

// Name the game's Game type (once per program, at file scope). The game object is a static:
// constructed before main(), so keep heavy set-up in on_start, where the engine exists.
#define PHX_GAME(GameType)                                                                     \
    namespace { GameType phx_game_instance_; }                                                 \
    phx::Game& phx::game_instance() { return phx_game_instance_; }                             \
    static_assert(std::is_base_of<phx::Game, GameType>::value, "PHX_GAME needs a phx::Game")

#endif // PHX_RUNTIME_MAIN_H
