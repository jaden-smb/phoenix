// phx/anim/anim.h — sprite-sheet, frame-based animation driven by a tiny data-driven state
// machine. The system advances each Animator's timer, picks the current frame, and writes
// the source rect into the Animator so the render side stays dumb (it just blits the rect).
// See docs/10-gameplay-systems.md §5.
//
// Decoupled by design: depends only on `core` + `ecs`. Clips and transitions are DATA
// (authored, e.g. baked by `phxsprite`), never code — `idle ⇄ run ⇄ jump ⇄ fall` is a
// table, not a switch. Frame timing uses `scalar`, so fixed/float builds animate the same.
#ifndef PHX_ANIM_ANIM_H
#define PHX_ANIM_ANIM_H

#include "phx/core/types.h"
#include "phx/core/math.h"
#include "phx/ecs/world.h"

namespace phx {

// One animation = a contiguous run of `count` frames starting at sheet frame `first`,
// played at `fps`. Looping clips wrap; non-looping clips clamp on the last frame.
struct AnimClip {
    uint16_t first = 0;   // first frame index into the sheet
    uint16_t count = 1;   // number of frames in the clip
    uint8_t  fps   = 1;   // playback rate (0 = static, never advances)
    bool     loop  = true;
};

// Maps a sheet frame index to a source rect. Frames are packed left-to-right, top-to-bottom
// in a `cols`-wide grid of `frame_w` x `frame_h` cells.
struct SpriteSheet {
    uint16_t frame_w = 8;
    uint16_t frame_h = 8;
    uint16_t cols    = 1;
};

// ECS component. Holds the clip table + sheet (data), the playback cursor, AND the computed
// output rect (cur_*) the renderer reads. State id == clip index (the state machine drives
// both together); `finished` latches when a non-looping clip reaches its last frame.
struct Animator {
    Span<const AnimClip> clips;       // the authored clip table
    SpriteSheet          sheet;
    uint16_t clip   = 0;              // current clip / state id
    uint16_t frame  = 0;             // frame WITHIN the current clip (0..count-1)
    scalar   timer  = scalar{};      // accumulates dt toward the next frame
    bool     finished = false;
    // output (written by AnimationSystem::tick, read by the render side):
    int16_t  cur_sx = 0, cur_sy = 0, cur_sw = 0, cur_sh = 0;

    void play(uint16_t clip_id) {    // switch clip and restart from frame 0
        clip = clip_id; frame = 0; timer = scalar{}; finished = false;
    }
};

// Data-driven transitions: an edge fires when the machine is in `from` and receives
// `trigger`, switching to `to`. Edges are authored data (a sidecar), not code.
struct AnimStateMachine {
    struct Edge { uint8_t from; uint8_t to; uint16_t trigger; };
    Span<const Edge> edges;

    // Request a transition. If an edge matches (animator.clip, trig), switch clips.
    // Returns true if a transition fired.
    bool set_trigger(Animator& a, uint16_t trig) const {
        for (size_t i = 0; i < edges.size(); ++i)
            if (edges[i].from == a.clip && edges[i].trigger == trig) {
                a.play(edges[i].to);
                return true;
            }
        return false;
    }
};

// Advances every Animator in the world by one fixed step and refreshes its output rect.
class AnimationSystem {
public:
    void tick(ecs::World&, scalar dt) const;

    // Compute the source rect for a clip's current frame (also used to seed cur_* once).
    static void apply_rect(Animator&);
};

} // namespace phx
#endif // PHX_ANIM_ANIM_H
