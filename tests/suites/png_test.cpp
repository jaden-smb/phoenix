// tests/png_test.cpp — end-to-end PNG asset path: decode a real PNG with the pipeline's decoder
// (the exact call the phxpack CLI makes), bake the pixels into a `.phxp` bundle, mount it through
// the platform seam, upload the zero-copy texture view, draw it, and verify the PNG's colours
// reached the framebuffer. Proves decode → bake → mount → render for authored PNG art.
#include "phx/platform/platform.h"
#include "phx/platform/gfx_soft.h"
#include "phx/resource/cache.h"
#include "phx/render/renderer.h"
#include "phx/core/caps.h"
#include "bundle_writer.h"
#include "png.h"
#include "fixtures/png_fixtures.h"

#include <cstdio>
#include <vector>

using namespace phx;

namespace {
int g_checks = 0, g_fail = 0;
void check(bool ok, const char* what) { ++g_checks; if (!ok) { ++g_fail; std::printf("    FAIL %s\n", what); } }
} // namespace

int main() {
    // Drop the embedded PNG to disk too, so the phxpack CLI can bake it (.png path) in `make
    // phxpack`. Then decode it here exactly as the CLI does and bake the RGBA8 into a bundle.
    if (FILE* pf = std::fopen("build/png_in.png", "wb")) { std::fwrite(kPng3x2, 1, sizeof(kPng3x2), pf); std::fclose(pf); }

    std::vector<uint32_t> px; uint16_t w = 0, h = 0;
    check(phxtool::png_decode(kPng3x2, sizeof(kPng3x2), px, w, h), "png_decode");
    check(w == 3 && h == 2, "decoded dimensions");

    const char* bundle = "build/test_png.phxp";
    { phxtool::BundleWriter bw(/*tier*/2); bw.add_texture("hero", px.data(), w, h);
      check(bw.write(bundle), "bake PNG-derived texture into bundle"); }

    const phx_platform* plat = phx_platform_get();
    phx_platform_desc desc{}; desc.title = "png_test"; desc.width = 8; desc.height = 8;
    if (plat->init(&desc) != 0) { std::printf("platform init failed\n"); return 1; }

    static uint8_t arena_buf[4 << 20];
    ArenaAllocator arena; arena.init(arena_buf, sizeof(arena_buf));

    auto cr = ResourceCache::create(arena);
    ResourceCache* cache = cr.unwrap();
    check(cache->mount(plat, bundle) == Status::Ok, "mount bundle");

    auto tv = cache->texture("hero"_hash);
    check(tv.ok(), "texture('hero') from bundle");

    auto rr = Renderer::create(plat->gfx(), arena, caps());
    Renderer* r = rr.unwrap();
    TextureView t = tv.unwrap();
    TextureDesc td{}; td.pixels = t.pixels; td.width = t.width; td.height = t.height; td.format = t.format;
    TextureId tex = r->load_texture(td);

    Camera2D cam{};
    r->begin_frame(cam);
    DrawSprite s{}; s.tex = tex; s.sx = 0; s.sy = 0; s.sw = 3; s.sh = 2; s.pos = vec2{};
    r->draw_sprite(s);
    r->end_frame();

    phx_soft_fb fb = phx_gfx_soft_lock(plat->gfx());
    auto px_at = [&](int x, int y) { return fb.pixels[y * fb.w + x]; };
    check(px_at(0, 0) == 0xFF0000FFu, "pixel(0,0) red from PNG");
    check(px_at(1, 0) == 0xFF00FF00u, "pixel(1,0) green from PNG");
    check(px_at(2, 0) == 0xFFFF0000u, "pixel(2,0) blue from PNG");
    check(px_at(0, 1) == 0xFF00FFFFu, "pixel(0,1) yellow from PNG");
    check(px_at(2, 1) == 0xFFFF00FFu, "pixel(2,1) magenta from PNG");

    plat->shutdown();
    std::printf("\npng_test: %d checks, %d failures\n", g_checks, g_fail);
    std::printf(g_fail == 0 ? "PNG PASS\n\n" : "PNG FAIL\n\n");
    return g_fail == 0 ? 0 : 1;
}
