// tests/unit/test_particles.cpp — the miracle-player's fixed-capacity particle pool
// (examples/miracle-player/src/particles.h) and the streaming ring's no-underrun invariant.
// The pool's hard promise is that it never writes past capacity (bound-check), and its integer
// simulation is byte-identical run-to-run and tier-to-tier. The ring test proves the per-frame
// pump cadence keeps the audio consumer fed at the GBA/host rates the visualizer uses.
#include "phx_test.h"
#include "particles.h"          // examples/miracle-player/src
#include "phx/audio/stream.h"   // RingBuffer

#include <cstdint>
#include <vector>

using namespace miracle;
using phx::RingBuffer;

PHX_TEST(particle_pool_never_overflows) {
    ParticlePool<8> pool;
    for (int i = 0; i < 8; ++i)
        CHECK(pool.spawn(0, 0, 0, 0, 100, 0xFFFFFFFFu, 1));   // fills to capacity
    CHECK_EQ(pool.count(), 8u);
    CHECK(pool.full());
    // Every further spawn is refused and the count never moves past the bound.
    for (int i = 0; i < 100; ++i) CHECK(!pool.spawn(0, 0, 0, 0, 100, 0, 1));
    CHECK_EQ(pool.count(), 8u);
}

PHX_TEST(particle_pool_ages_and_compacts) {
    ParticlePool<16> pool;
    pool.spawn(0, 0, 0, 0, 1, 0, 1);      // dies on the next update (life <= 1)
    pool.spawn(0, 0, 0, 0, 5, 0, 1);      // survives a few frames
    CHECK_EQ(pool.count(), 2u);
    pool.update(0);
    CHECK_EQ(pool.count(), 1u);           // the life-1 particle was compacted out
    for (int i = 0; i < 5; ++i) pool.update(0);
    CHECK_EQ(pool.count(), 0u);           // the other aged out too
}

PHX_TEST(particle_physics_is_integer) {
    ParticlePool<4> pool;
    const int32_t Q = 1 << 16;
    pool.spawn(0, 0, 2 * Q, -3 * Q, 100, 0, 1);   // vx=+2, vy=-3 px/frame (Q16)
    pool.update(1 << 15);                          // gravity = +0.5 px/frame^2
    const Particle& p = pool.data()[0];
    CHECK_EQ(p.x, 2 * Q);                          // moved right by vx
    CHECK_EQ(p.y, -3 * Q);                         // moved up by vy
    CHECK_EQ(p.vy, -3 * Q + (1 << 15));            // gravity added to vy
}

PHX_TEST(particle_sim_is_deterministic) {
    auto run = [](std::vector<int32_t>& out) {
        ParticlePool<64> pool; Rng rng(0xABCDEFu);
        for (int frame = 0; frame < 40; ++frame) {
            for (int k = 0; k < 3; ++k)
                pool.spawn(int32_t(rng.next() % 240) << 16, 0,
                           rng.range(1 << 16), -(1 << 16) - int32_t(rng.next() % (1 << 15)),
                           uint16_t(10 + rng.next() % 20), rng.next(), 1);
            pool.update(1 << 14);
        }
        out.push_back(int32_t(pool.count()));
        for (uint32_t i = 0; i < pool.count(); ++i)
            { out.push_back(pool.data()[i].x); out.push_back(pool.data()[i].y); }
    };
    std::vector<int32_t> a, b;
    run(a); run(b);
    CHECK_EQ(a.size(), b.size());
    CHECK(a == b);                                 // same seed -> byte-identical evolution
}

// The visualizer's streaming ring must never underrun: pump() tops it up once per video frame,
// and the audio consumer drains at most a frame's worth. With a 16384-sample ring this holds for
// both the GBA (304 samples/frame) and host (735 samples/frame) cadences after the first fill.
PHX_TEST(ring_never_underruns_at_frame_cadence) {
    auto simulate = [](uint32_t consume_per_frame) {
        const uint32_t cap = 1u << 14;
        std::vector<int16_t> storage(cap);
        RingBuffer ring; ring.init(storage.data(), cap);
        // A source longer than we will read; the "pump" writes as much as fits each frame.
        auto pump = [&](void) {
            uint32_t free = ring.free_space();
            static int16_t block[512];
            while (free) {
                uint32_t n = free < 512 ? free : 512;
                for (uint32_t i = 0; i < n; ++i) block[i] = 1;   // non-zero marker
                uint32_t w = ring.write(block, n);
                if (!w) break;
                free -= w;
            }
        };
        pump();                                                  // pre-roll fill (frame 0)
        int16_t out[1024];
        for (int frame = 0; frame < 2000; ++frame) {
            uint32_t got = ring.read(out, consume_per_frame);
            if (got != consume_per_frame) return false;          // underrun!
            pump();                                              // refill for next frame
        }
        return true;
    };
    CHECK(simulate(304));    // GBA vblank-locked cadence (18157 Hz / ~59.7)
    CHECK(simulate(735));    // host cadence (44100 Hz / 60)
}
