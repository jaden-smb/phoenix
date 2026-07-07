// examples/emberwing/game.h — the game's glue: its EngineCtx (threaded to every scene), ECS
// components, tuning constants and asset hashes. Reuses ENGINE components where they exist
// (phx::Transform/Body/AABBColl from physics, phx::Animator from anim) and adds only game
// data. Logic lives in systems.cpp; scenes + the composition root in scenes.cpp.
//
// Portability rules this file obeys (see README.md §4): no STL, no platform/OS headers, and
// every tuning value is integer px/s (`s_from_int`) or an integer frame count, so the float
// (PC/PSP) and fixed16 (GBA) scalar tiers simulate identically.
#ifndef EMBERWING_GAME_H
#define EMBERWING_GAME_H

#include "phx/runtime/app.h"
#include "phx/ecs/world.h"
#include "phx/render/renderer.h"
#include "phx/input/input.h"
#include "phx/scene/scene.h"
#include "phx/ui/ui.h"
#include "phx/physics/physics.h"
#include "phx/anim/anim.h"
#include "phx/audio/mixer.h"
#include "phx/audio/command_queue.h"
#include "phx/resource/cache.h"

namespace game {
using namespace phx;

// ---- asset names (FNV-1a at compile time) ----------------------------------------------
constexpr NameHash kTilesTex = "tiles"_hash;
constexpr NameHash kLevelMap = "level"_hash;

// ---- collision layers -------------------------------------------------------------------
// Engine AABB overlap is O(n²) over everything holding an AABBColl, so only entities that
// need physics Hits carry one (player, enemies, geysers, lava). Pickups and touch-triggers
// (waystone/gate) are collected by trigger_system with a direct box-vs-player test instead —
// ~60 cheap checks rather than 60 extra bodies in the pairwise pass (a GBA budget choice).
enum CollLayer : uint16_t {
    kLayerPlayer = 1 << 0,
    kLayerEnemy  = 1 << 1,   // cinderling / thornback / ashwisp (behaviour via Enemy::kind)
    kLayerHazard = 1 << 2,   // lava rects + geyser flame columns
};

// ---- tuning (integer px/s and frame counts only — tier-exact) ---------------------------
namespace tune {
inline const scalar kRun         = s_from_int(75);    // ground/air run speed px/s
inline const scalar kJump        = s_from_int(205);   // initial jump speed px/s (apex ≈ 33 px)
inline const scalar kGravity     = s_from_int(640);   // px/s²
inline const scalar kTerminal    = s_from_int(240);   // fall speed clamp px/s
inline const scalar kStompBounce = s_from_int(160);   // upward bounce after a stomp kill
inline const scalar kHurtKick    = s_from_int(130);   // knock-up on taking a hit
inline const scalar kCinderRun   = s_from_int(22);    // patroller speeds px/s
inline const scalar kThornRun    = s_from_int(18);
inline const scalar kWispBob     = s_from_int(30);    // ashwisp vertical bob speed px/s

constexpr int16_t kMaxHealth   = 3;
constexpr int16_t kInvulnFr    = 60;    // i-frames after a hit
constexpr int8_t  kCoyoteFr    = 4;     // grace frames to jump after leaving a ledge
constexpr int8_t  kJumpBufFr   = 6;     // frames a jump press is remembered before landing
constexpr int16_t kGeyserIdle  = 96;    // geyser cycle, in frames: idle -> warn -> erupt
constexpr int16_t kGeyserWarn  = 20;
constexpr int16_t kGeyserErupt = 60;
constexpr int16_t kGeyserCycle = kGeyserIdle + kGeyserWarn + kGeyserErupt;
constexpr int16_t kParticleTtl = 24;    // spark burst lifetime (frames)
constexpr int     kEmberScore  = 1;
constexpr int     kShardScore  = 25;
constexpr int     kStompScore  = 5;
constexpr int     kHpBonus     = 20;    // per remaining heart at the gate
} // namespace tune

// ---- rendering --------------------------------------------------------------------------
// The single drawable. `ox/oy` offset the visual from the collider centre (tall sprites such
// as the geyser flame anchor differently); `hidden` lets a system keep the entity but skip
// the draw (idle geysers). Written from the Animator each frame by animation_system.
struct SpriteRef {
    TextureId tex = kNoTexture;
    int16_t   sx = 0, sy = 0, sw = 16, sh = 16;
    int8_t    ox = 0, oy = 0;
    uint8_t   layer = 2;
    uint16_t  flags = 0;
    bool      hidden = false;
};

// A baked Sprite asset resolved once at startup: uploaded texture + frame grid + clip table.
struct SpriteAsset {
    TextureId   tex = kNoTexture;
    SpriteSheet sheet{};
    AnimClip    clips[6]{};
    uint16_t    clip_count = 0;
};

// Clip indices per asset — MUST match the clip order authored in bake.h.
enum HeroClip     : uint16_t { kHeroIdle = 0, kHeroRun, kHeroJump, kHeroFall, kHeroHurt };
enum CinderClip   : uint16_t { kCinderWalk = 0 };
enum ThornClip    : uint16_t { kThornWalk = 0 };
enum WispClip     : uint16_t { kWispFloat = 0 };
enum GeyserClip   : uint16_t { kGeyserWarnClip = 0, kGeyserEruptClip };
enum StoneClip    : uint16_t { kStoneDormant = 0, kStoneLit };

// ---- gameplay components ----------------------------------------------------------------
struct Player {
    int16_t health   = tune::kMaxHealth;
    int16_t invuln   = 0;        // i-frames remaining
    int8_t  coyote   = 0;        // frames since grounded (jump grace)
    int8_t  jump_buf = 0;        // buffered jump-press frames
    bool    facing_left = false;
    bool    jump_held   = false; // A held since takeoff (release cuts the jump)
};

enum class EnemyKind : uint8_t { Cinder, Thorn, Wisp };
// One enemy component, three behaviours. Cinder/Thorn patrol horizontally around home.x;
// Wisp bobs vertically around home.y (no gravity). Thorn is spiky: stomping it hurts YOU.
struct Enemy {
    EnemyKind kind = EnemyKind::Cinder;
    int8_t    dir  = -1;
    vec2      home{};
    scalar    range{};
};

enum class PickupKind : uint8_t { Ember, Shard, Heart };
struct Pickup { PickupKind kind = PickupKind::Ember; vec2 half{}; };

// Lava rects and geyser columns. `damage` >= kMaxHealth means instant death (lava).
struct Hazard { int16_t damage = 1; };

enum class GeyserState : uint8_t { Idle, Warn, Erupt };
struct Geyser { int16_t timer = 0; GeyserState state = GeyserState::Idle; };

struct Checkpoint { bool lit = false; vec2 half{}; };
struct Goal       { vec2 half{}; };
struct Particle   { int16_t ttl = tune::kParticleTtl; };

// ---- persistence ------------------------------------------------------------------------
// Best-run snapshot, written at the Sungate and on pause-quit. POD, fixed layout; the
// magic+version distinguish a real save from fresh storage (uninitialised SRAM / no file).
struct SaveData {
    uint32_t magic;        // kSaveMagic
    uint16_t version;      // kSaveVersion
    uint16_t best_score;
    uint8_t  best_shards;
    uint8_t  clears;
    uint16_t pad;
};
constexpr uint32_t kSaveMagic   = 0x45584850u;   // 'PHXE' (little-endian)
constexpr uint16_t kSaveVersion = 1;

} // namespace game

