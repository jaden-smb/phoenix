// phx/audio/mixer.cpp — the SoA software mixer (docs/10 §2). All-integer pipeline: per-voice
// Q16 resampling cursor + Q8 stereo gains, accumulated in int32 and saturated to int16. No
// `scalar`, so fixed/float builds produce byte-identical audio.
#include "phx/audio/mixer.h"
#include "phx/audio/stream.h"

namespace phx {
namespace {

inline int32_t clampf_q8(float v) {                 // float gain in [0,1] -> Q8 int
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return int32_t(v * 256.0f + 0.5f);
}
inline int16_t sat16(int32_t v) {
    return v > 32767 ? int16_t(32767) : (v < -32768 ? int16_t(-32768) : int16_t(v));
}

} // namespace

Result<AudioMixer*> AudioMixer::create(ArenaAllocator& a, const Caps& caps, uint32_t out_rate) {
    auto* m = a.make<AudioMixer>();
    if (!m) return Result<AudioMixer*>::fail(Status::OutOfMemory);
    m->max_      = caps.audio_channels ? caps.audio_channels : 1;
    m->out_rate_ = out_rate ? out_rate : 44100;
    m->music_vol_ = 256;

    m->active_ = a.alloc_array<uint8_t>(m->max_);
    m->loop_   = a.alloc_array<uint8_t>(m->max_);
    m->gen_    = a.alloc_array<uint8_t>(m->max_);
    m->data_   = a.alloc_array<const int16_t*>(m->max_);
    m->len_    = a.alloc_array<uint32_t>(m->max_);
    m->pos_    = a.alloc_array<uint64_t>(m->max_);
    m->step_   = a.alloc_array<uint32_t>(m->max_);
    m->gl_     = a.alloc_array<int32_t>(m->max_);
    m->gr_     = a.alloc_array<int32_t>(m->max_);
    m->acc_    = a.alloc_array<int32_t>(kBlock * 2);
    if (!m->active_ || !m->acc_) return Result<AudioMixer*>::fail(Status::OutOfMemory);

    for (uint32_t i = 0; i < m->max_; ++i) { m->active_[i] = 0; m->gen_[i] = 0; }
    return Result<AudioMixer*>::good(m);
}

int32_t AudioMixer::find_free() const {
    for (uint32_t i = 1; i < max_; ++i) if (!active_[i]) return int32_t(i);   // 0 reserved = music
    return -1;
}

bool AudioMixer::decode(VoiceId id, uint32_t& idx) const {
    if (id == kNoVoice) return false;
    uint32_t i = id >> 8;
    uint8_t  g = uint8_t(id & 0xFF);
    if (i >= max_ || !active_[i] || gen_[i] != g) return false;
    idx = i; return true;
}

// Configure slot `i` to play `s` with the given Q8 gains; returns its handle.
static VoiceId arm(uint8_t* active, uint8_t* loop, uint8_t* gen, const int16_t** data,
                   uint32_t* len, uint64_t* pos, uint32_t* step, int32_t* gl, int32_t* gr,
                   uint32_t i, const SoundView& s, int32_t lq, int32_t rq, bool lp,
                   uint32_t out_rate) {
    active[i] = 1; loop[i] = lp ? 1 : 0;
    data[i] = s.samples; len[i] = s.frames;
    pos[i] = 0;
    step[i] = uint32_t((uint64_t(s.rate ? s.rate : out_rate) << 16) / (out_rate ? out_rate : 44100));
    gl[i] = lq; gr[i] = rq;
    return (i << 8) | gen[i];
}

VoiceId AudioMixer::play_sfx(const SoundView& s, float vol, float pan, bool loop) {
    if (!s.samples || s.frames == 0) return kNoVoice;
    int32_t slot = find_free();
    if (slot < 0) return kNoVoice;                  // all voices busy (honest ceiling)
    if (pan < -1.0f) pan = -1.0f;
    if (pan >  1.0f) pan =  1.0f;
    int32_t vq = clampf_q8(vol);
    int32_t lq = pan <= 0.0f ? vq : (vq * clampf_q8(1.0f - pan)) >> 8;
    int32_t rq = pan >= 0.0f ? vq : (vq * clampf_q8(1.0f + pan)) >> 8;
    return arm(active_, loop_, gen_, data_, len_, pos_, step_, gl_, gr_,
               uint32_t(slot), s, lq, rq, loop, out_rate_);
}

void AudioMixer::play_music(const SoundView& s, float vol, bool loop) {
    if (!s.samples || s.frames == 0) return;
    music_stream_ = nullptr;                        // resident music overrides any stream
    int32_t vq = clampf_q8(vol);
    arm(active_, loop_, gen_, data_, len_, pos_, step_, gl_, gr_,
        kMusicVoice, s, vq, vq, loop, out_rate_);
}

void AudioMixer::play_music_stream(AudioStream* s) {
    music_stream_ = s;                              // streamed music overrides the resident slot
    if (active_[kMusicVoice]) { active_[kMusicVoice] = 0; ++gen_[kMusicVoice]; }
}

void AudioMixer::stop_music() {
    if (active_[kMusicVoice]) { active_[kMusicVoice] = 0; ++gen_[kMusicVoice]; }
    music_stream_ = nullptr;
}

void AudioMixer::set_music_volume(float v) { music_vol_ = clampf_q8(v); }

void AudioMixer::stop(VoiceId id) {
    uint32_t i;
    if (decode(id, i)) { active_[i] = 0; ++gen_[i]; }   // bump gen so the handle goes stale
}
bool AudioMixer::is_active(VoiceId id) const { uint32_t i; return decode(id, i); }

void AudioMixer::stop_all() {
    for (uint32_t i = 0; i < max_; ++i) if (active_[i]) { active_[i] = 0; ++gen_[i]; }
    music_stream_ = nullptr;
}
uint32_t AudioMixer::active_count() const {
    uint32_t n = 0; for (uint32_t i = 0; i < max_; ++i) n += active_[i]; return n;
}

void AudioMixer::mix(int16_t* out, uint32_t frames) {
    uint32_t done = 0;
    while (done < frames) {
        const uint32_t n = (frames - done) < kBlock ? (frames - done) : kBlock;
        for (uint32_t k = 0; k < n * 2; ++k) acc_[k] = 0;

        for (uint32_t v = 0; v < max_; ++v) {
            if (!active_[v]) continue;
            const int16_t* d = data_[v];
            const uint32_t len = len_[v];
            uint64_t pos = pos_[v];
            const uint32_t st = step_[v];
            int32_t gl = gl_[v], gr = gr_[v];
            if (v == kMusicVoice) { gl = (gl * music_vol_) >> 8; gr = (gr * music_vol_) >> 8; }

            for (uint32_t f = 0; f < n; ++f) {
                uint32_t si = uint32_t(pos >> 16);
                if (si >= len) {
                    if (loop_[v]) { pos %= (uint64_t(len) << 16); si = uint32_t(pos >> 16); }
                    else { active_[v] = 0; ++gen_[v]; break; }
                }
                int32_t s = d[si];
                acc_[f * 2 + 0] += (s * gl) >> 8;
                acc_[f * 2 + 1] += (s * gr) >> 8;
                pos += st;
            }
            pos_[v] = pos;
        }

        // Streamed music: drain the ring 1:1 (it is already at out_rate) and add to the bus,
        // scaled by music_vol_. An underrun (got < n) simply contributes silence for the gap.
        if (music_stream_) {
            int16_t mono[kBlock];
            uint32_t got = music_stream_->read(mono, n);
            for (uint32_t f = 0; f < got; ++f) {
                int32_t s = (int32_t(mono[f]) * music_vol_) >> 8;
                acc_[f * 2 + 0] += s;
                acc_[f * 2 + 1] += s;
            }
        }

        int16_t* o = out + done * 2;
        for (uint32_t k = 0; k < n * 2; ++k) o[k] = sat16(acc_[k]);
        done += n;
    }
}

} // namespace phx
