// tools/phxtmap/main.cpp — the GUI tilemap editor (docs/08 §1): edits the open Tiled `.tmj`
// author format (never engine blobs) with the mouse, DOGFOODING the engine itself — the same
// App loop, SDL window, software renderer, and immediate-mode UI the games use (the risk
// register's mitigation for editor maintenance). The document logic lives in editor.h and is
// unit-tested headlessly; this file is only the interactive shell.
//
//   phxtmap [--out FILE.tmj] [--size WxH] [FILE.tmj]
//
//   mouse LMB        paint the selected tile (drag paints) / pick from the palette strip
//                    (entity mode: place a spawn of the selected type)
//   hold X key (B)   + LMB: erase tiles / remove spawns
//   C key (X)        cycle the selected tile GID          E key (R)  toggle tile/entity mode
//   Q key (L)        cycle the spawn type                 Tab        cycle the edited layer
//   arrows/WASD      scroll the camera                    Enter      SAVE to --out
//
// Tiles render as a procedural 16-swatch atlas (layout editing needs positions, not art;
// the baked game shows the real tileset). Window close quits; unsaved edits show a '*'.
#include "phx/runtime/app.h"
#include "phx/ui/ui.h"
#include "phx/render/renderer.h"
#include "phx/platform/platform.h"
#include "phx/input/input.h"

#include "editor.h"       // TmapDoc (tools/phxtmap)
#include "debug_font.h"   // tools/common

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace phx;

namespace {

constexpr int kViewW = 320, kViewH = 240;      // logical framebuffer (window is 3x)
constexpr int kPaletteN = 16;                  // GIDs 1..16 as procedural swatches
const char*   kSpawnTypes[] = { "player", "coin", "enemy", "spike" };
constexpr int kSpawnTypeN = 4;

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

struct EditorGame final : Game {
    phxtool::TmapDoc doc;
    std::string      out_path = "level.tmj";

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

    static uint32_t g_font_px[phxtool::kDebugFontW * phxtool::kDebugFontH];
    static uint32_t g_tiles_px[kPaletteN * 8 * 8];

    void reflatten() {
        size_t o = 0;
        for (const auto& L : doc.layers) { std::memcpy(flat.data() + o, L.data(), L.size() * 2); o += L.size(); }
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

        flat.assign(size_t(doc.width) * doc.height * doc.layers.size(), 0);
        reflatten();
        TilemapDesc md{};
        md.indices = flat.data(); md.width = uint16_t(doc.width); md.height = uint16_t(doc.height);
        md.layers = uint8_t(doc.layers.size());
        md.tile_w = uint8_t(doc.tile_w); md.tile_h = uint8_t(doc.tile_h); md.tileset = tiles_tex;
        map = r.upload_tilemap(md);                 // soft backend keeps the pointer: edits are live
    }

    void on_fixed_update(App& app, scalar) override {
        const InputState& in = app.input();

        if (in.down(Button::Left))  cam_x -= 2;
        if (in.down(Button::Right)) cam_x += 2;
        if (in.down(Button::Up))    cam_y -= 2;
        if (in.down(Button::Down))  cam_y += 2;

        if (in.just(Button::Select)) layer = (layer + 1) % int(doc.layers.size());
        if (in.just(Button::X))      gid = gid % kPaletteN + 1;
        if (in.just(Button::R))      entity_mode = !entity_mode;
        if (in.just(Button::L))      spawn_type = (spawn_type + 1) % kSpawnTypeN;
        if (in.just(Button::Start)) {
            if (doc.save_file(out_path)) std::printf("phxtmap: saved %s\n", out_path.c_str());
            else                         std::fprintf(stderr, "phxtmap: cannot write %s\n", out_path.c_str());
        }

        const int px = s_to_int(in.pointer.x), py = s_to_int(in.pointer.y);
        const bool press = in.pointer_down && !prev_down;
        const bool erase = in.down(Button::B);

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
                        if (erase) doc.remove_spawn_at(wx, wy);
                        else       doc.add_spawn(kSpawnTypes[spawn_type], wx, wy);
                    }
                } else if (wx >= 0 && wy >= 0) {                      // drag paints
                    const int tx = wx / doc.tile_w, ty = wy / doc.tile_h;
                    if (doc.in_bounds(tx, ty)) {
                        doc.set_tile(layer, tx, ty, erase ? uint16_t(0) : uint16_t(gid));
                        reflatten();                                  // live into the soft backend
                    }
                }
            }
        }
        prev_down = in.pointer_down;
    }

    void on_render(App& app, scalar) override {
        Renderer& r = app.render();
        Camera2D cam{};
        cam.pos = vec2{ s_from_int(cam_x), s_from_int(cam_y) };
        r.begin_frame(cam);
        for (uint8_t l = 0; l < uint8_t(doc.layers.size()); ++l) r.draw_tilemap(map, l);

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
        }
        char status[64]; int n = 0;
        auto put = [&](const char* s) { while (*s && n < 62) status[n++] = *s++; };
        put(entity_mode ? "ENT " : "TILE ");
        if (entity_mode) { put(kSpawnTypes[spawn_type]); }
        else { put("L"); status[n++] = char('0' + layer); put(" G");
               if (gid >= 10) status[n++] = char('0' + gid / 10);
               status[n++] = char('0' + gid % 10); }
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

} // namespace

int main(int argc, char** argv) {
    EditorGame game;
    int bw = 24, bh = 18;
    const char* in_path = nullptr;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc)       game.out_path = argv[++i];
        else if (a == "--size" && i + 1 < argc) std::sscanf(argv[++i], "%dx%d", &bw, &bh);
        else if (a == "--help") {
            std::printf("usage: phxtmap [--out FILE.tmj] [--size WxH] [FILE.tmj]\n");
            return 0;
        } else in_path = argv[i];
    }

    if (in_path) {
        std::string text;
        if (FILE* f = std::fopen(in_path, "rb")) {
            int c; while ((c = std::fgetc(f)) != EOF) text += char(c);
            std::fclose(f);
        }
        if (text.empty() || !phxtool::TmapDoc::load(text, game.doc)) {
            std::fprintf(stderr, "phxtmap: cannot load '%s'\n", in_path);
            return 1;
        }
        if (game.out_path == "level.tmj") game.out_path = in_path;   // default: save in place
        std::printf("phxtmap: loaded %s (%dx%d, %zu layers, %zu spawns)\n", in_path,
                    game.doc.width, game.doc.height, game.doc.layers.size(), game.doc.spawns.size());
    } else {
        game.doc = phxtool::TmapDoc::blank(bw, bh, 8, 8, "tiles");
    }

    Config cfg = Config::from_defaults();
    cfg.title = "phxtmap"; cfg.width = kViewW; cfg.height = kViewH;
    cfg.sim_hz = 60; cfg.vsync = 1;
    App app(cfg);
    return app.run(&game);
}
