// tools/common/text_raster.h — the seam between the tool widget kit (twk.h) and a native-resolution
// text rasterizer. twk::Gui draws its text as 5x7 bitmap glyphs on the engine's nearest-upscaled
// canvas; a tool that links a TrueType rasterizer (ttf_text.h) hands one to Gui::set_text_raster()
// and its text is drawn smooth, at WINDOW resolution, in a platform overlay layer instead
// (phx/platform/desktop.h: phx_desktop_overlay_begin). Keeping this a plain interface means
// twk.h itself needs no font data and no rasterizer, so the tools that do not opt in (phxtmap,
// phxentity, the headless editor tests) link exactly what they did before. Host-only.
#ifndef PHX_TOOLS_TEXT_RASTER_H
#define PHX_TOOLS_TEXT_RASTER_H

#include <cstdint>

namespace twk {

// One rasterized glyph: an 8-bit coverage bitmap (0 = none, 255 = solid). `left`/`top` place its
// top-left corner relative to the pen origin ON THE BASELINE, in window pixels, y growing down
// (so `top` is negative for a glyph that rises above the baseline).
struct RasterGlyph {
    const uint8_t* cov = nullptr;
    int w = 0, h = 0;
    int left = 0, top = 0;
};

// A monospace face laid out on the tool's canvas grid: with `px_scale` window pixels per canvas
// pixel, one character advances exactly kAdv (6) * px_scale pixels and the line pitch is
// kLineH (10) * px_scale, so swapping it in for the bitmap font never moves a widget.
class TextRaster {
public:
    virtual ~TextRaster() = default;
    // The glyph for ASCII `ch` at `px_scale` (>= 1). Characters the face lacks come back as its
    // "missing glyph" box. The pointer stays valid for the life of the rasterizer; null = draw nothing.
    virtual const RasterGlyph* glyph(int ch, int px_scale) = 0;
};

} // namespace twk
#endif // PHX_TOOLS_TEXT_RASTER_H
