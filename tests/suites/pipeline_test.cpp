// tests/pipeline_test.cpp — the two-stage asset pipeline end to end (docs/08): the per-format
// CONVERTERS (phxsprite/phxtile/phxsnd/phxbin) bake author sources into intermediate `.phx*`
// files, then the phxpack ASSEMBLER MERGES those intermediates into one `assets.phxp`, which the
// runtime ResourceCache mounts and reads back. Exercises the real converter builders + the
// intermediate write/read(merge) paths in-process, then verifies every asset type from the
// assembled bundle. It also drops the source fixtures + runs the .gen.h emit so the CLI smoke
// (`make tools` / ctest `tools_cli`) can re-run the actual tool binaries over them.
#include "phx/platform/platform.h"
#include "phx/resource/cache.h"
#include "phx/core/caps.h"

#include "builders.h"        // the converter bake logic
#include "bundle_reader.h"   // the assembler's merge logic
#include "editor.h"                        // phxtmap's document model (load/edit/save .tmj)
#include "../../tools/phxentity/editor.h"     // phxentity's document model (phxbin JSON tables)

#include "fixtures/png_fixtures.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace phx;

namespace {
int g_checks = 0, g_fail = 0;
void check(bool ok, const char* what) { ++g_checks; if (!ok) { ++g_fail; std::printf("    FAIL %s\n", what); } }

void write_file(const char* path, const void* data, size_t n) {
    if (FILE* f = std::fopen(path, "wb")) { std::fwrite(data, 1, n, f); std::fclose(f); }
}

// Minimal 16-bit mono WAV (RIFF), same layout as audio_test's.
std::vector<uint8_t> make_wav_mono16(uint32_t rate, const int16_t* s, uint32_t frames) {
    auto put16 = [](std::vector<uint8_t>& v, uint16_t x){ v.push_back(uint8_t(x)); v.push_back(uint8_t(x>>8)); };
    auto put32 = [](std::vector<uint8_t>& v, uint32_t x){ for (int i=0;i<4;++i) v.push_back(uint8_t(x>>(8*i))); };
    auto tag   = [](std::vector<uint8_t>& v, const char* t){ for (int i=0;i<4;++i) v.push_back(uint8_t(t[i])); };
    std::vector<uint8_t> w;
    const uint32_t dataSize = frames * 2;
    tag(w,"RIFF"); put32(w,36+dataSize); tag(w,"WAVE");
    tag(w,"fmt "); put32(w,16); put16(w,1); put16(w,1); put32(w,rate); put32(w,rate*2); put16(w,2); put16(w,16);
    tag(w,"data"); put32(w,dataSize);
    for (uint32_t i = 0; i < frames; ++i) { w.push_back(uint8_t(s[i])); w.push_back(uint8_t(s[i]>>8)); }
    return w;
}

// A small Tiled map (one tile layer + one object group), like tiled_test's.
const char* kTmj =
"{ \"width\":3, \"height\":2, \"tilewidth\":8, \"tileheight\":8,"
"  \"tilesets\":[ { \"firstgid\":1, \"name\":\"tiles\" } ],"
"  \"layers\":["
"    { \"type\":\"tilelayer\", \"name\":\"ground\", \"width\":3, \"height\":2, \"data\":[1,2,1,2,1,2] },"
"    { \"type\":\"objectgroup\", \"name\":\"things\", \"objects\":["
"       { \"name\":\"p\", \"type\":\"player\", \"x\":8,  \"y\":16, \"width\":8, \"height\":8 },"
"       { \"name\":\"c\", \"type\":\"coin\",   \"x\":16, \"y\":8,  \"width\":8, \"height\":8 } ] } ] }";

const char* kSprdef = "sheet p_sheet.png 2 2\nclip walk 0 4 8 1\nclip idle 0 1 0 0\n";

const char* kItems =
"{ \"struct\":\"ItemRecord\","
"  \"fields\":[ {\"name\":\"id\",\"type\":\"u16\"}, {\"name\":\"price\",\"type\":\"u32\"}, {\"name\":\"atk\",\"type\":\"i16\"} ],"
"  \"records\":[ {\"id\":1,\"price\":100,\"atk\":5}, {\"id\":2,\"price\":250,\"atk\":-3} ] }";

// Assemble (== phxpack's merge): read each intermediate bundle and re-add its assets by hash.
void merge(phxtool::BundleWriter& out, const char* path) {
    std::vector<phxtool::ReadAsset> assets;
    check(phxtool::bundle_read(path, assets), "read intermediate bundle");
    for (auto& a : assets) out.add_raw(a.hash, a.type, std::move(a.blob));
}
} // namespace

