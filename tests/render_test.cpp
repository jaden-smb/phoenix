// tests/render_test.cpp — headless software-render smoke. Boots the null platform (which
// owns a CPU framebuffer), draws a tilemap + a sprite through the unified Renderer API,
// reads the framebuffer back, asserts specific pixels, and writes a PPM "screenshot" —
// the first visual artifact produced by the engine.
#include "phx/platform/platform.h"
#include "phx/platform/gfx_soft.h"
#include "phx/render/renderer.h"
#include "phx/core/caps.h"

#include <cstdio>
#include <cstdlib>

using namespace phx;

namespace {
constexpr int FBW = 64, FBH = 48;

// colors (RGBA8, memory order [R,G,B,A])
const Rgba kClear  = rgba(30, 30, 46);
const Rgba kBlue   = rgba(40, 80, 200);
const Rgba kYellow = rgba(220, 200, 40);
const Rgba kRed    = rgba(220, 40, 40);

int g_checks = 0, g_fail = 0;
void expect_px(const phx_soft_fb& fb, int x, int y, Rgba want, const char* what) {
    ++g_checks;
    Rgba got = fb.pixels[y * fb.w + x];
    if (got != want) {
        ++g_fail;
        std::printf("    FAIL pixel(%d,%d) %s: got %08X want %08X\n", x, y, what, got, want);
    }
}

void save_ppm(const phx_soft_fb& fb, const char* path) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", fb.w, fb.h);
    for (int i = 0; i < fb.w * fb.h; ++i) {
        Rgba v = fb.pixels[i];
        unsigned char rgb[3] = { (unsigned char)(v & 0xFF),
                                 (unsigned char)((v >> 8) & 0xFF),
                                 (unsigned char)((v >> 16) & 0xFF) };
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
    std::printf("    wrote screenshot: %s (%dx%d)\n", path, fb.w, fb.h);
}
} // namespace

