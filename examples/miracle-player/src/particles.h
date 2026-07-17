// examples/miracle-player/src/particles.h — a fixed-capacity particle pool for the visualizer.
// Engine-free integer POD (like viz.h): all state is Q16 fixed-point, so the simulation is
// byte-identical on both scalar tiers (float PC / fixed16 GBA) — never `scalar`. No heap: the
// storage is an inline array, so a pool lives inside the game object, not on any hot path.
// spawn() REFUSES when full (returns false) rather than ever writing past capacity — the bound
// is a hard invariant the unit test pins.
#ifndef MIRACLE_PARTICLES_H
#define MIRACLE_PARTICLES_H

#include <stdint.h>

namespace miracle {

struct Particle {
    int32_t  x, y;     // Q16 pixel position
    int32_t  vx, vy;   // Q16 velocity (pixels/frame)
    uint16_t life;     // frames remaining (0 = dead)
    uint16_t life0;    // initial life, for fade
    uint32_t color;    // Rgba
    uint8_t  size;     // draw size in pixels
    uint8_t  _pad[3];
};

// Deterministic xorshift PRNG — spawn variety that is identical on both tiers.
struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed = 0x1234567u) : s(seed ? seed : 1u) {}
    uint32_t next() { uint32_t x = s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return s = x; }
    // signed value in [-range, range]
    int32_t range(int32_t r) { return r ? int32_t(next() % uint32_t(2 * r + 1)) - r : 0; }
};

template <uint32_t Cap>
class ParticlePool {
public:
    static constexpr uint32_t capacity = Cap;

    void     clear()       { count_ = 0; }
    uint32_t count() const  { return count_; }
    bool     full()  const  { return count_ >= Cap; }
    const Particle* data() const { return parts_; }

    // Add one particle. Returns false (no-op) when the pool is full — never overflows.
    bool spawn(int32_t x, int32_t y, int32_t vx, int32_t vy, uint16_t life, uint32_t color, uint8_t size) {
        if (count_ >= Cap) return false;
        Particle& p = parts_[count_++];
        p.x = x; p.y = y; p.vx = vx; p.vy = vy;
        p.life = life; p.life0 = life; p.color = color; p.size = size;
        return true;
    }

    // Advance one frame: integrate, apply gravity, age, and compact out the dead. Active
    // particles are kept packed in [0, count()).
    void update(int32_t gravity_q16) {
        uint32_t w = 0;
        for (uint32_t i = 0; i < count_; ++i) {
            Particle p = parts_[i];
            if (p.life <= 1) continue;         // dies this frame -> drop
            p.x += p.vx; p.y += p.vy; p.vy += gravity_q16; --p.life;
            parts_[w++] = p;
        }
        count_ = w;
    }

private:
    Particle parts_[Cap] {};
    uint32_t count_ = 0;
};

} // namespace miracle
#endif // MIRACLE_PARTICLES_H
