// tests/test_stream.cpp — audio streaming (docs/06 §5): the SPSC RingBuffer mechanics and the
// AudioStream producer (resample source -> ring), including looping, non-loop end + drain,
// half-rate resampling, and consumer underrun. Plus a mixer integration: stream music through
// the music bus and verify the drained samples land in the mix. All-integer -> tier-identical.
#include "phx_test.h"
#include "phx/audio/stream.h"
#include "phx/audio/mixer.h"
#include "phx/memory/allocators.h"

#include <cstdlib>

using namespace phx;

namespace {
ArenaAllocator* fresh_arena(size_t bytes = 1 << 20) {
    static const size_t kPool = 32 << 20;        // bound-checked: outgrowing the pool must
    static uint8_t* pool = new uint8_t[kPool];   // fail loudly, not hand out memory past it
    static size_t   off  = 0;
    if (off + bytes > kPool) { std::abort(); }
    ArenaAllocator* a = new ArenaAllocator();
    a->init(pool + off, bytes);
    off += bytes;
    return a;
}
} // namespace

PHX_TEST(ring_write_read_wraparound) {
    int16_t storage[8];
    RingBuffer r; r.init(storage, 8);
    CHECK_EQ(r.capacity(), 8u);
    CHECK_EQ(r.free_space(), 8u);

    int16_t in[5] = { 1, 2, 3, 4, 5 };
    CHECK_EQ(r.write(in, 5), 5u);
    CHECK_EQ(r.available(), 5u);
    CHECK_EQ(r.free_space(), 3u);

    int16_t out[8] = {};
    CHECK_EQ(r.read(out, 3), 3u);             // pull 1,2,3
    CHECK(out[0] == 1 && out[1] == 2 && out[2] == 3);
    CHECK_EQ(r.available(), 2u);

    // free is now 6; write 6 more -> indices wrap past the end of the backing array
    int16_t in2[6] = { 6, 7, 8, 9, 10, 11 };
    CHECK_EQ(r.write(in2, 6), 6u);
    CHECK_EQ(r.available(), 8u);
    int16_t out2[8] = {};
    CHECK_EQ(r.read(out2, 8), 8u);            // 4,5 then 6..11 in order across the wrap
    CHECK(out2[0] == 4 && out2[1] == 5 && out2[2] == 6 && out2[7] == 11);
}

