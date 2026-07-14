// tests/ppu_test.cpp — headless verification of the GBA-native PPU render backend.
// Drives the SAME unified Renderer the software backend uses, but linked against the GBA
// PPU backend (engine/render/src/gba/gba_ppu.cpp): RGBA8 atlases are quantized into 4bpp
// paletted tiles + OAM, then `ppu_compose` rasterizes the frame exactly as the silicon
// would. We read the framebuffer back and assert tile colours, transparency (palette index
// 0), sprite compositing, H/V flips, scrolled wrap, MULTI-BG layering, big-map window
// streaming, atlas-sub-rect OBJs (the 1D re-pack), the per-tile 16-colour limit, palette-
// BANK exhaustion, 8px tile alignment, invalid OBJ shapes, and the 128-sprite OAM ceiling
// — every constraint that would bite on hardware, caught here on the host. The compositor
// is also unit-tested directly via ppu_model.h.
#include "phx/platform/platform.h"
#include "phx/platform/gfx_soft.h"
#include "phx/render/renderer.h"
#include "phx/core/caps.h"
#include "ppu_model.h"          // internal PPU model (added to -I by the Makefile target)
#include "tex_encode.h"         // the tier-0 bake encoder (tools/phxpack; host-only test)

#include <cstdio>
#include <vector>

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

    // a 24x16 atlas of 16x16 frames: frame 0 all white, frame 1 (at sx=8.. no — 16px)…
    // two 8x8 frames side by side in a 16x16 atlas is enough: use a 16x16 texture whose
    // RIGHT 8x8 quarter is green — an OBJ drawn from that sub-rect proves the 1D re-pack.
    static uint32_t atlas[16 * 16];
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            atlas[y * 16 + x] = (x >= 8 && y < 8) ? kGreen : kWhite;
    TextureDesc ad{}; ad.pixels = atlas; ad.width = 16; ad.height = 16;
    TextureId atlas_tex = r->load_texture(ad);
    check(atlas_tex != kNoTexture, "atlas upload");

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

    // === frame 5: an OBJ from an atlas SUB-RECT (16x16 frame grid -> 1D re-pack) =======
    r->begin_frame(cam);
    {
        // the atlas' top-right 8x8 (green) — its tile is NOT contiguous with tile 0 in the
        // char store, so this exercises the per-frame contiguous OBJ tile run
        DrawSprite s{}; s.tex = atlas_tex; s.sx = 8; s.sy = 0; s.sw = 8; s.sh = 8;
        s.pos = vec2{ s_from_int(100), s_from_int(100) }; s.layer = 1;
        r->draw_sprite(s);
        // and the whole 16x16 (2x2 tiles, a real multi-tile OBJ shape)
        DrawSprite w{}; w.tex = atlas_tex; w.sx = 0; w.sy = 0; w.sw = 16; w.sh = 16;
        w.pos = vec2{ s_from_int(140), s_from_int(100) }; w.layer = 1;
        r->draw_sprite(w);
    }
    r->end_frame();
    {
        phx_soft_fb fb = phx_gfx_soft_lock(plat->gfx());
        expect_px(fb, 104, 104, kGreen, "atlas sub-rect OBJ shows the right frame");
        expect_px(fb, 143, 103, kWhite, "16x16 OBJ top-left quarter white");
        expect_px(fb, 153, 103, kGreen, "16x16 OBJ top-right quarter green");
        expect_px(fb, 153, 113, kWhite, "16x16 OBJ bottom-right quarter white");
    }

    // === frame 6: MULTI-BG — a second layer with an empty cell shows the first through ==
    {
        // full-screen-ish back map: every cell tile 1 (blue); front map: tile 2 with a hole
        static uint16_t back_idx[4 * 3], front_idx[4 * 3];
        for (int i = 0; i < 12; ++i) { back_idx[i] = 1; front_idx[i] = 2; }
        front_idx[0] = 0;                                   // hole at cell (0,0)
        TilemapDesc bd{}; bd.indices = back_idx; bd.width = 4; bd.height = 3; bd.layers = 1;
        bd.tile_w = 8; bd.tile_h = 8; bd.tileset = ts_tex;
        TilemapId back = r->upload_tilemap(bd);
        TilemapDesc fd = bd; fd.indices = front_idx;
        TilemapId front = r->upload_tilemap(fd);
        r->begin_frame(cam);
        r->draw_tilemap(back, 0);                           // slot 0 (behind)
        r->draw_tilemap(front, 0);                          // slot 1 (in front)
        r->end_frame();
        phx_soft_fb fb = phx_gfx_soft_lock(plat->gfx());
        expect_px(fb, 3,  3, kBlue,  "front hole -> back BG (blue) shows through");
        expect_px(fb, 11, 3, kGreen, "front layer (green) covers the back");
    }

    // === frame 7: BIG map — wider than one screenblock, streamed through the window ====
    {
        // 64x3 map: all tile 1 (blue) except column 40 = tile 2 (green). Scroll so that
        // column 40 lands at screen x 0..7: the old 32x32-cap backend could not show this.
        static uint16_t big[64 * 3];
        for (int i = 0; i < 64 * 3; ++i) big[i] = 1;
        big[0 * 64 + 40] = 2; big[1 * 64 + 40] = 2; big[2 * 64 + 40] = 2;
        TilemapDesc gd{}; gd.indices = big; gd.width = 64; gd.height = 3; gd.layers = 1;
        gd.tile_w = 8; gd.tile_h = 8; gd.tileset = ts_tex;
        TilemapId bigmap = r->upload_tilemap(gd);
        r->set_tilemap_scroll(bigmap, vec2{ s_from_int(40 * 8), s_from_int(0) });
        r->begin_frame(cam);
        r->draw_tilemap(bigmap, 0);
        r->end_frame();
        phx_soft_fb fb = phx_gfx_soft_lock(plat->gfx());
        expect_px(fb, 3,  3, kGreen, "big map: streamed window shows column 40");
        expect_px(fb, 11, 3, kBlue,  "big map: column 41 blue");
    }

    // === frame 8: the 128-OBJ hardware ceiling (drop + count, no overrun) ==============
    r->begin_frame(cam);
    for (int i = 0; i < 130; ++i) {
        DrawSprite s{}; s.tex = red_tex; s.sx = 0; s.sy = 0; s.sw = 8; s.sh = 8;
        s.pos = vec2{ s_from_int(i % 200), s_from_int(120) }; s.layer = 1;
        r->draw_sprite(s);
    }
    r->end_frame();
    check(r->stats().sprites_dropped == 2, "130 sprites -> 2 dropped at the 128 ceiling");

    // === a SEPARATE renderer (fresh palette banks): the honest hardware limits =========
    {
        auto rr2 = Renderer::create(plat->gfx(), arena, caps());
        Renderer* r2 = rr2.unwrap();

        // 15 distinct opaque colours (+ index 0) fits one 16-colour bank: OK
        static uint32_t ok15[8 * 8];
        for (int i = 0; i < 64; ++i) ok15[i] = rgba(uint8_t(((i % 15) + 1) * 8), 0, 0);
        TextureDesc o{}; o.pixels = ok15; o.width = 8; o.height = 8;
        check(r2->load_texture(o) != kNoTexture, "15-colour texture fits one palette bank");

        // 16 distinct opaque colours in ONE tile can't index a 4bpp bank: rejected
        static uint32_t over[8 * 8] = {};
        for (int i = 0; i < 16; ++i) over[i] = rgba(uint8_t((i + 1) * 8), 0, 0);
        for (int i = 16; i < 64; ++i) over[i] = rgba(8, 0, 0);
        TextureDesc ov{}; ov.pixels = over; ov.width = 8; ov.height = 8;
        check(r2->load_texture(ov) == kNoTexture, ">15 colours in one tile rejected");

        // a non-8px-aligned atlas can't be tiled: rejected
        static uint32_t mis[8 * 7] = {};
        TextureDesc mt{}; mt.pixels = mis; mt.width = 8; mt.height = 7;
        check(r2->load_texture(mt) == kNoTexture, "non-8px-aligned texture rejected");

        // an OBJ shape the hardware doesn't have (16x24 = 2x3 tiles) is not drawn
        static uint32_t tall[16 * 24];
        for (auto& p : tall) p = kRed;
        TextureDesc td{}; td.pixels = tall; td.width = 16; td.height = 24;
        TextureId tall_tex = r2->load_texture(td);
        check(tall_tex != kNoTexture, "16x24 texture uploads (BG use would be fine)");
        r2->begin_frame(cam);
        DrawSprite s{}; s.tex = tall_tex; s.sx = 0; s.sy = 0; s.sw = 16; s.sh = 24;
        s.pos = vec2{ s_from_int(0), s_from_int(0) }; s.layer = 1;
        r2->draw_sprite(s);
        r2->end_frame();
        phx_soft_fb fb = phx_gfx_soft_lock(plat->gfx());
        expect_px(fb, 2, 2, kBack, "invalid OBJ shape (2x3 tiles) rejected");

        // PALETTE-BANK exhaustion: bank 0 is full of the red ramp, bank 1 holds the 16x24
        // texture's red; 14 more textures of 15 FRESH colours each (distinct at 5 bits!)
        // fill banks 2..15; one more full bank's worth must then be rejected.
        static uint32_t fill[8 * 8];
        TextureId last_ok = kNoTexture, overflow = kNoTexture;
        for (int t = 0; t < 14; ++t) {
            for (int i = 0; i < 64; ++i)
                fill[i] = rgba(uint8_t(((i % 15) + 1) * 8), uint8_t((t + 1) * 16), 0);
            TextureDesc f{}; f.pixels = fill; f.width = 8; f.height = 8;
            last_ok = r2->load_texture(f);
        }
        check(last_ok != kNoTexture, "all 16 palette banks claimable");
        for (int i = 0; i < 64; ++i)
            fill[i] = rgba(uint8_t(((i % 15) + 1) * 8), 200, 100);
        TextureDesc f{}; f.pixels = fill; f.width = 8; f.height = 8;
        overflow = r2->load_texture(f);
        check(overflow == kNoTexture, "a 17th bank's worth of colours rejected (PALRAM full)");
    }

    // === direct unit checks on the compositor + colour conversion ======================
    {
        // BGR555 round-trip is exact for pure primaries (so PPU == soft for those colours)
        check(bgr555_to_rgba8(rgba8_to_bgr555(kRed))   == kRed,   "BGR555 exact: red");
        check(bgr555_to_rgba8(rgba8_to_bgr555(kGreen)) == kGreen, "BGR555 exact: green");
        check(bgr555_to_rgba8(rgba8_to_bgr555(kBlue))  == kBlue,  "BGR555 exact: blue");

        // hand-built window + 1 OBJ: transparency, overdraw order, palette banks
        static PpuTile tiles[2];
        for (auto& r_ : tiles[0].row) r_ = 0;                // tile 0: transparent
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) ppu_tile_set(tiles[1], x, y, 1);   // solid idx 1
        ppu_tile_set(tiles[1], 0, 0, 0);                     // ...except top-left (clear)
        static PpuTile objt[1];
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) ppu_tile_set(objt[0], x, y, 1);

        static uint16_t pal[kPalTotal] = {};
        pal[0]              = rgba8_to_bgr555(rgba(10, 20, 30));   // backdrop
        pal[1]              = rgba8_to_bgr555(kRed);               // bank 0 slot 1
        pal[1 * kPalSize + 1] = rgba8_to_bgr555(kGreen);           // bank 1 slot 1

        static PpuScreenEntry win[kBgWin * kBgWin] = {};
        for (auto& e : win) e = PpuScreenEntry{ 1, 0, 0 };          // tile 1, bank 0

        PpuObj obj{ 0, 0, 0, 1, 1, 0, 1 };   // one 8x8 OBJ at origin, obj tile 0, BANK 1
        static uint32_t out[kScreenW * kScreenH];
        PpuState s{};
        s.tiles = tiles; s.obj_tiles = objt; s.palette = pal;
        s.bg[0].win = win; s.bg[0].enabled = true;
        s.oam = &obj; s.obj_count = 1;
        ppu_compose(s, out);
        check(out[0] == bgr555_to_rgba8(pal[1 * kPalSize + 1]),
              "compose: OBJ drawn from its own palette bank");
        check(out[9] == bgr555_to_rgba8(pal[1]),
              "compose: BG texel from bank 0 beside the OBJ");
        // every tile's local (0,0) texel is clear: screen (16,0) is one, outside the OBJ
        check(out[16] == bgr555_to_rgba8(pal[0]),
              "compose: clear texel -> backdrop");
        check(out[kScreenW * 100 + 100] == bgr555_to_rgba8(pal[1]),
              "compose: window wraps at 256 px to fill the screen");
    }

    // === PAL4_TILES bake path == RGBA8 upload path (docs/06 §4) ========================
    // The tier-0 bake (tools/phxpack/tex_encode.h) runs this backend's quantizer offline;
    // uploading the baked blob must compose the EXACT frame the RGBA8 quantize produces —
    // for both the one-palette (OBJ-safe) case and the per-tile-palette spill.
    {
        // one-bank atlas: 2 tiles, 3 colours + a transparent texel
        static uint32_t atlas[16 * 8];
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 16; ++x)
                atlas[y * 16 + x] = (x < 8) ? kBlue : ((y < 4) ? kGreen : kRed);
        atlas[0] = 0;                                        // alpha 0 -> nibble 0

        // spilling atlas: 2 tiles x 15 fresh colours each (>15 total -> per-tile palettes)
        static uint32_t spill[16 * 8];
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 16; ++x)
                spill[y * 16 + x] = rgba(uint8_t((((y * 8 + (x & 7)) % 15) + 1) * 8),
                                         (x < 8) ? 64 : 128, 0);

        // encode both through the REAL bake encoder
        std::vector<uint8_t> enc_atlas, enc_spill;
        check(phxtool::pal4_encode(atlas, 16, 8, enc_atlas), "bake encodes the one-bank atlas");
        check(phxtool::pal4_encode(spill, 16, 8, enc_spill), "bake encodes the spilling atlas");
        {
            const TexturePal4Header& ph =
                *reinterpret_cast<const TexturePal4Header*>(enc_atlas.data());
            check(ph.pal_count == 1, "one-bank atlas baked a single palette (OBJ-safe)");
            const TexturePal4Header& ps =
                *reinterpret_cast<const TexturePal4Header*>(enc_spill.data());
            check(ps.pal_count == 2, ">15-colour atlas spilled to per-tile palettes");
        }

        // render the same scene through two fresh renderers: RGBA8 vs baked PAL4
        static uint16_t cells[4] = { 1, 2, 2, 1 };
        static Rgba frame_a[kScreenW * kScreenH];
        for (int pass = 0; pass < 2; ++pass) {
            auto rrp = Renderer::create(plat->gfx(), arena, caps());
            Renderer* rp = rrp.unwrap();
            TextureDesc t1{}, t2{};
            t1.width = t2.width = 16; t1.height = t2.height = 8;
            if (pass == 0) { t1.pixels = atlas; t2.pixels = spill; }
            else {
                t1.pixels = enc_atlas.data(); t1.format = PixelFormat::PAL4_TILES;
                t2.pixels = enc_spill.data(); t2.format = PixelFormat::PAL4_TILES;
            }
            TextureId ta = rp->load_texture(t1);
            TextureId tb = rp->load_texture(t2);
            check(ta != kNoTexture && tb != kNoTexture,
                  pass == 0 ? "RGBA8 pass uploads" : "PAL4 pass uploads");

            TilemapDesc md{}; md.indices = cells; md.width = 2; md.height = 2;
            md.layers = 1; md.tile_w = 8; md.tile_h = 8; md.tileset = tb;   // spill as BG
            TilemapId map = rp->upload_tilemap(md);

            rp->begin_frame(cam);
            rp->draw_tilemap(map, 0);
            DrawSprite sp{}; sp.tex = ta; sp.sx = 8; sp.sy = 0; sp.sw = 8; sp.sh = 8;
            sp.pos = vec2{ s_from_int(30), s_from_int(20) }; sp.layer = 1;
            rp->draw_sprite(sp);                             // one-bank texture as an OBJ
            rp->end_frame();

            phx_soft_fb fb = phx_gfx_soft_lock(plat->gfx());
            if (pass == 0) {
                for (int i = 0; i < kScreenW * kScreenH; ++i) frame_a[i] = fb.pixels[i];
            } else {
                bool same = true;
                for (int i = 0; i < kScreenW * kScreenH && same; ++i)
                    same = fb.pixels[i] == frame_a[i];
                check(same, "baked PAL4 upload composes the EXACT RGBA8-quantize frame");
            }
        }
    }

    plat->shutdown();
    std::printf("\nppu_test: %d checks, %d failures\n", g_checks, g_fail);
    std::printf(g_fail == 0 ? "PPU PASS\n\n" : "PPU FAIL\n\n");
    return g_fail == 0 ? 0 : 1;
}
