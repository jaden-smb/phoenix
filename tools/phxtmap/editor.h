// tools/phxtmap/editor.h — the tilemap editor's DOCUMENT model, separated from the GUI so
// it is unit-testable headlessly. Loads/saves the open Tiled `.tmj` JSON (docs/08 §1: editors
// output AUTHOR formats that the converters bake — never engine blobs), edits tile cells and
// spawn objects in place, and round-trips through the same `tiled_load` the bake path uses.
// Host-only (STL fine).
#ifndef PHX_TOOLS_PHXTMAP_EDITOR_H
#define PHX_TOOLS_PHXTMAP_EDITOR_H

#include "tiled.h"   // tools/phxpack — the one Tiled importer

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace phxtool {

class TmapDoc {
public:
    int width = 16, height = 12, tile_w = 8, tile_h = 8;
    std::string tileset = "tiles";
    std::vector<std::vector<uint16_t>> layers;                  // cell = GID (0 empty)
    std::vector<std::string> layer_names;
    std::vector<std::pair<double,double>> layer_parallax;       // 1:1 = moves with the world
    std::vector<TiledSpawn> spawns;

    // A blank single-layer document.
    static TmapDoc blank(int w, int h, int tw, int th, const std::string& ts) {
        TmapDoc d;
        d.width = w; d.height = h; d.tile_w = tw; d.tile_h = th; d.tileset = ts;
        d.layers.assign(1, std::vector<uint16_t>(size_t(w) * h, 0));
        d.layer_names.assign(1, "main");
        d.layer_parallax.assign(1, { 1.0, 1.0 });
        return d;
    }

    // Load from `.tmj` text via the real importer (whatever it accepts, the bake accepts).
    static bool load(const std::string& tmj_text, TmapDoc& out) {
        TiledMap tm;
        if (!tiled_load(tmj_text, tm)) return false;
        out.width = tm.width; out.height = tm.height;
        out.tile_w = tm.tile_w; out.tile_h = tm.tile_h;
        out.tileset = tm.tileset.empty() ? "tiles" : tm.tileset;
        out.layers = tm.layers;
        out.layer_parallax = tm.layer_parallax;
        out.layer_names.assign(out.layers.size(), "");
        for (size_t i = 0; i < out.layer_names.size(); ++i)
            out.layer_names[i] = "layer" + std::to_string(i);
        out.spawns = tm.spawns;
        return true;
    }

    // ---- edits ----
    bool in_bounds(int x, int y) const { return x >= 0 && y >= 0 && x < width && y < height; }

    uint16_t tile(int layer, int x, int y) const {
        if (layer < 0 || layer >= int(layers.size()) || !in_bounds(x, y)) return 0;
        return layers[size_t(layer)][size_t(y) * width + x];
    }
    void set_tile(int layer, int x, int y, uint16_t gid) {
        if (layer < 0 || layer >= int(layers.size()) || !in_bounds(x, y)) return;
        layers[size_t(layer)][size_t(y) * width + x] = gid;
        dirty = true;
    }

    void add_spawn(const std::string& type, int x, int y) {
        TiledSpawn s;
        s.name = type + std::to_string(spawns.size());
        s.type = type; s.x = x; s.y = y; s.w = 0; s.h = 0;
        spawns.push_back(std::move(s));
        dirty = true;
    }
    // Remove the topmost spawn whose position lands in the same tile cell as (x, y).
    bool remove_spawn_at(int x, int y) {
        for (size_t i = spawns.size(); i-- > 0; ) {
            if (spawns[i].x / tile_w == x / tile_w && spawns[i].y / tile_h == y / tile_h) {
                spawns.erase(spawns.begin() + long(i));
                dirty = true;
                return true;
            }
        }
        return false;
    }

    // ---- save: emit Tiled-compatible `.tmj` JSON (the exact dialect tiled_load parses) ----
    std::string save_tmj() const {
        std::string j = "{ \"width\":" + std::to_string(width) +
                        ", \"height\":" + std::to_string(height) +
                        ", \"tilewidth\":" + std::to_string(tile_w) +
                        ", \"tileheight\":" + std::to_string(tile_h) + ",";
        j += "\"tilesets\":[{\"firstgid\":1,\"name\":\"" + tileset + "\"}],";
        j += "\"layers\":[";
        for (size_t l = 0; l < layers.size(); ++l) {
            if (l) j += ",";
            j += "{\"type\":\"tilelayer\",\"name\":\"" + layer_names[l] + "\"";
            j += ",\"width\":" + std::to_string(width) + ",\"height\":" + std::to_string(height);
            const auto par = l < layer_parallax.size() ? layer_parallax[l] : std::make_pair(1.0, 1.0);
            if (par.first != 1.0)  j += ",\"parallaxx\":" + num(par.first);
            if (par.second != 1.0) j += ",\"parallaxy\":" + num(par.second);
            j += ",\"data\":[";
            for (size_t i = 0; i < layers[l].size(); ++i) {
                if (i) j += ",";
                j += std::to_string(layers[l][i]);
            }
            j += "]}";
        }
        if (!spawns.empty()) {
            j += ",{\"type\":\"objectgroup\",\"name\":\"entities\",\"objects\":[";
            for (size_t i = 0; i < spawns.size(); ++i) {
                if (i) j += ",";
                const TiledSpawn& s = spawns[i];
                j += "{\"name\":\"" + s.name + "\",\"type\":\"" + s.type + "\"";
                j += ",\"x\":" + std::to_string(s.x) + ",\"y\":" + std::to_string(s.y);
                j += ",\"width\":" + std::to_string(s.w) + ",\"height\":" + std::to_string(s.h) + "}";
            }
            j += "]}";
        }
        j += "]}";
        return j;
    }

    bool save_file(const std::string& path) {
        const std::string j = save_tmj();
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
        const bool ok = std::fwrite(j.data(), 1, j.size(), f) == j.size();
        std::fclose(f);
        if (ok) dirty = false;
        return ok;
    }

    bool dirty = false;   // unsaved edits (shown in the GUI title bar)

private:
    // Compact decimal for parallax factors (avoids "0.500000"); JSON has no notion of
    // precision, and tiled_load reads any decimal form back.
    static std::string num(double v) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%g", v);
        return buf;
    }
};

} // namespace phxtool
#endif // PHX_TOOLS_PHXTMAP_EDITOR_H
