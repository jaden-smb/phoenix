// examples/tinyllm/src/gba_ppu_main.cpp — the SHIPPING GBA entry: a transformer language model
// generating text on real Game Boy Advance hardware, through the native PPU (Mode 0 tiles).
//
// The whole memory strategy is visible in this file. The baked `.phxp` — which contains the
// ~300 KB int8 model — is linked into CARTRIDGE ROM by bin2s and read zero-copy in place; the
// only things in the console's 256 KB of EWRAM are the engine arena, the KV cache, and one
// screen of tile indices. Nothing about the model is ever copied into RAM.
//
// Same game code as the host build; only this boot boilerplate and the linked PPU backend
// differ. Built by `make gba-tinyllm-ppu`.
#include "tinyllm.h"

using namespace phx;
using namespace tinyllm;

// The .phxp bundle linked into the ROM by bin2s (read-only, in cartridge ROM).
extern "C" const unsigned char tinyllm_phxp[];
extern "C" const unsigned int  tinyllm_phxp_size;

// Game-side hooks exported by the GBA platform backend (not part of the C seam).
extern "C" void phx_gba_set_bundle(const void* data, unsigned long size);
extern "C" void phx_gba_set_direct(int on);
extern "C" void phx_gba_vblank_clock_start(void);

int main() {
    phx_gba_set_bundle(tinyllm_phxp, tinyllm_phxp_size);
    phx_gba_set_direct(1);                          // PPU owns the display; stub the soft fb
    // Give the fixed-step loop a real elapsed-frame count. Without this the platform advances
    // the sim clock one step per LOOP ITERATION (its fallback when nothing counts vblanks),
    // which for a ROM that deliberately spends 70 ms a frame on inference means elapsed steps
    // stop meaning elapsed time — and the status line's tokens/sec would read ~3x high.
    phx_gba_vblank_clock_start();

    Config cfg = Config::from_defaults();
    cfg.sim_hz = 60;
    cfg.title  = "tinyllm (PPU)";
    cfg.width  = 240; cfg.height = 160;             // native PPU resolution (no 2x upscale)
    // EWRAM engine arena: the LLM working set (activations + logits + the KV cache) dominates
    // it. See examples/tinyllm/README.md for the budget table; `make size-gate-tinyllm` gates it.
    cfg.total_ram     = 200u << 10;
    cfg.frame_scratch = 4u   << 10;
    cfg.max_entities  = 16;                         // this app uses no ECS entities

    App app(cfg);

    // The game object stays small (the console cells and the whole LLM working set are
    // arena-allocated in on_start), so it sits safely on the GBA's tiny IWRAM stack.
    TinyLlmGame game;
    game.bundle = "tinyllm.phxp";                   // path ignored on GBA (single ROM bundle)
    game.kv_budget_bytes = 96u << 10;
    // Two transformer stages per rendered frame. Measured on mGBA with the 260K model — the
    // trade is display refresh against the per-frame vblank quantization tax:
    //   1 stage/frame  -> 21.4 fps, 1.96 tok/s
    //   2 stages/frame -> 11.9 fps, 2.17 tok/s   <- shipped
    //   3 stages/frame ->  9.5 fps, 2.39 tok/s
    //  11 stages/frame ->  2.6 fps, 2.62 tok/s   (a whole token per frame: the inference-bound
    //                                             ceiling, but the console freezes 380 ms at a time)
    // Two keeps the console redrawing ~5 times per token (so the busy marker reads as working)
    // AND keeps a frame under the platform's 5-vblank catch-up clamp, which is what makes the
    // elapsed-step count a true clock for the tokens/sec readout.
    game.stages_per_frame = 2;
    return app.run(&game);                          // runs until power-off
}
