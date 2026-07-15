// tools/phxtmap/main.cpp — the GUI tilemap editor (docs/08 §1): edits the open Tiled `.tmj`
// author format (never engine blobs) with the mouse, DOGFOODING the engine itself — the same
// App loop, SDL window, software renderer, and immediate-mode UI the games use (the risk
// register's mitigation for editor maintenance). The document logic lives in editor.h and is
// unit-tested headlessly; this file is only the interactive shell.
//
//   phxtmap [--out FILE.tmj] [--size WxH] [--types a,b,c] [--prefabs TABLE.json] [FILE.tmj]
//
//   mouse LMB        apply the active tool (drag paints; rect drags a marquee) / pick from
//                    the palette strip (entity mode: place a spawn of the selected type)
//   hold X key (B)   + LMB: erase tiles (works with every tool) / remove spawns
//   C key (X)        cycle the selected tile GID          E key (R)  toggle tile/entity mode
//   X+C              cycle the TOOL (paint/fill/rect/pick)
//   Q key (L)        cycle the spawn type                 Tab        cycle the edited layer
//   Z key (A)        UNDO                                 X+Z        REDO
//   V key (Y)        cycle the selected GID's collision   X+Tab      ADD a layer
//                    (none -> solid -> oneway -> hazard)
//   arrows/WASD      scroll the camera                    Enter      SAVE to --out
//
// Spawn types come from --types, from a --prefabs table (the phxentity/phxbin record table
// whose string "type" column names the game's prefabs — ONE author file shared by both
// editors and the bake), plus whatever the loaded map already uses — nothing is hardcoded.
// Collision flags show as a coloured underline in the palette (white=solid,
// yellow=oneway, red=hazard) and are saved as Tiled tileset per-tile properties, which the
// bake turns into the engine's per-tile collision table.
//
// Tiles render as a procedural 16-swatch atlas (layout editing needs positions, not art;
// the baked game shows the real tileset). Window close quits; unsaved edits show a '*'.
#include "phx/runtime/app.h"
#include "phx/ui/ui.h"
#include "phx/render/renderer.h"
#include "phx/platform/platform.h"
#include "phx/input/input.h"

#include "editor.h"                 // TmapDoc (tools/phxtmap)
#include "../phxentity/editor.h"    // BinDoc — the shared prefab-schema table (--prefabs)
#include "debug_font.h"             // tools/common

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace phx;

namespace {

constexpr int kViewW = 320, kViewH = 240;      // logical framebuffer (window is 3x)
constexpr int kPaletteN = 16;                  // GIDs 1..16 as procedural swatches
constexpr int kMaxEditLayers = 8;              // the tilemap is uploaded ONCE at this capacity
                                               // (no unload API — re-uploading would leak slots)

// Distinct, readable swatch colours for GIDs 1..16 (procedural tileset).
Rgba swatch_colour(int gid) {
    static const Rgba kPal[kPaletteN] = {
        rgba(150, 90, 50),  rgba(200, 205, 220), rgba(90, 160, 90),  rgba(70, 110, 200),
        rgba(200, 170, 60), rgba(170, 80, 170),  rgba(80, 180, 180), rgba(200, 90, 60),
        rgba(120, 120, 130),rgba(60, 130, 60),   rgba(160, 130, 200),rgba(210, 140, 150),
        rgba(100, 80, 40),  rgba(50, 90, 110),   rgba(180, 180, 90), rgba(140, 60, 60),
    };
    return kPal[(gid - 1) % kPaletteN];
}

// The paint tools (docs/08 §1): paint = per-cell brush (drag paints), fill = 4-connected
// flood fill, rect = drag a marquee then release to fill it, pick = eyedrop a GID from the
// map (and hop back to paint). All honour the erase modifier by painting GID 0.
enum Tool : uint8_t { kToolPaint, kToolFill, kToolRect, kToolPick, kToolCount };
const char* const kToolNames[kToolCount] = { "PAINT", "FILL", "RECT", "PICK" };

struct EditorGame final : Game {
    phxtool::TmapDoc doc;
    std::string      out_path = "level.tmj";
    std::vector<std::string> spawn_types { "player", "coin", "enemy", "spike" };

    UI          ui;
    BitmapFont  font{};
    TextureId   tiles_tex = kNoTexture;
    TilemapId   map = kNoTilemap;
    std::vector<uint16_t> flat;                // layers stacked, zero-copy live into the soft backend

