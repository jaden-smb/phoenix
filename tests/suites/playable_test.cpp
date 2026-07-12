// tests/playable_test.cpp — a HEADLESS PLAYABLE slice. Boots the full App, spawns an
// entity, drives it with scripted input through the real fixed-step loop, renders it
// through the software backend, and verifies it actually moved by reading the framebuffer.
// This exercises the whole stack — memory, ecs, input, render, loop — with no window.
#include "phx/runtime/app.h"
#include "phx/input/input.h"
#include "phx/render/renderer.h"
#include "phx/ecs/world.h"
#include "phx/platform/platform.h"
#include "phx/platform/gfx_soft.h"

#include <cstdio>

// null backend test hooks
extern "C" void phx_null_set_step_ns(uint64_t);
extern "C" void phx_null_set_max_frames(uint64_t);
extern "C" void phx_null_set_buttons(uint32_t);

using namespace phx;

namespace {
constexpr int FBW = 64, FBH = 48;
const Rgba kRed   = rgba(220, 40, 40);
const Rgba kClear = rgba(30, 30, 46);
const int  kStartX = 10, kY = 20, kFrames = 20;

struct Position { vec2 v; };
struct SpriteRef { TextureId tex; };

uint32_t g_red_pixels[8 * 8];

struct PlayerGame final : Game {
    ecs::Entity player = ecs::kInvalid;
    TextureId   tex    = kNoTexture;
    bool ok = false;
    int  checks = 0, fail = 0;

    void on_start(App& app) override {
        for (int i = 0; i < 64; ++i) g_red_pixels[i] = kRed;
        TextureDesc d{}; d.pixels = g_red_pixels; d.width = 8; d.height = 8;
        tex = app.render().load_texture(d);

        player = app.world().spawn();
        app.world().add<Position>(player, { vec2{ s_from_int(kStartX), s_from_int(kY) } });
        app.world().add<SpriteRef>(player, { tex });
    }

    void on_fixed_update(App& app, scalar dt) override {
        const InputState& in = app.input();
        const scalar speed = s_from_int(60);     // 60 px/s ; with dt=1/60 -> +1 px/frame
        app.world().each<Position>([&](ecs::Entity, Position& p) {
            if (in.down(Button::Right)) p.v.x += speed * dt;
            if (in.down(Button::Left))  p.v.x -= speed * dt;
        });
    }

    void on_render(App& app, scalar) override {
        Renderer& r = app.render();
        Camera2D cam{};
        r.begin_frame(cam);
        app.world().each<Position, SpriteRef>([&](ecs::Entity, Position& p, SpriteRef& s) {
            DrawSprite ds{}; ds.tex = s.tex; ds.sx = 0; ds.sy = 0; ds.sw = 8; ds.sh = 8;
            ds.pos = p.v; ds.layer = 1; ds.z = 0;
            r.draw_sprite(ds);
        });
        r.end_frame();
    }

    // Verify in on_stop — runs BEFORE the platform frees its framebuffer.
    void on_stop(App& app) override {
        phx_soft_fb fb = phx_gfx_soft_lock(app.platform()->gfx());
        auto px = [&](int x, int y) { return fb.pixels[y * fb.w + x]; };

        const int expectX = kStartX + kFrames;   // moved +1 px/frame for kFrames

        ++checks; if (px(expectX + 3, kY + 3) != kRed)  { ++fail; std::printf("    FAIL sprite not at moved pos (%d,%d)=%08X\n", expectX + 3, kY + 3, px(expectX + 3, kY + 3)); }
        ++checks; if (px(kStartX + 1, kY + 3) != kClear){ ++fail; std::printf("    FAIL sprite did not vacate start (%d,%d)=%08X\n", kStartX + 1, kY + 3, px(kStartX + 1, kY + 3)); }
        ++checks; if (px(58, 44) != kClear)             { ++fail; std::printf("    FAIL background not clear\n"); }

        // also confirm the ECS still holds exactly one entity at the expected position.
        // Tolerance ±1px: fixed-point (GBA tier) rounds 60*(1/60) slightly under 1.0 per
        // frame, so the fixed build lands on 29 where float lands on 30 — both correct
        // (this is the documented fixed/float divergence, docs/00 R2).
        Position* p = app.world().get<Position>(player);
        int gotx = p ? s_to_int(p->v.x) : -1;
        ++checks; if (!p || gotx < expectX - 1 || gotx > expectX) { ++fail; std::printf("    FAIL ecs pos x=%d want ~%d\n", gotx, expectX); }
        ++checks; if (app.world().count() != 1)          { ++fail; std::printf("    FAIL entity count=%u want 1\n", app.world().count()); }

        ok = (fail == 0);
        std::printf("    player moved x:%d -> %d over %d frames\n", kStartX, expectX, kFrames);
    }
};
} // namespace

int main() {
    phx_null_set_step_ns(1000000000ull / 60);   // 1 clock tick == 1 sim step
    phx_null_set_max_frames(kFrames);
    phx_null_set_buttons(button_bit(Button::Right));   // hold RIGHT the whole time

    Config cfg = Config::from_defaults();
    cfg.title = "playable"; cfg.sim_hz = 60;
    cfg.width = FBW; cfg.height = FBH;
    cfg.total_ram = 32u << 20; cfg.frame_scratch = 256u << 10; cfg.max_entities = 1024;

    App app(cfg);
    PlayerGame game;
    int rc = app.run(&game);

    std::printf("\nplayable_test: rc=%d  %d checks, %d failures\n", rc, game.checks, game.fail);
    std::printf((rc == 0 && game.ok) ? "PLAYABLE PASS\n\n" : "PLAYABLE FAIL\n\n");
    return (rc == 0 && game.ok) ? 0 : 1;
}
