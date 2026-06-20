// phx/ecs/world.h — sparse-set ECS. Predictable, bounded, zero per-entity heap churn.
// The SAME World scales from 512 entities in 256 KB (GBA) to 65536 on PC.
// See docs/04-ecs.md for the design rationale (sparse-set vs archetype).
#ifndef PHX_ECS_WORLD_H
#define PHX_ECS_WORLD_H

#include "phx/core/types.h"
#include "phx/core/math.h"        // scalar (for System::tick)
#include "phx/memory/allocators.h"

namespace phx::ecs {

// Entity = index:24 | generation:8. Generation guards against stale handles:
// despawn bumps the slot's generation so a dangling Entity fails is_alive().
using Entity = uint32_t;
constexpr Entity   kInvalid    = 0xFFFFFFFFu;
constexpr uint32_t kTombstone  = 0xFFFFFFFFu;
constexpr uint32_t kMaxComponentTypes = 64;     // ceiling on distinct component types

inline constexpr uint32_t index_of(Entity e) { return e & 0x00FFFFFFu; }
inline constexpr uint8_t  gen_of(Entity e)   { return uint8_t(e >> 24); }
inline constexpr Entity   make_entity(uint32_t idx, uint8_t gen) {
    return (uint32_t(gen) << 24) | (idx & 0x00FFFFFFu);
}

// ---------------------------------------------------------------------------
// SparseSet<C> — dense, cache-friendly storage for one component type.
// sparse[entity index] -> dense index ; dense[] packed entities ; comps[] packed data.
// ---------------------------------------------------------------------------
template <class C>
class SparseSet {
    uint32_t* sparse_ = nullptr;   // size = max_entities
    Entity*   dense_  = nullptr;   // packed
    C*        comps_  = nullptr;   // packed, parallel to dense_
    uint32_t  size_   = 0;
    uint32_t  max_    = 0;
public:
    void init(ArenaAllocator& a, uint32_t max) {       // one-shot, boot-time
        max_    = max;
        sparse_ = a.alloc_array<uint32_t>(max);
        dense_  = a.alloc_array<Entity>(max);
        comps_  = a.alloc_array<C>(max);
        for (uint32_t i = 0; i < max; ++i) sparse_[i] = kTombstone;
    }
    C& add(Entity e, const C& v) {
        uint32_t i = index_of(e);
        if (sparse_[i] != kTombstone) return comps_[sparse_[i]];   // idempotent add
        sparse_[i]    = size_;
        dense_[size_] = e;
        comps_[size_] = v;
        return comps_[size_++];
    }
    void remove(Entity e) {                            // swap-remove, O(1)
        uint32_t i = index_of(e);
        if (i >= max_ || sparse_[i] == kTombstone) return;
        uint32_t d    = sparse_[i];
        uint32_t last = --size_;
        dense_[d]  = dense_[last];
        comps_[d]  = comps_[last];
        sparse_[index_of(dense_[d])] = d;
        sparse_[i] = kTombstone;
    }
    bool has(Entity e) const { return index_of(e) < max_ && sparse_[index_of(e)] != kTombstone; }
    C&   get(Entity e)       { return comps_[sparse_[index_of(e)]]; }
    C*   try_get(Entity e)   { return has(e) ? &comps_[sparse_[index_of(e)]] : nullptr; }

    C*       data()  { return comps_; }
    Entity*  ents()  { return dense_; }
    uint32_t size() const { return size_; }
};

// ---------------------------------------------------------------------------
// World — owns entity slots + a type-indexed registry of component sets. Everything
// is carved from an arena at create(); nothing here allocates at runtime.
// ---------------------------------------------------------------------------
class World {
public:
    static Result<World*> create(ArenaAllocator& a, uint32_t max_entities);

    Entity spawn();
    void   despawn(Entity);                  // immediate; use defer() during iteration
    bool   is_alive(Entity) const;
    uint32_t count() const { return alive_count_; }
    uint32_t capacity() const { return max_; }

    template <class C> C&   add(Entity e, const C& v = {}) { return store<C>().add(e, v); }
    template <class C> void remove(Entity e)               { store<C>().remove(e); }
    template <class C> C*   get(Entity e)                  { return store<C>().try_get(e); }
    template <class C> bool has(Entity e)                  { return store<C>().has(e); }

    // Iterate entities that have ALL of <C0, Cs...> ; driven by the C0 dense array.
    template <class C0, class... Cs, class Fn>
    void each(Fn&& fn) {
        SparseSet<C0>& s = store<C0>();
        Entity* ents = s.ents();
        C0*     data = s.data();
        // iterate backwards so an in-loop remove of the current entity is swap-safe
        for (uint32_t n = s.size(); n-- > 0; ) {
            Entity e = ents[n];
            if ((has<Cs>(e) && ...))
                fn(e, data[n], *get<Cs>(e)...);   // guarded above, so deref is valid
        }
    }

    // Deferred structural changes — safe to record during iteration, applied on flush.
    struct Commands {
        World* w;
        void despawn(Entity e) { w->queue_despawn(e); }
    };
    Commands defer() { return Commands{ this }; }
    void     flush_deferred();

private:
    // Lazily creates (boot-arena backed) and returns the storage for component C.
    template <class C>
    SparseSet<C>& store() {
        const TypeId id = type_id<C>();
        PHX_ASSERT_MSG(id < kMaxComponentTypes, "too many component types");
        if (!sets_[id].set) {
            auto* s = arena_->make<SparseSet<C>>();
            s->init(*arena_, max_);
            sets_[id].set    = s;
            sets_[id].remove = [](void* p, Entity e) { static_cast<SparseSet<C>*>(p)->remove(e); };
            used_[used_count_++] = static_cast<uint16_t>(id);
        }
        return *static_cast<SparseSet<C>*>(sets_[id].set);
    }

    void queue_despawn(Entity e);
    void remove_from_all(Entity e);

    struct ErasedSet {
        void* set = nullptr;
        void (*remove)(void*, Entity) = nullptr;
    };

    ArenaAllocator* arena_ = nullptr;
    uint32_t  max_         = 0;
    uint32_t  alive_count_ = 0;

    uint8_t*  generation_  = nullptr;   // per slot — handle staleness guard
    uint8_t*  live_        = nullptr;   // per slot — 1 if currently allocated
    uint32_t* free_slots_  = nullptr;   // recycle stack of indices
    uint32_t  free_top_    = 0;
    uint32_t  high_water_  = 0;         // next never-used index

    ErasedSet sets_[kMaxComponentTypes];
    uint16_t  used_[kMaxComponentTypes];
    uint16_t  used_count_ = 0;

    Entity*   pending_     = nullptr;   // deferred despawn queue
    uint32_t  pending_cap_ = 0;
    uint32_t  pending_top_ = 0;
};

// Components are plain data. Systems are logic. They meet only through World data.
struct System {
    virtual ~System() = default;
    virtual void tick(World&, scalar dt) = 0;
};

} // namespace phx::ecs
#endif // PHX_ECS_WORLD_H
