// examples/platformer/components.h — the example's glue: its EngineCtx (the context the
// SceneStack threads to every scene), its ECS components, and asset name hashes. The game
// reuses ENGINE components where they exist (phx::Transform/Body/AABBColl from physics,
// phx::Animator from anim) and adds only game-specific data. Logic lives in systems.cpp.
//
// EngineCtx is GAME-defined on purpose: the scene module forward-declares it opaquely, so
// each game wires whatever subsystems it needs into one context without the engine
// depending on the game. This is the extensibility the opaque seam exists to provide.
#ifndef PLATFORMER_COMPONENTS_H
#define PLATFORMER_COMPONENTS_H

#include "phx/runtime/app.h"
#include "phx/ecs/world.h"
#include "phx/render/renderer.h"
#include "phx/input/input.h"
#include "phx/scene/scene.h"
#include "phx/ui/ui.h"
#include "phx/physics/physics.h"
#include "phx/anim/anim.h"
#include "phx/audio/mixer.h"
#include "phx/resource/cache.h"

namespace phx {
// The runtime context every scene receives. Bundles the App + the subsystems the game owns.
struct EngineCtx {
    App*            app    = nullptr;
    ecs::World*     world  = nullptr;
    Renderer*       render = nullptr;
    const InputState* input = nullptr;
    SceneStack*     scenes = nullptr;
    PhysicsWorld*   physics = nullptr;
    ResourceCache*  res    = nullptr;
    Camera2D*       camera = nullptr;
    BitmapFont*     font   = nullptr;
    AudioMixer*     mixer  = nullptr;

    // shared asset handles (resolved once at startup)
    TextureId hero_tex = kNoTexture, coin_tex = kNoTexture, tiles_tex = kNoTexture;
    TextureId enemy_tex = kNoTexture, spike_tex = kNoTexture;
    TilemapId map = kNoTilemap;
    uint8_t   map_layers = 1;                  // tile layers to draw (last = gameplay/solid)
    int32_t   level_w = 0, level_h = 0;        // level extent in pixels (for camera clamp)
    ecs::Entity player = ecs::kInvalid;
    vec2     player_spawn{};                    // where the player (re)spawns on death
    uint16_t enemies_killed = 0;               // stomped enemies (HUD/score + tests)
    uint16_t player_deaths  = 0;               // respawns so far (tests)
    bool     show_profiler  = false;           // Select toggles the frame profiler overlay

    // hero animation, loaded from the Sprite asset at startup (clips outlive the Animator)
    AnimClip    hero_clips[4]{};
    uint16_t    hero_clip_count = 0;
    SpriteSheet hero_sheet{};

    // SFX loaded from baked Sound assets; played on jump / coin pickup
    SoundView jump_snd{};
    SoundView coin_snd{};
    uint32_t  sfx_triggers = 0;

    // per-step physics hits (player↔coin etc.), produced by physics_system
    Hit      hits[64];
    uint32_t hit_count = 0;

    const char* save_path = "platformer.sav";  // persistence key (a file on PC/PSP, SRAM on GBA)
    bool        loaded = false;                 // a valid save was restored at boot
};
} // namespace phx

