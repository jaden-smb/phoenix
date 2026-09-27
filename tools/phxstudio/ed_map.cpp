// tools/phxstudio/ed_map.cpp — the TILEMAP editor panel (a rebuild of phxtmap's GUI on the tool
// widget kit). Edits the Tiled .tmj author format through TmapDoc (tools/phxtmap/editor.h,
// unit-tested): tile layers with the REAL tileset art, per-GID collision flags, entity spawns,
// parallax factors, map resize, multi-tile stamps, undo/redo.
//
// Each tile layer is composited on the CPU into its own RGBA image (updated cell by cell as you
// paint) and drawn as ONE clipped, integer-zoomed image through the engine renderer — so a big
// map at 1x costs a handful of sprites, not tens of thousands.
//
//   B brush  E eraser  G fill  R rectangle  I picker (drag = pick a stamp)  S select+copy
//   T spawns (click: place/select, drag: move, Delete: remove)  H hand
//   Tab / Shift+Tab   next / previous layer        V   cycle the brush tile's collision
//   Tiles tab: double-click a tile (or right-click > Edit this tile) to paint it in the pixel
//   editor; "Create tileset" turns a swatch map into a real, editable tileset PNG. The map picks up
//   the tileset again whenever its PNG is saved.
//   Ctrl+wheel zoom · wheel scroll · middle-drag pan · F fit · Ctrl+C / Ctrl+V stamps
#include "host.h"
#include "../phxtmap/editor.h"
#include "../phxentity/editor.h"
#include "pixeldoc.h"

#include "phx/render/renderer.h"
#include "phx/resource/bundle.h"   // kTileFlag*

#include <algorithm>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace phxstudio {
namespace {

using namespace twk;
using phxtool::TmapDoc;
using phxtool::TiledSpawn;
using phxtool::TiledProp;

enum class MTool : uint8_t { Brush, Eraser, Fill, Rect, Picker, Select, Spawn, Hand };

struct MToolInfo { MTool t; int icon; int key; const char* help; };
const MToolInfo kMTools[] = {
    { MTool::Brush,  kIconPencil,  'b', "Brush (B) - paints the selected tile or stamp" },
    { MTool::Eraser, kIconEraser,  'e', "Eraser (E) - clears cells (right-drag with any tool too)" },
    { MTool::Fill,   kIconBucket,  'g', "Fill (G) - the connected area of the clicked tile" },
    { MTool::Rect,   kIconRectFill,'r', "Rectangle (R) - drag to fill a box with the tile" },
    { MTool::Picker, kIconPicker,  'i', "Picker (I) - click a cell, or drag a box to pick a stamp" },
    { MTool::Select, kIconSelect,  's', "Select (S) - drag a box; Ctrl+C copies it as a stamp, Delete clears" },
    { MTool::Spawn,  kIconFlag,    't', "Spawns (T) - click to place/select, drag to move, Delete removes" },
    { MTool::Hand,   kIconHand,    'h', "Pan (H, or middle-drag)" },
};

const Rgba kSwatch[16] = {
    rgba(120, 124, 140), rgba(92, 160, 90),  rgba(170, 120, 80), rgba(80, 130, 200),
    rgba(200, 180, 90),  rgba(190, 90, 90),  rgba(150, 100, 190), rgba(90, 180, 180),
    rgba(220, 220, 220), rgba(60, 90, 60),   rgba(120, 80, 50),  rgba(40, 60, 120),
    rgba(240, 150, 60),  rgba(110, 40, 40),  rgba(200, 120, 200), rgba(60, 60, 70),
};

class MapView final : public DocView {
public:
    TmapDoc doc;

    bool load(Host& h, const std::string& p, std::string* err) {
        path = p;
        std::string text;
        {
            FILE* f = std::fopen(p.c_str(), "rb");
            if (!f) { if (err) *err = "cannot open " + p; return false; }
            char buf[65536];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
            std::fclose(f);
        }
        std::string e;
        if (!TmapDoc::load(text, doc, &e)) { if (err) *err = base_name(p) + ": " + e; return false; }
        disk_stamp = file_stamp(p);
        load_tileset(h);
        layer_ = int(doc.layers.size()) - 1;             // the gameplay layer
        hidden_.assign(doc.layers.size(), false);
        return true;
    }

    void init_new(Host& h, const std::string& p, const TmapDoc& d) {
        path = p;
        doc = d;
        doc.dirty = true;
        load_tileset(h);
        layer_ = int(doc.layers.size()) - 1;
        hidden_.assign(doc.layers.size(), false);
    }

    FileKind kind() const override { return FileKind::Map; }
    int icon() const override { return kIconFileMap; }
    bool dirty() const override { return doc.dirty; }
    bool save(Host& h, std::string* err) override {
        if (!doc.save_file(path)) { if (err) *err = "cannot write " + path; return false; }
        disk_stamp = file_stamp(path);
        h.file_saved(path);
        return true;
    }
    bool reload(Host& h, std::string* err) override {
        MapView fresh;
        if (!fresh.load(h, path, err)) return false;
        doc = fresh.doc;
        disk_stamp = fresh.disk_stamp;
        layer_ = std::min(layer_, int(doc.layers.size()) - 1);
        hidden_.resize(doc.layers.size(), false);
        return true;
    }
    void release(Host& h) override {
        phx::Renderer& r = h.renderer();
        r.unload_texture(ts_tex_); ts_tex_ = kNoTexture;
        for (Comp& c : comps_) r.unload_texture(c.tex);
        comps_.clear();
        r.unload_texture(col_.tex); col_ = Comp{};
    }
    bool undo() override { if (!doc.undo()) return false; after_structure_change(); return true; }
    bool redo() override { if (!doc.redo()) return false; after_structure_change(); return true; }
    std::string status() const override {
        std::string s = fmt("%dx%d tiles of %dx%d", doc.width, doc.height, doc.tile_w, doc.tile_h);
        s += fmt("  layer %d/%zu", layer_ + 1, doc.layers.size());
        s += fmt("  %zu spawns  zoom %dx", doc.spawns.size(), cv_.zoom);
        if (hx_ >= 0) s += fmt("  cell (%d,%d)", hx_, hy_);
        return s;
    }
    std::vector<Action> actions() override {
        std::vector<Action> a;
        a.push_back({ "Resize map...", "", [this] { want_resize_ = true; } });
        a.push_back({ "Add layer", "", [this] { doc.push_undo(); doc.add_layer(""); hidden_.push_back(false); layer_ = int(doc.layers.size()) - 1; } });
        a.push_back({ "Fit map in view", "F", [this] { cv_.fitted = false; } });
        a.push_back({ "Edit the brush tile", "", [this] { want_edit_tile_ = true; }, ts_real && can_open_ });
        a.push_back({ "Edit the tileset", "", [this] { want_edit_tileset_ = true; }, ts_real && can_open_ });
        a.push_back({ "Create a tileset image...", "", [this] { want_create_ts_ = true; }, !ts_real });
        return a;
    }

    void draw(Host& h, const Rect& area) override {
        can_open_ = h.can_open_files();
        Gui& g = h.gui();
        if (!id_) id_ = Gui::hash(path.c_str());
        g.push_id(id_);
        if (want_resize_) { want_resize_ = false; resize_dialog(h); }
        if (want_create_ts_) { want_create_ts_ = false; create_tileset_dialog(h); }
        if (want_edit_tileset_) { want_edit_tileset_ = false; if (ts_real) h.open_tile(ts_path, 0, doc.tile_w, doc.tile_h); }
        if (want_edit_tile_) { want_edit_tile_ = false; edit_tile(h, stamp_.gids.empty() ? 1 : stamp_.gids[0]); }
        // pick the tileset up again whenever its PNG changes on disk (saved from the pixel editor)
        if (ts_real && h.ticks() % 30 == 0) {
            const int64_t st = file_stamp(ts_path);
            if (st && st != ts_stamp_) load_tileset(h);
        }
        if (hidden_.size() != doc.layers.size()) hidden_.resize(doc.layers.size(), false);
        layer_ = std::max(0, std::min(layer_, int(doc.layers.size()) - 1));

        Rect r = area;
        g.rect(r, rgba(22, 22, 32), kSubBg);
        const Rect tools = cut_left(r, 22);
        const Rect side = cut_right(r, std::min(200, std::max(160, area.w / 4)));
        const Rect top = cut_top(r, 16);
        draw_tools(g, tools);
        draw_topbar(h, top);
        keys(h);
        compose(h);
        draw_canvas(h, r);
        draw_side(h, side);
        g.pop_id();
    }

private:
    struct Comp {                               // one layer composited to an RGBA image
        TextureId tex = kNoTexture;
        std::vector<uint32_t> px;
        std::vector<uint16_t> drawn;            // the GIDs the image currently shows
        int w = 0, h = 0;
    };
    uint32_t id_ = 0;
    // tileset
    std::vector<uint32_t> ts_px;
    int ts_w = 0, ts_h = 0;
    bool ts_real = false;                       // a PNG (vs the procedural swatches)
    std::string ts_path;                        // resolved image path
    TextureId ts_tex_ = kNoTexture;
    int ts_version_ = 0;
    // composites
    std::vector<Comp> comps_;
    Comp col_;                                  // collision overlay of the gameplay layer
    std::vector<uint8_t> col_flags_seen_;
    int comp_ts_version_ = -1;
    bool comp_too_big_ = false;
    // view
    Canvas cv_;
    MTool tool_ = MTool::Brush;
    int layer_ = 0;
    std::vector<bool> hidden_;
    bool grid_ = true, collision_ = true, spawns_ = true, parallax_ = false, dim_ = false;
    int side_tab_ = 0;
    int pal_zoom_ = 2, pal_scroll_ = 0, pal_hscroll_ = 0;
    TmapDoc::Stamp stamp_;                      // what the brush paints (1x1 = a single GID)
    int hx_ = -1, hy_ = -1;
    // gestures
    bool stroking_ = false;
    int ax_ = 0, ay_ = 0, lx_ = 0, ly_ = 0;
    int sel_x0_ = 0, sel_y0_ = 0, sel_x1_ = -1, sel_y1_ = -1;
    int spawn_sel_ = -1;
    int spawn_dx_ = 0, spawn_dy_ = 0;
    bool spawn_moving_ = false;
    std::string spawn_type_ = "player";
    std::vector<std::string> types_;
    uint64_t types_tick_ = ~0ull;
    bool can_open_ = false;                   // the host opens other documents (not a solo shell)
    bool want_resize_ = false, want_create_ts_ = false, want_edit_tileset_ = false, want_edit_tile_ = false;
    int64_t ts_stamp_ = 0;
    uint16_t ctx_gid_ = 0;

