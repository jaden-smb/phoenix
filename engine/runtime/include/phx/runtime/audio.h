// phx/runtime/audio.h — the game's sound output, owned by the App (App::audio()). A game says
// what to play; the engine owns the mixer, the lock-free intent queue and the platform's device,
// so the same game code sounds on every target with no platform call:
//
//     jump = to_sound(res->sound("jump"_hash).unwrap());      // on_start
//     app.audio().play(jump);                                  // on_fixed_update
//
// Nothing is allocated or started until the first play: the first call carves the mixer and the
// queue from the persistent arena and starts the platform's device (phx_platform::audio(); SDL's
// audio thread, a PSP thread, the GBA's DirectSound IRQ) at the device's rate. A game that never
// plays a sound pays nothing, and a game that runs its own mixer and device (examples/emberwing)
// is untouched: the two never meet. Where the platform has no device (the null backend, headless
// tests) or it fails to open, the App mixes one frame's worth per rendered frame itself, so
// voices advance deterministically and peak() reports what would have been heard.
//
// Thread model: play()/stop_*() only push intents (AudioCommandQueue, SPSC); the device's fill
// callback is the one place the mixer is touched. Sounds are fire-and-forget (no voice handles
// cross threads); stop_all() ends loops. Header-only on purpose: the mixer is only linked into
// games that play sound.
#ifndef PHX_RUNTIME_AUDIO_H
#define PHX_RUNTIME_AUDIO_H

#include "phx/audio/command_queue.h"
#include "phx/audio/mixer.h"
#include "phx/core/log.h"
#include "phx/memory/allocators.h"
#include "phx/platform/platform.h"
#include "phx/resource/cache.h"

#include <new>

namespace phx {

class App;

// A baked sound (ResourceCache::sound) as the mixer's clip: zero-copy, the bundle's samples.
inline SoundView to_sound(const SoundDataView& d) {
    return SoundView{ d.samples, d.frames, d.rate ? d.rate : 44100u };
}

class GameAudio {
public:
    static constexpr uint32_t kQueueCap     = 64;      // intents in flight (power of two)
    static constexpr uint32_t kHeadlessRate = 44100;   // output rate when there is no device

    // A sound effect: vol 0..1, pan -1 (left) .. +1 (right). False when it can't be queued (no
    // samples, audio unavailable, or the queue is full this frame).
    bool play(const SoundView& s, float vol = 1.0f, float pan = 0.0f, bool loop = false) {
        return s.samples && s.frames && ensure() && queue_.play_sfx(s, vol, pan, loop) && (++plays_, true);
    }
    // Music on its own bus (one track; a new one replaces it).
    bool play_music(const SoundView& s, float vol = 1.0f, bool loop = true) {
        return s.samples && s.frames && ensure() && queue_.play_music(s, vol, loop) && (++plays_, true);
    }
    bool stop_music()               { return mixer_ && queue_.stop_music(); }
    bool stop_all()                 { return mixer_ && queue_.stop_all(); }
    bool set_music_volume(float v)  { return ensure() && queue_.set_music_volume(v); }

    // The output rate: the platform device's (SDL/PSP 44100, GBA 18157), else kHeadlessRate.
    uint32_t rate() const { return dev_ && dev_->rate > 0 ? uint32_t(dev_->rate) : kHeadlessRate; }
    bool started()    const { return mixer_ != nullptr; }   // the mixer is up (until teardown)
    bool has_device() const { return device_on_; }          // a real device is sounding it
    // This run's sounds queued, and the loudest sample the App mixed without a device. Both
    // survive the App's teardown (so a test can read them after run()); reset at the next boot.
    uint32_t plays()  const { return plays_; }
    int32_t peak()    const { return peak_; }
    // Output frames mixed so far (by the device's callback, or headlessly): a device that
    // opened but never pulls samples shows up as 0 here.
    uint32_t frames_mixed() const { return frames_.load(std::memory_order_relaxed); }

private:
    friend class App;

