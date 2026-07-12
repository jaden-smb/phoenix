// tests/tiled_test.cpp — the phxtile path end to end: parse a Tiled `.tmj` (JSON) map, bake its
// tile layer into a Tilemap + its object group into a Spawns table (alongside a tileset texture),
// mount the bundle, and verify both — the tilemap RENDERS the right tiles and the spawn points
// resolve by type. Proves authored Tiled maps flow JSON → import → bake → mount → render/spawn.
#include "phx/platform/platform.h"
#include "phx/platform/gfx_soft.h"
#include "phx/resource/cache.h"
#include "phx/render/renderer.h"
#include "phx/core/caps.h"
#include "bundle_writer.h"
#include "tiled.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace phx;

namespace {
constexpr int FBW = 48, FBH = 32;
const Rgba kBlue   = rgba(40, 80, 200);
const Rgba kYellow = rgba(220, 200, 40);

int g_checks = 0, g_fail = 0;
void check(bool ok, const char* what) { ++g_checks; if (!ok) { ++g_fail; std::printf("    FAIL %s\n", what); } }

// A small Tiled JSON map: one tile layer (checker of tiles 1/2) + one object group (2 spawns).
const char* kTmj =
"{ \"width\":3, \"height\":2, \"tilewidth\":8, \"tileheight\":8,"
"  \"tilesets\":[ { \"firstgid\":1, \"name\":\"tiles\" } ],"
"  \"layers\":["
"    { \"type\":\"tilelayer\", \"name\":\"ground\", \"width\":3, \"height\":2, \"data\":[1,2,1,2,1,2] },"
"    { \"type\":\"objectgroup\", \"name\":\"things\", \"objects\":["
"       { \"name\":\"p\", \"type\":\"player\", \"x\":8,  \"y\":16, \"width\":8, \"height\":8 },"
"       { \"name\":\"c\", \"type\":\"coin\",   \"x\":16, \"y\":8,  \"width\":8, \"height\":8 } ] } ] }";
} // namespace