    // Open the tileset in the pixel editor focused on GID `gid`'s tile.
    void edit_tile(Host& h, uint16_t gid) {
        if (!ts_real) { create_tileset_dialog(h); return; }
        if (h.access(ts_path) != Access::Write) { h.toast(base_name(ts_path) + " is outside the project (read-only)", Toast::Warn); return; }
        h.open_tile(ts_path, gid ? int(gid) - 1 : 0, doc.tile_w, doc.tile_h);
    }
    // palette pick drag
    int pal_x0_ = -1, pal_y0_ = -1;

    // ---- tileset ----
    int ts_cols() const { return std::max(1, ts_w / std::max(1, doc.tile_w)); }
    int ts_rows() const { return std::max(1, ts_h / std::max(1, doc.tile_h)); }
    int ts_count() const { return ts_cols() * ts_rows(); }

    void load_tileset(Host& h) {
        ts_real = false;
        ts_path.clear();
        const std::string dir = dir_name(path);
        std::vector<std::string> cands;
        if (!doc.tileset_image.empty()) cands.push_back(is_abs_path(doc.tileset_image) ? doc.tileset_image : join_path(dir, doc.tileset_image));
        cands.push_back(join_path(dir, doc.tileset + ".png"));
        for (const std::string& rel : h.files_with({ ".png" }))
            if (stem_of(rel) == doc.tileset) cands.push_back(join_path(h.root(), rel));
        for (const std::string& c : cands) {
            if (h.access(c) == Access::None) continue;       // never read outside the project
            PixelDoc img;
            if (PixelDoc::load_png(c, img, nullptr)) {
                ts_px = img.px; ts_w = img.w; ts_h = img.h; ts_real = true; ts_path = c;
                ts_stamp_ = file_stamp(c);
                doc.tileset_cols = ts_cols(); doc.tileset_count = ts_count();
                doc.tileset_img_w = ts_w; doc.tileset_img_h = ts_h;
                break;
            }
        }
        if (!ts_real) {
            // procedural swatches: 16 tiles, each a colour with a bevel so neighbours read apart
            const int tw = std::max(1, doc.tile_w), th = std::max(1, doc.tile_h);
            ts_w = tw * 16; ts_h = th;
            ts_px.assign(size_t(ts_w) * size_t(ts_h), 0);
            for (int t = 0; t < 16; ++t)
                for (int y = 0; y < th; ++y)
                    for (int x = 0; x < tw; ++x) {
                        Rgba c = kSwatch[t];
                        if (x == 0 || y == 0) c = scale_rgb(c, 5, 4);
                        if (x == tw - 1 || y == th - 1) c = scale_rgb(c, 3, 5);
                        ts_px[size_t(y) * size_t(ts_w) + size_t(t * tw + x)] = c;
                    }
        }
        phx::Renderer& r = h.renderer();
        r.unload_texture(ts_tex_);
        phx::TextureDesc d{};
        d.pixels = ts_px.data(); d.size = uint32_t(ts_px.size() * 4);
        d.width = uint16_t(ts_w); d.height = uint16_t(ts_h);
        ts_tex_ = r.load_texture(d);
        ++ts_version_;
    }
    // The tileset source rect of a GID (false for 0 / out of range).
    bool gid_src(uint16_t gid, int& sx, int& sy) const {
        if (gid == 0) return false;
        int i = int(gid) - 1;
        if (!ts_real) i %= 16;
        if (i >= ts_count()) return false;
        sx = (i % ts_cols()) * doc.tile_w; sy = (i / ts_cols()) * doc.tile_h;
        return true;
    }

    // ---- compositing ----
    void blit_cell(Comp& c, int cx, int cy, uint16_t gid) {
        const int tw = doc.tile_w, th = doc.tile_h;
        int sx = 0, sy = 0;
        const bool ok = gid_src(gid, sx, sy);
        for (int y = 0; y < th; ++y) {
            uint32_t* dst = &c.px[size_t(cy * th + y) * size_t(c.w) + size_t(cx * tw)];
            if (!gid) { std::fill(dst, dst + tw, 0u); continue; }
            if (!ok) {                                          // a GID past the tileset: magenta X
                for (int x = 0; x < tw; ++x) dst[x] = (x == y || x == tw - 1 - y) ? 0xFFFF00FFu : 0x80400040u | 0xFF000000u;
                continue;
            }
            std::copy(&ts_px[size_t(sy + y) * size_t(ts_w) + size_t(sx)], &ts_px[size_t(sy + y) * size_t(ts_w) + size_t(sx)] + tw, dst);
        }
    }
    void flag_cell(Comp& c, int cx, int cy, uint8_t f) {
        const int tw = doc.tile_w, th = doc.tile_h;
        for (int y = 0; y < th; ++y)
            for (int x = 0; x < tw; ++x) {
                uint32_t p = 0;
                const bool edge = x == 0 || y == 0 || x == tw - 1 || y == th - 1;
                if (f & phx::kTileFlagHazard) p = (((x + y) & 3) == 0 || edge) ? 0xFF3C3CF0u : 0;  // red hatch
                else if (f & phx::kTileFlagOneWay) p = y < 2 ? 0xFF40D8F0u : 0;                    // yellow lid
                else if (f & phx::kTileFlagSolid) p = edge ? 0xFFF0F0F0u : 0;                      // white outline
                c.px[size_t(cy * th + y) * size_t(c.w) + size_t(cx * tw + x)] = p;
            }
    }
    void ensure_comp(Host& h, Comp& c) {
        const int w = doc.width * doc.tile_w, hh = doc.height * doc.tile_h;
        if (c.w == w && c.h == hh && c.tex != kNoTexture) return;
        h.renderer().unload_texture(c.tex);
        c.w = w; c.h = hh;
        c.px.assign(size_t(w) * size_t(hh), 0);
        c.drawn.assign(size_t(doc.width) * size_t(doc.height), 0xFFFF);
        phx::TextureDesc d{};
        d.pixels = c.px.data(); d.size = uint32_t(c.px.size() * 4);
        d.width = uint16_t(w); d.height = uint16_t(hh);
        c.tex = h.renderer().load_texture(d);
    }
    void compose(Host& h) {
        const int64_t bytes = int64_t(doc.width) * doc.tile_w * doc.height * doc.tile_h * 4;
        comp_too_big_ = bytes > (int64_t(64) << 20) || doc.width * doc.tile_w > 16384 || doc.height * doc.tile_h > 16384;
        if (comp_too_big_) return;
        const bool full = comp_ts_version_ != ts_version_;
        comp_ts_version_ = ts_version_;
        while (comps_.size() > doc.layers.size()) { h.renderer().unload_texture(comps_.back().tex); comps_.pop_back(); }
        comps_.resize(doc.layers.size());
        for (size_t l = 0; l < doc.layers.size(); ++l) {
            Comp& c = comps_[l];
            ensure_comp(h, c);
            if (full) std::fill(c.drawn.begin(), c.drawn.end(), 0xFFFF);
            const std::vector<uint16_t>& L = doc.layers[l];
            for (int y = 0; y < doc.height; ++y)
                for (int x = 0; x < doc.width; ++x) {
                    const size_t i = size_t(y) * size_t(doc.width) + size_t(x);
                    if (c.drawn[i] != L[i]) { blit_cell(c, x, y, L[i]); c.drawn[i] = L[i]; }
                }
        }
        // collision overlay of the gameplay (last) layer
        ensure_comp(h, col_);
        const bool flags_changed = col_flags_seen_ != doc.tile_flags;
        if (flags_changed || full) { std::fill(col_.drawn.begin(), col_.drawn.end(), 0xFFFF); col_flags_seen_ = doc.tile_flags; }
        const std::vector<uint16_t>& G = doc.layers.back();
        const bool any = doc.has_tile_flags();
        for (int y = 0; y < doc.height; ++y)
            for (int x = 0; x < doc.width; ++x) {
                const size_t i = size_t(y) * size_t(doc.width) + size_t(x);
                if (col_.drawn[i] == G[i]) continue;
                uint8_t f = doc.tile_flag(G[i]);
                if (!any && G[i]) f = phx::kTileFlagSolid;          // the bake's solid_from fallback
                flag_cell(col_, x, y, f);
                col_.drawn[i] = G[i];
            }
    }
    void after_structure_change() {
        if (hidden_.size() != doc.layers.size()) hidden_.resize(doc.layers.size(), false);
        layer_ = std::max(0, std::min(layer_, int(doc.layers.size()) - 1));
        spawn_sel_ = std::min(spawn_sel_, int(doc.spawns.size()) - 1);
    }

