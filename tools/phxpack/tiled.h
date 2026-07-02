// tools/phxpack/tiled.h — import a Tiled `.tmj` (JSON) map into engine-shaped data: tile layers
// (the engine's 0=empty, GID=tile+1 convention — which Tiled gives directly when firstgid==1)
// plus object groups as spawn points. Host-only (uses json.h). Collision is the tilemap itself
// (the physics TileGrid treats tiles >= solid_from as solid), so no separate collision asset.
#ifndef PHX_TOOLS_TILED_H
#define PHX_TOOLS_TILED_H

#include "json.h"

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
    // Tiled's native per-layer parallax factors (parallaxx/parallaxy, default 1) — one pair
    // per tile layer, same order as `layers`. Carried into the baked Tilemap as Q16.
    std::vector<std::pair<double,double>> layer_parallax;
    std::vector<TiledSpawn> spawns;            // flattened from every object group
    bool has_parallax() const {
        for (auto& p : layer_parallax) if (p.first != 1.0 || p.second != 1.0) return true;
        return false;
    }
};

// Tiled GIDs carry flip/rotate flags in the top 4 bits; the index is the low 28.
inline uint32_t tiled_gid_index(double raw) { return uint32_t(int64_t(raw)) & 0x0FFFFFFFu; }

inline bool tiled_load(const std::string& text, TiledMap& out) {
    JsonValue root;
    if (!JsonParser::parse(text, root) || !root.is_obj()) return false;

    out.width  = root.int_at("width");
    out.height = root.int_at("height");
    out.tile_w = root.int_at("tilewidth");
    out.tile_h = root.int_at("tileheight");
    if (out.width <= 0 || out.height <= 0 || out.tile_w <= 0 || out.tile_h <= 0) return false;

    // First tileset's name (or its image/source stem) -> the texture this map references.
    if (const JsonValue* ts = root.find("tilesets"); ts && ts->is_arr() && !ts->arr.empty()) {
        const JsonValue& t0 = ts->arr[0];
        std::string nm = t0.str_at("name");
        if (nm.empty()) nm = t0.str_at("image");
        if (nm.empty()) nm = t0.str_at("source");
        // reduce a path/extension to a bare stem
        size_t s = nm.find_last_of("/\\"); if (s != std::string::npos) nm = nm.substr(s + 1);
        size_t d = nm.find_last_of('.');   if (d != std::string::npos) nm = nm.substr(0, d);
        out.tileset = nm;
    }

    const JsonValue* layers = root.find("layers");
    if (!layers || !layers->is_arr()) return false;
    const size_t cells = size_t(out.width) * size_t(out.height);

    for (const JsonValue& L : layers->arr) {
        const std::string& lt = L.str_at("type");
        if (lt == "tilelayer") {
            const JsonValue* data = L.find("data");
            if (!data || !data->is_arr() || data->arr.size() != cells) return false;  // CSV-encoded only
            std::vector<uint16_t> idx; idx.reserve(cells);
            for (const JsonValue& g : data->arr) {
                const uint32_t gi = tiled_gid_index(g.as_num());
                idx.push_back(uint16_t(gi));                  // small tilesets fit uint16
            }
            out.layers.push_back(std::move(idx));
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
    if (out.layers.empty()) return false;
    return true;
}

} // namespace phxtool
#endif // PHX_TOOLS_TILED_H
