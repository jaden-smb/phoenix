// tests/test_physics.cpp — AABB-vs-tilemap physics: gravity, ground stop, wall stop, head
// bonk, free-fall (no collider), and the entity overlap pass. Pure ECS (no platform/render),
// so it runs in the unit suite on both scalar tiers. Tolerances absorb fixed/float drift.
#include "phx_test.h"
#include "phx/physics/physics.h"
#include "phx/ecs/world.h"

using namespace phx;
using namespace phx::ecs;

namespace {

World* fresh_world(uint32_t cap = 256) {
    static uint8_t* pool = new uint8_t[8 << 20];
    static size_t   off  = 0;
    ArenaAllocator* a = new ArenaAllocator();
    a->init(pool + off, 1 << 20);
    off += (1 << 20);
    return World::create(*a, cap).unwrap();
}

// 8x6 tile grid, 8px tiles. Bottom row (ty=5) solid; one wall column at tx=6 (ty 0..4).
//   0 = air, 1 = solid.
const uint16_t kTiles[8 * 6] = {
    0,0,0,0,0,0,1,0,
    0,0,0,0,0,0,1,0,
    0,0,0,0,0,0,1,0,
    0,0,0,0,0,0,1,0,
    0,0,0,0,0,0,1,0,
    1,1,1,1,1,1,1,1,
};
TileGrid grid() {
    TileGrid g; g.tiles = kTiles; g.w = 8; g.h = 6; g.tile_w = 8; g.tile_h = 8; g.solid_from = 1;
    return g;
}

// Fixed step dt = 1/60 s, expressed in the active scalar tier.
#if PHX_CAPS_HAS_FLOAT_HW
const scalar kDt = 1.0f / 60.0f;
#else
const scalar kDt = fixed16::from_int(1) / fixed16::from_int(60);
#endif
} // namespace

PHX_TEST(physics_gravity_lands_on_ground) {
    World* w = fresh_world();
    PhysicsWorld phy; phy.set_tilemap(grid());
    phy.set_gravity(vec2{ s_from_int(0), s_from_int(600) });

    Entity e = w->spawn();
    w->add<Transform>(e, { vec2{ s_from_int(32), s_from_int(8) } });   // center, above floor
    w->add<Body>(e, {});
    w->add<AABBColl>(e, { vec2{ s_from_int(4), s_from_int(4) } });     // 8x8 box

    Hit hits[8];
    for (int i = 0; i < 120; ++i) phy.step(*w, kDt, Span<Hit>{ hits, 8 });

    Body* b = w->get<Body>(e);
    Transform* t = w->get<Transform>(e);
    CHECK(b->on_ground);
    // floor top is ty=5 -> y=40; box half=4 -> resting center y=36.
    CHECK_NEAR(s_to_double(t->pos.y), 36.0, 1.0);
    // did not tunnel through the floor (bottom edge never below the floor top)
    CHECK(s_to_int(t->pos.y) + 4 <= 41);
}

PHX_TEST(physics_wall_stops_horizontal) {
    World* w = fresh_world();
    PhysicsWorld phy; phy.set_tilemap(grid());
    phy.set_gravity(vec2{ s_from_int(0), s_from_int(0) });    // no gravity: isolate X

    Entity e = w->spawn();
    w->add<Transform>(e, { vec2{ s_from_int(20), s_from_int(20) } });
    w->add<Body>(e, { vec2{ s_from_int(200), s_from_int(0) } });   // drive right toward tx=6
    w->add<AABBColl>(e, { vec2{ s_from_int(4), s_from_int(4) } });

    Hit hits[8];
    for (int i = 0; i < 120; ++i) phy.step(*w, kDt, Span<Hit>{ hits, 8 });

    Transform* t = w->get<Transform>(e);
    Body* b = w->get<Body>(e);
    // wall column tx=6 -> left edge x=48; box half=4 -> resting center x=44.
    CHECK_NEAR(s_to_double(t->pos.x), 44.0, 1.0);
    CHECK(!(b->vel.x > s_from_int(0)));   // velocity killed on contact
}

