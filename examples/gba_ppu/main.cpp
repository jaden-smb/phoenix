// examples/gba_ppu/main.cpp — drives the GBA-native PPU render backend on REAL hardware.
// Unlike gba_smoke (which CPU-rasterizes into a Mode-3 bitmap), this links the PPU backend
// (engine/render/src/gba/gba_ppu.cpp): the SAME portable Renderer front end records a
// tilemap + sprite, and the backend quantizes them to 4bpp tiles + a 16-colour palette and
// programs the PPU — tiles->VRAM charblocks, palette->PALRAM, map->a screenblock, sprite->OAM,
// DISPCNT=Mode 0+BG0+OBJ — so the silicon scans the frame out (no software blit). It renders at
// the native 240x160; the soft framebuffer is unused on this path, so the platform fb is tiny.
//
// Built by `make gba-ppu` (devkitARM). Verified headlessly via mGBA's GDB stub + a VRAM/OAM/
// PALRAM/DISPCNT dump (the `ppu_compose` model is the golden definition of the frame).
#include "phx/platform/platform.h"
#include "phx/render/renderer.h"
#include "phx/input/input.h"
#include "phx/core/caps.h"

#include <stdlib.h>   // malloc — the engine arena lives in EWRAM

using namespace phx;

namespace {
constexpr int SCRW = 240, SCRH = 160;                 // native PPU resolution
constexpr int TILES_X = SCRW / 8, TILES_Y = SCRH / 8; // 30 x 20
}

int main() {
    const phx_platform* plat = phx_platform_get();
    phx_platform_desc desc{};
    // The PPU path scans VRAM/OAM directly; the platform's software framebuffer is unused, so
    // we give it a tiny one (the backend renders natively at 240x160 regardless).
    desc.title = "phx-ppu"; desc.width = 8; desc.height = 8;
    if (plat->init(&desc) != 0) return 1;

    const size_t kArena = 96 * 1024;                  // PPU backend: 4bpp char store + BG map etc.
    void* arena_buf = malloc(kArena);
    if (!arena_buf) return 1;
    ArenaAllocator arena; arena.init(arena_buf, kArena);
    auto rr = Renderer::create(plat->gfx(), arena, caps());
    if (!rr.ok()) return 1;
    Renderer* r = rr.unwrap();

    // 16x8 tileset: tile0 blue | tile1 yellow.  An 8x8 red sprite.  (<=16 colours, 8px aligned.)
    static uint32_t tileset[16 * 8];
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 16; ++x) tileset[y * 16 + x] = (x < 8) ? rgba(40, 80, 200) : rgba(220, 200, 40);
    static uint32_t red[8 * 8]; for (auto& p : red) p = rgba(220, 40, 40);
    TextureDesc tsd{}; tsd.pixels = tileset; tsd.width = 16; tsd.height = 8;
    TextureId ts_tex = r->load_texture(tsd);
    TextureDesc spd{}; spd.pixels = red; spd.width = 8; spd.height = 8;
    TextureId red_tex = r->load_texture(spd);

    // 30x20 checkerboard tilemap (cell = tile index + 1; alternating 1/2) — fills the screen.
    static uint16_t indices[TILES_X * TILES_Y];
    for (int y = 0; y < TILES_Y; ++y)
        for (int x = 0; x < TILES_X; ++x) indices[y * TILES_X + x] = uint16_t(((x + y) & 1) ? 1 : 2);
    TilemapDesc md{}; md.indices = indices; md.width = TILES_X; md.height = TILES_Y;
    md.layers = 1; md.tile_w = 8; md.tile_h = 8; md.tileset = ts_tex;
    TilemapId map = r->upload_tilemap(md);

    InputState in;
    int px = SCRW / 2 - 4, py = SCRH / 2 - 4;
    for (;;) {
        phx_input_raw raw; plat->poll_input(&raw); in.update(raw);
        if (in.down(Button::Right) && px < SCRW - 8) ++px;
        if (in.down(Button::Left)  && px > 0)        --px;
        if (in.down(Button::Down)  && py < SCRH - 8) ++py;
        if (in.down(Button::Up)    && py > 0)        --py;

        r->begin_frame(Camera2D{});
        r->draw_tilemap(map, 0);
        DrawSprite s{}; s.tex = red_tex; s.sw = 8; s.sh = 8;
        s.pos = vec2{ s_from_int(px), s_from_int(py) }; s.layer = 1;
        r->draw_sprite(s);
        r->end_frame();
        plat->present();
    }
    // unreachable
}
