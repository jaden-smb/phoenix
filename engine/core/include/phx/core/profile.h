// phx/core/profile.h — frame profiling as plain data. The runtime loop stamps each phase
// with the platform clock and stores microseconds here; the UI's profile_overlay (or the
// game's own HUD) reads it. Pure integer POD: no clock access from core (layering — core is
// the closed foundation), no floats (tier-agnostic), no allocation, ~16 bytes.
#ifndef PHX_CORE_PROFILE_H
#define PHX_CORE_PROFILE_H

#include "phx/core/types.h"

namespace phx {

// One frame's phase timings, in microseconds. `budget_us` is the frame budget the overlay
// scales against (set from the sim rate at boot; 60 Hz -> 16667).
struct FrameProfile {
    uint32_t update_us  = 0;   // all fixed steps this frame
    uint32_t render_us  = 0;   // the game's render hook (record + backend draw)
    uint32_t present_us = 0;   // platform present (swap / vblank wait / blit)
    uint32_t frame_us   = 0;   // whole loop iteration (>= the sum; includes input/pump)
    uint32_t budget_us  = 16667;
};

// Integer EMA smoother (alpha = 1/8) so an overlay readout holds still enough to read.
// push() a raw value each frame; read `avg_us`.
struct ProfileAvg {
    uint32_t avg_us = 0;
    void     push(uint32_t us) { avg_us = avg_us ? (avg_us * 7u + us) / 8u : us; }
};

} // namespace phx
#endif // PHX_CORE_PROFILE_H
