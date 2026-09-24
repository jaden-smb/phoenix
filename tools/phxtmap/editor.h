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
#include "json.h"    // tools/phxpack — for the tileset "image" the importer does not keep

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
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
    // The tileset's source image as written in the file (Tiled's tileset "image"): the editor
    // draws the REAL tiles from it; the bake keys the texture off the tileset name (its stem).
    std::string tileset_image;
    int tileset_cols = 0, tileset_count = 0;     // known once the GUI decoded the image (for Tiled)
    int tileset_img_w = 0, tileset_img_h = 0;

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
        out.tileset_image.clear();
        JsonValue root;
        if (JsonParser::parse(tmj_text, root, nullptr))
            if (const JsonValue* ts = root.find("tilesets"); ts && ts->is_arr() && !ts->arr.empty()) {
                out.tileset_image = ts->arr[0].str_at("image");
                out.tileset_cols = ts->arr[0].int_at("columns", 0);
                out.tileset_count = ts->arr[0].int_at("tilecount", 0);
                out.tileset_img_w = ts->arr[0].int_at("imagewidth", 0);
                out.tileset_img_h = ts->arr[0].int_at("imageheight", 0);
            }
        out.undo_.clear(); out.redo_.clear();
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

    // ---- area tools (the picker is just tile()) ----
    // 4-connected flood fill of the contiguous same-GID region containing (x, y). Returns
    // the number of cells changed — 0 when out of bounds or the region is already `gid`,
    // so the GUI can drop the undo step for a no-op click.
    int flood_fill(int layer, int x, int y, uint16_t gid) {
        if (layer < 0 || layer >= int(layers.size()) || !in_bounds(x, y)) return 0;
        std::vector<uint16_t>& L = layers[size_t(layer)];
        const uint16_t from = L[size_t(y) * width + x];
        if (from == gid) return 0;
        int changed = 0;
        std::vector<std::pair<int, int>> stack{ { x, y } };
        while (!stack.empty()) {
            const auto [cx, cy] = stack.back();
            stack.pop_back();
            if (!in_bounds(cx, cy)) continue;
            uint16_t& cell = L[size_t(cy) * width + cx];
            if (cell != from) continue;
            cell = gid;
            ++changed;
            stack.push_back({ cx + 1, cy }); stack.push_back({ cx - 1, cy });
            stack.push_back({ cx, cy + 1 }); stack.push_back({ cx, cy - 1 });
        }
        dirty = true;
        return changed;
    }
    // Paint the axis-aligned rectangle spanning the two corner cells (either order; clamped
    // to the map). Returns the number of cells whose value actually changed.
    int fill_rect(int layer, int x0, int y0, int x1, int y1, uint16_t gid) {
        if (layer < 0 || layer >= int(layers.size())) return 0;
        if (x0 > x1) std::swap(x0, x1);
        if (y0 > y1) std::swap(y0, y1);
        x0 = std::max(x0, 0);          y0 = std::max(y0, 0);
        x1 = std::min(x1, width - 1);  y1 = std::min(y1, height - 1);
        int changed = 0;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                uint16_t& cell = layers[size_t(layer)][size_t(y) * width + x];
                if (cell != gid) { cell = gid; ++changed; }
            }
        if (changed) dirty = true;
        return changed;
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

    bool remove_layer(int l) {
        if (l < 0 || l >= int(layers.size()) || layers.size() == 1) return false;   // keep >= 1
        layers.erase(layers.begin() + l);
        layer_names.erase(layer_names.begin() + l);
        if (l < int(layer_parallax.size())) layer_parallax.erase(layer_parallax.begin() + l);
        dirty = true;
        return true;
    }
    // Move layer l one step toward the back (dir -1) or front (+1). The LAST layer is the
    // gameplay/solid layer, so reordering changes what collides — the GUI says so.
    bool move_layer(int l, int dir) {
        const int to = l + dir;
        if (l < 0 || to < 0 || l >= int(layers.size()) || to >= int(layers.size())) return false;
        std::swap(layers[size_t(l)], layers[size_t(to)]);
        std::swap(layer_names[size_t(l)], layer_names[size_t(to)]);
        while (layer_parallax.size() < layers.size()) layer_parallax.emplace_back(1.0, 1.0);
        std::swap(layer_parallax[size_t(l)], layer_parallax[size_t(to)]);
        dirty = true;
        return true;
    }
    void rename_layer(int l, const std::string& n) {
        if (l < 0 || l >= int(layers.size()) || n.empty() || layer_names[size_t(l)] == n) return;
        layer_names[size_t(l)] = n;
        dirty = true;
    }
    void set_parallax(int l, double fx, double fy) {
        if (l < 0 || l >= int(layers.size())) return;
        while (layer_parallax.size() < layers.size()) layer_parallax.emplace_back(1.0, 1.0);
        if (layer_parallax[size_t(l)] == std::make_pair(fx, fy)) return;
        layer_parallax[size_t(l)] = { fx, fy };
        dirty = true;
    }
    // New map size in tiles; `ax`/`ay` in {0,1,2} anchor the old cells left/centre/right,
    // top/middle/bottom. Spawns move with the anchor; the ones that fall outside are dropped.
    void resize(int nw, int nh, int ax = 0, int ay = 0) {
        nw = std::max(1, std::min(nw, 4096)); nh = std::max(1, std::min(nh, 4096));
        if (nw == width && nh == height) return;
        const int ox = ax == 0 ? 0 : ax == 1 ? (nw - width) / 2 : nw - width;
        const int oy = ay == 0 ? 0 : ay == 1 ? (nh - height) / 2 : nh - height;
        for (auto& L : layers) {
            std::vector<uint16_t> n(size_t(nw) * size_t(nh), 0);
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x) {
                    const int tx = x + ox, ty = y + oy;
                    if (tx >= 0 && ty >= 0 && tx < nw && ty < nh) n[size_t(ty) * size_t(nw) + size_t(tx)] = L[size_t(y) * size_t(width) + size_t(x)];
                }
            L.swap(n);
        }
        std::vector<TiledSpawn> keep;
        for (TiledSpawn s : spawns) {
            s.x += ox * tile_w; s.y += oy * tile_h;
            if (s.x >= 0 && s.y >= 0 && s.x < nw * tile_w && s.y < nh * tile_h) keep.push_back(s);
        }
        spawns.swap(keep);
        width = nw; height = nh;
        dirty = true;
    }

    // ---- stamps: a rectangular block of GIDs (0 = transparent in the stamp) ----
    struct Stamp { int w = 1, h = 1; std::vector<uint16_t> gids{ 1 }; };
    Stamp copy_stamp(int layer, int x0, int y0, int x1, int y1) const {
        if (x0 > x1) std::swap(x0, x1);
        if (y0 > y1) std::swap(y0, y1);
        Stamp st;
        st.w = x1 - x0 + 1; st.h = y1 - y0 + 1;
        st.gids.assign(size_t(st.w) * size_t(st.h), 0);
        for (int y = 0; y < st.h; ++y) for (int x = 0; x < st.w; ++x) st.gids[size_t(y) * size_t(st.w) + size_t(x)] = tile(layer, x0 + x, y0 + y);
        return st;
    }
    // Paint the stamp with its top-left at (x, y); `skip_empty` keeps cells under stamp 0s.
    int paint_stamp(int layer, int x, int y, const Stamp& st, bool skip_empty = true) {
        int changed = 0;
        for (int j = 0; j < st.h; ++j)
            for (int i = 0; i < st.w; ++i) {
                const uint16_t g = st.gids[size_t(j) * size_t(st.w) + size_t(i)];
                if (skip_empty && g == 0) continue;
                if (!in_bounds(x + i, y + j) || tile(layer, x + i, y + j) == g) continue;
                set_tile(layer, x + i, y + j, g);
                ++changed;
            }
        return changed;
    }
    // Replace every `from` GID on the layer (or on all layers when layer < 0).
    int replace_gid(int layer, uint16_t from, uint16_t to) {
        int k = 0;
        for (int l = 0; l < int(layers.size()); ++l) {
            if (layer >= 0 && l != layer) continue;
            for (uint16_t& g : layers[size_t(l)]) if (g == from && from != to) { g = to; ++k; }
        }
        if (k) dirty = true;
        return k;
    }
    // Cells of each GID in use on a layer (index = GID) — the palette's usage badges.
    std::vector<int> gid_usage(int layer) const {
        std::vector<int> u;
        if (layer < 0 || layer >= int(layers.size())) return u;
        for (uint16_t g : layers[size_t(layer)]) { if (g >= u.size()) u.resize(size_t(g) + 1, 0); ++u[g]; }
        return u;
    }

    // ---- spawns ----
    // The topmost spawn whose marker (a tile-sized box at its position, or its own w/h when
    // set) contains the pixel (px, py); -1 when none.
    int spawn_at(int px, int py) const {
        for (size_t i = spawns.size(); i-- > 0; ) {
            const TiledSpawn& s = spawns[i];
            const int w = s.w > 0 ? s.w : tile_w, h = s.h > 0 ? s.h : tile_h;
            if (px >= s.x && py >= s.y && px < s.x + w && py < s.y + h) return int(i);
        }
        return -1;
    }
    void move_spawn(int i, int x, int y) {
        if (i < 0 || i >= int(spawns.size())) return;
        if (spawns[size_t(i)].x == x && spawns[size_t(i)].y == y) return;
        spawns[size_t(i)].x = x; spawns[size_t(i)].y = y;
        dirty = true;
    }
    void remove_spawn(int i) {
        if (i < 0 || i >= int(spawns.size())) return;
        spawns.erase(spawns.begin() + i);
        dirty = true;
    }
    // A spawn name not in use yet ("coin", "coin2", ...).
    std::string fresh_spawn_name(const std::string& base) const {
        auto used = [&](const std::string& n) { for (const TiledSpawn& s : spawns) if (s.name == n) return true; return false; };
        if (!used(base)) return base;
        for (int k = 2;; ++k) { const std::string n = base + std::to_string(k); if (!used(n)) return n; }
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
    bool can_undo() const { return !undo_.empty(); }
    bool can_redo() const { return !redo_.empty(); }
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
        if (!tileset_image.empty()) {
            // Enough of Tiled's tileset schema for Tiled itself to open the map with its art.
            j += ",\"image\":\"" + tileset_image + "\",\"tilewidth\":" + std::to_string(tile_w) +
                 ",\"tileheight\":" + std::to_string(tile_h);
            if (tileset_cols > 0)  j += ",\"columns\":" + std::to_string(tileset_cols);
            if (tileset_count > 0) j += ",\"tilecount\":" + std::to_string(tileset_count);
            if (tileset_img_w > 0) j += ",\"imagewidth\":" + std::to_string(tileset_img_w) +
                                        ",\"imageheight\":" + std::to_string(tileset_img_h);
        }
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

    // Everything an edit gesture can touch (incl. the map size: resize is undoable).
    struct Snapshot {
        int width, height;
        std::vector<std::vector<uint16_t>> layers;
        std::vector<std::string> layer_names;
        std::vector<std::pair<double,double>> layer_parallax;
        std::vector<uint8_t> tile_flags;
        std::vector<TiledSpawn> spawns;
    };
    Snapshot snap() const { return Snapshot{ width, height, layers, layer_names, layer_parallax, tile_flags, spawns }; }
    void restore(const Snapshot& s) {
        width = s.width; height = s.height;
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
