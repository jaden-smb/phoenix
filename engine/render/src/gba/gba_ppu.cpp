// engine/render/src/gba/gba_ppu.cpp — the GBA-native render backend (render tier 0, PPU).
// Instead of CPU-rasterizing arbitrary 24-bit sprites like the software reference, this
// backend speaks the GBA's actual hardware language: it quantizes each RGBA8 atlas into
// 4bpp paletted 8x8 tiles sharing one 16-colour BGR555 palette, turns tilemaps into text-
// BG screen entries, and turns sprites into OAM (OBJ) entries — honouring the real machine
// limits (≤16 palette colours, 8x8 tile alignment, the 128-sprite ceiling). It then runs
// `ppu_compose` (engine/render/src/gba/ppu_model.h) to produce the exact frame the PPU
// would scan out, into the platform's RGBA8 framebuffer.
//
// Why this shape: the GBA-native visual model is fully exercised and verified HEADLESSLY
// (every constraint that would bite on hardware — palette overflow, unaligned art, too
// many sprites — surfaces here, on the host, in a test). Composing on the CPU also lets
// this backend run today on ANY software-tier platform (null/sdl, and the GBA itself via
// the Mode-3 blit). The remaining hardware step is to DMA this same tile/map/OAM/palette
// data into VRAM/OAM/PALRAM and let the silicon PPU scan it out (see end()'s note); that
// is an optimization, not a correctness change — `ppu_compose` is its golden oracle.
//
// Exactly one render backend TU is linked; this one defines phx_make_render_backend()
// (selected at link time instead of soft/gl/gu).
#include "backend.h"
#include "phx/platform/gfx_soft.h"
#include "ppu_model.h"

#if defined(PHX_GBA_HW)
#include <stdint.h>
// Platform hook (engine/platform/src/gba): take over the display so present() skips the Mode-3
// software blit and lets the PPU scan our VRAM/OAM/PALRAM out directly.
extern "C" void phx_gba_set_direct(int on);
#endif

namespace phx {
namespace {

using namespace phx::gba;

constexpr uint16_t kMaxTextures = 256;
constexpr uint16_t kMaxTilemaps = 32;
// char-store capacity (tiles): 256 4bpp tiles = 8 KB, fits BG charblock 0 (0x06000000) with the
// map screenblock above it. The text BG is modelled (and submitted to hardware) as 32x32 cells —
// the GBA's base text-BG size — so the map store is one screenblock. Both are sized to keep the
// backend's arena footprint small enough for the GBA's 256 KB EWRAM (the platformer-PPU ROM).
constexpr uint16_t kTileCap     = 256;           // char-store capacity (tiles) -> 16 KB tiles_
constexpr uint16_t kMapCap      = 32 * 32;       // max BG screen entries we model -> one screenblock

// A quantized texture: a contiguous run of tiles in the char store, addressed in 2D as a
// `cols` x `rows` grid (so a sprite/tile source sub-rect maps to a tile rectangle).
struct TexRec { uint16_t base, cols, rows; bool used; };
struct MapRec {
    const uint16_t* idx;
    uint16_t w, h;
    uint8_t  layers, tw, th;
    TextureId tileset;
    int32_t  scroll_x, scroll_y;
};

class GbaPpuBackend final : public IRenderBackend {
public:
    void init(phx_gfx* gfx, ArenaAllocator& a, const Caps& caps) {
        gfx_     = gfx;
        tiles_   = a.alloc_array<PpuTile>(kTileCap);
        tex_     = a.alloc_array<TexRec>(kMaxTextures);
        free_    = a.alloc_array<TextureId>(kMaxTextures);
        maps_    = a.alloc_array<MapRec>(kMaxTilemaps);
        bg_map_  = a.alloc_array<PpuScreenEntry>(kMapCap);
        oam_     = a.alloc_array<PpuObj>(kObjMax);
#if !defined(PHX_GBA_HW)
        scratch_ = a.alloc_array<uint32_t>(kScreenW * kScreenH);   // compose target (host/sw tier)
#endif

        // Tile 0 is the reserved fully-transparent tile (BG "empty" cells point here).
        for (auto& p : tiles_[0].px) p = 0;
        tile_count_ = 1;

        // Palette index 0 is the backdrop AND the per-tile transparent colour. Match the
        // software reference's clear colour so headless golden images line up.
        palette_[0] = rgba8_to_bgr555(rgba(30, 30, 46));
        pal_count_  = 1;

        // The hardware OBJ ceiling is 128; respect a tighter caps budget if one is set.
        obj_limit_ = caps.max_sprites && caps.max_sprites < kObjMax ? uint16_t(caps.max_sprites)
                                                                    : kObjMax;
#if defined(PHX_GBA_HW)
        phx_gba_set_direct(1);   // we own the display; the platform must not Mode-3-blit over VRAM
#endif
    }

