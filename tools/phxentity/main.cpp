// tools/phxentity/main.cpp — the GUI entity/prefab table editor (docs/08 §1): edits the
// phxbin author JSON (typed record tables → `phxbin` bakes them to .phxbin + a .gen.h),
// DOGFOODING the engine — the same App loop, SDL window, software renderer, and UI the
// games use. The document logic lives in editor.h and is unit-tested headlessly.
//
//   phxentity [--out FILE.json] FILE.json
//
//   arrows/WASD   move the cell cursor (record row / field column)
//   Z (A)         +1 on the cell        X key (B)   -1 on the cell
//   Q (L) / E (R) -10 / +10             C key (X)   NEW record (clone of the cursor row)
//   V (Y)         DELETE the cursor row Enter       SAVE to --out
//
// Values clamp to the field's declared type range. Unsaved edits show a '*'.
#include "phx/runtime/app.h"
#include "phx/ui/ui.h"
#include "phx/render/renderer.h"
#include "phx/platform/platform.h"
#include "phx/input/input.h"

#include "editor.h"       // BinDoc (this dir — tools/phxentity)
#include "debug_font.h"   // tools/common

#include <cstdio>
#include <string>

using namespace phx;

namespace {

constexpr int kViewW = 320, kViewH = 240;

struct EntityGame final : Game {
    phxtool::BinDoc doc;
    std::string     out_path = "prefabs.json";

    UI         ui;
    BitmapFont font{};
    int row = 0, col = 0, scroll = 0;

    static uint32_t g_font_px[phxtool::kDebugFontW * phxtool::kDebugFontH];

    void on_start(App& app) override {
        phxtool::build_debug_font(g_font_px);
        TextureDesc fd{}; fd.pixels = g_font_px;
        fd.width = phxtool::kDebugFontW; fd.height = phxtool::kDebugFontH;
        font.tex = app.render().load_texture(fd);
    }

    void clamp_cursor() {
        const int rows = int(doc.records.size()), cols = int(doc.fields.size());
        if (row >= rows) row = rows ? rows - 1 : 0;
        if (row < 0) row = 0;
        if (col >= cols) col = cols ? cols - 1 : 0;
        if (col < 0) col = 0;
    }

    void on_fixed_update(App& app, scalar) override {
        const InputState& in = app.input();
        if (in.just(Button::Up))    --row;
        if (in.just(Button::Down))  ++row;
        if (in.just(Button::Left))  --col;
        if (in.just(Button::Right)) ++col;
        clamp_cursor();

        if (in.just(Button::A)) doc.step(size_t(row), size_t(col), +1);
        if (in.just(Button::B)) doc.step(size_t(row), size_t(col), -1);
        if (in.just(Button::L)) doc.step(size_t(row), size_t(col), -10);
        if (in.just(Button::R)) doc.step(size_t(row), size_t(col), +10);
        if (in.just(Button::X)) { doc.add_record(size_t(row)); row = int(doc.records.size()) - 1; }
        if (in.just(Button::Y)) { doc.remove_record(size_t(row)); clamp_cursor(); }
        if (in.just(Button::Start)) {
            if (doc.save_file(out_path)) std::printf("phxentity: saved %s\n", out_path.c_str());
            else                         std::fprintf(stderr, "phxentity: cannot write %s\n", out_path.c_str());
        }

        // keep the cursor row on screen (12px rows, header + status reserve ~40px)
        const int visible = (kViewH - 40) / 12;
        if (row < scroll) scroll = row;
        if (row >= scroll + visible) scroll = row - visible + 1;
    }

