# Phoenix Engine — Gameplay Systems

> Input · Audio · Scene · Physics · Animation · UI. These sit above ECS/render/resource
> and below the game. Each is small, decoupled, and meets the others only through ECS
> data or explicit handles — never through direct includes of one another.

---

## 1. Input (`engine/input/`)

Turns the platform's `phx_input_raw` snapshot into semantic, frame-stable state.

```cpp
namespace phx {
enum class Button : uint8_t {            // canonical order — same meaning everywhere
    Up, Down, Left, Right, A, B, X, Y, L, R, Start, Select, Count
};
struct InputState {
    uint32_t held, pressed, released;     // bitmasks over Button
    vec2     lstick, rstick;              // [-1,1], zero on GBA
    vec2     pointer; bool pointer_down;  // mouse/touch; off on GBA

    bool down(Button b)     const { return held    & (1u<<int(b)); }
    bool just(Button b)     const { return pressed & (1u<<int(b)); }   // edge
    bool up(Button b)       const { return released& (1u<<int(b)); }
    void poll(const phx_platform*);       // reads raw, computes edges vs last frame
};
} // namespace phx
```

Per-platform mapping table (lives in `input/src/maps.cpp`):

| `Button` | GBA key      | PSP        | PC keyboard | PC gamepad |
|----------|--------------|------------|-------------|------------|
| A        | A            | Cross      | Z / Space   | A          |
| B        | B            | Circle     | X           | B          |
| Start    | Start        | Start      | Enter       | Start      |
| Up..Right| D-pad        | D-pad      | Arrows/WASD | D-pad/stick|

Edge detection (`pressed`/`released`) is computed against last frame, so gameplay reads
"jump pressed this frame" identically on every device. Bindings are data (remappable on
PC); the default table is fixed for consoles. Input is sampled once per frame *before*
the fixed-step sim loop, so all sub-steps see a consistent snapshot (determinism).

---

## 2. Audio (`engine/audio/`)

A small software mixer feeding the platform audio device. One path for all targets;
voice count scales with `phx_caps::audio_channels`.

```cpp
namespace phx {
class AudioMixer {
public:
    static Result<AudioMixer*> create(phx_audio*, ArenaAllocator&, const phx_caps&);
    VoiceId play_sfx(SoundView, float vol=1, float pan=0, bool loop=false);
    void    stop(VoiceId);
    void    play_music(SoundView, bool loop=true);   // streamed where supported
    void    set_music_volume(float);
    void    mix(int16_t* out, uint32_t frames);      // called by platform callback
};
} // namespace phx
```

| Feature           | GBA                          | PSP                  | PC                |
|-------------------|------------------------------|----------------------|-------------------|
| SFX               | 2 DirectSound ch + 4 PSG     | 8 voices, ADPCM      | 32 voices, PCM    |
| Music             | resident tracker module      | streamed ADPCM       | streamed OGG/PCM  |
| Streaming         | no (resident)                | yes (ring buffer)    | yes               |
| Mixer rate        | ~13–18 kHz (timer-driven DMA)| 44.1 kHz             | 44.1/48 kHz       |

The mixer is the same SoA voice loop everywhere; the GBA build compiles a fixed-point,
2-voice specialization (PSG handled separately for music). Compressed formats are
decoded in `mix()` per chunk (ADPCM) or pre-decoded for short SFX. Music streaming uses
the `AudioStream` ring from `docs/06` §5.

---

## 3. Scene Management (`engine/scene/`)

A LIFO **scene stack** (so you can push a pause/menu over gameplay), with explicit
transitions and a persistent-object channel.

```cpp
namespace phx {
struct Scene {
    virtual void on_enter(EngineCtx*) {}
    virtual void on_exit(EngineCtx*)  {}
    virtual void on_pause(EngineCtx*) {}     // another scene pushed on top
    virtual void on_resume(EngineCtx*) {}
    virtual void update(EngineCtx*, scalar dt) {}
    virtual void render(EngineCtx*, scalar alpha) {}
};
class SceneStack {
public:
    void push(Scene*, Transition = Transition::None);
    void pop(Transition = Transition::None);
    void replace(Scene*, Transition = Transition::Fade);
    void update(EngineCtx*, scalar dt);      // updates top (or all, if "transparent")
    void render(EngineCtx*, scalar alpha);   // renders back-to-front for overlays
    Blackboard& persistent();                // survives scene changes (save data, etc.)
};
enum class Transition { None, Fade, SlideLeft, SlideRight };
} // namespace phx
```

- Each scene owns a **scene-scoped arena** (`StackAllocator` mark on enter, reset on
  exit) → leaving a scene frees all its allocations in O(1), no leaks.
- The ECS `World` can be **per-scene** or shared; the example uses one world cleared on
  scene change, with persistent data (score, save slot) in the `Blackboard`.
- Transitions are driven by a tiny coroutine-free state machine and render via the same
  sprite path (a fading quad), so they work on GBA (palette fade) and PSP/PC (alpha).

---

## 4. Physics (`engine/physics/`)

Deliberately **minimal and tile-friendly** for the MVP, with a clear expansion seam.
No solver, no rotation — AABB + tile collision is what 2D platformers need and what the
GBA can afford.

