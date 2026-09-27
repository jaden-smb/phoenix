// engine/runtime/src/behaviours.cpp — see phx/runtime/behaviours.h. The components register
// themselves (PHX_COMPONENT), so a prefab's `components` column can name them. Linked by game
// projects and the behaviours suite.
#include "phx/runtime/behaviours.h"
#include "phx/runtime/app.h"
#include "phx/runtime/audio.h"

namespace phx {

PHX_COMPONENT(PlatformerController, PHX_FIELD(PlatformerController, speed), PHX_FIELD(PlatformerController, jump),
              PHX_FIELD_HASH(PlatformerController, jump_sound), PHX_FIELD_HASH(PlatformerController, idle_clip),
              PHX_FIELD_HASH(PlatformerController, walk_clip), PHX_FIELD_HASH(PlatformerController, jump_clip));
PHX_COMPONENT(Patrol, PHX_FIELD(Patrol, speed), PHX_FIELD(Patrol, range));
PHX_COMPONENT(Pickup, PHX_FIELD(Pickup, value), PHX_FIELD_HASH(Pickup, counter), PHX_FIELD_HASH(Pickup, sound));
PHX_COMPONENT(Hazard, PHX_FIELD_HASH(Hazard, sound));
PHX_COMPONENT(Checkpoint, PHX_FIELD_HASH(Checkpoint, sound));
PHX_COMPONENT(Exit, PHX_FIELD_HASH(Exit, target));
PHX_COMPONENT(CameraFollow, PHX_FIELD(CameraFollow, offset_y));

bool play_clip(ecs::World& w, ecs::Entity e, NameHash clip) {
    const SpriteRenderer* s = w.get<SpriteRenderer>(e);
    Animator* an = w.get<Animator>(e);
    if (!s || !an || !clip || !s->clip_names) return false;
    for (uint16_t i = 0; i < s->clip_count; ++i)
        if (s->clip_names[i] == clip) {
            if (an->clip != i) an->play(i);
            return true;
        }
    return false;
}

void Behaviours::start(App& app, Level& level, ResourceCache& res, PhysicsWorld& physics) {
    level_ = &level; res_ = &res; physics_ = &physics;
    player_ = ecs::kInvalid;
    uint16_t first = 0xFFFF;
    ecs::World& w = app.world();
    w.each<PlatformerController, PrefabRef>([&](ecs::Entity e, PlatformerController&, PrefabRef& p) {
        if (p.spawn < first) { first = p.spawn; player_ = e; }
    });
    if (player_ != ecs::kInvalid) respawn_ = w.get<Transform>(player_)->pos;
    for (uint32_t i = 0; i < kMaxCounters; ++i) { counter_names_[i] = 0; counters_[i] = 0; }
    exit_ = 0; hit_count_ = 0;
    camera_ = Camera2D{};
    follow(app, w);
}

void Behaviours::update(App& app, scalar dt) {
    if (!physics_) return;
    ecs::World& w = app.world();
    if (player_ != ecs::kInvalid && !w.is_alive(player_)) player_ = ecs::kInvalid;
    if (player_ != ecs::kInvalid) control(app, w);
    patrol(w, dt);
    hit_count_ = physics_->step(w, dt, Span<Hit>{ hits_, kMaxHits });
    if (player_ != ecs::kInvalid) {
        contacts(app, w);
        // hazard TILES (the map editor's hazard flag), as the map authored them
        const Transform* t = w.get<Transform>(player_);
        const AABBColl* c = w.get<AABBColl>(player_);
        if (t && c && (physics_->tile_flags_in(aabb::from_center(t->pos, c->half)) & kTileHazard)) respawn(app);
    }
    anim_.tick(w, dt);
    follow(app, w);
}

void Behaviours::control(App& app, ecs::World& w) {
    const PlatformerController* pc = w.get<PlatformerController>(player_);
    Body* b = w.get<Body>(player_);
    if (!pc || !b) return;
    const InputState& in = app.input();
    const int dir = (in.down(Button::Right) ? 1 : 0) - (in.down(Button::Left) ? 1 : 0);
    b->vel.x = pc->speed * s_from_int(dir);
    if (in.just(Button::A) && b->on_ground) {
        b->vel.y = s_from_int(0) - pc->jump;
        play(app, pc->jump_sound);
    }
    if (SpriteRenderer* s = w.get<SpriteRenderer>(player_))
        if (dir) s->flags = dir < 0 ? uint16_t(kFlipX) : uint16_t(0);
    if (!b->on_ground && play_clip(w, player_, pc->jump_clip)) return;
    play_clip(w, player_, dir ? pc->walk_clip : pc->idle_clip);
}

void Behaviours::patrol(ecs::World& w, scalar) {
    w.each<Patrol, Transform, Body>([&](ecs::Entity e, Patrol& p, Transform& t, Body& b) {
        if (!p.homed) {
            p.homed = true;
            p.home_x = t.pos.x;
        } else if (b.vel.x == s_from_int(0)) {
            p.dir = int8_t(-p.dir);                       // the last step hit a wall: turn round
        }
        const scalar lo = p.home_x - s_from_int(p.range), hi = p.home_x + s_from_int(p.range);
        if (t.pos.x > hi) p.dir = -1;
        if (t.pos.x < lo) p.dir = 1;
        b.vel.x = p.speed * s_from_int(p.dir);
        if (SpriteRenderer* s = w.get<SpriteRenderer>(e)) s->flags = p.dir < 0 ? uint16_t(kFlipX) : uint16_t(0);
    });
}

void Behaviours::contacts(App& app, ecs::World& w) {
    for (uint32_t i = 0; i < hit_count_; ++i) {
        const ecs::Entity o = hits_[i].a == player_ ? hits_[i].b : hits_[i].b == player_ ? hits_[i].a : ecs::kInvalid;
        if (o == ecs::kInvalid || !w.is_alive(o)) continue;
        if (const Pickup* pk = w.get<Pickup>(o)) {
            slot(pk->counter) += pk->value;
            play(app, pk->sound);
            w.remove<Pickup>(o);                           // counted once, even if touched twice
            w.defer().despawn(o);
            continue;
        }
        if (const Hazard* hz = w.get<Hazard>(o)) {
            play(app, hz->sound);
            respawn(app);
            return;                                        // the player moved: the rest are stale
        }
        if (const Checkpoint* cp = w.get<Checkpoint>(o)) {
            const vec2 at = w.get<Transform>(o)->pos;
            if (at.x != respawn_.x || at.y != respawn_.y) { respawn_ = at; play(app, cp->sound); }
            continue;
        }
        if (const Exit* ex = w.get<Exit>(o)) exit_ = ex->target ? ex->target : NameHash("exit"_hash);
    }
}

void Behaviours::respawn(App& app) {
    ecs::World& w = app.world();
    if (player_ == ecs::kInvalid) return;
    if (Transform* t = w.get<Transform>(player_)) t->pos = respawn_;
    if (Body* b = w.get<Body>(player_)) b->vel = vec2{};
    slot("deaths"_hash) += 1;
}

void Behaviours::follow(App& app, ecs::World& w) {
    if (!level_) return;
    ecs::Entity target = ecs::kInvalid;
    int16_t offset_y = 0;
    uint16_t first = 0xFFFF;
    w.each<CameraFollow, PrefabRef>([&](ecs::Entity e, CameraFollow& f, PrefabRef& p) {
        if (p.spawn < first) { first = p.spawn; target = e; offset_y = f.offset_y; }
    });
    const Transform* t = target != ecs::kInvalid ? w.get<Transform>(target) : nullptr;
    if (!t) return;
    const int sw = app.config().width, sh = app.config().height;
    int px = s_to_int(t->pos.x) - sw / 2, py = s_to_int(t->pos.y) + offset_y - sh / 2;
    const int maxx = level_->width_px() - sw > 0 ? level_->width_px() - sw : 0;
    const int maxy = level_->height_px() - sh > 0 ? level_->height_px() - sh : 0;
    px = px < 0 ? 0 : (px > maxx ? maxx : px);
    py = py < 0 ? 0 : (py > maxy ? maxy : py);
    camera_.pos = vec2{ s_from_int(px), s_from_int(py) };
}

void Behaviours::play(App& app, NameHash sound) {
    if (!sound || !res_) return;
    if (auto s = res_->sound(sound)) app.audio().play(to_sound(s.unwrap()));
}

int32_t& Behaviours::slot(NameHash name) {
    for (uint32_t i = 0; i < kMaxCounters; ++i) if (counter_names_[i] == name) return counters_[i];
    for (uint32_t i = 0; i < kMaxCounters; ++i)
        if (!counter_names_[i]) { counter_names_[i] = name; counters_[i] = 0; return counters_[i]; }
    return scratch_;                                       // more than kMaxCounters: not kept
}

int32_t Behaviours::counter(NameHash name) const {
    for (uint32_t i = 0; i < kMaxCounters; ++i) if (counter_names_[i] == name) return counters_[i];
    return 0;
}

void Behaviours::set_counter(NameHash name, int32_t v) { slot(name) = v; }

} // namespace phx