    void on_render(App& app, scalar) override {
        Renderer& r = app.render();
        r.begin_frame(Camera2D{});
        ui.begin(r, app.input());

        ui.rect(UIRect{ vec2{}, vec2{ s_from_int(kViewW), s_from_int(kViewH) } }, rgba(24, 24, 34), 10);

        char line[64]; int n;
        auto put = [&](const char* s) { while (*s && n < 62) line[n++] = *s++; };
        auto put_int = [&](int64_t v) {
            char tmp[24]; std::snprintf(tmp, sizeof(tmp), "%lld", (long long)v); put(tmp); };

        // header: struct name + field columns
        n = 0; put(doc.struct_name.c_str()); if (doc.dirty) put(" *"); line[n] = 0;
        ui.text(vec2{ s_from_int(4), s_from_int(4) }, font, line, rgba(255, 255, 120), 20);
        const int col_w = 9 * 8;                      // 8 chars + a gap per column
        for (size_t f = 0; f < doc.fields.size(); ++f) {
            n = 0; put(doc.fields[f].name.c_str()); line[n] = 0;
            ui.text(vec2{ s_from_int(40 + int(f) * col_w), s_from_int(16) }, font, line,
                    int(f) == col ? rgba(255, 255, 120) : rgba(150, 150, 170), 20);
        }

        // records grid
        const int visible = (kViewH - 40) / 12;
        for (int v = 0; v < visible; ++v) {
            const int rec = scroll + v;
            if (rec >= int(doc.records.size())) break;
            const int y = 28 + v * 12;
            if (rec == row)
                ui.rect(UIRect{ vec2{ s_from_int(2), s_from_int(y - 1) },
                                vec2{ s_from_int(kViewW - 4), s_from_int(10) } }, rgba(50, 50, 80), 15);
            n = 0; put_int(rec); line[n] = 0;
            ui.text(vec2{ s_from_int(4), s_from_int(y) }, font, line, rgba(120, 120, 140), 20);
            for (size_t f = 0; f < doc.fields.size(); ++f) {
                n = 0; put_int(doc.records[size_t(rec)][f]); line[n] = 0;
                const bool cur = rec == row && int(f) == col;
                ui.text(vec2{ s_from_int(40 + int(f) * col_w), s_from_int(y) }, font, line,
                        cur ? rgba(255, 255, 255) : rgba(200, 200, 215), 20);
            }
        }

        // status/help line
        n = 0; put("Z:+1 X:-1 Q-E:-+10 C:NEW V:DEL ENTER:SAVE"); line[n] = 0;
        ui.text(vec2{ s_from_int(4), s_from_int(kViewH - 10) }, font, line, rgba(150, 150, 170), 20);

        ui.end();
        r.end_frame();
    }
};

uint32_t EntityGame::g_font_px[phxtool::kDebugFontW * phxtool::kDebugFontH];

} // namespace

int main(int argc, char** argv) {
    EntityGame game;
    const char* in_path = nullptr;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) game.out_path = argv[++i];
        else if (a == "--help") {
            std::printf("usage: phxentity [--out FILE.json] FILE.json\n");
            return 0;
        } else in_path = argv[i];
    }
    if (!in_path) { std::fprintf(stderr, "phxentity: need an input .json (see --help)\n"); return 1; }

    std::string text;
    if (FILE* f = std::fopen(in_path, "rb")) {
        int c; while ((c = std::fgetc(f)) != EOF) text += char(c);
        std::fclose(f);
    }
    if (text.empty() || !phxtool::BinDoc::load(text, game.doc)) {
        std::fprintf(stderr, "phxentity: cannot load '%s'\n", in_path);
        return 1;
    }
    if (game.out_path == "prefabs.json") game.out_path = in_path;   // default: save in place
    std::printf("phxentity: loaded %s ('%s': %zu fields, %zu records)\n", in_path,
                game.doc.struct_name.c_str(), game.doc.fields.size(), game.doc.records.size());

    Config cfg = Config::from_defaults();
    cfg.title = "phxentity"; cfg.width = kViewW; cfg.height = kViewH;
    cfg.sim_hz = 60; cfg.vsync = 1;
    App app(cfg);
    return app.run(&game);
}
