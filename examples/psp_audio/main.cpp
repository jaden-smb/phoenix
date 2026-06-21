// examples/psp_audio/main.cpp — verifies the PSP sceAudio output device on a real emulator/PSP.
// The portable AudioMixer + lock-free AudioCommandQueue are already proven on the host; what this
// exercises is the PSP DEVICE path: phx_psp_audio_start() reserving a real sceAudio channel and a
// dedicated PSP thread driving sceAudioOutputBlocking from the game-supplied fill callback.
//
// The game thread pushes a PlaySfx intent into the SPSC queue; the audio thread drains it into the
// mixer and mixes a 440 Hz tone into the device buffer. The fill tracks how many frames the device
// pulled and the peak |sample| produced, and the result is reported via a thread NAME visible in
// PPSSPP's log:  sceKernelCreateThread("AUDIO_DEVICE_PASS"/"AUDIO_DEVICE_FAIL", ...).
// Built by `make psp-audio`. Run: grep the log for AUDIO_DEVICE_PASS.
#include "phx/audio/mixer.h"
#include "phx/audio/command_queue.h"
#include "phx/memory/allocators.h"
#include "phx/core/caps.h"

#include <pspkernel.h>
#include <stdlib.h>

PSP_MODULE_INFO("PHX_AUDIO_VERIFY", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(8192);

using namespace phx;

extern "C" int  phx_psp_audio_start(int rate, void (*fill)(void*, int16_t*, int), void* user);
extern "C" void phx_psp_audio_stop(void);

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

struct FillState {
    AudioMixer*        mixer = nullptr;
    AudioCommandQueue* queue = nullptr;
    volatile unsigned  frames = 0;     // device-pulled frame count (proves the audio thread ran)
    volatile int       peak   = 0;     // max |sample| produced (proves non-silent output)
};

// Runs on the PSP audio thread: drain the game's intents into the mixer, then mix into the device
// buffer — the single-threaded-mixer discipline the engine prescribes for device playback.
void audio_fill(void* user, int16_t* out, int frames) {
    FillState* s = static_cast<FillState*>(user);
    s->queue->drain(*s->mixer);
    s->mixer->mix(out, unsigned(frames));
    int p = 0;
    for (int i = 0; i < frames * 2; ++i) { int a = out[i] < 0 ? -out[i] : out[i]; if (a > p) p = a; }
    if (p > s->peak) s->peak = p;
    s->frames += unsigned(frames);
}
} // namespace

int main() {
    setup_callbacks();

    const size_t kArena = 512 * 1024;
    void* arena_buf = malloc(kArena);
    if (!arena_buf) { sceKernelExitGame(); return 0; }
    ArenaAllocator arena; arena.init(arena_buf, kArena);

    auto mr = AudioMixer::create(arena, caps(), 44100);
    if (!mr.ok()) { sceKernelExitGame(); return 0; }
    AudioMixer* mixer = mr.unwrap();

    static AudioCommand storage[16];
    AudioCommandQueue queue; queue.init(storage, 16);

    // A 0.25s 440 Hz square tone (amplitude 9000) the game "plays" as an SFX.
    const uint32_t nframes = 44100 / 4;
    static int16_t tone[44100 / 4];
    for (uint32_t i = 0; i < nframes; ++i) tone[i] = ((i / 50) & 1) ? 9000 : -9000;
    SoundView sfx{ tone, nframes, 44100 };

    static FillState st;
    st.mixer = mixer; st.queue = &queue;

    if (phx_psp_audio_start(44100, audio_fill, &st) != 0) {
        int vt = sceKernelCreateThread("AUDIO_DEVICE_FAIL", cb_thread, 0x18, 0xFA0, 0, nullptr);
        (void)vt;
        for (;;) sceKernelDelayThread(1000000);
    }

    // Game thread: push the SFX intent (the audio thread drains + plays it).
    queue.play_sfx(sfx, 1.0f, 0.0f, false);

    sceKernelDelayThread(400000);   // 0.4s — let the device consume the tone
    phx_psp_audio_stop();

    const bool ok = (st.frames > 0) && (st.peak == 9000);
    int vt = sceKernelCreateThread(ok ? "AUDIO_DEVICE_PASS" : "AUDIO_DEVICE_FAIL",
                                   cb_thread, 0x18, 0xFA0, 0, nullptr);
    (void)vt;

    for (;;) sceKernelDelayThread(1000000);   // keep running so the verdict thread is observable
    return 0;
}
