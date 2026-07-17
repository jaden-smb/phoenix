// examples/miracle-player/src/desktop_main.cpp — the WINDOWED desktop entry (SDL software or GL
// backend, chosen at link time; `make miracle`). Opens a real audio device: the SDL audio thread
// mixes the streamed song into the device buffer and advances the shared sample cursor — the
// single source of truth the visualizer reads for A/V lock. Game code is identical to the
// headless build; only this boot boilerplate differs.
#include "player.h"
#include "bake.h"

#include <atomic>
#include <cstring>

using namespace phx;
using namespace miracle;

// Exported by the SDL platform backend (the only TU allowed to include SDL).
extern "C" int  phx_sdl_audio_start(int rate, void (*fill)(void*, int16_t*, int), void* user);
extern "C" void phx_sdl_audio_stop(void);

namespace {
struct AudioGlue { MiracleGame* game = nullptr; };

// Runs on the SDL audio thread — the audio-owning side of the seek handshake AND the mixer's sole
// caller. First service any pending seek (re-open the stream here, with the game's producer gated
// off), then mix the streamed song and advance the device sample count while draining; otherwise
// emit silence (intro / paused / ended) without touching the ring.
void audio_fill(void* user, int16_t* out, int frames) {
    MiracleGame* g = static_cast<AudioGlue*>(user)->game;
    if (g->seek_state.load(std::memory_order_acquire))
        g->apply_seek();
    if (g->draining.load(std::memory_order_acquire)) {
        g->mixer->mix(out, uint32_t(frames));
        g->advance_played(uint32_t(frames));
    } else {
        std::memset(out, 0, size_t(frames) * 2 * sizeof(int16_t));
    }
}
} // namespace

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

    // The mixer is created in on_start; run() boots subsystems before the first frame, so start
    // the device from a first-update hook (the mixer/stream/atomics are ready by then).
    struct DesktopGame final : MiracleGame {
        AudioGlue glue{};
        bool audio_started = false;
        void on_fixed_update(App& a, scalar dt) override {
            if (!audio_started && mixer) {
                glue = AudioGlue{ this };
                audio_started = phx_sdl_audio_start(int(mixer_rate), audio_fill, &glue) == 0;
                if (!audio_started) device_audio = false;   // no device: fall back to headless mix
            }
            MiracleGame::on_fixed_update(a, dt);
        }
        void on_stop(App& a) override {
            if (audio_started) phx_sdl_audio_stop();
            MiracleGame::on_stop(a);
        }
    };

    DesktopGame dgame;
    dgame.bundle = bundle;
    dgame.mixer_rate = 44100;
    dgame.device_audio = true;
    return app.run(&dgame);
}