    // Quantize an RGBA8 atlas into 4bpp tiles + the shared 16-colour palette. Fails (kNo-
    // Texture) on the honest GBA constraints: non-RGBA8, non-8px-aligned, char-store full,
    // or a >16-colour palette — exactly what would make the art unshippable on hardware.
    TextureId upload_tex(const TextureDesc& d) override {
        if (d.format != PixelFormat::RGBA8 || !d.pixels)      return kNoTexture;
        if (d.width == 0 || d.height == 0)                    return kNoTexture;
        if ((d.width % kTile) || (d.height % kTile))          return kNoTexture;  // tile-align
        const uint16_t cols = uint16_t(d.width  / kTile);
        const uint16_t rows = uint16_t(d.height / kTile);
        const uint32_t need = uint32_t(cols) * rows;
        if (tile_count_ + need > kTileCap)                    return kNoTexture;  // char store full

        const uint16_t base = tile_count_;
        const uint32_t* src = static_cast<const uint32_t*>(d.pixels);
        for (uint16_t tr = 0; tr < rows; ++tr) {
            for (uint16_t tc = 0; tc < cols; ++tc) {
                PpuTile& t = tiles_[base + tr * cols + tc];
                for (int y = 0; y < kTile; ++y) {
                    for (int x = 0; x < kTile; ++x) {
                        uint32_t texel = src[(tr * kTile + y) * d.width + (tc * kTile + x)];
                        int idx;
                        if ((texel >> 24) == 0) idx = 0;             // alpha 0 -> transparent
                        else if (!intern_colour(texel, idx))         // palette overflow
                            return kNoTexture;
                        t.px[y * kTile + x] = uint8_t(idx);
                    }
                }
            }
        }
        tile_count_ += uint16_t(need);

        TextureId id;
        if (free_n_ > 0)                    id = free_[--free_n_];
        else if (tex_n_ < kMaxTextures)    id = tex_n_++;
        else                               return kNoTexture;
        tex_[id] = TexRec{ base, cols, rows, true };
        return id;
    }

    void free_tex(TextureId id) override {
        if (id >= tex_n_ || !tex_[id].used) return;  // out of range / double free
        tex_[id].used = false;                       // (tiles are not reclaimed; ids are)
        free_[free_n_++] = id;
    }

    TilemapId upload_map(const TilemapDesc& d) override {
        if (map_n_ >= kMaxTilemaps) return kNoTilemap;
        TilemapId id = map_n_++;
        maps_[id] = MapRec{ d.indices, d.width, d.height, d.layers, d.tile_w, d.tile_h,
                            d.tileset, 0, 0 };
        return id;
    }

    void set_scroll(TilemapId id, vec2 px) override {
        if (id >= map_n_) return;
        maps_[id].scroll_x = s_to_int(px.x);
        maps_[id].scroll_y = s_to_int(px.y);
    }

    void begin(const Camera2D& cam) override {
        // Camera pan maps to free BG/OBJ scroll (the GBA's superpower). Camera ZOOM is
        // deliberately NOT applied: a text-BG + plain OBJ can't scale, so zoom would need the
        // affine BG/OBJ path (Mode-1/2 + REG_BGxPA.., a per-scanline matrix) — the opt-in 2.5D
        // route in docs/03 §4, out of the MVP. The PPU renders 1:1; cam.zoom is ignored here.
        cam_x_  = s_to_int(cam.pos.x);
        cam_y_  = s_to_int(cam.pos.y);
        obj_n_  = 0;
        st_.bg_enabled = false;
        stats_  = RenderStats{};
    }

