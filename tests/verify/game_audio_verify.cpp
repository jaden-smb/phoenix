// tests/verify/game_audio_verify.cpp — the App's engine-owned audio (phx/runtime/audio.h) on a
// REAL output device. audio_device_verify.cpp proves the SDL device glue with a hand-wired mixer;
// this proves the path a game actually uses: a PHX_GAME-style Game calls app.audio().play() and
// nothing else, and the engine must start the platform's device through the seam
// (phx_platform::audio()) and have its callback pull samples. The game queues a short tone on
// start and keeps a looping one going, so the device has something to mix throughout.
//
// Built by `make game-audio-verify`. Needs SDL2, a window and an output device. Exit 0 = PASS.
#include "phx/runtime/app.h"

#include <cstdio>

using namespace phx;

namespace {
struct ToneGame final : Game {
    int16_t  tone[4410];                 // 0.1 s of a 441 Hz square wave at 44.1 kHz
    bool     queued = false, saw_device = false;
    uint32_t frames_at_end = 0;
    uint32_t rendered = 0;

    void on_start(App& app) override {
        for (int i = 0; i < 4410; ++i) tone[i] = ((i / 50) & 1) ? int16_t(6000) : int16_t(-6000);
        queued = app.audio().play(SoundView{ tone, 4410, 44100 }, 0.25f, 0.0f, true);
    }
    void on_render(App& app, scalar) override {
        saw_device = saw_device || app.audio().has_device();
        frames_at_end = app.audio().frames_mixed();
        if (++rendered >= 45) {          // ~0.75 s: several device buffers' worth
            app.audio().stop_all();
            app.request_quit();
        }
    }
};
} // namespace

int main() {
    Config cfg = Config::from_defaults();
    cfg.title = "game audio verify";
    cfg.width = 160; cfg.height = 120;
    cfg.total_ram = 16u << 20; cfg.frame_scratch = 256u << 10;
    App app(cfg);
    static ToneGame game;
    const int rc = app.run(&game);
    const bool ok = rc == 0 && game.queued && game.saw_device && game.frames_at_end > 0 &&
                    app.audio().plays() == 1;
    std::printf("game audio: rc=%d queued=%d device=%d frames_mixed=%u rate=%u\n", rc, game.queued,
                game.saw_device, unsigned(game.frames_at_end), unsigned(app.audio().rate()));
    std::printf(ok ? "GAME AUDIO DEVICE PASS\n" : "GAME AUDIO DEVICE FAIL\n");
    return ok ? 0 : 1;
}