    // ---- spawn types ----
    const std::vector<std::string>& types(Host& h) {
        if (types_tick_ != h.ticks() / 120) {
            types_tick_ = h.ticks() / 120;
            std::vector<std::string> t = h.prefab_types();
            for (const char* d : { "player", "coin", "enemy", "spike" }) t.push_back(d);
            for (const std::string& s : doc.spawn_types()) t.push_back(s);
            types_.clear();
            std::set<std::string> seen;
            for (const std::string& s : t) if (!s.empty() && seen.insert(s).second) types_.push_back(s);
        }
        return types_;
    }

    // ---- UI ----
    void draw_tools(Gui& g, const Rect& r) {
        g.rect(r, g.th.panel, kSubFill);
        int y = r.y + 3;
        for (const MToolInfo& t : kMTools) {
            if (g.icon_button(Rect{ r.x + 3, y, 16, 15 }, t.icon, t.help, tool_ == t.t)) tool_ = t.t;
            y += 17;
        }
        y += 6;
        // the brush tile
        const Rect sw{ r.x + 3, y, 16, 16 };
        checkerboard(g, sw, 4);
        int sx, sy;
        if (stamp_.w == 1 && stamp_.h == 1 && gid_src(stamp_.gids[0], sx, sy)) g.image(sw, ts_tex_, sx, sy, doc.tile_w, doc.tile_h);
        else if (stamp_.w * stamp_.h > 1) g.text(sw.x + 1, sw.y + 4, fmt("%dx%d", stamp_.w, stamp_.h).substr(0, 3), g.th.text);
        g.frame_rect(sw, g.th.accent, kSubOver);
        g.tip(sw, stamp_.w * stamp_.h > 1 ? fmt("Stamp %dx%d tiles", stamp_.w, stamp_.h) : fmt("Brush tile GID %u", unsigned(stamp_.gids[0])));
    }

    void draw_topbar(Host& h, const Rect& r) {
        Gui& g = h.gui();
        g.rect(r, g.th.panel, kSubFill);
        Gui::Row row(Rect{ r.x + 4, r.y + 2, r.w - 8, 12 }, 3);
        // layer selector
        std::vector<std::string> names;
        for (size_t l = 0; l < doc.layers.size(); ++l)
            names.push_back(doc.layer_names[l] + (l + 1 == doc.layers.size() ? "  (gameplay)" : ""));
        g.dropdown(g.id("layer-dd"), row.take(std::min(140, std::max(70, r.w / 3))), names, layer_, "The layer you paint on (Tab / Shift+Tab). The last layer collides.");
        auto toggle = [&](int icon, bool& v, const char* help) {
            if (g.icon_button(row.take(15), icon, help, v)) v = !v;
        };
        toggle(kIconGrid, grid_, "Tile grid");
        toggle(kIconWall, collision_, "Collision overlay on the gameplay layer: white = solid, yellow = one-way, red = hazard");
        toggle(kIconFlag, spawns_, "Show entity spawns");
        toggle(kIconEyeOff, dim_, "Dim every layer except the one you paint");
        toggle(kIconLayers, parallax_, "Parallax preview: each layer scrolls by its factor as you pan (painting is off while on)");
        if (g.icon_button(row.take_right(14), kIconZoomIn, "Zoom in (Ctrl+wheel)")) cv_.zoom_at(r.x + r.w / 2, r.y + 100, Canvas::step_zoom(cv_.zoom, 1));
        const std::string z = fmt("%dx", cv_.zoom);
        g.text(row.take_right(Gui::text_w(z)).x, r.y + 4, z, g.th.dim);
        if (g.icon_button(row.take_right(14), kIconZoomOut, "Zoom out")) cv_.zoom_at(r.x + r.w / 2, r.y + 100, Canvas::step_zoom(cv_.zoom, -1));
        if (g.button(row.take_right(24), "fit", Btn{ false, true, false, 0, "Fit the whole map (F)" })) cv_.fitted = false;
    }

    void keys(Host& h) {
        Gui& g = h.gui();
        if (g.text_focus() || h.modal_open()) return;
        for (const MToolInfo& t : kMTools) if (g.hotkey(t.key)) tool_ = t.t;
        if (g.hotkey(PHX_KEY_TAB)) layer_ = (layer_ + 1) % int(doc.layers.size());
        if (g.hotkey(PHX_KEY_TAB, kShift)) layer_ = (layer_ + int(doc.layers.size()) - 1) % int(doc.layers.size());
        if (g.hotkey('f')) cv_.fitted = false;
        if (g.hotkey('v') && stamp_.w * stamp_.h == 1 && stamp_.gids[0]) { doc.push_undo(); doc.cycle_tile_flag(stamp_.gids[0]); }
        if (g.key('c', kCtrl) && sel_x1_ >= 0) {
            stamp_ = doc.copy_stamp(layer_, sel_x0_, sel_y0_, sel_x1_, sel_y1_);
            h.toast(fmt("stamp %dx%d copied - the brush paints it", stamp_.w, stamp_.h));
            tool_ = MTool::Brush;
        }
        if (g.key('v', kCtrl)) tool_ = MTool::Brush;
        if (g.hotkey(PHX_KEY_DELETE) || g.hotkey(PHX_KEY_BACKSPACE)) {
            if (tool_ == MTool::Spawn && spawn_sel_ >= 0) { doc.push_undo(); doc.remove_spawn(spawn_sel_); spawn_sel_ = -1; }
            else if (sel_x1_ >= 0) { doc.push_undo(); if (!doc.fill_rect(layer_, sel_x0_, sel_y0_, sel_x1_, sel_y1_, 0)) doc.drop_undo(); }
        }
        if (g.hotkey(PHX_KEY_ESCAPE)) { sel_x1_ = -1; spawn_sel_ = -1; }
    }

    // Layer l's parallax offset in content pixels (preview).
    int par_off(int l, bool y) const {
        if (!parallax_ || l >= int(doc.layer_parallax.size())) return 0;
        const double f = y ? doc.layer_parallax[size_t(l)].second : doc.layer_parallax[size_t(l)].first;
        const int view = y ? -(cv_.oy - view_y0_) / std::max(1, cv_.zoom) : -(cv_.ox - view_x0_) / std::max(1, cv_.zoom);
        return int(view * (1.0 - f));
    }
    int view_x0_ = 0, view_y0_ = 0;

