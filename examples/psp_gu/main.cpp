// examples/psp_gu/main.cpp — verifies the PSP-native GU render backend on a real emulator/PSP.
// It renders a known scene (blue+yellow tiles and a red sprite on a dark-blue clear) through the
// SAME portable Renderer, but linked against the GU backend (gu_backend.cpp) so the frame is built
// as a real sceGu GU_SPRITES display list and rasterized by the GU into the eDRAM framebuffer.
//
// Since external screen-grab can't capture PPSSPP's accelerated GL output, this verifies the GU
// output ON-PSP: after the GU finishes, it reads back the eDRAM framebuffer and checks key pixels
// (tile centres, sprite centre, a clear area) against the colours gu_compose (the proven golden
// model) defines, then reports the result via a thread NAME that shows up in PPSSPP's log:
//   sceKernelCreateThread("GU_VERIFY_PASS"/"GU_VERIFY_FAIL", ...)
// Built by `make psp-gu`. Run: grep the log for GU_VERIFY_PASS.
#include "phx/platform/platform.h"
#include "phx/render/renderer.h"
#include "phx/input/input.h"
#include "phx/core/caps.h"

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <stdlib.h>

PSP_MODULE_INFO("PHX_GU_VERIFY", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(8192);

extern "C" void phx_psp_set_direct(int on);

using namespace phx;

namespace {
int exit_cb(int, int, void*) { sceKernelExitGame(); return 0; }
int cb_thread(SceSize, void*) {
    int cb = sceKernelCreateCallback("exit", exit_cb, nullptr);
    sceKernelRegisterExitCallback(cb);
    sceKernelSleepThreadCB();
    return 0;
}
void setup_callbacks() {
    int th = sceKernelCreateThread("cb", cb_thread, 0x11, 0xFA0, 0, nullptr);
    if (th >= 0) sceKernelStartThread(th, 0, nullptr);
}

constexpr int kBufW = 512;   // eDRAM framebuffer stride (texels), matches the GU backend

// Read one pixel back from the GU's eDRAM draw buffer (offset 0), via the uncached mirror.
uint32_t fb_pixel(int x, int y) {
    volatile uint32_t* fb =
        reinterpret_cast<volatile uint32_t*>(reinterpret_cast<uintptr_t>(sceGeEdramGetAddr()) | 0x40000000u);
    return fb[y * kBufW + x];
}
inline uint32_t rgb_of(uint32_t c) { return c & 0x00FFFFFFu; }   // ignore alpha when comparing
}

int main() {
    setup_callbacks();
    phx_psp_set_direct(1);                       // the GU backend owns the display

    const phx_platform* plat = phx_platform_get();
    phx_platform_desc desc{}; desc.title = "phx-psp-gu"; desc.width = 480; desc.height = 272;
    if (plat->init(&desc) != 0) { sceKernelExitGame(); return 0; }

    const size_t kArena = 512 * 1024;
    void* arena_buf = malloc(kArena);
    if (!arena_buf) { sceKernelExitGame(); return 0; }
    ArenaAllocator arena; arena.init(arena_buf, kArena);
    auto rr = Renderer::create(plat->gfx(), arena, caps());
    if (!rr.ok()) { sceKernelExitGame(); return 0; }
    Renderer* r = rr.unwrap();

    // 16x8 tileset: tile0 blue | tile1 yellow.  8x8 red sprite.  (PoT, opaque.)
    static uint32_t tileset[16 * 8];
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 16; ++x) tileset[y * 16 + x] = (x < 8) ? rgba(40, 80, 200) : rgba(220, 200, 40);
    static uint32_t red[8 * 8]; for (auto& p : red) p = rgba(220, 40, 40);
    TextureDesc tsd{}; tsd.pixels = tileset; tsd.width = 16; tsd.height = 8;
    TextureId ts_tex = r->load_texture(tsd);
    TextureDesc spd{}; spd.pixels = red; spd.width = 8; spd.height = 8;
    TextureId red_tex = r->load_texture(spd);

    // 2x1 map: cell (0,0)=tile0 (blue) at x:0..7, cell (1,0)=tile1 (yellow) at x:8..15.
    static uint16_t indices[2] = { 1, 2 };
    TilemapDesc md{}; md.indices = indices; md.width = 2; md.height = 1;
    md.layers = 1; md.tile_w = 8; md.tile_h = 8; md.tileset = ts_tex;
    TilemapId map = r->upload_tilemap(md);

    // Render a couple of frames (the first lazily inits the GU), then read back + verify.
    for (int f = 0; f < 2; ++f) {
        r->begin_frame(Camera2D{});
        r->draw_tilemap(map, 0);
        DrawSprite s{}; s.tex = red_tex; s.sw = 8; s.sh = 8;
        s.pos = vec2{ s_from_int(100), s_from_int(100) }; s.layer = 1;
        r->draw_sprite(s);
        r->end_frame();
        plat->present();
    }

    // Expected colours (gu_compose's result for this scene): blue tile centre, yellow tile centre,
    // red sprite centre, dark-blue clear. The GU framebuffer is 8888==phx Rgba byte order.
    const bool ok =
        rgb_of(fb_pixel(4,   4))   == rgb_of(rgba(40, 80, 200)) &&   // tile (0,0) -> blue
        rgb_of(fb_pixel(12,  4))   == rgb_of(rgba(220, 200, 40)) &&  // tile (1,0) -> yellow
        rgb_of(fb_pixel(104, 104)) == rgb_of(rgba(220, 40, 40)) &&   // sprite centre -> red
        rgb_of(fb_pixel(300, 200)) == rgb_of(rgba(30, 30, 46));      // clear area

    // Report the verdict via a thread name (visible in PPSSPP's log).
    int vt = sceKernelCreateThread(ok ? "GU_VERIFY_PASS" : "GU_VERIFY_FAIL", cb_thread, 0x18, 0xFA0, 0, nullptr);
    (void)vt;

    for (;;) { sceDisplayWaitVblankStart(); }    // keep running so the log/thread is observable
    return 0;
}
