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

// A data-driven transition: while the animator is in clip `from` (kAnyClip: in any clip) and
// receives `trigger`, it switches to clip `to`. Edges are authored data (a sprite's `trans` lines,
// baked by phxsprite), not code. The trigger kAnimDone fires by itself when a non-looping clip
// reaches its end, so `trans attack idle done` returns to idle after one swing.
struct AnimEdge {
    uint8_t  from;       // clip index, or kAnyClip
    uint8_t  to;         // clip index
    NameHash trigger;    // "jump"_hash, kAnimDone, ...   (offset 4: the baked SpriteTransDef's layout)
};
constexpr uint8_t  kAnyClip  = 0xFF;
constexpr NameHash kAnimDone = "done"_hash;

// ECS component. Holds the clip table + sheet (data), the playback cursor, AND the computed
// output rect (cur_*) the renderer reads. State id == clip index (the state machine drives
// both together); `finished` latches when a non-looping clip reaches its last frame.
struct Animator {
    Span<const AnimClip> clips;       // the authored clip table
    Span<const AnimEdge> edges;       // its transitions (optional; see trigger())
    SpriteSheet          sheet;
    uint16_t clip   = 0;              // current clip / state id
    uint16_t frame  = 0;             // frame WITHIN the current clip (0..count-1)
    scalar   timer  = scalar{};      // accumulates dt toward the next frame
    bool     finished = false;
    // 1/fps of the current clip, cached by AnimationSystem::tick when `clip` changes
    // (a scalar divide is a soft 64-bit division on GBA — too hot per entity per frame).
    scalar   frame_dur = scalar{};
    uint16_t dur_clip  = 0xFFFF;      // clip `frame_dur` was computed for (0xFFFF = none)
    // output (written by AnimationSystem::tick, read by the render side):
    int16_t  cur_sx = 0, cur_sy = 0, cur_sw = 0, cur_sh = 0;

    void play(uint16_t clip_id) {    // switch clip and restart from frame 0
        clip = clip_id; frame = 0; timer = scalar{}; finished = false;
    }
    // Fire a transition trigger. An edge from the current clip wins over a kAnyClip edge; an
    // any-clip edge into the clip already playing does nothing (so a trigger can be sent every
    // step). True when a transition fired.
    bool trigger(NameHash trig) { return fire(edges, trig); }
    bool fire(Span<const AnimEdge> es, NameHash trig) {
        for (size_t i = 0; i < es.size(); ++i)
            if (es[i].from == clip && es[i].trigger == trig) { play(es[i].to); return true; }
        for (size_t i = 0; i < es.size(); ++i)
            if (es[i].from == kAnyClip && es[i].trigger == trig && es[i].to != clip) { play(es[i].to); return true; }
        return false;
    }
};

// A standalone edge table (the Animator carries its own in `edges`; this drives any animator).
struct AnimStateMachine {
    using Edge = AnimEdge;
    Span<const Edge> edges;

    // Request a transition. If an edge matches (animator.clip, trig), switch clips.
    // Returns true if a transition fired.
    bool set_trigger(Animator& a, NameHash trig) const { return a.fire(edges, trig); }
};

// Advances every Animator in the world by one fixed step and refreshes its output rect. When a
// non-looping clip finishes, the animator's own edges receive kAnimDone.
class AnimationSystem {
public:
    void tick(ecs::World&, scalar dt) const;

    // Compute the source rect for a clip's current frame (also used to seed cur_* once).
    static void apply_rect(Animator&);
};

} // namespace phx
#endif // PHX_ANIM_ANIM_H
