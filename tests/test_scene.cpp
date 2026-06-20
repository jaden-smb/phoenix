// tests/test_scene.cpp — scene stack: push/pop lifecycle order, active-run routing (update
// vs render, transparency), replace, deferred ops from inside a scene, the persistent
// Blackboard, and O(1) scene-arena rollback on exit. Pure core+memory; runs on both tiers.
#include "phx_test.h"
#include "phx/scene/scene.h"
#include "phx/memory/allocators.h"

#include <cstring>

using namespace phx;

// The runtime defines EngineCtx; here the test supplies a minimal one (scenes only need the
// stack, to push from inside a callback).
namespace phx { struct EngineCtx { SceneStack* stack = nullptr; }; }

namespace {

char g_log[512]; int g_len = 0;
void reset_log() { g_len = 0; g_log[0] = 0; }
void put(char a, char b) { g_log[g_len++] = a; g_log[g_len++] = b; g_log[g_len] = 0; }

const scalar kDt = s_from_int(0);

// Records every callback as "<id><code>": +enter -exit p pause r resume u update R render.
struct TScene : Scene {
    char id;
    explicit TScene(char c) : id(c) {}
    void on_enter(EngineCtx*) override        { put(id, '+'); }
    void on_exit(EngineCtx*) override         { put(id, '-'); }
    void on_pause(EngineCtx*) override        { put(id, 'p'); }
    void on_resume(EngineCtx*) override       { put(id, 'r'); }
    void update(EngineCtx*, scalar) override  { put(id, 'u'); }
    void render(EngineCtx*, scalar) override  { put(id, 'R'); }
};

// Allocates from the scene-scoped arena on enter (to prove O(1) rollback on exit).
struct AllocScene : Scene {
    void on_enter(EngineCtx* c) override { c->stack->scene_arena().alloc(4096); }
};

// Pushes a child the first time it updates (tests deferred ops from inside a callback).
struct Pusher : Scene {
    Scene* child = nullptr; bool done = false;
    void on_enter(EngineCtx*) override       { put('P', '+'); }
    void on_pause(EngineCtx*) override       { put('P', 'p'); }
    void update(EngineCtx* c, scalar) override {
        put('P', 'u');
        if (!done) { c->stack->push(child); done = true; }
    }
};

// Fresh stack with its own persistent arena + scene scratch StackAllocator.
SceneStack* fresh_stack(StackAllocator** scratch_out) {
    static uint8_t* pool = new uint8_t[8 << 20];
    static size_t   off  = 0;
    ArenaAllocator* a = new ArenaAllocator(); a->init(pool + off, 1 << 20); off += (1 << 20);
    StackAllocator* sc = new StackAllocator(); sc->init(pool + off, 1 << 20); off += (1 << 20);
    *scratch_out = sc;
    return SceneStack::create(*a, *sc).unwrap();
}

} // namespace

PHX_TEST(scene_push_pop_lifecycle_order) {
    StackAllocator* sc; SceneStack* st = fresh_stack(&sc);
    EngineCtx ctx{ st };
    TScene A('A'), B('B');

    reset_log();
    st->push(&A); st->push(&B); st->update(&ctx, kDt);   // both pushes apply, then top updates
    CHECK(std::strcmp(g_log, "A+ApB+Bu") == 0);
    CHECK_EQ(st->depth(), 2u);
    CHECK(st->top() == &B);

    reset_log();
    st->pop(); st->update(&ctx, kDt);
    CHECK(std::strcmp(g_log, "B-ArAu") == 0);
    CHECK_EQ(st->depth(), 1u);
    CHECK(st->top() == &A);

    reset_log();
    st->pop(); st->update(&ctx, kDt);
    CHECK(std::strcmp(g_log, "A-") == 0);
    CHECK(st->empty());
}

PHX_TEST(scene_update_routes_to_top_unless_transparent) {
    StackAllocator* sc; SceneStack* st = fresh_stack(&sc);
    EngineCtx ctx{ st };
    TScene A('A'), B('B');
    B.update_below = true;                       // let the scene beneath also update

    reset_log();
    st->push(&A); st->push(&B); st->update(&ctx, kDt);
    CHECK(std::strcmp(g_log, "A+ApB+AuBu") == 0);   // both update, bottom-first
}

PHX_TEST(scene_render_back_to_front_overlay) {
    StackAllocator* sc; SceneStack* st = fresh_stack(&sc);
    EngineCtx ctx{ st };
    TScene A('A'), B('B');

    st->push(&A); st->push(&B);
    reset_log();
    st->render(&ctx, s_from_int(1));            // opaque top -> only B renders
    CHECK(std::strcmp(g_log, "A+ApB+BR") == 0); // (pending applied here, then render)

    B.render_below = true;
    reset_log();
    st->render(&ctx, s_from_int(1));            // overlay -> A then B
    CHECK(std::strcmp(g_log, "ARBR") == 0);
}

PHX_TEST(scene_replace_swaps_top) {
    StackAllocator* sc; SceneStack* st = fresh_stack(&sc);
    EngineCtx ctx{ st };
    TScene A('A'), B('B');

    st->push(&A); st->update(&ctx, kDt);
    reset_log();
    st->replace(&B); st->update(&ctx, kDt);
    CHECK(std::strcmp(g_log, "A-B+Bu") == 0);   // old exits, new enters, depth unchanged
    CHECK_EQ(st->depth(), 1u);
    CHECK(st->top() == &B);
}

PHX_TEST(scene_deferred_push_from_update) {
    StackAllocator* sc; SceneStack* st = fresh_stack(&sc);
    EngineCtx ctx{ st };
    Pusher P; TScene C('C'); P.child = &C;

    reset_log();
    st->push(&P); st->update(&ctx, kDt);        // P enters + updates, enqueues push(C)
    CHECK(std::strcmp(g_log, "P+Pu") == 0);
    CHECK_EQ(st->depth(), 1u);

    reset_log();
    st->update(&ctx, kDt);                       // deferred push(C) applies, then C updates
    CHECK(std::strcmp(g_log, "PpC+Cu") == 0);
    CHECK_EQ(st->depth(), 2u);
    CHECK(st->top() == &C);
}

PHX_TEST(scene_blackboard_persists_across_changes) {
    StackAllocator* sc; SceneStack* st = fresh_stack(&sc);
    EngineCtx ctx{ st };
    TScene A('A'), B('B');

    st->persistent().set_int("score"_hash, 42);
    st->push(&A); st->update(&ctx, kDt);
    st->replace(&B); st->update(&ctx, kDt);     // scene changed...
    st->pop(); st->update(&ctx, kDt);

    CHECK(st->persistent().has("score"_hash));
    CHECK_EQ(st->persistent().get_int("score"_hash), int64_t(42));   // ...score survived
    CHECK_EQ(st->persistent().get_int("missing"_hash, -1), int64_t(-1));

    st->persistent().set_int("score"_hash, 100);                     // update in place
    CHECK_EQ(st->persistent().get_int("score"_hash), int64_t(100));
}

PHX_TEST(scene_arena_rolled_back_on_exit) {
    StackAllocator* sc; SceneStack* st = fresh_stack(&sc);
    EngineCtx ctx{ st };
    AllocScene a;

    size_t before = sc->used();
    st->push(&a); st->update(&ctx, kDt);        // on_enter allocates 4 KB from scene arena
    CHECK(sc->used() > before);
    st->pop(); st->update(&ctx, kDt);           // on_exit -> reset_to(mark)
    CHECK_EQ(sc->used(), before);               // O(1) free, no leak
}
