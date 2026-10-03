// tools/common/twk_geom.h — the tool widget kit's pure layout math: the Rect type, RectCut-style
// slicing, integer-upscale fitting and scrollbar geometry. No engine dependency, so headless
// document models (e.g. tools/phxstudio/model.h) share the exact same Rect as the GUI. Host-only.
#ifndef PHX_TOOLS_TWK_GEOM_H
#define PHX_TOOLS_TWK_GEOM_H

#include <algorithm>
#include <cstdint>

namespace twk {

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    bool empty() const { return w <= 0 || h <= 0; }
    int  right() const { return x + w; }
    int  bottom() const { return y + h; }
    Rect inset(int d) const { return Rect{ x + d, y + d, std::max(0, w - 2 * d), std::max(0, h - 2 * d) }; }
    Rect inset(int dx, int dy) const { return Rect{ x + dx, y + dy, std::max(0, w - 2 * dx), std::max(0, h - 2 * dy) }; }
    bool operator==(const Rect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
    bool operator!=(const Rect& o) const { return !(*this == o); }
};

inline Rect intersect(const Rect& a, const Rect& b) {
    const int x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
    const int x1 = std::min(a.right(), b.right()), y1 = std::min(a.bottom(), b.bottom());
    return Rect{ x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0) };
}

// "RectCut" layout: slice a strip off one edge of `r` (which shrinks) and return the strip.
inline Rect cut_left(Rect& r, int n)   { n = std::max(0, std::min(n, r.w)); Rect o{ r.x, r.y, n, r.h }; r.x += n; r.w -= n; return o; }
inline Rect cut_right(Rect& r, int n)  { n = std::max(0, std::min(n, r.w)); Rect o{ r.right() - n, r.y, n, r.h }; r.w -= n; return o; }
inline Rect cut_top(Rect& r, int n)    { n = std::max(0, std::min(n, r.h)); Rect o{ r.x, r.y, r.w, n }; r.y += n; r.h -= n; return o; }
inline Rect cut_bottom(Rect& r, int n) { n = std::max(0, std::min(n, r.h)); Rect o{ r.x, r.bottom() - n, r.w, n }; r.h -= n; return o; }

// Largest integer upscale of (w,h) that fits `box` (≥1), or a proportional downscale when even
// 1:1 overflows; centered.
inline Rect fit_rect(int w, int h, const Rect& box, int max_scale = 8) {
    if (w <= 0 || h <= 0 || box.w <= 0 || box.h <= 0) return Rect{ box.x, box.y, 0, 0 };
    int dw, dh;
    if (w <= box.w && h <= box.h) {
        int k = std::min(box.w / w, box.h / h);
        k = std::max(1, std::min(k, max_scale));
        dw = w * k; dh = h * k;
    } else if (int64_t(w) * box.h > int64_t(h) * box.w) { dw = box.w; dh = std::max(1, int(int64_t(h) * box.w / w)); }
    else                                                { dh = box.h; dw = std::max(1, int(int64_t(w) * box.h / h)); }
    return Rect{ box.x + (box.w - dw) / 2, box.y + (box.h - dh) / 2, dw, dh };
}

// ---- scrolling math (pure) ----
inline int clamp_scroll(int scroll, int count, int visible) {
    const int max_first = std::max(0, count - std::max(1, visible));
    return std::max(0, std::min(scroll, max_first));
}
inline void scroll_thumb(int count, int visible, int scroll, int track, int& pos, int& len) {
    if (count <= visible || count <= 0) { pos = 0; len = track; return; }
    len = std::max(6, int(int64_t(track) * visible / count));
    const int span = track - len;
    const int max_first = count - visible;
    pos = max_first > 0 ? int(int64_t(span) * clamp_scroll(scroll, count, visible) / max_first) : 0;
}
inline int scroll_from_track(int count, int visible, int track, int offset) {
    if (count <= visible || track <= 0) return 0;
    int pos = 0, len = 0;
    scroll_thumb(count, visible, 0, track, pos, len);
    const int span = std::max(1, track - len);
    const int p = std::max(0, std::min(span, offset - len / 2));
    return clamp_scroll(int((int64_t(p) * (count - visible) + span / 2) / span), count, visible);
}

} // namespace twk
#endif // PHX_TOOLS_TWK_GEOM_H
