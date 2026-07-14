// engine/render/src/gu/gu_model.h — a BEHAVIORAL model of the PSP Graphics Unit's 2D path,
// plus a pure host-side compositor. Same verification strategy as the GBA PPU model
// (engine/render/src/gba/ppu_model.h): the backend records the frame as GU sprite quads
// (textured, nearest-sampled, alpha-tested, tinted) and `gu_compose()` rasterizes them to
// RGBA8 exactly as the GU would scan them out — so the whole native PSP render path is
// verified HEADLESSLY, through the same Renderer front end the software backend uses.
//
// The PSP GU is a full-colour textured-triangle GPU (no palette quantization, unlike the
// GBA PPU), so 2D rendering is a list of texture-mapped GU_SPRITES quads. This model's
// rasterization is bit-identical to the software reference's blit (same nearest source
// stepping, alpha-0 skip, and per-channel tint), which makes the soft golden images the
// GU backend's exact oracle. On real hardware (PHX_TARGET_PSP) the backend builds the same
// quads into a sceGu display list instead; `gu_compose` is that path's definition of right.
// Not a public header (internal to render/src/gu).
#ifndef PHX_RENDER_GU_MODEL_H
#define PHX_RENDER_GU_MODEL_H

#include "phx/core/types.h"
#include "phx/core/pixel.h"

namespace phx {
namespace gu {

constexpr int kScreenW = 480;   // PSP native resolution (the model composes any fb size)
constexpr int kScreenH = 272;

// A bound texture: full RGBA8 pixels (the PSP GU samples 32-bit textures directly).
// `swz` marks the tier-1 bake's swizzled block layout (phx/core/pixel.h): on hardware the
// GU samples it zero-copy with the swizzle bit set; the model computes the same addresses.
// A pure reorder — texel values are identical, so the soft goldens still apply exactly.
struct GuTexRef { const uint32_t* px; int32_t w, h; uint8_t swz = 0; };

inline uint32_t gu_texel(const GuTexRef& t, int x, int y) {
    return t.swz ? t.px[swz_texel_index(uint32_t(x), uint32_t(y), uint32_t(t.w))]
                 : t.px[y * t.w + x];
}

// One textured sprite quad: a source rect sampled (nearest) into a screen dest rect, with
// optional flip and a per-channel modulate (vertex colour). Dest is already camera-adjusted.
struct GuQuad {
    uint16_t tex;
    int16_t  sx, sy, sw, sh;     // source rect in the texture (texels)
    int16_t  dx, dy, dw, dh;     // screen destination rect (pixels)
    uint8_t  flip;               // bit0 = H, bit1 = V
    Rgba     tint;               // GU vertex colour modulate; white = unchanged
};

struct GuState {
    const GuTexRef* tex   = nullptr;  uint16_t tex_count  = 0;
    const GuQuad*   quads = nullptr;  uint32_t quad_count = 0;
    Rgba            clear = rgba(30, 30, 46);
    int             w = kScreenW, h = kScreenH;
};

// Rasterize the GU quad list into an RGBA8 framebuffer (s.w x s.h). Pure: no allocation,
// no globals. Bit-identical to the software backend's blit so its golden images apply.
inline void gu_compose(const GuState& s, uint32_t* out) {
    for (int i = 0; i < s.w * s.h; ++i) out[i] = s.clear;

    for (uint32_t q = 0; q < s.quad_count; ++q) {
        const GuQuad& d = s.quads[q];
        if (d.tex >= s.tex_count) continue;
        const GuTexRef& t = s.tex[d.tex];
        if (!t.px) continue;
        const int dw = d.dw > 0 ? d.dw : d.sw;
        const int dh = d.dh > 0 ? d.dh : d.sh;
        if (dw <= 0 || dh <= 0 || d.sw <= 0 || d.sh <= 0) continue;
        const bool fx  = (d.flip & 1) != 0, fy = (d.flip & 2) != 0;
        const bool mod = (d.tint != rgba(255, 255, 255));

        for (int y = 0; y < dh; ++y) {
            const int py = d.dy + y;
            if (py < 0 || py >= s.h) continue;
            const int soy  = (dh == d.sh) ? y : (y * d.sh / dh);     // nearest source row
            const int srcY = fy ? (d.sy + d.sh - 1 - soy) : (d.sy + soy);
            if (srcY < 0 || srcY >= t.h) continue;
            for (int x = 0; x < dw; ++x) {
                const int px = d.dx + x;
                if (px < 0 || px >= s.w) continue;
                const int sox  = (dw == d.sw) ? x : (x * d.sw / dw); // nearest source col
                const int srcX = fx ? (d.sx + d.sw - 1 - sox) : (d.sx + sox);
                if (srcX < 0 || srcX >= t.w) continue;
                uint32_t texel = gu_texel(t, srcX, srcY);
                if ((texel >> 24) == 0) continue;                    // alpha 0 -> skip
                if (mod) {
                    uint32_t r = uint32_t(rgba_r(texel)) * rgba_r(d.tint) / 255;
                    uint32_t g = uint32_t(rgba_g(texel)) * rgba_g(d.tint) / 255;
                    uint32_t b = uint32_t(rgba_b(texel)) * rgba_b(d.tint) / 255;
                    texel = rgba(uint8_t(r), uint8_t(g), uint8_t(b), rgba_a(texel));
                }
                out[py * s.w + px] = texel;
            }
        }
    }
}

} // namespace gu
} // namespace phx
#endif // PHX_RENDER_GU_MODEL_H
