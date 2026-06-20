// phx/scene/scene.cpp — scene stack mechanics + Blackboard (docs/10 §3).
//
// Structural ops are deferred and applied at the top of update()/render() so a scene may
// push/pop from inside its own callbacks. The "active run" is the contiguous span of scenes
// from the top down through any that are transparent (update_below / render_below), updated
// or rendered bottom-first so overlays composite correctly.
#include "phx/scene/scene.h"
#include "phx/core/assert.h"

namespace phx {

// ---- Blackboard ----------------------------------------------------------------------
void Blackboard::init(ArenaAllocator& a, uint32_t cap) {
    cap_  = cap;
    keys_ = a.alloc_array<NameHash>(cap);
    vals_ = a.alloc_array<int64_t>(cap);
    n_    = 0;
}
int32_t Blackboard::find(NameHash key) const {
    for (uint32_t i = 0; i < n_; ++i) if (keys_[i] == key) return int32_t(i);
    return -1;
}
void Blackboard::set_int(NameHash key, int64_t value) {
    int32_t i = find(key);
    if (i >= 0) { vals_[i] = value; return; }
    PHX_ASSERT_MSG(n_ < cap_, "Blackboard full");
    keys_[n_] = key; vals_[n_] = value; ++n_;
}
int64_t Blackboard::get_int(NameHash key, int64_t def) const {
    int32_t i = find(key);
    return i >= 0 ? vals_[i] : def;
}
bool Blackboard::has(NameHash key) const { return find(key) >= 0; }

// ---- SceneStack ----------------------------------------------------------------------
Result<SceneStack*> SceneStack::create(ArenaAllocator& persistent, StackAllocator& scene_scratch,
                                       uint32_t max_depth) {
    auto* s = persistent.make<SceneStack>();
    if (!s) return Result<SceneStack*>::fail(Status::OutOfMemory);
    s->cap_      = max_depth;
    s->stack_    = persistent.alloc_array<Scene*>(max_depth);
    s->marks_    = persistent.alloc_array<size_t>(max_depth);
    s->pend_cap_ = max_depth * 2 + 4;
    s->pending_  = persistent.alloc_array<SceneStack::Cmd>(s->pend_cap_);
    s->scratch_  = &scene_scratch;
    if (!s->stack_ || !s->marks_ || !s->pending_) return Result<SceneStack*>::fail(Status::OutOfMemory);
    s->board_.init(persistent);
    return Result<SceneStack*>::good(s);
}

void SceneStack::push(Scene* sc, Transition tr) {
    PHX_ASSERT_MSG(pend_n_ < pend_cap_, "scene command queue full");
    pending_[pend_n_++] = Cmd{ Op::Push, sc, tr };
}
void SceneStack::pop(Transition tr) {
    PHX_ASSERT_MSG(pend_n_ < pend_cap_, "scene command queue full");
    pending_[pend_n_++] = Cmd{ Op::Pop, nullptr, tr };
}
void SceneStack::replace(Scene* sc, Transition tr) {
    PHX_ASSERT_MSG(pend_n_ < pend_cap_, "scene command queue full");
    pending_[pend_n_++] = Cmd{ Op::Replace, sc, tr };
}

void SceneStack::do_push(EngineCtx* ctx, Scene* sc, Transition tr) {
    PHX_ASSERT_MSG(depth_ < cap_, "scene stack overflow");
    if (depth_ > 0) stack_[depth_ - 1]->on_pause(ctx);
    marks_[depth_] = scratch_->mark();      // scene-scoped arena begins here
    stack_[depth_++] = sc;
    last_tr_ = tr;
    sc->on_enter(ctx);
}
void SceneStack::do_pop(EngineCtx* ctx, Transition tr) {
    if (depth_ == 0) return;
    Scene* top = stack_[--depth_];
    top->on_exit(ctx);
    scratch_->reset_to(marks_[depth_]);     // O(1) free of everything the scene allocated
    last_tr_ = tr;
    if (depth_ > 0) stack_[depth_ - 1]->on_resume(ctx);
}

void SceneStack::apply_pending(EngineCtx* ctx) {
    // Drain in order. New commands enqueued during a callback wait for the next drain.
    uint32_t i = 0;
    while (i < pend_n_) {
        const Cmd c = pending_[i++];
        switch (c.op) {
            case Op::Push:    do_push(ctx, c.scene, c.tr); break;
            case Op::Pop:     do_pop(ctx, c.tr);           break;
            case Op::Replace: do_pop(ctx, c.tr); do_push(ctx, c.scene, c.tr); break;
        }
    }
    pend_n_ = 0;
}

// Lowest index of the active run: walk down while each scene lets the one below through.
namespace {
uint32_t run_floor(Scene** stack, uint32_t depth, bool render) {
    if (depth == 0) return 0;
    uint32_t f = depth - 1;
    while (f > 0 && (render ? stack[f]->render_below : stack[f]->update_below)) --f;
    return f;
}
} // namespace

void SceneStack::update(EngineCtx* ctx, scalar dt) {
    apply_pending(ctx);
    if (depth_ == 0) return;
    for (uint32_t i = run_floor(stack_, depth_, /*render*/false); i < depth_; ++i)
        stack_[i]->update(ctx, dt);
}

void SceneStack::render(EngineCtx* ctx, scalar alpha) {
    apply_pending(ctx);
    if (depth_ == 0) return;
    for (uint32_t i = run_floor(stack_, depth_, /*render*/true); i < depth_; ++i)
        stack_[i]->render(ctx, alpha);
}

Scene* SceneStack::top() { return depth_ ? stack_[depth_ - 1] : nullptr; }

} // namespace phx