    void draw_canvas(Host& h, const Rect& area) {
        Gui& g = h.gui();
        const int mw = doc.width * doc.tile_w, mh = doc.height * doc.tile_h;
        if (!cv_.fitted) {
            cv_.fit(area, mw, mh, 8);
            if (cv_.zoom < 2 && mw * 2 <= area.w * 3) cv_.zoom = std::max(1, cv_.zoom);
            view_x0_ = cv_.ox; view_y0_ = cv_.oy;
        }
        g.push_clip(area);
        cv_.navigate(g, area, g.id("pan"), tool_ == MTool::Hand);
        const Rect map_r{ cv_.ox, cv_.oy, mw * cv_.zoom, mh * cv_.zoom };
        g.rect(intersect(map_r, area), rgba(34, 36, 50), kSubFill);
        if (comp_too_big_) {
            g.text(area.x + 10, area.y + 10, "map too large to preview as layer images (> 64 MB); editing still works", g.th.warn);
        } else {
            for (size_t l = 0; l < doc.layers.size() && l < size_t(kSubImageSpan - 2); ++l) {
                if (hidden_[l]) continue;
                const Rgba tint = dim_ && int(l) != layer_ ? rgba(110, 110, 130) : rgba(255, 255, 255);
                const Rect lr{ map_r.x + par_off(int(l), false) * cv_.zoom, map_r.y + par_off(int(l), true) * cv_.zoom, map_r.w, map_r.h };
                g.image(lr, comps_[l].tex, 0, 0, mw, mh, tint, uint8_t(kSubImage + l));
            }
            if (collision_ && !parallax_) g.image(map_r, col_.tex, 0, 0, mw, mh, rgba(255, 255, 255), uint8_t(kSubImage + kSubImageSpan - 2));
        }
        // grid
        if (grid_ && doc.tile_w * cv_.zoom >= 6) {
            const Rect vis = intersect(map_r, area);
            const Rgba gc = rgba(58, 60, 80);
            const int tw = doc.tile_w * cv_.zoom, th = doc.tile_h * cv_.zoom;
            for (int x = (vis.x - map_r.x) / tw; x <= (vis.right() - map_r.x) / tw; ++x) g.vline(map_r.x + x * tw, vis.y, vis.h, gc, uint8_t(kSubImage + kSubImageSpan - 1));
            for (int y = (vis.y - map_r.y) / th; y <= (vis.bottom() - map_r.y) / th; ++y) g.hline(vis.x, map_r.y + y * th, vis.w, gc, uint8_t(kSubImage + kSubImageSpan - 1));
        }
        g.frame_rect(Rect{ map_r.x - 1, map_r.y - 1, map_r.w + 2, map_r.h + 2 }, g.th.line, kSubOver);

        // selection marquee
        if (sel_x1_ >= 0) {
            const Rect sr{ map_r.x + sel_x0_ * doc.tile_w * cv_.zoom, map_r.y + sel_y0_ * doc.tile_h * cv_.zoom,
                           (sel_x1_ - sel_x0_ + 1) * doc.tile_w * cv_.zoom + 1, (sel_y1_ - sel_y0_ + 1) * doc.tile_h * cv_.zoom + 1 };
            g.dashed(sr, g.th.text, rgba(20, 20, 30), int(g.frame / 6), kSubTop);
        }
        // spawns
        if (spawns_) draw_spawns(g, map_r);

        // hover cell + brush preview
        hx_ = hy_ = -1;
        if (g.hover(area)) {
            const int px = cv_.to_cx(g.mx()), py = cv_.to_cy(g.my());
            const int cx = Canvas::floordiv(px, doc.tile_w), cy = Canvas::floordiv(py, doc.tile_h);
            if (doc.in_bounds(cx, cy)) {
                hx_ = cx; hy_ = cy;
                const uint16_t gid = doc.tile(layer_, cx, cy);
                const uint8_t f = doc.tile_flag(gid);
                g.hint = fmt("cell (%d,%d)  px (%d,%d)  GID %u%s", cx, cy, px, py, unsigned(gid),
                             f & phx::kTileFlagSolid ? "  solid" : f & phx::kTileFlagOneWay ? "  one-way" : f & phx::kTileFlagHazard ? "  hazard" : "");
            }
            if (tool_ == MTool::Brush || tool_ == MTool::Eraser) {
                const int bw = tool_ == MTool::Brush ? stamp_.w : 1, bh = tool_ == MTool::Brush ? stamp_.h : 1;
                // ghost of the stamp under the pointer
                if (tool_ == MTool::Brush)
                    for (int j = 0; j < stamp_.h; ++j)
                        for (int i = 0; i < stamp_.w; ++i) {
                            int sx, sy;
                            if (!gid_src(stamp_.gids[size_t(j) * size_t(stamp_.w) + size_t(i)], sx, sy)) continue;
                            g.image(Rect{ map_r.x + (cx + i) * doc.tile_w * cv_.zoom, map_r.y + (cy + j) * doc.tile_h * cv_.zoom,
                                          doc.tile_w * cv_.zoom, doc.tile_h * cv_.zoom }, ts_tex_, sx, sy, doc.tile_w, doc.tile_h,
                                    rgba(200, 200, 255), kSubOver);
                        }
                g.frame_rect(Rect{ map_r.x + cx * doc.tile_w * cv_.zoom, map_r.y + cy * doc.tile_h * cv_.zoom,
                                   bw * doc.tile_w * cv_.zoom, bh * doc.tile_h * cv_.zoom }, tool_ == MTool::Eraser ? g.th.bad : g.th.accent, kSubTop);
            } else if (tool_ != MTool::Hand && tool_ != MTool::Spawn) {
                g.frame_rect(Rect{ map_r.x + cx * doc.tile_w * cv_.zoom, map_r.y + cy * doc.tile_h * cv_.zoom,
                                   doc.tile_w * cv_.zoom, doc.tile_h * cv_.zoom }, g.th.text, kSubTop);
            }
            if (tool_ != MTool::Hand) g.cursor = PHX_CURSOR_CROSSHAIR;
        }
        g.pop_clip();
        if (tool_ != MTool::Hand) use_tool(h, area, map_r);
    }

    void draw_spawns(Gui& g, const Rect& map_r) {
        for (size_t i = 0; i < doc.spawns.size(); ++i) {
            const TiledSpawn& s = doc.spawns[i];
            const int w = (s.w > 0 ? s.w : doc.tile_w) * cv_.zoom, hh = (s.h > 0 ? s.h : doc.tile_h) * cv_.zoom;
            const Rect sr{ map_r.x + s.x * cv_.zoom, map_r.y + s.y * cv_.zoom, std::max(3, w), std::max(3, hh) };
            const bool sel = int(i) == spawn_sel_;
            const Rgba c = type_colour(s.type);
            g.stipple(sr, c, kSubOver);
            g.frame_rect(sr, sel ? g.th.text : c, kSubText);
            if (sel) g.frame_rect(Rect{ sr.x - 1, sr.y - 1, sr.w + 2, sr.h + 2 }, g.th.accent, kSubTop);
            if (cv_.zoom >= 2 || sel) {
                const std::string label = s.type.empty() ? s.name : s.type;
                const int tw = Gui::text_w(label) + 4;
                g.rect(Rect{ sr.x, sr.y - 10, tw, 10 }, rgba(20, 20, 30), kSubText);
                g.text(sr.x + 2, sr.y - 9, label, c, kSubTop);
            }
        }
    }
    static Rgba type_colour(const std::string& t) {
        uint32_t hsh = 2166136261u;
        for (char c : t) { hsh ^= uint8_t(c); hsh *= 16777619u; }
        return hsv_to_rgb(int(hsh % 360), 150, 250);
    }

