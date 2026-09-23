// examples/tinyllm/src/main.cpp — the portable/headless entry (null platform), and the fast dev
// loop: bake the bundle, generate from a fixed prompt and seed, print the text to stdout.
//
// `PHX_MAX_FRAMES=N ./build/tinyllm` bounds the run. Environment knobs (all optional):
//   TINYLLM_MODEL   path to a .phxllm (default build/tinyllm.phxllm; falls back to the fixture)
//   TINYLLM_PROMPT  index 0..3 of the baked-in prompts
//   TINYLLM_GREEDY  1 = argmax instead of temperature/top-p sampling
//   TINYLLM_SEED    sampler seed
// The windowed build is desktop_main.cpp; the shipping console build is gba_ppu_main.cpp.
#include "tinyllm.h"
#include "bake.h"          // host-only bundle bake at boot

#include <cstdio>
#include <cstdlib>

using namespace phx;
using namespace tinyllm;

namespace {
uint32_t env_u32(const char* name, uint32_t fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    return uint32_t(std::strtoul(v, nullptr, 0));
}
} // namespace

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
    cfg.title  = "tinyllm";
    cfg.width  = 240; cfg.height = 160;
    cfg.total_ram = 32u << 20; cfg.frame_scratch = 256u << 10; cfg.max_entities = 16;

    App app(cfg);
    TinyLlmGame game;
    game.bundle = bundle;
    game.prompt_index    = int32_t(env_u32("TINYLLM_PROMPT", 0));
    game.sampler.greedy  = env_u32("TINYLLM_GREEDY", 0) != 0;
    game.sampler.seed    = env_u32("TINYLLM_SEED", 0x2545F491u);
    game.stages_per_frame = 4096;    // headless: run each token to completion in one frame
    game.max_tokens      = env_u32("TINYLLM_TOKENS", 96);
    game.quit_when_done  = true;
    const int rc = app.run(&game);

    // No tokens/sec here on purpose: the null platform runs a VIRTUAL clock (one microsecond per
    // read), so a headless throughput number would be fiction. Measure it in the SDL window
    // (`make tinyllm-sdl`) or on hardware — the on-screen status line is the real readout.
    std::printf("\n--- tinyllm ---\nprompt : %s\noutput : %s\ntokens : %u   ctx %d/%d\n",
                kPrompts[game.prompt_index], game.transcript(), game.tokens_generated(),
                game.context_used(), game.context_max());
    return rc;
}
