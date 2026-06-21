// examples/gba_audio/main.cpp — verifies the GBA DirectSound output device on emulated hardware.
// The portable AudioMixer + lock-free AudioCommandQueue are already host-proven; this exercises the
// GBA DEVICE path: phx_gba_audio_start() programming SOUNDCNT/Timer0/DMA1->FIFO A, and the per-frame
// pump (in present()) double-buffering 8-bit PCM downmixed from the mixer's stereo S16 output.
//
// The GBA has no console, so the verdict is published in GDB-readable globals (read with the mGBA
// GDB stub, the same way the platformer ROM was verified): phx_gba_audio_verdict==1 on success,
// plus phx_gba_audio_frames / phx_gba_audio_peak. The sound registers (SOUNDCNT_X/H, TM0CNT, DMA1)
// and the DMA source buffer (a +/-35 square wave) can also be inspected directly. Built `make gba-audio`.
#include "phx/platform/platform.h"
#include "phx/audio/mixer.h"
#include "phx/audio/command_queue.h"
#include "phx/memory/allocators.h"
#include "phx/core/caps.h"

#include <stdlib.h>

using namespace phx;

extern "C" void phx_gba_set_direct(int on);
extern "C" int  phx_gba_audio_start(int rate, void (*fill)(void*, int16_t*, int), void* user);
extern "C" void phx_gba_audio_stop(void);

// Published for the GDB stub to read (volatile so they survive -O2 and land in memory).
extern "C" {
volatile int phx_gba_audio_frames  = 0;
volatile int phx_gba_audio_peak    = 0;
volatile int phx_gba_audio_verdict = -1;   // -1 running, 1 pass, 0 fail
}

namespace {
struct FillState { AudioMixer* mixer; AudioCommandQueue* queue; };

// Runs inside present()->pump (the GBA's single thread): drain intents, mix into the device buffer.
void audio_fill(void* user, int16_t* out, int frames) {
    FillState* s = static_cast<FillState*>(user);
    s->queue->drain(*s->mixer);
    s->mixer->mix(out, unsigned(frames));
    int p = 0;
    for (int i = 0; i < frames * 2; ++i) { int a = out[i] < 0 ? -out[i] : out[i]; if (a > p) p = a; }
    if (p > phx_gba_audio_peak) phx_gba_audio_peak = p;
    phx_gba_audio_frames += frames;
}
} // namespace

int main() {
    phx_gba_set_direct(1);                         // no rendering here: present() just vblanks + pumps audio

    const phx_platform* plat = phx_platform_get();
    phx_platform_desc desc{}; desc.title = "phx-gba-audio"; desc.width = 240; desc.height = 160;
    if (plat->init(&desc) != 0) { for (;;) {} }

    // Big buffers must come from the EWRAM heap, not static arrays — the default gba.specs puts
    // .bss/.data in the 32 KB IWRAM, which a large static would overflow.
    const size_t kArena = 32 * 1024;
    void* arena_buf = malloc(kArena);
    if (!arena_buf) { phx_gba_audio_verdict = 0; for (;;) {} }
    ArenaAllocator arena; arena.init(arena_buf, kArena);
    auto mr = AudioMixer::create(arena, caps(), 16384);
    if (!mr.ok()) { phx_gba_audio_verdict = 0; for (;;) {} }
    AudioMixer* mixer = mr.unwrap();

    static AudioCommand storage[16];
    AudioCommandQueue queue; queue.init(storage, 16);

    // A 440 Hz-ish square tone (amplitude 9000) at 16384 Hz; the game "plays" it as an SFX.
    const uint32_t nframes = 16384 / 4;            // 0.25 s
    int16_t* tone = static_cast<int16_t*>(malloc(nframes * sizeof(int16_t)));
    if (!tone) { phx_gba_audio_verdict = 0; for (;;) {} }
    for (uint32_t i = 0; i < nframes; ++i) tone[i] = ((i / 18) & 1) ? 9000 : -9000;
    SoundView sfx{ tone, nframes, 16384 };

    static FillState st; st.mixer = mixer; st.queue = &queue;
    phx_gba_audio_start(16384, audio_fill, &st);
    queue.play_sfx(sfx, 1.0f, 0.0f, true);          // loop so the device buffer stays populated

    for (int f = 0; f < 60; ++f) plat->present();   // ~1 s: each present pumps a sound buffer
    phx_gba_audio_stop();

    phx_gba_audio_verdict =
        (phx_gba_audio_frames > 0 && phx_gba_audio_peak >= 4000 && phx_gba_audio_peak <= 12000) ? 1 : 0;

    for (;;) { plat->present(); }                   // spin so the GDB stub can read the verdict
    return 0;
}
