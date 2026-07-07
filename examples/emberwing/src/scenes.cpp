// examples/emberwing/scenes.cpp — the four scenes (Title → Level ⇄ Pause, Level → Clear) over
// the engine SceneStack, the sprite draw pass (camera-culled), the HUD, and the EmberwingGame
// composition root that mounts the bundle and wires every subsystem. Engine APIs only; this
// TU compiles unchanged for every target.
#include "game.h"
#include "phx/core/caps.h"
#include "phx/core/log.h"

namespace game {

using namespace tune;

// ---- asset helpers ------------------------------------------------------------------------
namespace {

TextureId load_tex(EngineCtx* c, NameHash h) {
    auto r = c->res->texture(h);
    if (!r) return kNoTexture;
    TextureView v = r.unwrap();
    TextureDesc d{}; d.pixels = v.pixels; d.width = v.width; d.height = v.height;
    return c->render->load_texture(d);
}

// Resolve a baked Sprite asset: upload its texture, copy the frame grid + clip table.
SpriteAsset load_sprite(EngineCtx* c, NameHash h) {
    SpriteAsset a{};
    a.tex = load_tex(c, h);
    if (auto sr = c->res->sprite(h); sr.ok()) {
        SpriteView sv = sr.unwrap();
        a.sheet = SpriteSheet{ sv.frame_w, sv.frame_h, sv.cols };
        a.clip_count = sv.clip_count < 6 ? sv.clip_count : 6;
        for (uint16_t i = 0; i < a.clip_count; ++i)
            a.clips[i] = AnimClip{ sv.clips[i].first, sv.clips[i].count,
                                   sv.clips[i].fps, sv.clips[i].loop != 0 };
    }
    return a;
}

SoundView load_snd(EngineCtx* c, NameHash h) {
    if (auto r = c->res->sound(h); r.ok()) {
        auto v = r.unwrap();
        return SoundView{ v.samples, v.frames, v.rate };
    }
    return SoundView{};
}

// Attach an Animator playing `clip` from a resolved Sprite asset.
void attach_anim(ecs::World& w, ecs::Entity e, const SpriteAsset& a, uint16_t clip) {
    Animator an{};
    an.clips = Span<const AnimClip>{ a.clips, a.clip_count };
    an.sheet = a.sheet;
    an.play(clip);
    AnimationSystem::apply_rect(an);          // seed the source rect before the first tick
    w.add<Animator>(e, an);
}

// Despawn every entity (fresh run). All game entities carry a Transform; each() iterates
// backwards, so despawning the current entity is swap-safe.
void clear_world(ecs::World& w) {
    w.each<Transform>([&](ecs::Entity e, Transform&) { w.despawn(e); });
}

// UI positions are WORLD coordinates (sprites all go through the camera), so screen-fixed
// HUD/menus anchor to the camera: screen (x, y) -> camera + (x, y).
inline vec2 scr(EngineCtx* c, int x, int y) {
    return vec2{ c->camera->pos.x + s_from_int(x), c->camera->pos.y + s_from_int(y) };
}

// The world sprite pass: camera-culled (the level holds ~90 drawables but only ~15 are in
// view — culling keeps the frame under the 128-sprite tier-0 ceiling alongside the HUD),
// honouring `hidden` (idle geysers). The player's i-frames blink by SKIPPING the draw on
// alternate 4-frame windows — a tint would be invisible on the GBA PPU (OBJs have no
// per-channel multiply), so the blink is expressed in a way every backend can show.
void draw_world_sprites(EngineCtx* c) {
    Renderer& r = *c->render;
    const int sw = c->app->config().width, sh = c->app->config().height;
    const int cx0 = c->cam_x - 32, cy0 = c->cam_y - 40;
    const int cx1 = c->cam_x + sw + 32, cy1 = c->cam_y + sh + 40;
    const bool blink_off = (c->app->frame() & 4) != 0;   // 4-on/4-off i-frame flash

    c->world->each<SpriteRef, Transform>([&](ecs::Entity e, SpriteRef& s, Transform& t) {
        if (s.hidden) return;
        const int px = s_to_int(t.pos.x), py = s_to_int(t.pos.y);
        if (px < cx0 || px > cx1 || py < cy0 || py > cy1) return;
        if (Player* pl = c->world->get<Player>(e); pl && pl->invuln > 0 && blink_off)
            return;

        DrawSprite ds{};
        ds.tex = s.tex; ds.sx = s.sx; ds.sy = s.sy; ds.sw = s.sw; ds.sh = s.sh;
        ds.pos = vec2{ t.pos.x - s_from_int(s.sw / 2 - s.ox),
                       t.pos.y - s_from_int(s.sh / 2 - s.oy) };
        ds.flags = s.flags; ds.layer = s.layer; ds.z = 0;
        r.draw_sprite(ds);
    });
}

} // namespace

// ---- level scene ----------------------------------------------------------------------------
class LevelScene final : public Scene {
    UI hud_;

