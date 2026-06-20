// phx/core/time.h — monotonic clock + the fixed-step accumulator that drives the loop.
// Deterministic by construction: a fixed sim step + a spiral-of-death clamp. See docs/00 §7.
#ifndef PHX_CORE_TIME_H
#define PHX_CORE_TIME_H

#include "phx/core/types.h"
#include "phx/core/math.h"     // scalar + PHX_CAPS_HAS_FLOAT_HW

namespace phx {

constexpr uint64_t kNsPerSec = 1000000000ull;

// Drives the fixed-timestep simulation. advance() returns how many sim steps to run
// this frame; alpha() is the [0,1) render interpolation factor between steps.
struct StepAccumulator {
    uint64_t step_ns = kNsPerSec / 60;   // 1 / sim_hz
    uint64_t acc     = 0;
    uint64_t max_ns  = (kNsPerSec / 60) * 5;   // clamp: never catch up more than 5 steps

    void configure(uint16_t sim_hz, uint8_t max_catchup = 5) {
        step_ns = kNsPerSec / (sim_hz ? sim_hz : 60);
        max_ns  = step_ns * max_catchup;
        acc     = 0;
    }

    // Feed elapsed wall time; returns the number of fixed steps to simulate.
    int advance(uint64_t elapsed_ns) {
        acc += min2<uint64_t>(elapsed_ns, max_ns);     // clamp spiral-of-death
        int steps = 0;
        while (acc >= step_ns) { acc -= step_ns; ++steps; }
        return steps;
    }

    // Render interpolation factor in [0,1): how far we are toward the next step.
    scalar alpha() const {
#if PHX_CAPS_HAS_FLOAT_HW
        return scalar(double(acc) / double(step_ns));
#else
        // fixed16: (acc << 16) / step_ns, done in 64-bit to avoid overflow.
        return fixed16::from_raw(int32_t((int64_t(acc) << fixed16::kShift) / int64_t(step_ns)));
#endif
    }
};

// The fixed simulation dt as a scalar (seconds). Precomputed once from sim_hz.
inline scalar fixed_dt(uint16_t sim_hz) {
#if PHX_CAPS_HAS_FLOAT_HW
    return scalar(1.0 / double(sim_hz ? sim_hz : 60));
#else
    return fixed16::from_raw(int32_t((int64_t(fixed16::kOne)) / int64_t(sim_hz ? sim_hz : 60)));
#endif
}

} // namespace phx
#endif // PHX_CORE_TIME_H
