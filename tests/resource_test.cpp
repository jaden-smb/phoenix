// tests/resource_test.cpp — end-to-end asset pipeline. Bakes a `.phxp` bundle (texture +
// tilemap) with the offline writer, mounts it through the platform seam (real host file
// I/O), reads ZERO-COPY views, then RENDERS from the bundle and verifies pixels. Proves
// the whole content path: bake -> mount -> view -> draw.
#include "phx/platform/platform.h"
#include "phx/platform/gfx_soft.h"
#include "phx/resource/cache.h"
#include "phx/render/renderer.h"
#include "phx/core/caps.h"
#include "bundle_writer.h"

#include <cstdio>
#include <string>

using namespace phx;

namespace {
constexpr int FBW = 48, FBH = 32;
const Rgba kClear  = rgba(30, 30, 46);
const Rgba kBlue   = rgba(40, 80, 200);
const Rgba kYellow = rgba(220, 200, 40);

int g_checks = 0, g_fail = 0;
void check(bool ok, const char* what) {
    ++g_checks; if (!ok) { ++g_fail; std::printf("    FAIL %s\n", what); }
}

void bake_bundle(const char* path, bool compress) {
    phxtool::BundleWriter w(/*tier*/2);
    w.set_compression(compress);

    // 16x8 tileset: tile0 blue | tile1 yellow (each 8x8) — big flat colour runs, so it
    // compresses dramatically, exercising the LZSS path end to end.
    uint32_t tileset[16 * 8];
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 16; ++x)
            tileset[y * 16 + x] = (x < 8) ? kBlue : kYellow;
    w.add_texture("tiles", tileset, 16, 8);

    // 3x2 tilemap, checker of tile 1/2 (cell value = tileIndex+1; 0 = empty)
    static const uint16_t idx[3 * 2] = { 1, 2, 1,
                                         2, 1, 2 };
    w.add_tilemap("level1", idx, 3, 2, /*layers*/1, /*tw*/8, /*th*/8, "tiles");

    // a small data blob to exercise the generic path
    const char payload[] = "phx-blob-v1";
    w.add_blob("meta", payload, sizeof(payload));

    if (!w.write(path)) { std::printf("    FAIL could not write %s\n", path); }
}

long file_size(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return -1;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fclose(f);
    return n;
}
} // namespace

int main() {
    // Bake the same content uncompressed and compressed: identical assets reach the
    // framebuffer either way, but the compressed bundle is smaller on disk.
    const char* raw_bundle = "build/test_assets_raw.phxp";
    const char* bundle     = "build/test_assets.phxp";
    bake_bundle(raw_bundle, /*compress*/false);
    bake_bundle(bundle,     /*compress*/true);
    long raw_sz = file_size(raw_bundle), cmp_sz = file_size(bundle);
    check(cmp_sz > 0 && raw_sz > 0, "both bundles written");
    check(cmp_sz < raw_sz, "compressed bundle smaller than raw");

    // platform + framebuffer
    const phx_platform* plat = phx_platform_get();
    phx_platform_desc desc{}; desc.title = "resource_test"; desc.width = FBW; desc.height = FBH;
    if (plat->init(&desc) != 0) { std::printf("platform init failed\n"); return 1; }

    static uint8_t arena_buf[8 << 20];
    ArenaAllocator arena; arena.init(arena_buf, sizeof(arena_buf));

    // resource cache: mount the baked bundle through the seam
    auto cr = ResourceCache::create(arena);
    check(cr.ok(), "ResourceCache::create");
    ResourceCache* cache = cr.unwrap();
    check(cache->mount(plat, bundle) == Status::Ok, "mount bundle");
    check(cache->asset_count() == 3, "asset_count == 3");

    // read zero-copy views
    auto tv = cache->texture("tiles"_hash);
    check(tv.ok(), "texture('tiles') found");
    check(tv.ok() && tv.unwrap().width == 16 && tv.unwrap().height == 8, "texture dims");

    auto mv = cache->tilemap("level1"_hash);
    check(mv.ok(), "tilemap('level1') found");
    check(mv.ok() && mv.unwrap().width == 3 && mv.unwrap().tileset == "tiles"_hash, "tilemap meta");

    auto bv = cache->blob("meta"_hash);
    check(bv.ok(), "blob('meta') found");
    check(bv.ok() && std::string((const char*)bv.unwrap().data) == "phx-blob-v1", "blob content");

    // missing asset reports NotFound (not a crash)
    check(!cache->texture("nope"_hash).ok(), "missing texture -> NotFound");

    // RENDER from the bundle: upload the texture view, upload the tilemap view, draw
    auto rr = Renderer::create(plat->gfx(), arena, caps());
    check(rr.ok(), "Renderer::create");
    Renderer* r = rr.unwrap();

    TextureView t = tv.unwrap();
    TextureDesc td{}; td.pixels = t.pixels; td.width = t.width; td.height = t.height; td.format = t.format;
    TextureId tiles_tex = r->load_texture(td);

    TilemapView m = mv.unwrap();
    TilemapDesc md{}; md.indices = m.indices; md.width = m.width; md.height = m.height;
    md.layers = m.layers; md.tile_w = m.tile_w; md.tile_h = m.tile_h; md.tileset = tiles_tex;
    TilemapId map = r->upload_tilemap(md);

    Camera2D cam{};
    r->begin_frame(cam);
    r->draw_tilemap(map, 0);
    r->end_frame();

    // verify the bundle's content actually reached the framebuffer
    phx_soft_fb fb = phx_gfx_soft_lock(plat->gfx());
    auto px = [&](int x, int y) { return fb.pixels[y * fb.w + x]; };
    check(px(3, 3)   == kBlue,   "tile(0,0) blue from bundle");
    check(px(11, 3)  == kYellow, "tile(1,0) yellow from bundle");
    check(px(3, 11)  == kYellow, "tile(0,1) yellow (checker) from bundle");
    check(px(40, 28) == kClear,  "background clear");

    plat->shutdown();

    std::printf("\nresource_test: %d checks, %d failures\n", g_checks, g_fail);
    std::printf(g_fail == 0 ? "RESOURCE PASS\n\n" : "RESOURCE FAIL\n\n");
    return g_fail == 0 ? 0 : 1;
}
