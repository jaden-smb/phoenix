// phx/scene/scene.h — a LIFO scene stack: push a pause/menu over gameplay, pop back, or
// replace one screen with another. Lifecycle callbacks (enter/exit/pause/resume) plus a
// persistent Blackboard that survives scene changes (score, save slot). See docs/10 §3.
//
// Decoupled by design: scenes receive an OPAQUE `EngineCtx*` that the composition root
// (runtime) defines and threads through — the scene module only forward-declares it, so
// scene stays at L3 and never depends on runtime/app (which would form a cycle). Concrete
// scenes (game code, above runtime) include the full context and use it.
//
// Each scene gets a scene-scoped arena slice: a StackAllocator mark is taken on enter and
// rolled back on exit, so leaving a scene frees ALL its allocations in O(1) — no leaks.
#ifndef PHX_SCENE_SCENE_H
#define PHX_SCENE_SCENE_H

#include "phx/core/types.h"
#include "phx/core/math.h"
#include "phx/memory/allocators.h"

namespace phx {

struct EngineCtx;   // opaque; defined by the runtime composition root (or a test)

enum class Transition : uint8_t { None, Fade, SlideLeft, SlideRight };

// A screen/state. Override what you need; defaults are no-ops. `update_below`/`render_below`
// let a scene be transparent to the one beneath it: a HUD overlay sets both (game keeps
// running and showing behind); a pause menu sets only render_below (game frozen but visible).
struct Scene {
    virtual ~Scene() = default;
    virtual void on_enter(EngineCtx*)            {}
    virtual void on_exit(EngineCtx*)             {}
    virtual void on_pause(EngineCtx*)            {}   // another scene was pushed on top
    virtual void on_resume(EngineCtx*)           {}   // the scene above us was popped
    virtual void update(EngineCtx*, scalar /*dt*/)    {}
    virtual void render(EngineCtx*, scalar /*alpha*/) {}

    bool update_below = false;   // cascade update to the scene beneath
    bool render_below = false;   // render the scene beneath first (overlay)
};

// Persistent key→value store that outlives scene changes. Minimal: integer-valued, keyed by
// FNV-1a NameHash (use "score"_hash). Linear scan over a small fixed table — cheap, no heap.
class Blackboard {
public:
    void init(ArenaAllocator& a, uint32_t cap = 32);
    void    set_int(NameHash key, int64_t value);
    int64_t get_int(NameHash key, int64_t def = 0) const;
    bool    has(NameHash key) const;
    uint32_t count() const { return n_; }
private:
    int32_t  find(NameHash key) const;
    NameHash* keys_ = nullptr;
    int64_t*  vals_ = nullptr;
    uint32_t  n_ = 0, cap_ = 0;
};

class SceneStack {
public:
    // `persistent` backs the stack arrays + Blackboard; `scene_scratch` is rolled back per
    // scene on exit (the scene-scoped arena).
    static Result<SceneStack*> create(ArenaAllocator& persistent,
                                      StackAllocator& scene_scratch,
                                      uint32_t max_depth = 8);

    // Structural ops are DEFERRED: recorded now, applied at the next update()/render() so a
    // scene can safely push/pop from inside its own update without corrupting the stack.
    void push(Scene*, Transition = Transition::None);
    void pop(Transition = Transition::None);
    void replace(Scene*, Transition = Transition::None);

    void update(EngineCtx*, scalar dt);      // applies pending ops, then updates the active run
    void render(EngineCtx*, scalar alpha);   // applies pending ops, then renders back-to-front

    Scene*      top();
    uint32_t    depth() const { return depth_; }
    bool        empty() const { return depth_ == 0; }
    Transition  last_transition() const { return last_tr_; }   // seam: drive a fade quad
    Blackboard& persistent() { return board_; }
    StackAllocator& scene_arena() { return *scratch_; }

private:
    SceneStack() = default;
    friend class ArenaAllocator;

    enum class Op : uint8_t { Push, Pop, Replace };
    struct Cmd { Op op; Scene* scene; Transition tr; };

    void apply_pending(EngineCtx*);
    void do_push(EngineCtx*, Scene*, Transition);
    void do_pop(EngineCtx*, Transition);

    Scene**  stack_  = nullptr;     // [0..depth_)
    size_t*  marks_  = nullptr;     // scene_scratch mark captured on each push
    uint32_t depth_  = 0;
    uint32_t cap_    = 0;

    Cmd*     pending_  = nullptr;
    uint32_t pend_n_   = 0;
    uint32_t pend_cap_ = 0;

    StackAllocator* scratch_ = nullptr;
    Blackboard      board_;
    Transition      last_tr_ = Transition::None;
};

} // namespace phx
#endif // PHX_SCENE_SCENE_H
