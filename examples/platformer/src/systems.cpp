// examples/platformer/systems.cpp — the example's logic: gameplay systems, the three scenes
// (title → level → pause), and the PlatformerGame composition root. Uses ONLY engine APIs;
// compiles unchanged for every target. No platform headers, no STL in the game path.
#include "components.h"
#include "phx/core/caps.h"
#include "phx/core/log.h"

namespace game {

// ---- tuning ---------------------------------------------------------------------------
static const scalar kRun     = s_from_int(70);    // px/s
static const scalar kJump    = s_from_int(200);   // px/s (upward)
static const scalar kGravity = s_from_int(600);   // px/s^2

// ---- systems --------------------------------------------------------------------------
void player_system(EngineCtx* c, scalar /*dt*/) {
    const InputState& in = *c->input;
    ecs::World& w = *c->world;
    w.each<Player, Body, Transform>([&](ecs::Entity e, Player& pl, Body& b, Transform&) {
        scalar vx = s_from_int(0);
        if (in.down(Button::Right))      { vx =  kRun; pl.facing_left = false; }
        else if (in.down(Button::Left))  { vx = -kRun; pl.facing_left = true;  }
        b.vel.x = vx;
        if (in.just(Button::A) && b.on_ground) {                   // jump
            b.vel.y = -kJump;
            if (c->mixer && c->jump_snd.samples) { c->mixer->play_sfx(c->jump_snd, 0.7f); ++c->sfx_triggers; }
        }

        const uint16_t want = (vx != s_from_int(0)) ? uint16_t(kWalk) : uint16_t(kIdle);
        if (Animator* an = w.get<Animator>(e); an && an->clip != want) an->play(want);
        if (SpriteRef* sr = w.get<SpriteRef>(e)) {
            if (pl.facing_left) sr->flags |= kFlipX; else sr->flags &= uint16_t(~kFlipX);
        }
    });
}

void physics_system(EngineCtx* c, scalar dt) {
    c->hit_count = c->physics->step(*c->world, dt, Span<Hit>{ c->hits, 64 });
}

void interaction_system(EngineCtx* c, scalar /*dt*/) {
    ecs::World& w = *c->world;
    for (uint32_t i = 0; i < c->hit_count; ++i) {
        ecs::Entity a = c->hits[i].a, b = c->hits[i].b;
        ecs::Entity coin = ecs::kInvalid;
        if (w.has<Player>(a) && w.has<Coin>(b)) coin = b;
        else if (w.has<Player>(b) && w.has<Coin>(a)) coin = a;
        if (coin == ecs::kInvalid) continue;
        Blackboard& bb = c->scenes->persistent();
        bb.set_int("score"_hash, bb.get_int("score"_hash) + w.get<Coin>(coin)->value);
        if (c->mixer && c->coin_snd.samples) { c->mixer->play_sfx(c->coin_snd); ++c->sfx_triggers; }
        w.despawn(coin);                       // safe: not iterating the world here
    }
}

void animation_system(EngineCtx* c, scalar dt) {
    static AnimationSystem sys;
    sys.tick(*c->world, dt);
    c->world->each<Animator, SpriteRef>([&](ecs::Entity, Animator& a, SpriteRef& s) {
        s.sx = a.cur_sx; s.sy = a.cur_sy; s.sw = a.cur_sw; s.sh = a.cur_sh;
    });
}

void camera_system(EngineCtx* c, scalar /*dt*/) {
    Transform* t = c->world->get<Transform>(c->player);
    if (!t) return;
    const int sw = c->app->config().width, sh = c->app->config().height;
    int px = s_to_int(t->pos.x) - sw / 2, py = s_to_int(t->pos.y) - sh / 2;
    const int maxx = c->level_w - sw > 0 ? c->level_w - sw : 0;
    const int maxy = c->level_h - sh > 0 ? c->level_h - sh : 0;
    px = px < 0 ? 0 : (px > maxx ? maxx : px);
    py = py < 0 ? 0 : (py > maxy ? maxy : py);
    c->camera->pos = vec2{ s_from_int(px), s_from_int(py) };
}

// ---- helpers --------------------------------------------------------------------------
namespace {
TextureId load_tex(EngineCtx* c, NameHash h) {
    auto r = c->res->texture(h);
    if (!r) return kNoTexture;
    TextureView v = r.unwrap();
    TextureDesc d{}; d.pixels = v.pixels; d.width = v.width; d.height = v.height;
    return c->render->load_texture(d);
}

void draw_world_sprites(EngineCtx* c) {
    Renderer& r = *c->render;
    c->world->each<SpriteRef, Transform>([&](ecs::Entity, SpriteRef& s, Transform& t) {
        DrawSprite ds{};
        ds.tex = s.tex; ds.sx = s.sx; ds.sy = s.sy; ds.sw = s.sw; ds.sh = s.sh;
        ds.pos = vec2{ t.pos.x - s_from_int(s.sw / 2), t.pos.y - s_from_int(s.sh / 2) };
        ds.flags = s.flags; ds.layer = s.layer; ds.z = 0;
        r.draw_sprite(ds);
    });
}
} // namespace

// ---- scenes ---------------------------------------------------------------------------
class LevelScene final : public Scene {
    UI hud_;
public:
    void on_enter(EngineCtx* c) override {
        ecs::World& w = *c->world;

        TilemapView tv = c->res->tilemap(kLevelMap).unwrap();
        TilemapDesc d{};
        d.indices = tv.indices; d.width = tv.width; d.height = tv.height; d.layers = tv.layers;
        d.tile_w = tv.tile_w; d.tile_h = tv.tile_h; d.tileset = c->tiles_tex;
        c->map = c->render->upload_tilemap(d);

        TileGrid g; g.tiles = tv.indices; g.w = tv.width; g.h = tv.height;
        g.tile_w = tv.tile_w; g.tile_h = tv.tile_h; g.solid_from = 1;
        c->physics->set_tilemap(g);
        c->physics->set_gravity(vec2{ s_from_int(0), kGravity });
        c->level_w = tv.width * tv.tile_w; c->level_h = tv.height * tv.tile_h;

        // entities come from the Spawns table (authored as a Tiled object group, baked offline)
        auto spr = c->res->spawns(kLevelMap);
        if (spr.ok()) {
            SpawnsView sv = spr.unwrap();
            for (uint32_t i = 0; i < sv.count; ++i) {
                const SpawnDef& sp = sv.spawns[i];
                const vec2 pos{ s_from_int(sp.x), s_from_int(sp.y) };
                if (sp.type == "player"_hash) {
                    ecs::Entity p = w.spawn();
                    w.add<Transform>(p, { pos });
                    w.add<Body>(p, {});
                    w.add<AABBColl>(p, { vec2{ s_from_int(4), s_from_int(4) }, kLayerPlayer, kLayerCoin });
                    w.add<Player>(p, {});
                    SpriteRef sr{}; sr.tex = c->hero_tex; sr.sw = 8; sr.sh = 8; sr.layer = 2;
                    w.add<SpriteRef>(p, sr);
                    Animator an{};                                  // clips/sheet from the Sprite asset
                    an.clips = Span<const AnimClip>{ c->hero_clips, c->hero_clip_count };
                    an.sheet = c->hero_sheet; an.play(kIdle);
                    w.add<Animator>(p, an);
                    c->player = p;
                } else if (sp.type == "coin"_hash) {
                    ecs::Entity e = w.spawn();
                    w.add<Transform>(e, { pos });
                    w.add<AABBColl>(e, { vec2{ s_from_int(4), s_from_int(4) }, kLayerCoin, kLayerPlayer });
                    w.add<Coin>(e, {});
                    SpriteRef cs{}; cs.tex = c->coin_tex; cs.sw = 8; cs.sh = 8; cs.layer = 1;
                    w.add<SpriteRef>(e, cs);
                }
            }
        }
        PHX_LOG_INFO("LevelScene ready: %u entities", c->world->count());
    }

