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
struct CountGame : Game {
    int  fixed_updates = 0;
    int  renders       = 0;
    bool started = false, stopped = false;
    bool alpha_in_range = true;

    void on_start(App&) override { started = true; }
    void on_fixed_update(App&, scalar) override { ++fixed_updates; }
    void on_render(App&, scalar alpha) override {
        ++renders;
        double a = s_to_double(alpha);             // tier-agnostic (float or fixed16)
        if (!(a >= 0.0 && a < 1.0)) alpha_in_range = false;
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

    std::printf("\nsmoke results: rc=%d started=%d stopped=%d fixed=%d render=%d frames=%llu alpha_ok=%d\n",
                rc, game.started, game.stopped, game.fixed_updates, game.renders,
                (unsigned long long)app.frame(), game.alpha_in_range);

    bool ok = rc == 0
           && game.started && game.stopped
           && game.fixed_updates == 100
           && game.renders == 100
           && app.frame() == 100
           && game.alpha_in_range;

    std::printf(ok ? "SMOKE PASS\n\n" : "SMOKE FAIL\n\n");
    return ok ? 0 : 1;
}