    int  cam_x = 0, cam_y = 0;
    int  layer = 0;
    int  gid = 1;
    int  spawn_type = 0;
    bool entity_mode = false;
    bool prev_down = false;
    Tool tool = kToolPaint;
    bool     rect_active = false;              // a rect-tool marquee drag is in progress
    int      rect_x0 = 0, rect_y0 = 0, rect_x1 = 0, rect_y1 = 0;   // marquee corners (cells)
    uint16_t rect_gid = 0;                     // GID captured at press (erase state then)

    static uint32_t g_font_px[phxtool::kDebugFontW * phxtool::kDebugFontH];
    static uint32_t g_tiles_px[kPaletteN * 8 * 8];

    // Merge the loaded map's spawn vocabulary into the placeable list (no hardcoding).
    void merge_doc_spawn_types() {
        for (const std::string& t : doc.spawn_types()) {
            bool seen = false;
            for (const std::string& s : spawn_types) if (s == t) { seen = true; break; }
            if (!seen) spawn_types.push_back(t);
        }
    }

    // Copy every document layer into the fixed-capacity flat buffer (unused capacity stays
    // empty). The soft backend keeps the pointer, so this makes any edit — paint, undo,
    // add-layer — live without a re-upload.
    void reflatten() {
        std::fill(flat.begin(), flat.end(), uint16_t(0));
        size_t o = 0;
        int copied = 0;
        for (const auto& L : doc.layers) {
            if (copied++ >= kMaxEditLayers) break;   // beyond the view capacity (kept in the doc)
            std::memcpy(flat.data() + o, L.data(), L.size() * 2);
            o += L.size();
        }
    }

    void upload_map(Renderer& r) {
        flat.assign(size_t(doc.width) * doc.height * kMaxEditLayers, 0);
        reflatten();
        TilemapDesc md{};
        md.indices = flat.data(); md.width = uint16_t(doc.width); md.height = uint16_t(doc.height);
        md.layers = uint8_t(kMaxEditLayers);
        md.tile_w = uint8_t(doc.tile_w); md.tile_h = uint8_t(doc.tile_h); md.tileset = tiles_tex;
        map = r.upload_tilemap(md);                 // soft backend keeps the pointer: edits are live
    }

    void on_start(App& app) override {
        Renderer& r = app.render();

        phxtool::build_debug_font(g_font_px);
        TextureDesc fd{}; fd.pixels = g_font_px;
        fd.width = phxtool::kDebugFontW; fd.height = phxtool::kDebugFontH;
        font.tex = r.load_texture(fd);

        // procedural tileset atlas: 16 swatches side by side (GID n -> atlas cell n-1)
        for (int t = 0; t < kPaletteN; ++t)
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x) {
                    const bool border = x == 0 || y == 0;
                    const Rgba c = swatch_colour(t + 1);
                    g_tiles_px[y * (kPaletteN * 8) + t * 8 + x] =
                        border ? rgba(uint8_t((c & 0xFF) / 2), uint8_t(((c >> 8) & 0xFF) / 2),
                                      uint8_t(((c >> 16) & 0xFF) / 2)) : c;
                }
        TextureDesc td{}; td.pixels = g_tiles_px; td.width = kPaletteN * 8; td.height = 8;
        tiles_tex = r.load_texture(td);

        upload_map(r);
    }

