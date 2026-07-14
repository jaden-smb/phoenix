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

// Texture pixel encodings. RGBA8 is the universal interchange format; the paletted and
// swizzled forms are what the per-target bake (tools/phxpack, docs/06 §4) actually ships:
//   PAL4_TILES — tier 0 (GBA): 4bpp 8×8 tiles indexing 16-colour BGR555 palettes, the PPU's
//                native layout (see phx/resource/bundle.h for the exact blob layout).
//   RGBA8_SWZ  — tier 1 (PSP): RGBA8 texels in the GU's swizzled block order (16-byte × 8-row
//                blocks), sampled zero-copy by the GU with the swizzle bit set.
// PAL8 is reserved (no producer or consumer yet).
enum class PixelFormat : uint8_t { RGBA8 = 0, PAL8 = 1, PAL4_TILES = 2, RGBA8_SWZ = 3 };

// ---- 15-bit BGR555 (the GBA's colour space; also used by PAL4_TILES palettes) ----

// Pack an RGBA8 texel (R | G<<8 | B<<16 | A<<24) to BGR555 (R | G<<5 | B<<10).
inline uint16_t rgba8_to_bgr555(uint32_t c) {
    uint32_t r = (c        & 0xFF) >> 3;
    uint32_t g = ((c >> 8)  & 0xFF) >> 3;
    uint32_t b = ((c >> 16) & 0xFF) >> 3;
    return uint16_t(r | (g << 5) | (b << 10));
}

// Expand a BGR555 colour back to an RGBA8 texel (5-bit -> 8-bit, replicating high bits).
inline uint32_t bgr555_to_rgba8(uint16_t c) {
    uint32_t r5 =  c        & 0x1F;
    uint32_t g5 = (c >> 5)  & 0x1F;
    uint32_t b5 = (c >> 10) & 0x1F;
    uint8_t  r  = uint8_t((r5 << 3) | (r5 >> 2));
    uint8_t  g  = uint8_t((g5 << 3) | (g5 >> 2));
    uint8_t  b  = uint8_t((b5 << 3) | (b5 >> 2));
    return rgba(r, g, b);
}

// ---- PAL4_TILES payload layout (shared by the offline bake, resource views and render) ----
//
// The pixel data that follows a texture's blob header when format == PAL4_TILES — the
// offline mirror of the GBA PPU upload quantizer (see tools/phxpack/tex_encode.h for the
// encoder, engine/render/src/gba for the consumer):
//
//   TexturePal4Header               { pal_count }
//   uint16 palettes[pal_count][16]  BGR555; slot 0 is always 0 (transparent), colours at 1..15
//   uint8  pal_used[pal_count]      colours interned per palette (0..15)
//   uint8  tile_pal[n_tiles]        palette index per tile (n_tiles = (w/8)*(h/8), row-major)
//   <pad to a 4-byte boundary from the payload start>
//   uint32 tile_rows[n_tiles*8]     packed 4bpp rows, VRAM order: one uint32 per row, texel x
//                                   at bits [x*4, x*4+4) — nibble 0 = transparent
struct TexturePal4Header {
    uint16_t pal_count;    // 16-colour palettes that follow (>= 1)
    uint16_t reserved;
};
static_assert(sizeof(TexturePal4Header) == 4, "TexturePal4Header layout changed");

constexpr uint32_t kPal4PalBytes  = 16 * 2;   // one 16-colour BGR555 palette
constexpr uint32_t kPal4TileBytes = 32;       // one packed 4bpp 8×8 tile

inline uint32_t pal4_tile_count(uint16_t w, uint16_t h) {
    return (uint32_t(w) / 8u) * (uint32_t(h) / 8u);
}
// Byte offsets into the payload (== the bytes right after the texture blob header).
inline uint32_t pal4_palettes_off()                  { return uint32_t(sizeof(TexturePal4Header)); }
inline uint32_t pal4_pal_used_off(uint16_t pal_count) {
    return pal4_palettes_off() + uint32_t(pal_count) * kPal4PalBytes;
}
inline uint32_t pal4_tile_pal_off(uint16_t pal_count) {
    return pal4_pal_used_off(pal_count) + pal_count;
}
inline uint32_t pal4_tile_data_off(uint16_t pal_count, uint32_t n_tiles) {
    return (pal4_tile_pal_off(pal_count) + n_tiles + 3u) & ~3u;
}
inline uint32_t pal4_payload_size(uint16_t pal_count, uint32_t n_tiles) {
    return pal4_tile_data_off(pal_count, n_tiles) + n_tiles * kPal4TileBytes;
}
// Texel of tile `tile` at (x, y), 0..15 (0 = transparent). `tile_data` = payload + tile_data_off.
inline uint8_t pal4_texel(const uint8_t* tile_data, uint32_t tile, uint32_t x, uint32_t y) {
    const uint8_t b = tile_data[tile * kPal4TileBytes + y * 4u + (x >> 1)];
    return (x & 1u) ? uint8_t(b >> 4) : uint8_t(b & 0xF);
}

// ---- RGBA8_SWZ layout (PSP GU swizzle; shared the same way) ----
//
// width*height*4 bytes: RGBA8 texels reordered into the GU's swizzled layout — the image is
// split into 16-byte-wide × 8-row blocks (4×8 texels at 32bpp) stored contiguously, row-major
// by block. A pure reorder: texel VALUES are untouched, so sampling swizzled vs linear is
// pixel-identical (the GU backend stays bit-exact vs the software golden reference).
inline bool swz_size_ok(uint16_t w, uint16_t h) {
    return w != 0 && h != 0 && (w % 4u) == 0 && (h % 8u) == 0;
}
// Index (in texels) of (x, y) within a swizzled image of width `w` (texels).
inline uint32_t swz_texel_index(uint32_t x, uint32_t y, uint32_t w) {
    const uint32_t block = (y >> 3) * (w >> 2) + (x >> 2);
    return block * 32u + ((y & 7u) << 2) + (x & 3u);
}

} // namespace phx
#endif // PHX_CORE_PIXEL_H
