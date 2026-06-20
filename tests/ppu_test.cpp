// tests/ppu_test.cpp — headless verification of the GBA-native PPU render backend.
// Drives the SAME unified Renderer the software backend uses, but linked against the GBA
// PPU backend (engine/render/src/gba/gba_ppu.cpp): RGBA8 atlases are quantized into 4bpp
// paletted tiles + OAM, then `ppu_compose` rasterizes the frame exactly as the silicon
// would. We read the framebuffer back and assert tile colours, transparency (palette index
// 0), sprite compositing, H/V flips, scrolled wrap, the >16-colour palette limit, 8px tile
// alignment, and the 128-sprite OAM ceiling — every constraint that would bite on hardware,
// caught here on the host. The compositor is also unit-tested directly via ppu_model.h.
#include "phx/platform/platform.h"
#include "phx/platform/gfx_soft.h"
#include "phx/render/renderer.h"
#include "phx/core/caps.h"
#include "ppu_model.h"          // internal PPU model (added to -I by the Makefile target)

#include <cstdio>

using namespace phx;
using namespace phx::gba;

namespace {
constexpr int FBW = kScreenW, FBH = kScreenH;   // a GBA-resolution device (240x160)

const Rgba kBlue  = rgba(0, 0, 255);
const Rgba kGreen = rgba(0, 255, 0);
const Rgba kRed   = rgba(255, 0, 0);
const Rgba kWhite = rgba(255, 255, 255);
// The backdrop is the soft backend's clear colour as it survives the GBA's 15-bit path.
const Rgba kBack  = bgr555_to_rgba8(rgba8_to_bgr555(rgba(30, 30, 46)));

int g_checks = 0, g_fail = 0;
void check(bool cond, const char* what) {
    ++g_checks;
    if (!cond) { ++g_fail; std::printf("    FAIL %s\n", what); }
}
void expect_px(const phx_soft_fb& fb, int x, int y, Rgba want, const char* what) {
    ++g_checks;
    Rgba got = fb.pixels[y * fb.w + x];
    if (got != want) { ++g_fail; std::printf("    FAIL pixel(%d,%d) %s: got %08X want %08X\n",
                                             x, y, what, got, want); }
}
} // namespace

