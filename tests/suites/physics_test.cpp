// tests/physics_test.cpp — a HEADLESS gravity + ground-collision slice. Boots the full App,
// spawns a body above a tile floor, runs the real fixed-step loop while PhysicsWorld
// integrates + resolves against the tilemap, renders the resting sprite, then verifies via
// BOTH the framebuffer and the ECS that it fell and landed flush on the floor (no tunnel).
#include "phx/runtime/app.h"
#include "phx/physics/physics.h"
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
constexpr int FBW = 64, FBH = 48, FRAMES = 120;
const Rgba kRed   = rgba(220, 40, 40);
const Rgba kClear = rgba(30, 30, 46);

// 8x6 tiles @ 8px. Bottom row solid (the floor); everything else air.
const uint16_t kTiles[8 * 6] = {
    0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,
    1,1,1,1,1,1,1,1,
};

uint32_t g_red[8 * 8];

struct PhysicsGame final : Game {
    PhysicsWorld phy;
    ecs::Entity  body = ecs::kInvalid;
    TextureId    tex  = kNoTexture;
    bool ok = false; int checks = 0, fail = 0;

    void on_start(App& app) override {
        for (int i = 0; i < 64; ++i) g_red[i] = kRed;
        TextureDesc d{}; d.pixels = g_red; d.width = 8; d.height = 8;
        tex = app.render().load_texture(d);

        TileGrid g; g.tiles = kTiles; g.w = 8; g.h = 6; g.tile_w = 8; g.tile_h = 8; g.solid_from = 1;
        phy.set_tilemap(g);
        phy.set_gravity(vec2{ s_from_int(0), s_from_int(600) });

        body = app.world().spawn();
        app.world().add<Transform>(body, { vec2{ s_from_int(32), s_from_int(8) } });  // center
        app.world().add<Body>(body, {});
        app.world().add<AABBColl>(body, { vec2{ s_from_int(4), s_from_int(4) } });     // 8x8
    }

    void on_fixed_update(App& app, scalar dt) override {
        Hit hits[8];
        phy.step(app.world(), dt, Span<Hit>{ hits, 8 });
    }

    void on_render(App& app, scalar) override {
        Renderer& r = app.render();
        Camera2D cam{};
        r.begin_frame(cam);
        app.world().each<Transform, AABBColl>([&](ecs::Entity, Transform& t, AABBColl& c) {
            DrawSprite ds{}; ds.tex = tex; ds.sx = 0; ds.sy = 0; ds.sw = 8; ds.sh = 8;
            ds.pos = t.pos - c.half;     // center -> top-left for the renderer
            ds.layer = 1; ds.z = 0;
            r.draw_sprite(ds);
        });
        r.end_frame();
    }

    void on_stop(App& app) override {
        phx_soft_fb fb = phx_gfx_soft_lock(app.platform()->gfx());
        auto px = [&](int x, int y) { return fb.pixels[y * fb.w + x]; };

        Body* b = app.world().get<Body>(body);
        Transform* t = app.world().get<Transform>(body);
        int resty = b && t ? s_to_int(t->pos.y) : -1;   // expected ~36 (floor top 40 - half 4)

        ++checks; if (!b || !b->on_ground)              { ++fail; std::printf("    FAIL not on_ground\n"); }
        ++checks; if (resty < 35 || resty > 37)         { ++fail; std::printf("    FAIL rest y=%d want ~36\n", resty); }
        ++checks; if (resty + 4 > 41)                   { ++fail; std::printf("    FAIL tunneled (bottom=%d past floor 40)\n", resty + 4); }
        // sprite is at its resting center; the start row is now vacated; below stays clear.
        ++checks; if (px(32, 36) != kRed)               { ++fail; std::printf("    FAIL sprite not at rest (32,36)=%08X\n", px(32, 36)); }
        ++checks; if (px(32, 8)  != kClear)             { ++fail; std::printf("    FAIL sprite did not leave start (32,8)\n"); }
        ++checks; if (px(32, 45) != kClear)             { ++fail; std::printf("    FAIL pixel below floor not clear\n"); }

        ok = (fail == 0);
        std::printf("    body fell y:8 -> %d, on_ground=%d\n", resty, b ? b->on_ground : 0);
    }
};
} // namespace

int main() {
    phx_null_set_step_ns(1000000000ull / 60);
    phx_null_set_max_frames(FRAMES);
    phx_null_set_buttons(0);

    Config cfg = Config::from_defaults();
    cfg.title = "physics"; cfg.sim_hz = 60;
    cfg.width = FBW; cfg.height = FBH;
    cfg.total_ram = 32u << 20; cfg.frame_scratch = 256u << 10; cfg.max_entities = 1024;

    App app(cfg);
    PhysicsGame game;
    int rc = app.run(&game);

    std::printf("\nphysics_test: rc=%d  %d checks, %d failures\n", rc, game.checks, game.fail);
    std::printf((rc == 0 && game.ok) ? "PHYSICS PASS\n\n" : "PHYSICS FAIL\n\n");
    return (rc == 0 && game.ok) ? 0 : 1;
}
