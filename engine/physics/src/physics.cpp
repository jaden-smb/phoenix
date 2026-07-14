// phx/physics/physics.cpp — the AABB-vs-tilemap MVP physics (docs/10 §4).
//
// Algorithm per fixed step, per (Transform, Body) entity:
//   1. Integrate gravity into velocity (unless the body opts out).
//   2. Move on X, then resolve overlap with solid tiles on X only.
//   3. Move on Y, then resolve on Y only; a downward stop sets on_ground.
// Axis separation is what gives correct wall-slide / ground behaviour cheaply: each axis
// is resolved independently so sliding along a wall keeps the other axis' motion. At a
// 60 Hz fixed step, per-step displacement stays well under a tile, so a single
// move-then-resolve pass per axis is exact (no tunnelling).
#include "phx/physics/physics.h"
#include "phx/core/hot.h"

namespace phx {
namespace {

inline bool gt0(scalar v) { return v > s_from_int(0); }
inline bool lt0(scalar v) { return v < s_from_int(0); }

// Tile index containing a pixel coordinate (truncation; world coords are non-negative).
// The ARM7 has no divider, and this runs several times per body per axis per step — for the
// (universal) power-of-two tile size and non-negative coordinate case, use a shift instead
// of the soft-division call. Negative coords (a body poking past the map's left/top edge)
// keep the exact truncating division so behaviour is unchanged.
inline int tile_of(scalar coord, int size) {
    const int v = s_to_int(coord);
    if (v >= 0 && (size & (size - 1)) == 0)
        return v >> __builtin_ctz(unsigned(size));
    return v / size;
}

// Strict AABB overlap (touching edges do NOT count, so a body resting flush on a tile is
// not perpetually re-resolved).
inline bool strict_overlap(scalar al, scalar at, scalar ar, scalar ab,
                           scalar bl, scalar bt, scalar br, scalar bb) {
    return ar > bl && al < br && ab > bt && at < bb;
}

} // namespace

PHX_HOT_CODE void PhysicsWorld::resolve_x(Transform& t, Body& b, const AABBColl& c) const {
    if (!gt0(b.vel.x) && !lt0(b.vel.x)) return;     // no X motion → nothing to resolve
    const scalar top = t.pos.y - c.half.y, bot = t.pos.y + c.half.y;
    // Candidate tile span (±1 so out-of-bounds walls and shallow penetration are covered).
    const int tx0 = tile_of(t.pos.x - c.half.x, grid_.tile_w) - 1;
    const int tx1 = tile_of(t.pos.x + c.half.x, grid_.tile_w) + 1;
    const int ty0 = tile_of(top, grid_.tile_h) - 1;
    const int ty1 = tile_of(bot, grid_.tile_h) + 1;

    for (int ty = ty0; ty <= ty1; ++ty)
        for (int tx = tx0; tx <= tx1; ++tx) {
            if (!grid_.solid(tx, ty)) continue;
            const scalar tl = s_from_int(tx * grid_.tile_w), tr = s_from_int((tx + 1) * grid_.tile_w);
            const scalar tt = s_from_int(ty * grid_.tile_h), tb = s_from_int((ty + 1) * grid_.tile_h);
            const scalar left = t.pos.x - c.half.x, right = t.pos.x + c.half.x;
            if (!strict_overlap(left, top, right, bot, tl, tt, tr, tb)) continue;
            t.pos.x = gt0(b.vel.x) ? tl - c.half.x   // moving right → snap left of the tile
                                   : tr + c.half.x;  // moving left  → snap right of the tile
            b.vel.x = s_from_int(0);
            return;                                  // one blocking tile is enough per axis
        }
}

PHX_HOT_CODE void PhysicsWorld::resolve_y(Transform& t, Body& b, const AABBColl& c,
                                          scalar prev_bot) const {
    if (!gt0(b.vel.y) && !lt0(b.vel.y)) return;
    const scalar left = t.pos.x - c.half.x, right = t.pos.x + c.half.x;
    const int tx0 = tile_of(left, grid_.tile_w) - 1;
    const int tx1 = tile_of(right, grid_.tile_w) + 1;
    const int ty0 = tile_of(t.pos.y - c.half.y, grid_.tile_h) - 1;
    const int ty1 = tile_of(t.pos.y + c.half.y, grid_.tile_h) + 1;

    for (int ty = ty0; ty <= ty1; ++ty)
        for (int tx = tx0; tx <= tx1; ++tx) {
            const uint8_t f = grid_.flags_at(tx, ty);
            const bool is_solid = (f & kTileSolid) != 0;
            // A one-way platform blocks only a body moving DOWN whose bottom edge was at or
            // above the platform's top before this step (so jumping up through it, or walking
            // off a lower ledge into its tile, passes freely).
            const bool is_oneway = (f & kTileOneWay) != 0;
            if (!is_solid && !is_oneway) continue;
            const scalar tl = s_from_int(tx * grid_.tile_w), tr = s_from_int((tx + 1) * grid_.tile_w);
            const scalar tt = s_from_int(ty * grid_.tile_h), tb = s_from_int((ty + 1) * grid_.tile_h);
            if (!is_solid && !(gt0(b.vel.y) && !gt0(prev_bot - tt))) continue;
            const scalar top = t.pos.y - c.half.y, bot = t.pos.y + c.half.y;
            if (!strict_overlap(left, top, right, bot, tl, tt, tr, tb)) continue;
            if (gt0(b.vel.y)) { t.pos.y = tt - c.half.y; b.on_ground = true; }  // landed
            else              { t.pos.y = tb + c.half.y; }                      // bonked head
            b.vel.y = s_from_int(0);
            return;
        }
}

PHX_HOT_CODE uint32_t PhysicsWorld::step(ecs::World& w, scalar dt, Span<Hit> out_hits) {
    // 1+2+3: integrate and resolve every dynamic body against the tilemap.
    w.each<Body, Transform>([&](ecs::Entity e, Body& b, Transform& t) {
        if (b.flags & kBodyStatic) return;
        if (!(b.flags & kBodyNoGravity)) b.vel += gravity_ * dt;

        const AABBColl* c = w.get<AABBColl>(e);
        t.pos.x += b.vel.x * dt;
        if (c) resolve_x(t, b, *c);

        b.on_ground = false;
        const scalar prev_bot = c ? t.pos.y + c->half.y : scalar{};   // for one-way platforms
        t.pos.y += b.vel.y * dt;
        if (c) resolve_y(t, b, *c, prev_bot);
    });

    // 4: AABB-vs-AABB overlap pass (entity triggers). O(n²) over a bounded set — fine for
    // the MVP; docs/10 notes a uniform-grid broadphase as the expansion seam.
    constexpr uint32_t kMaxBodies = 256;
    ecs::Entity ents[kMaxBodies];
    aabb        boxes[kMaxBodies];
    uint16_t    layers[kMaxBodies], masks[kMaxBodies];
    uint32_t    n = 0;
    w.each<AABBColl, Transform>([&](ecs::Entity e, AABBColl& c, Transform& t) {
        if (n >= kMaxBodies) return;
        ents[n]   = e;
        boxes[n]  = aabb::from_center(t.pos, c.half);
        layers[n] = c.layer;
        masks[n]  = c.mask;
        ++n;
    });

    uint32_t hits = 0;
    for (uint32_t i = 0; i < n; ++i)
        for (uint32_t j = i + 1; j < n; ++j) {
            const bool match = (layers[i] & masks[j]) || (layers[j] & masks[i]);
            if (!match || !boxes[i].overlaps(boxes[j])) continue;
            if (hits < out_hits.size()) out_hits[hits] = Hit{ ents[i], ents[j], false };
            ++hits;
        }
    return hits;
}

uint8_t PhysicsWorld::tile_flags_in(const aabb& box) const {
    const int tx0 = tile_of(box.min.x, grid_.tile_w);
    const int tx1 = tile_of(box.max.x, grid_.tile_w);
    const int ty0 = tile_of(box.min.y, grid_.tile_h);
    const int ty1 = tile_of(box.max.y, grid_.tile_h);
    uint8_t f = 0;
    for (int ty = ty0; ty <= ty1; ++ty)
        for (int tx = tx0; tx <= tx1; ++tx) f |= grid_.flags_at(tx, ty);
    return f;
}

bool PhysicsWorld::overlap(ecs::World& w, const aabb& box, uint16_t mask,
                           ecs::Entity ignore) const {
    bool found = false;
    w.each<AABBColl, Transform>([&](ecs::Entity e, AABBColl& c, Transform& t) {
        if (found || e == ignore || !(c.layer & mask)) return;
        if (box.overlaps(aabb::from_center(t.pos, c.half))) found = true;
    });
    return found;
}

} // namespace phx
