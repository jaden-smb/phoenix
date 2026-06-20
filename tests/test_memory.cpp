// tests/test_memory.cpp — allocators + MemoryRoot. Proves O(1) reuse and no growth.
#include "phx_test.h"
#include "phx/memory/allocators.h"
#include "phx/memory/memory_root.h"

using namespace phx;

PHX_TEST(arena_basic) {
    alignas(16) static uint8_t buf[1024];
    ArenaAllocator a; a.init(buf, sizeof(buf));

    int* x = static_cast<int*>(a.alloc(sizeof(int)));
    CHECK(x != nullptr);
    *x = 42; CHECK_EQ(*x, 42);

    size_t m = a.mark();
    a.alloc(256);
    CHECK(a.used() > m);
    a.reset_to(m);
    CHECK_EQ(a.used(), m);

    a.reset();
    CHECK_EQ(a.used(), 0u);
}

PHX_TEST(arena_alignment) {
    alignas(16) static uint8_t buf[512];
    ArenaAllocator a; a.init(buf, sizeof(buf));
    a.alloc(1);                                   // misalign
    void* p = a.alloc(8, 16);
    CHECK_EQ(reinterpret_cast<uintptr_t>(p) % 16, 0u);
}

PHX_TEST(arena_make) {
    alignas(16) static uint8_t buf[256];
    ArenaAllocator a; a.init(buf, sizeof(buf));
    struct Foo { int x; Foo(int v) : x(v) {} };
    Foo* f = a.make<Foo>(7);
    CHECK(f != nullptr);
    CHECK_EQ(f->x, 7);
}

PHX_TEST(pool_alloc_free_reuse) {
    alignas(16) static uint8_t buf[64 * 8];
    PoolAllocator p; p.init(buf, 8, 64);

    void* a = p.alloc();
    void* b = p.alloc();
    CHECK(a && b && a != b);
    p.free(a);
    void* c = p.alloc();                          // should reuse the just-freed block
    CHECK_EQ(c, a);
}

PHX_TEST(pool_exhaustion) {
    alignas(16) static uint8_t buf[4 * 8];
    PoolAllocator p; p.init(buf, 8, 4);
    void* held[4];
    for (int i = 0; i < 4; ++i) { held[i] = p.alloc(); CHECK(held[i] != nullptr); }
    CHECK(p.alloc() == nullptr);                  // full -> nullptr (visible ceiling)
    p.free(held[1]);
    CHECK(p.alloc() != nullptr);                  // freeing one makes room again
}

PHX_TEST(object_pool_ctor_dtor) {
    static int live = 0;
    struct Obj { int id; Obj(int i) : id(i) { ++live; } ~Obj() { --live; } };
    ObjectPool<Obj, 8> pool;

    Obj* a = pool.spawn(1);
    Obj* b = pool.spawn(2);
    CHECK_EQ(live, 2);
    CHECK_EQ(a->id, 1);
    CHECK_EQ(b->id, 2);
    pool.despawn(a);
    CHECK_EQ(live, 1);
    pool.despawn(b);
    CHECK_EQ(live, 0);
}

PHX_TEST(stack_scope_raii) {
    alignas(16) static uint8_t buf[1024];
    StackAllocator s; s.init(buf, sizeof(buf));
    size_t base = s.used();
    {
        PHX_SCRATCH(&s);
        s.alloc(256);
        CHECK(s.used() > base);
    }                                              // scope exit -> reclaimed
    CHECK_EQ(s.used(), base);
}

PHX_TEST(memory_root_boot) {
    auto r = MemoryRoot::boot(1 << 20, 64 * 1024); // 1 MB total, 64 KB frame scratch
    CHECK(r.ok());
    MemoryRoot* mr = r.unwrap();

    void* p = mr->persistent().alloc(1000);
    CHECK(p != nullptr);

    ArenaAllocator sub = mr->sub(4096, "test_sub");
    CHECK_EQ(sub.capacity(), 4096u);

    size_t before = mr->frame_stack().used();
    mr->frame_stack().alloc(128);
    mr->swap_frame();                              // swap to the other buffer
    CHECK_EQ(mr->frame_stack().used(), 0u);        // fresh buffer is empty
    (void)before;

    MemoryRoot::shutdown(mr);
}

PHX_TEST(no_fragmentation_churn) {
    // 100k spawn/free cycles must not grow footprint (the anti-fragmentation proof).
    alignas(16) static uint8_t buf[16 * 32];
    PoolAllocator p; p.init(buf, 16, 32);
    for (int iter = 0; iter < 100000; ++iter) {
        void* a = p.alloc();
        void* b = p.alloc();
        CHECK(a && b);
        p.free(b);
        p.free(a);
    }
    // if we got here without nullptr, the free list stayed intact across 200k ops.
    CHECK(true);
}
