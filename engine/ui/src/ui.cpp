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

void UI::dialogue(const UIRect& box, const BitmapFont& font, const DialogueView& dv,
                  int line, scalar reveal_t, uint8_t layer) {
    if (!dv.lines || line < 0 || line >= int(dv.count) || font.tex == kNoTexture) return;
    const char* str = dv.lines + size_t(line) * dv.stride;

    rect(box, panel, layer);

    const int bx = s_to_int(box.pos.x), by = s_to_int(box.pos.y);
    int x0 = bx + 3, y0 = by + 3;
    if (dv.portrait != kNoTexture && dv.portrait_w) {
        image(UIRect{ vec2{ s_from_int(x0), s_from_int(y0) },
                      vec2{ s_from_int(dv.portrait_w), s_from_int(dv.portrait_h) } },
              dv.portrait, 0, 0, dv.portrait_w, dv.portrait_h,
              rgba(255, 255, 255), uint8_t(layer + 1));
        x0 += dv.portrait_w + 3;
    }
    const int cols = (bx + s_to_int(box.size.x) - 3 - x0) / font.advance;
    if (cols <= 0) return;

    // Reveal budget: floor(reveal_t × total printable chars), in Q16 so both scalar tiers
    // reveal the identical character on the identical tick.
    int total = 0; for (const char* p = str; *p; ++p) ++total;
    int budget = total;
    if (s_to_q16(reveal_t) < (1 << 16)) {
        int32_t q = s_to_q16(reveal_t); if (q < 0) q = 0;
        budget = int((int64_t(q) * total) >> 16);
    }

    // Word-wrapped typewriter: separators count toward the reveal (uniform pacing); a word
    // that would overflow the row wraps before it draws; over-long words hard-wrap.
    int x = x0, y = y0, col = 0, shown = 0;
    for (const char* p = str; *p && shown < budget; ) {
        if (*p == '\n') { ++p; ++shown; col = 0; x = x0; y += font.line_h; continue; }
        if (*p == ' ') {
            ++p; ++shown;
            int wl = 0; for (const char* q = p; *q && *q != ' ' && *q != '\n'; ++q) ++wl;
            if (col + 1 + wl > cols) { col = 0; x = x0; y += font.line_h; }
            else                     { ++col; x += font.advance; }
            continue;
        }
        if (col >= cols) { col = 0; x = x0; y += font.line_h; }
        const unsigned char c = (unsigned char)*p;
        if (c >= font.first_char) {
            const int cell = c - font.first_char;
            DrawSprite d{};
            d.tex = font.tex;
            d.sx = int16_t((cell % font.cols) * font.glyph_w);
            d.sy = int16_t((cell / font.cols) * font.glyph_h);
            d.sw = int16_t(font.glyph_w); d.sh = int16_t(font.glyph_h);
            d.pos = vec2{ s_from_int(x), s_from_int(y) };
            d.tint = label; d.layer = uint8_t(layer + 1); d.z = 2;
            r_->draw_sprite(d);
        }
        ++col; x += font.advance; ++shown; ++p;
    }

    // Fully revealed: a small "continue" marker in the bottom-right corner of the box.
    if (budget >= total)
        rect(UIRect{ vec2{ box.pos.x + box.size.x - s_from_int(5),
                           box.pos.y + box.size.y - s_from_int(5) },
                     vec2{ s_from_int(3), s_from_int(3) } },
             label, uint8_t(layer + 2));
}

namespace {
// us -> "12.3" (ms, one decimal) into buf; returns chars written. Integer math only.
int fmt_ms(char* buf, uint32_t us) {
    uint32_t tenths = (us + 50u) / 100u;             // round to 0.1 ms
    uint32_t whole = tenths / 10u, frac = tenths % 10u;
    int n = 0;
    char tmp[8]; int t = 0;
    do { tmp[t++] = char('0' + whole % 10u); whole /= 10u; } while (whole && t < 7);
    while (t) buf[n++] = tmp[--t];
    buf[n++] = '.'; buf[n++] = char('0' + frac);
    return n;
}
} // namespace

void UI::profile_overlay(vec2 pos, const FrameProfile& p, const BitmapFont* font,
                         uint8_t layer) {
    // Layout: a dark panel, three phase bars scaled so the full bar width == the frame
    // budget (clamped at 2x so a spike stays on screen), a white tick at the budget line,
    // and an optional one-line ms readout underneath.
    constexpr int kBarW = 60, kBarH = 3, kGap = 1;
    const uint32_t budget = p.budget_us ? p.budget_us : 16667u;
    auto scaled = [&](uint32_t us) {
        uint32_t w = uint32_t(uint64_t(us) * kBarW / budget);
        return int(w > 2u * kBarW ? 2u * kBarW : w);
    };
    const int px = s_to_int(pos.x), py = s_to_int(pos.y);
    const int rows = 3;
    const int panel_h = rows * (kBarH + kGap) + kGap + (font ? font->line_h : 0);

    rect(UIRect{ pos, vec2{ s_from_int(kBarW * 2 + 4), s_from_int(panel_h + 2) } },
         rgba(20, 20, 30, 220), layer);

    const struct { uint32_t us; Rgba c; } rows_def[rows] = {
        { p.update_us,  rgba(90, 200, 90) },     // update  = green
        { p.render_us,  rgba(90, 140, 240) },    // render  = blue
        { p.present_us, rgba(230, 160, 70) },    // present = orange
    };
    for (int i = 0; i < rows; ++i) {
        const int y = py + 1 + kGap + i * (kBarH + kGap);
        rect(UIRect{ vec2{ s_from_int(px + 2), s_from_int(y) },
                     vec2{ s_from_int(scaled(rows_def[i].us) > 0 ? scaled(rows_def[i].us) : 1),
                           s_from_int(kBarH) } },
             rows_def[i].c, uint8_t(layer + 1));
    }
    // budget tick: a 1px white line where the bars hit 100% of the frame budget
    rect(UIRect{ vec2{ s_from_int(px + 2 + kBarW), s_from_int(py + 1) },
                 vec2{ s_from_int(1), s_from_int(rows * (kBarH + kGap) + kGap) } },
         rgba(255, 255, 255), uint8_t(layer + 2));

    if (font) {
        char line[48]; int n = 0;
        auto put = [&](const char* s) { while (*s && n < 46) line[n++] = *s++; };
        put("U ");  n += fmt_ms(line + n, p.update_us);
        put(" R "); n += fmt_ms(line + n, p.render_us);
        put(" P "); n += fmt_ms(line + n, p.present_us);
        put(" F "); n += fmt_ms(line + n, p.frame_us);
        line[n] = '\0';
        text(vec2{ s_from_int(px + 2), s_from_int(py + 1 + rows * (kBarH + kGap) + kGap) },
             *font, line, rgba(220, 220, 230), uint8_t(layer + 1));
    }
}

void UI::end() {
    prev_btn_count_ = btn_count_;
    if (prev_btn_count_ > 0 && focus_ >= prev_btn_count_) focus_ = prev_btn_count_ - 1;
    if (focus_ < 0) focus_ = 0;
}

} // namespace phx
