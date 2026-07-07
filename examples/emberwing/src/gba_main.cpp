// examples/emberwing/gba_main.cpp — the GBA entry point. Same game, same engine, same
// systems/scenes TUs as the host build; what differs (exactly as in the platformer example):
//   1. No filesystem: the .phxp bundle is baked offline at tier 0 (sounds already at the
//      18157 Hz Direct Sound rate) and linked into the ROM; phx_gba_set_bundle() registers
//      it before boot so the game's res->mount() finds it.
//   2. Budgets sized for 256 KB EWRAM: a 120x80 framebuffer (the platform 2x-scales to
//      240x160), a ~160 KB engine arena, 192 entities, a small scene scratch.
//   3. Real audio: phx_gba_audio_start() drives DirectSound from the per-frame vblank pump;
//      the fill callback drains the game's command queue into the 18157 Hz mixer (single
//      -threaded on GBA, so the SPSC discipline is trivially satisfied).
// Built by `make gba-emberwing` (devkitARM); not part of the host `make check`.
#include "game.h"

using namespace phx;
using namespace game;

// The .phxp bundle linked into the ROM by bin2s (read-only, lives in cartridge ROM).
extern "C" const unsigned char emberwing_phxp[];
extern "C" const unsigned int  emberwing_phxp_size;

// Game-side hooks exported by the GBA platform backend (not part of the C seam).
extern "C" void phx_gba_set_bundle(const void* data, unsigned long size);
extern "C" int  phx_gba_audio_start(int rate, void (*fill)(void*, int16_t*, int), void* user);

namespace {
constexpr int kGbaAudioRate = 18157;               // vblank-locked Direct Sound rate (304/frame)
struct AudioGlue { AudioMixer* mixer; AudioCommandQueue* queue; };

void audio_fill(void* user, int16_t* out, int frames) {
    AudioGlue* g = static_cast<AudioGlue*>(user);
    g->queue->drain(*g->mixer);
    g->mixer->mix(out, uint32_t(frames));
}
} // namespace

int main() {
    phx_gba_set_bundle(emberwing_phxp, emberwing_phxp_size);

    Config cfg = Config::from_defaults();          // budgets seeded from phx::caps() (GBA tier)
    cfg.sim_hz = 60;
    cfg.title  = "Emberwing";
    cfg.width  = 120; cfg.height = 80;             // 2x-scaled to 240x160 by the GBA backend
    cfg.total_ram     = 160u << 10;                // EWRAM engine arena (fb allocated apart)
    cfg.frame_scratch = 4u  << 10;
    cfg.max_entities  = 192;                       // ~95 spawns + particle bursts, with slack

    App app(cfg);

    struct GbaGame final : EmberwingGame {
        AudioGlue glue{};
        bool audio_started = false;
        void on_fixed_update(App& a, scalar dt) override {
            if (!audio_started && mixer) {         // mixer exists after on_start
                glue = AudioGlue{ mixer, &queue };
                audio_started = phx_gba_audio_start(kGbaAudioRate, audio_fill, &glue) == 0;
                if (!audio_started) device_audio = false;
            }
            EmberwingGame::on_fixed_update(a, dt);
        }
    };

    GbaGame game;
    game.scene_scratch_bytes = 16u << 10;          // scenes are tiny on GBA
    game.mixer_rate   = kGbaAudioRate;             // mix 1:1 with the tier-0-baked sounds
    game.device_audio = true;
    game.bundle = "emberwing.phxp";                // path ignored on GBA (single ROM bundle)
    return app.run(&game);                          // runs until power-off
}
