// examples/emberwing/systems.cpp — the gameplay systems: player movement (coyote time, jump
// buffer, variable-height jump), the three enemy behaviours, geyser timing, Hit resolution,
// game-side pickup/trigger collection, particles, and the look-ahead camera. Uses ONLY engine
// APIs; compiles unchanged for every target. No platform headers, no STL on the game path.
//
// Determinism discipline (the engine's cross-tier gate): velocities are integer px/s, timers
// are integer frame counts, and camera smoothing is integer pixel math — identical on the
// float (PC/PSP) and fixed16 (GBA) scalar tiers.
#include "game.h"
#include "phx/core/log.h"

namespace game {

using namespace tune;

// ---- player -----------------------------------------------------------------------------
void player_system(EngineCtx* c, scalar /*dt*/) {
    const InputState& in = *c->input;
    ecs::World& w = *c->world;
    w.each<Player, Body, Transform>([&](ecs::Entity e, Player& pl, Body& b, Transform&) {
        // horizontal run (instant accel keeps the sim tier-exact and the controls crisp)
        scalar vx = s_from_int(0);
        if (in.down(Button::Right))      { vx =  kRun; pl.facing_left = false; }
        else if (in.down(Button::Left))  { vx = -kRun; pl.facing_left = true;  }
        b.vel.x = vx;

        // coyote time + jump buffering (integer frame counters)
        if (b.on_ground) pl.coyote = kCoyoteFr;
        else if (pl.coyote > 0) --pl.coyote;
        if (in.just(Button::A)) pl.jump_buf = kJumpBufFr;
        else if (pl.jump_buf > 0) --pl.jump_buf;

        if (pl.jump_buf > 0 && (b.on_ground || pl.coyote > 0)) {
            b.vel.y = -kJump;
            pl.jump_buf = 0; pl.coyote = 0; pl.jump_held = true;
            sfx(c, c->snd_jump, 0.7f);
        }
        // variable jump height: releasing A while rising cuts the ascent once
        if (pl.jump_held && !in.down(Button::A)) {
            if (b.vel.y < s_from_int(0)) b.vel.y = s_half(b.vel.y);
            pl.jump_held = false;
        }
        if (b.vel.y > kTerminal) b.vel.y = kTerminal;   // terminal fall speed

        if (pl.invuln > 0) --pl.invuln;

        // animation state from movement (hurt clip rides the first third of the i-frames)
        uint16_t want;
        if      (pl.invuln > kInvulnFr - 20)      want = kHeroHurt;
        else if (!b.on_ground)                    want = (b.vel.y < s_from_int(0)) ? kHeroJump
                                                                                   : kHeroFall;
        else if (vx != s_from_int(0))             want = kHeroRun;
        else                                      want = kHeroIdle;
        if (Animator* an = w.get<Animator>(e); an && an->clip != want) an->play(want);
        if (SpriteRef* sr = w.get<SpriteRef>(e)) {
            if (pl.facing_left) sr->flags |= kFlipX; else sr->flags &= uint16_t(~kFlipX);
        }
    });
}

// ---- enemies ------------------------------------------------------------------------------
// Cinderlings and thornbacks patrol horizontally around home.x; ashwisps bob vertically
// around home.y (no gravity). Runs before physics so velocities integrate this step.
void enemy_system(EngineCtx* c, scalar /*dt*/) {
    ecs::World& w = *c->world;
    w.each<Enemy, Body, Transform>([&](ecs::Entity e, Enemy& en, Body& b, Transform& t) {
        if (en.kind == EnemyKind::Wisp) {
            if      (t.pos.y <= en.home.y - en.range) en.dir = 1;
            else if (t.pos.y >= en.home.y + en.range) en.dir = -1;
            b.vel.y = (en.dir > 0) ? kWispBob : -kWispBob;
        } else {
            // A wall zeroes vel.x in the physics resolve — read that as "turn around"
            // (spawns seed vel.x so the first frame doesn't false-trigger).
            if (b.vel.x == s_from_int(0)) en.dir = int8_t(-en.dir);
            if      (t.pos.x <= en.home.x - en.range) en.dir = 1;
            else if (t.pos.x >= en.home.x + en.range) en.dir = -1;
            const scalar run = (en.kind == EnemyKind::Thorn) ? kThornRun : kCinderRun;
            b.vel.x = (en.dir > 0) ? run : -run;
            if (SpriteRef* sr = w.get<SpriteRef>(e)) {
                if (en.dir < 0) sr->flags |= kFlipX; else sr->flags &= uint16_t(~kFlipX);
            }
        }
    });
}

// ---- geysers ------------------------------------------------------------------------------
// idle -> warn (puff) -> erupt (flame column). Timers are frame counts; spawns phase-offset
// them so a corridor of geysers forms a readable wave. The hazard only bites while erupting
// (interaction_system checks the state); the sprite is hidden while idle.
void geyser_system(EngineCtx* c, scalar /*dt*/) {
    ecs::World& w = *c->world;
    Transform* pt = c->world->get<Transform>(c->player);
    w.each<Geyser, Transform>([&](ecs::Entity e, Geyser& g, Transform& t) {
        if (++g.timer >= kGeyserCycle) g.timer = 0;
        GeyserState want;
        if      (g.timer < kGeyserIdle)                want = GeyserState::Idle;
        else if (g.timer < kGeyserIdle + kGeyserWarn)  want = GeyserState::Warn;
        else                                           want = GeyserState::Erupt;
        if (want == g.state) return;
        g.state = want;

        SpriteRef* sr = w.get<SpriteRef>(e);
        Animator*  an = w.get<Animator>(e);
        if (want == GeyserState::Idle) { if (sr) sr->hidden = true; return; }
        if (sr) sr->hidden = false;
        if (an) an->play(want == GeyserState::Warn ? kGeyserWarnClip : kGeyserEruptClip);
        // audible only near the player (a whole corridor erupting at once would be noise)
        if (want == GeyserState::Erupt && pt) {
            const int dx = s_to_int(t.pos.x) - s_to_int(pt->pos.x);
            if (dx > -200 && dx < 200) sfx(c, c->snd_geyser, 0.5f);
        }
    });
}

// ---- physics ------------------------------------------------------------------------------
void physics_system(EngineCtx* c, scalar dt) {
    c->hit_count = c->physics->step(*c->world, dt, Span<Hit>{ c->hits, 64 });
}

// ---- damage / death -------------------------------------------------------------------------
void respawn_player(EngineCtx* c) {
    ecs::World& w = *c->world;
    if (Transform* t = w.get<Transform>(c->player)) t->pos = c->respawn;
    if (Body* b = w.get<Body>(c->player)) b->vel = vec2{ s_from_int(0), s_from_int(0) };
    if (Player* pl = w.get<Player>(c->player)) {
        pl->health = kMaxHealth;
        pl->invuln = kInvulnFr + 30;          // longer grace after a death
    }
    ++c->deaths;
    if (c->camera) c->camera->shake = 4;
}

namespace {
// Damage the player (unless in i-frames). damage >= kMaxHealth (lava) kills outright.
void hurt_player(EngineCtx* c, ecs::Entity pe, Player& pl, int16_t damage) {
    if (pl.invuln > 0) return;
    pl.health = int16_t(pl.health - damage);
    pl.invuln = kInvulnFr;
    if (Body* b = c->world->get<Body>(pe)) b->vel.y = -kHurtKick;
    sfx(c, c->snd_hurt, 0.8f);
    if (c->camera) c->camera->shake = 3;
    if (pl.health <= 0) respawn_player(c);
}
} // namespace

// ---- Hit resolution (player vs enemies/hazards) --------------------------------------------
void interaction_system(EngineCtx* c, scalar /*dt*/) {
    ecs::World& w = *c->world;
    for (uint32_t i = 0; i < c->hit_count; ++i) {
        // Orient the pair so `pe` is the player (if it's in this hit at all).
        ecs::Entity a = c->hits[i].a, b = c->hits[i].b;
        ecs::Entity pe = w.has<Player>(a) ? a : (w.has<Player>(b) ? b : ecs::kInvalid);
        if (pe == ecs::kInvalid) continue;
        ecs::Entity other = (pe == a) ? b : a;
        Player* pl = w.get<Player>(pe);
        if (!pl) continue;

        if (Enemy* en = w.get<Enemy>(other)) {
            Body*      pb = w.get<Body>(pe);
            Transform* pt = w.get<Transform>(pe);
            Transform* et = w.get<Transform>(other);
            const bool from_above = pb && pt && et && pb->vel.y > s_from_int(0)
                                    && pt->pos.y < et->pos.y;
            if (from_above && en->kind != EnemyKind::Thorn) {          // stomp kill
                if (et) spawn_burst(c, et->pos, 4);
                w.despawn(other);                       // safe: iterating hits, not the world
                pb->vel.y = -kStompBounce;
                ++c->stomps;
                c->scenes->persistent().set_int("score"_hash,
                    c->scenes->persistent().get_int("score"_hash) + kStompScore);
                sfx(c, c->snd_stomp, 0.9f);
                if (c->camera) c->camera->shake = 2;
            } else {                                    // side brush — or a spiny stomp
                hurt_player(c, pe, *pl, 1);
            }
        } else if (Hazard* hz = w.get<Hazard>(other)) {
            // Geyser columns only bite while erupting; lava always (and kills outright).
            if (Geyser* g = w.get<Geyser>(other); g && g->state != GeyserState::Erupt)
                continue;
            hurt_player(c, pe, *pl, hz->damage);
        }
    }
}

// ---- pickups + touch triggers (game-side, off the O(n²) physics pass) ----------------------
void trigger_system(EngineCtx* c, scalar /*dt*/) {
    ecs::World& w = *c->world;
    Transform* pt = w.get<Transform>(c->player);
    AABBColl*  pc = w.get<AABBColl>(c->player);
    Player*    pl = w.get<Player>(c->player);
    if (!pt || !pc || !pl) return;
    const aabb pbox = aabb::from_center(pt->pos, pc->half);
    Blackboard& bb = c->scenes->persistent();

    ecs::Entity taken[8]; uint32_t taken_n = 0;         // despawn AFTER the iteration
    w.each<Pickup, Transform>([&](ecs::Entity e, Pickup& p, Transform& t) {
        if (taken_n >= 8) return;
        if (!pbox.overlaps(aabb::from_center(t.pos, p.half))) return;
        switch (p.kind) {
            case PickupKind::Ember:
                ++c->embers;
                bb.set_int("score"_hash, bb.get_int("score"_hash) + kEmberScore);
                sfx(c, c->snd_ember);
                break;
            case PickupKind::Shard:
                ++c->shards;
                bb.set_int("score"_hash, bb.get_int("score"_hash) + kShardScore);
                sfx(c, c->snd_shard);
                spawn_burst(c, t.pos, 6);
                break;
            case PickupKind::Heart:
                if (pl->health >= kMaxHealth) return;   // full: leave the mercy for later
                ++pl->health;
                sfx(c, c->snd_heart);
                break;
        }
        taken[taken_n++] = e;
    });
    for (uint32_t i = 0; i < taken_n; ++i) w.despawn(taken[i]);

    // waystones: light once, move the respawn point forward
    w.each<Checkpoint, Transform>([&](ecs::Entity e, Checkpoint& cp, Transform& t) {
        if (cp.lit || !pbox.overlaps(aabb::from_center(t.pos, cp.half))) return;
        cp.lit = true;
        ++c->checkpoints;
        c->respawn = vec2{ t.pos.x, t.pos.y };
        if (Animator* an = w.get<Animator>(e)) an->play(kStoneLit);
        sfx(c, c->snd_checkpoint);
        spawn_burst(c, t.pos, 6);
    });

    // the Sungate: roll the tally exactly once
    w.each<Goal, Transform>([&](ecs::Entity, Goal& g, Transform& t) {
        if (c->cleared || !pbox.overlaps(aabb::from_center(t.pos, g.half))) return;
        c->cleared = true;
        bb.set_int("score"_hash, bb.get_int("score"_hash) + pl->health * kHpBonus);
        sfx(c, c->snd_goal);
        spawn_burst(c, t.pos, 6);
        if (c->best.clears < 255) ++c->best.clears;
        save_game(c);                                   // persist the best run
        c->scenes->push(make_clear_scene(c));
    });
}

// ---- particles ------------------------------------------------------------------------------
void spawn_burst(EngineCtx* c, vec2 pos, uint8_t n) {
    // Deterministic fan of spark velocities (px/s); gravity arcs them down.
    static const int16_t kVel[6][2] = {
        { -60, -150 }, { 60, -150 }, { -30, -100 }, { 30, -100 }, { -90, -50 }, { 90, -50 },
    };
    ecs::World& w = *c->world;
    if (n > 6) n = 6;
    for (uint8_t i = 0; i < n; ++i) {
        ecs::Entity e = w.spawn();
        w.add<Transform>(e, { pos });
        Body b{}; b.vel = vec2{ s_from_int(kVel[i][0]), s_from_int(kVel[i][1]) };
        w.add<Body>(e, b);                              // no AABBColl: falls through, TTL reaps
        w.add<Particle>(e, {});
        SpriteRef sr{}; sr.tex = c->spark_tex; sr.sw = 8; sr.sh = 8; sr.layer = 4;
        w.add<SpriteRef>(e, sr);
    }
}

void particle_system(EngineCtx* c, scalar /*dt*/) {
    ecs::World& w = *c->world;
    ecs::Entity dead[16]; uint32_t dead_n = 0;
    w.each<Particle>([&](ecs::Entity e, Particle& p) {
        if (--p.ttl <= 0 && dead_n < 16) dead[dead_n++] = e;
    });
    for (uint32_t i = 0; i < dead_n; ++i) w.despawn(dead[i]);
}

// ---- animation -----------------------------------------------------------------------------
void animation_system(EngineCtx* c, scalar dt) {
    static AnimationSystem sys;
    sys.tick(*c->world, dt);
    c->world->each<Animator, SpriteRef>([&](ecs::Entity, Animator& a, SpriteRef& s) {
        s.sx = a.cur_sx; s.sy = a.cur_sy; s.sw = a.cur_sw; s.sh = a.cur_sh;
    });
}

// ---- camera --------------------------------------------------------------------------------
// Look-ahead follow: the view leads the player's facing by 28 px and sits 8 px high, easing
// 1/8th of the remaining distance per step (integer math, so both tiers move the same pixel).
void camera_system(EngineCtx* c, scalar /*dt*/) {
    Transform* t = c->world->get<Transform>(c->player);
    Player*    pl = c->world->get<Player>(c->player);
    if (!t || !pl) return;
    const int sw = c->app->config().width, sh = c->app->config().height;

    int tx = s_to_int(t->pos.x) + (pl->facing_left ? -28 : 28) - sw / 2;
    int ty = s_to_int(t->pos.y) - 8 - sh / 2;
    const int maxx = c->level_w - sw > 0 ? c->level_w - sw : 0;
    const int maxy = c->level_h - sh > 0 ? c->level_h - sh : 0;
    tx = tx < 0 ? 0 : (tx > maxx ? maxx : tx);
    ty = ty < 0 ? 0 : (ty > maxy ? maxy : ty);

    int dx = tx - c->cam_x, dy = ty - c->cam_y;
    c->cam_x += dx / 8 + (dx / 8 == 0 && dx != 0 ? (dx > 0 ? 1 : -1) : 0);
    c->cam_y += dy / 8 + (dy / 8 == 0 && dy != 0 ? (dy > 0 ? 1 : -1) : 0);

    c->camera->pos = vec2{ s_from_int(c->cam_x), s_from_int(c->cam_y) };
    if (c->camera->shake > 0) --c->camera->shake;       // decay the impact shake
}

// ---- persistence -----------------------------------------------------------------------------
// Best-run snapshot through the platform save seam (file on PC/PSP, battery SRAM on GBA).
void save_game(EngineCtx* c) {
    const phx_platform* plat = c->app->platform();
    if (!plat->save) return;
    const int run = int(c->scenes->persistent().get_int("score"_hash));
    if (run > int(c->best.best_score)) c->best.best_score = uint16_t(run);
    if (c->shards > c->best.best_shards) c->best.best_shards = uint8_t(c->shards);
    c->best.magic = kSaveMagic; c->best.version = kSaveVersion;
    plat->save(c->save_path, &c->best, uint32_t(sizeof c->best));
}

bool load_game(EngineCtx* c) {
    const phx_platform* plat = c->app->platform();
    if (!plat->load) return false;
    SaveData sd{};
    uint32_t got = 0;
    if (plat->load(c->save_path, &sd, uint32_t(sizeof sd), &got) != 0) return false;
    if (got < sizeof sd || sd.magic != kSaveMagic || sd.version != kSaveVersion) return false;
    c->best = sd;
    c->loaded = true;
    return true;
}

} // namespace game
