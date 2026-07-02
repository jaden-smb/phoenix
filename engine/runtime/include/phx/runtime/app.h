// phx/runtime/app.h — the composition root: owns the fixed-timestep loop and subsystem
// lifetime. It lives ABOVE core (not inside it) so that `core` stays a zero-out-edge
// foundation — memory/platform depend on core's types, and the loop depends on
// memory+platform, so bundling the loop into core would form a module cycle. Keeping the
// App here makes the dependency graph strictly acyclic (verified by depcheck).
#ifndef PHX_RUNTIME_APP_H
#define PHX_RUNTIME_APP_H

#include "phx/core/config.h"
#include "phx/core/profile.h"
#include "phx/core/time.h"
#include "phx/memory/memory_root.h"
#include "phx/input/input.h"
#include "phx/ecs/world.h"
#include "phx/render/renderer.h"

struct phx_platform;

namespace phx {

class App;

// The game implements these hooks. No platform code ever appears in a Game.
struct Game {
    virtual ~Game() = default;
    virtual void on_start(App&)                {}
    virtual void on_fixed_update(App&, scalar /*dt*/) {}   // runs 0..N times per frame
    virtual void on_render(App&, scalar /*alpha*/)    {}   // once per frame, interpolated
    virtual void on_stop(App&)                 {}
};

class App {
public:
    explicit App(const Config& cfg) : cfg_(cfg) {}

    int  run(Game* game);          // boots subsystems, runs the loop, tears down; returns exit code
    void request_quit() { quit_ = true; }

    // accessors for Game hooks — the subsystems the App owns and threads into gameplay
    const Config&       config()   const { return cfg_; }
    MemoryRoot&         mem()             { return *mem_; }
    const phx_platform* platform() const  { return plat_; }
    const InputState&   input()    const  { return input_; }
    ecs::World&         world()           { return *world_; }
    Renderer&           render()          { return *render_; }
    uint64_t            frame()    const  { return frame_; }
    scalar              dt()       const  { return dt_; }
    // Last frame's phase timings (update/render/present/frame, µs), stamped by the loop from
    // the platform clock every frame. Feed to UI::profile_overlay or a custom HUD readout.
    const FrameProfile& profile()  const  { return prof_; }

private:
    Config               cfg_;
    MemoryRoot*          mem_    = nullptr;
    const phx_platform*  plat_   = nullptr;
    ecs::World*          world_  = nullptr;
    Renderer*            render_ = nullptr;
    InputState           input_  {};
    StepAccumulator      acc_;
    FrameProfile         prof_   {};
    scalar               dt_     = scalar{};
    uint64_t             frame_  = 0;
    bool                 quit_   = false;
};

} // namespace phx
#endif // PHX_RUNTIME_APP_H
