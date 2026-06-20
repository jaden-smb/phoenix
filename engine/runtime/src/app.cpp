// phx/runtime/src/app.cpp — the fixed-timestep main loop (docs/00 §7). Decoupled from any
// platform: it talks only to the phx_platform C seam and the engine's own allocators.
#include "phx/runtime/app.h"
#include "phx/core/log.h"
#include "phx/platform/platform.h"

namespace phx {
namespace {
// Adapter so the engine's LogSink (LogLevel) can drive the platform's C log (phx_log_level)
// without an unsafe function-pointer cast.
const phx_platform* g_log_plat = nullptr;
void plat_log_adapter(LogLevel level, const char* msg) {
    if (g_log_plat && g_log_plat->log)
        g_log_plat->log(static_cast<phx_log_level>(level), msg);
}
} // namespace

int App::run(Game* game) {
    // 0. validate config (programmer error if bad — fail loud at boot, never mid-frame)
    if (Status st = cfg_.validate(); st != Status::Ok) {
        PHX_LOG_ERROR("invalid Config (status=%d)", int(st));
        return 1;
    }

    // 1. memory: the single boot allocation, carved into arenas + frame stacks
    auto mr = MemoryRoot::boot(cfg_.total_ram, cfg_.frame_scratch);
    if (!mr.ok()) { PHX_LOG_ERROR("MemoryRoot::boot failed (ram=%u)", cfg_.total_ram); return 1; }
    mem_ = mr.unwrap();

    // 2. platform: exactly one backend is linked; resolve and init it
    plat_ = phx_platform_get();
    phx_platform_desc desc{};
    desc.title           = cfg_.title;
    desc.width           = cfg_.width;
    desc.height          = cfg_.height;
    desc.vsync           = cfg_.vsync ? 1 : 0;
    desc.root_arena      = nullptr;
    desc.root_arena_size = 0;
    if (plat_->init(&desc) != 0) { PHX_LOG_ERROR("platform init failed (%s)", cfg_.title); return 1; }
    g_log_plat = plat_;
    log_set_sink(plat_->log ? plat_log_adapter : nullptr);
    log_set_level(cfg_.log_level);

    // 3. ECS world (sized from config/caps) — carved from the persistent arena
    uint32_t max_ents = cfg_.max_entities ? cfg_.max_entities : caps().max_entities;
    auto wr = ecs::World::create(mem_->persistent(), max_ents);
    if (!wr.ok()) { PHX_LOG_ERROR("World::create failed (max=%u)", max_ents); return 1; }
    world_ = wr.unwrap();

    // 4. renderer (software/GL/GU/PPU per tier) — needs the platform gfx device
    if (phx_gfx* gfx = plat_->gfx()) {
        auto rr = Renderer::create(gfx, mem_->persistent(), caps());
        if (!rr.ok()) { PHX_LOG_ERROR("Renderer::create failed (tier=%d)", int(caps().render_tier)); return 1; }
        render_ = rr.unwrap();
    } else {
        PHX_LOG_WARN("no gfx device for '%s'; running without a renderer", cfg_.title);
    }

    // 5. timing
    acc_.configure(cfg_.sim_hz);
    dt_ = fixed_dt(cfg_.sim_hz);

    PHX_LOG_INFO("Phoenix boot: '%s'  ram=%uKB  sim=%uHz  ents=%u", cfg_.title,
                 cfg_.total_ram / 1024u, cfg_.sim_hz, max_ents);

    // 6. game start hook
    game->on_start(*this);

    // 7. the loop: fixed-step sim, interpolated render, O(1) transient reclaim
    phx_input_raw raw{};
    uint64_t prev = plat_->clock_ns();
    while (!quit_) {
        if (plat_->pump_events() == 0) { quit_ = true; break; }

        uint64_t now = plat_->clock_ns();
        int steps = acc_.advance(now - prev);
        prev = now;

        plat_->poll_input(&raw);
        input_.update(raw);        // raw -> semantic edges, once per frame

        for (int i = 0; i < steps; ++i)
            game->on_fixed_update(*this, dt_);

        game->on_render(*this, acc_.alpha());

        mem_->swap_frame();        // double-buffered transient reclaim, O(1)
        ++frame_;

        plat_->present();          // swap / vblank (no-op headless)
    }

    // 6. teardown in reverse
    game->on_stop(*this);
    plat_->shutdown();
    MemoryRoot::shutdown(mem_);
    PHX_LOG_INFO("Phoenix shutdown after %llu frames", (unsigned long long)frame_);
    return 0;
}

} // namespace phx
