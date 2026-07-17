// tests/smoke_app.cpp — end-to-end headless smoke: boot the engine, run the fixed-step
// loop for a deterministic number of frames on the null platform, and assert the
// heartbeat is exactly as expected. This is the first time the whole boot path runs.
#include "phx/runtime/app.h"
#include "phx/core/log.h"

#include <cstdio>

// hooks exported by the null backend to make the loop deterministic
extern "C" void phx_null_set_step_ns(uint64_t ns);
extern "C" void phx_null_set_max_frames(uint64_t n);

using namespace phx;

namespace {
struct Tag {};

struct CountGame : Game {
    int  fixed_updates = 0;
    int  renders       = 0;
    bool started = false, stopped = false;
    bool alpha_in_range = true;

    // Proves world.defer()/flush_deferred() wiring (docs/04-ecs.md): App::run must apply a
    // deferred despawn recorded inside on_fixed_update before the SAME frame's on_render
    // runs, with no manual flush_deferred() call from the game.
    ecs::Entity keep{}, doomed{};
    bool deferred_recorded  = false;
    bool auto_flush_checked = false;
    bool auto_flush_ok      = false;

    void on_start(App& app) override {
        started = true;
        keep   = app.world().spawn();
        doomed = app.world().spawn();
        app.world().add<Tag>(keep);
        app.world().add<Tag>(doomed);
    }
    void on_fixed_update(App& app, scalar) override {
        ++fixed_updates;
        if (!deferred_recorded) {
            // Record via each() (the intended defer() use case) rather than a bare despawn.
            app.world().each<Tag>([&](ecs::Entity e, Tag&) {
                if (e == doomed) app.world().defer().despawn(e);
            });
            deferred_recorded = true;
        }
    }
    void on_render(App& app, scalar alpha) override {
        ++renders;
        double a = s_to_double(alpha);             // tier-agnostic (float or fixed16)
        if (!(a >= 0.0 && a < 1.0)) alpha_in_range = false;
        if (deferred_recorded && !auto_flush_checked) {
            auto_flush_checked = true;
            auto_flush_ok = !app.world().is_alive(doomed) && app.world().is_alive(keep);
        }
    }
    void on_stop(App&) override { stopped = true; }
};
} // namespace

int main() {
    // one clock tick == one sim step  => exactly one fixed update per frame, deterministically
    phx_null_set_step_ns(1000000000ull / 60);
    phx_null_set_max_frames(100);

    Config cfg   = Config::from_defaults();
    cfg.title    = "smoke";
    cfg.sim_hz   = 60;
    cfg.total_ram = 8u << 20;          // 8 MB is plenty for a heartbeat (override the PC default)
    cfg.frame_scratch = 256u << 10;    // 256 KB x2

    App app(cfg);
    CountGame game;
    int rc = app.run(&game);

    std::printf("\nsmoke results: rc=%d started=%d stopped=%d fixed=%d render=%d frames=%llu "
                "alpha_ok=%d auto_flush_ok=%d\n",
                rc, game.started, game.stopped, game.fixed_updates, game.renders,
                (unsigned long long)app.frame(), game.alpha_in_range, game.auto_flush_ok);

    bool ok = rc == 0
           && game.started && game.stopped
           && game.fixed_updates == 100
           && game.renders == 100
           && app.frame() == 100
           && game.alpha_in_range
           && game.auto_flush_checked
           && game.auto_flush_ok;

    // Regression soak: the loop's profiler clock reads must not leak time into the fixed-step
    // accumulator. Uncompensated, the 1 µs-per-read ticks added +5 µs/frame, which crossed a
    // whole extra step every ~3,334 frames — one phantom double-sim-step frame per minute (the
    // occasional GBA stutter; same virtual-clock convention). Run past that boundary and
    // require EXACTLY one fixed update per frame.
    phx_null_set_max_frames(4000);
    App soak_app(cfg);
    CountGame soak;
    int soak_rc = soak_app.run(&soak);
    std::printf("soak results: rc=%d fixed=%d frames=%llu\n",
                soak_rc, soak.fixed_updates, (unsigned long long)soak_app.frame());
    ok = ok && soak_rc == 0 && soak.fixed_updates == 4000 && soak_app.frame() == 4000;

    std::printf(ok ? "SMOKE PASS\n\n" : "SMOKE FAIL\n\n");
    return ok ? 0 : 1;
}
