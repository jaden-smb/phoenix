// examples/emberwing/desktop_main.cpp — the WINDOWED desktop entry (SDL software or GL
// backend, chosen at link time; see `make emberwing-sdl` / `make emberwing-gl`). Unlike
// main.cpp it opens the real audio device: the SDL audio thread drains the game's SPSC
// command queue into the mixer — the exact single-threaded-mixer discipline the engine
// prescribes (docs/10 §2). Game code is identical; only this boot boilerplate differs.
#include "game.h"
#include "bake.h"          // host-only bundle bake at boot

using namespace phx;
using namespace game;

// Exported by the SDL platform backend (the only TU allowed to include SDL).
extern "C" int  phx_sdl_audio_start(int rate, void (*fill)(void*, int16_t*, int), void* user);
extern "C" void phx_sdl_audio_stop(void);

namespace {
struct AudioGlue { AudioMixer* mixer; AudioCommandQueue* queue; };

// Runs on the SDL audio thread: apply the game's queued intents, then mix the device block.
void audio_fill(void* user, int16_t* out, int frames) {
    AudioGlue* g = static_cast<AudioGlue*>(user);
    g->queue->drain(*g->mixer);
    g->mixer->mix(out, uint32_t(frames));
}
} // namespace

int main() {
    const char* bundle = "emberwing.phxp";
    if (!bake_emberwing_assets(bundle)) {
        std::fprintf(stderr, "emberwing: could not write %s\n", bundle);
        return 1;
    }

    Config cfg = Config::from_defaults();
    cfg.sim_hz = 60;
    cfg.title  = "Emberwing — Cinder Hollow";
    cfg.width  = 240; cfg.height = 160;
    cfg.total_ram = 32u << 20; cfg.frame_scratch = 256u << 10; cfg.max_entities = 1024;

    App app(cfg);

    // The mixer is created in on_start; run() boots subsystems before the first frame, so
    // start the device from a tiny first-update hook. EmberwingGame exposes mixer/queue
    // publicly for exactly this glue.
    struct DesktopGame final : EmberwingGame {
        AudioGlue glue{};
        bool audio_started = false;
        void on_fixed_update(App& a, scalar dt) override {
            if (!audio_started && mixer) {
                glue = AudioGlue{ mixer, &queue };
                audio_started = phx_sdl_audio_start(int(mixer_rate), audio_fill, &glue) == 0;
                if (!audio_started) device_audio = false;   // no device: fall back headless
            }
            EmberwingGame::on_fixed_update(a, dt);
        }
        void on_stop(App& a) override {
            if (audio_started) phx_sdl_audio_stop();
            EmberwingGame::on_stop(a);
        }
    };

    DesktopGame dgame;
    dgame.bundle = bundle;
    dgame.device_audio = true;
    return app.run(&dgame);
}
