// phx/physics/physics.h — minimal, tile-friendly 2D physics for the MVP. Axis-separated
// AABB-vs-tilemap collision (X then Y) plus an AABB-vs-AABB overlap pass for gameplay
// triggers (stomp, pickups). No solver, no rotation — exactly what a 2D platformer needs
// and what a GBA can afford. See docs/10-gameplay-systems.md §4.
//
// Decoupled by design: depends only on `core` + `ecs`. It meets gameplay through three
// plain-data ECS components (Transform/Body/AABBColl) and a TileGrid view the caller
// supplies (e.g. from a resource TilemapView) — physics never includes render/resource.
//
// Conventions: +Y points DOWN (screen space). Transform.pos is the AABB CENTER. World
// coordinates are assumed non-negative (tiles index from the origin); out-of-bounds tiles
// read as solid, so the tilemap behaves as a closed box.
#ifndef PHX_PHYSICS_PHYSICS_H
#define PHX_PHYSICS_PHYSICS_H

#include "phx/core/types.h"
#include "phx/core/math.h"
#include "phx/ecs/world.h"

namespace phx {

// --- ECS components (plain data) -------------------------------------------------------
struct Transform { vec2 pos; };                              // world position = AABB center
struct AABBColl  { vec2 half; uint16_t layer = 0xFFFF, mask = 0xFFFF; };
struct Body      { vec2 vel; bool on_ground = false; uint8_t flags = 0; };

enum BodyFlags : uint8_t {
    kBodyNone      = 0,
    kBodyNoGravity = 1u << 0,   // ignore world gravity (projectiles, UI props)
    kBodyStatic    = 1u << 1,   // never integrated or moved (one-way platforms, doors)
};

// Emitted by the entity-vs-entity overlap pass. Overlap-only (triggers); resolution is
// the game's choice. `tile` is reserved for a future tile-hit channel.
struct Hit { ecs::Entity a, b; bool tile = false; };

// --- Collision tile grid (caller-owned view) ------------------------------------------
// Solid iff tile index >= solid_from (so index 0 is empty/air by convention). Out-of-range
// tiles are solid, making the map a closed box.
struct TileGrid {
    const uint16_t* tiles  = nullptr;
    int             w      = 0;     // in tiles
    int             h      = 0;
    int             tile_w = 8;
    int             tile_h = 8;
    uint16_t        solid_from = 1;

    bool solid(int tx, int ty) const {
        if (!tiles || tx < 0 || ty < 0 || tx >= w || ty >= h) return true;
        return tiles[ty * w + tx] >= solid_from;
    }
};

// --- The physics step ------------------------------------------------------------------
// Stateless apart from the collision grid + gravity the caller configures. Holds no
// allocations; safe to keep one per scene on the stack or in a game struct.
class PhysicsWorld {
public:
    void set_tilemap(const TileGrid& g) { grid_ = g; }
    void set_gravity(vec2 g)            { gravity_ = g; }

    // Integrate + resolve every (Transform, Body) entity for one fixed step, then run the
    // overlap pass over (Transform, AABBColl) entities. Returns the number of Hits written
    // to out_hits (capped at out_hits.size()). Entities without an AABBColl are integrated
    // but skip tile collision.
    uint32_t step(ecs::World& w, scalar dt, Span<Hit> out_hits);

    // Point query: is `box` (world AABB) overlapping any (Transform, AABBColl) entity
    // whose layer matches `mask`? `ignore` excludes one entity (usually the querier).
    bool overlap(ecs::World& w, const aabb& box, uint16_t mask,
                 ecs::Entity ignore = ecs::kInvalid) const;

private:
    void resolve_x(Transform&, Body&, const AABBColl&) const;
    void resolve_y(Transform&, Body&, const AABBColl&) const;

    TileGrid grid_;
    vec2     gravity_{};
};

} // namespace phx
#endif // PHX_PHYSICS_PHYSICS_H