    void use_tool(Host& h, const Rect& area, const Rect& map_r) {
        Gui& g = h.gui();
        const uint32_t cid = g.id("canvas");
        const bool right = (g.in->pressed & kMouseR) || (stroking_ && (g.in->held & kMouseR));
        const uint32_t btn = right ? kMouseR : kMouseL;
        if (!g.drag(cid, area, btn)) {
            if (stroking_) {
                stroking_ = false;
                if (tool_ == MTool::Rect && !right) {
                    const uint16_t gid = stamp_.gids[0];
                    if (!doc.fill_rect(layer_, ax_, ay_, lx_, ly_, gid)) doc.drop_undo();
                } else if (tool_ == MTool::Picker && (ax_ != lx_ || ay_ != ly_)) {
                    stamp_ = doc.copy_stamp(layer_, ax_, ay_, lx_, ly_);
                    tool_ = MTool::Brush;
                } else if (!gesture_changed_) doc.drop_undo();
                spawn_moving_ = false;
            }
            return;
        }
        const int px = cv_.to_cx(g.mx()), py = cv_.to_cy(g.my());
        const int cx = Canvas::floordiv(px, doc.tile_w), cy = Canvas::floordiv(py, doc.tile_h);
        const bool first = g.just_pressed(cid);
        if (parallax_ && tool_ != MTool::Spawn) {
            if (first) h.toast("turn the parallax preview off to paint", Toast::Warn);
            return;
        }
        if (first) {
            stroking_ = true;
            gesture_changed_ = false;
            ax_ = lx_ = cx; ay_ = ly_ = cy;
            if (tool_ != MTool::Picker && tool_ != MTool::Select) doc.push_undo();
        }
        const MTool t = right && tool_ != MTool::Spawn ? MTool::Eraser : tool_;
        switch (t) {
        case MTool::Brush: case MTool::Eraser: {
            auto paint = [&](int x, int y) {
                if (t == MTool::Eraser) { if (doc.tile(layer_, x, y)) { doc.set_tile(layer_, x, y, 0); gesture_changed_ = true; } }
                else if (doc.paint_stamp(layer_, x, y, stamp_, stamp_.w * stamp_.h > 1)) gesture_changed_ = true;
            };
            if (first) paint(cx, cy);
            else {
                // step cell by cell between samples (fast drags don't leave gaps)
                int x0 = lx_, y0 = ly_;
                const int dx = std::abs(cx - x0), dy = -std::abs(cy - y0), sx = x0 < cx ? 1 : -1, sy = y0 < cy ? 1 : -1;
                int err = dx + dy;
                while (!(x0 == cx && y0 == cy)) {
                    const int e2 = 2 * err;
                    if (e2 >= dy) { err += dy; x0 += sx; }
                    if (e2 <= dx) { err += dx; y0 += sy; }
                    if (stamp_.w * stamp_.h == 1 || t == MTool::Eraser || ((x0 - ax_) % stamp_.w == 0 && (y0 - ay_) % stamp_.h == 0)) paint(x0, y0);
                }
            }
            lx_ = cx; ly_ = cy;
            break;
        }
        case MTool::Fill:
            if (first && doc.flood_fill(layer_, cx, cy, stamp_.gids[0])) gesture_changed_ = true;
            break;
        case MTool::Rect: case MTool::Picker: case MTool::Select: {
            lx_ = std::max(0, std::min(doc.width - 1, cx)); ly_ = std::max(0, std::min(doc.height - 1, cy));
            ax_ = std::max(0, std::min(doc.width - 1, ax_)); ay_ = std::max(0, std::min(doc.height - 1, ay_));
            const Rect sr{ map_r.x + std::min(ax_, lx_) * doc.tile_w * cv_.zoom, map_r.y + std::min(ay_, ly_) * doc.tile_h * cv_.zoom,
                           (std::abs(lx_ - ax_) + 1) * doc.tile_w * cv_.zoom, (std::abs(ly_ - ay_) + 1) * doc.tile_h * cv_.zoom };
            g.push_clip(area);
            g.frame_rect(sr, t == MTool::Rect ? g.th.accent : g.th.info, kSubTop);
            g.pop_clip();
            g.hint = fmt("%d x %d tiles", std::abs(lx_ - ax_) + 1, std::abs(ly_ - ay_) + 1);
            if (t == MTool::Picker && first && doc.in_bounds(cx, cy)) {
                stamp_ = TmapDoc::Stamp{};
                stamp_.gids[0] = doc.tile(layer_, cx, cy);
            }
            if (t == MTool::Select) { sel_x0_ = std::min(ax_, lx_); sel_y0_ = std::min(ay_, ly_); sel_x1_ = std::max(ax_, lx_); sel_y1_ = std::max(ay_, ly_); }
            break;
        }
        case MTool::Spawn: {
            const bool snap = !(g.in->mods & kShift);
            if (first) {
                const int hit = doc.spawn_at(px, py);
                if (hit >= 0) {
                    spawn_sel_ = hit;
                    spawn_dx_ = px - doc.spawns[size_t(hit)].x; spawn_dy_ = py - doc.spawns[size_t(hit)].y;
                    spawn_moving_ = true;
                } else if (px >= 0 && py >= 0 && px < doc.width * doc.tile_w && py < doc.height * doc.tile_h) {
                    const int x = snap ? cx * doc.tile_w : px, y = snap ? cy * doc.tile_h : py;
                    doc.add_spawn(spawn_type_, x, y);
                    doc.spawns.back().name = doc.fresh_spawn_name(spawn_type_);
                    spawn_sel_ = int(doc.spawns.size()) - 1;
                    spawn_moving_ = false;
                    gesture_changed_ = true;
                    side_tab_ = 2;
                }
            } else if (spawn_moving_ && spawn_sel_ >= 0) {
                int x = px - spawn_dx_, y = py - spawn_dy_;
                if (snap) { x = Canvas::floordiv(x + doc.tile_w / 2, doc.tile_w) * doc.tile_w; y = Canvas::floordiv(y + doc.tile_h / 2, doc.tile_h) * doc.tile_h; }
                x = std::max(0, std::min(doc.width * doc.tile_w - 1, x));
                y = std::max(0, std::min(doc.height * doc.tile_h - 1, y));
                const TiledSpawn& s = doc.spawns[size_t(spawn_sel_)];
                if (s.x != x || s.y != y) { doc.move_spawn(spawn_sel_, x, y); gesture_changed_ = true; }
            }
            break;
        }
        default: break;
        }
    }
    bool gesture_changed_ = false;
    int spawn_scroll_ = 0;

    // ---- side panel ----
    void draw_side(Host& h, const Rect& r0) {
        Gui& g = h.gui();
        Rect col = r0;
        g.rect(col, g.th.panel, kSubFill);
        g.vline(col.x, col.y, col.h, g.th.line);
        cut_left(col, 1);
        const int t = g.tabs(cut_top(col, 14), { "Tiles", "Layers", "Spawns", "Map" }, side_tab_);
        if (t >= 0) side_tab_ = t;
        const Rect body = col.inset(4, 3);
        switch (side_tab_) {
        case 0: side_tiles(h, body); break;
        case 1: side_layers(h, body); break;
        case 2: side_spawns(h, body); break;
        default: side_map(h, body); break;
        }
    }