    void on_fixed_update(App& app, scalar) override {
        const InputState& in = app.input();

        if (in.down(Button::Left))  cam_x -= 2;
        if (in.down(Button::Right)) cam_x += 2;
        if (in.down(Button::Up))    cam_y -= 2;
        if (in.down(Button::Down))  cam_y += 2;

        const bool mod = in.down(Button::B);       // X key: erase / alternate action modifier
        if (in.just(Button::Select)) {
            if (mod) {                             // X+Tab: add a layer (and edit it)
                if (int(doc.layers.size()) < kMaxEditLayers) {
                    doc.push_undo();
                    doc.add_layer("");
                    layer = int(doc.layers.size()) - 1;
                    reflatten();
                } else {
                    std::printf("phxtmap: layer cap (%d) reached\n", kMaxEditLayers);
                }
            } else {
                layer = (layer + 1) % int(doc.layers.size());
            }
        }
        if (in.just(Button::X)) {                  // C: cycle GID, X+C: cycle the tool
            if (mod) { tool = Tool((tool + 1) % kToolCount); rect_active = false; }
            else     gid = gid % kPaletteN + 1;
        }
        if (in.just(Button::R))      entity_mode = !entity_mode;
        if (in.just(Button::L))      spawn_type = (spawn_type + 1) % int(spawn_types.size());
        if (in.just(Button::A)) {                  // Z: undo, X+Z: redo
            if (mod) { if (!doc.redo()) std::printf("phxtmap: nothing to redo\n"); }
            else     { if (!doc.undo()) std::printf("phxtmap: nothing to undo\n"); }
            reflatten();                           // undo may also add/remove a layer
            if (layer >= int(doc.layers.size())) layer = int(doc.layers.size()) - 1;
        }
        if (in.just(Button::Y) && !entity_mode) {  // V: cycle the selected GID's collision
            doc.push_undo();
            doc.cycle_tile_flag(uint16_t(gid));
        }
        if (in.just(Button::Start)) {
            if (doc.save_file(out_path)) std::printf("phxtmap: saved %s\n", out_path.c_str());
            else                         std::fprintf(stderr, "phxtmap: cannot write %s\n", out_path.c_str());
        }

        const int px = s_to_int(in.pointer.x), py = s_to_int(in.pointer.y);
        const bool press = in.pointer_down && !prev_down;
        const bool erase = mod;

        if (in.pointer_down && px >= 0 && py >= 0) {
            if (py >= kViewH - 12) {                                  // palette strip
                if (press && !entity_mode) {
                    const int slot = (px - 2) / 10;
                    if (slot >= 0 && slot < kPaletteN && (px - 2) % 10 < 8) gid = slot + 1;
                }
            } else {                                                  // map viewport
                const int wx = px + cam_x, wy = py + cam_y;
                if (entity_mode) {
                    if (press) {
                        doc.push_undo();                              // one gesture, one undo step
                        bool changed = true;
                        if (erase) changed = doc.remove_spawn_at(wx, wy);
                        else       doc.add_spawn(spawn_types[size_t(spawn_type)], wx, wy);
                        if (!changed) doc.drop_undo();
                    }
                } else if (wx >= 0 && wy >= 0) {
                    const int tx = wx / doc.tile_w, ty = wy / doc.tile_h;
                    const uint16_t paint = erase ? uint16_t(0) : uint16_t(gid);
                    if (doc.in_bounds(tx, ty)) {
                        switch (tool) {
                        case kToolPaint:                              // drag paints
                            if (press) doc.push_undo();               // a stroke = one undo step
                            doc.set_tile(layer, tx, ty, paint);
                            reflatten();                              // live into the soft backend
                            break;
                        case kToolFill:                               // 4-connected flood fill
                            if (press) {
                                doc.push_undo();
                                if (doc.flood_fill(layer, tx, ty, paint) == 0) doc.drop_undo();
                                else reflatten();
                            }
                            break;
                        case kToolRect:                               // press anchors, drag sizes
                            if (press) {
                                rect_active = true;
                                rect_x0 = rect_x1 = tx; rect_y0 = rect_y1 = ty;
                                rect_gid = paint;                     // erase state AT press
                            } else if (rect_active) {
                                rect_x1 = tx; rect_y1 = ty;
                            }
                            break;
                        case kToolPick:                               // eyedrop -> back to paint
                            if (press) {
                                const uint16_t got = doc.tile(layer, tx, ty);
                                if (got >= 1 && got <= kPaletteN) { gid = got; tool = kToolPaint; }
                            }
                            break;
                        default: break;
                        }
                    }
                }
            }
        }
        if (!in.pointer_down && prev_down && rect_active) {           // release commits the rect
            rect_active = false;
            doc.push_undo();
            if (doc.fill_rect(layer, rect_x0, rect_y0, rect_x1, rect_y1, rect_gid) == 0)
                doc.drop_undo();
            else
                reflatten();
        }
        prev_down = in.pointer_down;
    }

