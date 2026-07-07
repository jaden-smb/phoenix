// examples/emberwing/gba_ppu_main.cpp — GBA entry for Emberwing rendered by the NATIVE PPU
// backend (Mode-0 tiles + OBJ on real silicon), as opposed to gba_main.cpp which drives the
// software rasterizer into a Mode-3 bitmap. This is the SHIPPING GBA configuration: the CPU
// stops rasterizing pixels entirely (the PPU scans out four streamed text BGs + OAM), which
// frees the frame budget the software path was blowing — and with it the audio pump, so the
// DirectSound stream stops starving. Same game, same systems/scenes TUs, same
// tier-0 bundle; the differences from gba_main.cpp:
//   1. It links the PPU backend (gba_ppu.cpp) instead of the soft rasterizer (link-time
//      choice; see `make gba-emberwing-ppu`), so the engine programs VRAM/OAM/PALRAM.
//   2. It renders at the GBA's NATIVE 240x160 (no 2x-upscale step) — the full designed view.
//   3. It calls phx_gba_set_direct(1) BEFORE boot (the backend sets it again, but that is
//      too late for the platform's init, which would otherwise try to allocate a 240x160
//      software framebuffer — 150 KB of EWRAM that must stay with the engine arena). With
//      direct mode pre-set the platform allocates only a 1x1 stub framebuffer.
// Built by `make gba-emberwing-ppu` (devkitARM); not part of the host `make check`.
#include "game.h"

using namespace phx;
using namespace game;

// The .phxp bundle linked into the ROM by bin2s (read-only, lives in cartridge ROM).
extern "C" const unsigned char emberwing_phxp[];
extern "C" const unsigned int  emberwing_phxp_size;

// Game-side hooks exported by the GBA platform backend (not part of the C seam).
extern "C" void phx_gba_set_bundle(const void* data, unsigned long size);
extern "C" void phx_gba_set_direct(int on);
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
    phx_gba_set_direct(1);                         // PPU owns the display; stub the soft fb

    Config cfg = Config::from_defaults();
    cfg.sim_hz = 60;
    cfg.title  = "Emberwing (PPU)";
    cfg.width  = 240; cfg.height = 160;            // native PPU resolution (no 2x upscale)
    cfg.total_ram     = 160u << 10;                // EWRAM engine arena (PPU stores ~45 KB)
    cfg.frame_scratch = 4u  << 10;
    cfg.max_entities  = 160;                       // ~96 spawns + particle bursts, with slack

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
    game.scene_scratch_bytes = 12u << 10;          // scenes are tiny on GBA
    game.mixer_rate   = kGbaAudioRate;             // mix 1:1 with the tier-0-baked sounds
    game.device_audio = true;
    game.bundle = "emberwing.phxp";                // path ignored on GBA (single ROM bundle)
    return app.run(&game);                          // runs until power-off
}
