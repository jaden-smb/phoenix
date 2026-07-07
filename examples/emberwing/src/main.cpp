// examples/emberwing/main.cpp — the headless/production entry point. One source, four
// targets: this compiles UNCHANGED for Linux/Windows (null or SDL platform picked at link
// time by the build; see desktop_main.cpp for the windowed build with a live audio device).
// It bakes the bundle first (a real project ships a prebuilt .phxp), then runs the App.
#include "game.h"
#include "bake.h"          // host-only; console builds embed a prebaked bundle instead

using namespace phx;
using namespace game;

int main() {
    const char* bundle = "emberwing.phxp";
    if (!bake_emberwing_assets(bundle)) {
        std::fprintf(stderr, "emberwing: could not write %s\n", bundle);
        return 1;
    }

    Config cfg = Config::from_defaults();
    cfg.sim_hz = 60;
    cfg.title  = "Emberwing — Cinder Hollow";
    cfg.width  = 240; cfg.height = 160;            // GBA-native view; PSP/PC scale up
    cfg.total_ram = 32u << 20; cfg.frame_scratch = 256u << 10; cfg.max_entities = 1024;

    App app(cfg);
    EmberwingGame game;
    game.bundle = bundle;
    return app.run(&game);
}
