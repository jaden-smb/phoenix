// phx/audio/src/stream.cpp — the producer side of audio streaming. pump() resamples the
// source into the ring at the output rate (nearest-sample, the same Q16 cursor the mixer uses
// for resident voices), so the consumer drains 1:1. All-integer -> identical on both tiers.
#include "phx/audio/stream.h"

namespace phx {

void AudioStream::open(const SoundView& src, bool loop) {
    src_  = src;
    loop_ = loop;
    pos_  = 0;
    step_ = uint32_t((uint64_t(src.rate ? src.rate : out_rate_) << 16) / out_rate_);
    ring_.reset();
    producing_ = (src.samples != nullptr && src.frames != 0);
}

uint32_t AudioStream::pump() {
    if (!producing_) return 0;
    int16_t tmp[256];
    uint32_t produced = 0;
    while (producing_) {
        uint32_t fs = ring_.free_space();
        if (fs == 0) break;                      // ring full — caller will drain then pump again
        const uint32_t batch = fs < 256 ? fs : 256;
        uint32_t made = 0;
        for (; made < batch; ++made) {
            uint32_t si = uint32_t(pos_ >> 16);
            if (si >= src_.frames) {
                if (loop_) { pos_ %= (uint64_t(src_.frames) << 16); si = uint32_t(pos_ >> 16); }
                else { producing_ = false; break; }   // non-loop source exhausted
            }
            tmp[made] = src_.samples[si];
            pos_ += step_;
        }
        if (made) { ring_.write(tmp, made); produced += made; }
        if (made < batch) break;                 // stopped early (source ended)
    }
    return produced;
}

} // namespace phx