int main() {
    // Parse the Tiled map and check the importer's view of it.
    phxtool::TiledMap tm;
    check(phxtool::tiled_load(kTmj, tm), "tiled_load");
    check(tm.width == 3 && tm.height == 2 && tm.tile_w == 8 && tm.tile_h == 8, "map dimensions");
    check(tm.tileset == "tiles", "tileset name");
    check(tm.layers.size() == 1, "one tile layer");
    check(tm.layers.size() == 1 && tm.layers[0].size() == 6, "layer cell count");
    check(tm.spawns.size() == 2, "two spawns");
    check(tm.spawns.size() == 2 && tm.spawns[0].type == "player" && tm.spawns[0].x == 8 && tm.spawns[0].y == 16, "player spawn");
    check(tm.spawns.size() == 2 && tm.spawns[1].type == "coin" && tm.spawns[1].x == 16, "coin spawn");

    // Also drop it to disk so the phxpack CLI can bake the .tmj path in `make phxpack`.
    if (FILE* f = std::fopen("build/level.tmj", "wb")) { std::fputs(kTmj, f); std::fclose(f); }

    // Bake: a tileset texture (tile0 blue | tile1 yellow), the tilemap, and the spawns.
    const char* bundle = "build/test_tiled.phxp";
    {
        phxtool::BundleWriter bw(/*tier*/2);
        uint32_t tileset[16 * 8];
        for (int y = 0; y < 8; ++y) for (int x = 0; x < 16; ++x)
            tileset[y * 16 + x] = (x < 8) ? kBlue : kYellow;
        bw.add_texture("tiles", tileset, 16, 8);

        std::vector<uint16_t> flat;
        for (auto& L : tm.layers) flat.insert(flat.end(), L.begin(), L.end());
        bw.add_tilemap("level", flat.data(), uint16_t(tm.width), uint16_t(tm.height),
                       uint8_t(tm.layers.size()), uint8_t(tm.tile_w), uint8_t(tm.tile_h), "tiles");

        std::vector<phx::SpawnDef> sd;
        for (auto& s : tm.spawns)
            sd.push_back(phx::SpawnDef{ phx::fnv1a(s.type.c_str()), int16_t(s.x), int16_t(s.y), uint16_t(s.w), uint16_t(s.h) });
        bw.add_spawns("level", sd);
        check(bw.write(bundle), "bake tilemap + spawns + tileset");
    }

    const phx_platform* plat = phx_platform_get();
    phx_platform_desc desc{}; desc.title = "tiled_test"; desc.width = FBW; desc.height = FBH;
    if (plat->init(&desc) != 0) { std::printf("platform init failed\n"); return 1; }

    static uint8_t arena_buf[8 << 20];
    ArenaAllocator arena; arena.init(arena_buf, sizeof(arena_buf));

    ResourceCache* cache = ResourceCache::create(arena).unwrap();
    check(cache->mount(plat, bundle) == Status::Ok, "mount bundle");

    auto mv = cache->tilemap("level"_hash);
    check(mv.ok(), "tilemap('level') found");
    TilemapView m = mv.unwrap();
    check(m.width == 3 && m.height == 2 && m.tileset == "tiles"_hash, "tilemap meta from Tiled");
    check(m.indices && m.indices[0] == 1 && m.indices[1] == 2 && m.indices[5] == 2, "tilemap indices from Tiled");

    auto sp = cache->spawns("level"_hash);
    check(sp.ok(), "spawns('level') found");
    SpawnsView sv = sp.unwrap();
    check(sv.count == 2, "two spawn points");
    check(sv.count == 2 && sv.spawns[0].type == "player"_hash && sv.spawns[0].x == 8 && sv.spawns[0].y == 16, "player spawn resolves");
    check(sv.count == 2 && sv.spawns[1].type == "coin"_hash && sv.spawns[1].x == 16, "coin spawn resolves");

    // Parallax: a map whose single 3x1 layer scrolls at half camera speed (Tiled's native
    // parallaxx). The ODD index count (3) forces the Q16 table's 4-byte alignment padding,
    // so this proves factor import -> bake -> mount AND the padded layout in one pass.
    {
        const char* kParTmj =
        "{ \"width\":3, \"height\":1, \"tilewidth\":8, \"tileheight\":8,"
        "  \"tilesets\":[ { \"firstgid\":1, \"name\":\"tiles\" } ],"
        "  \"layers\":[ { \"type\":\"tilelayer\", \"name\":\"sky\", \"width\":3, \"height\":1,"
        "                \"parallaxx\":0.5, \"parallaxy\":0, \"data\":[1,2,1] } ] }";
        phxtool::TiledMap ptm;
        check(phxtool::tiled_load(kParTmj, ptm), "tiled_load (parallax map)");
        check(ptm.has_parallax(), "importer sees the parallax factors");
        check(ptm.layer_parallax.size() == 1 && ptm.layer_parallax[0].first == 0.5 &&
              ptm.layer_parallax[0].second == 0.0, "parallaxx/parallaxy parsed");

        const char* pbundle = "build/test_tiled_par.phxp";
        phxtool::BundleWriter pw(/*tier*/2);
        pw.add_tilemap("sky", ptm.layers[0].data(), 3, 1, 1, 8, 8, "tiles",
                       &ptm.layer_parallax);
        check(pw.write(pbundle), "bake parallax tilemap");

        ResourceCache* pc = ResourceCache::create(arena).unwrap();
        check(pc->mount(plat, pbundle) == Status::Ok, "mount parallax bundle");
        auto pv = pc->tilemap("sky"_hash);
        check(pv.ok(), "tilemap('sky') found");
        TilemapView pm = pv.unwrap();
        check(pm.indices && pm.indices[0] == 1 && pm.indices[2] == 1, "parallax map indices intact");
        check(pm.parallax_q16 != nullptr, "parallax table present in the view");
        check(pm.parallax_q16 && pm.parallax_q16[0] == (1 << 15) && pm.parallax_q16[1] == 0,
              "Q16 factors survive bake -> mount (0.5, 0) past the alignment pad");
        check((reinterpret_cast<uintptr_t>(pm.parallax_q16) & 3u) == 0, "Q16 table is 4-aligned");
    }

    // The original (no-parallax) map must NOT grow a table — old-format blobs read back as-is.
    check(m.parallax_q16 == nullptr, "map without factors has no parallax table");

    // Render the imported map and confirm the tiles reach the framebuffer.
    auto rr = Renderer::create(plat->gfx(), arena, caps());
    Renderer* r = rr.unwrap();
    TextureView t = cache->texture("tiles"_hash).unwrap();
    TextureDesc td{}; td.pixels = t.pixels; td.width = t.width; td.height = t.height; td.format = t.format;
    TextureId tiles_tex = r->load_texture(td);
    TilemapDesc md{}; md.indices = m.indices; md.width = m.width; md.height = m.height;
    md.layers = m.layers; md.tile_w = m.tile_w; md.tile_h = m.tile_h; md.tileset = tiles_tex;
    TilemapId map = r->upload_tilemap(md);

    r->begin_frame(Camera2D{});
    r->draw_tilemap(map, 0);
    r->end_frame();

    phx_soft_fb fb = phx_gfx_soft_lock(plat->gfx());
    auto px = [&](int x, int y) { return fb.pixels[y * fb.w + x]; };
    check(px(3, 3)  == kBlue,   "tile(0,0) blue from Tiled map");
    check(px(11, 3) == kYellow, "tile(1,0) yellow from Tiled map");
    check(px(3, 11) == kYellow, "tile(0,1) yellow (checker) from Tiled map");

    plat->shutdown();
    std::printf("\ntiled_test: %d checks, %d failures\n", g_checks, g_fail);
    std::printf(g_fail == 0 ? "TILED PASS\n\n" : "TILED FAIL\n\n");
    return g_fail == 0 ? 0 : 1;
}
