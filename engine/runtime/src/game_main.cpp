// engine/runtime/src/game_main.cpp — the portable half of the game entry (phx/runtime/main.h):
// the target profile -> boot Config policy, and the run. The per-target main()s live in
// src/entry/ and are linked only by project builds.
#include "phx/runtime/main.h"
#include "phx/core/log.h"

namespace phx {

Config game_config(Game& game, const TargetProfile& target) {
    Config cfg = Config::from_defaults();
    if (target.total_ram)     cfg.total_ram     = target.total_ram;
    if (target.frame_scratch) cfg.frame_scratch = target.frame_scratch;
    if (target.max_entities)  cfg.max_entities  = target.max_entities;
    game.on_configure(cfg);
    if (target.screen_w > 0 && target.screen_h > 0 &&
        (cfg.width != target.screen_w || cfg.height != target.screen_h)) {
        PHX_LOG_WARN("%s renders %dx%d; the game asked for %dx%d", target.name, int(target.screen_w),
                     int(target.screen_h), int(cfg.width), int(cfg.height));
        cfg.width  = target.screen_w;
        cfg.height = target.screen_h;
    }
    return cfg;
}

int run_game(Game& game, const TargetProfile& target) {
    App app(game_config(game, target));
    return app.run(&game);
}

} // namespace phx
