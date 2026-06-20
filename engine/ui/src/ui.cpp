// phx/ui/ui.cpp — immediate-mode UI primitives over the renderer's sprite path (docs/10 §6).
// Solid fills are a 1x1 white texture scaled to the rect and tinted; text steps a bitmap
// font atlas glyph by glyph; the focus ring is driven by Up/Down edges between frames.
#include "phx/ui/ui.h"

namespace phx {
namespace {
uint32_t g_white_pixel = 0xFFFFFFFFu;   // 1x1 RGBA white; stable storage for the quad texture
} // namespace

void UI::begin(Renderer& r, const InputState& in) {
    r_  = &r;
    in_ = &in;
    if (white_ == kNoTexture) {
        TextureDesc d{}; d.pixels = &g_white_pixel; d.width = 1; d.height = 1;
        white_ = r_->load_texture(d);
    }
    // Focus navigation uses last frame's button count so the ring size is known.
    if (prev_btn_count_ > 0) {
        int delta = 0;
        if (in_->just(Button::Down)) ++delta;
        if (in_->just(Button::Up))   --delta;
        if (delta) focus_ = ring_step(focus_, prev_btn_count_, delta);
    }
    btn_count_ = 0;
}

void UI::rect(const UIRect& rc, Rgba color, uint8_t layer) {
    DrawSprite d{};
    d.tex = white_; d.sx = 0; d.sy = 0; d.sw = 1; d.sh = 1;
    d.dw = int16_t(s_to_int(rc.size.x)); d.dh = int16_t(s_to_int(rc.size.y));
    d.pos = rc.pos; d.tint = color; d.layer = layer; d.z = 0;
    r_->draw_sprite(d);
}

void UI::image(const UIRect& dst, TextureId tex, int sx, int sy, int sw, int sh,
               Rgba tint, uint8_t layer) {
    DrawSprite d{};
    d.tex = tex; d.sx = int16_t(sx); d.sy = int16_t(sy); d.sw = int16_t(sw); d.sh = int16_t(sh);
    d.dw = int16_t(s_to_int(dst.size.x)); d.dh = int16_t(s_to_int(dst.size.y));
    d.pos = dst.pos; d.tint = tint; d.layer = layer; d.z = 1;
    r_->draw_sprite(d);
}

void UI::bar(const UIRect& rc, scalar t, Rgba fg, Rgba bg, uint8_t layer) {
    t = clamp(t, s_from_int(0), s_from_int(1));
    rect(rc, bg, layer);                                  // background (full width)
    UIRect fgr{ rc.pos, vec2{ rc.size.x * t, rc.size.y } };
    rect(fgr, fg, uint8_t(layer + 1));                    // foreground fill (on top)
}

void UI::text(vec2 pos, const BitmapFont& font, const char* str, Rgba tint, uint8_t layer) {
    if (!str || font.tex == kNoTexture) return;
    int x = s_to_int(pos.x), y = s_to_int(pos.y);
    const int x0 = x;
    for (const char* p = str; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c == '\n') { x = x0; y += font.line_h; continue; }
        if (c >= font.first_char && c != ' ') {
            int cell = c - font.first_char;
            int sx = (cell % font.cols) * font.glyph_w;
            int sy = (cell / font.cols) * font.glyph_h;
            DrawSprite d{};
            d.tex = font.tex; d.sx = int16_t(sx); d.sy = int16_t(sy);
            d.sw = int16_t(font.glyph_w); d.sh = int16_t(font.glyph_h);
            d.pos = vec2{ s_from_int(x), s_from_int(y) };
            d.tint = tint; d.layer = layer; d.z = 2;
            r_->draw_sprite(d);
        }
        x += font.advance;
    }
}

bool UI::button(const UIRect& rc, const BitmapFont& font, const char* lbl) {
    const int idx = btn_count_++;
    const bool focused = (idx == focus_);
    rect(rc, focused ? panel_focus : panel, kLayer);
    if (lbl) {
        // rough centering for fixed-width glyphs
        int len = 0; for (const char* p = lbl; *p; ++p) ++len;
        scalar tx = rc.pos.x + s_half(rc.size.x) - s_from_int(len * font.advance / 2);
        scalar ty = rc.pos.y + s_half(rc.size.y) - s_from_int(font.glyph_h / 2);
        text(vec2{ tx, ty }, font, lbl, label, uint8_t(kLayer + 1));
    }
    return focused && in_ && in_->just(Button::A);
}

void UI::end() {
    prev_btn_count_ = btn_count_;
    if (prev_btn_count_ > 0 && focus_ >= prev_btn_count_) focus_ = prev_btn_count_ - 1;
    if (focus_ < 0) focus_ = 0;
}

} // namespace phx