    void side_tiles(Host& h, const Rect& b) {
        Gui& g = h.gui();
        int y = b.y;
        g.text(b.x, y + 2, ts_real ? base_name(ts_path) : "no tileset image (swatches)", ts_real ? g.th.text : g.th.warn, kSubText, b.w - 40);
        if (g.button(Rect{ b.right() - 34, y, 16, 12 }, "1x", Btn{ pal_zoom_ == 1 })) pal_zoom_ = 1;
        if (g.button(Rect{ b.right() - 16, y, 16, 12 }, "2x", Btn{ pal_zoom_ == 2 })) pal_zoom_ = 2;
        g.tip(Rect{ b.right() - 34, y, 34, 12 }, "Palette zoom (falls back to 1x when the tileset is too wide)");
        y += 15;
        // palette grid (the tileset image, clickable per tile; drag selects a stamp)
        const int cols = ts_real ? ts_cols() : 16, rows = ts_real ? ts_rows() : 1;
        int pz = pal_zoom_;                                     // fall back to 1x when 2x won't fit
        while (pz > 1 && cols * doc.tile_w * pz > b.w - 6) --pz;
        const int tw = doc.tile_w * pz, th = doc.tile_h * pz;
        const int shown_cols = std::max(1, std::min(cols, (b.w - 6) / std::max(1, tw)));
        pal_hscroll_ = std::max(0, std::min(pal_hscroll_, cols - shown_cols));
        const Rect pal{ b.x, y, shown_cols * tw, std::min(b.bottom() - y - 60, rows * th) };
        const int vis_rows = std::max(1, pal.h / std::max(1, th));
        g.rect(Rect{ pal.x - 1, pal.y - 1, pal.w + 2, pal.h + 2 }, g.th.line, kSubWidget);
        checkerboard(g, pal, 4);
        g.wheel_scroll(pal, pal_scroll_, rows, vis_rows, 1);
        g.push_clip(pal);
        // draw the visible rows of the tileset in one image per row band (columns may be clipped)
        if (const int w = g.wheel_x(pal)) pal_hscroll_ = std::max(0, std::min(cols - shown_cols, pal_hscroll_ - w));
        g.image(Rect{ pal.x - pal_hscroll_ * tw, pal.y - pal_scroll_ * th, cols * tw, rows * th }, ts_tex_, 0, 0, cols * doc.tile_w, rows * doc.tile_h,
                rgba(255, 255, 255), kSubImage);
        // collision badges + selection
        for (int ry = 0; ry < vis_rows + 1; ++ry)
            for (int cx = 0; cx < shown_cols; ++cx) {
                const int row = ry + pal_scroll_;
                if (row >= rows) break;
                const uint16_t gid = uint16_t(row * cols + cx + pal_hscroll_ + 1);
                const Rect tr{ pal.x + cx * tw, pal.y + ry * th, tw, th };
                const uint8_t f = doc.tile_flag(gid);
                if (f) g.rect(Rect{ tr.x, tr.bottom() - 2, tr.w, 2 }, f & phx::kTileFlagSolid ? rgba(240, 240, 240) : f & phx::kTileFlagOneWay ? g.th.warn : g.th.bad, kSubOver);
                bool in_stamp = false;
                if (stamp_.w * stamp_.h == 1) in_stamp = stamp_.gids[0] == gid;
                if (in_stamp) g.frame_rect(tr, g.th.accent, kSubText);
            }
        const uint32_t pid = g.id("pal");
        if (g.drag(pid, pal)) {
            const int cx = std::max(0, std::min(shown_cols - 1, (g.mx() - pal.x) / std::max(1, tw))) + pal_hscroll_;
            const int cy = std::max(0, std::min(rows - 1, (g.my() - pal.y) / std::max(1, th) + pal_scroll_));
            if (g.just_pressed(pid)) { pal_x0_ = cx; pal_y0_ = cy; }
            const int x0 = std::min(pal_x0_, cx), x1 = std::max(pal_x0_, cx), y0 = std::min(pal_y0_, cy), y1 = std::max(pal_y0_, cy);
            stamp_.w = x1 - x0 + 1; stamp_.h = y1 - y0 + 1;
            stamp_.gids.assign(size_t(stamp_.w) * size_t(stamp_.h), 0);
            for (int j = 0; j < stamp_.h; ++j) for (int i = 0; i < stamp_.w; ++i) stamp_.gids[size_t(j) * size_t(stamp_.w) + size_t(i)] = uint16_t((y0 + j) * cols + x0 + i + 1);
            if (tool_ != MTool::Fill && tool_ != MTool::Rect) tool_ = MTool::Brush;
            if (stamp_.w * stamp_.h > 1) g.frame_rect(Rect{ pal.x + (x0 - pal_hscroll_) * tw, pal.y + (y0 - pal_scroll_) * th, stamp_.w * tw, stamp_.h * th }, g.th.accent, kSubText);
        }
        if (g.hover(pal)) {
            const int cx = (g.mx() - pal.x) / std::max(1, tw) + pal_hscroll_, cy = (g.my() - pal.y) / std::max(1, th) + pal_scroll_;
            const uint16_t gid = uint16_t(cy * cols + cx + 1);
            g.hint = fmt("GID %u  - click: brush, drag: stamp, %sright-click: more", unsigned(gid),
                         can_open_ ? "double-click: edit the tile, " : "");
            if (g.in->clicks >= 2 && can_open_) edit_tile(h, gid);         // the press of a double-click
            if (g.right_clicked(pal)) { ctx_gid_ = gid; g.open_context(g.id("tile-ctx")); }
        }
        g.pop_clip();
        g.scrollbar_v(g.id("pal-sb"), Rect{ pal.right() + 2, pal.y, 4, pal.h }, rows, vis_rows, pal_scroll_);
        if (cols > shown_cols) { g.scrollbar_h(g.id("pal-hs"), Rect{ pal.x, pal.bottom() + 2, pal.w, 4 }, cols, shown_cols, pal_hscroll_); y = pal.bottom() + 12; }
        else y = pal.bottom() + 6;
        // collision flag of the brush tile
        {
            const uint8_t cf = doc.tile_flag(ctx_gid_);
            const int hit = g.menu(g.id("tile-ctx"), {
                MenuItem{ ts_real ? fmt("Edit tile %u...", unsigned(ctx_gid_)) : std::string("Create a tileset to paint..."), "dbl-click", can_open_ || !ts_real, false, false, kIconPencil },
                MenuItem::sep(),
                MenuItem{ "Collision: none", "", ctx_gid_ != 0, cf == 0 },
                MenuItem{ "Collision: solid", "", ctx_gid_ != 0, (cf & phx::kTileFlagSolid) != 0 },
                MenuItem{ "Collision: one-way", "", ctx_gid_ != 0, (cf & phx::kTileFlagOneWay) != 0 },
                MenuItem{ "Collision: hazard", "", ctx_gid_ != 0, (cf & phx::kTileFlagHazard) != 0 } });
            const uint8_t flags[4] = { 0, phx::kTileFlagSolid, phx::kTileFlagOneWay, phx::kTileFlagHazard };
            if (hit == 0) edit_tile(h, ctx_gid_);
            if (hit >= 2 && hit <= 5) { doc.push_undo(); doc.set_tile_flag(ctx_gid_, flags[hit - 2]); }
        }
        // edit the art itself
        {
            Gui::Row er(Rect{ b.x, y, b.w, 13 }, 3);
            if (ts_real && can_open_) {
                Btn eb; eb.icon = kIconPencil; eb.help = "Paint the brush tile in the pixel editor (the map reloads the art when you save)";
                if (g.button(er.take((b.w - 3) / 2), "this tile", eb)) edit_tile(h, stamp_.gids.empty() ? 1 : stamp_.gids[0]);
                Btn tb; tb.icon = kIconFileImage; tb.help = "Open the whole tileset image in the pixel editor";
                if (g.button(er.rest(), "tileset", tb)) h.open_tile(ts_path, 0, doc.tile_w, doc.tile_h);
            } else if (!ts_real) {
                Btn cb; cb.icon = kIconPlus; cb.on = true; cb.help = "Make a real tileset PNG (starting from these swatches) you can paint";
                if (g.button(er.rest(), "create tileset...", cb)) create_tileset_dialog(h);
            }
            if (can_open_ || !ts_real) y += 18;
        }
        const uint16_t gid = stamp_.gids.empty() ? 0 : stamp_.gids[0];
        g.text(b.x, y, fmt("collision of GID %u", unsigned(gid)), g.th.dim);
        y += 11;
        const uint8_t f = doc.tile_flag(gid);
        struct Opt { const char* l; uint8_t v; Rgba c; const char* help; };
        const Opt opts[] = { { "none", 0, g.th.dim, "Decoration: never collides" },
                             { "solid", phx::kTileFlagSolid, rgba(240, 240, 240), "Blocks from every side" },
                             { "1-way", phx::kTileFlagOneWay, g.th.warn, "A platform you can jump up through" },
                             { "hazard", phx::kTileFlagHazard, g.th.bad, "Hurts on touch (spikes, lava)" } };
        const int bw = (b.w - 9) / 4;
        for (int i = 0; i < 4; ++i) {
            Btn bb; bb.on = f == opts[i].v; bb.tint = opts[i].c; bb.help = opts[i].help; bb.enabled = gid != 0 && stamp_.w * stamp_.h == 1;
            if (g.button(Rect{ b.x + i * (bw + 3), y, bw, 13 }, opts[i].l, bb)) { doc.push_undo(); doc.set_tile_flag(gid, opts[i].v); }
        }
        y += 17;
        if (!doc.has_tile_flags()) g.text(b.x, y, "no flags: every tile on the last layer is solid", g.th.faint, kSubText, b.w);
        else {
            const std::vector<int> use = doc.gid_usage(int(doc.layers.size()) - 1);
            g.text(b.x, y, fmt("gameplay layer uses %d distinct tiles", int(std::count_if(use.begin() + (use.empty() ? 0 : 1), use.end(), [](int v) { return v > 0; }))), g.th.faint, kSubText, b.w);
        }
    }

    void side_layers(Host& h, const Rect& b) {
        Gui& g = h.gui();
        int y = b.y;
        const int n = int(doc.layers.size());
        for (int i = n - 1; i >= 0; --i) {                          // front-most first, like Tiled
            const Rect rr{ b.x, y, b.w, 13 };
            const bool sel = i == layer_;
            if (sel) g.rect(rr, g.th.sel, kSubWidget);
            else if (g.hover(rr)) g.rect(rr, g.th.hover, kSubWidget);
            if (g.icon_button(Rect{ rr.x + 1, rr.y + 1, 12, 11 }, hidden_[size_t(i)] ? kIconEyeOff : kIconEye, "Show / hide (editor only)")) hidden_[size_t(i)] = !hidden_[size_t(i)];
            g.text(rr.x + 16, rr.y + 3, doc.layer_names[size_t(i)], hidden_[size_t(i)] ? g.th.faint : g.th.text, kSubText, rr.w - 60);
            const auto par = i < int(doc.layer_parallax.size()) ? doc.layer_parallax[size_t(i)] : std::make_pair(1.0, 1.0);
            const std::string tag = i == n - 1 ? "solid" : fmt("%.2gx", par.first);
            g.text(rr.right() - Gui::text_w(tag) - 3, rr.y + 3, tag, i == n - 1 ? g.th.good : g.th.faint);
            if (g.clicked(rr)) layer_ = i;
            y += 14;
        }
        y += 4;
        Gui::Row row(Rect{ b.x, y, b.w, 12 }, 3);
        if (g.button(row.take(34), "+ add", Btn{ false, true, false, 0, "Add a layer on top (becomes the new gameplay layer!)" })) {
            doc.push_undo(); doc.add_layer(""); hidden_.push_back(false); layer_ = int(doc.layers.size()) - 1;
        }
        if (g.button(row.take(34), "delete", Btn{ false, n > 1, false, 0, "Delete the selected layer" }) && n > 1) {
            doc.push_undo(); doc.remove_layer(layer_); after_structure_change();
        }
        if (g.icon_button(row.take(12), kIconUp, "Move toward the front", false, layer_ + 1 < n) && layer_ + 1 < n) { doc.push_undo(); doc.move_layer(layer_, 1); ++layer_; }
        if (g.icon_button(row.take(12), kIconDown, "Move toward the back", false, layer_ > 0) && layer_ > 0) { doc.push_undo(); doc.move_layer(layer_, -1); --layer_; }
        y += 18;
        // selected layer properties
        g.section(b.x, y, b.w, "SELECTED LAYER", g.th.faint);
        y += 12;
        std::string nm = doc.layer_names[size_t(layer_)];
        g.text(b.x, y + 3, "name", g.th.dim);
        if (g.text_field(g.id("lname"), Rect{ b.x + 44, y, b.w - 44, 13 }, nm, "layer name")) { doc.push_undo(); doc.rename_layer(layer_, nm); }
        y += 17;
        auto par = layer_ < int(doc.layer_parallax.size()) ? doc.layer_parallax[size_t(layer_)] : std::make_pair(1.0, 1.0);
        g.text(b.x, y + 3, "parallax", g.th.dim);
        double px = par.first, py = par.second;
        const bool c1 = g.float_field(g.id("parx"), Rect{ b.x + 52, y, 40, 13 }, px, -4.0, 4.0, 0.05, "Horizontal factor: 1 = moves with the world, 0 = fixed to the screen, 0.5 = half speed");
        const bool c2 = g.float_field(g.id("pary"), Rect{ b.x + 96, y, 40, 13 }, py, -4.0, 4.0, 0.05, "Vertical factor");
        if (c1 || c2) { doc.push_undo(); doc.set_parallax(layer_, px, py); }
        y += 18;
        const std::vector<int> use = doc.gid_usage(layer_);
        int cells = 0;
        for (size_t k = 1; k < use.size(); ++k) cells += use[k];
        g.text(b.x, y, fmt("%d painted cells", cells), g.th.faint);
        y += 11;
        g.text(b.x, y, layer_ == n - 1 ? "the LAST layer is the gameplay layer:" : "a backdrop layer (no collision)", layer_ == n - 1 ? g.th.good : g.th.faint, kSubText, b.w);
        if (layer_ == n - 1) g.text(b.x, y + 10, "physics reads its tiles", g.th.good, kSubText, b.w);
    }

