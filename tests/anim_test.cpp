// tests/anim_test.cpp — a HEADLESS animation slice. Boots the full App, drives an Animator
// through the real fixed-step loop, and renders the current frame's source rect from a
// 4-frame sprite sheet where each frame is a distinct colour. Verifies via BOTH the
// framebuffer (the on-screen colour is the expected frame) and the ECS (frame index).
#include "phx/runtime/app.h"
#include "phx/anim/anim.h"
#include "phx/render/renderer.h"
#include "phx/ecs/world.h"
#include "phx/platform/platform.h"
#include "phx/platform/gfx_soft.h"

#include <cstdio>

extern "C" void phx_null_set_step_ns(uint64_t);
extern "C" void phx_null_set_max_frames(uint64_t);
extern "C" void phx_null_set_buttons(uint32_t);

using namespace phx;

namespace {
constexpr int FBW = 64, FBH = 48, FRAMES = 16;   // 16 steps @ 8fps -> frame index 2
const Rgba kClear  = rgba(30, 30, 46);
const Rgba kFrame[4] = { rgba(220,40,40), rgba(40,200,40), rgba(40,40,200), rgba(200,200,40) };

// 4-frame sheet: 32x8, each 8x8 cell a solid frame colour (cols=4).
uint32_t g_sheet[32 * 8];
const AnimClip kClips[1] = { { /*first*/0, /*count*/4, /*fps*/8, /*loop*/true } };

struct AnimGame final : Game {
    AnimationSystem sys;
    ecs::Entity e   = ecs::kInvalid;
    TextureId   tex = kNoTexture;
    bool ok = false; int checks = 0, fail = 0;

    void on_start(App& app) override {
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 32; ++x)
                g_sheet[y * 32 + x] = kFrame[x / 8];
        TextureDesc d{}; d.pixels = g_sheet; d.width = 32; d.height = 8;
        tex = app.render().load_texture(d);

        e = app.world().spawn();
        Animator a;
        a.clips = Span<const AnimClip>{ kClips, 1 };
        a.sheet.frame_w = 8; a.sheet.frame_h = 8; a.sheet.cols = 4;
        a.play(0);
        app.world().add<Animator>(e, a);
    }

    void on_fixed_update(App& app, scalar dt) override {
        sys.tick(app.world(), dt);
    }

    void on_render(App& app, scalar) override {
        Renderer& r = app.render();
        Camera2D cam{};
        r.begin_frame(cam);
        app.world().each<Animator>([&](ecs::Entity, Animator& a) {
            DrawSprite ds{}; ds.tex = tex;
            ds.sx = a.cur_sx; ds.sy = a.cur_sy; ds.sw = a.cur_sw; ds.sh = a.cur_sh;
            ds.pos = vec2{ s_from_int(20), s_from_int(20) };
            ds.layer = 1; ds.z = 0;
            r.draw_sprite(ds);
        });
        r.end_frame();
    }

    void on_stop(App& app) override {
        phx_soft_fb fb = phx_gfx_soft_lock(app.platform()->gfx());
        auto px = [&](int x, int y) { return fb.pixels[y * fb.w + x]; };

        Animator* a = app.world().get<Animator>(e);
        const int f = a ? int(a->frame) : -1;       // expected 2 after 16 steps @ 8fps

        ++checks; if (f != 2)                       { ++fail; std::printf("    FAIL frame=%d want 2\n", f); }
        ++checks; if (px(24, 24) != kFrame[2])      { ++fail; std::printf("    FAIL on-screen colour=%08X want frame2 %08X\n", px(24,24), kFrame[2]); }
        ++checks; if (px(24, 24) == kFrame[0])      { ++fail; std::printf("    FAIL still showing frame 0 (never animated)\n"); }
        ++checks; if (px(2, 2)  != kClear)          { ++fail; std::printf("    FAIL background not clear\n"); }

        ok = (fail == 0);
        std::printf("    animator reached frame %d; on-screen colour = %08X\n", f, px(24, 24));
    }
};
} // namespace

int main() {
    phx_null_set_step_ns(1000000000ull / 60);
    phx_null_set_max_frames(FRAMES);
    phx_null_set_buttons(0);

    Config cfg = Config::from_defaults();
    cfg.title = "anim"; cfg.sim_hz = 60;
    cfg.width = FBW; cfg.height = FBH;
    cfg.total_ram = 32u << 20; cfg.frame_scratch = 256u << 10; cfg.max_entities = 1024;

    App app(cfg);
    AnimGame game;
    int rc = app.run(&game);

    std::printf("\nanim_test: rc=%d  %d checks, %d failures\n", rc, game.checks, game.fail);
    std::printf((rc == 0 && game.ok) ? "ANIM PASS\n\n" : "ANIM FAIL\n\n");
    return (rc == 0 && game.ok) ? 0 : 1;
}