int main() {
    // --- 1. drop author-source fixtures to disk -----------------------------
    write_file("build/p_sheet.png", kSheet8x2, sizeof(kSheet8x2));
    write_file("build/p_hero.sprdef", kSprdef, std::strlen(kSprdef));
    write_file("build/p_level.tmj", kTmj, std::strlen(kTmj));
    const int16_t tone[8] = { 0, 1000, 2000, 3000, -3000, -2000, -1000, 0 };
    std::vector<uint8_t> wav = make_wav_mono16(22050, tone, 8);
    write_file("build/p_tone.wav", wav.data(), wav.size());
    write_file("build/p_items.json", kItems, std::strlen(kItems));

    // --- 2. CONVERTERS: each source -> a one-asset(-group) intermediate bundle ----
    { phxtool::BundleWriter w(2); check(phxtool::build_sprite(w, "build/p_hero.sprdef", "hero"), "phxsprite build");
      check(w.write("build/p_hero.phxspr"), "write .phxspr"); }
    { phxtool::BundleWriter w(2); check(phxtool::build_tmj(w, "build/p_level.tmj", "level"), "phxtile build");
      check(w.write("build/p_level.phxtmap"), "write .phxtmap"); }
    { phxtool::BundleWriter w(2); check(phxtool::build_wav(w, "build/p_tone.wav", "tone"), "phxsnd build");
      check(w.write("build/p_tone.phxsnd"), "write .phxsnd"); }
    { phxtool::BundleWriter w(2); check(phxtool::build_bin(w, "build/p_items.json", "items", "build/p_items.gen.h"), "phxbin build");
      check(w.write("build/p_items.phxbin"), "write .phxbin"); }

    // --- 2b. bake determinism (docs/08 §9): identical input -> byte-identical bundle ----
    {
        phxtool::BundleWriter w2(2);
        check(phxtool::build_sprite(w2, "build/p_hero.sprdef", "hero"), "re-bake hero");
        check(w2.write("build/p_hero2.phxspr"), "write re-baked .phxspr");
        std::vector<uint8_t> a, b;
        check(phxtool::read_file("build/p_hero.phxspr", a)
              && phxtool::read_file("build/p_hero2.phxspr", b) && a == b,
              "two bakes of the same source are byte-identical (reproducible)");
    }

    // --- 3. ASSEMBLER: merge the intermediates into one bundle (phxpack) -----
    const char* bundle = "build/p_assets.phxp";
    { phxtool::BundleWriter out(2);
      merge(out, "build/p_hero.phxspr");
      merge(out, "build/p_level.phxtmap");
      merge(out, "build/p_tone.phxsnd");
      merge(out, "build/p_items.phxbin");
      check(out.write(bundle), "assemble merged bundle"); }

    // --- 4. mount the assembled bundle and verify every asset type ----------
    const phx_platform* plat = phx_platform_get();
    phx_platform_desc desc{}; desc.title = "pipeline_test"; desc.width = 16; desc.height = 16;
    if (plat->init(&desc) != 0) { std::printf("platform init failed\n"); return 1; }

    static uint8_t arena_buf[8 << 20];
    ArenaAllocator arena; arena.init(arena_buf, sizeof(arena_buf));
    ResourceCache* cache = ResourceCache::create(arena).unwrap();
    check(cache->mount(plat, bundle) == Status::Ok, "mount assembled bundle");

    // sprite (+ its sheet texture), from phxsprite
    auto tv = cache->texture("p_sheet"_hash);
    check(tv.ok() && tv.unwrap().width == 8 && tv.unwrap().height == 2, "sheet texture merged from .phxspr");
    auto spr = cache->sprite("hero"_hash);
    check(spr.ok(), "sprite('hero') merged");
    if (spr.ok()) { SpriteView s = spr.unwrap();
        check(s.texture == "p_sheet"_hash && s.frame_w == 2 && s.frame_h == 2 && s.cols == 4, "sprite frame grid");
        check(s.clip_count == 2 && s.clips && s.clips[0].name == "walk"_hash && s.clips[0].count == 4, "sprite clips"); }

    // tilemap (+ spawns), from phxtile
    auto mv = cache->tilemap("level"_hash);
    check(mv.ok(), "tilemap('level') merged");
    if (mv.ok()) { TilemapView m = mv.unwrap();
        check(m.width == 3 && m.height == 2 && m.tileset == "tiles"_hash, "tilemap meta merged from .phxtmap");
        check(m.indices && m.indices[0] == 1 && m.indices[1] == 2 && m.indices[5] == 2, "tilemap indices"); }
    auto sp = cache->spawns("level"_hash);
    check(sp.ok() && sp.unwrap().count == 2, "spawns('level') merged");
    if (sp.ok()) { SpawnsView s = sp.unwrap();
        check(s.spawns[0].type == "player"_hash && s.spawns[0].x == 8 && s.spawns[1].type == "coin"_hash, "spawns resolve"); }

    // sound, from phxsnd
    auto snd = cache->sound("tone"_hash);
    check(snd.ok(), "sound('tone') merged");
    if (snd.ok()) { SoundDataView s = snd.unwrap();
        check(s.frames == 8 && s.rate == 22050, "sound meta from .phxsnd");
        check(s.samples && s.samples[1] == 1000 && s.samples[4] == -3000, "sound samples"); }

    // per-target encode: the SAME 22050 Hz WAV baked for tier 0 (GBA) is resampled at bake
    // time to the 18157 Hz device rate (fewer ROM bytes, 1:1 runtime mixing); tier 2 kept
    // the source rate above. First sample must survive; frame count scales by 18157/22050.
    {
        phxtool::BundleWriter w0(0);
        check(phxtool::build_wav(w0, "build/p_tone.wav", "tone"), "phxsnd build (tier 0)");
        check(w0.write("build/p_tone0.phxp"), "write tier-0 bundle");
        ResourceCache* c0 = ResourceCache::create(arena).unwrap();
        check(c0->mount(plat, "build/p_tone0.phxp") == Status::Ok, "mount tier-0 bundle");
        auto s0 = c0->sound("tone"_hash);
        check(s0.ok(), "sound('tone') tier 0");
        if (s0.ok()) { SoundDataView s = s0.unwrap();
            check(s.rate == 18157, "tier-0 sound resampled to the GBA device rate");
            check(s.frames == uint32_t((uint64_t(8) << 16) / ((uint64_t(22050) << 16) / 18157)),
                  "tier-0 frame count scaled by 18157/22050");
            check(s.samples && s.samples[0] == 0, "tier-0 first sample intact"); }
    }

    // per-target TEXTURE encode (docs/06 §4, the texture counterpart of the sound check
    // above): the same RGBA8 art bakes to PAL4_TILES on tier 0, swizzled RGBA8 on tier 1,
    // plain RGBA8 on tier 2 — and art a tier can't express honestly stays RGBA8.
    {
        static uint32_t art[8 * 8];
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                art[y * 8 + x] = (x < 4) ? rgba(216, 40, 40) : rgba(40, 216, 40);
        art[9] = 0;                                    // one transparent texel at (1,1)

        for (int tier = 0; tier <= 2; ++tier) {
            char path[64]; std::snprintf(path, sizeof(path), "build/p_tex%d.phxp", tier);
            phxtool::BundleWriter wt{uint8_t(tier)};
            wt.add_texture("art", art, 8, 8);
            if (tier == 0) wt.add_texture("odd", art, 8, 2);   // h % 8 -> not tier-0 encodable
            check(wt.write(path), "write per-tier texture bundle");

            ResourceCache* ct = ResourceCache::create(arena).unwrap();
            check(ct->mount(plat, path) == Status::Ok, "mount per-tier texture bundle");
            auto tvr = ct->texture("art"_hash);
            check(tvr.ok(), "texture('art') per tier");
            if (!tvr.ok()) continue;
            TextureView v = tvr.unwrap();
            const uint8_t* pay = static_cast<const uint8_t*>(v.pixels);
            if (tier == 0) {
                check(v.format == PixelFormat::PAL4_TILES, "tier 0 baked PAL4_TILES");
                const TexturePal4Header* ph = reinterpret_cast<const TexturePal4Header*>(pay);
                check(ph->pal_count == 1, "2-colour art fits one palette (OBJ-safe)");
                const uint16_t* pal   = reinterpret_cast<const uint16_t*>(pay + pal4_palettes_off());
                const uint8_t*  tiles = pay + pal4_tile_data_off(ph->pal_count, 1);
                check(pal4_texel(tiles, 0, 1, 1) == 0, "transparent texel -> nibble 0");
                const uint8_t s00 = pal4_texel(tiles, 0, 0, 0);
                check(s00 != 0 && pal[s00] == rgba8_to_bgr555(rgba(216, 40, 40)),
                      "PAL4 texel (0,0) decodes to the quantized source colour");
                // the fallback texture kept RGBA8 (and the runtime treats it like a v1 blob)
                auto odd = ct->texture("odd"_hash);
                check(odd.ok() && odd.unwrap().format == PixelFormat::RGBA8,
                      "non-8px-aligned art honestly stays RGBA8 on tier 0");
            } else if (tier == 1) {
                check(v.format == PixelFormat::RGBA8_SWZ, "tier 1 baked swizzled RGBA8");
                const uint32_t* px = reinterpret_cast<const uint32_t*>(pay);
                check(px[swz_texel_index(5, 3, 8)] == art[3 * 8 + 5],
                      "swizzled texel reads back bit-exact");
            } else {
                check(v.format == PixelFormat::RGBA8, "tier 2 keeps plain RGBA8");
                check(std::memcmp(pay, art, sizeof(art)) == 0, "tier-2 texels untouched");
            }
        }
    }

    // data table, from phxbin: [u32 count][u32 stride][records...], ItemRecord{u16 id; u32 price; i16 atk;}
    auto bl = cache->blob("items"_hash);
    check(bl.ok(), "blob('items') merged");
    if (bl.ok()) { BlobView b = bl.unwrap();
        const uint8_t* p = static_cast<const uint8_t*>(b.data);
        uint32_t count = 0, stride = 0;
        std::memcpy(&count, p + 0, 4); std::memcpy(&stride, p + 4, 4);
        // ItemRecord natural C layout: id u16 @0, price u32 @4 (aligned), atk i16 @8 -> pad to 12.
        check(count == 2 && stride == 12, "table count/stride matches natural C layout");
        uint16_t id0 = 0; uint32_t price0 = 0; int16_t atk1 = 0;
        std::memcpy(&id0,    p + 8 + 0, 2);
        std::memcpy(&price0, p + 8 + 4, 4);
        std::memcpy(&atk1,   p + 8 + stride + 8, 2);
        check(id0 == 1 && price0 == 100 && atk1 == -3, "table record fields"); }

    // phxtmap editor document model: blank -> paint tiles + place spawns + a parallax layer
    // -> save .tmj -> the REAL importer parses it back identically (editors emit author
    // formats the converters bake, docs/08 §1) -> erase round-trips too.
    {
        using phxtool::TmapDoc;
        TmapDoc d = TmapDoc::blank(4, 3, 8, 8, "tiles");
        d.layers.insert(d.layers.begin(), std::vector<uint16_t>(12, 0));   // backdrop under main
        d.layer_names.insert(d.layer_names.begin(), "backdrop");
        d.layer_parallax.insert(d.layer_parallax.begin(), { 0.5, 1.0 });
        d.set_tile(0, 1, 0, 7);                    // a cloud on the backdrop
        d.set_tile(1, 2, 2, 3);                    // ground on the main layer
        d.add_spawn("player", 8, 16);
        d.add_spawn("coin", 24, 16);
        check(d.tile(1, 2, 2) == 3 && d.tile(0, 1, 0) == 7, "editor set/get tiles");
        check(d.dirty, "edits mark the doc dirty");

        TmapDoc r;
        check(TmapDoc::load(d.save_tmj(), r), "editor .tmj re-parses via the real importer");
        check(r.width == 4 && r.height == 3 && r.layers.size() == 2, "round-trip dimensions/layers");
        check(r.tile(0, 1, 0) == 7 && r.tile(1, 2, 2) == 3, "round-trip painted tiles");
        check(r.layer_parallax.size() == 2 && r.layer_parallax[0].first == 0.5 &&
              r.layer_parallax[1].first == 1.0, "round-trip parallax factors");
        check(r.spawns.size() == 2 && r.spawns[0].type == "player" && r.spawns[0].x == 8 &&
              r.spawns[1].type == "coin" && r.spawns[1].x == 24, "round-trip spawns");

        check(r.remove_spawn_at(25, 17), "erase the spawn under the pointer's cell");
        check(r.spawns.size() == 1 && r.spawns[0].type == "player", "right spawn removed");
        r.set_tile(1, 2, 2, 0);
        TmapDoc r2;
        check(TmapDoc::load(r.save_tmj(), r2) && r2.tile(1, 2, 2) == 0 && r2.spawns.size() == 1,
              "erase edits survive a second round-trip");

        // Layer names authored in the doc survive the importer (not synthesized "layerN").
        check(r2.layer_names.size() == 2 && r2.layer_names[0] == "backdrop" &&
              r2.layer_names[1] == "main", "round-trip layer names");

        // The doc's spawn vocabulary is harvested for the GUI's placeable list.
        auto tys = r2.spawn_types();
        check(tys.size() == 1 && tys[0] == "player", "spawn_types harvests the doc's vocabulary");
    }

    // phxtmap editor: per-GID collision flags author-edit + round-trip (saved as Tiled
    // tileset per-tile properties, re-imported by the same tiled_load the bake uses), and
    // bounded undo/redo over paint/spawn/flag/layer gestures.
    {
        using phxtool::TmapDoc;
        TmapDoc d = TmapDoc::blank(4, 3, 8, 8, "tiles");

        // collision authoring: cycle none -> solid -> oneway -> hazard -> none
        d.cycle_tile_flag(3);
        check(d.tile_flag(3) == phx::kTileFlagSolid, "cycle: none -> solid");
        d.cycle_tile_flag(3);
        check(d.tile_flag(3) == phx::kTileFlagOneWay, "cycle: solid -> oneway");
        d.cycle_tile_flag(3);
        check(d.tile_flag(3) == phx::kTileFlagHazard, "cycle: oneway -> hazard");
        d.set_tile_flag(2, phx::kTileFlagSolid);
        d.set_tile_flag(0, phx::kTileFlagSolid);            // GID 0 is air: must be refused
        check(d.tile_flag(0) == 0, "GID 0 never takes a flag");

        TmapDoc fr;
        check(TmapDoc::load(d.save_tmj(), fr), "flagged .tmj re-parses via the real importer");
        check(fr.tile_flag(2) == phx::kTileFlagSolid && fr.tile_flag(3) == phx::kTileFlagHazard,
              "collision flags survive save -> import (tileset per-tile properties)");

        // undo/redo: each gesture is one step; redo is cleared by a new edit
        TmapDoc u = TmapDoc::blank(4, 3, 8, 8, "tiles");
        check(!u.undo() && !u.redo(), "nothing to undo/redo on a fresh doc");
        u.push_undo(); u.set_tile(0, 1, 1, 5);              // gesture 1: paint
        u.push_undo(); u.add_spawn("coin", 8, 8);           // gesture 2: spawn
        u.push_undo(); u.add_layer("fg");                   // gesture 3: layer
        check(u.layers.size() == 2 && u.spawns.size() == 1 && u.tile(0, 1, 1) == 5, "edits applied");
        check(u.undo() && u.layers.size() == 1, "undo pops the layer");
        check(u.undo() && u.spawns.empty(), "undo pops the spawn");
        check(u.undo() && u.tile(0, 1, 1) == 0, "undo pops the paint");
        check(!u.undo(), "undo stack exhausted");
        check(u.redo() && u.tile(0, 1, 1) == 5, "redo re-applies the paint");
        check(u.redo() && u.spawns.size() == 1, "redo re-applies the spawn");
        u.push_undo(); u.set_tile(0, 0, 0, 9);              // a new edit...
        check(!u.redo(), "...clears the redo stack");
        // a no-op gesture can be dropped so undo never "does nothing"
        u.push_undo();
        const size_t depth = u.undo_depth();
        u.drop_undo();
        check(u.undo_depth() == depth - 1, "drop_undo discards the no-op gesture");
    }

    // phxentity editor document model: load the items table, edit (clamped to the field's
    // declared type), clone + delete records, save — and prove the saved JSON still BAKES
    // through the real phxbin builder (editors emit author formats the converters accept).
    {
        using phxtool::BinDoc;
        BinDoc d;
        check(BinDoc::load(kItems, d), "entity editor loads the phxbin JSON");
        check(d.struct_name == "ItemRecord" && d.fields.size() == 3 && d.records.size() == 2,
              "entity doc shape");
        check(d.records[0][1] == 100 && d.records[1][2] == -3, "entity doc values");

        d.step(0, 1, +10);                              // price 100 -> 110
        d.step(1, 2, -1000000);                         // atk clamps to i16 min
        check(d.records[0][1] == 110 && d.records[1][2] == -32768, "step + type clamp");
        d.add_record(0);                                // clone record 0
        d.remove_record(1);
        check(d.records.size() == 2 && d.records[1][1] == 110, "clone + delete records");

        write_file("build/p_items_edited.json", d.save_json().data(), d.save_json().size());
        BinDoc r;
        check(BinDoc::load(d.save_json(), r) && r.records.size() == 2 &&
              r.records[1][1] == 110 && r.records[1][2] == 5, "entity doc round-trip");
        phxtool::BundleWriter wb(2);
        check(phxtool::build_bin(wb, "build/p_items_edited.json", "items2",
                                 "build/p_items_edited.gen.h"),
              "edited table still bakes through the real phxbin builder");
    }

    // phxentity schema authoring: a fresh table from a CLI-style spec (--new/--fields),
    // field add/remove keeping every record in shape, and honest failures on bad specs.
    {
        using phxtool::BinDoc;
        BinDoc d;
        std::string err;
        check(BinDoc::blank("Enemy", { "hp:u16", "atk:i8" }, d, &err), "blank table from a schema");
        check(d.struct_name == "Enemy" && d.fields.size() == 2 && d.records.empty(), "blank shape");
        d.add_record(0); d.add_record(0);
        check(d.add_field("speed", "u8"), "add a field");
        check(d.fields.size() == 3 && d.records[0].size() == 3 && d.records[1][2] == 0,
              "records grew with the schema");
        check(!d.add_field("hp", "u16"), "duplicate field name refused");
        check(!d.add_field("x", "f64"), "unknown field type refused");
        check(d.remove_field(1), "remove a field");
        check(d.fields.size() == 2 && d.fields[1].name == "speed" && d.records[0].size() == 2,
              "records shrank with the schema");

        BinDoc bad;
        check(!BinDoc::blank("Enemy", { "hp=u16" }, bad, &err) && !err.empty(),
              "bad field spec reports why");
        check(!BinDoc::blank("Enemy", {}, bad, &err), "empty schema refused");

        // the new table's saved JSON still bakes through the real phxbin builder
        const std::string j = d.save_json();
        write_file("build/p_new_table.json", j.data(), j.size());
        phxtool::BundleWriter wb(2);
        check(phxtool::build_bin(wb, "build/p_new_table.json", "enemies"),
              "schema-authored table bakes through phxbin");

        // malformed author JSON reports a positioned parse error (the diagnostics seam)
        BinDoc m;
        check(!BinDoc::load("{ \"struct\":\"X\",\n  \"fields\":[{\"name\":\"a\" \"type\":\"u8\"}] }", m, &err) &&
              err.find("line 2") != std::string::npos,
              "parse errors carry line/col positions");
    }

    // the generated accessor header exists and declares the struct
    bool gen_ok = false;
    if (FILE* h = std::fopen("build/p_items.gen.h", "rb")) {
        std::string s; int c; while ((c = std::fgetc(h)) != EOF) s += char(c); std::fclose(h);
        gen_ok = s.find("struct ItemRecord") != std::string::npos &&
                 s.find("static_assert(sizeof(ItemRecord)") != std::string::npos;
    }
    check(gen_ok, "phxbin emitted a matching .gen.h");

    plat->shutdown();
    std::printf("\npipeline_test: %d checks, %d failures\n", g_checks, g_fail);
    std::printf(g_fail == 0 ? "PIPELINE PASS\n\n" : "PIPELINE FAIL\n\n");
    return g_fail == 0 ? 0 : 1;
}
