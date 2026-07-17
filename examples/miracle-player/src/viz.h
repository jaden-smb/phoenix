// examples/miracle-player/src/viz.h — the precomputed "visualization track" format shared by
// the host bake tool (tools/phxviz), the on-device player app, and the unit tests.
//
// THE KEY IDEA: no FFT runs on the GBA. At bake time the host analyses the song and emits one
// fixed-size record per video frame; the ROM reads the stream ZERO-COPY (it is a generic
// AssetType::Blob in the .phxp bundle) and, each frame, maps the audio playback position to a
// record index by pure INTEGER division — identical on both scalar tiers, byte-for-byte.
//
// This header is deliberately engine-free and GBA-safe: only <stdint.h> fixed-width integers,
// POD structs, no STL, no float, no engine module. So it compiles unchanged into the host tool
// (STL/float around it) and the console ROM. Layout is asserted so the baked bytes and the
// runtime view can never disagree (same discipline as tools/phxbin's generated structs).
#ifndef MIRACLE_VIZ_H
#define MIRACLE_VIZ_H

#include <stdint.h>

namespace miracle {

// 'P''V''Z''1' little-endian. Bump kVizVersion on any layout change.
constexpr uint32_t kVizMagic   = uint32_t('P') | (uint32_t('V') << 8) |
                                 (uint32_t('Z') << 16) | (uint32_t('1') << 24);
constexpr uint16_t kVizVersion = 1;
constexpr uint32_t kVizBands   = 16;   // log-spaced spectrum bands per frame

// One visualization record per VIDEO frame. Fixed 24 bytes (multiple of 4 -> the record array
// stays naturally aligned for zero-copy). Every field is a uint8 in [0,255]: quantised at bake
// time so the runtime only ever reads bytes.
struct VizFrame {
    uint8_t bands[kVizBands];  // log-spaced magnitude spectrum (the spectrogram / bars)
    uint8_t rms;               // overall loudness envelope
    uint8_t onset;             // spectral-flux onset strength (0 = none)
    uint8_t low;               // bass band energy   (particle / shake routing)
    uint8_t mid;               // mid band energy
    uint8_t high;              // treble band energy
    uint8_t beat;              // 1 on a peak-picked beat frame, else 0
    uint8_t _pad[2];           // pad to 24 bytes
};

// Blob header, immediately followed by frame_count VizFrame records.
struct VizHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t band_count;    // == kVizBands (checked)
    uint32_t frame_count;   // number of VizFrame records that follow
    uint32_t device_rate;   // audio sample rate the stream is aligned to (Hz)
    uint32_t hop_samples;   // audio samples per video frame; index = samples / hop_samples
};

static_assert(sizeof(VizFrame)  == 24, "VizFrame layout changed (record must stay 4-byte sized)");
static_assert(sizeof(VizHeader) == 20, "VizHeader layout changed");

// --- runtime accessors (pure integer, tier-identical, no allocation) -----------------------

inline const VizFrame* viz_frames(const VizHeader* h) {
    return reinterpret_cast<const VizFrame*>(reinterpret_cast<const uint8_t*>(h) + sizeof(VizHeader));
}

// Map an absolute audio sample position to a record index, clamped to the last frame. Pure
// integer division: the single source of truth for A/V lock (derive from samples consumed).
inline uint32_t viz_index_for_sample(const VizHeader* h, uint64_t sample) {
    if (h->hop_samples == 0 || h->frame_count == 0) return 0;
    uint64_t i = sample / h->hop_samples;
    return i < h->frame_count ? uint32_t(i) : h->frame_count - 1;
}

inline const VizFrame* viz_frame_at(const VizHeader* h, uint64_t sample) {
    return viz_frames(h) + viz_index_for_sample(h, sample);
}

// Validate a mounted blob before trusting it: magic/version/band-count and that the declared
// record array actually fits in `size` bytes. Returns the typed header or nullptr.
inline const VizHeader* viz_validate(const void* data, uint32_t size) {
    if (!data || size < sizeof(VizHeader)) return nullptr;
    const VizHeader* h = reinterpret_cast<const VizHeader*>(data);
    if (h->magic != kVizMagic || h->version != kVizVersion) return nullptr;
    if (h->band_count != kVizBands || h->hop_samples == 0)   return nullptr;
    uint64_t need = uint64_t(sizeof(VizHeader)) + uint64_t(h->frame_count) * sizeof(VizFrame);
    return need <= size ? h : nullptr;
}

} // namespace miracle
#endif // MIRACLE_VIZ_H
