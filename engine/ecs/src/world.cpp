// phx/ecs/src/world.cpp — non-template World internals: entity slot lifecycle and the
// deferred-despawn flush. The component storage (templates) lives in world.h.
#include "phx/ecs/world.h"

namespace phx::ecs {

Result<World*> World::create(ArenaAllocator& a, uint32_t max_entities) {
    World* w = a.make<World>();
    if (!w) return Result<World*>::fail(Status::OutOfMemory);

    w->arena_       = &a;
    w->max_         = max_entities;
    w->generation_  = a.alloc_array<uint8_t>(max_entities);
    w->live_        = a.alloc_array<uint8_t>(max_entities);
    w->free_slots_  = a.alloc_array<uint32_t>(max_entities);
    w->pending_cap_ = max_entities;
    w->pending_     = a.alloc_array<Entity>(max_entities);

    if (!w->generation_ || !w->live_ || !w->free_slots_ || !w->pending_)
        return Result<World*>::fail(Status::OutOfMemory);

    for (uint32_t i = 0; i < max_entities; ++i) { w->generation_[i] = 0; w->live_[i] = 0; }
    return Result<World*>::good(w);
}

Entity World::spawn() {
    uint32_t idx;
    if (free_top_ > 0) {
        idx = free_slots_[--free_top_];                 // recycle a freed slot
    } else {
        PHX_ASSERT_MSG(high_water_ < max_, "entity pool exhausted");
        idx = high_water_++;                            // fresh slot
    }
    live_[idx] = 1;
    ++alive_count_;
    return make_entity(idx, generation_[idx]);
}

bool World::is_alive(Entity e) const {
    uint32_t idx = index_of(e);
    return idx < max_ && live_[idx] && generation_[idx] == gen_of(e);
}

void World::remove_from_all(Entity e) {
    for (uint16_t k = 0; k < used_count_; ++k) {
        ErasedSet& s = sets_[used_[k]];
        s.remove(s.set, e);                             // no-op if absent
    }
}

void World::despawn(Entity e) {
    if (!is_alive(e)) return;
    uint32_t idx = index_of(e);
    remove_from_all(e);
    live_[idx] = 0;
    ++generation_[idx];                                 // invalidate stale handles
    free_slots_[free_top_++] = idx;
    --alive_count_;
}

void World::queue_despawn(Entity e) {
    if (pending_top_ < pending_cap_) pending_[pending_top_++] = e;
}

void World::flush_deferred() {
    for (uint32_t i = 0; i < pending_top_; ++i) despawn(pending_[i]);
    pending_top_ = 0;
}

} // namespace phx::ecs
