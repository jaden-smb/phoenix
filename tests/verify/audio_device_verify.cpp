// tests/audio_device_verify.cpp — verifies the SDL audio DEVICE glue end to end on real
// hardware. The mixer + lock-free command queue are already unit-tested headlessly; what was
// never runnable here is the device path: phx_sdl_audio_start() opening a real output device and
// the SDL audio thread invoking our fill callback. This wires the production discipline — the
// game thread pushes a PlaySfx intent into the SPSC AudioCommandQueue, the audio thread drains
// it into the AudioMixer and mixes into the device buffer — then confirms the callback actually
// fired and produced non-silent audio.
//
// Built by `make audio-verify`. Needs SDL2 + an output device ($PULSE/pipewire/alsa).
#include "phx/audio/mixer.h"
#include "phx/audio/command_queue.h"
#include "phx/memory/allocators.h"
#include "phx/core/caps.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdint>

using namespace phx;

// Exported by the SDL backend (the only TU allowed to include SDL). Open a stereo S16 device
// and run `fill` on the audio thread; stop with phx_sdl_audio_stop().
extern "C" int  phx_sdl_audio_start(int rate, void (*fill)(void*, int16_t*, int), void* user);
extern "C" void phx_sdl_audio_stop(void);

namespace {
struct FillState {
    AudioMixer*        mixer = nullptr;
    AudioCommandQueue* queue = nullptr;
    std::atomic<uint64_t> frames{0};   // total frames the device asked for (proves the thread ran)
    std::atomic<int>      peak{0};     // max |sample| produced (proves non-silent output)
};

// Runs on the SDL audio thread: drain the game's intents into the mixer, then mix. This is the
// exact single-threaded-mixer discipline the engine prescribes for device playback.
void audio_fill(void* user, int16_t* out, int frames) {
    FillState* s = static_cast<FillState*>(user);
    s->queue->drain(*s->mixer);
    s->mixer->mix(out, frames);
    int p = 0;
    for (int i = 0; i < frames * 2; ++i) { int a = out[i] < 0 ? -out[i] : out[i]; if (a > p) p = a; }
    if (p > s->peak.load(std::memory_order_relaxed)) s->peak.store(p, std::memory_order_relaxed);
    s->frames.fetch_add(uint64_t(frames), std::memory_order_relaxed);
}
} // namespace

int main() {
    static uint8_t pool[1 << 20];
    ArenaAllocator arena; arena.init(pool, sizeof(pool));
    auto mr = AudioMixer::create(arena, caps(), 44100);
    if (!mr.ok()) { std::printf("mixer create failed\n"); return 1; }
    AudioMixer* mixer = mr.unwrap();

    static AudioCommand storage[16];
    AudioCommandQueue queue; queue.init(storage, 16);

    // A 0.25s 440Hz square tone the game will "play" as an SFX.
    const int rate = 44100;
    const uint32_t nframes = rate / 4;
    static int16_t tone[44100 / 4];
    for (uint32_t i = 0; i < nframes; ++i) tone[i] = ((i / 50) & 1) ? 9000 : -9000;
    SoundView sfx{ tone, nframes, uint32_t(rate) };

    FillState st; st.mixer = mixer; st.queue = &queue;

    if (phx_sdl_audio_start(rate, audio_fill, &st) != 0) {
        std::printf("phx_sdl_audio_start failed (no audio device?)\n");
        return 1;
    }

    // Game thread: push the SFX intent (the audio thread will drain + play it).
    queue.play_sfx(sfx, 1.0f, 0.0f, false);

    // Let the device run long enough to consume the tone.
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    phx_sdl_audio_stop();

    const uint64_t got = st.frames.load();
    const int      peak = st.peak.load();
    int checks = 0, fail = 0;
    ++checks; if (got == 0)    { ++fail; std::printf("    FAIL: audio callback never fired\n"); }
    ++checks; if (peak == 0)   { ++fail; std::printf("    FAIL: device output was silent (SFX never mixed)\n"); }
    ++checks; if (peak > 20000){ ++fail; std::printf("    FAIL: output clipped/garbage (peak=%d)\n", peak); }

    std::printf("\naudio_device_verify: device pulled %llu frames, peak |sample|=%d\n",
                (unsigned long long)got, peak);
    std::printf("audio_device_verify: %d checks, %d failures\n", checks, fail);
    std::printf(fail == 0 ? "AUDIO DEVICE PASS (SDL device opened; audio thread mixed the SFX live)\n\n"
                          : "AUDIO DEVICE FAIL\n\n");
    return fail == 0 ? 0 : 1;
}
