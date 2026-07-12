// tests/test_anim.cpp — sprite-sheet animation: frame advance over time, looping wrap,
// non-looping clamp, source-rect computation from the sheet grid, the data-driven state
// machine, and static clips. Pure ECS, runs in the unit suite on both scalar tiers.
#include "phx_test.h"
#include "phx/anim/anim.h"
#include "phx/ecs/world.h"

using namespace phx;
using namespace phx::ecs;

namespace {

World* fresh_world(uint32_t cap = 256) {
    static uint8_t* pool = new uint8_t[8 << 20];
    static size_t   off  = 0;
    ArenaAllocator* a = new ArenaAllocator();
    a->init(pool + off, 1 << 20);
    off += (1 << 20);
    return World::create(*a, cap).unwrap();
}

#if PHX_CAPS_HAS_FLOAT_HW
const scalar kDt = 1.0f / 60.0f;
#else
const scalar kDt = fixed16::from_int(1) / fixed16::from_int(60);
#endif

// state/clip 0 = idle (static), 1 = run (4 frames @ 8fps, loop), 2 = jump (2 frames, no loop)
const AnimClip kClips[3] = {
    { /*first*/0, /*count*/1, /*fps*/1,  /*loop*/true  },   // idle
    { /*first*/1, /*count*/4, /*fps*/8,  /*loop*/true  },   // run
    { /*first*/5, /*count*/2, /*fps*/10, /*loop*/false },   // jump
};
SpriteSheet sheet16() { SpriteSheet s; s.frame_w = 16; s.frame_h = 16; s.cols = 4; return s; }

Animator make_animator(uint16_t clip) {
    Animator a;
    a.clips = Span<const AnimClip>{ kClips, 3 };
    a.sheet = sheet16();
    a.play(clip);
    return a;
}

} // namespace

PHX_TEST(anim_advances_frames_over_time) {
    World* w = fresh_world();
    AnimationSystem sys;
    Entity e = w->spawn();
    w->add<Animator>(e, make_animator(1));   // run @ 8fps -> 0.125s/frame

    for (int i = 0; i < 8; ++i) sys.tick(*w, kDt);   // ~0.133s -> one frame crossed
    CHECK_EQ(w->get<Animator>(e)->frame, 1u);
    for (int i = 0; i < 8; ++i) sys.tick(*w, kDt);   // ~0.267s total
    CHECK_EQ(w->get<Animator>(e)->frame, 2u);
}

PHX_TEST(anim_loops_wrap) {
    World* w = fresh_world();
    AnimationSystem sys;
    Entity e = w->spawn();
    w->add<Animator>(e, make_animator(1));   // run, loop, 4 frames

    for (int i = 0; i < 32; ++i) sys.tick(*w, kDt);   // ~0.533s -> 4 advances -> wrapped to 0
    Animator* a = w->get<Animator>(e);
    CHECK_EQ(a->frame, 0u);
    CHECK(!a->finished);
}

PHX_TEST(anim_nonloop_clamps_and_finishes) {
    World* w = fresh_world();
    AnimationSystem sys;
    Entity e = w->spawn();
    w->add<Animator>(e, make_animator(2));   // jump @ 10fps, 2 frames, no loop

    for (int i = 0; i < 30; ++i) sys.tick(*w, kDt);   // 0.5s, well past the 2 frames
    Animator* a = w->get<Animator>(e);
    CHECK_EQ(a->frame, 1u);                   // clamped on last frame
    CHECK(a->finished);
}

PHX_TEST(anim_source_rect_from_sheet_grid) {
    World* w = fresh_world();
    AnimationSystem sys;
    Entity e = w->spawn();
    w->add<Animator>(e, make_animator(2));    // jump.first = 5
    sys.tick(*w, kDt);                         // populates cur_* for frame 0 -> global 5

    Animator* a = w->get<Animator>(e);
    CHECK_EQ(a->cur_sx, 16);                   // (5 % 4) * 16
    CHECK_EQ(a->cur_sy, 16);                   // (5 / 4) * 16
    CHECK_EQ(a->cur_sw, 16);
    CHECK_EQ(a->cur_sh, 16);
}

PHX_TEST(anim_static_clip_does_not_advance) {
    World* w = fresh_world();
    AnimationSystem sys;
    Entity e = w->spawn();
    w->add<Animator>(e, make_animator(0));    // idle: count 1 -> never advances

    for (int i = 0; i < 60; ++i) sys.tick(*w, kDt);
    Animator* a = w->get<Animator>(e);
    CHECK_EQ(a->frame, 0u);
    CHECK_EQ(a->cur_sx, 0);                    // global frame 0
    CHECK_EQ(a->cur_sy, 0);
}

PHX_TEST(anim_state_machine_transitions) {
    const AnimStateMachine::Edge kEdges[2] = {
        { /*from*/0, /*to*/1, /*trigger*/1 },   // idle --MOVE--> run
        { /*from*/1, /*to*/0, /*trigger*/2 },   // run  --STOP--> idle
    };
    AnimStateMachine sm; sm.edges = Span<const AnimStateMachine::Edge>{ kEdges, 2 };

    Animator a = make_animator(0);
    // advance run a bit won't matter since we're in idle; trigger MOVE -> run, frame reset
    a.frame = 3;
    CHECK(sm.set_trigger(a, 1));               // idle --MOVE--> run
    CHECK_EQ(a.clip, 1u);
    CHECK_EQ(a.frame, 0u);                      // play() reset the cursor

    CHECK(!sm.set_trigger(a, 99));             // no edge for this trigger -> no change
    CHECK_EQ(a.clip, 1u);

    CHECK(sm.set_trigger(a, 2));               // run --STOP--> idle
    CHECK_EQ(a.clip, 0u);
}