int main() {
    // 1. boot the headless platform with a 64x48 software framebuffer
    const phx_platform* plat = phx_platform_get();
    phx_platform_desc desc{};
    desc.title = "render_test"; desc.width = FBW; desc.height = FBH; desc.vsync = 0;
    if (plat->init(&desc) != 0) { std::printf("platform init failed\n"); return 1; }

    // 2. arena + renderer (software tier)
    static uint8_t arena_buf[8 << 20];
    ArenaAllocator arena; arena.init(arena_buf, sizeof(arena_buf));
    auto rr = Renderer::create(plat->gfx(), arena, caps());
    if (!rr.ok()) { std::printf("Renderer::create failed\n"); return 1; }
    Renderer* r = rr.unwrap();

    // 3. build textures in memory:
    //    - a 16x8 tileset: tile0 (blue) | tile1 (yellow), each 8x8
    static uint32_t tileset[16 * 8];
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 16; ++x)
            tileset[y * 16 + x] = (x < 8) ? kBlue : kYellow;
    //    - an 8x8 solid red sprite
    static uint32_t red[8 * 8];
    for (int i = 0; i < 64; ++i) red[i] = kRed;

    TextureDesc tsd{}; tsd.pixels = tileset; tsd.width = 16; tsd.height = 8;
    TextureId ts_tex = r->load_texture(tsd);
    TextureDesc spd{}; spd.pixels = red; spd.width = 8; spd.height = 8;
    TextureId red_tex = r->load_texture(spd);

    // 4. a 4x3 tilemap (cell value = tileIndex+1; 0 = empty), checkerboard of tiles 1/2
    static const uint16_t indices[4 * 3] = {
        1, 2, 1, 2,
        2, 1, 2, 1,
        1, 2, 1, 2,
    };
    TilemapDesc md{}; md.indices = indices; md.width = 4; md.height = 3; md.layers = 1;
    md.tile_w = 8; md.tile_h = 8; md.tileset = ts_tex;
    TilemapId map = r->upload_tilemap(md);

    // 5. render one frame: tilemap background, then a red sprite on top
    Camera2D cam{};
    r->begin_frame(cam);
    r->draw_tilemap(map, 0);
    DrawSprite spr{}; spr.tex = red_tex; spr.sx = 0; spr.sy = 0; spr.sw = 8; spr.sh = 8;
    spr.pos = vec2{ s_from_int(20), s_from_int(20) }; spr.layer = 1; spr.z = 0;
    r->draw_sprite(spr);
    r->end_frame();

    // 6. read back + assert
    phx_soft_fb fb = phx_gfx_soft_lock(plat->gfx());
    expect_px(fb, 3,  3,  kBlue,   "tile(0,0) blue");
    expect_px(fb, 11, 3,  kYellow, "tile(1,0) yellow");
    expect_px(fb, 3,  11, kYellow, "tile(0,1) yellow");   // checker flips on row 1
    expect_px(fb, 23, 23, kRed,    "sprite over tile");
    expect_px(fb, 50, 40, kClear,  "background");

    const RenderStats& st = r->stats();
    ++g_checks; if (st.tiles_drawn != 12)       { ++g_fail; std::printf("    FAIL tiles_drawn=%u want 12\n", st.tiles_drawn); }
    ++g_checks; if (st.sprites_submitted != 1)  { ++g_fail; std::printf("    FAIL sprites_submitted=%u want 1\n", st.sprites_submitted); }

    save_ppm(fb, "build/render_out.ppm");

    // --- camera zoom: re-render the SAME scene at 2x and assert it scaled about the origin ---
    // tile(0,0) blue now spans screen 0..15, tile(1,0) yellow 16..31, the red sprite (world
    // 20,20 size 8) spans screen 40..55. Edge-difference scaling => no seams, exact at 2x.
    r->begin_frame(Camera2D{ vec2{}, s_from_int(2), 0 });
    r->draw_tilemap(map, 0);
    r->draw_sprite(spr);
    r->end_frame();
    fb = phx_gfx_soft_lock(plat->gfx());
    expect_px(fb, 3,  3,  kBlue,   "zoom2 tile(0,0) blue grown");
    expect_px(fb, 20, 3,  kYellow, "zoom2 tile(1,0) yellow at 2x offset");
    expect_px(fb, 3,  20, kYellow, "zoom2 tile(0,1) yellow at 2x offset");
    expect_px(fb, 44, 44, kRed,    "zoom2 sprite scaled to 16px");
    expect_px(fb, 23, 23, kBlue,   "zoom2: old sprite spot is now a background tile");

    // --- camera shake: a fresh renderer (frame 0 -> deterministic +8px X jitter) shifts the
    // scene left by 8, so the yellow tile(1,0) (world x 8..15) lands at the origin. shake is
    // a front-end camera offset, so this exercises the same path every backend sees. ---
    auto rr2 = Renderer::create(plat->gfx(), arena, caps());
    if (rr2.ok()) {
        Renderer* r2 = rr2.unwrap();
        TextureId ts2 = r2->load_texture(tsd);
        TilemapDesc md2 = md; md2.tileset = ts2;
        TilemapId map2 = r2->upload_tilemap(md2);
        Camera2D sc{}; sc.shake = 8;
        r2->begin_frame(sc);
        r2->draw_tilemap(map2, 0);
        r2->end_frame();
        fb = phx_gfx_soft_lock(plat->gfx());
        expect_px(fb, 3, 3, kYellow, "shake +8x: tile(1,0) yellow shifted to the origin");
    } else { ++g_fail; std::printf("    FAIL second renderer for shake\n"); }

    plat->shutdown();

    std::printf("\nrender_test: %d checks, %d failures\n", g_checks, g_fail);
    std::printf(g_fail == 0 ? "RENDER PASS\n\n" : "RENDER FAIL\n\n");
    return g_fail == 0 ? 0 : 1;
}
