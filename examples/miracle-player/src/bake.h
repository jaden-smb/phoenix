// examples/miracle-player/src/bake.h — HOST-ONLY asset bake for the "A Small Miracle" music
// visualizer. Runs the SAME importers the CLI tools use (docs/08): a WAV -> a resident Sound
// asset (tier 0 resamples to the GBA device rate) and the offline visualization track
// (tools/phxviz/analyze.h -> a generic Blob the ROM reads zero-copy), plus a font atlas and a
// heart glyph for the dedication cards. Produces the .phxp the game mounts. NOT compiled into
// the game binary (uses the host BundleWriter / STL). MP3 is decoded to WAV on the host first
// (ffmpeg) — no MP3 decoder ships in-tree.
#ifndef MIRACLE_BAKE_H
#define MIRACLE_BAKE_H

#include "bundle_writer.h"   // tools/phxpack
#include "wav.h"             // RIFF/WAV decoder
#include "analyze.h"         // tools/phxviz — offline visualization analysis
#include "debug_font.h"      // tools/common — shared 5x7 font atlas
#include "viz.h"             // the shared POD format (for kVizBands etc.)

#include <cstdint>
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

namespace miracle {

// An 8x8 red heart glyph (RGBA8), baked as its own texture and drawn inline on the outro card —
// the GBA has no emoji, so the "❤" is a real tile (the prompt's requirement).
inline void build_heart(uint32_t* px) {
    static const uint8_t rows[8] = {
        0b01100110,
        0b11111111,
        0b11111111,
        0b11111111,
        0b01111110,
        0b00111100,
        0b00011000,
        0b00000000,
    };
    const uint32_t red = 0xFF3C1EE6u;   // rgba(230,30,60,255): R low byte .. A high byte (pixel.h)
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            px[y * 8 + x] = (rows[y] & (0x80u >> x)) ? red : 0u;
}

inline uint32_t pack_rgba(int r, int g, int b, int a = 255) {
    auto c = [](int v) { return uint32_t(v < 0 ? 0 : v > 255 ? 255 : v); };
    return c(r) | (c(g) << 8) | (c(b) << 16) | (c(a) << 24);   // R low .. A high (pixel.h layout)
}

// 16 solid 8x8 colour tiles (a warm->cool gradient) — the spectrogram tileset. The BG map's
// cell value b+1 selects band b's colour tile; cell 0 is the reserved empty/transparent tile.
inline void build_spectrum_tiles(uint32_t* px) {          // 128x8 RGBA (16 tiles across)
    for (int t = 0; t < 16; ++t) {
        const uint32_t col = pack_rgba(230 - t * 10, 50 + t * 11, 120 + t * 7);
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                px[y * 128 + t * 8 + x] = col;
    }
}

// 4 colour spark dots (8x8, rounded) — the particle atlas. Drawn as OBJ on GBA (tinting/scaling
// are not expressible there, so the colour is baked in and the size is native 8x8).
inline void build_sparks(uint32_t* px) {                  // 32x8 RGBA (4 sparks across)
    static const uint32_t cols[4] = {
        pack_rgba(255, 220, 120), pack_rgba(120, 220, 255),
        pack_rgba(255, 120, 180), pack_rgba(180, 255, 140),
    };
    for (int i = 0; i < 32 * 8; ++i) px[i] = 0;
    for (int t = 0; t < 4; ++t)
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) {
                const int dx = x - 3, dy = y - 3;         // rounded dot (radius ~3)
                if (dx * dx + dy * dy <= 9) px[y * 32 + t * 8 + x] = cols[t];
            }
}

// Decode a WAV file into mono int16; on failure (or no path) synthesise a short tone so the
// headless suite and a source-less build still exercise the whole pipeline.
inline bool load_song(const char* wav_path, std::vector<int16_t>& mono, uint32_t& rate) {
    if (wav_path && *wav_path) {
        std::vector<uint8_t> bytes;
        FILE* f = std::fopen(wav_path, "rb");
        if (f) {
            std::fseek(f, 0, SEEK_END); long sz = std::ftell(f); std::fseek(f, 0, SEEK_SET);
            if (sz > 0) { bytes.resize(size_t(sz)); size_t got = std::fread(bytes.data(), 1, size_t(sz), f); if (got != size_t(sz)) bytes.clear(); }
            std::fclose(f);
        }
        if (!bytes.empty() && phxtool::wav_decode(bytes.data(), bytes.size(), mono, rate) && !mono.empty())
            return true;
    }
    // Fallback: a 3-second 220 Hz + 440 Hz two-tone at 22050 Hz (audible, non-silent, tiny).
    rate = 22050;
    mono.resize(size_t(rate) * 3);
    for (uint32_t i = 0; i < mono.size(); ++i) {
        const double t = double(i) / rate;
        const double s = 0.35 * std::sin(2.0 * M_PI * 220.0 * t) + 0.25 * std::sin(2.0 * M_PI * 440.0 * t);
        mono[i] = int16_t(s * 30000.0);
    }
    return true;
}

// Bake the bundle. tier: 0=GBA (resamples audio+viz to 18157 Hz), 2=PC (keeps the WAV rate).
// wav_path: the pre-decoded song WAV, or nullptr/"" for the synthetic fallback.
inline bool bake_miracle_assets(const char* out, uint8_t tier = 2, const char* wav_path = nullptr) {
    std::vector<int16_t> mono; uint32_t rate = 0;
    if (!load_song(wav_path, mono, rate)) return false;

    phxtool::BundleWriter w{tier};

    // Resident song PCM (tier 0 resamples to the 18157 Hz device rate at bake time).
    w.add_sound("song", mono.data(), uint32_t(mono.size()), rate);

    // Visualization track, analysed at the SAME rate the baked audio plays at, so the runtime's
    // integer samples->index map lines up on both host and device.
    const uint32_t viz_rate = (tier == 0) ? 18157u : rate;
    std::vector<uint8_t> viz = phxviz::analyze_pcm(mono.data(), uint32_t(mono.size()), rate, viz_rate);
    w.add_blob("viz", viz.data(), uint32_t(viz.size()));

    // Font atlas (the shared 5x7 debug font) + the heart glyph for the outro.
    uint32_t font[phxtool::kDebugFontW * phxtool::kDebugFontH];
    phxtool::build_debug_font(font);
    w.add_texture("font", font, phxtool::kDebugFontW, phxtool::kDebugFontH);

    uint32_t heart[8 * 8];
    build_heart(heart);
    w.add_texture("heart", heart, 8, 8);

    uint32_t spectrum[128 * 8];      // the 16 colour bars tileset
    build_spectrum_tiles(spectrum);
    w.add_texture("spectrum", spectrum, 128, 8);

    uint32_t sparks[32 * 8];         // the 4-colour particle atlas
    build_sparks(sparks);
    w.add_texture("spark", sparks, 32, 8);

    return w.write(out);
}

} // namespace miracle
#endif // MIRACLE_BAKE_H