    // A text-BG layer -> screen entries. The PPU's BG tiles are 8x8; tile_w/h must be 8
    // (an honest hardware limit). Cell value is tileIndex+1 (0 = empty -> transparent
    // tile 0). Only one BG layer is modelled (GBA has 4; multi-BG parallax is follow-up).
    void draw_tilemap(TilemapId id, uint8_t layer) override {
        if (id >= map_n_) return;
        const MapRec& m = maps_[id];
        if (layer >= m.layers || m.tw != kTile || m.th != kTile) return;
        if (m.tileset >= tex_n_ || !tex_[m.tileset].used)        return;
        if (uint32_t(m.w) * m.h > kMapCap)                       return;
        const TexRec& ts = tex_[m.tileset];
        const uint16_t* cells = m.idx + size_t(layer) * size_t(m.w) * size_t(m.h);

        for (uint32_t i = 0; i < uint32_t(m.w) * m.h; ++i) {
            uint16_t c = cells[i];
            if (c == 0) { bg_map_[i] = PpuScreenEntry{ 0, 0 }; continue; }  // empty -> clear
            uint16_t tindex = uint16_t(c - 1);
            if (tindex >= ts.cols * ts.rows) { bg_map_[i] = PpuScreenEntry{ 0, 0 }; continue; }
            bg_map_[i] = PpuScreenEntry{ uint16_t(ts.base + tindex), 0 };
            ++stats_.tiles_drawn;
        }
        st_.bg_map     = bg_map_;
        st_.bg_w       = m.w;
        st_.bg_h       = m.h;
        st_.scroll_x   = m.scroll_x + cam_x_;
        st_.scroll_y   = m.scroll_y + cam_y_;
        st_.bg_enabled = true;
    }

    // Sprites -> OAM (OBJ). Source rect must be 8px-aligned; dest scaling and per-channel
    // tint are not expressible on plain OBJ (would need affine / there is no RGB tint on
    // the GBA), so they are ignored here — honest hardware behaviour. Beyond the OBJ ceiling
    // sprites are dropped and counted, matching the front-end's graceful-overflow contract.
    void submit_sprites(const DrawSprite* s, uint32_t n) override {
        for (uint32_t i = 0; i < n; ++i) {
            const DrawSprite& d = s[i];
            if (d.tex >= tex_n_ || !tex_[d.tex].used)        continue;
            if ((d.sw % kTile) || (d.sh % kTile) || d.sw <= 0 || d.sh <= 0) continue;
            if (obj_n_ >= obj_limit_) { ++stats_.sprites_dropped; continue; }
            const TexRec& t = tex_[d.tex];
            const uint16_t tc = uint16_t(d.sx / kTile), tr = uint16_t(d.sy / kTile);
            PpuObj& o = oam_[obj_n_++];
            o.x       = int16_t(s_to_int(d.pos.x) - cam_x_);
            o.y       = int16_t(s_to_int(d.pos.y) - cam_y_);
            o.tile    = uint16_t(t.base + tr * t.cols + tc);
            o.w_tiles = uint8_t(d.sw / kTile);
            o.h_tiles = uint8_t(d.sh / kTile);
            o.stride  = t.cols;
            o.flip    = uint8_t(((d.flags & kFlipX) ? 1 : 0) | ((d.flags & kFlipY) ? 2 : 0));
        }
        ++stats_.batches;
    }

    void end() override {
        st_.tiles     = tiles_;
        st_.palette   = palette_;
        st_.oam       = oam_;
        st_.obj_count = obj_n_;

#if defined(PHX_GBA_HW)
        // Real hardware: program the PPU (tiles->VRAM, palette->PALRAM, map->screenblock,
        // OBJ->OAM, Mode-0 DISPCNT) and let the silicon scan it out. `ppu_compose` (the else
        // branch) is the exact golden definition of the frame this must produce.
        submit_hardware();
#else
        // Headless / software-tier present: compose exactly what the PPU would scan out.
        ppu_compose(st_, scratch_);

        // Blit the 240x160 PPU frame into the platform framebuffer (clipped if smaller).
        phx_soft_fb fb = phx_gfx_soft_lock(gfx_);
        if (fb.pixels) {
            const int cw = fb.w < kScreenW ? fb.w : kScreenW;
            const int ch = fb.h < kScreenH ? fb.h : kScreenH;
            for (int y = 0; y < ch; ++y)
                for (int x = 0; x < cw; ++x)
                    fb.pixels[y * fb.w + x] = scratch_[y * kScreenW + x];
        }
#endif
    }

