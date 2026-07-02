// tests/ui_test.cpp — a HEADLESS UI slice. Boots the full App and, each frame, draws bitmap
// text (tinted), a HUD bar, and two focus-ring menu buttons. A scripted per-frame input
// (tap Down, then tap A) moves the focus ring and activates the second button. Verifies via
// the framebuffer (glyph pixels, bar fg/bg colours) and via UI state (focus + activation).
#include "phx/runtime/app.h"
#include "phx/ui/ui.h"
#include "phx/render/renderer.h"
#include "phx/platform/platform.h"
#include "phx/platform/gfx_soft.h"
#include "phx/input/input.h"

#include <cstdio>

extern "C" void phx_null_set_step_ns(uint64_t);
extern "C" void phx_null_set_max_frames(uint64_t);
extern "C" void phx_null_set_button_script(const uint32_t*, uint32_t);

using namespace phx;

namespace {
constexpr int FBW = 160, FBH = 80, FRAMES = 5;   // wide enough for the profiler overlay too
const Rgba kRed   = rgba(220, 40, 40);
const Rgba kGreen = rgba(40, 200, 40);
const Rgba kGray  = rgba(70, 70, 90);
const Rgba kClear = rgba(30, 30, 46);

// 16x3-cell, 8px font atlas: every cell solid white except cell 0 (space) transparent.
uint32_t g_font[128 * 24];

// Per-frame input: f0 idle, f1 tap Down (move focus 0->1), f2 idle, f3 tap A (activate), f4 idle.
uint32_t g_script[FRAMES];

struct UIGame final : Game {
    UI ui;
    BitmapFont font;
    int activated = -1;
    bool ok = false; int checks = 0, fail = 0;

    void on_start(App& app) override {
        for (int y = 0; y < 24; ++y)
            for (int x = 0; x < 128; ++x) {
                int cell = (x / 8) + (y / 8) * 16;
                g_font[y * 128 + x] = (cell == 0) ? 0u : 0xFFFFFFFFu;   // space transparent
            }
        TextureDesc d{}; d.pixels = g_font; d.width = 128; d.height = 24;
        font.tex = app.render().load_texture(d);
        font.glyph_w = font.glyph_h = 8; font.cols = 16; font.first_char = 32;
        font.advance = 8; font.line_h = 8;
    }

    void on_render(App& app, scalar) override {
        Renderer& r = app.render();
        Camera2D cam{};
        r.begin_frame(cam);
        ui.begin(r, app.input());

        ui.text(vec2{ s_from_int(4), s_from_int(4) }, font, "A B", kRed);
        ui.bar(UIRect{ vec2{ s_from_int(4), s_from_int(20) }, vec2{ s_from_int(40), s_from_int(6) } },
               s_from_int(1) / s_from_int(2), kGreen, kGray);

        if (ui.button(UIRect{ vec2{ s_from_int(4),  s_from_int(30) }, vec2{ s_from_int(40), s_from_int(8) } }, font, "ONE")) activated = 0;
        if (ui.button(UIRect{ vec2{ s_from_int(4),  s_from_int(40) }, vec2{ s_from_int(40), s_from_int(8) } }, font, "TWO")) activated = 1;

        // Dialogue: box 1 reveals HALF of "HI LLAMA" (typewriter mid-line: H I . L drawn,
        // L A M A not yet, no continue marker); box 2 fully reveals it in a NARROW box so
        // "LLAMA" word-wraps to the second row, with the continue marker in the corner.
        DialogueView dlg{};
        dlg.lines = "HI LLAMA"; dlg.stride = 9; dlg.count = 1;
        ui.dialogue(UIRect{ vec2{ s_from_int(64), s_from_int(22) }, vec2{ s_from_int(90), s_from_int(14) } },
                    font, dlg, 0, s_half(s_from_int(1)));
        ui.dialogue(UIRect{ vec2{ s_from_int(64), s_from_int(44) }, vec2{ s_from_int(60), s_from_int(26) } },
                    font, dlg, 0, s_from_int(1));

        // Profiler overlay with a SYNTHETIC profile so the bar pixels are deterministic:
        // update = half the budget (30px), render = the full budget (60px, touching the
        // tick), present = a quarter (15px). Bars-only (no font) — the GBA-safe mode.
        FrameProfile prof{};
        prof.budget_us = 16667;
        prof.update_us = prof.budget_us / 2;
        prof.render_us = prof.budget_us;
        prof.present_us = prof.budget_us / 4;
        prof.frame_us  = prof.update_us + prof.render_us + prof.present_us;
        ui.profile_overlay(vec2{ s_from_int(64), s_from_int(4) }, prof, nullptr);

        ui.end();
        r.end_frame();
    }

