// phx/audio/command_queue.h — the thread seam between the game and a real audio device. With a
// device callback (e.g. SDL), the audio thread is the ONLY thread allowed to touch the mixer;
// the game thread must not call play_sfx/mix concurrently. So the game pushes intents into this
// single-producer/single-consumer lock-free queue, and the audio callback drain()s them into the
// mixer right before it mixes — making all mixer mutation single-threaded by construction.
//
// Same SPSC discipline as RingBuffer (std::atomic acquire/release, power-of-two capacity): one
// producer (game), one consumer (audio callback). Headlessly testable single-threaded, and
// correct across two threads. Commands are fire-and-forget (no cross-thread return value).
#ifndef PHX_AUDIO_COMMAND_QUEUE_H
#define PHX_AUDIO_COMMAND_QUEUE_H

#include "phx/audio/mixer.h"

#include <atomic>

namespace phx {

struct AudioCommand {
    enum Type : uint8_t { PlaySfx, PlayMusic, StopMusic, StopAll, MusicVolume } type = StopAll;
    SoundView snd{};
    float     vol  = 1.0f;
    float     pan  = 0.0f;
    bool      loop = false;
};

// Caller supplies the backing storage (no allocation); cap MUST be a power of two.
class AudioCommandQueue {
public:
    void init(AudioCommand* storage, uint32_t cap_pow2) {
        buf_ = storage; cap_ = cap_pow2; mask_ = cap_pow2 - 1;
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

    // --- producer (game thread) ---
    bool push(const AudioCommand& c) {
        const uint32_t h = head_.load(std::memory_order_relaxed);
        const uint32_t t = tail_.load(std::memory_order_acquire);
        if (h - t >= cap_) return false;                 // full -> drop (never block the game)
        buf_[h & mask_] = c;
        head_.store(h + 1, std::memory_order_release);
        return true;
    }
    bool play_sfx(const SoundView& s, float vol = 1.0f, float pan = 0.0f, bool loop = false) {
        return push(AudioCommand{ AudioCommand::PlaySfx, s, vol, pan, loop });
    }
    bool play_music(const SoundView& s, float vol = 1.0f, bool loop = true) {
        return push(AudioCommand{ AudioCommand::PlayMusic, s, vol, 0.0f, loop });
    }
    bool stop_music()        { return push(AudioCommand{ AudioCommand::StopMusic }); }
    bool stop_all()          { return push(AudioCommand{ AudioCommand::StopAll }); }
    bool set_music_volume(float v) { AudioCommand c{ AudioCommand::MusicVolume }; c.vol = v; return push(c); }

    // --- consumer (audio thread) --- apply every queued command to the mixer, in order.
    uint32_t drain(AudioMixer& m) {
        uint32_t t = tail_.load(std::memory_order_relaxed);
        const uint32_t h = head_.load(std::memory_order_acquire);
        uint32_t n = 0;
        for (; t != h; ++t, ++n) {
            const AudioCommand& c = buf_[t & mask_];
            switch (c.type) {
                case AudioCommand::PlaySfx:     m.play_sfx(c.snd, c.vol, c.pan, c.loop); break;
                case AudioCommand::PlayMusic:   m.play_music(c.snd, c.vol, c.loop);      break;
                case AudioCommand::StopMusic:   m.stop_music();                          break;
                case AudioCommand::StopAll:     m.stop_all();                            break;
                case AudioCommand::MusicVolume: m.set_music_volume(c.vol);               break;
            }
        }
        tail_.store(t, std::memory_order_release);
        return n;
    }

    uint32_t pending() const {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_relaxed);
    }

private:
    AudioCommand* buf_  = nullptr;
    uint32_t      cap_  = 0;
    uint32_t      mask_ = 0;
    std::atomic<uint32_t> head_{0};   // producer
    std::atomic<uint32_t> tail_{0};   // consumer
};

} // namespace phx
#endif // PHX_AUDIO_COMMAND_QUEUE_H