int main() {
    const phx_platform* plat = phx_platform_get();
    phx_platform_desc desc{};
    desc.title = "ppu_test"; desc.width = FBW; desc.height = FBH; desc.vsync = 0;
    if (plat->init(&desc) != 0) { std::printf("platform init failed\n"); return 1; }

    static uint8_t arena_buf[16 << 20];
    ArenaAllocator arena; arena.init(arena_buf, sizeof(arena_buf));
    auto rr = Renderer::create(plat->gfx(), arena, caps());
    if (!rr.ok()) { std::printf("Renderer::create failed\n"); return 1; }
    Renderer* r = rr.unwrap();

    // --- assets: a 2-tile tileset (blue|green), and three 8x8 sprite textures -----------
    static uint32_t tileset[16 * 8];
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 16; ++x) tileset[y * 16 + x] = (x < 8) ? kBlue : kGreen;
    TextureDesc tsd{}; tsd.pixels = tileset; tsd.width = 16; tsd.height = 8;
    TextureId ts_tex = r->load_texture(tsd);
    check(ts_tex != kNoTexture, "tileset upload");

    // red sprite with a transparent (alpha 0) top-left texel
    static uint32_t spr_red[8 * 8];
    for (int i = 0; i < 64; ++i) spr_red[i] = kRed;
    spr_red[0] = 0;   // alpha 0 -> palette index 0 -> transparent
    TextureDesc rd{}; rd.pixels = spr_red; rd.width = 8; rd.height = 8;
    TextureId red_tex = r->load_texture(rd);
    check(red_tex != kNoTexture, "red sprite upload");

    // left-half red / right-half white sprite (for flip tests)
    static uint32_t spr_lr[8 * 8];
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) spr_lr[y * 8 + x] = (x < 4) ? kRed : kWhite;
    TextureDesc ld{}; ld.pixels = spr_lr; ld.width = 8; ld.height = 8;
    TextureId lr_tex = r->load_texture(ld);
    check(lr_tex != kNoTexture, "lr sprite upload");

    // a 4x3 tilemap, checkerboard of tiles 1/2, last cell EMPTY (0 -> backdrop)
    static const uint16_t indices[4 * 3] = {
        1, 2, 1, 2,
        2, 1, 2, 1,
        1, 2, 1, 0,
    };
    TilemapDesc md{}; md.indices = indices; md.width = 4; md.height = 3; md.layers = 1;
    md.tile_w = 8; md.tile_h = 8; md.tileset = ts_tex;
    TilemapId map = r->upload_tilemap(md);
    check(map != kNoTilemap, "tilemap upload");

    Camera2D cam{};

    // === frame 1: tilemap only — tile colours, checker flip, empty cell = backdrop =====
    r->begin_frame(cam);
    r->draw_tilemap(map, 0);
    r->end_frame();
    {
        phx_soft_fb fb = phx_gfx_soft_lock(plat->gfx());
        expect_px(fb, 3,  3,  kBlue,  "tile(0,0) blue");
        expect_px(fb, 11, 3,  kGreen, "tile(1,0) green");
        expect_px(fb, 3,  11, kGreen, "tile(0,1) green (checker)");
        expect_px(fb, 11, 11, kBlue,  "tile(1,1) blue (checker)");
        expect_px(fb, 27, 19, kBack,  "empty cell (3,2) -> backdrop");
        const RenderStats& st = r->stats();
        check(st.tiles_drawn == 11, "tiles_drawn == 11 (one empty cell skipped)");
    }

    // === frame 2: a sprite with a transparent texel over the BG ========================
    r->begin_frame(cam);
    r->draw_tilemap(map, 0);
    {
        DrawSprite s{}; s.tex = red_tex; s.sx = 0; s.sy = 0; s.sw = 8; s.sh = 8;
        s.pos = vec2{ s_from_int(16), s_from_int(16) }; s.layer = 1;
        r->draw_sprite(s);
    }
    r->end_frame();
    {
        phx_soft_fb fb = phx_gfx_soft_lock(plat->gfx());
        // (16,16) is the sprite's transparent corner -> BG tile (2,2) = blue shows through
        expect_px(fb, 16, 16, kBlue, "sprite transparent corner shows BG");
        expect_px(fb, 20, 20, kRed,  "sprite opaque body");
        check(r->stats().sprites_submitted == 1, "1 sprite submitted");
    }

    // === frame 3: H-flip — left/right halves swap ======================================
    r->begin_frame(cam);
    {
        DrawSprite a{}; a.tex = lr_tex; a.sx = 0; a.sy = 0; a.sw = 8; a.sh = 8;
        a.pos = vec2{ s_from_int(0), s_from_int(80) }; a.layer = 1;
        r->draw_sprite(a);
        DrawSprite b = a; b.pos = vec2{ s_from_int(16), s_from_int(80) }; b.flags = kFlipX;
        r->draw_sprite(b);
    }
    r->end_frame();
    {
        phx_soft_fb fb = phx_gfx_soft_lock(plat->gfx());
        expect_px(fb, 1,  80, kRed,   "unflipped: left half red");
        expect_px(fb, 6,  80, kWhite, "unflipped: right half white");
        expect_px(fb, 17, 80, kWhite, "Hflip: left half now white");
        expect_px(fb, 22, 80, kRed,   "Hflip: right half now red");
    }

    // === frame 4: scrolled BG wraps ====================================================
    r->set_tilemap_scroll(map, vec2{ s_from_int(8), s_from_int(0) });  // scroll one tile right
    r->begin_frame(cam);
    r->draw_tilemap(map, 0);
    r->end_frame();
    {
        phx_soft_fb fb = phx_gfx_soft_lock(plat->gfx());
        // scrolled +8px: screen tile (0,0) now shows source tile (1,0) = green
        expect_px(fb, 3, 3, kGreen, "scroll +8 -> green at origin");
    }
    r->set_tilemap_scroll(map, vec2{ s_from_int(0), s_from_int(0) });

    // === frame 5: the 128-OBJ hardware ceiling (drop + count, no overrun) ==============
    r->begin_frame(cam);
    for (int i = 0; i < 130; ++i) {
        DrawSprite s{}; s.tex = red_tex; s.sx = 0; s.sy = 0; s.sw = 8; s.sh = 8;
        s.pos = vec2{ s_from_int(i % 200), s_from_int(120) }; s.layer = 1;
        r->draw_sprite(s);
    }
    r->end_frame();
    check(r->stats().sprites_dropped == 2, "130 sprites -> 2 dropped at the 128 ceiling");

    // === a SEPARATE renderer (fresh palette): >16-colour + tile-alignment limits =======
    {
        auto rr2 = Renderer::create(plat->gfx(), arena, caps());
        Renderer* r2 = rr2.unwrap();

        // 15 distinct opaque colours (+ index 0) fits one 16-colour bank: OK
        static uint32_t ok15[8 * 8];
        for (int i = 0; i < 64; ++i) ok15[i] = rgba(uint8_t(((i % 15) + 1) * 8), 0, 0);
        TextureDesc o{}; o.pixels = ok15; o.width = 8; o.height = 8;
        check(r2->load_texture(o) != kNoTexture, "15-colour texture fits the palette");

        // 16 distinct opaque colours needs a 17th palette slot: rejected
        static uint32_t over[8 * 8] = {};
        for (int i = 0; i < 16; ++i) over[i] = rgba(uint8_t((i + 1) * 8), 0, 0);
        for (int i = 16; i < 64; ++i) over[i] = rgba(8, 0, 0);
        TextureDesc ov{}; ov.pixels = over; ov.width = 8; ov.height = 8;
        check(r2->load_texture(ov) == kNoTexture, ">16 colours rejected (GBA palette limit)");

        // a non-8px-aligned atlas can't be tiled: rejected
        static uint32_t mis[8 * 7] = {};
        TextureDesc mt{}; mt.pixels = mis; mt.width = 8; mt.height = 7;
        check(r2->load_texture(mt) == kNoTexture, "non-8px-aligned texture rejected");
    }

    // === direct unit checks on the compositor + colour conversion ======================
    {
        // BGR555 round-trip is exact for pure primaries (so PPU == soft for those colours)
        check(bgr555_to_rgba8(rgba8_to_bgr555(kRed))   == kRed,   "BGR555 exact: red");
        check(bgr555_to_rgba8(rgba8_to_bgr555(kGreen)) == kGreen, "BGR555 exact: green");
        check(bgr555_to_rgba8(rgba8_to_bgr555(kBlue))  == kBlue,  "BGR555 exact: blue");

        // hand-built 1-tile BG + 1 OBJ: transparency + overdraw order
        PpuTile tiles[2];
        for (auto& p : tiles[0].px) p = 0;          // tile 0: transparent
        for (auto& p : tiles[1].px) p = 1;          // tile 1: solid palette idx 1
        tiles[1].px[0] = 0;                          // ...except its top-left texel (clear)
        uint16_t pal[16] = {};
        pal[0] = rgba8_to_bgr555(rgba(10, 20, 30)); // backdrop
        pal[1] = rgba8_to_bgr555(kRed);
        PpuScreenEntry mapent{ 1, 0 };
        PpuObj obj{ 0, 0, 1, 1, 1, 1, 0 };          // one 8x8 OBJ at origin, tile 1
        static uint32_t out[kScreenW * kScreenH];
        PpuState s{};
        s.tiles = tiles; s.palette = pal;
        s.bg_map = &mapent; s.bg_w = 1; s.bg_h = 1; s.bg_enabled = true;
        s.oam = &obj; s.obj_count = 1;
        ppu_compose(s, out);
        check(out[0] == bgr555_to_rgba8(pal[0]), "compose: clear texel -> backdrop");
        check(out[1] == bgr555_to_rgba8(pal[1]), "compose: opaque OBJ texel drawn");
        // the 1x1 BG wraps across the whole screen, so distant pixels show its solid texel
        check(out[kScreenW * 100 + 100] == bgr555_to_rgba8(pal[1]),
              "compose: BG wraps to fill the screen");
    }

    plat->shutdown();
    std::printf("\nppu_test: %d checks, %d failures\n", g_checks, g_fail);
    std::printf(g_fail == 0 ? "PPU PASS\n\n" : "PPU FAIL\n\n");
    return g_fail == 0 ? 0 : 1;
}
