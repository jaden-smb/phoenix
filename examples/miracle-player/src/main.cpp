// examples/miracle-player/src/main.cpp — the portable/headless entry (null platform). Bakes the
// bundle (the pre-decoded song WAV if present at build/miracle.wav, else a short synthetic tone)
// and runs the App with the headless audio stand-in. `PHX_MAX_FRAMES=N ./build/miracle` gives a
// bounded, clean-exit smoke run. The windowed build with real audio is desktop_main.cpp.
#include "player.h"
#include "bake.h"          // host-only bundle bake at boot

using namespace phx;
using namespace miracle;

int main() {
    const char* bundle = "miracle.phxp";
    if (!bake_miracle_assets(bundle, 2, "build/miracle.wav")) {
        std::fprintf(stderr, "miracle: could not write %s\n", bundle);
        return 1;
    }

    Config cfg = Config::from_defaults();
    cfg.sim_hz = 60;
    cfg.title  = "A Small Miracle";
    cfg.width  = 240; cfg.height = 160;
    cfg.total_ram = 32u << 20; cfg.frame_scratch = 256u << 10; cfg.max_entities = 64;

    App app(cfg);
    MiracleGame game;
    game.bundle = bundle;
    game.mixer_rate = 44100;
    game.device_audio = false;      // no device: the headless stand-in mixes + advances position
    return app.run(&game);
}