namespace phx {
using namespace game;

// The runtime context every scene receives (the scene module holds it opaquely). Game-defined
// on purpose: it bundles exactly the subsystems THIS game wires together.
struct EngineCtx {
    App*               app     = nullptr;
    ecs::World*        world   = nullptr;
    Renderer*          render  = nullptr;
    const InputState*  input   = nullptr;
    SceneStack*        scenes  = nullptr;
    PhysicsWorld*      physics = nullptr;
    ResourceCache*     res     = nullptr;
    Camera2D*          camera  = nullptr;
    BitmapFont*        font    = nullptr;
    TextureId          credit  = kNoTexture;   // pre-rendered 2× title credit strip (224×16)
    AudioMixer*        mixer   = nullptr;
    AudioCommandQueue* queue   = nullptr;   // game thread pushes; a device thread (or the
                                            // headless pump) drains into the mixer

    // sprite assets + loose textures, resolved once at startup
    SpriteAsset hero, cinder, thorn, wisp, geyser, ember, shard, heart, waystone, gate;
    TextureId   tiles_tex = kNoTexture, spark_tex = kNoTexture;

    // level state
    TilemapId map = kNoTilemap;
    uint8_t   map_layers = 1;               // draw order: backdrops first, solid layer last
    int32_t   level_w = 0, level_h = 0;     // pixels, for the camera clamp
    ecs::Entity player = ecs::kInvalid;
    vec2      respawn{};                    // last lit waystone (or the level spawn)
    int32_t   cam_x = 0, cam_y = 0;         // smoothed camera (integer pixels — tier-exact)