PHX_TEST(physics_head_bonk_stops_upward) {
    World* w = fresh_world();
    PhysicsWorld phy; phy.set_tilemap(grid());
    phy.set_gravity(vec2{ s_from_int(0), s_from_int(0) });

    Entity e = w->spawn();
    // start just under the bottom-row ceiling-from-below is the floor; instead bonk the
    // closed-box top: start near y=8 moving up into the OOB ceiling (ty=-1 is solid).
    w->add<Transform>(e, { vec2{ s_from_int(20), s_from_int(8) } });
    w->add<Body>(e, { vec2{ s_from_int(0), s_from_int(-200) } });   // moving up
    w->add<AABBColl>(e, { vec2{ s_from_int(4), s_from_int(4) } });

    Hit hits[8];
    for (int i = 0; i < 120; ++i) phy.step(*w, kDt, Span<Hit>{ hits, 8 });

    Transform* t = w->get<Transform>(e);
    Body* b = w->get<Body>(e);
    // top of world (ty=0) starts at y=0; box half=4 -> resting center y=4.
    CHECK_NEAR(s_to_double(t->pos.y), 4.0, 1.0);
    CHECK(!(b->vel.y < s_from_int(0)));
}

PHX_TEST(physics_free_fall_without_collider) {
    World* w = fresh_world();
    PhysicsWorld phy; phy.set_tilemap(grid());
    phy.set_gravity(vec2{ s_from_int(0), s_from_int(600) });

    Entity e = w->spawn();
    w->add<Transform>(e, { vec2{ s_from_int(10), s_from_int(2) } });
    w->add<Body>(e, {});                                   // no AABBColl -> ignores tiles

    Hit hits[8];
    for (int i = 0; i < 30; ++i) phy.step(*w, kDt, Span<Hit>{ hits, 8 });

    Transform* t = w->get<Transform>(e);
    CHECK(!w->get<Body>(e)->on_ground);
    CHECK(s_to_double(t->pos.y) > 10.0);                  // fell straight through the floor
}

PHX_TEST(physics_overlap_pass_emits_hit) {
    World* w = fresh_world();
    PhysicsWorld phy; phy.set_tilemap(grid());
    phy.set_gravity(vec2{ s_from_int(0), s_from_int(0) });

    Entity a = w->spawn();
    w->add<Transform>(a, { vec2{ s_from_int(20), s_from_int(20) } });
    w->add<AABBColl>(a, { vec2{ s_from_int(4), s_from_int(4) }, 0x1, 0x2 });
    w->add<Body>(a, {});

    Entity b = w->spawn();
    w->add<Transform>(b, { vec2{ s_from_int(22), s_from_int(20) } });   // overlaps a
    w->add<AABBColl>(b, { vec2{ s_from_int(4), s_from_int(4) }, 0x2, 0x1 });
    w->add<Body>(b, {});

    Hit hits[8];
    uint32_t n = phy.step(*w, kDt, Span<Hit>{ hits, 8 });
    CHECK_EQ(n, 1u);

    // a third, far-away body produces no extra hit
    Entity c = w->spawn();
    w->add<Transform>(c, { vec2{ s_from_int(60), s_from_int(20) } });
    w->add<AABBColl>(c, { vec2{ s_from_int(4), s_from_int(4) }, 0x2, 0x1 });
    w->add<Body>(c, {});
    n = phy.step(*w, kDt, Span<Hit>{ hits, 8 });
    CHECK_EQ(n, 1u);
}

PHX_TEST(physics_overlap_query) {
    World* w = fresh_world();
    PhysicsWorld phy; phy.set_tilemap(grid());

    Entity a = w->spawn();
    w->add<Transform>(a, { vec2{ s_from_int(20), s_from_int(20) } });
    w->add<AABBColl>(a, { vec2{ s_from_int(4), s_from_int(4) }, 0x1, 0xFFFF });

    aabb q = aabb::from_center(vec2{ s_from_int(21), s_from_int(20) },
                              vec2{ s_from_int(4), s_from_int(4) });
    CHECK(phy.overlap(*w, q, 0x1));                 // matches layer 0x1
    CHECK(!phy.overlap(*w, q, 0x4));                // wrong mask
    CHECK(!phy.overlap(*w, q, 0x1, a));             // self ignored
}
