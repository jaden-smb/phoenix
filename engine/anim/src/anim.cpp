// phx/anim/anim.cpp — the frame advance + source-rect computation (docs/10 §5).
//
// Per fixed step, per Animator: add dt to the timer; while the timer covers a whole frame
// (1/fps seconds), step to the next frame — looping clips wrap, non-looping clips clamp on
// the last frame and latch `finished`. Then write the current frame's source rect into the
// Animator for the renderer to blit. Frame duration is `scalar`, so fixed/float agree.
#include "phx/anim/anim.h"

namespace phx {

void AnimationSystem::apply_rect(Animator& a) {
    if (a.clip >= a.clips.size()) return;
    const AnimClip& c = a.clips[a.clip];
    const uint16_t  frame_in_clip = (c.count > 0 && a.frame >= c.count) ? uint16_t(c.count - 1) : a.frame;
    const uint16_t  g = uint16_t(c.first + frame_in_clip);   // global sheet frame index
    const uint16_t  cols = a.sheet.cols ? a.sheet.cols : 1;
    a.cur_sx = int16_t((g % cols) * a.sheet.frame_w);
    a.cur_sy = int16_t((g / cols) * a.sheet.frame_h);
    a.cur_sw = int16_t(a.sheet.frame_w);
    a.cur_sh = int16_t(a.sheet.frame_h);
}

void AnimationSystem::tick(ecs::World& w, scalar dt) const {
    w.each<Animator>([&](ecs::Entity, Animator& a) {
        if (a.clip >= a.clips.size()) { apply_rect(a); return; }
        const AnimClip& c = a.clips[a.clip];

        if (c.fps > 0 && c.count > 1 && !(a.finished && !c.loop)) {
            const scalar frame_dur = s_from_int(1) / s_from_int(int(c.fps));
            a.timer += dt;
            // consume whole frames; the while-loop handles dt spanning several frames
            while (a.timer >= frame_dur) {
                a.timer -= frame_dur;
                if (a.frame + 1 < c.count) {
                    ++a.frame;
                } else if (c.loop) {
                    a.frame = 0;
                } else {
                    a.finished = true;     // clamp on last frame; stop consuming
                    a.timer = scalar{};
                    break;
                }
            }
        }
        apply_rect(a);
    });
}

} // namespace phx