    // SFX/music views into the mounted bundle
    SoundView snd_jump{}, snd_ember{}, snd_stomp{}, snd_hurt{}, snd_checkpoint{},
              snd_shard{}, snd_heart{}, snd_goal{}, snd_geyser{}, music{};

    // run counters (HUD + headless test assertions)
    uint16_t embers = 0, shards = 0, stomps = 0, deaths = 0, checkpoints = 0;
    uint16_t sfx_triggers = 0;
    bool     cleared = false;
    bool     show_profiler = false;

    // per-step physics hits, produced by physics_system
    Hit      hits[64];
    uint32_t hit_count = 0;

    const char* save_path = "emberwing.sav";
    bool        loaded = false;             // a valid save was restored at boot
    SaveData    best{};                     // restored / updated best-run stats
};
} // namespace phx

namespace game {

// Push an SFX intent (fire-and-forget; the audio consumer drains it before mixing).
inline void sfx(EngineCtx* c, const SoundView& s, float vol = 1.0f) {
    if (c->queue && s.samples) { c->queue->play_sfx(s, vol); ++c->sfx_triggers; }
}

// systems (systems.cpp) — called in this order by the level scene, before rendering
void player_system(EngineCtx*, scalar dt);
void enemy_system(EngineCtx*, scalar dt);
void geyser_system(EngineCtx*, scalar dt);
void physics_system(EngineCtx*, scalar dt);
void interaction_system(EngineCtx*, scalar dt);   // resolves physics Hits (enemies/hazards)
void trigger_system(EngineCtx*, scalar dt);       // pickups + waystones + the gate
void particle_system(EngineCtx*, scalar dt);
void animation_system(EngineCtx*, scalar dt);
void camera_system(EngineCtx*, scalar dt);

// helpers shared by systems/scenes (systems.cpp)
void spawn_burst(EngineCtx*, vec2 pos, uint8_t n);   // spark particles (stomp/pickup feedback)
void respawn_player(EngineCtx*);
void save_game(EngineCtx*);
bool load_game(EngineCtx*);

// scene factories (scenes.cpp)
Scene* make_title_scene(EngineCtx*);
Scene* make_level_scene(EngineCtx*);
Scene* make_pause_scene(EngineCtx*);
Scene* make_clear_scene(EngineCtx*);

// The engine's Game hook: owns the subsystems the App doesn't (physics/scenes/audio/UI),
// mounts the bundle and drives the scene stack. Entry points tweak the public knobs.
struct EmberwingGame : Game {
    PhysicsWorld   physics;
    Camera2D       camera;
    BitmapFont     font;
    SceneStack*    scenes = nullptr;
    ResourceCache* res    = nullptr;
    AudioMixer*    mixer  = nullptr;
    AudioCommandQueue queue;
    AudioCommand   queue_storage[32];
    StackAllocator scene_scratch;
    EngineCtx      ctx;

    const char* bundle    = "emberwing.phxp";
    const char* save_path = "emberwing.sav";
    size_t   scene_scratch_bytes = 64u << 10;  // shrink further on GBA (entry point sets it)
    uint32_t mixer_rate   = 44100;             // GBA entry passes the 18157 Hz device rate
    bool     device_audio = false;             // true when a platform audio pump owns the
                                               // mixer; false => on_render drains headlessly
    int32_t  audio_peak   = 0;                 // running peak of headless mixes (tests)

    void on_start(App&) override;
    void on_fixed_update(App&, scalar) override;
    void on_render(App&, scalar) override;

    // read-only state queries (valid until App shutdown; use from on_stop)
    int      score() const;            // blackboard run score
    int      player_x() const;
    int      player_y() const;
    int      health() const;
    int      embers() const     { return ctx.embers; }
    int      shards() const     { return ctx.shards; }
    int      stomps() const     { return ctx.stomps; }
    int      deaths() const     { return ctx.deaths; }
    int      checkpoints() const{ return ctx.checkpoints; }
    bool     cleared() const    { return ctx.cleared; }
    int      sfx_count() const  { return ctx.sfx_triggers; }
    uint32_t depth() const;
    bool     was_loaded() const { return ctx.loaded; }
    int32_t  audio_peak_level() const { return audio_peak; }
};

// tiny int->string for the HUD (no STL in the game TUs)
inline int fmt_uint(char* buf, unsigned v) {
    char tmp[12]; int n = 0;
    do { tmp[n++] = char('0' + v % 10); v /= 10; } while (v);
    for (int i = 0; i < n; ++i) buf[i] = tmp[n - 1 - i];
    buf[n] = 0; return n;
}

} // namespace game
#endif // EMBERWING_GAME_H