    void side_spawns(Host& h, const Rect& b) {
        Gui& g = h.gui();
        int y = b.y;
        const std::vector<std::string>& ts = types(h);
        g.text(b.x, y + 2, "place", g.th.dim);
        int sel = 0;
        for (size_t i = 0; i < ts.size(); ++i) if (ts[i] == spawn_type_) sel = int(i);
        if (g.dropdown(g.id("stype"), Rect{ b.x + 36, y, b.w - 36, 13 }, ts, sel, "The type new spawns get (from prefab tables + this map)")) {
            spawn_type_ = ts[size_t(sel)];
            tool_ = MTool::Spawn;
        }
        y += 17;
        // list
        const int rows = std::max(3, std::min(int(doc.spawns.size()), (b.h - 130) / 11));
        const Rect list{ b.x, y, b.w, rows * 11 + 2 };
        g.rect(list, g.th.field, kSubWidget);
        int& scroll = spawn_scroll_;
        g.wheel_scroll(list, scroll, int(doc.spawns.size()), rows, 1);
        scroll = clamp_scroll(scroll, int(doc.spawns.size()), rows);
        for (int i = 0; i < rows && scroll + i < int(doc.spawns.size()); ++i) {
            const int k = scroll + i;
            const TiledSpawn& s = doc.spawns[size_t(k)];
            const Rect rr{ list.x + 1, list.y + 1 + i * 11, list.w - 2, 11 };
            if (k == spawn_sel_) g.rect(rr, g.th.sel, kSubImage);
            else if (g.hover(rr)) g.rect(rr, g.th.hover, kSubImage);
            g.rect(Rect{ rr.x + 2, rr.y + 3, 5, 5 }, type_colour(s.type), kSubOver);
            g.text(rr.x + 10, rr.y + 2, s.name, g.th.text, kSubText, rr.w - 70);
            g.text(rr.right() - 60, rr.y + 2, fmt("%d,%d", s.x, s.y), g.th.faint);
            if (g.clicked(rr)) { spawn_sel_ = k; tool_ = MTool::Spawn; }
        }
        if (doc.spawns.empty()) g.text(list.x + 4, list.y + 3, "no spawns - pick a type, then click the map", g.th.faint, kSubText, list.w - 6);
        y = list.bottom() + 6;
        if (spawn_sel_ < 0 || spawn_sel_ >= int(doc.spawns.size())) return;
        TiledSpawn s = doc.spawns[size_t(spawn_sel_)];
        g.section(b.x, y, b.w, "SELECTED SPAWN", g.th.faint);
        y += 12;
        bool ch = false;
        g.text(b.x, y + 3, "name", g.th.dim);
        ch |= g.text_field(g.id("sname"), Rect{ b.x + 34, y, b.w - 34, 13 }, s.name, "name");
        y += 16;
        g.text(b.x, y + 3, "type", g.th.dim);
        ch |= g.text_field(g.id("stext"), Rect{ b.x + 34, y, b.w - 34, 13 }, s.type, "type", 0, "Baked as fnv1a(type): the game switches on it");
        y += 16;
        g.text(b.x, y + 3, "x,y", g.th.dim);
        ch |= g.int_field(g.id("sx"), Rect{ b.x + 34, y, 44, 13 }, s.x, 0, doc.width * doc.tile_w - 1, 1, "Pixel position (drag on the map; Shift = no snapping)");
        ch |= g.int_field(g.id("sy"), Rect{ b.x + 82, y, 44, 13 }, s.y, 0, doc.height * doc.tile_h - 1, 1);
        y += 16;
        g.text(b.x, y + 3, "w,h", g.th.dim);
        ch |= g.int_field(g.id("sw"), Rect{ b.x + 34, y, 44, 13 }, s.w, 0, 4096, 1, "Size in pixels (0 = a point)");
        ch |= g.int_field(g.id("sh"), Rect{ b.x + 82, y, 44, 13 }, s.h, 0, 4096, 1);
        y += 18;

        // Per-instance properties (Tiled custom properties). One named like a prefab column
        // overrides it for this spawn only; any other is this spawn's own data for the game.
        g.section(b.x, y, b.w, "PROPERTIES", g.th.faint);
        y += 12;
        const std::vector<std::string>& kinds = TmapDoc::prop_types();
        for (size_t k = 0; k < s.props.size(); ++k) {
            TiledProp& p = s.props[k];
            const int ik = int(k);
            ch |= g.text_field(g.id("pname", ik), Rect{ b.x, y, 46, 13 }, p.name, "name", 0,
                               "The property's name: a prefab column (w, h, sprite, body, ...) overrides it");
            std::vector<std::string> items = kinds;
            int ts = 3;
            for (size_t t = 0; t < items.size(); ++t) if (items[t] == p.type) ts = int(t);
            if (std::find(items.begin(), items.end(), p.type) == items.end()) { items.push_back(p.type); ts = int(items.size()) - 1; }
            if (g.dropdown(g.id("ptype", ik), Rect{ b.x + 48, y, 40, 13 }, items, ts, "int, float, bool or string")) {
                p.type = items[size_t(ts)];
                p.value = TmapDoc::normalise_prop(p.type, p.value);
                ch = true;
            }
            const Rect vr{ b.x + 90, y, b.w - 90 - 14, 13 };
            if (p.type == "bool") {
                const bool on = p.value == "true";
                if (g.button(vr, on ? "true" : "false", Btn{ on, true, false, 0, "Click to toggle" })) { p.value = on ? "false" : "true"; ch = true; }
            } else if (g.text_field(g.id("pval", ik), vr, p.value, "value")) {
                ch = true;
            }
            if (g.icon_button(Rect{ b.right() - 12, y, 12, 13 }, kIconClose, "Remove this property")) {
                s.props.erase(s.props.begin() + long(k));
                ch = true;
                break;
            }
            y += 15;
        }
        if (s.props.empty()) {
            g.text(b.x, y + 1, "none: this spawn is its prefab as is", g.th.faint, kSubText, b.w);
            y += 11;
        }
        if (g.button(Rect{ b.x, y, 70, 13 }, "property", Btn{ false, true, false, kIconPlus,
                     "Add a property to this spawn (e.g. range = 24, or w = 12 to override the prefab)" })) {
            TiledProp np;
            np.name = doc.fresh_prop_name(spawn_sel_, "prop");
            np.type = "int"; np.value = "0";
            s.props.push_back(np);
            ch = true;
        }
        y += 18;
        // (Values stay as typed while editing, so "-" can become "-5"; saving writes each one as
        // its type stores it, TmapDoc::prop_value_json.)
        if (ch) { doc.push_undo(); doc.spawns[size_t(spawn_sel_)] = s; doc.dirty = true; }
        if (g.button(Rect{ b.x, y, 60, 13 }, "delete", Btn{ false, true, false, kIconTrash, "Remove this spawn (Delete)" })) {
            doc.push_undo(); doc.remove_spawn(spawn_sel_); spawn_sel_ = -1;
        }
    }