    // One entity per baked SpawnDef. Positions are authored as exact centres by the level
    // generator (which knows every sprite's size), so spawning stays dumb data -> components.
    void spawn_entity(EngineCtx* c, const SpawnDef& sp) {
        ecs::World& w = *c->world;
        const vec2 pos{ s_from_int(sp.x), s_from_int(sp.y) };

        if (sp.type == "player"_hash) {
            ecs::Entity p = w.spawn();
            w.add<Transform>(p, { pos });
            w.add<Body>(p, {});
            w.add<AABBColl>(p, { vec2{ s_from_int(5), s_from_int(7) }, kLayerPlayer,
                                 uint16_t(kLayerEnemy | kLayerHazard) });
            w.add<Player>(p, {});
            SpriteRef sr{}; sr.tex = c->hero.tex; sr.layer = 3; sr.oy = -1;
            w.add<SpriteRef>(p, sr);
            attach_anim(w, p, c->hero, kHeroIdle);
            c->player = p;
            c->respawn = pos;
        } else if (sp.type == "ember"_hash || sp.type == "shard"_hash
                   || sp.type == "heart"_hash) {
            ecs::Entity e = w.spawn();
            w.add<Transform>(e, { pos });
            Pickup pk{};
            pk.half = vec2{ s_from_int(4), s_from_int(4) };
            const SpriteAsset* art = &c->ember;
            if      (sp.type == "shard"_hash) { pk.kind = PickupKind::Shard; art = &c->shard; }
            else if (sp.type == "heart"_hash) { pk.kind = PickupKind::Heart; art = &c->heart; }
            w.add<Pickup>(e, pk);
            SpriteRef sr{}; sr.tex = art->tex; sr.sw = 8; sr.sh = 8; sr.layer = 1;
            w.add<SpriteRef>(e, sr);
            if (art->clip_count) attach_anim(w, e, *art, 0);
        } else if (sp.type == "cinder"_hash || sp.type == "thorn"_hash) {
            const bool thorn = sp.type == "thorn"_hash;
            ecs::Entity e = w.spawn();
            w.add<Transform>(e, { pos });
            Body b{}; b.vel.x = thorn ? -kThornRun : -kCinderRun;   // seed the patrol
            w.add<Body>(e, b);
            w.add<AABBColl>(e, { vec2{ s_from_int(6), s_from_int(6) }, kLayerEnemy,
                                 kLayerPlayer });
            Enemy en{}; en.kind = thorn ? EnemyKind::Thorn : EnemyKind::Cinder;
            en.home = pos; en.range = s_from_int(24);
            w.add<Enemy>(e, en);
            const SpriteAsset& art = thorn ? c->thorn : c->cinder;
            SpriteRef sr{}; sr.tex = art.tex; sr.layer = 2; sr.oy = -2;
            w.add<SpriteRef>(e, sr);
            attach_anim(w, e, art, 0);
        } else if (sp.type == "wisp"_hash) {
            ecs::Entity e = w.spawn();
            w.add<Transform>(e, { pos });
            Body b{}; b.flags = kBodyNoGravity;
            w.add<Body>(e, b);
            w.add<AABBColl>(e, { vec2{ s_from_int(6), s_from_int(5) }, kLayerEnemy,
                                 kLayerPlayer });
            Enemy en{}; en.kind = EnemyKind::Wisp; en.home = pos; en.range = s_from_int(20);
            w.add<Enemy>(e, en);
            SpriteRef sr{}; sr.tex = c->wisp.tex; sr.layer = 2;
            w.add<SpriteRef>(e, sr);
            attach_anim(w, e, c->wisp, kWispFloat);
        } else if (sp.type == "geyser"_hash) {
            ecs::Entity e = w.spawn();
            w.add<Transform>(e, { pos });
            w.add<AABBColl>(e, { vec2{ s_from_int(6), s_from_int(14) }, kLayerHazard,
                                 kLayerPlayer });
            w.add<Hazard>(e, { 1 });
            // Phase-offset by position so geyser corridors erupt as a travelling wave.
            Geyser g{}; g.timer = int16_t((sp.x * 5 / 8) % kGeyserCycle);
            w.add<Geyser>(e, g);
            SpriteRef sr{}; sr.tex = c->geyser.tex; sr.sw = 16; sr.sh = 32; sr.layer = 1;
            sr.hidden = true;
            w.add<SpriteRef>(e, sr);
            attach_anim(w, e, c->geyser, kGeyserWarnClip);
        } else if (sp.type == "lava"_hash) {
            ecs::Entity e = w.spawn();
            w.add<Transform>(e, { pos });
            w.add<AABBColl>(e, { vec2{ s_from_int(sp.w / 2), s_from_int(sp.h / 2) },
                                 kLayerHazard, kLayerPlayer });
            w.add<Hazard>(e, { int16_t(kMaxHealth) });   // lava kills outright
        } else if (sp.type == "waystone"_hash) {
            ecs::Entity e = w.spawn();
            w.add<Transform>(e, { pos });
            Checkpoint cp{}; cp.half = vec2{ s_from_int(8), s_from_int(12) };
            w.add<Checkpoint>(e, cp);
            // 16×32 frame (a real GBA OBJ shape); the stone art sits in the lower rows,
            // so lift the visual 2 px to plant its base on the ground.
            SpriteRef sr{}; sr.tex = c->waystone.tex; sr.sw = 16; sr.sh = 32; sr.layer = 1;
            sr.oy = -2;
            w.add<SpriteRef>(e, sr);
            attach_anim(w, e, c->waystone, kStoneDormant);
        } else if (sp.type == "gate"_hash) {
            ecs::Entity e = w.spawn();
            w.add<Transform>(e, { pos });
            Goal g{}; g.half = vec2{ s_from_int(12), s_from_int(16) };
            w.add<Goal>(e, g);
            SpriteRef sr{}; sr.tex = c->gate.tex; sr.sw = 32; sr.sh = 32; sr.layer = 1;
            w.add<SpriteRef>(e, sr);
            attach_anim(w, e, c->gate, 0);
        }
    }

public:
    void on_enter(EngineCtx* c) override {
        ecs::World& w = *c->world;
        clear_world(w);                                  // fresh run
        c->embers = c->shards = c->stomps = c->checkpoints = 0;
        c->cleared = false;
        c->scenes->persistent().set_int("score"_hash, 0);

        TilemapView tv = c->res->tilemap(kLevelMap).unwrap();
        if (c->map == kNoTilemap) {                      // upload once; re-entry reuses it
            TilemapDesc d{};
            d.indices = tv.indices; d.width = tv.width; d.height = tv.height;
            d.layers = tv.layers; d.tile_w = tv.tile_w; d.tile_h = tv.tile_h;
            d.tileset = c->tiles_tex;
            c->map = c->render->upload_tilemap(d);
            c->map_layers = tv.layers;
            // Per-layer parallax baked from Tiled's parallaxx/parallaxy (Q16 in the view).
            if (tv.parallax_q16)
                for (uint8_t l = 0; l < tv.layers; ++l)
                    c->render->set_tilemap_parallax(c->map, l,
                                                    s_from_q16(tv.parallax_q16[l * 2 + 0]),
                                                    s_from_q16(tv.parallax_q16[l * 2 + 1]));
        }

        // Collision reads the LAST tile layer — backdrops first, gameplay/solid layer last.
        TileGrid g;
        g.tiles = tv.indices + size_t(tv.layers - 1) * tv.width * tv.height;
        g.w = tv.width; g.h = tv.height;
        g.tile_w = tv.tile_w; g.tile_h = tv.tile_h; g.solid_from = 1;
        c->physics->set_tilemap(g);
        c->physics->set_gravity(vec2{ s_from_int(0), kGravity });
        c->level_w = tv.width * tv.tile_w; c->level_h = tv.height * tv.tile_h;

        auto spr = c->res->spawns(kLevelMap);
        if (spr.ok()) {
            SpawnsView sv = spr.unwrap();
            for (uint32_t i = 0; i < sv.count; ++i) spawn_entity(c, sv.spawns[i]);
        }

        // Snap the camera onto the player so the level doesn't open with a pan.
        if (Transform* t = c->world->get<Transform>(c->player)) {
            const int sw = c->app->config().width, sh = c->app->config().height;
            const int maxx = c->level_w - sw > 0 ? c->level_w - sw : 0;
            const int maxy = c->level_h - sh > 0 ? c->level_h - sh : 0;
            int px = s_to_int(t->pos.x) + 28 - sw / 2;
            int py = s_to_int(t->pos.y) - 8 - sh / 2;
            c->cam_x = px < 0 ? 0 : (px > maxx ? maxx : px);
            c->cam_y = py < 0 ? 0 : (py > maxy ? maxy : py);
            c->camera->pos = vec2{ s_from_int(c->cam_x), s_from_int(c->cam_y) };
        }
        PHX_LOG_INFO("LevelScene ready: %u entities", c->world->count());
    }

