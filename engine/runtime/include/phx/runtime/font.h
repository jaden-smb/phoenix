// phx/runtime/font.h — a font from the bundle, ready for phx::UI:
//
//     BitmapFont font;
//     load_font(app.render(), *res, "font"_hash, font);      // on_start
//     ui.text(pos, font, "Hello");                           // on_render
//     const int w = UI::text_width(font, "Hello");            // centre / right-align with it
//
// A baked Font asset (a `.font` sheet or an imported BMFont `.fnt`, tools/phxpack builders.h)
// brings its glyph table — proportional widths, offsets, the line height — and names its atlas
// texture, which this uploads. The glyph table is read in place (zero-copy): the BitmapFont
// points into the bundle, so keep the bundle mounted while the font is in use. A name that is
// only a TEXTURE (an older project's font.png) loads as the classic grid: 8x8 cells, 16 columns
// from ASCII 32, with `grid_advance` px per character.
//
// Header-only: games that draw no text link nothing.
#ifndef PHX_RUNTIME_FONT_H
#define PHX_RUNTIME_FONT_H

#include "phx/render/renderer.h"
#include "phx/resource/cache.h"
#include "phx/ui/ui.h"

#include <cstddef>

namespace phx {

static_assert(sizeof(FontGlyph) == sizeof(FontGlyphDef) && offsetof(FontGlyph, sx) == offsetof(FontGlyphDef, sx) &&
              offsetof(FontGlyph, w) == offsetof(FontGlyphDef, w) &&
              offsetof(FontGlyph, advance) == offsetof(FontGlyphDef, advance) &&
              offsetof(FontGlyph, xoff) == offsetof(FontGlyphDef, xoff) &&
              offsetof(FontGlyph, yoff) == offsetof(FontGlyphDef, yoff),
              "ui FontGlyph and the baked FontGlyphDef must share a layout");

// Load font `name` into `out`. False (and `out` untouched) when there is no such font or texture.
inline bool load_font(Renderer& r, ResourceCache& res, NameHash name, BitmapFont& out, uint8_t grid_advance = 6) {
    uint16_t tex_w = 0;
    auto upload = [&](NameHash tex) -> TextureId {
        if (!res.has(tex, AssetType::Texture)) return kNoTexture;
        auto t = res.texture(tex);
        if (!t) return kNoTexture;
        const TextureView v = t.unwrap();
        tex_w = v.width;
        TextureDesc d{};
        d.pixels = v.pixels; d.width = v.width; d.height = v.height; d.format = v.format;
        return r.load_texture(d);
    };
    if (res.has(name, AssetType::Font)) {
        auto f = res.font(name);
        if (!f) return false;
        const FontView v = f.unwrap();
        const TextureId tex = upload(v.texture);
        if (tex == kNoTexture) return false;
        BitmapFont b;
        b.tex = tex;
        b.glyph_w = v.cell_w; b.glyph_h = v.cell_h;
        b.cols = uint8_t(v.cell_w && tex_w / v.cell_w ? (tex_w / v.cell_w > 255 ? 255 : tex_w / v.cell_w) : 16);
        b.first_char = v.first_char;
        b.advance = v.advance; b.line_h = v.line_h;
        b.glyphs = reinterpret_cast<const FontGlyph*>(v.glyphs);
        b.glyph_count = v.glyph_count;
        out = b;
        return true;
    }
    const TextureId tex = upload(name);
    if (tex == kNoTexture) return false;
    BitmapFont b;
    b.tex = tex;
    b.glyph_w = 8; b.glyph_h = 8; b.cols = 16; b.first_char = 32;
    b.advance = grid_advance; b.line_h = 9;
    out = b;
    return true;
}

} // namespace phx
#endif // PHX_RUNTIME_FONT_H
