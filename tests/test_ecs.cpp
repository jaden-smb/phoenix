// tests/test_ecs.cpp — sparse-set World: lifecycle, components, iteration, determinism.
#include "phx_test.h"
#include "phx/ecs/world.h"
#include "phx/memory/memory_root.h"

using namespace phx;
using namespace phx::ecs;

namespace {
struct Position { int x, y; };
struct Velocity { int x, y; };
struct Tag {};

// Each test gets a fresh world carved from a small arena.
World* fresh_world(uint32_t cap = 256) {
    static uint8_t* pool = new uint8_t[8 << 20];   // 8 MB scratch for tests (host only)
    static size_t   off  = 0;
    ArenaAllocator* a = new ArenaAllocator();       // leaked intentionally (test lifetime)
    a->init(pool + off, 1 << 20);
    off += (1 << 20);
    return World::create(*a, cap).unwrap();
}
} // namespace

PHX_TEST(ecs_spawn_despawn) {
    World* w = fresh_world();
    Entity a = w->spawn();
    Entity b = w->spawn();
    CHECK(w->is_alive(a));
    CHECK(w->is_alive(b));
    CHECK_EQ(w->count(), 2u);
    CHECK(a != b);

    w->despawn(a);
    CHECK(!w->is_alive(a));
    CHECK(w->is_alive(b));
    CHECK_EQ(w->count(), 1u);
}

PHX_TEST(ecs_generation_guards_stale_handle) {
    World* w = fresh_world();
    Entity a = w->spawn();
    w->despawn(a);
    Entity reused = w->spawn();                      // likely recycles a's slot
    CHECK_EQ(index_of(reused), index_of(a));         // same index...
    CHECK(gen_of(reused) != gen_of(a));              // ...but a newer generation
    CHECK(!w->is_alive(a));                          // the OLD handle is rejected
    CHECK(w->is_alive(reused));
}

PHX_TEST(ecs_components_add_get_remove) {
    World* w = fresh_world();
    Entity e = w->spawn();
    w->add<Position>(e, {3, 4});
    CHECK(w->has<Position>(e));
    CHECK(!w->has<Velocity>(e));

    Position* p = w->get<Position>(e);
    CHECK(p != nullptr);
    CHECK_EQ(p->x, 3);
    CHECK_EQ(p->y, 4);

    w->remove<Position>(e);
    CHECK(!w->has<Position>(e));
    CHECK(w->get<Position>(e) == nullptr);
}

PHX_TEST(ecs_despawn_clears_components) {
    World* w = fresh_world();
    Entity e = w->spawn();
    w->add<Position>(e, {1, 1});
    w->add<Velocity>(e, {2, 2});
    w->despawn(e);
    // a recycled handle to the same slot must not see the old components
    Entity e2 = w->spawn();
    CHECK_EQ(index_of(e2), index_of(e));
    CHECK(!w->has<Position>(e2));
    CHECK(!w->has<Velocity>(e2));
}

PHX_TEST(ecs_each_iteration) {
    World* w = fresh_world();
    for (int i = 0; i < 10; ++i) {
        Entity e = w->spawn();
        w->add<Position>(e, {i, 0});
        if (i % 2 == 0) w->add<Velocity>(e, {1, 0});
    }
    int pos_count = 0, both_count = 0;
    w->each<Position>([&](Entity, Position&) { ++pos_count; });
    w->each<Position, Velocity>([&](Entity, Position&, Velocity&) { ++both_count; });
    CHECK_EQ(pos_count, 10);
    CHECK_EQ(both_count, 5);
}

PHX_TEST(ecs_each_mutation) {
    World* w = fresh_world();
    for (int i = 0; i < 5; ++i) { Entity e = w->spawn(); w->add<Position>(e, {i, i}); }
    w->each<Position>([&](Entity, Position& p) { p.x += 100; });
    int sum = 0;
    w->each<Position>([&](Entity, Position& p) { sum += p.x; });
    CHECK_EQ(sum, 100 * 5 + (0 + 1 + 2 + 3 + 4));
}

PHX_TEST(ecs_deferred_despawn_during_iteration) {
    World* w = fresh_world();
    for (int i = 0; i < 6; ++i) { Entity e = w->spawn(); w->add<Position>(e, {i, 0}); }
    // despawn even-x entities while iterating (deferred -> safe)
    w->each<Position>([&](Entity e, Position& p) {
        if (p.x % 2 == 0) w->defer().despawn(e);
    });
    w->flush_deferred();
    CHECK_EQ(w->count(), 3u);
    int remaining = 0;
    w->each<Position>([&](Entity, Position&) { ++remaining; });
    CHECK_EQ(remaining, 3);
}

PHX_TEST(ecs_churn_stable) {
    World* w = fresh_world(512);
    for (int iter = 0; iter < 20000; ++iter) {
        Entity e = w->spawn();
        w->add<Position>(e, {iter, iter});
        w->add<Tag>(e);
        w->despawn(e);
        CHECK_EQ(w->count(), 0u);                    // balanced; no leak
    }
    CHECK(true);
}
