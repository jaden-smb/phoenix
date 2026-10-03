// tools/common/ttf_text.h — the Studio's TrueType text: JetBrains Mono (SIL OFL 1.1, subset to
// printable ASCII and embedded in jetbrains_mono_data.h), rasterized with stb_truetype
// (third_party/stb_truetype.h, public domain) into anti-aliased coverage bitmaps that twk::Gui
// blends at window resolution. JetBrains Mono's advance is exactly 0.6 em, so an em of
// 10 * px_scale window pixels advances exactly 6 * px_scale: the same 6 px pitch (and 10 px line)
// as the bitmap font it replaces, on every UI scale. Host-only. Linked by the tools that opt in
// (ttf_text.cpp): today phxstudio.
#ifndef PHX_TOOLS_TTF_TEXT_H
#define PHX_TOOLS_TTF_TEXT_H

#include "text_raster.h"

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace phxtool {

class TtfText final : public twk::TextRaster {
public:
    TtfText();
    ~TtfText() override;
    TtfText(const TtfText&) = delete;
    TtfText& operator=(const TtfText&) = delete;

    bool ok() const { return ok_; }                       // the embedded face parsed
    const twk::RasterGlyph* glyph(int ch, int px_scale) override;

private:
    struct Face;                                          // one px_scale's glyph bitmaps
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::map<int, std::unique_ptr<Face>> faces_;
    uint8_t curve_[256];                                  // coverage -> alpha (text weight)
    bool ok_ = false;
};

} // namespace phxtool
#endif // PHX_TOOLS_TTF_TEXT_H
