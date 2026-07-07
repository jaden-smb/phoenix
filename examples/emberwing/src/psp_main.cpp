// examples/emberwing/psp_main.cpp — the PSP entry point. Same game/engine TUs; PSP boot
// boilerplate + the EBOOT-embedded bundle + the sceAudio thread, mirroring the platformer's
// psp_main.cpp. The audio thread drains the game's SPSC command queue into the mixer (the
// engine's prescribed device discipline). Built by `make psp-emberwing` (pspsdk).
#include "game.h"

#include <pspkernel.h>
#include <stdlib.h>

PSP_MODULE_INFO("Emberwing", 0, 1, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(16384);

// Standard HOME-button exit plumbing so the EBOOT behaves on hardware / PPSSPP.
namespace {
int exit_cb(int, int, void*) { sceKernelExitGame(); return 0; }
int cb_thread(SceSize, void*) {
    int cb = sceKernelCreateCallback("exit", exit_cb, nullptr);
    sceKernelRegisterExitCallback(cb);
    sceKernelSleepThreadCB();
    return 0;
}
void setup_callbacks() {
    int th = sceKernelCreateThread("cb", cb_thread, 0x11, 0xFA0, 0, nullptr);
    if (th >= 0) sceKernelStartThread(th, 0, nullptr);
}
} // namespace

using namespace phx;
using namespace game;

// The .phxp bundle linked into the EBOOT by bin2s (read-only).
extern "C" const unsigned char emberwing_phxp[];
extern "C" const unsigned int  emberwing_phxp_size;

// Game-side hooks exported by the PSP platform backend (not part of the C seam).
extern "C" void phx_psp_set_bundle(const void* data, unsigned long size);
extern "C" int  phx_psp_audio_start(int rate, void (*fill)(void*, int16_t*, int), void* user);
extern "C" void phx_psp_audio_stop(void);

namespace {
struct AudioGlue { AudioMixer* mixer; AudioCommandQueue* queue; };

void audio_fill(void* user, int16_t* out, int frames) {   // runs on the sceAudio thread
    AudioGlue* g = static_cast<AudioGlue*>(user);
    g->queue->drain(*g->mixer);
    g->mixer->mix(out, uint32_t(frames));
}
} // namespace

int main() {
    setup_callbacks();
    phx_psp_set_bundle(emberwing_phxp, emberwing_phxp_size);

    Config cfg = Config::from_defaults();
    cfg.sim_hz = 60;
    cfg.title  = "Emberwing — Cinder Hollow";
    cfg.width  = 240; cfg.height = 136;            // half-PSP; the backend 2x-scales to 480x272
    cfg.total_ram     = 4u << 20;
    cfg.frame_scratch = 64u << 10;
    cfg.max_entities  = 1024;

    App app(cfg);

    struct PspGame final : EmberwingGame {
        AudioGlue glue{};
        bool audio_started = false;
        void on_fixed_update(App& a, scalar dt) override {
            if (!audio_started && mixer) {
                glue = AudioGlue{ mixer, &queue };
                audio_started = phx_psp_audio_start(int(mixer_rate), audio_fill, &glue) == 0;
                if (!audio_started) device_audio = false;
            }
            EmberwingGame::on_fixed_update(a, dt);
        }
        void on_stop(App& a) override {
            if (audio_started) phx_psp_audio_stop();
            EmberwingGame::on_stop(a);
        }
    };

    PspGame game;
    game.scene_scratch_bytes = 64u << 10;
    game.device_audio = true;
    game.bundle = "emberwing.phxp";                // path ignored on PSP (embedded bundle)
    return app.run(&game);                          // runs until HOME → Exit
}
