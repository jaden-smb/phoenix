// tests/test_physics.cpp — AABB-vs-tilemap physics: gravity, ground stop, wall stop, head
// bonk, free-fall (no collider), the entity overlap pass, and the per-tile collision flags
// mode (decorative / one-way / hazard tiles). Pure ECS (no platform/render), so it runs in
// the unit suite on both scalar tiers. Tolerances absorb fixed/float drift.
#include "phx_test.h"
#include "phx/physics/physics.h"
#include "phx/ecs/world.h"
#include "phx/resource/bundle.h"   // kTileFlag* — must mirror physics' TileFlags (asserted below)

#include <cstdlib>                 // std::abort (fixture pool bound check)

using namespace phx;
using namespace phx::ecs;

// The physics TileFlags values must equal the baked bundle constants — physics stays
// resource-free by design (no include), so this is the drift guard for the pair.
static_assert(uint8_t(phx::kTileSolid)  == uint8_t(phx::kTileFlagSolid),  "TileFlags drifted from bundle");
static_assert(uint8_t(phx::kTileOneWay) == uint8_t(phx::kTileFlagOneWay), "TileFlags drifted from bundle");
static_assert(uint8_t(phx::kTileHazard) == uint8_t(phx::kTileFlagHazard), "TileFlags drifted from bundle");

namespace {

World* fresh_world(uint32_t cap = 256) {
    constexpr size_t kPool = 16 << 20;
    static uint8_t* pool = new uint8_t[kPool];
    static size_t   off  = 0;
    if (off + (1 << 20) > kPool) std::abort();     // pool exhausted: grow kPool (tests/README)
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

// A flags-mode grid: tile index selects a TileFlags byte instead of the >= solid_from rule.
//   0 = air, 1 = solid, 2 = one-way platform, 3 = hazard (non-solid), 4 = decorative (no flags).
const uint16_t kFlagTiles[8 * 6] = {
    0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,
    0,0,2,2,2,2,0,0,      // one-way platform at ty=2 (top edge y=16)
    0,0,0,0,0,0,0,0,
    4,0,0,3,0,0,0,0,      // decorative at tx=0, hazard at tx=3 (ty=4)
    1,1,1,1,1,1,1,1,      // solid floor ty=5
};
const uint8_t kFlagTable[5] = { 0, kTileSolid, kTileOneWay, kTileHazard, 0 };
TileGrid flag_grid() {
    TileGrid g; g.tiles = kFlagTiles; g.w = 8; g.h = 6; g.tile_w = 8; g.tile_h = 8;
    g.flags = kFlagTable; g.flag_count = 5;
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

PHX_TEST(physics_flags_decorative_tile_not_solid) {
    // In flags mode a non-empty tile with no flags is scenery: the body falls straight
    // through it to the real floor (under solid_from it would have been a wall).
    World* w = fresh_world();
    PhysicsWorld phy; phy.set_tilemap(flag_grid());
    phy.set_gravity(vec2{ s_from_int(0), s_from_int(600) });

    Entity e = w->spawn();
    w->add<Transform>(e, { vec2{ s_from_int(4), s_from_int(8) } });   // above the decor tile (tx=0,ty=4)
    w->add<Body>(e, {});
    w->add<AABBColl>(e, { vec2{ s_from_int(4), s_from_int(4) } });

    Hit hits[8];
    for (int i = 0; i < 120; ++i) phy.step(*w, kDt, Span<Hit>{ hits, 8 });

    Transform* t = w->get<Transform>(e);
    CHECK(w->get<Body>(e)->on_ground);
    CHECK_NEAR(s_to_double(t->pos.y), 36.0, 1.0);          // rests on the floor, not the decor
}

PHX_TEST(physics_oneway_platform_lands_from_above) {
    World* w = fresh_world();
    PhysicsWorld phy; phy.set_tilemap(flag_grid());
    phy.set_gravity(vec2{ s_from_int(0), s_from_int(600) });

    Entity e = w->spawn();
    w->add<Transform>(e, { vec2{ s_from_int(32), s_from_int(4) } });  // above the platform (top y=16)
    w->add<Body>(e, {});
    w->add<AABBColl>(e, { vec2{ s_from_int(4), s_from_int(4) } });

    Hit hits[8];
    for (int i = 0; i < 120; ++i) phy.step(*w, kDt, Span<Hit>{ hits, 8 });

    Transform* t = w->get<Transform>(e);
    CHECK(w->get<Body>(e)->on_ground);
    CHECK_NEAR(s_to_double(t->pos.y), 12.0, 1.0);          // bottom edge resting at y=16
}

PHX_TEST(physics_oneway_platform_passes_from_below) {
    World* w = fresh_world();
    PhysicsWorld phy; phy.set_tilemap(flag_grid());
    phy.set_gravity(vec2{ s_from_int(0), s_from_int(0) });   // no gravity: isolate the ascent

    Entity e = w->spawn();
    w->add<Transform>(e, { vec2{ s_from_int(32), s_from_int(36) } });  // on the floor, under the platform
    w->add<Body>(e, { vec2{ s_from_int(0), s_from_int(-200) } });      // jump straight up
    w->add<AABBColl>(e, { vec2{ s_from_int(4), s_from_int(4) } });

    Hit hits[8];
    for (int i = 0; i < 120; ++i) phy.step(*w, kDt, Span<Hit>{ hits, 8 });

    Transform* t = w->get<Transform>(e);
    // Passed THROUGH the platform (no bonk at its underside y=24) up to the world ceiling.
    CHECK_NEAR(s_to_double(t->pos.y), 4.0, 1.0);

    // Now fall back down: the same platform catches the body from above.
    phy.set_gravity(vec2{ s_from_int(0), s_from_int(600) });
    for (int i = 0; i < 120; ++i) phy.step(*w, kDt, Span<Hit>{ hits, 8 });
    CHECK(w->get<Body>(e)->on_ground);
    CHECK_NEAR(s_to_double(t->pos.y), 12.0, 1.0);
}

PHX_TEST(physics_tile_flags_in_reports_hazard) {
    PhysicsWorld phy; phy.set_tilemap(flag_grid());

    // A box inside the hazard tile (tx=3, ty=4) reports it — and nothing else.
    aabb over_hazard = aabb::from_center(vec2{ s_from_int(28), s_from_int(36) },
                                         vec2{ s_from_int(2), s_from_int(2) });
    CHECK_EQ(unsigned(phy.tile_flags_in(over_hazard)), unsigned(kTileHazard));

    // A box in open air reports no flags.
    aabb in_air = aabb::from_center(vec2{ s_from_int(12), s_from_int(28) },
                                    vec2{ s_from_int(2), s_from_int(2) });
    CHECK_EQ(unsigned(phy.tile_flags_in(in_air)), 0u);
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