    // App: at boot (nothing starts here) and at teardown.
    void attach(const phx_audio* dev, ArenaAllocator* arena, const Caps& caps, uint16_t sim_hz) {
        dev_ = dev; arena_ = arena; caps_ = caps; sim_hz_ = sim_hz ? sim_hz : 60;
        plays_ = 0; peak_ = 0;
        frames_.store(0, std::memory_order_relaxed);
    }
    void detach() {                 // after stop_ (the device is already silent)
        device_on_ = false; mixer_ = nullptr; arena_ = nullptr; dev_ = nullptr; failed_ = false;
        pump_ = nullptr; stop_ = nullptr;
    }

    bool ensure() {
        if (mixer_) return true;
        if (!arena_ || failed_) return false;
        auto m = AudioMixer::create(*arena_, caps_, rate());
        AudioCommand* store = arena_->alloc_array<AudioCommand>(kQueueCap);
        if (!m.ok() || !store) {
            failed_ = true;
            PHX_LOG_ERROR("audio: no room for the mixer in the arena (raise Config::total_ram)");
            return false;
        }
        for (uint32_t i = 0; i < kQueueCap; ++i) new (&store[i]) AudioCommand();
        mixer_ = m.unwrap();
        queue_.init(store, kQueueCap);
        stop_ = &GameAudio::stop_device;
        if (dev_ && dev_->start && dev_->start(int(rate()), &GameAudio::fill, this) == 0) {
            device_on_ = true;
        } else {
            if (dev_) PHX_LOG_WARN("audio: the output device did not start; mixing silently");
            pump_ = &GameAudio::pump_headless;
        }
        return true;
    }

    // The device's callback (its thread / IRQ): apply the intents, then mix.
    static void fill(void* user, int16_t* out, int frames) {
        GameAudio* a = static_cast<GameAudio*>(user);
        a->queue_.drain(*a->mixer_);
        a->mixer_->mix(out, uint32_t(frames));
        a->count_frames(uint32_t(frames));
    }
    // One writer (the device callback, or the game thread when headless): a load + store, not a
    // read-modify-write, which the GBA's ARM7TDMI has no atomic instruction for.
    void count_frames(uint32_t n) {
        frames_.store(frames_.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
    }
    // No device: the App calls this once per rendered frame on the game thread.
    static void pump_headless(GameAudio& a) {
        int16_t buf[256 * 2];
        a.queue_.drain(*a.mixer_);
        for (uint32_t left = a.rate() / a.sim_hz_; left > 0;) {
            const uint32_t n = left < 256 ? left : 256;
            a.mixer_->mix(buf, n);
            for (uint32_t i = 0; i < n * 2; ++i) {
                const int32_t v = buf[i] < 0 ? -int32_t(buf[i]) : int32_t(buf[i]);
                if (v > a.peak_) a.peak_ = v;
            }
            a.count_frames(n);
            left -= n;
        }
    }
    static void stop_device(GameAudio& a) {
        if (a.device_on_ && a.dev_ && a.dev_->stop) a.dev_->stop();
        a.device_on_ = false;
    }

    // Set on first use only, so app.cpp reaches the mixer through these and never links it.
    void (*pump_)(GameAudio&) = nullptr;
    void (*stop_)(GameAudio&) = nullptr;

    const phx_audio*  dev_    = nullptr;
    ArenaAllocator*   arena_  = nullptr;
    Caps              caps_{};
    uint16_t          sim_hz_ = 60;
    AudioMixer*       mixer_  = nullptr;
    AudioCommandQueue queue_;
    bool              device_on_ = false;
    bool              failed_    = false;
    uint32_t          plays_     = 0;
    int32_t           peak_      = 0;
    std::atomic<uint32_t> frames_{ 0 };
};

} // namespace phx
#endif // PHX_RUNTIME_AUDIO_H