    RenderStats& stats() override { return stats_; }

private:
#if defined(PHX_GBA_HW)
    // Push the built model into the PPU's memory-mapped state. VRAM/PALRAM/OAM forbid 8-bit
    // writes, so everything goes out as 16-/32-bit units. Layout: BG charblock 0 @0x06000000
    // holds the 4bpp tiles; the same tiles are duplicated into OBJ tile VRAM @0x06010000
    // (OBJ addressing is separate); the text BG map lives in screenblock 24 @0x0600C000 (well
    // clear of the tile data for our scenes); one shared 16-colour palette goes to both the BG
    // and OBJ palette banks (the model uses a single palette for tiles + sprites).
    void submit_hardware() {
        volatile uint16_t* const PAL_BG   = reinterpret_cast<volatile uint16_t*>(0x05000000);
        volatile uint16_t* const PAL_OBJ  = reinterpret_cast<volatile uint16_t*>(0x05000200);
        volatile uint32_t* const CHAR_BG  = reinterpret_cast<volatile uint32_t*>(0x06000000);
        volatile uint32_t* const CHAR_OBJ = reinterpret_cast<volatile uint32_t*>(0x06010000);
        volatile uint16_t* const SCRBLK   = reinterpret_cast<volatile uint16_t*>(0x0600C000);
        volatile uint16_t* const OAM      = reinterpret_cast<volatile uint16_t*>(0x07000000);
        volatile uint16_t* const DISPCNT  = reinterpret_cast<volatile uint16_t*>(0x04000000);
        volatile uint16_t* const BG0CNT   = reinterpret_cast<volatile uint16_t*>(0x04000008);
        volatile uint16_t* const BG0HOFS  = reinterpret_cast<volatile uint16_t*>(0x04000010);
        volatile uint16_t* const BG0VOFS  = reinterpret_cast<volatile uint16_t*>(0x04000012);
        constexpr uint16_t kScreenblock = 24;     // 0x0600C000 = VRAM base + 24*0x800

        // palette -> BG bank 0 + OBJ bank 0
        for (int i = 0; i < kPalSize; ++i) { PAL_BG[i] = palette_[i]; PAL_OBJ[i] = palette_[i]; }

        // tiles -> BG charblock 0 AND OBJ tile VRAM, packed 4bpp (2 px/byte, low nibble = left)
        for (uint16_t t = 0; t < tile_count_; ++t) {
            const uint8_t* px = tiles_[t].px;
            for (int r = 0; r < kTile; ++r) {
                uint32_t row = 0;
                for (int c = 0; c < kTile; ++c) row |= uint32_t(px[r * kTile + c] & 0xF) << (c * 4);
                CHAR_BG [t * 8 + r] = row;        // 8 words = 32 bytes per 4bpp tile
                CHAR_OBJ[t * 8 + r] = row;
            }
        }

        // text BG map -> screenblock (32x32). Clear first; our scenes fit within 32x32 cells.
        for (int i = 0; i < 32 * 32; ++i) SCRBLK[i] = 0;
        if (st_.bg_enabled && bg_map_) {
            const int w = st_.bg_w < 32 ? st_.bg_w : 32;
            const int h = st_.bg_h < 32 ? st_.bg_h : 32;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const PpuScreenEntry& e = bg_map_[y * st_.bg_w + x];
                    uint16_t se = uint16_t(e.tile & 0x3FF);
                    if (e.flip & 1) se |= 1u << 10;          // H flip
                    if (e.flip & 2) se |= 1u << 11;          // V flip
                    SCRBLK[y * 32 + x] = se;                 // palbank 0
                }
        }