    void on_stop(App& app) override {
        phx_soft_fb fb = phx_gfx_soft_lock(app.platform()->gfx());
        auto px = [&](int x, int y) { return fb.pixels[y * fb.w + x]; };

        ++checks; if (px(6, 6)   != kRed)   { ++fail; std::printf("    FAIL 'A' glyph not red (6,6)=%08X\n", px(6,6)); }
        ++checks; if (px(22, 6)  != kRed)   { ++fail; std::printf("    FAIL 'B' glyph not red (22,6)=%08X\n", px(22,6)); }
        ++checks; if (px(14, 6)  != kClear) { ++fail; std::printf("    FAIL space drew something (14,6)=%08X\n", px(14,6)); }
        ++checks; if (px(6, 22)  != kGreen) { ++fail; std::printf("    FAIL bar fg not green (6,22)=%08X\n", px(6,22)); }
        ++checks; if (px(40, 22) != kGray)  { ++fail; std::printf("    FAIL bar bg not gray (40,22)=%08X\n", px(40,22)); }
        ++checks; if (ui.focus() != 1)      { ++fail; std::printf("    FAIL focus=%d want 1\n", ui.focus()); }
        ++checks; if (activated != 1)       { ++fail; std::printf("    FAIL activated=%d want 1\n", activated); }

        // Profiler overlay: phase bars + budget tick at the expected pixels (pos 64,4).
        const Rgba kUpd = rgba(90, 200, 90), kRen = rgba(90, 140, 240), kPre = rgba(230, 160, 70);
        ++checks; if (px(70, 7)   != kUpd)  { ++fail; std::printf("    FAIL update bar (70,7)=%08X\n", px(70,7)); }
        ++checks; if (px(70, 11)  != kRen)  { ++fail; std::printf("    FAIL render bar (70,11)=%08X\n", px(70,11)); }
        ++checks; if (px(125, 11) != kRen)  { ++fail; std::printf("    FAIL render bar reaches the budget (125,11)=%08X\n", px(125,11)); }
        ++checks; if (px(70, 15)  != kPre)  { ++fail; std::printf("    FAIL present bar (70,15)=%08X\n", px(70,15)); }
        ++checks; if (px(126, 7)  != rgba(255,255,255)) { ++fail; std::printf("    FAIL budget tick (126,7)=%08X\n", px(126,7)); }
        // And the loop actually stamped the REAL profile (null clock steps deterministically).
        ++checks; if (app.profile().frame_us == 0) { ++fail; std::printf("    FAIL app profile not stamped\n"); }

        // Dialogue box 1 (half reveal): 'H' and 'L' drawn, 'M' not yet, no marker.
        const Rgba kLabel = rgba(230, 230, 240), kPanel = rgba(50, 50, 70);
        ++checks; if (px(68, 26)  != kLabel) { ++fail; std::printf("    FAIL dlg 'H' (68,26)=%08X\n", px(68,26)); }
        ++checks; if (px(92, 26)  != kLabel) { ++fail; std::printf("    FAIL dlg 'L' revealed (92,26)=%08X\n", px(92,26)); }
        ++checks; if (px(102, 26) != kPanel) { ++fail; std::printf("    FAIL dlg 'M' drawn too early (102,26)=%08X\n", px(102,26)); }
        ++checks; if (px(150, 32) != kPanel) { ++fail; std::printf("    FAIL dlg marker before full reveal (150,32)=%08X\n", px(150,32)); }
        // Dialogue box 2 (full reveal, narrow): "LLAMA" wrapped to row 2 + continue marker.
        ++checks; if (px(68, 48)  != kLabel) { ++fail; std::printf("    FAIL dlg2 'H' (68,48)=%08X\n", px(68,48)); }
        ++checks; if (px(68, 56)  != kLabel) { ++fail; std::printf("    FAIL dlg2 'L' not wrapped to row 2 (68,56)=%08X\n", px(68,56)); }
        ++checks; if (px(120, 66) != kLabel) { ++fail; std::printf("    FAIL dlg2 continue marker (120,66)=%08X\n", px(120,66)); }

        ok = (fail == 0);
        std::printf("    text+bar drawn; focus moved to %d; activated button %d\n", ui.focus(), activated);
    }
};
} // namespace

int main() {
    g_script[0] = 0;
    g_script[1] = button_bit(Button::Down);
    g_script[2] = 0;
    g_script[3] = button_bit(Button::A);
    g_script[4] = 0;

    phx_null_set_step_ns(1000000000ull / 60);
    phx_null_set_max_frames(FRAMES);
    phx_null_set_button_script(g_script, FRAMES);

    Config cfg = Config::from_defaults();
    cfg.title = "ui"; cfg.sim_hz = 60;
    cfg.width = FBW; cfg.height = FBH;
    cfg.total_ram = 32u << 20; cfg.frame_scratch = 256u << 10; cfg.max_entities = 1024;

    App app(cfg);
    UIGame game;
    int rc = app.run(&game);

    std::printf("\nui_test: rc=%d  %d checks, %d failures\n", rc, game.checks, game.fail);
    std::printf((rc == 0 && game.ok) ? "UI PASS\n\n" : "UI FAIL\n\n");
    return (rc == 0 && game.ok) ? 0 : 1;
}