```cpp
namespace phx {
struct AABBColl { vec2 half; uint16_t layer, mask; };   // ECS component
struct Body     { vec2 vel; bool on_ground; uint8_t flags; };

struct Hit { Entity other; vec2 normal; scalar t; bool tile; };

class PhysicsWorld {
public:
    void set_tilemap(const TilemapView&);    // collision layer from phxtile
    // swept AABB vs tilemap (per axis), then AABB-vs-AABB overlap pass:
    void step(ecs::World&, scalar dt, Span<Hit> out_hits);
    bool overlap(const aabb&, uint16_t mask, Entity ignore = ecs::kInvalid) const;
};
} // namespace phx
```

Algorithm (per fixed step):
1. Integrate velocity → tentative position.
2. **Swept tile collision per axis** (X then Y): sample the collision layer over the
   swept rect, resolve to the nearest blocking edge, set `on_ground` on a downward stop.
   Axis-separated resolution gives correct wall-slide/ground behavior cheaply.
3. **Broadphase AABB overlap** for entity-vs-entity (uniform grid binning, ceiling from
   caps) → emit `Hit`s for gameplay (enemy stomp, pickups). Overlap-only by default
   (triggers); resolution is opt-in.

```
 swept X then Y (separable):
   ┌────┐ vel                ┌────┐
   │ P  │───►   wall ▓    →   │ P ▓   stop at wall, keep Y motion
   └────┘                     └────┘
```

**Expansion seam:** `PhysicsWorld` is an interface; the MVP impl is `AABBTilePhysics`.
A future `ImpulsePhysics2D` (mass, restitution, simple manifolds) or an integration of
a slim external 2D lib can replace it without touching gameplay — they all consume the
same `AABBColl`/`Body` components and emit `Hit`s.

---

## 5. Animation (`engine/anim/`)

Sprite-sheet, frame-based, driven by a tiny state machine. Consumes the animation table
baked by `phxsprite`.

```cpp
namespace phx {
struct AnimClip { uint16_t first, count; uint8_t fps; bool loop; };
struct Animator {                              // ECS component
    SpriteSheetId sheet; uint16_t clip; uint16_t frame; scalar timer; uint8_t state;
};
struct AnimStateMachine {                      // data-driven transitions
    struct Edge { uint8_t from, to; uint16_t trigger; };
    Span<AnimClip> clips; Span<Edge> edges;
    void set_trigger(Animator&, uint16_t trig);   // request state change
};
class AnimationSystem {
public:
    void tick(ecs::World&, scalar dt);         // advance timers, pick frame, set SpriteRef
};
} // namespace phx
```

- `AnimationSystem` advances each `Animator`, computes the current frame, and writes the
  source rect into the entity's `SpriteRef` — so the render system stays dumb.
- The **state machine** is data (clips + edges from the prefab/`phxsprite` sidecar), not
  code: `idle ⇄ run ⇄ jump ⇄ fall` for the player is authored, not hardcoded.
- Frame timing uses `scalar` so fixed/float builds animate identically.

```
   ┌──────┐  move>0   ┌──────┐  vy<0   ┌──────┐
   │ idle │──────────►│ run  │────────►│ jump │
   └──────┘◄──────────└──────┘         └──┬───┘
       ▲    move==0        ▲ land           │ vy>0
       └───────────────────┴────────────────┘ fall
```

---

## 6. UI (`engine/ui/`)

An **immediate-mode**, retained-where-it-pays UI that emits `DrawSprite`s (text = font
atlas), sized for GBA/PSP limits. No widget tree allocations per frame.

```cpp
namespace phx {
class UI {
public:
    void begin(EngineCtx*, const InputState&);
    // primitives (all batch into the renderer's sprite path):
    void text(vec2, NameHash font, const char*, Color = white);
    void text_fmt(vec2, NameHash font, const char* fmt, ...);
    bool button(Rect, const char* label);     // returns true on press (menus)
    void image(Rect, TextureId, Rect src);
    void bar(Rect, scalar t, Color fg, Color bg);   // HUD health/energy
    // dialogue:
    void dialogue(const DialogueView&, int line, scalar reveal_t);  // typewriter
    void end();
};
} // namespace phx
```

Supported surfaces:
- **Menus** — focus-based navigation by D-pad/buttons (not just mouse), because
  consoles have no pointer. `button()` participates in a focus ring.
- **Text rendering** — bitmap font atlas; fixed-width glyphs on GBA to save tiles.
- **HUD** — `bar()`, `image()`, `text_fmt()` for score/health/lives; cheap, per-frame.
- **Dialogue** — typewriter reveal driven by `reveal_t`, fed from `phxbin` dialogue
  tables; portrait via `image()`.

GBA constraints baked in: glyphs are 8×8 tiles drawn as BG/OBJ; the UI batches into the
same ≤128 OBJ budget and warns (via `RenderStats`) if a HUD-heavy frame would overflow.

---

## 7. How they compose (the example's per-frame flow)

```
 input.poll ─► [sim step] InputSystem → AISystem → PhysicsWorld.step →
               (Hit handling: stomp/pickup) → AnimationSystem → CameraSystem
           ─► SceneStack.update ─► audio events queued
 render: SceneStack.render → world→DrawSprite (sprites/tilemap) → UI.* (HUD/menu)
         → Renderer.end_frame → platform.present
 audio:  platform callback → AudioMixer.mix
```

Every arrow is data or a handle — no system includes another system's header. That is
the decoupling the architecture exists to guarantee.
