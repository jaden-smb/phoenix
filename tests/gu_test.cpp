// tests/gu_test.cpp — headless verification of the PSP GU render backend. Drives the SAME
// unified Renderer the software backend uses, linked against the GU backend (engine/render/
// src/gu/gu_backend.cpp): the frame is recorded as GU sprite quads and `gu_compose`
// rasterizes them. Because the PSP GU is a full-colour textured GPU, its output is
// bit-identical to the software reference — so this renders the exact render_test scene and
// asserts the exact same pixels (blue/yellow tiles + a red sprite + clear background), then
// additionally checks dest scaling, per-channel tint modulate, and H-flip, plus a direct
// gu_compose unit check. Same verification pattern as the GBA PPU (tests/ppu_test.cpp).
#include "phx/platform/platform.h"
#include "phx/platform/gfx_soft.h"
#include "phx/render/renderer.h"
#include "phx/core/caps.h"
#include "gu_model.h"           // internal GU model (added to -I by the Makefile target)

#include <cstdio>

using namespace phx;
using namespace phx::gu;

namespace {
constexpr int FBW = 64, FBH = 48;

const Rgba kClear  = rgba(30, 30, 46);
const Rgba kBlue   = rgba(40, 80, 200);
const Rgba kYellow = rgba(220, 200, 40);
const Rgba kRed    = rgba(220, 40, 40);
const Rgba kWhite  = rgba(255, 255, 255);

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
    desc.title = "gu_test"; desc.width = FBW; desc.height = FBH; desc.vsync = 0;
    if (plat->init(&desc) != 0) { std::printf("platform init failed\n"); return 1; }

    static uint8_t arena_buf[8 << 20];
    ArenaAllocator arena; arena.init(arena_buf, sizeof(arena_buf));
    auto rr = Renderer::create(plat->gfx(), arena, caps());
    if (!rr.ok()) { std::printf("Renderer::create failed\n"); return 1; }
    Renderer* r = rr.unwrap();