        // OBJ -> OAM (4 halfwords/entry; 4th is affine, unused). Hide all, then emit ours.
        for (int i = 0; i < kObjMax; ++i) OAM[i * 4 + 0] = 0x0200;   // attr0 bit9 = disabled
        for (uint16_t i = 0; i < obj_n_; ++i) {
            const PpuObj& o = oam_[i];
            uint16_t shape, size;
            if (!obj_shape_size(o.w_tiles, o.h_tiles, shape, size)) continue;  // unmappable -> skip
            if (o.h_tiles > 1 && o.stride != o.w_tiles)             continue;  // not 1D-contiguous
            const uint16_t a0 = uint16_t((uint16_t(o.y) & 0xFF) | (shape << 14));      // 16-col, normal
            const uint16_t a1 = uint16_t((uint16_t(o.x) & 0x1FF)
                                         | ((o.flip & 1) ? (1u << 12) : 0)              // H flip
                                         | ((o.flip & 2) ? (1u << 13) : 0)             // V flip
                                         | (size << 14));
            const uint16_t a2 = uint16_t(o.tile & 0x3FF);          // priority 0, palbank 0
            OAM[i * 4 + 0] = a0; OAM[i * 4 + 1] = a1; OAM[i * 4 + 2] = a2;
        }

        // BG0: charblock 0 (bits2-3=0), screenblock 24 (bits8-12), 4bpp, 32x32 (size 0).
        *BG0HOFS = uint16_t(st_.scroll_x);
        *BG0VOFS = uint16_t(st_.scroll_y);
        *BG0CNT  = uint16_t(kScreenblock << 8);
        // Mode 0 | BG0 on (bit8) | OBJ on (bit12) | 1D OBJ tile mapping (bit6).
        *DISPCNT = uint16_t((1u << 8) | (1u << 12) | (1u << 6));
    }

    // Map a w_tiles x h_tiles OBJ to a hardware (shape,size). Returns false if the GBA has no
    // such OBJ shape (those sprites are skipped — an honest hardware limit).
    static bool obj_shape_size(uint8_t w, uint8_t h, uint16_t& shape, uint16_t& size) {
        struct E { uint8_t w, h, shape, size; };
        static const E kT[] = {
            {1,1,0,0},{2,2,0,1},{4,4,0,2},{8,8,0,3},   // square
            {2,1,1,0},{4,1,1,1},{4,2,1,2},{8,4,1,3},   // horizontal
            {1,2,2,0},{1,4,2,1},{2,4,2,2},{4,8,2,3},   // vertical
        };
        for (const E& e : kT) if (e.w == w && e.h == h) { shape = e.shape; size = e.size; return true; }
        return false;
    }
#endif // PHX_GBA_HW

    // Find `texel`'s BGR555 colour in the palette, or add it. Returns false if the palette
    // is full (the >16-colour GBA limit). idx is set to the 1..15 palette slot on success.
    bool intern_colour(uint32_t texel, int& idx) {
        uint16_t c = rgba8_to_bgr555(texel);
        for (int i = 1; i < pal_count_; ++i)
            if (palette_[i] == c) { idx = i; return true; }
        if (pal_count_ >= kPalSize) return false;     // > 16 colours: unshippable on GBA
        palette_[pal_count_] = c;
        idx = pal_count_++;
        return true;
    }

    phx_gfx*   gfx_ = nullptr;
    PpuTile*   tiles_ = nullptr;  uint16_t tile_count_ = 0;
    uint16_t   palette_[kPalSize] = {};  int pal_count_ = 0;
    TexRec*    tex_ = nullptr;    uint16_t tex_n_ = 0;
    TextureId* free_ = nullptr;   uint16_t free_n_ = 0;
    MapRec*    maps_ = nullptr;   uint16_t map_n_ = 0;
    PpuScreenEntry* bg_map_ = nullptr;
    PpuObj*    oam_ = nullptr;    uint16_t obj_n_ = 0, obj_limit_ = kObjMax;
    uint32_t*  scratch_ = nullptr;
    int32_t    cam_x_ = 0, cam_y_ = 0;
    PpuState   st_{};
    RenderStats stats_{};
};

} // namespace

IRenderBackend* phx_make_render_backend(phx_gfx* gfx, ArenaAllocator& arena, const Caps& caps) {
    GbaPpuBackend* be = arena.make<GbaPpuBackend>();
    if (!be) return nullptr;
    be->init(gfx, arena, caps);
    return be;
}

} // namespace phx