PHX_TEST(ring_rejects_overfill_and_underread) {
    int16_t storage[4];
    RingBuffer r; r.init(storage, 4);
    int16_t in[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    CHECK_EQ(r.write(in, 8), 4u);             // only 4 fit
    CHECK_EQ(r.free_space(), 0u);
    int16_t out[8] = {};
    CHECK_EQ(r.read(out, 8), 4u);             // only 4 available
    CHECK(out[0] == 1 && out[3] == 4);
}

PHX_TEST(stream_pump_fills_and_reproduces_source) {
    int16_t src[4] = { 10, 20, 30, 40 };
    int16_t ring[16];
    AudioStream s; s.init(ring, 16, /*out_rate*/44100);
    s.open(SoundView{ src, 4, 44100 }, /*loop*/false);   // src_rate == out_rate -> 1:1
    uint32_t made = s.pump();
    CHECK_EQ(made, 4u);
    CHECK_EQ(s.buffered(), 4u);
    int16_t out[4] = {};
    CHECK_EQ(s.read(out, 4), 4u);
    CHECK(out[0] == 10 && out[1] == 20 && out[2] == 30 && out[3] == 40);
}

PHX_TEST(stream_loops_to_fill_ring) {
    int16_t src[2] = { 7, 9 };
    int16_t ring[8];
    AudioStream s; s.init(ring, 8, 44100);
    s.open(SoundView{ src, 2, 44100 }, /*loop*/true);
    CHECK_EQ(s.pump(), 8u);                   // fills the whole ring by looping the 2-sample src
    int16_t out[8] = {};
    s.read(out, 8);
    bool pattern = true;
    for (int i = 0; i < 8; ++i) pattern &= (out[i] == (i % 2 == 0 ? 7 : 9));
    CHECK(pattern);
    CHECK(s.active());                        // a looping stream is always active
}

PHX_TEST(stream_nonloop_ends_then_drains) {
    int16_t src[3] = { 1, 2, 3 };
    int16_t ring[16];
    AudioStream s; s.init(ring, 16, 44100);
    s.open(SoundView{ src, 3, 44100 }, /*loop*/false);
    CHECK_EQ(s.pump(), 3u);
    CHECK_EQ(s.pump(), 0u);                   // source exhausted -> nothing more produced
    CHECK(s.active());                        // still active: 3 samples remain buffered
    int16_t out[3] = {};
    CHECK_EQ(s.read(out, 3), 3u);
    CHECK(out[0] == 1 && out[2] == 3);
    CHECK(!s.active());                       // drained + done -> inactive
}

PHX_TEST(stream_half_rate_resamples_up) {
    int16_t src[3] = { 100, 200, 300 };
    int16_t ring[16];
    AudioStream s; s.init(ring, 16, /*out_rate*/44100);
    s.open(SoundView{ src, 3, /*src_rate*/22050 }, /*loop*/false);  // step = 0.5 -> each src ~2x
    uint32_t made = s.pump();
    CHECK_EQ(made, 6u);                       // 3 source frames at half rate -> 6 output frames
    int16_t out[6] = {};
    s.read(out, 6);
    CHECK(out[0] == 100 && out[1] == 100);    // nearest-sample duplication
    CHECK(out[2] == 200 && out[3] == 200);
    CHECK(out[4] == 300 && out[5] == 300);
}

PHX_TEST(stream_read_underrun_returns_partial) {
    int16_t src[2] = { 5, 6 };
    int16_t ring[8];
    AudioStream s; s.init(ring, 8, 44100);
    s.open(SoundView{ src, 2, 44100 }, false);
    s.pump();
    int16_t out[8] = {};
    CHECK_EQ(s.read(out, 8), 2u);             // only 2 buffered -> partial, no garbage
}

PHX_TEST(mixer_streams_music_through_the_bus) {
    Caps c = caps(); c.audio_channels = 4;
    AudioMixer* m = AudioMixer::create(*fresh_arena(), c, 44100).unwrap();

    static int16_t src[64];
    for (int i = 0; i < 64; ++i) src[i] = 1000;
    static int16_t ring[256];
    static AudioStream s;
    s.init(ring, 256, 44100);
    s.open(SoundView{ src, 64, 44100 }, /*loop*/true);
    s.pump();                                  // resource tick fills the ring

    m->play_music_stream(&s);
    int16_t out[32 * 2] = {};
    m->mix(out, 32);                           // music bus drains the ring 1:1 at unity volume
    CHECK(out[0] == 1000 && out[1] == 1000);   // L and R carry the streamed sample
    CHECK(out[62] == 1000 && out[63] == 1000);

    m->set_music_volume(0.5f);                 // halve the music bus
    s.pump();
    int16_t out2[8 * 2] = {};
    m->mix(out2, 8);
    CHECK(out2[0] == 500 && out2[1] == 500);   // 1000 * 0.5
}

PHX_TEST(mixer_music_underrun_is_silent_not_garbage) {
    Caps c = caps(); c.audio_channels = 4;
    AudioMixer* m = AudioMixer::create(*fresh_arena(), c, 44100).unwrap();
    static int16_t src[4] = { 2000, 2000, 2000, 2000 };
    static int16_t ring[16];
    static AudioStream s;
    s.init(ring, 16, 44100);
    s.open(SoundView{ src, 4, 44100 }, /*loop*/false);
    s.pump();                                  // only 4 samples available
    m->play_music_stream(&s);
    int16_t out[16 * 2] = {};
    m->mix(out, 16);                           // ask for 16 -> 4 real, 12 silent
    CHECK(out[0] == 2000 && out[6] == 2000);   // first 4 frames present
    CHECK(out[8] == 0 && out[30] == 0);        // the underrun region is clean silence
}
