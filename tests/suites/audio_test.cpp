// tests/audio_test.cpp — end-to-end audio path: bake a PCM clip into a `.phxp` blob with the
// offline writer, mount it through the platform seam (zero-copy), wrap the blob as a SoundView,
// and mix it — proving content flows bake -> mount -> view -> mixer just like the render path.
#include "phx/platform/platform.h"
#include "phx/resource/cache.h"
#include "phx/audio/mixer.h"
#include "phx/audio/stream.h"
#include "phx/core/caps.h"
#include "bundle_writer.h"
#include "wav.h"

#include <cstdio>
#include <vector>

using namespace phx;

namespace {
int g_checks = 0, g_fail = 0;
void check(bool ok, const char* what) { ++g_checks; if (!ok) { ++g_fail; std::printf("    FAIL %s\n", what); } }

// Minimal in-memory 16-bit mono WAV (RIFF) builder, to exercise the WAV decode -> Sound path.
std::vector<uint8_t> make_wav_mono16(uint32_t rate, const int16_t* s, uint32_t frames) {
    auto put16 = [](std::vector<uint8_t>& v, uint16_t x){ v.push_back(uint8_t(x)); v.push_back(uint8_t(x>>8)); };
    auto put32 = [](std::vector<uint8_t>& v, uint32_t x){ for (int i=0;i<4;++i) v.push_back(uint8_t(x>>(8*i))); };
    auto tag   = [](std::vector<uint8_t>& v, const char* t){ for (int i=0;i<4;++i) v.push_back(uint8_t(t[i])); };
    std::vector<uint8_t> w;
    const uint32_t dataSize = frames * 2;
    tag(w,"RIFF"); put32(w,36+dataSize); tag(w,"WAVE");
    tag(w,"fmt "); put32(w,16); put16(w,1); put16(w,1); put32(w,rate); put32(w,rate*2); put16(w,2); put16(w,16);
    tag(w,"data"); put32(w,dataSize);
    for (uint32_t i=0;i<frames;++i) put16(w, uint16_t(s[i]));
    return w;
}

void bake_bundle(const char* path) {
    phxtool::BundleWriter w(/*tier*/2);
    int16_t clip[64];
    for (int i = 0; i < 64; ++i) clip[i] = 3000;            // constant-amplitude mono PCM
    w.add_blob("blip", clip, sizeof(clip));

    // A WAV decoded by the pipeline, baked as a Sound asset (the .wav -> Sound path).
    int16_t tone[16]; for (int i = 0; i < 16; ++i) tone[i] = 1500;
    std::vector<uint8_t> wav = make_wav_mono16(22050, tone, 16);
    std::vector<int16_t> mono; uint32_t rate = 0;
    if (!phxtool::wav_decode(wav.data(), wav.size(), mono, rate))
        std::printf("    FAIL wav_decode in bake\n");
    w.add_sound("tone", mono.data(), uint32_t(mono.size()), rate);
    // also drop the WAV so the phxpack CLI can bake the .wav path in `make phxpack`
    if (FILE* wf = std::fopen("build/tone.wav", "wb")) { std::fwrite(wav.data(), 1, wav.size(), wf); std::fclose(wf); }

    if (!w.write(path)) std::printf("    FAIL could not write %s\n", path);
}
} // namespace

int main() {
    const char* bundle = "build/test_audio.phxp";
    bake_bundle(bundle);

    const phx_platform* plat = phx_platform_get();
    phx_platform_desc desc{}; desc.title = "audio_test"; desc.width = 16; desc.height = 16;
    if (plat->init(&desc) != 0) { std::printf("platform init failed\n"); return 1; }

    static uint8_t arena_buf[4 << 20];
    ArenaAllocator arena; arena.init(arena_buf, sizeof(arena_buf));

    auto cr = ResourceCache::create(arena);
    check(cr.ok(), "ResourceCache::create");
    ResourceCache* cache = cr.unwrap();
    check(cache->mount(plat, bundle) == Status::Ok, "mount bundle");

    auto bv = cache->blob("blip"_hash);
    check(bv.ok(), "blob('blip') found");
    check(!cache->blob("nope"_hash).ok(), "missing blob -> NotFound");

    Caps c = caps(); c.audio_channels = 8;
    auto mr = AudioMixer::create(arena, c, 44100);
    check(mr.ok(), "AudioMixer::create");
    AudioMixer* mix = mr.unwrap();

    // zero-copy: the SoundView points straight at the mmap'd bundle bytes
    BlobView b = bv.unwrap();
    SoundView snd{ static_cast<const int16_t*>(b.data), b.size / 2, 44100 };
    check(snd.frames == 64, "soundview frames from blob size");

    mix->play_music(snd, 1.0f, /*loop*/true);
    int16_t out[32 * 2];
    mix->mix(out, 32);
    check(out[0] == 3000 && out[1] == 3000, "mixed PCM from the bundle (L+R)");
    check(mix->active_count() == 1, "one voice active");

    // STREAM the same bundle blob: the stream's source view points straight at the mmap'd
    // bytes, pump() (the "resource tick") resamples them into the ring, and the mixer's music
    // bus drains it — content path bake -> mount -> stream -> mix, no resident copy.
    int16_t* ring = arena.alloc_array<int16_t>(256);
    AudioStream* stream = arena.make<AudioStream>();
    stream->init(ring, 256, 44100);
    stream->open(snd, /*loop*/true);
    check(stream->pump() == 256, "pump fills the ring from the mounted blob");
    mix->play_music_stream(stream);
    int16_t sout[32 * 2];
    mix->mix(sout, 32);
    check(sout[0] == 3000 && sout[1] == 3000, "streamed PCM reached the mix from the bundle");

    // The Sound asset baked from a decoded WAV: mount -> SoundDataView -> SoundView -> mix.
    auto tone_r = cache->sound("tone"_hash);
    check(tone_r.ok(), "sound('tone') found");
    SoundDataView sdv = tone_r.unwrap();
    check(sdv.frames == 16 && sdv.rate == 22050, "WAV decoded + baked as a Sound asset");
    mix->stop_all();
    SoundView tone{ sdv.samples, sdv.frames, sdv.rate };
    mix->play_sfx(tone, 1.0f, 0.0f, false);
    int16_t wout[8 * 2];
    mix->mix(wout, 8);
    check(wout[0] == 1500 && wout[1] == 1500, "WAV-derived sound mixed (L+R)");

    plat->shutdown();
    std::printf("\naudio_test: %d checks, %d failures\n", g_checks, g_fail);
    std::printf(g_fail == 0 ? "AUDIO PASS\n\n" : "AUDIO FAIL\n\n");
    return g_fail == 0 ? 0 : 1;
}
