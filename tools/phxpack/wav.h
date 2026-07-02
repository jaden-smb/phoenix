// tools/phxpack/wav.h — a minimal RIFF/WAVE decoder for the asset pipeline (host-only). Reads
// uncompressed PCM (format 1), 8-bit unsigned or 16-bit signed, mono or stereo, and produces a
// MONO 16-bit signed stream — exactly the shape the mixer's SoundView wants (stereo is downmixed
// by averaging; 8-bit is centered and scaled to 16-bit). Returns false on anything unsupported
// so the bake fails loudly. Authoring tools export this format; ADPCM/float can be added later.
#ifndef PHX_TOOLS_WAV_H
#define PHX_TOOLS_WAV_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace phxtool {

namespace detail {
inline uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | (uint16_t(p[1]) << 8)); }
inline uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline bool tag(const uint8_t* p, const char* t) {
    return p[0] == uint8_t(t[0]) && p[1] == uint8_t(t[1]) && p[2] == uint8_t(t[2]) && p[3] == uint8_t(t[3]);
}
} // namespace detail

// Decode `data[0..n)`; on success fills `mono` (16-bit signed) and `rate`, returns true.
inline bool wav_decode(const uint8_t* data, size_t n, std::vector<int16_t>& mono, uint32_t& rate) {
    using namespace detail;
    if (n < 12 || !tag(data, "RIFF") || !tag(data + 8, "WAVE")) return false;

    uint16_t fmt = 0, channels = 0, bits = 0;
    uint32_t srate = 0;
    const uint8_t* pcm = nullptr; size_t pcm_size = 0;
    bool have_fmt = false;

    size_t off = 12;
    while (off + 8 <= n) {
        const uint8_t* id = data + off;
        const uint32_t sz = le32(data + off + 4);
        const uint8_t* body = data + off + 8;
        if (off + 8 + size_t(sz) > n) break;          // truncated chunk
        if (tag(id, "fmt ")) {
            if (sz < 16) return false;
            fmt      = le16(body + 0);
            channels = le16(body + 2);
            srate    = le32(body + 4);
            bits     = le16(body + 14);
            have_fmt = true;
        } else if (tag(id, "data")) {
            pcm = body; pcm_size = sz;
        }
        off += 8 + size_t(sz) + (sz & 1u);            // chunks are word-aligned (pad byte)
    }

    if (!have_fmt || !pcm || fmt != 1) return false;  // only uncompressed PCM
    if (channels < 1 || channels > 2) return false;
    if (bits != 8 && bits != 16) return false;

    rate = srate ? srate : 44100;
    const uint32_t bytes_per_sample = bits / 8;
    const uint32_t frame_bytes = bytes_per_sample * channels;
    if (frame_bytes == 0) return false;
    const uint32_t frames = uint32_t(pcm_size / frame_bytes);
    mono.resize(frames);

    for (uint32_t i = 0; i < frames; ++i) {
        const uint8_t* f = pcm + size_t(i) * frame_bytes;
        int32_t acc = 0;
        for (uint32_t c = 0; c < channels; ++c) {
            int32_t s;
            if (bits == 16) s = int16_t(le16(f + c * 2));         // signed LE
            else            s = (int32_t(f[c]) - 128) * 256;      // 8-bit unsigned -> signed 16
                                                                  // (× not <<: negative-shift UB)
            acc += s;
        }
        mono[i] = int16_t(acc / int32_t(channels));               // downmix to mono
    }
    return true;
}

} // namespace phxtool
#endif // PHX_TOOLS_WAV_H
