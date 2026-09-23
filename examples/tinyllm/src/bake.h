// examples/tinyllm/src/bake.h — HOST-ONLY asset bake for the tinyllm example (STL/host allowed;
// never compiled into the game binary, exactly like miracle-player's bake.h).
//
// It runs the SAME pipeline every other example uses (docs/08): the model is an opaque binary, so
// it goes in as a generic Blob through `BundleWriter::add_blob` — the phxbin/phxpack path — and
// the runtime reads it ZERO-COPY in place off the bundle image. No new tool, no new format in the
// bundle. The font atlas goes in as an ordinary texture and gets the per-target encode for free
// (tier 0 bakes it to 4bpp paletted tiles, which is what the GBA PPU wants for a text console).
//
// The `.phxllm` itself is produced offline by tools/export_model.py from a real checkpoint. When
// that file is absent (no network, a fresh clone), the bake falls back to the tiny generated
// fixture so every target still builds and runs — the same shape as miracle-player falling back
// to a synthetic tone when the song WAV is missing.
#ifndef TINYLLM_BAKE_H
#define TINYLLM_BAKE_H

#include "bundle_writer.h"   // tools/phxpack
#include "llm_build.h"
#include "llm_format.h"
#include "text_font.h"

#include <cstdio>
#include <string>
#include <vector>

namespace tinyllm {

inline uint32_t pack_rgba(int r, int g, int b, int a = 255) {
    auto c = [](int v) { return uint32_t(v < 0 ? 0 : v > 255 ? 255 : v); };
    return c(r) | (c(g) << 8) | (c(b) << 16) | (c(a) << 24);   // R low .. A high (pixel.h layout)
}

// Render kFont5x7 into the 128x96 atlas: the 96 printable glyphs in white (cells 0..95) then the
// same 96 in amber (cells 96..191). Two colour banks instead of tinting, because the GBA PPU
// cannot tint a background tile — the console selects a colour by selecting a tile.
inline void build_font_atlas(uint32_t* px) {
    const uint32_t white = pack_rgba(240, 240, 245);
    const uint32_t amber = pack_rgba(255, 190, 90);
    for (int i = 0; i < kFontAtlasW * kFontAtlasH; ++i) px[i] = 0u;   // transparent
    for (int bank = 0; bank < 2; ++bank) {
        const uint32_t col = bank ? amber : white;
        for (int g = 0; g < kFontGlyphs; ++g) {
            const int cell = bank * kFontBankB + g;
            const int cx = (cell % kFontCols) * 8;
            const int cy = (cell / kFontCols) * 8;
            for (int ry = 0; ry < 7; ++ry)
                for (int rx = 0; rx < 5; ++rx)
                    if (kFont5x7[g][ry] & (1u << (4 - rx)))
                        px[(cy + ry) * kFontAtlasW + (cx + rx + 1)] = col;
        }
    }
}

// Read a `.phxllm` produced by tools/export_model.py. Returns an empty vector when the file is
// missing or obviously not a model, so callers can fall back rather than fail the build.
inline std::vector<uint8_t> load_model_file(const char* path) {
    std::vector<uint8_t> data;
    if (!path || !*path) return data;
    FILE* f = std::fopen(path, "rb");
    if (!f) return data;
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz > long(sizeof(LlmHeader))) {
        data.resize(size_t(sz));
        if (std::fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
    }
    std::fclose(f);
    if (!data.empty() && !llm_validate(data.data(), uint32_t(data.size()))) {
        std::fprintf(stderr, "tinyllm: '%s' failed .phxllm validation — ignoring\n", path);
        data.clear();
    }
    return data;
}

// Bake the bundle. `tier`: 0 = GBA (4bpp font tiles), 1 = PSP, 2 = PC.
// `model_path`: a baked .phxllm, or nullptr/"" / a missing file for the generated fixture.
inline bool bake_tinyllm_assets(const char* out, uint8_t tier = 2,
                                const char* model_path = nullptr, bool quiet = false) {
    std::vector<uint8_t> model = load_model_file(model_path);
    const bool real = !model.empty();
    if (!real) model = build::build_fixture_blob();

    const LlmHeader* h = llm_validate(model.data(), uint32_t(model.size()));
    if (!h) {
        std::fprintf(stderr, "tinyllm: internal error — the fixture model is not valid\n");
        return false;
    }

    phxtool::BundleWriter w{tier};
    // Deliberately NOT compressed: the runtime reads weights in place out of cartridge ROM, and
    // LZSS would force a ~300 KB decompress into EWRAM, which is the entire memory budget.
    w.set_compression(false);
    w.add_blob("model", model.data(), uint32_t(model.size()));

    std::vector<uint32_t> font(size_t(kFontAtlasW) * kFontAtlasH);
    build_font_atlas(font.data());
    w.add_texture("font", font.data(), kFontAtlasW, kFontAtlasH);

    if (!quiet) {
        std::printf("  model: %s (%u bytes) dim=%u layers=%u heads=%u/%u vocab=%u seq=%u\n",
                    real ? model_path : "generated fixture (no .phxllm found)",
                    unsigned(model.size()), h->dim, h->n_layers, h->n_heads, h->n_kv_heads,
                    h->vocab_size, h->seq_len);
        std::printf("  quantization: int8 group=%u, Q30 scales; KV cache int16 Q%d\n",
                    h->group_size, int(h->kv_shift));
    }
    return w.write(out);
}

} // namespace tinyllm
#endif // TINYLLM_BAKE_H