namespace game {
using namespace phx;

// asset names (FNV-1a at compile time)
constexpr NameHash kTilesTex = "tiles"_hash;
constexpr NameHash kLevelMap = "level"_hash;
constexpr NameHash kHeroTex  = "hero"_hash;
constexpr NameHash kCoinTex  = "coin"_hash;

// collision layers
enum CollLayer : uint16_t {
    kLayerPlayer = 1 << 0,
    kLayerCoin   = 1 << 1,
    kLayerEnemy  = 1 << 2,   // a patrolling enemy: stomp from above kills it, side contact hurts
    kLayerHazard = 1 << 3,   // a static spike: any contact hurts
};

// --- rendering: the single drawable, written from the Animator each frame ---
struct SpriteRef {
    TextureId tex = kNoTexture;
    int16_t   sx = 0, sy = 0, sw = 8, sh = 8;
    uint8_t   layer = 1;
    uint16_t  flags = 0;
};

// --- gameplay ---
enum AnimState : uint16_t { kIdle = 0, kWalk = 1 };
// `invuln` is the remaining post-hit invulnerability (seconds): brief i-frames so one spike or
// enemy brush costs a single point, not the whole bar in the frames the boxes overlap.
struct Player { int16_t health = 3; bool facing_left = false; scalar invuln{}; };
struct Coin   { uint16_t value = 1; };
// A patroller: walks between home_x ± range, turning at the bounds. Gravity-bound (has a Body).
struct Enemy  { int16_t dir = -1; scalar home_x{}; scalar range{}; };
// A static spike. Tag only — presence + an AABBColl on the hazard layer is the whole behaviour.
struct Hazard {};

// Persistent save blob (POD, fixed layout — written verbatim to the platform save store). The
// magic+version let load() tell a real save from fresh storage (uninitialised SRAM / no file).
struct SaveData {
    uint32_t magic;     // kSaveMagic
    uint16_t version;   // kSaveVersion
    uint16_t score;
    uint16_t deaths;
    uint16_t pad;
};
constexpr uint32_t kSaveMagic   = 0x53584850u;   // 'PHXS' (little-endian)
constexpr uint16_t kSaveVersion = 1;

// systems (systems.cpp)
void player_system(EngineCtx*, scalar dt);
void enemy_system(EngineCtx*, scalar dt);
void physics_system(EngineCtx*, scalar dt);
void interaction_system(EngineCtx*, scalar dt);

// persistence (systems.cpp): write/restore the run via the platform save seam
void save_game(EngineCtx*);
bool load_game(EngineCtx*);     // true if a valid save was applied
void animation_system(EngineCtx*, scalar dt);
void camera_system(EngineCtx*, scalar dt);

// scene factories (systems.cpp)
Scene* make_level_scene(EngineCtx*);
Scene* make_title_scene(EngineCtx*);
Scene* make_pause_scene(EngineCtx*);

// The engine's Game hook: owns the higher subsystems, mounts the bundle, and drives the
// scene stack. Subsystems the App doesn't own (physics/scenes/UI live here, not in runtime).
struct PlatformerGame : Game {
    PhysicsWorld   physics;
    Camera2D       camera;
    BitmapFont     font;
    SceneStack*    scenes = nullptr;
    ResourceCache* res    = nullptr;
    AudioMixer*    mixer  = nullptr;
    StackAllocator scene_scratch;
    EngineCtx      ctx;
    const char*    bundle = "platformer.phxp";
    const char*    save_path = "platformer.sav";      // overridable per target (PC/PSP/GBA)
    size_t         scene_scratch_bytes = 256u << 10;  // scene-arena budget; shrink on tight RAM (GBA)
    int32_t        audio_peak = 0;            // running peak of mixed output (proves audio plays)

    void on_start(App&) override;
    void on_fixed_update(App&, scalar) override;
    void on_render(App&, scalar) override;

    // read-only state queries (valid until App shutdown; use from on_stop)
    int      score() const;
    int      player_x() const;
    bool     player_on_ground() const;
    uint32_t depth() const;
    int      sfx_count() const;
    int32_t  audio_peak_level() const;
    int      health() const;          // current player HP (-1 if no player)
    int      enemies_killed() const;  // patrollers stomped
    int      deaths() const;          // player respawns
    bool     was_loaded() const;      // a saved game was restored at boot
};

// tiny int->string for the HUD (no STL in the game TU)
inline int fmt_uint(char* buf, unsigned v) {
    char tmp[12]; int n = 0;
    do { tmp[n++] = char('0' + v % 10); v /= 10; } while (v);
    for (int i = 0; i < n; ++i) buf[i] = tmp[n - 1 - i];
    buf[n] = 0; return n;
}

} // namespace game
#endif // PLATFORMER_COMPONENTS_H
