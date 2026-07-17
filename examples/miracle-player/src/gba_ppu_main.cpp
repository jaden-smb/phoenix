// examples/miracle-player/src/gba_ppu_main.cpp — the SHIPPING GBA entry: the "A Small Miracle"
// visualizer on the native PPU (Mode-0 tiles + OBJ). The spectrogram is a BG tilemap (the PPU
// can't scale/tint OBJ, so ui.rect bars can't work there — the BG map is the native answer),
// particles are 8x8 OBJ sparks, text is OBJ glyphs. The tier-0 .phxp (18157 Hz audio + viz baked
// at that rate) is linked into ROM by bin2s and read zero-copy. Same game code as the host build;
// only this boot boilerplate + the linked PPU backend differ. Built by `make gba-miracle-ppu`.
#include "player.h"

#include <cstring>

using namespace phx;
using namespace miracle;

// The .phxp bundle linked into the ROM by bin2s (read-only, in cartridge ROM).
extern "C" const unsigned char miracle_phxp[];
extern "C" const unsigned int  miracle_phxp_size;

// Game-side hooks exported by the GBA platform backend (not part of the C seam).
extern "C" void phx_gba_set_bundle(const void* data, unsigned long size);
extern "C" void phx_gba_set_direct(int on);
extern "C" int  phx_gba_audio_start(int rate, void (*fill)(void*, int16_t*, int), void* user);

namespace {
constexpr int kGbaAudioRate = 18157;               // vblank-locked Direct Sound rate (304/frame)
struct AudioGlue { MiracleGame* game = nullptr; };

// Runs in the VBLANK audio IRQ (the "audio thread"): service any pending seek (the main loop has
// gated its pump off — the seek handshake is IRQ-safe), then mix + advance while draining.
void audio_fill(void* user, int16_t* out, int frames) {
    MiracleGame* g = static_cast<AudioGlue*>(user)->game;
    if (g->seek_state.load(std::memory_order_acquire)) g->apply_seek();
    if (g->draining.load(std::memory_order_acquire)) {
        g->mixer->mix(out, uint32_t(frames));
        g->advance_played(uint32_t(frames));
    } else {
        std::memset(out, 0, size_t(frames) * 2 * sizeof(int16_t));
    }
}
} // namespace

struct GbaGame final : MiracleGame {
    AudioGlue glue{};
    bool audio_started = false;
    void on_fixed_update(App& a, scalar dt) override {
        if (!audio_started && mixer) {              // mixer exists after on_start
            glue = AudioGlue{ this };
            audio_started = phx_gba_audio_start(kGbaAudioRate, audio_fill, &glue) == 0;
            if (!audio_started) device_audio = false;
        }
        MiracleGame::on_fixed_update(a, dt);
    }
};

int main() {
    phx_gba_set_bundle(miracle_phxp, miracle_phxp_size);
    phx_gba_set_direct(1);                          // PPU owns the display; stub the soft fb

    Config cfg = Config::from_defaults();
    cfg.sim_hz = 60;
    cfg.title  = "A Small Miracle (PPU)";
    cfg.width  = 240; cfg.height = 160;             // native PPU resolution (no 2x upscale)
    cfg.total_ram     = 160u << 10;                 // EWRAM engine arena (ring + mixer + cache +
                                                    // the spectrogram cells + particle pool)
    cfg.frame_scratch = 4u  << 10;
    cfg.max_entities  = 16;                         // this app uses no ECS entities

    App app(cfg);

    // The game object is small (its big buffers live in the arena, allocated in on_start), so it
    // sits safely on the GBA's tiny IWRAM stack even when the audio IRQ (2 KB mixer accumulator)
    // preempts a deep call chain.
    GbaGame game;
    game.mixer_rate   = kGbaAudioRate;             // mix 1:1 with the tier-0-baked audio + viz
    game.device_audio = true;
    game.bundle = "miracle.phxp";                  // path ignored on GBA (single ROM bundle)
    return app.run(&game);                         // runs until power-off
}