    void side_map(Host& h, const Rect& b) {
        Gui& g = h.gui();
        int y = b.y;
        g.text(b.x, y, fmt("%d x %d tiles, %d x %d px each", doc.width, doc.height, doc.tile_w, doc.tile_h), g.th.text, kSubText, b.w);
        y += 11;
        g.text(b.x, y, fmt("%d x %d px", doc.width * doc.tile_w, doc.height * doc.tile_h), g.th.faint);
        y += 13;
        if (g.button(Rect{ b.x, y, b.w, 13 }, "resize map...", Btn{ false, true, false, 0, "Change the size in tiles (anchor where the old map sits)" })) resize_dialog(h);
        y += 20;
        g.section(b.x, y, b.w, "TILESET", g.th.faint);
        y += 12;
        g.text(b.x, y + 3, "name", g.th.dim);
        std::string nm = doc.tileset;
        if (g.text_field(g.id("tsname"), Rect{ b.x + 36, y, b.w - 36, 13 }, nm, "tileset", 0, "The texture the bake references (= the tileset PNG's file name without .png)")) {
            if (!nm.empty()) { doc.push_undo(); doc.tileset = nm; doc.dirty = true; load_tileset(h); }
        }
        y += 16;
        // choose image
        std::vector<std::string> pngs = { "(none)" };
        int cur = 0;
        for (const std::string& p : h.files_with({ ".png" })) {
            pngs.push_back(p);
            if (join_path(h.root(), p) == ts_path) cur = int(pngs.size()) - 1;
        }
        g.text(b.x, y + 3, "image", g.th.dim);
        if (g.dropdown(g.id("tsimg"), Rect{ b.x + 36, y, b.w - 36, 13 }, pngs, cur, "The tileset PNG (sets the name too, so the bake finds it)")) {
            doc.push_undo();
            if (cur == 0) { doc.tileset_image.clear(); }
            else {
                std::error_code ec;
                const std::string abs = join_path(h.root(), pngs[size_t(cur)]);
                std::string relp = pfs::relative(abs, pfs::path(path).parent_path(), ec).generic_string();
                doc.tileset_image = ec || relp.empty() ? abs : relp;
                doc.tileset = stem_of(abs);
            }
            doc.dirty = true;
            load_tileset(h);
        }
        y += 17;
        if (ts_real) {
            g.text(b.x, y, fmt("%dx%d px = %d tiles", ts_w, ts_h, ts_count()), g.th.faint);
            if (g.button(Rect{ b.right() - 40, y - 2, 40, 12 }, "edit", Btn{ false, true, false, 0, "Open the tileset in the pixel editor" })) h.open_file(ts_path);
            y += 12;
            if (ts_w % doc.tile_w || ts_h % doc.tile_h) { g.text(b.x, y, "image is not a whole number of tiles", g.th.warn, kSubText, b.w); y += 11; }
        } else {
            g.text(b.x, y, "no image: the tiles are swatches", g.th.faint, kSubText, b.w);
            y += 12;
            Btn cb; cb.icon = kIconPlus; cb.on = true; cb.help = "Make a real tileset PNG you can paint (starting from the swatches)";
            if (g.button(Rect{ b.x, y, b.w, 13 }, "create tileset...", cb)) create_tileset_dialog(h);
            y += 18;
        }
        y += 6;
        g.section(b.x, y, b.w, "BAKE", g.th.faint);
        y += 12;
        g.text(b.x, y, "phxtile / phxpack read this .tmj;", g.th.faint, kSubText, b.w);
        g.text(b.x, y + 10, "the last layer collides.", g.th.faint, kSubText, b.w);
    }

    // Turn a swatch map into a real tileset: a PNG of cols x rows tiles at the map's tile size,
    // starting from the swatch colours (so the map looks the same), saved next to the map and linked
    // as the map's tileset (image + name); then open it in the pixel editor on tile 1.
    void create_tileset_dialog(Host& h) {
        struct St { int cols = 8, rows = 4; std::string name; std::string err; bool swatches = true; };
        auto st = std::make_shared<St>();
        st->name = doc.tileset.empty() ? "tiles" : doc.tileset;
        int max_gid = 0;
        for (const auto& L : doc.layers) for (uint16_t g : L) max_gid = std::max<int>(max_gid, g);
        while (st->cols * st->rows < max_gid) ++st->rows;
        h.modal("Create a tileset", 330, 132, [this, st, &h, max_gid](Gui& g, Rect body) {
            g.text(body.x, body.y + 3, "file", g.th.dim);
            g.text_field(g.id("cts-name"), Rect{ body.x + 50, body.y, 120, 13 }, st->name, "tiles", 0,
                         "The PNG's name (= the tileset name the bake uses to find the texture)");
            g.text(body.x + 174, body.y + 3, ".png  next to the map", g.th.faint);
            g.text(body.x, body.y + 20, "tiles", g.th.dim);
            g.int_field(g.id("cts-c"), Rect{ body.x + 50, body.y + 17, 40, 13 }, st->cols, 1, 64, 1, "Tiles per row");
            g.text(body.x + 94, body.y + 20, "x", g.th.dim);
            g.int_field(g.id("cts-r"), Rect{ body.x + 102, body.y + 17, 40, 13 }, st->rows, 1, 64, 1, "Rows of tiles");
            g.text(body.x + 148, body.y + 20, fmt("= %d tiles of %dx%d", st->cols * st->rows, doc.tile_w, doc.tile_h), g.th.faint);
            g.checkbox(Rect{ body.x, body.y + 34, 220, 13 }, "start from the swatch colours", st->swatches);
            if (st->cols * st->rows < max_gid)
                g.text(body.x, body.y + 52, fmt("the map uses GIDs up to %d - make room for them", max_gid), g.th.warn, kSubText, body.w);
            if (!st->err.empty()) g.text(body.x, body.y + 64, st->err, g.th.bad, kSubText, body.w);
            const int y = body.bottom() - 14;
            if (g.button(Rect{ body.x, y, 70, 14 }, "Create", Btn{ true })) {
                std::string stem;
                for (char c : st->name) if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-') stem += c;
                if (stem.empty()) { st->err = "give the tileset a name"; return true; }
                const std::string png = join_path(dir_name(path), stem + ".png");
                if (h.access(png) != Access::Write) { st->err = "that file would be outside the project"; return true; }
                if (pfs::exists(png)) { st->err = stem + ".png already exists - pick it in the Map tab instead"; return true; }
                const int tw = doc.tile_w, th = doc.tile_h;
                PixelDoc img = PixelDoc::blank(st->cols * tw, st->rows * th, 0);
                if (st->swatches)
                    for (int t = 0; t < st->cols * st->rows; ++t) {
                        const int ox = (t % st->cols) * tw, oy = (t / st->cols) * th;
                        for (int y2 = 0; y2 < th; ++y2)
                            for (int x2 = 0; x2 < tw; ++x2)
                                img.set(ox + x2, oy + y2, ts_px[size_t(y2) * size_t(ts_w) + size_t((t % 16) * tw + x2)]);
                    }
                std::string err;
                if (!img.save_png(png, &err)) { st->err = err; return true; }
                doc.push_undo();
                doc.tileset = stem;
                doc.tileset_image = stem + ".png";
                doc.dirty = true;
                load_tileset(h);
                h.file_saved(png);
                h.toast("created " + base_name(png) + " - paint the tiles, save, and the map updates", Toast::Good);
                h.open_tile(png, 0, tw, th);
                return false;
            }
            if (g.button(Rect{ body.x + 76, y, 70, 14 }, "Cancel") || g.key(PHX_KEY_ESCAPE)) return false;
            return true;
        });
    }

    void resize_dialog(Host& h) {
        struct St { int w, hh, ax = 0, ay = 2; };
        auto st = std::make_shared<St>();
        st->w = doc.width; st->hh = doc.height;
        h.modal("Resize map", 280, 124, [this, st](Gui& g, Rect body) {
            g.text(body.x, body.y + 3, "tiles", g.th.dim);
            g.int_field(g.id("mw"), Rect{ body.x + 50, body.y, 50, 13 }, st->w, 1, 4096, 1);
            g.text(body.x + 104, body.y + 3, "x", g.th.dim);
            g.int_field(g.id("mh"), Rect{ body.x + 112, body.y, 50, 13 }, st->hh, 1, 4096, 1);
            g.text(body.x, body.y + 22, "anchor", g.th.dim);
            for (int ay = 0; ay < 3; ++ay)
                for (int ax = 0; ax < 3; ++ax)
                    if (g.button(Rect{ body.x + 50 + ax * 14, body.y + 20 + ay * 12, 13, 11 }, "", Btn{ st->ax == ax && st->ay == ay }))
                        { st->ax = ax; st->ay = ay; }
            g.text(body.x + 100, body.y + 24, "where the old map sits", g.th.faint);
            const int y = body.bottom() - 14;
            if (g.button(Rect{ body.x, y, 70, 14 }, "Resize", Btn{ true })) {
                doc.push_undo();
                doc.resize(st->w, st->hh, st->ax, st->ay);
                cv_.fitted = false;
                sel_x1_ = -1;
                return false;
            }
            if (g.button(Rect{ body.x + 76, y, 70, 14 }, "Cancel") || g.key(PHX_KEY_ESCAPE)) return false;
            return true;
        });
    }
};

} // namespace

std::unique_ptr<DocView> make_map_view(Host& h, const std::string& path, std::string* err) {
    std::unique_ptr<MapView> v(new MapView);
    if (!v->load(h, path, err)) return nullptr;
    return v;
}

std::unique_ptr<DocView> make_map_view_new(Host& h, const std::string& path, const phxtool::TmapDoc& doc) {
    std::unique_ptr<MapView> v(new MapView);
    v->init_new(h, path, doc);
    return v;
}

} // namespace phxstudio
