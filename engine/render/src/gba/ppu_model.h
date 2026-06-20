// engine/render/src/gba/ppu_model.h — a BEHAVIORAL model of the GBA Picture Processing
// Unit, plus a pure host-side compositor. This is how the native PPU backend is made
// verifiable without hardware: the backend builds this model (tiles, a text background,
// and OAM sprites in GBA-native terms), and `ppu_compose()` rasterizes it to RGBA8
// EXACTLY as the silicon would — 4bpp paletted tiles, colour index 0 = transparent,
// 15-bit BGR555 colour, scrolled text BG under 2D-mapped OBJ sprites.
//
// On real hardware (PHX_TARGET_GBA) the backend DMAs the same data to VRAM/OAM/PALRAM
// and the PPU does this rasterization in silicon; `ppu_compose` is the golden oracle the
// hardware path is defined against. Not a public header (internal to render/src/gba).
#ifndef PHX_RENDER_GBA_PPU_MODEL_H
#define PHX_RENDER_GBA_PPU_MODEL_H

#include "phx/core/types.h"
#include "phx/core/pixel.h"

namespace phx {
namespace gba {

constexpr int      kScreenW   = 240;   // GBA visible resolution
constexpr int      kScreenH   = 160;
constexpr int      kTile      = 8;     // a hardware tile is 8x8 texels
constexpr int      kPalSize   = 16;    // one 16-colour palette bank (4bpp); idx 0 = clear
constexpr uint16_t kObjMax    = 128;   // the PPU's OAM ceiling (an honest API limit)

// A 4bpp tile: 64 palette indices (0..15). Index 0 is transparent on the GBA.
struct PpuTile { uint8_t px[kTile * kTile]; };

// A text-BG screen entry: which tile + flip bits (single palette bank in this model).
struct PpuScreenEntry { uint16_t tile; uint8_t flip; };   // flip: bit0 = H, bit1 = V

// An OBJ (hardware sprite). Tiles are read in 2D mapping: a w_tiles x h_tiles rectangle
// out of a `stride`-wide tile grid starting at `tile` (matches an atlas sub-rect).
struct PpuObj {
    int16_t  x, y;                 // screen position of the top-left, in pixels
    uint16_t tile;                 // base tile index in the char store
    uint8_t  w_tiles, h_tiles;     // sprite size in tiles
    uint16_t stride;               // atlas width in tiles (for 2D tile addressing)
    uint8_t  flip;                 // bit0 = H, bit1 = V
};

// The whole modelled PPU state for one frame. Buffers are owned elsewhere (the backend
// allocates them from the render arena); this struct just points at them.
struct PpuState {
    const PpuTile*        tiles    = nullptr;   // shared char store (BG + OBJ)
    const uint16_t*       palette  = nullptr;   // kPalSize BGR555 entries; [0] = backdrop

    const PpuScreenEntry* bg_map   = nullptr;   // bg_w * bg_h entries (text background)
    uint16_t              bg_w = 0, bg_h = 0;    // background size in tiles
    int32_t               scroll_x = 0, scroll_y = 0;
    bool                  bg_enabled = false;

    const PpuObj*         oam      = nullptr;    // obj_count entries, painter order
    uint16_t              obj_count = 0;
};

// --- colour conversion (the GBA is 15-bit BGR555) -----------------------------------

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

// Rasterize the modelled PPU into an RGBA8 framebuffer (kScreenW x kScreenH). Pure:
// no allocation, no globals — exactly the pixels the GBA would scan out. `out` must hold
// kScreenW*kScreenH uint32_t.
inline void ppu_compose(const PpuState& s, uint32_t* out) {
    const uint32_t backdrop = s.palette ? bgr555_to_rgba8(s.palette[0]) : rgba(0, 0, 0);
    for (int i = 0; i < kScreenW * kScreenH; ++i) out[i] = backdrop;

    // --- text background (one BG layer), scrolled and wrapped like BG0 ---------------
    if (s.bg_enabled && s.bg_map && s.tiles && s.bg_w && s.bg_h) {
        const int bgpx = int(s.bg_w) * kTile;     // background size in pixels (wraps)
        const int bgpy = int(s.bg_h) * kTile;
        for (int py = 0; py < kScreenH; ++py) {
            int wy = py + s.scroll_y;
            wy %= bgpy; if (wy < 0) wy += bgpy;   // wrap
            const int ty = wy / kTile, iy = wy % kTile;
            for (int px = 0; px < kScreenW; ++px) {
                int wx = px + s.scroll_x;
                wx %= bgpx; if (wx < 0) wx += bgpx;
                const int tx = wx / kTile, ix = wx % kTile;
                const PpuScreenEntry e = s.bg_map[ty * s.bg_w + tx];
                const int sx = (e.flip & 1) ? (kTile - 1 - ix) : ix;
                const int sy = (e.flip & 2) ? (kTile - 1 - iy) : iy;
                const uint8_t idx = s.tiles[e.tile].px[sy * kTile + sx];
                if (idx == 0) continue;            // transparent -> backdrop shows
                out[py * kScreenW + px] = bgr555_to_rgba8(s.palette[idx & 0x0F]);
            }
        }
    }

    // --- OBJ sprites on top, painter order (later entries draw over earlier) ---------
    for (uint16_t o = 0; o < s.obj_count; ++o) {
        const PpuObj& ob = s.oam[o];
        const int w = ob.w_tiles * kTile, h = ob.h_tiles * kTile;
        for (int ry = 0; ry < h; ++ry) {
            const int py = ob.y + ry;
            if (py < 0 || py >= kScreenH) continue;
            const int sry = (ob.flip & 2) ? (h - 1 - ry) : ry;   // flipped row
            for (int rx = 0; rx < w; ++rx) {
                const int px = ob.x + rx;
                if (px < 0 || px >= kScreenW) continue;
                const int srx = (ob.flip & 1) ? (w - 1 - rx) : rx;
                // 2D tile addressing: which tile within the sprite, then texel in it.
                const int tcol = srx / kTile, trow = sry / kTile;
                const uint16_t tile = uint16_t(ob.tile + trow * ob.stride + tcol);
                const uint8_t idx = s.tiles[tile].px[(sry % kTile) * kTile + (srx % kTile)];
                if (idx == 0) continue;            // transparent texel
                out[py * kScreenW + px] = bgr555_to_rgba8(s.palette[idx & 0x0F]);
            }
        }
    }
}

} // namespace gba
} // namespace phx
#endif // PHX_RENDER_GBA_PPU_MODEL_H
