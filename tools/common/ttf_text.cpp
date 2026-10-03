// tools/common/ttf_text.cpp — see ttf_text.h. Host-only.
#include "ttf_text.h"
#include "jetbrains_mono_data.h"

// stb_truetype is vendored untouched: keep its own warnings out of our zero-warning build.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "third_party/stb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <cmath>
#include <cstring>

namespace phxtool {

namespace {
constexpr int kFirst = 32, kLast = 126;                  // the embedded subset: printable ASCII
constexpr int kMissing = kLast - kFirst + 1;             // slot of the "missing glyph" box
constexpr int kSlots = kMissing + 1;
constexpr int kEmPerCanvasPx = 10;                       // em = 10 canvas px  =>  advance 6 (0.6 em)
} // namespace

struct TtfText::Impl {
    stbtt_fontinfo font{};
};

struct TtfText::Face {
    std::vector<uint8_t> bits[kSlots];
    twk::RasterGlyph     glyphs[kSlots];
};

TtfText::TtfText() : impl_(new Impl) {
    const unsigned char* data = kJetBrainsMonoAscii;
    const int off = stbtt_GetFontOffsetForIndex(data, 0);
    ok_ = off >= 0 && stbtt_InitFont(&impl_->font, data, off) != 0;
    // Light text on a dark ground looks thin when the coverage is blended as-is in sRGB space, so
    // lift the mid-tones a little (a gamma of 1/1.15) — the same reason editors ship "text contrast".
    for (int i = 0; i < 256; ++i)
        curve_[i] = uint8_t(std::min(255.0, std::pow(i / 255.0, 1.0 / 1.15) * 255.0 + 0.5));
}

TtfText::~TtfText() = default;

const twk::RasterGlyph* TtfText::glyph(int ch, int px_scale) {
    if (!ok_ || px_scale < 1) return nullptr;
    auto it = faces_.find(px_scale);
    if (it == faces_.end()) {
        std::unique_ptr<Face> f(new Face);
        const float s = stbtt_ScaleForMappingEmToPixels(&impl_->font, float(kEmPerCanvasPx * px_scale));
        for (int slot = 0; slot < kSlots; ++slot) {
            const int cp = slot == kMissing ? 0 : kFirst + slot;
            const int gi = slot == kMissing ? 0 : stbtt_FindGlyphIndex(&impl_->font, cp);
            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            stbtt_GetGlyphBitmapBox(&impl_->font, gi, s, s, &x0, &y0, &x1, &y1);
            twk::RasterGlyph& g = f->glyphs[slot];
            g.w = x1 - x0; g.h = y1 - y0; g.left = x0; g.top = y0;
            if (g.w <= 0 || g.h <= 0) { g = twk::RasterGlyph{}; continue; }   // a space: nothing to draw
            std::vector<uint8_t>& b = f->bits[slot];
            b.assign(size_t(g.w) * size_t(g.h), 0);
            stbtt_MakeGlyphBitmap(&impl_->font, b.data(), g.w, g.h, g.w, s, s, gi);
            for (uint8_t& v : b) v = curve_[v];
            g.cov = b.data();
        }
        it = faces_.emplace(px_scale, std::move(f)).first;
    }
    const int slot = ch >= kFirst && ch <= kLast ? ch - kFirst : kMissing;
    const twk::RasterGlyph& g = it->second->glyphs[slot];
    return g.cov ? &g : nullptr;
}

} // namespace phxtool
