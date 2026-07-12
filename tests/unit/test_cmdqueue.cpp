// tests/test_cmdqueue.cpp — the lock-free audio command queue (the game/audio-thread seam).
// Verified single-threaded (deterministic): pushed commands drain into the mixer in order and
// produce the right voice state + mixed output; the queue reports full and never overruns. The
// SPSC atomics make it equally correct across the two real threads (game producer, audio callback
// consumer) — the structure that lets play_sfx() be called from the game while the device thread
// owns the mixer.
#include "phx_test.h"
#include "phx/audio/command_queue.h"
#include "phx/audio/mixer.h"
#include "phx/memory/allocators.h"

using namespace phx;

namespace {
AudioMixer* fresh_mixer(uint8_t channels = 8) {
    static uint8_t pool[1 << 20]; static size_t off = 0;
    static ArenaAllocator a; a.init(pool + off, (1 << 20) - off);
    Caps c = caps(); c.audio_channels = channels;
    return AudioMixer::create(a, c, 44100).unwrap();
}
int16_t* const_clip(uint32_t frames, int16_t amp) {
    int16_t* b = new int16_t[frames]; for (uint32_t i = 0; i < frames; ++i) b[i] = amp; return b;
}
} // namespace

PHX_TEST(cmdq_play_sfx_drains_into_mixer) {
    AudioMixer* m = fresh_mixer();
    AudioCommand storage[8];
    AudioCommandQueue q; q.init(storage, 8);

    SoundView s{ const_clip(64, 2000), 64, 44100 };
    CHECK(q.play_sfx(s, 1.0f));
    CHECK(q.play_sfx(s, 1.0f));
    CHECK_EQ(q.pending(), 2u);
    CHECK_EQ(m->active_count(), 0u);          // nothing applied until the audio thread drains

    CHECK_EQ(q.drain(*m), 2u);
    CHECK_EQ(q.pending(), 0u);
    CHECK_EQ(m->active_count(), 2u);          // both SFX now live on the mixer

    int16_t out[16 * 2]; m->mix(out, 16);
    CHECK(out[0] != 0);                       // and they actually mix to output
}

PHX_TEST(cmdq_music_and_volume_and_stop) {
    AudioMixer* m = fresh_mixer();
    AudioCommand storage[8];
    AudioCommandQueue q; q.init(storage, 8);
    SoundView music{ const_clip(128, 1000), 128, 44100 };

    q.play_music(music, 1.0f, true);
    q.set_music_volume(0.5f);
    q.drain(*m);
    int16_t out[8 * 2]; m->mix(out, 8);
    CHECK(out[0] == 500);                      // 1000 * 0.5 music volume

    q.stop_music();
    q.drain(*m);
    int16_t out2[8 * 2]; m->mix(out2, 8);
    CHECK(out2[0] == 0);                       // music stopped -> silence
}

PHX_TEST(cmdq_stop_all) {
    AudioMixer* m = fresh_mixer();
    AudioCommand storage[8];
    AudioCommandQueue q; q.init(storage, 8);
    SoundView s{ const_clip(64, 1500), 64, 44100 };
    q.play_sfx(s); q.play_sfx(s); q.drain(*m);
    CHECK_EQ(m->active_count(), 2u);
    q.stop_all(); q.drain(*m);
    CHECK_EQ(m->active_count(), 0u);
}

PHX_TEST(cmdq_reports_full_without_overrun) {
    AudioMixer* m = fresh_mixer();
    AudioCommand storage[4];                   // capacity 4
    AudioCommandQueue q; q.init(storage, 4);
    SoundView s{ const_clip(8, 100), 8, 44100 };
    CHECK(q.play_sfx(s)); CHECK(q.play_sfx(s)); CHECK(q.play_sfx(s)); CHECK(q.play_sfx(s));
    CHECK(!q.play_sfx(s));                      // 5th: queue full -> rejected, not corrupted
    CHECK_EQ(q.pending(), 4u);
    CHECK_EQ(q.drain(*m), 4u);                  // exactly the 4 that were accepted
    CHECK(q.play_sfx(s));                       // space again after draining
}