    void on_render(App& app, scalar) override {
        Renderer& r = app.render();
        Camera2D cam{};
        cam.pos = vec2{ s_from_int(cam_x), s_from_int(cam_y) };
        r.begin_frame(cam);
        const uint8_t shown = uint8_t(std::min(int(doc.layers.size()), kMaxEditLayers));
        for (uint8_t l = 0; l < shown; ++l) r.draw_tilemap(map, l);

        ui.begin(r, app.input());

        // spawn markers: a small tinted box + the type's initial, in world space
        for (const auto& s : doc.spawns) {
            const int sx = s.x - cam_x, sy = s.y - cam_y;
            if (sx < -8 || sy < -8 || sx > kViewW || sy > kViewH - 12) continue;
            ui.rect(UIRect{ vec2{ s_from_int(sx - 1), s_from_int(sy - 1) },
                            vec2{ s_from_int(8), s_from_int(8) } }, rgba(240, 240, 240, 160), 190);
            char init[2] = { char(s.type.empty() ? '?' : (s.type[0] & ~0x20)), 0 };   // upper-case
            ui.text(vec2{ s_from_int(sx), s_from_int(sy - 1) }, font, init, rgba(20, 20, 30), 191);
        }

        // rect-tool marquee: a 1px yellow outline over the dragged cell rectangle
        if (rect_active) {
            const int x0 = (rect_x0 < rect_x1 ? rect_x0 : rect_x1) * doc.tile_w - cam_x;
            const int y0 = (rect_y0 < rect_y1 ? rect_y0 : rect_y1) * doc.tile_h - cam_y;
            const int x1 = ((rect_x0 > rect_x1 ? rect_x0 : rect_x1) + 1) * doc.tile_w - cam_x;
            const int y1 = ((rect_y0 > rect_y1 ? rect_y0 : rect_y1) + 1) * doc.tile_h - cam_y;
            const Rgba mc = rgba(255, 255, 90, 200);
            ui.rect(UIRect{ vec2{ s_from_int(x0), s_from_int(y0) },
                            vec2{ s_from_int(x1 - x0), s_from_int(1) } }, mc, 195);
            ui.rect(UIRect{ vec2{ s_from_int(x0), s_from_int(y1 - 1) },
                            vec2{ s_from_int(x1 - x0), s_from_int(1) } }, mc, 195);
            ui.rect(UIRect{ vec2{ s_from_int(x0), s_from_int(y0) },
                            vec2{ s_from_int(1), s_from_int(y1 - y0) } }, mc, 195);
            ui.rect(UIRect{ vec2{ s_from_int(x1 - 1), s_from_int(y0) },
                            vec2{ s_from_int(1), s_from_int(y1 - y0) } }, mc, 195);
        }

        // palette strip + status line
        ui.rect(UIRect{ vec2{ s_from_int(0), s_from_int(kViewH - 12) },
                        vec2{ s_from_int(kViewW), s_from_int(12) } }, rgba(28, 28, 40), 200);
        for (int t = 0; t < kPaletteN; ++t) {
            const int x = 2 + t * 10;
            ui.image(UIRect{ vec2{ s_from_int(x), s_from_int(kViewH - 10) },
                             vec2{ s_from_int(8), s_from_int(8) } },
                     tiles_tex, t * 8, 0, 8, 8, rgba(255, 255, 255), 201);
            if (t + 1 == gid && !entity_mode)                          // selection frame
                ui.rect(UIRect{ vec2{ s_from_int(x - 1), s_from_int(kViewH - 11) },
                                vec2{ s_from_int(10), s_from_int(1) } }, rgba(255, 255, 90), 202);
            // collision underline: white=solid, yellow=oneway, red=hazard
            const uint8_t f = doc.tile_flag(uint16_t(t + 1));
            if (f) {
                const Rgba fc = (f & phx::kTileFlagSolid)  ? rgba(240, 240, 240)
                              : (f & phx::kTileFlagOneWay) ? rgba(240, 220, 60)
                                                           : rgba(240, 70, 60);
                ui.rect(UIRect{ vec2{ s_from_int(x), s_from_int(kViewH - 2) },
                                vec2{ s_from_int(8), s_from_int(1) } }, fc, 202);
            }
        }
        char status[64]; int n = 0;
        auto put = [&](const char* s) { while (*s && n < 62) status[n++] = *s++; };
        put(entity_mode ? "ENT " : "TILE ");
        if (entity_mode) { put(spawn_types[size_t(spawn_type)].c_str()); }
        else {
            put(kToolNames[tool]); put(" ");
            put("L"); status[n++] = char('0' + layer); put(" G");
            if (gid >= 10) status[n++] = char('0' + gid / 10);
            status[n++] = char('0' + gid % 10);
            const uint8_t f = doc.tile_flag(uint16_t(gid));
            if (f & phx::kTileFlagSolid)  put(" SOL");
            if (f & phx::kTileFlagOneWay) put(" ONE");
            if (f & phx::kTileFlagHazard) put(" HAZ");
        }
        if (doc.dirty) put(" *");
        put("  ENTER-SAVE");
        status[n] = 0;
        ui.text(vec2{ s_from_int(kViewW - 8 * (n > 20 ? 20 : n) - 2), s_from_int(kViewH - 10) },
                font, status, rgba(220, 220, 230), 203);

        ui.end();
        r.end_frame();
    }
};

