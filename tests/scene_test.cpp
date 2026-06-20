// tests/scene_test.cpp — a HEADLESS scene-stack slice. Boots the full App, runs a gameplay
// scene, then pushes a transparent menu over it through the real loop. Verifies via the
// framebuffer that the overlay composites correctly (gameplay shows behind the menu) and
// that the stack ends two deep with the menu on top. Wires the scene stack into App's hooks
// exactly as a real game would: the Game brackets the frame, scenes only draw.
#include "phx/runtime/app.h"
#include "phx/scene/scene.h"
#include "phx/render/renderer.h"
#include "phx/platform/platform.h"
#include "phx/platform/gfx_soft.h"

#include <cstdio>

extern "C" void phx_null_set_step_ns(uint64_t);
extern "C" void phx_null_set_max_frames(uint64_t);
extern "C" void phx_null_set_buttons(uint32_t);

using namespace phx;

// The runtime will define EngineCtx; here the test threads App + the stack to its scenes.
namespace phx { struct EngineCtx { App* app = nullptr; SceneStack* stack = nullptr; }; }

namespace {
constexpr int FBW = 64, FBH = 48, FRAMES = 12;
const Rgba kRed  = rgba(220, 40, 40);
const Rgba kBlue = rgba(40, 40, 200);

uint32_t g_red[8 * 8], g_blue[8 * 8];

void draw_at(EngineCtx* c, TextureId tex, int x, int y, uint8_t layer) {
    DrawSprite ds{}; ds.tex = tex; ds.sx = 0; ds.sy = 0; ds.sw = 8; ds.sh = 8;
    ds.pos = vec2{ s_from_int(x), s_from_int(y) }; ds.layer = layer; ds.z = 0;
    c->app->render().draw_sprite(ds);
}

struct GameplayScene : Scene {
    TextureId tex = kNoTexture;
    void render(EngineCtx* c, scalar) override { draw_at(c, tex, 10, 10, 0); }
};

struct MenuScene : Scene {
    TextureId tex = kNoTexture;
    MenuScene() { render_below = true; }     // gameplay stays visible behind the menu
    void render(EngineCtx* c, scalar) override { draw_at(c, tex, 40, 30, 1); }
};

struct SceneGame final : Game {
    SceneStack*   stack = nullptr;
    GameplayScene game_s;
    MenuScene     menu_s;
    EngineCtx     ctx{};
    StackAllocator scene_scratch;
    bool pushed_menu = false, ok = false; int checks = 0, fail = 0;

    void on_start(App& app) override {
        for (int i = 0; i < 64; ++i) { g_red[i] = kRed; g_blue[i] = kBlue; }
        TextureDesc dr{}; dr.pixels = g_red;  dr.width = 8; dr.height = 8; game_s.tex = app.render().load_texture(dr);
        TextureDesc db{}; db.pixels = g_blue; db.width = 8; db.height = 8; menu_s.tex = app.render().load_texture(db);

        void* buf = app.mem().persistent().alloc(256u << 10);   // scene-scoped scratch
        scene_scratch.init(buf, 256u << 10);
        stack = SceneStack::create(app.mem().persistent(), scene_scratch).unwrap();
        ctx.app = &app; ctx.stack = stack;
        stack->push(&game_s);
    }

    void on_fixed_update(App& app, scalar dt) override {
        if (!pushed_menu && app.frame() >= 6) { stack->push(&menu_s); pushed_menu = true; }
        stack->update(&ctx, dt);
    }

    void on_render(App& app, scalar a) override {
        Renderer& r = app.render();
        Camera2D cam{};
        r.begin_frame(cam);
        stack->render(&ctx, a);      // scenes draw back-to-front; Game owns begin/end
        r.end_frame();
    }

    void on_stop(App& app) override {
        phx_soft_fb fb = phx_gfx_soft_lock(app.platform()->gfx());
        auto px = [&](int x, int y) { return fb.pixels[y * fb.w + x]; };

        ++checks; if (px(13, 13) != kRed)    { ++fail; std::printf("    FAIL gameplay sprite missing (13,13)=%08X\n", px(13,13)); }
        ++checks; if (px(43, 33) != kBlue)   { ++fail; std::printf("    FAIL menu overlay missing (43,33)=%08X\n", px(43,33)); }
        ++checks; if (stack->depth() != 2u)  { ++fail; std::printf("    FAIL depth=%u want 2\n", stack->depth()); }
        ++checks; if (stack->top() != &menu_s){ ++fail; std::printf("    FAIL top is not the menu\n"); }

        ok = (fail == 0);
        std::printf("    stack depth=%u (gameplay + menu overlay both on screen)\n", stack->depth());
        (void)app;
    }
};
} // namespace

int main() {
    phx_null_set_step_ns(1000000000ull / 60);
    phx_null_set_max_frames(FRAMES);
    phx_null_set_buttons(0);

    Config cfg = Config::from_defaults();
    cfg.title = "scene"; cfg.sim_hz = 60;
    cfg.width = FBW; cfg.height = FBH;
    cfg.total_ram = 32u << 20; cfg.frame_scratch = 256u << 10; cfg.max_entities = 1024;

    App app(cfg);
    SceneGame game;
    int rc = app.run(&game);

    std::printf("\nscene_test: rc=%d  %d checks, %d failures\n", rc, game.checks, game.fail);
    std::printf((rc == 0 && game.ok) ? "SCENE PASS\n\n" : "SCENE FAIL\n\n");
    return (rc == 0 && game.ok) ? 0 : 1;
}
