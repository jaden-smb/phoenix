// phx/audio/stream.h — audio streaming (docs/06 §5). Large music/clips don't sit fully
// resident: a producer tick decodes+resamples the next chunk into a ring buffer ahead of the
// read cursor, and the mixer drains the ring on the audio path. This splits the heavy work
// (resample, and later real decode) onto a low-priority/resource tick — the audio callback
// only sums cheap, already-output-rate samples.
//
// The ring is single-producer/single-consumer: pump() is the only writer, the mixer's read()
// the only reader. Indices are std::atomic with acquire/release, so it is correct when the
// producer and consumer run on different threads (the real device case) and equally correct
// single-threaded (how the headless tests drive it). On GBA (one core, no FS) nothing streams.
#ifndef PHX_AUDIO_STREAM_H
#define PHX_AUDIO_STREAM_H

#include "phx/core/types.h"
#include "phx/audio/mixer.h"      // SoundView

#include <atomic>

namespace phx {

// SPSC ring of mono int16 samples. Capacity MUST be a power of two (mask-based wrap + the
// free-running index subtraction both rely on it). No allocation: storage is caller-provided.
class RingBuffer {
public:
    void init(int16_t* storage, uint32_t cap_pow2) {
        buf_ = storage; cap_ = cap_pow2; mask_ = cap_pow2 - 1;
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }
    void reset() { head_.store(0, std::memory_order_relaxed); tail_.store(0, std::memory_order_relaxed); }

    uint32_t capacity() const { return cap_; }
    uint32_t available() const {     // readable count (consumer view)
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_relaxed);
    }
    uint32_t free_space() const {    // writable count (producer view)
        return cap_ - (head_.load(std::memory_order_relaxed) - tail_.load(std::memory_order_acquire));
    }

    // Producer: copy up to n samples in; returns the count actually written (<= free_space()).
    uint32_t write(const int16_t* src, uint32_t n) {
        const uint32_t h = head_.load(std::memory_order_relaxed);
        const uint32_t t = tail_.load(std::memory_order_acquire);
        const uint32_t fs = cap_ - (h - t);
        if (n > fs) n = fs;
        for (uint32_t i = 0; i < n; ++i) buf_[(h + i) & mask_] = src[i];
        head_.store(h + n, std::memory_order_release);
        return n;
    }
    // Consumer: copy up to n samples out; returns the count actually read (<= available()).
    uint32_t read(int16_t* dst, uint32_t n) {
        const uint32_t t = tail_.load(std::memory_order_relaxed);
        const uint32_t h = head_.load(std::memory_order_acquire);
        const uint32_t av = h - t;
        if (n > av) n = av;
        for (uint32_t i = 0; i < n; ++i) dst[i] = buf_[(t + i) & mask_];
        tail_.store(t + n, std::memory_order_release);
        return n;
    }

private:
    int16_t* buf_  = nullptr;
    uint32_t cap_  = 0;
    uint32_t mask_ = 0;
    std::atomic<uint32_t> head_{0};   // producer-owned (write index)
    std::atomic<uint32_t> tail_{0};   // consumer-owned (read index)
};

// A streaming audio source: resamples a SoundView to the output rate into its ring on pump(),
// and hands output-rate mono samples to the mixer on read(). The source can be a zero-copy
// view straight into a mmap'd bundle blob — so the "resource system refills the stream" is
// literally pump() reading from the mounted asset.
class AudioStream {
public:
    // Provide ring storage (cap_pow2 power-of-two samples) and the mixer's output rate.
    void init(int16_t* ring_storage, uint32_t ring_cap_pow2, uint32_t out_rate) {
        ring_.init(ring_storage, ring_cap_pow2);
        out_rate_ = out_rate ? out_rate : 44100;
    }

    // Begin streaming `src` (loop to restart at end). Resets the cursor and drains the ring.
    void open(const SoundView& src, bool loop);
    void close() { producing_ = false; ring_.reset(); }

    // Live until a non-loop source has been fully resampled AND drained from the ring.
    bool     active()   const { return producing_ || ring_.available() != 0; }
    uint32_t buffered() const { return ring_.available(); }   // samples ready to read

    // Producer tick: resample source -> ring until the ring is full or a non-loop source ends.
    // Returns the number of samples produced this call. Call off the audio path.
    uint32_t pump();

    // Consumer: pop up to n output-rate mono samples; returns count (< n on underrun).
    uint32_t read(int16_t* dst, uint32_t n) { return ring_.read(dst, n); }

private:
    RingBuffer ring_;
    SoundView  src_{};
    uint64_t   pos_      = 0;     // Q16 source cursor
    uint32_t   step_     = 0;     // Q16 increment = src_rate / out_rate
    uint32_t   out_rate_ = 44100;
    bool       loop_     = false;
    bool       producing_ = false; // source still has samples to resample into the ring
};

} // namespace phx
#endif // PHX_AUDIO_STREAM_H
