// tests/test_audio.cpp — software mixer: silence, unity passthrough, volume, pan, summing,
// non-loop end, loop wrap, resampling, the voice ceiling, generation-guarded stop, and music
// volume. All-integer pipeline -> byte-identical on both scalar tiers.
#include "phx_test.h"
#include "phx/audio/mixer.h"
#include "phx/memory/allocators.h"

#include <cstdlib>

using namespace phx;

namespace {
ArenaAllocator* fresh_arena() {
    static const size_t kPool = 32 << 20;        // bound-checked: outgrowing the pool must
    static uint8_t* pool = new uint8_t[kPool];   // fail loudly, not hand out memory past it
    static size_t   off  = 0;                    // (ASan caught exactly that at 8 MB)
    if (off + (1 << 20) > kPool) { std::abort(); }
    ArenaAllocator* a = new ArenaAllocator();
    a->init(pool + off, 1 << 20);
    off += (1 << 20);
    return a;
}
AudioMixer* fresh_mixer(uint8_t channels = 8, uint32_t out_rate = 44100) {
    Caps c = caps(); c.audio_channels = channels;
    return AudioMixer::create(*fresh_arena(), c, out_rate).unwrap();
}
// A constant-amplitude mono clip.
int16_t* const_clip(uint32_t frames, int16_t amp) {
    int16_t* b = new int16_t[frames];
    for (uint32_t i = 0; i < frames; ++i) b[i] = amp;
    return b;
}
} // namespace

PHX_TEST(audio_silence_when_idle) {
    AudioMixer* m = fresh_mixer();
    int16_t out[64]; for (int i = 0; i < 64; ++i) out[i] = 123;
    m->mix(out, 32);
    bool all_zero = true; for (int i = 0; i < 64; ++i) all_zero &= (out[i] == 0);
    CHECK(all_zero);
    CHECK_EQ(m->active_count(), 0u);
}

PHX_TEST(audio_unity_passthrough_stereo) {
    AudioMixer* m = fresh_mixer();
    SoundView s{ const_clip(16, 1000), 16, 44100 };   // same rate -> 1:1
    m->play_sfx(s, 1.0f, 0.0f, false);
    int16_t out[20 * 2];
    m->mix(out, 16);
    CHECK_EQ(out[0], 1000);    // L
    CHECK_EQ(out[1], 1000);    // R
    CHECK_EQ(out[30], 1000);   // frame 15 L
    CHECK_EQ(m->active_count(), 1u);
}

PHX_TEST(audio_volume_scales) {
    AudioMixer* m = fresh_mixer();
    SoundView s{ const_clip(8, 1000), 8, 44100 };
    m->play_sfx(s, 0.5f, 0.0f, false);
    int16_t out[8 * 2];
    m->mix(out, 8);
    CHECK_EQ(out[0], 500);     // 1000 * 128/256
    CHECK_EQ(out[1], 500);
}

PHX_TEST(audio_pan_hard_right) {
    AudioMixer* m = fresh_mixer();
    SoundView s{ const_clip(8, 1000), 8, 44100 };
    m->play_sfx(s, 1.0f, 1.0f, false);   // full right
    int16_t out[8 * 2];
    m->mix(out, 8);
    CHECK_EQ(out[0], 0);       // L silent
    CHECK_EQ(out[1], 1000);    // R full
}

PHX_TEST(audio_voices_sum) {
    AudioMixer* m = fresh_mixer();
    SoundView a{ const_clip(8, 1000), 8, 44100 };
    SoundView b{ const_clip(8,  500), 8, 44100 };
    m->play_sfx(a); m->play_sfx(b);
    int16_t out[8 * 2];
    m->mix(out, 8);
    CHECK_EQ(out[0], 1500);
    CHECK_EQ(m->active_count(), 2u);
}

PHX_TEST(audio_nonloop_ends) {
    AudioMixer* m = fresh_mixer();
    SoundView s{ const_clip(4, 1000), 4, 44100 };
    m->play_sfx(s, 1.0f, 0.0f, false);
    int16_t out[8 * 2];
    m->mix(out, 8);
    CHECK_EQ(out[0], 1000);          // plays for its 4 frames
    CHECK_EQ(out[3 * 2], 1000);
    CHECK_EQ(out[4 * 2], 0);         // then silent
    CHECK_EQ(out[7 * 2], 0);
    CHECK_EQ(m->active_count(), 0u); // voice freed
}

PHX_TEST(audio_loop_wraps) {
    AudioMixer* m = fresh_mixer();
    SoundView s{ const_clip(4, 1000), 4, 44100 };
    m->play_sfx(s, 1.0f, 0.0f, true);
    int16_t out[16 * 2];
    m->mix(out, 16);
    CHECK_EQ(out[0], 1000);
    CHECK_EQ(out[15 * 2], 1000);     // still playing after several wraps
    CHECK_EQ(m->active_count(), 1u);
}

PHX_TEST(audio_resample_half_rate) {
    AudioMixer* m = fresh_mixer();
    int16_t* clip = new int16_t[4]{ 10, 20, 30, 40 };
    SoundView s{ clip, 4, 22050 };   // half the 44100 output -> each sample twice
    m->play_sfx(s, 1.0f, 0.0f, false);
    int16_t out[8 * 2];
    m->mix(out, 8);
    CHECK_EQ(out[0 * 2], 10); CHECK_EQ(out[1 * 2], 10);
    CHECK_EQ(out[2 * 2], 20); CHECK_EQ(out[3 * 2], 20);
    CHECK_EQ(out[4 * 2], 30); CHECK_EQ(out[5 * 2], 30);
}

PHX_TEST(audio_voice_ceiling) {
    AudioMixer* m = fresh_mixer(/*channels*/3);     // slot 0 = music, so 2 sfx slots
    SoundView s{ const_clip(64, 100), 64, 44100 };
    VoiceId a = m->play_sfx(s, 1, 0, true);
    VoiceId b = m->play_sfx(s, 1, 0, true);
    VoiceId c = m->play_sfx(s, 1, 0, true);          // no slot left
    CHECK(a != kNoVoice);
    CHECK(b != kNoVoice);
    CHECK_EQ(c, kNoVoice);
}

PHX_TEST(audio_stop_and_generation_guard) {
    AudioMixer* m = fresh_mixer();
    SoundView s{ const_clip(64, 100), 64, 44100 };
    VoiceId v1 = m->play_sfx(s, 1, 0, true);
    CHECK(m->is_active(v1));
    m->stop(v1);
    CHECK(!m->is_active(v1));
    VoiceId v2 = m->play_sfx(s, 1, 0, true);   // recycles the same slot
    CHECK(v1 != v2);
    m->stop(v1);                                // stale handle -> no-op
    CHECK(m->is_active(v2));                    // v2 unaffected
}

PHX_TEST(audio_music_volume) {
    AudioMixer* m = fresh_mixer();
    SoundView s{ const_clip(8, 1000), 8, 44100 };
    m->play_music(s, 1.0f, true);
    m->set_music_volume(0.5f);
    int16_t out[8 * 2];
    m->mix(out, 8);
    CHECK_EQ(out[0], 500);     // music scaled by 0.5
}
