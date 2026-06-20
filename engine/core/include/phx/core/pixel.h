// phx/core/pixel.h — fundamental pixel types shared by render and resource (so neither
// depends on the other). RGBA is memory order [R,G,B,A]: value = R | G<<8 | B<<16 | A<<24.
#ifndef PHX_CORE_PIXEL_H
#define PHX_CORE_PIXEL_H

#include "phx/core/types.h"

namespace phx {

using Rgba = uint32_t;

constexpr Rgba rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return Rgba(r) | (Rgba(g) << 8) | (Rgba(b) << 16) | (Rgba(a) << 24);
}
constexpr uint8_t rgba_r(Rgba c) { return uint8_t(c & 0xFF); }
constexpr uint8_t rgba_g(Rgba c) { return uint8_t((c >> 8) & 0xFF); }
constexpr uint8_t rgba_b(Rgba c) { return uint8_t((c >> 16) & 0xFF); }
constexpr uint8_t rgba_a(Rgba c) { return uint8_t((c >> 24) & 0xFF); }

enum class PixelFormat : uint8_t { RGBA8 = 0, PAL8 = 1 };   // PAL8 = paletted (GBA/PSP tiers)

} // namespace phx
#endif // PHX_CORE_PIXEL_H
