// engine/render/src/gba/ppu_model.h — a BEHAVIORAL model of the GBA Picture Processing
// Unit, plus a pure host-side compositor. This is how the native PPU backend is made
// verifiable without hardware: the backend builds this model (4bpp packed tiles, up to
// FOUR text backgrounds streamed through 32×32 screenblock windows, 16×16-colour palette
// banks, and OAM sprites in GBA-native terms), and `ppu_compose()` rasterizes it to RGBA8
// EXACTLY as the silicon would — colour index 0 = transparent, 15-bit BGR555, text BGs
// back-to-front under 1D-mapped OBJ sprites.
//
// On real hardware (PHX_TARGET_GBA + PHX_GBA_HW) the backend DMAs the same data to
// VRAM/OAM/PALRAM and the PPU does this rasterization in silicon; `ppu_compose` is the
// golden oracle the hardware path is defined against. Everything the model accepts must be
// expressible on the silicon (OBJ shapes, per-OBJ single palette bank, 10-bit tile indices),
// so a frame that composes here is a frame the GBA can show.
// Not a public header (internal to render/src/gba).
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
constexpr int      kPalBanks  = 16;    // BG palette RAM = 16 banks of 16 (OBJ mirrors them)
constexpr int      kPalTotal  = kPalSize * kPalBanks;
constexpr uint16_t kObjMax    = 128;   // the PPU's OAM ceiling (an honest API limit)
constexpr int      kBgSlots   = 4;     // the PPU's four text backgrounds
constexpr int      kBgWin     = 32;    // one screenblock = 32×32 cells (256×256 px, wraps)

// A 4bpp tile, packed exactly as VRAM stores it: 8 words, one row each, low nibble =
// leftmost texel. Nibble 0 is transparent on the GBA.
struct PpuTile { uint32_t row[kTile]; };

inline uint8_t ppu_tile_px(const PpuTile& t, int x, int y) {
    return uint8_t((t.row[y] >> (x * 4)) & 0xF);
}
inline void ppu_tile_set(PpuTile& t, int x, int y, uint8_t idx) {
    const uint32_t sh = uint32_t(x * 4);
    t.row[y] = (t.row[y] & ~(0xFu << sh)) | (uint32_t(idx & 0xF) << sh);
}

// A text-BG screen entry: tile + flip bits + the 16-colour palette bank it indexes —
// exactly the hardware's tile|hflip<<10|vflip<<11|bank<<12 halfword, kept unpacked here.
struct PpuScreenEntry { uint16_t tile; uint8_t flip; uint8_t bank; };

// One modelled text background: a 32×32 screenblock WINDOW the backend streams the visible
// part of an arbitrarily large tilemap into, plus the raw scroll. The window wraps at
// 256 px just like the silicon; cells the screen cannot sample are simply stale.
struct PpuBg {
    const PpuScreenEntry* win = nullptr;   // kBgWin * kBgWin entries
    int32_t scroll_x = 0, scroll_y = 0;
    bool    enabled = false;
};

// An OBJ (hardware sprite). Tiles are 1D-mapped: a w_tiles×h_tiles rectangle stored
// row-major CONTIGUOUSLY at `tile` in the OBJ char store (the backend copies atlas tiles
// into a per-frame run, so this always matches what OAM's 1D mapping can address).
struct PpuObj {
    int16_t  x, y;                 // screen position of the top-left, in pixels
    uint16_t tile;                 // base tile index into the OBJ tile run
    uint8_t  w_tiles, h_tiles;     // sprite size in tiles (a valid hardware shape)
    uint8_t  flip;                 // bit0 = H, bit1 = V
    uint8_t  bank;                 // 16-colour palette bank (one per OBJ, like OAM attr2)
};

// The whole modelled PPU state for one frame. Buffers are owned elsewhere (the backend
// allocates them from the render arena); this struct just points at them.
struct PpuState {
    const PpuTile*  tiles     = nullptr;   // BG char store (screen entries index it)
    const PpuTile*  obj_tiles = nullptr;   // per-frame OBJ tile run (OAM indexes it)
    const uint16_t* palette   = nullptr;   // kPalTotal BGR555 entries; [0] = backdrop

    PpuBg           bg[kBgSlots];          // slot 0 = furthest back … 3 = front
    const PpuObj*   oam       = nullptr;   // obj_count entries, painter order
    uint16_t        obj_count = 0;
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

    // --- text backgrounds, back (slot 0) to front (slot 3), wrapping at 256 px ---------
    constexpr int kBgPx = kBgWin * kTile;   // 256
    for (int b = 0; b < kBgSlots; ++b) {
        const PpuBg& bg = s.bg[b];
        if (!bg.enabled || !bg.win || !s.tiles) continue;
        for (int py = 0; py < kScreenH; ++py) {
            int wy = (py + bg.scroll_y) % kBgPx; if (wy < 0) wy += kBgPx;
            const int ty = wy / kTile, iy = wy % kTile;
            for (int px = 0; px < kScreenW; ++px) {
                int wx = (px + bg.scroll_x) % kBgPx; if (wx < 0) wx += kBgPx;
                const int tx = wx / kTile, ix = wx % kTile;
                const PpuScreenEntry e = bg.win[ty * kBgWin + tx];
                const int sx = (e.flip & 1) ? (kTile - 1 - ix) : ix;
                const int sy = (e.flip & 2) ? (kTile - 1 - iy) : iy;
                const uint8_t idx = ppu_tile_px(s.tiles[e.tile], sx, sy);
                if (idx == 0) continue;            // transparent -> lower layer shows
                out[py * kScreenW + px] =
                    bgr555_to_rgba8(s.palette[(e.bank & 0xF) * kPalSize + idx]);
            }
        }
    }

    // --- OBJ sprites on top, painter order (later entries draw over earlier) ---------
    for (uint16_t o = 0; o < s.obj_count; ++o) {
        const PpuObj& ob = s.oam[o];
        if (!s.obj_tiles) break;
        const int w = ob.w_tiles * kTile, h = ob.h_tiles * kTile;
        for (int ry = 0; ry < h; ++ry) {
            const int py = ob.y + ry;
            if (py < 0 || py >= kScreenH) continue;
            const int sry = (ob.flip & 2) ? (h - 1 - ry) : ry;   // flipped row
            for (int rx = 0; rx < w; ++rx) {
                const int px = ob.x + rx;
                if (px < 0 || px >= kScreenW) continue;
                const int srx = (ob.flip & 1) ? (w - 1 - rx) : rx;
                // 1D mapping: tiles run row-major, contiguous within the sprite.
                const int tcol = srx / kTile, trow = sry / kTile;
                const uint16_t tile = uint16_t(ob.tile + trow * ob.w_tiles + tcol);
                const uint8_t idx = ppu_tile_px(s.obj_tiles[tile], srx % kTile, sry % kTile);
                if (idx == 0) continue;            // transparent texel
                out[py * kScreenW + px] =
                    bgr555_to_rgba8(s.palette[(ob.bank & 0xF) * kPalSize + idx]);
            }
        }
    }
}

} // namespace gba
} // namespace phx
#endif // PHX_RENDER_GBA_PPU_MODEL_H