    // assets — identical to render_test so the GU output can be compared to the soft golden
    static uint32_t tileset[16 * 8];
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 16; ++x) tileset[y * 16 + x] = (x < 8) ? kBlue : kYellow;
    static uint32_t red[8 * 8];   for (int i = 0; i < 64; ++i) red[i]   = kRed;
    static uint32_t white[8 * 8]; for (int i = 0; i < 64; ++i) white[i] = kWhite;
    static uint32_t lr[8 * 8];
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) lr[y * 8 + x] = (x < 4) ? kRed : kWhite;

    TextureDesc tsd{}; tsd.pixels = tileset; tsd.width = 16; tsd.height = 8;
    TextureId ts_tex = r->load_texture(tsd);
    TextureDesc rd{};  rd.pixels  = red;   rd.width  = 8; rd.height = 8;
    TextureId red_tex = r->load_texture(rd);
    TextureDesc wd{};  wd.pixels  = white; wd.width  = 8; wd.height = 8;
    TextureId white_tex = r->load_texture(wd);
    TextureDesc ld{};  ld.pixels  = lr;    ld.width  = 8; ld.height = 8;
    TextureId lr_tex = r->load_texture(ld);

    static const uint16_t indices[4 * 3] = {
        1, 2, 1, 2,
        2, 1, 2, 1,
        1, 2, 1, 2,
    };
    TilemapDesc md{}; md.indices = indices; md.width = 4; md.height = 3; md.layers = 1;
    md.tile_w = 8; md.tile_h = 8; md.tileset = ts_tex;
    TilemapId map = r->upload_tilemap(md);

    Camera2D cam{};

    // === frame 1: the render_test reference scene — must match the soft backend exactly ==
    r->begin_frame(cam);
    r->draw_tilemap(map, 0);
    {
        DrawSprite s{}; s.tex = red_tex; s.sx = 0; s.sy = 0; s.sw = 8; s.sh = 8;
        s.pos = vec2{ s_from_int(20), s_from_int(20) }; s.layer = 1;
        r->draw_sprite(s);
    }
    r->end_frame();
    {
        phx_soft_fb fb = phx_gfx_soft_lock(plat->gfx());
        expect_px(fb, 3,  3,  kBlue,   "tile(0,0) blue");
        expect_px(fb, 11, 3,  kYellow, "tile(1,0) yellow");
        expect_px(fb, 3,  11, kYellow, "tile(0,1) yellow (checker)");
        expect_px(fb, 23, 23, kRed,    "red sprite over tile");
        expect_px(fb, 50, 40, kClear,  "background");
        const RenderStats& st = r->stats();
        check(st.tiles_drawn == 12,      "tiles_drawn == 12");
        check(st.sprites_submitted == 1, "sprites_submitted == 1");
    }

    // === frame 2: dest scaling + per-channel tint + H-flip ==============================
    r->begin_frame(cam);
    {
        // 8x8 red scaled to 16x16 at (0,30)
        DrawSprite a{}; a.tex = red_tex; a.sx = 0; a.sy = 0; a.sw = 8; a.sh = 8;
        a.pos = vec2{ s_from_int(0), s_from_int(30) }; a.dw = 16; a.dh = 16; a.layer = 1;
        r->draw_sprite(a);
        // white sprite tinted blue -> modulates to blue
        DrawSprite b{}; b.tex = white_tex; b.sx = 0; b.sy = 0; b.sw = 8; b.sh = 8;
        b.pos = vec2{ s_from_int(40), s_from_int(30) }; b.tint = kBlue; b.layer = 1;
        r->draw_sprite(b);
        // left-red/right-white, normal then H-flipped
        DrawSprite c{}; c.tex = lr_tex; c.sx = 0; c.sy = 0; c.sw = 8; c.sh = 8;
        c.pos = vec2{ s_from_int(20), s_from_int(38) }; c.layer = 1;
        r->draw_sprite(c);
        DrawSprite d = c; d.pos = vec2{ s_from_int(30), s_from_int(38) }; d.flags = kFlipX;
        r->draw_sprite(d);
    }
    r->end_frame();
    {
        phx_soft_fb fb = phx_gfx_soft_lock(plat->gfx());
        expect_px(fb, 10, 40, kRed,   "scaled 16x16 sprite covers (10,40)");
        expect_px(fb, 44, 34, kBlue,  "white sprite tinted blue -> blue");
        expect_px(fb, 21, 40, kRed,   "lr sprite left half red");
        expect_px(fb, 26, 40, kWhite, "lr sprite right half white");
        expect_px(fb, 31, 40, kWhite, "Hflip: left half now white");
        expect_px(fb, 36, 40, kRed,   "Hflip: right half now red");
    }

    // === direct gu_compose unit check ==================================================
    {
        static uint32_t tpx[2 * 2] = { kRed, kRed, kRed, kRed };
        GuTexRef tex{ tpx, 2, 2 };
        GuQuad q{}; q.tex = 0; q.sx = 0; q.sy = 0; q.sw = 2; q.sh = 2;
        q.dx = 0; q.dy = 0; q.dw = 4; q.dh = 4; q.tint = kWhite;   // 2x2 scaled to 4x4
        static uint32_t out[8 * 8];
        GuState s{}; s.tex = &tex; s.tex_count = 1; s.quads = &q; s.quad_count = 1;
        s.clear = kClear; s.w = 8; s.h = 8;
        gu_compose(s, out);
        check(out[0] == kRed,            "compose: scaled quad covers origin");
        check(out[3 * 8 + 3] == kRed,    "compose: scaled quad covers (3,3)");
        check(out[4 * 8 + 4] == kClear,  "compose: outside the 4x4 quad -> clear");
    }

    plat->shutdown();
    std::printf("\ngu_test: %d checks, %d failures\n", g_checks, g_fail);
    std::printf(g_fail == 0 ? "GU PASS\n\n" : "GU FAIL\n\n");
    return g_fail == 0 ? 0 : 1;
}
