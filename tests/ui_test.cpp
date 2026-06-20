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
constexpr int FBW = 64, FBH = 48, FRAMES = 5;
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
