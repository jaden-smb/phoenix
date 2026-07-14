// tools/phxpack/tiled.h — import a Tiled `.tmj` (JSON) map into engine-shaped data: tile layers
// (the engine's 0=empty, GID=tile+1 convention — which Tiled gives directly when firstgid==1)
// plus object groups as spawn points. Host-only (uses json.h).
//
// Collision comes from the map itself two ways (docs/10 §4): without tileset metadata every
// non-empty tile on the gameplay layer is solid (the physics TileGrid's solid_from fallback);
// WITH metadata — Tiled tileset per-tile `class` ("solid"/"oneway"/"hazard") or boolean
// properties of the same names — a per-tile flags table is baked into the Tilemap asset, so
// authors mix decorative, solid, one-way, and hazard tiles freely in one tileset.
#ifndef PHX_TOOLS_TILED_H
#define PHX_TOOLS_TILED_H

#include "json.h"
#include "phx/resource/bundle.h"   // kTileFlag* — the baked per-tile collision flag values

#include <cstdint>
#include <string>
#include <vector>

namespace phxtool {

struct TiledSpawn {
    std::string name;        // object name
    std::string type;        // object type/class (-> spawn type hash at bake time)
    int x = 0, y = 0, w = 0, h = 0;
};

struct TiledMap {
    int width = 0, height = 0, tile_w = 0, tile_h = 0;
    std::string tileset;                       // name of the first tileset (-> texture ref)
    std::vector<std::vector<uint16_t>> layers; // tile-layer GIDs (flip bits stripped)
    std::vector<std::string> layer_names;      // one per tile layer, same order as `layers`
    // Tiled's native per-layer parallax factors (parallaxx/parallaxy, default 1) — one pair
    // per tile layer, same order as `layers`. Carried into the baked Tilemap as Q16.
    std::vector<std::pair<double,double>> layer_parallax;
    // Per-tile collision flags (kTileFlag*), indexed by GID (0 = empty). Empty when the
    // tileset carries no collision metadata. Carried into the baked Tilemap when any is set.
    std::vector<uint8_t> tile_flags;
    std::vector<TiledSpawn> spawns;            // flattened from every object group
    bool has_parallax() const {
        for (auto& p : layer_parallax) if (p.first != 1.0 || p.second != 1.0) return true;
        return false;
    }
    bool has_tile_flags() const {
        for (uint8_t f : tile_flags) if (f) return true;
        return false;
    }
};

// Tiled GIDs carry flip/rotate flags in the top 4 bits; the index is the low 28.
inline uint32_t tiled_gid_index(double raw) { return uint32_t(int64_t(raw)) & 0x0FFFFFFFu; }

// Map a Tiled per-tile class string / property name to a collision flag bit (0 = not one).
inline uint8_t tiled_flag_of(const std::string& s) {
    if (s == "solid")                    return phx::kTileFlagSolid;
    if (s == "oneway" || s == "one_way") return phx::kTileFlagOneWay;
    if (s == "hazard")                   return phx::kTileFlagHazard;
    return 0;
}

inline bool tiled_load(const std::string& text, TiledMap& out, std::string* err = nullptr) {
    auto fail = [&](const std::string& why) {
        if (err) *err = why;
        return false;
    };

    JsonValue root;
    std::string jerr;
    if (!JsonParser::parse(text, root, &jerr)) return fail("invalid JSON: " + jerr);
    if (!root.is_obj()) return fail("top level is not a JSON object (expected a Tiled map)");

    out.width  = root.int_at("width");
    out.height = root.int_at("height");
    out.tile_w = root.int_at("tilewidth");
    out.tile_h = root.int_at("tileheight");
    if (out.width <= 0 || out.height <= 0 || out.tile_w <= 0 || out.tile_h <= 0)
        return fail("missing/invalid map header (width, height, tilewidth, tileheight must be > 0)");

    // First tileset: its name (or image/source stem) -> the texture this map references, and
    // its per-tile metadata -> the collision flags table.
    if (const JsonValue* ts = root.find("tilesets"); ts && ts->is_arr() && !ts->arr.empty()) {
        const JsonValue& t0 = ts->arr[0];
        std::string nm = t0.str_at("name");
        if (nm.empty()) nm = t0.str_at("image");
        if (nm.empty()) nm = t0.str_at("source");
        // reduce a path/extension to a bare stem
        size_t s = nm.find_last_of("/\\"); if (s != std::string::npos) nm = nm.substr(s + 1);
        size_t d = nm.find_last_of('.');   if (d != std::string::npos) nm = nm.substr(0, d);
        out.tileset = nm;

        const int firstgid = t0.int_at("firstgid", 1);
        if (const JsonValue* tiles = t0.find("tiles"); tiles && tiles->is_arr()) {
            for (const JsonValue& tv : tiles->arr) {
                const JsonValue* idv = tv.find("id");
                if (!idv) continue;
                const int gid = firstgid + idv->as_int();
                if (gid <= 0 || gid > 0xFFFF) continue;
                uint8_t f = 0;
                // Tiled 1.9+ per-tile "class" (older exports call it "type")
                f |= tiled_flag_of(tv.str_at("class"));
                f |= tiled_flag_of(tv.str_at("type"));
                if (const JsonValue* props = tv.find("properties"); props && props->is_arr())
                    for (const JsonValue& p : props->arr)
                        if (p.find("value") && p.find("value")->as_bool())
                            f |= tiled_flag_of(p.str_at("name"));
                if (!f) continue;
                if (int(out.tile_flags.size()) <= gid) out.tile_flags.resize(size_t(gid) + 1, 0);
                out.tile_flags[size_t(gid)] |= f;
            }
        }
    }

    const JsonValue* layers = root.find("layers");
    if (!layers || !layers->is_arr()) return fail("map has no \"layers\" array");
    const size_t cells = size_t(out.width) * size_t(out.height);

    for (const JsonValue& L : layers->arr) {
        const std::string& lt = L.str_at("type");
        if (lt == "tilelayer") {
            const std::string& lname = L.str_at("name");
            const JsonValue* data = L.find("data");
            if (!data || !data->is_arr())
                return fail("tile layer '" + lname + "' has no \"data\" array "
                            "(only CSV-encoded maps are supported — set Tile Layer Format to CSV in Tiled)");
            if (data->arr.size() != cells)
                return fail("tile layer '" + lname + "' has " + std::to_string(data->arr.size()) +
                            " cells, expected width*height = " + std::to_string(cells));
            std::vector<uint16_t> idx; idx.reserve(cells);
            for (const JsonValue& g : data->arr) {
                const uint32_t gi = tiled_gid_index(g.as_num());
                idx.push_back(uint16_t(gi));                  // small tilesets fit uint16
            }
            out.layers.push_back(std::move(idx));
            out.layer_names.push_back(lname.empty()
                                      ? "layer" + std::to_string(out.layers.size() - 1) : lname);
            const JsonValue* px = L.find("parallaxx");
            const JsonValue* py = L.find("parallaxy");
            out.layer_parallax.emplace_back(px ? px->as_num(1.0) : 1.0,
                                            py ? py->as_num(1.0) : 1.0);
        } else if (lt == "objectgroup") {
            const JsonValue* objs = L.find("objects");
            if (objs && objs->is_arr()) {
                for (const JsonValue& o : objs->arr) {
                    TiledSpawn sp;
                    sp.name = o.str_at("name");
                    sp.type = o.str_at("type");
                    if (sp.type.empty()) sp.type = o.str_at("class");   // Tiled 1.9+ renamed type->class
                    sp.x = o.int_at("x"); sp.y = o.int_at("y");
                    sp.w = o.int_at("width"); sp.h = o.int_at("height");
                    out.spawns.push_back(std::move(sp));
                }
            }
        }
    }
    if (out.layers.empty()) return fail("map has no tile layers");
    return true;
}

} // namespace phxtool
#endif // PHX_TOOLS_TILED_H