    void update(EngineCtx* c, scalar dt) override {
        player_system(c, dt);
        enemy_system(c, dt);
        geyser_system(c, dt);
        physics_system(c, dt);
        interaction_system(c, dt);
        trigger_system(c, dt);
        particle_system(c, dt);
        animation_system(c, dt);
        camera_system(c, dt);
        if (c->input->just(Button::Start))  { save_game(c); c->scenes->push(make_pause_scene(c)); }
        if (c->input->just(Button::Select)) c->show_profiler = !c->show_profiler;
    }

    void render(EngineCtx* c, scalar) override {
        Renderer& r = *c->render;
        for (uint8_t l = 0; l < c->map_layers; ++l)     // sky, crags, deco, solid — in order
            r.draw_tilemap(c->map, l);
        draw_world_sprites(c);

        // HUD: hearts, ember count, shard pips. Camera-anchored (scr); lost hearts and
        // uncollected shards use dedicated ART frames (hollow heart, dim pip) rather than
        // tint — the GBA PPU's OBJs cannot tint, so the state must live in the pixels.
        hud_.begin(r, *c->input);
        Player* pl = c->world->get<Player>(c->player);
        const int hp = pl ? pl->health : 0;
        for (int i = 0; i < kMaxHealth; ++i)
            hud_.image(UIRect{ scr(c, 2 + i * 9, 2), vec2{ s_from_int(8), s_from_int(8) } },
                       c->heart.tex, i < hp ? 0 : 16, 0, 8, 8);
        hud_.image(UIRect{ scr(c, 2, 12), vec2{ s_from_int(8), s_from_int(8) } },
                   c->ember.tex, 0, 0, 8, 8);
        char buf[16]; fmt_uint(buf, c->embers);
        hud_.text(scr(c, 12, 12), *c->font, buf, rgba(255, 220, 120));
        for (int i = 0; i < 3; ++i)
            hud_.image(UIRect{ scr(c, 2 + i * 9, 22), vec2{ s_from_int(8), s_from_int(8) } },
                       c->shard.tex, i < int(c->shards) ? 0 : 32, 0, 8, 8);
        if (c->show_profiler)
            hud_.profile_overlay(scr(c, 2, 32), c->app->profile(), c->font);
        hud_.end();
    }
};

// ---- title ----------------------------------------------------------------------------------
class TitleScene final : public Scene {
    UI ui_;
public:
    void render(EngineCtx* c, scalar) override {
        // Laid out for the SMALLEST view (the GBA soft build is 120x80): everything is
        // anchored off h/2 and the bottom edge so no two lines can collide at any height.
        const int w = c->app->config().width, h = c->app->config().height;
        ui_.begin(*c->render, *c->input);
        // the Sungate, dormant, as the backdrop promise of the goal
        ui_.image(UIRect{ scr(c, w / 2 - 16, h / 2 - 30),
                          vec2{ s_from_int(32), s_from_int(32) } },
                  c->gate.tex, 0, 0, 32, 32);
        ui_.text(scr(c, w / 2 - 36, h / 2 + 4), *c->font, "EMBERWING", rgba(255, 200, 90));
        ui_.text(scr(c, w / 2 - 52, h / 2 + 14), *c->font, "CINDER HOLLOW",
                 rgba(200, 140, 120));
        // "BY JADEN HAIWYRE" — the pre-baked 2× strip (bake.h), drawn in 32×16 chunks: the
        // widest OBJ-mappable slice on the GBA PPU (gold is baked in; OBJs cannot tint).
        // Views narrower than the 224px strip (the 120×80 GBA soft build) skip it.
        if (c->credit != kNoTexture && w >= 224) {
            const int cx = w / 2 - 112, cy = h / 2 + 28;
            for (int i = 0; i < 7; ++i)
                ui_.image(UIRect{ scr(c, cx + i * 32, cy),
                                  vec2{ s_from_int(32), s_from_int(16) } },
                          c->credit, i * 32, 0, 32, 16);
        }
        ui_.text(scr(c, w / 2 - 44, h - 12), *c->font, "PRESS START", rgba(230, 230, 240));
        if (c->best.best_score > 0) {
            char buf[24]; int n = 0;
            for (const char* s = "BEST "; *s; ++s) buf[n++] = *s;
            fmt_uint(buf + n, c->best.best_score);
            ui_.text(scr(c, w / 2 - 20, 2), *c->font, buf, rgba(150, 150, 170));
        }
        ui_.end();
    }
    void update(EngineCtx* c, scalar) override {
        if (c->input->just(Button::Start) || c->input->just(Button::A))
            c->scenes->replace(make_level_scene(c));
    }
};

// ---- pause ----------------------------------------------------------------------------------
class PauseScene final : public Scene {
    UI ui_;
public:
    PauseScene() { render_below = true; }   // gameplay stays visible, frozen, behind us
    void render(EngineCtx* c, scalar) override {
        const int w = c->app->config().width, h = c->app->config().height;
        const int px = w / 2 - 34, py = h / 2 - 22;
        ui_.begin(*c->render, *c->input);
        ui_.rect(UIRect{ scr(c, px, py), vec2{ s_from_int(68), s_from_int(44) } },
                 rgba(16, 10, 24));
        ui_.text(scr(c, px + 10, py + 4), *c->font, "PAUSED", rgba(255, 255, 255));
        if (ui_.button(UIRect{ scr(c, px + 6, py + 16),
                               vec2{ s_from_int(56), s_from_int(10) } }, *c->font, "RESUME"))
            c->scenes->pop();
        if (ui_.button(UIRect{ scr(c, px + 6, py + 30),
                               vec2{ s_from_int(56), s_from_int(10) } }, *c->font, "QUIT"))
            { save_game(c); c->app->request_quit(); }
        ui_.end();
    }
    void update(EngineCtx* c, scalar) override {
        if (c->input->just(Button::Start)) c->scenes->pop();   // Start also resumes
    }
};

// ---- level clear ------------------------------------------------------------------------------
class ClearScene final : public Scene {
    UI ui_;
    int16_t hold_ = 30;   // swallow input briefly so the goal touch doesn't skip the tally
public:
    ClearScene() { render_below = true; }
    void render(EngineCtx* c, scalar) override {
        const int w = c->app->config().width, h = c->app->config().height;
        const int px = w / 2 - 56, py = h / 2 - 34;
        ui_.begin(*c->render, *c->input);
        ui_.rect(UIRect{ scr(c, px, py), vec2{ s_from_int(112), s_from_int(68) } },
                 rgba(20, 12, 30));
        ui_.text(scr(c, px + 4, py + 4), *c->font, "SUNGATE REKINDLED", rgba(255, 220, 110));

        char buf[24]; int n;
        n = 0; for (const char* s = "EMBERS "; *s; ++s) buf[n++] = *s;
        fmt_uint(buf + n, c->embers);
        ui_.text(scr(c, px + 4, py + 18), *c->font, buf, rgba(230, 230, 240));
        n = 0; for (const char* s = "SHARDS "; *s; ++s) buf[n++] = *s;
        n += fmt_uint(buf + n, c->shards);
        buf[n++] = '-'; buf[n++] = '3'; buf[n] = 0;   // the debug font has no '/'
        ui_.text(scr(c, px + 4, py + 28), *c->font, buf, rgba(160, 220, 255));
        n = 0; for (const char* s = "SCORE "; *s; ++s) buf[n++] = *s;
        fmt_uint(buf + n, unsigned(c->scenes->persistent().get_int("score"_hash)));
        ui_.text(scr(c, px + 4, py + 38), *c->font, buf, rgba(255, 255, 255));
        n = 0; for (const char* s = "BEST "; *s; ++s) buf[n++] = *s;
        fmt_uint(buf + n, c->best.best_score);
        ui_.text(scr(c, px + 4, py + 48), *c->font, buf, rgba(150, 150, 170));
        if (hold_ <= 0)
            ui_.text(scr(c, px + 4, py + 58), *c->font, "PRESS START", rgba(255, 200, 90));
        ui_.end();
    }
    void update(EngineCtx* c, scalar) override {
        if (hold_ > 0) { --hold_; return; }
        if (c->input->just(Button::Start) || c->input->just(Button::A))
            c->scenes->replace(make_title_scene(c));   // replaces the WHOLE run: pop to title
    }
};

Scene* make_level_scene(EngineCtx* c) { return c->app->mem().persistent().make<LevelScene>(); }
Scene* make_title_scene(EngineCtx* c) { return c->app->mem().persistent().make<TitleScene>(); }
Scene* make_pause_scene(EngineCtx* c) { return c->app->mem().persistent().make<PauseScene>(); }
Scene* make_clear_scene(EngineCtx* c) { return c->app->mem().persistent().make<ClearScene>(); }

// ---- composition root -------------------------------------------------------------------------
void EmberwingGame::on_start(App& app) {
    ArenaAllocator& A = app.mem().persistent();
    res = ResourceCache::create(A).unwrap();
    if (res->mount(app.platform(), bundle) != Status::Ok)
        PHX_LOG_ERROR("emberwing: failed to mount '%s'", bundle);

    void* buf = A.alloc(scene_scratch_bytes);
    scene_scratch.init(buf, scene_scratch_bytes);
    scenes = SceneStack::create(A, scene_scratch).unwrap();

    ctx.app = &app; ctx.world = &app.world(); ctx.render = &app.render();
    ctx.input = &app.input(); ctx.scenes = scenes; ctx.physics = &physics;
    ctx.res = res; ctx.camera = &camera; ctx.font = &font;

    // sprite assets + loose textures
    ctx.hero     = load_sprite(&ctx, "hero"_hash);
    ctx.cinder   = load_sprite(&ctx, "cinder"_hash);
    ctx.thorn    = load_sprite(&ctx, "thorn"_hash);
    ctx.wisp     = load_sprite(&ctx, "wisp"_hash);
    ctx.geyser   = load_sprite(&ctx, "geyser"_hash);
    ctx.ember    = load_sprite(&ctx, "ember"_hash);
    ctx.shard    = load_sprite(&ctx, "shard"_hash);
    ctx.heart    = load_sprite(&ctx, "heart"_hash);
    ctx.waystone = load_sprite(&ctx, "waystone"_hash);
    ctx.gate     = load_sprite(&ctx, "gate"_hash);
    ctx.tiles_tex = load_tex(&ctx, kTilesTex);
    ctx.spark_tex = load_tex(&ctx, "spark"_hash);
    font.tex = load_tex(&ctx, "font"_hash);
    font.glyph_w = 8; font.glyph_h = 8; font.cols = 16; font.first_char = 32;
    font.advance = 8; font.line_h = 8;
    ctx.credit = load_tex(&ctx, "credit"_hash);    // the 2× title credit strip

    // audio: mixer at the entry point's device rate + the SPSC intent queue
    mixer = AudioMixer::create(A, caps(), mixer_rate).unwrap();
    queue.init(queue_storage, 32);
    ctx.mixer = mixer; ctx.queue = &queue;
    ctx.snd_jump       = load_snd(&ctx, "jump"_hash);
    ctx.snd_ember      = load_snd(&ctx, "ember_sfx"_hash);
    ctx.snd_stomp      = load_snd(&ctx, "stomp"_hash);
    ctx.snd_hurt       = load_snd(&ctx, "hurt"_hash);
    ctx.snd_checkpoint = load_snd(&ctx, "checkpoint"_hash);
    ctx.snd_shard      = load_snd(&ctx, "shard_sfx"_hash);
    ctx.snd_heart      = load_snd(&ctx, "heart_sfx"_hash);
    ctx.snd_goal       = load_snd(&ctx, "goal"_hash);
    ctx.snd_geyser     = load_snd(&ctx, "geyser_sfx"_hash);
    ctx.music          = load_snd(&ctx, "music"_hash);
    if (ctx.music.samples) queue.play_music(ctx.music, 0.55f, true);

    ctx.save_path = save_path;
    load_game(&ctx);                        // restore best-run stats if the store has them
    scenes->push(make_title_scene(&ctx));
}

void EmberwingGame::on_fixed_update(App&, scalar dt) { scenes->update(&ctx, dt); }

void EmberwingGame::on_render(App& app, scalar alpha) {
    Renderer& r = app.render();
    r.begin_frame(camera);
    scenes->render(&ctx, alpha);
    r.end_frame();

    // Headless audio pump: when no platform device owns the mixer, drain the game's intents
    // and mix a block here (single-threaded), tracking the peak so tests can assert output.
    if (!device_audio && mixer) {
        static int16_t mixbuf[128 * 2];
        queue.drain(*mixer);
        mixer->mix(mixbuf, 128);
        for (int i = 0; i < 128 * 2; ++i) {
            int32_t a = mixbuf[i] < 0 ? -mixbuf[i] : mixbuf[i];
            if (a > audio_peak) audio_peak = a;
        }
    }
}

// ---- read-only queries (tests) -----------------------------------------------------------------
int EmberwingGame::score() const {
    return scenes ? int(scenes->persistent().get_int("score"_hash)) : -1;
}
int EmberwingGame::player_x() const {
    if (!ctx.world || ctx.player == ecs::kInvalid) return -1;
    Transform* t = ctx.world->get<Transform>(ctx.player);
    return t ? s_to_int(t->pos.x) : -1;
}
int EmberwingGame::player_y() const {
    if (!ctx.world || ctx.player == ecs::kInvalid) return -1;
    Transform* t = ctx.world->get<Transform>(ctx.player);
    return t ? s_to_int(t->pos.y) : -1;
}
int EmberwingGame::health() const {
    if (!ctx.world || ctx.player == ecs::kInvalid) return -1;
    Player* p = ctx.world->get<Player>(ctx.player);
    return p ? int(p->health) : -1;
}
uint32_t EmberwingGame::depth() const { return scenes ? scenes->depth() : 0; }

} // namespace game