uint32_t EditorGame::g_font_px[phxtool::kDebugFontW * phxtool::kDebugFontH];
uint32_t EditorGame::g_tiles_px[kPaletteN * 8 * 8];

// "a,b,c" -> {"a","b","c"} (empty pieces dropped).
std::vector<std::string> split_types(const std::string& csv) {
    std::vector<std::string> out;
    size_t p = 0;
    while (p <= csv.size()) {
        size_t c = csv.find(',', p);
        if (c == std::string::npos) c = csv.size();
        if (c > p) out.emplace_back(csv.substr(p, c - p));
        p = c + 1;
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    EditorGame game;
    int bw = 24, bh = 18;
    const char* in_path = nullptr;
    const char* prefabs_path = nullptr;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc)       game.out_path = argv[++i];
        else if (a == "--size" && i + 1 < argc) std::sscanf(argv[++i], "%dx%d", &bw, &bh);
        else if (a == "--types" && i + 1 < argc) {
            auto ts = split_types(argv[++i]);
            if (!ts.empty()) game.spawn_types = std::move(ts);
        } else if (a == "--prefabs" && i + 1 < argc) prefabs_path = argv[++i];
        else if (a == "--help") {
            std::printf("usage: phxtmap [--out FILE.tmj] [--size WxH] [--types a,b,c]\n"
                        "               [--prefabs TABLE.json] [FILE.tmj]\n"
                        "  --types    the spawn types placeable in entity mode (default:\n"
                        "             player,coin,enemy,spike, plus any the loaded map uses)\n"
                        "  --prefabs  read the placeable types from a phxentity record table's\n"
                        "             string 'type'/'name' column (the shared prefab schema)\n");
            return 0;
        } else in_path = argv[i];
    }

    if (prefabs_path) {                          // the prefab table IS the vocabulary
        std::string text;
        if (FILE* f = std::fopen(prefabs_path, "rb")) {
            int c; while ((c = std::fgetc(f)) != EOF) text += char(c);
            std::fclose(f);
        }
        phxtool::BinDoc prefabs;
        std::string err;
        if (text.empty() || !phxtool::BinDoc::load(text, prefabs, &err)) {
            std::fprintf(stderr, "phxtmap: cannot load prefab table '%s'%s%s\n", prefabs_path,
                         err.empty() ? "" : ": ", err.c_str());
            return 1;
        }
        auto names = prefabs.name_column();
        if (names.empty()) {
            std::fprintf(stderr, "phxtmap: prefab table '%s' has no string 'type'/'name' "
                         "column (or no named records)\n", prefabs_path);
            return 1;
        }
        game.spawn_types = std::move(names);
        std::printf("phxtmap: %zu prefab types from %s ('%s')\n",
                    game.spawn_types.size(), prefabs_path, prefabs.struct_name.c_str());
    }

    if (in_path) {
        std::string text;
        if (FILE* f = std::fopen(in_path, "rb")) {
            int c; while ((c = std::fgetc(f)) != EOF) text += char(c);
            std::fclose(f);
        }
        if (text.empty()) {
            std::fprintf(stderr, "phxtmap: cannot read '%s'\n", in_path);
            return 1;
        }
        std::string err;
        if (!phxtool::TmapDoc::load(text, game.doc, &err)) {
            std::fprintf(stderr, "phxtmap: cannot load '%s': %s\n", in_path, err.c_str());
            return 1;
        }
        game.merge_doc_spawn_types();
        if (int(game.doc.layers.size()) > kMaxEditLayers)
            std::fprintf(stderr, "phxtmap: note: %zu layers, only the first %d are shown "
                         "(all are kept and saved)\n", game.doc.layers.size(), kMaxEditLayers);
        if (game.out_path == "level.tmj") game.out_path = in_path;   // default: save in place
        std::printf("phxtmap: loaded %s (%dx%d, %zu layers, %zu spawns%s)\n", in_path,
                    game.doc.width, game.doc.height, game.doc.layers.size(), game.doc.spawns.size(),
                    game.doc.has_tile_flags() ? ", collision flags" : "");
    } else {
        game.doc = phxtool::TmapDoc::blank(bw, bh, 8, 8, "tiles");
    }

    Config cfg = Config::from_defaults();
    cfg.title = "phxtmap"; cfg.width = kViewW; cfg.height = kViewH;
    cfg.sim_hz = 60; cfg.vsync = 1;
    App app(cfg);
    return app.run(&game);
}
