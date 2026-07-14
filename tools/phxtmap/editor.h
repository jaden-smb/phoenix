// tools/phxtmap/editor.h — the tilemap editor's DOCUMENT model, separated from the GUI so
// it is unit-testable headlessly. Loads/saves the open Tiled `.tmj` JSON (docs/08 §1: editors
// output AUTHOR formats that the converters bake — never engine blobs), edits tile cells,
// spawn objects, per-GID collision flags, and layers in place, with bounded undo/redo, and
// round-trips through the same `tiled_load` the bake path uses — INCLUDING the tileset
// collision metadata, so editing a map never strips what Tiled (or this editor) authored.
// Host-only (STL fine).
#ifndef PHX_TOOLS_PHXTMAP_EDITOR_H
#define PHX_TOOLS_PHXTMAP_EDITOR_H

#include "tiled.h"   // tools/phxpack — the one Tiled importer (+ kTileFlag* via bundle.h)

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
    std::vector<uint8_t> tile_flags;                            // per-GID kTileFlag* (collision)
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
    // On failure `err` (when given) receives the importer's "line L, col C: reason" message.
    static bool load(const std::string& tmj_text, TmapDoc& out, std::string* err = nullptr) {
        TiledMap tm;
        if (!tiled_load(tmj_text, tm, err)) return false;
        out.width = tm.width; out.height = tm.height;
        out.tile_w = tm.tile_w; out.tile_h = tm.tile_h;
        out.tileset = tm.tileset.empty() ? "tiles" : tm.tileset;
        out.layers = tm.layers;
        out.layer_names = tm.layer_names;
        out.layer_parallax = tm.layer_parallax;
        out.tile_flags = tm.tile_flags;
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
    // Every spawn type present in the document, in first-appearance order (the GUI merges
    // these into its placeable-type list, so an opened map offers its own vocabulary).
    std::vector<std::string> spawn_types() const {
        std::vector<std::string> out;
        for (const TiledSpawn& s : spawns) {
            if (s.type.empty()) continue;
            bool seen = false;
            for (const std::string& t : out) if (t == s.type) { seen = true; break; }
            if (!seen) out.push_back(s.type);
        }
        return out;
    }

    // ---- per-GID collision flags (kTileFlag*, baked into the Tilemap asset) ----
    uint8_t tile_flag(uint16_t gid) const {
        return gid < tile_flags.size() ? tile_flags[gid] : uint8_t(0);
    }
    void set_tile_flag(uint16_t gid, uint8_t flags) {
        if (gid == 0) return;                                    // GID 0 is empty air
        if (tile_flags.size() <= gid) tile_flags.resize(size_t(gid) + 1, 0);
        tile_flags[gid] = flags;
        dirty = true;
    }
    // One-key authoring: none -> solid -> oneway -> hazard -> none.
    void cycle_tile_flag(uint16_t gid) {
        const uint8_t f = tile_flag(gid);
        uint8_t next = 0;
        if      (f == 0)                    next = phx::kTileFlagSolid;
        else if (f & phx::kTileFlagSolid)   next = phx::kTileFlagOneWay;
        else if (f & phx::kTileFlagOneWay)  next = phx::kTileFlagHazard;
        set_tile_flag(gid, next);
    }
    bool has_tile_flags() const {
        for (uint8_t f : tile_flags) if (f) return true;
        return false;
    }

    void add_layer(const std::string& name) {
        layers.emplace_back(size_t(width) * height, 0);
        layer_names.push_back(name.empty() ? "layer" + std::to_string(layers.size() - 1) : name);
        layer_parallax.emplace_back(1.0, 1.0);
        dirty = true;
    }

    // ---- undo/redo (bounded snapshots; the GUI pushes one per edit gesture) ----
    // Call push_undo() BEFORE a gesture mutates the document (a paint stroke counts as one
    // gesture, so click-drag paints undo in one step).
    void push_undo() {
        undo_.push_back(snap());
        if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
        redo_.clear();
    }
    bool undo() {
        if (undo_.empty()) return false;
        redo_.push_back(snap());
        restore(undo_.back());
        undo_.pop_back();
        dirty = true;
        return true;
    }
    bool redo() {
        if (redo_.empty()) return false;
        undo_.push_back(snap());
        restore(redo_.back());
        redo_.pop_back();
        dirty = true;
        return true;
    }
    size_t undo_depth() const { return undo_.size(); }
    size_t redo_depth() const { return redo_.size(); }
    // Discard the most recent push_undo() — for a gesture that turned out to be a no-op
    // (e.g. an erase click that hit nothing), so undo never "does nothing".
    void drop_undo() { if (!undo_.empty()) undo_.pop_back(); }

    // ---- save: emit Tiled-compatible `.tmj` JSON (the exact dialect tiled_load parses) ----
    std::string save_tmj() const {
        std::string j = "{ \"width\":" + std::to_string(width) +
                        ", \"height\":" + std::to_string(height) +
                        ", \"tilewidth\":" + std::to_string(tile_w) +
                        ", \"tileheight\":" + std::to_string(tile_h) + ",";
        j += "\"tilesets\":[{\"firstgid\":1,\"name\":\"" + tileset + "\"";
        if (has_tile_flags()) {
            // Per-tile collision as boolean properties (the most general Tiled form — a tile
            // may carry several flags); id is 0-based, GID = firstgid + id.
            j += ",\"tiles\":[";
            bool first = true;
            for (size_t gid = 1; gid < tile_flags.size(); ++gid) {
                const uint8_t f = tile_flags[gid];
                if (!f) continue;
                if (!first) j += ",";
                first = false;
                j += "{\"id\":" + std::to_string(gid - 1) + ",\"properties\":[";
                bool fp = true;
                auto prop = [&](const char* name) {
                    if (!fp) j += ",";
                    fp = false;
                    j += std::string("{\"name\":\"") + name + "\",\"type\":\"bool\",\"value\":true}";
                };
                if (f & phx::kTileFlagSolid)  prop("solid");
                if (f & phx::kTileFlagOneWay) prop("oneway");
                if (f & phx::kTileFlagHazard) prop("hazard");
                j += "]}";
            }
            j += "]";
        }
        j += "}],";
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
    static constexpr size_t kMaxUndo = 64;

    // Everything an edit gesture can touch (geometry/tileset changes have no gesture).
    struct Snapshot {
        std::vector<std::vector<uint16_t>> layers;
        std::vector<std::string> layer_names;
        std::vector<std::pair<double,double>> layer_parallax;
        std::vector<uint8_t> tile_flags;
        std::vector<TiledSpawn> spawns;
    };
    Snapshot snap() const { return Snapshot{ layers, layer_names, layer_parallax, tile_flags, spawns }; }
    void restore(const Snapshot& s) {
        layers = s.layers; layer_names = s.layer_names; layer_parallax = s.layer_parallax;
        tile_flags = s.tile_flags; spawns = s.spawns;
    }

    std::vector<Snapshot> undo_, redo_;

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
