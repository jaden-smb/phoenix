// phx/memory/allocators.h — the four sanctioned allocators. After phx_init() returns,
// the engine performs ZERO general-purpose heap allocations on the hot path. Everything
// is carved from a root arena at boot. See docs/05-memory.md.
#ifndef PHX_MEMORY_ALLOCATORS_H
#define PHX_MEMORY_ALLOCATORS_H

#include "phx/core/types.h"
#include <new>          // placement new

namespace phx {

// ---------------------------------------------------------------------------
// ArenaAllocator — linear bump pointer. Freed as a whole (or back to a mark).
// ---------------------------------------------------------------------------
class ArenaAllocator {
    uint8_t* base_ = nullptr;
    size_t   cap_  = 0;
    size_t   off_  = 0;
public:
    void init(void* mem, size_t cap) { base_ = static_cast<uint8_t*>(mem); cap_ = cap; off_ = 0; }

    void* alloc(size_t n, size_t align = 16) {
        size_t p = align_up(off_, align);
        if (p + n > cap_) return nullptr;              // caller checks; boot-time fatal
        off_ = p + n;
        return base_ + p;
    }
    template <class T>
    T* alloc_array(size_t count) {
        return static_cast<T*>(alloc(sizeof(T) * count, alignof(T)));
    }
    template <class T, class... A>
    T* make(A&&... args) {
        void* m = alloc(sizeof(T), alignof(T));
        return m ? new (m) T(static_cast<A&&>(args)...) : nullptr;   // placement new only
    }

    size_t mark()  const         { return off_; }      // capture a rollback point
    void   reset_to(size_t m)    { off_ = m; }          // bulk free back to a mark
    void   reset()               { off_ = 0; }
    size_t used()  const         { return off_; }
    size_t capacity() const      { return cap_; }
};

// ---------------------------------------------------------------------------
// StackAllocator — arena + RAII scope guard. The per-frame transient allocator.
// ---------------------------------------------------------------------------
class StackAllocator : public ArenaAllocator {
public:
    struct Scope {                                      // frees everything alloc'd within
        StackAllocator* s; size_t m;
        explicit Scope(StackAllocator* s_) : s(s_), m(s_->mark()) {}
        ~Scope() { s->reset_to(m); }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    };
};
#define PHX_SCRATCH(allocPtr) ::phx::StackAllocator::Scope PHX_CONCAT(_phx_scope_, __LINE__)(allocPtr)

// ---------------------------------------------------------------------------
// PoolAllocator — fixed-size blocks, O(1) alloc/free, free list threaded in-place.
// ---------------------------------------------------------------------------
class PoolAllocator {
    uint8_t* base_ = nullptr;
    size_t   blk_  = 0;
    uint32_t count_ = 0;
    uint32_t free_head_ = kInvalid;
    static constexpr uint32_t kInvalid = 0xFFFFFFFFu;
public:
    void init(void* mem, size_t block_size, uint32_t count) {
        base_ = static_cast<uint8_t*>(mem);
        blk_  = block_size < sizeof(uint32_t) ? sizeof(uint32_t) : block_size;
        count_ = count;
        free_head_ = 0;
        for (uint32_t i = 0; i < count; ++i)            // thread free list through blocks
            *reinterpret_cast<uint32_t*>(base_ + i * blk_) = (i + 1 < count) ? (i + 1) : kInvalid;
    }
    void* alloc() {
        if (free_head_ == kInvalid) return nullptr;     // pool exhausted (visible ceiling)
        uint32_t i = free_head_;
        free_head_ = *reinterpret_cast<uint32_t*>(base_ + i * blk_);
        return base_ + i * blk_;
    }
    void free(void* p) {
        if (!p) return;
        uint32_t i = static_cast<uint32_t>((static_cast<uint8_t*>(p) - base_) / blk_);
        *reinterpret_cast<uint32_t*>(base_ + i * blk_) = free_head_;
        free_head_ = i;
    }
    uint32_t capacity() const { return count_; }
};

// ---------------------------------------------------------------------------
// ObjectPool<T,N> — typed recycling with ctor/dtor. For bullets, particles, etc.
// Storage is inline (no allocation) — the ceiling N is fixed and visible.
// ---------------------------------------------------------------------------
template <class T, uint32_t N>
class ObjectPool {
    alignas(T) uint8_t storage_[sizeof(T) * N];
    PoolAllocator pool_;
public:
    ObjectPool() { pool_.init(storage_, sizeof(T), N); }

    template <class... A>
    T* spawn(A&&... args) {
        void* m = pool_.alloc();
        return m ? new (m) T(static_cast<A&&>(args)...) : nullptr;   // nullptr = full
    }
    void despawn(T* p) {
        if (!p) return;
        p->~T();
        pool_.free(p);
    }
    static constexpr uint32_t capacity() { return N; }
};

} // namespace phx
#endif // PHX_MEMORY_ALLOCATORS_H
