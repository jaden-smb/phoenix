// examples/tinyllm/src/desktop_main.cpp — the WINDOWED desktop entry (SDL software backend;
// `make tinyllm-sdl`). Renders exactly what the GBA renders — the same 240x160 tilemap console
// through the same game code — so console layout can be iterated on without flashing a ROM.
// Only this boot boilerplate and the linked backend differ from the headless build.
#include "tinyllm.h"
#include "bake.h"

#include <cstdio>
#include <cstdlib>

using namespace phx;
using namespace tinyllm;

int main() {
    const char* bundle = "tinyllm.phxp";
    const char* model  = std::getenv("TINYLLM_MODEL");
    if (!model || !*model) model = "build/tinyllm.phxllm";
    if (!bake_tinyllm_assets(bundle, 2, model)) {
        std::fprintf(stderr, "tinyllm: could not write %s\n", bundle);
        return 1;
    }

    Config cfg = Config::from_defaults();
    cfg.sim_hz = 60;
    cfg.title  = "tinyllm — a language model on a Game Boy Advance";
    cfg.width  = 240; cfg.height = 160;
    cfg.total_ram = 32u << 20; cfg.frame_scratch = 256u << 10; cfg.max_entities = 16;

    App app(cfg);
    TinyLlmGame game;
    game.bundle = bundle;
    // A desktop CPU runs a whole token in well under a frame, so give it the whole token; the
    // ROM's one-stage-per-frame pacing (gba_ppu_main.cpp) is what makes a 16 MHz console usable.
    game.stages_per_frame = 4096;
    return app.run(&game);
}
