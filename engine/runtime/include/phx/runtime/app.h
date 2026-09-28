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
#include "phx/runtime/audio.h"

struct phx_platform;

namespace phx {

class App;

// The game implements these hooks. No platform code ever appears in a Game.
struct Game {
    virtual ~Game() = default;
    // Before boot, when the engine starts the game (phx/runtime/main.h): set the title, the
    // logical resolution, sim_hz. `cfg` arrives holding the target's budgets; leave them unless
    // the game needs more (read phx::caps() to size by tier). Nothing is running yet: no App.
    virtual void on_configure(Config&)         {}
    virtual void on_start(App&)                {}
    virtual void on_fixed_update(App&, scalar /*dt*/) {}   // runs 0..N times per frame
    virtual void on_render(App&, scalar /*alpha*/)    {}   // once per frame, interpolated
    virtual void on_stop(App&)                 {}
};

// Developer tools a desktop entry may install (phx/runtime/devtools.h). Both hooks are optional
// and never set on a console build.
struct DevHooks {
    void* user = nullptr;
    // Before the frame's fixed steps: returns how many to run (0 = paused, 1 = a single step).
    int  (*frame)(void* user, App& app, int steps) = nullptr;
    // After on_render (the frame is drawn), before present: draw over it.
    void (*overlay)(void* user, App& app) = nullptr;
    // At teardown, before Game::on_stop: close what the tools keep open (a trace file).
    void (*stop)(void* user, App& app) = nullptr;
};

// The run's high-water marks, taken at the end of every frame: what a budget report compares
// with the target's limits (phx/runtime/budget.h). A few compares per frame, always on.
struct RuntimePeaks {
    uint32_t entities        = 0;   // live entities
    uint32_t sprites         = 0;   // sprites submitted in one frame
    uint32_t sprites_dropped = 0;   // over the whole run: sprites past caps().max_sprites
    uint32_t tiles           = 0;   // tiles drawn in one frame
    uint32_t batches         = 0;
    uint32_t frame_scratch   = 0;   // bytes of the frame stack still in use when a frame ends
    uint64_t arena_used      = 0;   // the persistent arena's use (it only grows) ...
    uint64_t arena_capacity  = 0;   // ... and its size: kept here, so they outlive run()'s teardown
};

class App {
public:
    explicit App(const Config& cfg) : cfg_(cfg) {}
    void set_dev_hooks(const DevHooks& h) { dev_ = h; }

    int  run(Game* game);          // boots subsystems, runs the loop, tears down; returns exit code
    void request_quit() { quit_ = true; }

    // accessors for Game hooks — the subsystems the App owns and threads into gameplay
    const Config&       config()   const { return cfg_; }
    MemoryRoot&         mem()             { return *mem_; }
    const phx_platform* platform() const  { return plat_; }
    const InputState&   input()    const  { return input_; }
    // The active control remap (identity by default). Mutable so a game's options scene can
    // rebind buttons / tune the stick deadzone; takes effect on the next frame's update.
    InputMap&           input_map()       { return input_.map; }
    ecs::World&         world()           { return *world_; }
    Renderer&           render()          { return *render_; }
    // Sound output (phx/runtime/audio.h): play/stop intents; the engine owns mixer + device.
    GameAudio&          audio()           { return audio_; }
    uint64_t            frame()    const  { return frame_; }
    scalar              dt()       const  { return dt_; }
    // Last frame's phase timings (update/render/present/frame, µs), stamped by the loop from
    // the platform clock every frame. Feed to UI::profile_overlay or a custom HUD readout.
    const FrameProfile& profile()  const  { return prof_; }
    const RuntimePeaks& peaks()    const  { return peaks_; }

private:
    Config               cfg_;
    MemoryRoot*          mem_    = nullptr;
    const phx_platform*  plat_   = nullptr;
    ecs::World*          world_  = nullptr;
    Renderer*            render_ = nullptr;
    GameAudio            audio_;
    DevHooks             dev_    {};
    RuntimePeaks         peaks_  {};
    InputState           input_  {};
    StepAccumulator      acc_;
    FrameProfile         prof_   {};
    scalar               dt_     = scalar{};
    uint64_t             frame_  = 0;
    bool                 quit_   = false;
};

} // namespace phx
#endif // PHX_RUNTIME_APP_H
