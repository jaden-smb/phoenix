// phx/audio/mixer.h — a small software mixer: one SoA voice loop for every target, voice
// count scaling with caps.audio_channels. It mixes 16-bit PCM voices (per-voice gain + pan +
// nearest-sample resampling) into a caller-supplied interleaved-stereo buffer — so it is a
// PURE function with no audio device, fully testable headlessly and identical on both scalar
// tiers (the whole pipeline is integer, never `scalar`). See docs/10-gameplay-systems.md §2.
//
// The platform's audio callback calls mix() to fill the device buffer; tests call it directly
// and assert on the samples. Depends only on core + memory (no platform dependency).
#ifndef PHX_AUDIO_MIXER_H
#define PHX_AUDIO_MIXER_H

#include "phx/core/types.h"
#include "phx/core/caps.h"
#include "phx/memory/allocators.h"

namespace phx {

class AudioStream;   // phx/audio/stream.h — streamed music source (drained on the music bus)

// A decoded 16-bit signed PCM clip, MONO (the mixer pans it to stereo). For short SFX this
// points straight at a baked bundle blob (zero-copy); music streams the same shape per chunk.
struct SoundView {
    const int16_t* samples = nullptr;
    uint32_t       frames  = 0;
    uint32_t       rate    = 44100;     // source sample rate (resampled to the output rate)
};

// Generation-guarded handle: stop() on a stale id (its slot was recycled) is a safe no-op.
using VoiceId = uint32_t;
constexpr VoiceId kNoVoice = 0xFFFFFFFFu;

class AudioMixer {
public:
    static Result<AudioMixer*> create(ArenaAllocator&, const Caps&, uint32_t out_rate = 44100);

    VoiceId play_sfx(const SoundView&, float vol = 1.0f, float pan = 0.0f, bool loop = false);
    void    stop(VoiceId);
    bool    is_active(VoiceId) const;

    void    play_music(const SoundView&, float vol = 1.0f, bool loop = true);
    // Stream music from an AudioStream instead of a resident clip: the music bus drains the
    // stream's ring each mix() (the caller pump()s it off the audio path). Overrides any
    // resident music; honours set_music_volume(). Pass nullptr to detach. The stream must
    // outlive the attachment.
    void    play_music_stream(AudioStream*);
    void    stop_music();                  // silence the music bus (resident clip or stream)
    void    set_music_volume(float);

    void    stop_all();
    uint32_t active_count() const;

    // Mix `frames` STEREO frames into `out` (interleaved L,R = 2*frames int16 samples).
    // Additive over all active voices, then saturated to int16. The buffer is fully written
    // (silence where no voice plays). Safe to call with frames == 0.
    void mix(int16_t* out, uint32_t frames);

private:
    AudioMixer() = default;
    friend class ArenaAllocator;

    static constexpr uint32_t kMusicVoice = 0;     // slot 0 is reserved for music
    static constexpr uint32_t kBlock      = 512;   // mix accumulation block (frames)

    int32_t  find_free() const;                    // a free SFX slot (1..max-1), or -1
    bool     decode(VoiceId, uint32_t& idx) const; // validate handle -> slot index

    // SoA voice state (all arena-allocated at create; nothing allocates at runtime).
    uint8_t*        active_ = nullptr;
    uint8_t*        loop_   = nullptr;
    uint8_t*        gen_    = nullptr;
    const int16_t** data_   = nullptr;
    uint32_t*       len_    = nullptr;             // frames in the clip
    uint64_t*       pos_    = nullptr;             // Q16 sample cursor
    uint32_t*       step_   = nullptr;             // Q16 increment = src_rate/out_rate
    int32_t*        gl_     = nullptr;             // Q8 left gain
    int32_t*        gr_     = nullptr;             // Q8 right gain

    int32_t*  acc_      = nullptr;                 // kBlock*2 int32 mix accumulator
    uint32_t  max_      = 0;
    uint32_t  out_rate_ = 44100;
    int32_t   music_vol_ = 256;                    // Q8
    AudioStream* music_stream_ = nullptr;          // streamed music source (overrides slot 0)
};

} // namespace phx
#endif // PHX_AUDIO_MIXER_H