    void update(EngineCtx* c, scalar dt) override {
        player_system(c, dt);
        physics_system(c, dt);
        interaction_system(c, dt);
        animation_system(c, dt);
        camera_system(c, dt);
        if (c->input->just(Button::Start)) c->scenes->push(make_pause_scene(c));
    }

    void render(EngineCtx* c, scalar) override {
        Renderer& r = *c->render;
        r.draw_tilemap(c->map, 0);
        draw_world_sprites(c);

        hud_.begin(r, *c->input);
        char buf[32]; int n = 0;
        for (const char* s = "SCORE "; *s; ++s) buf[n++] = *s;
        fmt_uint(buf + n, unsigned(c->scenes->persistent().get_int("score"_hash)));
        hud_.text(vec2{ s_from_int(2), s_from_int(2) }, *c->font, buf, rgba(240, 240, 80));
        int hp = 3; if (Player* pl = c->world->get<Player>(c->player)) hp = pl->health;
        hud_.bar(UIRect{ vec2{ s_from_int(2), s_from_int(12) }, vec2{ s_from_int(40), s_from_int(5) } },
                 s_from_int(hp) / s_from_int(3), rgba(220, 40, 40), rgba(60, 60, 60));
        hud_.end();
    }
};

class TitleScene final : public Scene {
    UI ui_;
public:
    void render(EngineCtx* c, scalar) override {
        ui_.begin(*c->render, *c->input);
        ui_.text(vec2{ s_from_int(28), s_from_int(40) }, *c->font, "PRESS START", rgba(255, 255, 255));
        ui_.end();
    }
    void update(EngineCtx* c, scalar) override {
        // Start (Enter in mGBA) or A (X in mGBA) begins the game — A is the discoverable one.
        if (c->input->just(Button::Start) || c->input->just(Button::A))
            c->scenes->replace(make_level_scene(c));
    }
};

class PauseScene final : public Scene {
    UI ui_;
public:
    PauseScene() { render_below = true; }   // gameplay stays visible, frozen, behind us
    void render(EngineCtx* c, scalar) override {
        ui_.begin(*c->render, *c->input);
        ui_.rect(UIRect{ vec2{ s_from_int(30), s_from_int(28) }, vec2{ s_from_int(68), s_from_int(40) } }, rgba(10, 10, 20));
        ui_.text(vec2{ s_from_int(44), s_from_int(32) }, *c->font, "PAUSED", rgba(255, 255, 255));
        if (ui_.button(UIRect{ vec2{ s_from_int(36), s_from_int(44) }, vec2{ s_from_int(56), s_from_int(8) } }, *c->font, "RESUME")) c->scenes->pop();
        if (ui_.button(UIRect{ vec2{ s_from_int(36), s_from_int(56) }, vec2{ s_from_int(56), s_from_int(8) } }, *c->font, "QUIT"))   c->app->request_quit();
        ui_.end();
    }
    void update(EngineCtx* c, scalar) override {
        if (c->input->just(Button::Start)) c->scenes->pop();   // Start also resumes
    }
};

Scene* make_level_scene(EngineCtx* c) { return c->app->mem().persistent().make<LevelScene>(); }
Scene* make_title_scene(EngineCtx* c) { return c->app->mem().persistent().make<TitleScene>(); }
Scene* make_pause_scene(EngineCtx* c) { return c->app->mem().persistent().make<PauseScene>(); }

// ---- composition root -----------------------------------------------------------------
void PlatformerGame::on_start(App& app) {
    ArenaAllocator& A = app.mem().persistent();
    res = ResourceCache::create(A).unwrap();
    if (res->mount(app.platform(), bundle) != Status::Ok)
        PHX_LOG_ERROR("platformer: failed to mount '%s'", bundle);

    void* buf = A.alloc(scene_scratch_bytes);
    scene_scratch.init(buf, scene_scratch_bytes);
    scenes = SceneStack::create(A, scene_scratch).unwrap();

    ctx.app = &app; ctx.world = &app.world(); ctx.render = &app.render();
    ctx.input = &app.input(); ctx.scenes = scenes; ctx.physics = &physics;
    ctx.res = res; ctx.camera = &camera; ctx.font = &font;

    ctx.tiles_tex = load_tex(&ctx, kTilesTex);
    ctx.hero_tex  = load_tex(&ctx, kHeroTex);
    ctx.coin_tex  = load_tex(&ctx, kCoinTex);
    font.tex = load_tex(&ctx, "font"_hash);
    font.glyph_w = 8; font.glyph_h = 8; font.cols = 16; font.first_char = 32;
    font.advance = 8; font.line_h = 8;

    // hero animation table, loaded from the Sprite asset (frame grid + clips authored offline)
    if (auto sr = res->sprite(kHeroTex); sr.ok()) {
        SpriteView sv = sr.unwrap();
        ctx.hero_sheet = SpriteSheet{ sv.frame_w, sv.frame_h, sv.cols };
        ctx.hero_clip_count = sv.clip_count < 4 ? sv.clip_count : 4;
        for (uint16_t i = 0; i < ctx.hero_clip_count; ++i)
            ctx.hero_clips[i] = AnimClip{ sv.clips[i].first, sv.clips[i].count, sv.clips[i].fps, sv.clips[i].loop != 0 };
    }

    // audio: the mixer + the SFX baked from WAVs (the SoundView points at the mmap'd PCM)
    mixer = AudioMixer::create(A, caps(), 44100).unwrap();
    ctx.mixer = mixer;
    if (auto js = res->sound("jump"_hash); js.ok()) { auto v = js.unwrap(); ctx.jump_snd = SoundView{ v.samples, v.frames, v.rate }; }
    if (auto cs = res->sound("coin"_hash); cs.ok()) { auto v = cs.unwrap(); ctx.coin_snd = SoundView{ v.samples, v.frames, v.rate }; }

    scenes->push(make_title_scene(&ctx));
}

void PlatformerGame::on_fixed_update(App&, scalar dt) { scenes->update(&ctx, dt); }

void PlatformerGame::on_render(App& app, scalar alpha) {
    Renderer& r = app.render();
    r.begin_frame(camera);
    scenes->render(&ctx, alpha);
    r.end_frame();

    // Stand in for the platform audio callback: pull a block from the mixer and track the peak
    // so the headless test can confirm the baked Sound assets actually produced output.
    if (mixer) {
        static int16_t mixbuf[128 * 2];
        mixer->mix(mixbuf, 128);
        for (int i = 0; i < 128 * 2; ++i) { int32_t a = mixbuf[i] < 0 ? -mixbuf[i] : mixbuf[i]; if (a > audio_peak) audio_peak = a; }
    }
}

int PlatformerGame::score() const {
    return scenes ? int(scenes->persistent().get_int("score"_hash)) : -1;
}
int PlatformerGame::player_x() const {
    if (!ctx.world || ctx.player == ecs::kInvalid) return -1;
    Transform* t = ctx.world->get<Transform>(ctx.player);
    return t ? s_to_int(t->pos.x) : -1;
}
bool PlatformerGame::player_on_ground() const {
    if (!ctx.world || ctx.player == ecs::kInvalid) return false;
    Body* b = ctx.world->get<Body>(ctx.player);
    return b && b->on_ground;
}
uint32_t PlatformerGame::depth() const { return scenes ? scenes->depth() : 0; }
int      PlatformerGame::sfx_count() const { return int(ctx.sfx_triggers); }
int32_t  PlatformerGame::audio_peak_level() const { return audio_peak; }

} // namespace game
