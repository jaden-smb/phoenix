# Phoenix Engine — Development Status & Journal

A living log of what is **built and verified** vs. **designed but not yet implemented**,
plus the immediate next steps. Updated as development proceeds (see the README's
"Development status" section for the short version).

> Legend:  ✅ implemented + tested   🟡 partial / interface only   ⬜ designed (docs only)

---

## Current milestone: **M2 — reached real hardware. One C++17 codebase now cross-compiles to a Game Boy Advance ROM and a PSP EBOOT, in addition to the host; full asset pipeline + the example running off it; SDL window/GL/audio backends authored.**
### (M1 COMPLETE — a full headless platformer runs on every engine system)
> The portability thesis is proven on metal: the **same** portable engine that passes the host
> suite cross-compiles with **devkitARM** → a real `.gba` ROM and **pspsdk** → a real `EBOOT.PBP`
> (3 architectures: x86, ARM7TDMI, MIPS Allegrex). The SDL audio device + a lock-free command
> queue complete the audio-output glue. What's left is *running* on emulators/hardware (needs a
> display/PPSSPP, absent here) and hardware-accelerated render backends (GBA PPU / PSP GU) for
> speed — the software renderer drives both today.

The engine runs a **complete headless playable slice** (boot → spawn → input → fixed-step
sim → render → verify by framebuffer), a **full asset pipeline** (bake a `.phxp`
bundle → mount through the platform seam → zero-copy views → render from the bundle),
**tile physics** (gravity → swept AABB-vs-tilemap collision → land/wall/bonk, verified),
**sprite animation** (data-driven clips + state machine → source rect on screen, verified),
a **scene stack** (LIFO push/pop, menu-over-gameplay overlay, persistent Blackboard,
O(1) scene-arena rollback, verified), and an **immediate-mode UI** (bitmap text, HUD bar,
focus-ring menu navigation, verified by reading glyph/bar pixels off the framebuffer).

**The M1 capstone — the example platformer — is built and verified.** It assembles *every*
system into one game: it mounts a `phxpack`-baked bundle via `ResourceCache`, runs a
**title → level → pause** `SceneStack`, drives a player with input + `PhysicsWorld`
(gravity/jump/tile collision), animates via `AnimationSystem`, collects coins through the
physics overlap pass, and draws a HUD with the `UI`. A scripted controller plays it headlessly
and asserts the outcome from the ECS + framebuffer — identically on both scalar tiers.
Everything builds clean (`-Wall -Wextra -Wpedantic`, zero warnings) from **one codebase
under both scalar tiers**:

```
$ make check          # unit + loop + render(soft+ppu+gu) + gameplay slices + resource + phxpack + dep gate
PASS  120336 checks across 101 cases
SMOKE PASS      (100 deterministic frames)
RENDER PASS     (tilemap+sprite; wrote build/render_out.ppm)
PPU PASS        (GBA-native: 4bpp tiles + OAM via ppu_compose; 28 checks, palette/align/OBJ-ceiling)
GU PASS         (PSP-native: GU sprite quads via gu_compose; 16 checks, bit-identical to soft)
PLAYABLE PASS   (entity moved x:10 -> 30 by scripted input; framebuffer + ECS verified)
PHYSICS PASS    (body fell y:8 -> 36, landed on tile floor; framebuffer + ECS verified)
ANIM PASS       (animator reached frame 2 @ 8fps; on-screen colour = that frame; ECS verified)
SCENE PASS      (pushed a menu over gameplay; overlay composites; stack 2 deep; framebuffer)
UI PASS         (text + HUD bar drawn; D-pad moved focus 0->1; A activated button 1; framebuffer)
PLATFORMER PASS (capstone: score=2 player_x=59 on_ground=1 depth=1 sfx=2 audio_peak=9000)
AUDIO PASS      (mix + stream PCM via ring; decode a WAV -> bake Sound -> mount -> mix; 13 checks)
TEXCACHE PASS   (25 checks: LRU cache hits/budget eviction/slot recycling/oversize reject)
PNG PASS        (decode a real PNG via DEFLATE -> bake -> mount -> render its colours)
SPRITE PASS     (13 checks: sheet PNG -> Texture+Sprite -> SpriteView -> Animator -> frame on screen)
TILED PASS      (20 checks: .tmj JSON -> Tilemap+Spawns -> mount -> render tiles + resolve spawns)
RESOURCE PASS   (17 checks: bake raw + LZSS -> mount -> decompress -> views -> render)
PHXPACK PASS    (CLI: PNG/PPM/CSV/sprdef/tmj/wav -> valid .phxp bundle, raw and --compress)
depcheck: OK (28 edges, acyclic, layering respected)

$ make platformer TIER=gba_sim   # the WHOLE GAME under GBA fixed-point math
PLATFORMER PASS (score=2 player_x=59 on_ground=1 — byte-identical outcome to the float tier)

$ make gba       # devkitARM: the SAME engine -> a real Game Boy Advance ROM
ROM: build/gba/phx-smoke.gba (19164 bytes, header fixed)

$ make psp       # pspsdk: the SAME engine -> a real PSP EBOOT
EBOOT: build/psp/EBOOT.PBP (194386 bytes)
```

First rendered frame (`build/render_out.ppm`, 64×48) — a blue/yellow tile checkerboard
with a red sprite composited on top, proving camera + tilemap + sprite + sort + alpha:

```
BBBBBBBBYYYYYYYYBBBBBBBBYYYYYYYY........   . = background
YYYYYYYYBBBBBBBBYYYYYYYYBBBBBBBB........   B = blue  tile
BBBBBBBBYYYYYYYYBBBBRRRRRRRRYYYY........   Y = yellow tile
....................RRRRRRRR............   R = red sprite (on top)
```

Toolchain on this machine: `g++ 15.2`, `make`, `python3`, **devkitARM g++ 15.2 + libgba/libtonc
+ gbafix**, **pspsdk psp-g++ 15.2** (so `make gba`/`make psp` cross-compile real binaries). No
`cmake`, no SDL2, no GBA/PSP emulator with a display — so the SDL backends and *running* the
ROM/EBOOT are out of reach here, but everything cross-compiles.

### Architecture refinement this session
The `App`/main-loop was extracted out of `core` into a new top-level **`runtime`**
module. Reason: `memory` and `platform` depend on `core`'s types, while the loop
depends on `memory`+`platform` — bundling the loop into `core` formed a module-level
cycle (caught by `depcheck`). `core` is now a *closed foundation* (zero outgoing
module edges); `runtime` is the composition root at the top of the layering.

---

## Module status

| Module | State | Notes |
|--------|-------|-------|
| **core/types** | ✅ | fixed-width ints, `Result`/`Status`, `TypeId`, FNV-1a `_hash`, `Span` |
| **core/assert** | ✅ | `PHX_ASSERT`/`PHX_VERIFY`; host handler (abort+log) |
| **core/fixed** | ✅ | Q16.16 `fixed16`; `fx_sqrt/sin/cos/rcp` via LUT + Newton; **no HW divide used** |
| **core/math** | ✅ | scalar-generic `Vec2/AABB/Mat3`; `scalar` = float or fixed16 per tier |
| **core/caps** | ✅ | compile-time capability tiers (GBA/PSP/PC) |
| **memory/allocators** | ✅ | Arena, Stack(+RAII Scope), Pool, ObjectPool — O(1), no fragmentation |
| **memory/memory_root** | ✅ | single boot allocation; double-buffered frame stacks; sub-arenas; memory map |
| **ecs/world** | ✅ | sparse-set World; spawn/despawn w/ generation guard; add/get/remove/has; `each<>`; deferred despawn |
| **core/log** | ✅ | leveled macros, fixed-buffer formatting, swappable sink; compile-time floor |
| **core/config** | ✅ | immutable boot config; `from_defaults()` fills budgets from caps; `validate()` |
| **core/time** | ✅ | `StepAccumulator` (step count, spiral clamp, alpha); `fixed_dt` — tier-agnostic |
| **runtime/app (loop)** | ✅ | boots MemoryRoot+platform, **owns World+Renderer+InputState**, fixed-step loop, Game hooks via `App&` accessors; runs headless deterministically |
| **platform** | ✅ | C seam + **`null`** (headless fb + scripted input + file I/O), **`sdl`** (real window: soft-fb→texture or GL context, keyboard→buttons, monotonic clock, **+ audio device w/ a fill callback**) behind `PHX_HAVE_SDL`, **`gba`** (Mode 3 fb + RGBA8→BGR555 blit + keypad + VBlank, `PHX_TARGET_GBA`), and **`psp`** (480×272 8888 fb + sceCtrl + VBlank, `PHX_TARGET_PSP`). The GBA/PSP backends cross-compile into a real ROM / EBOOT |
| **render** | 🟡 | unified API + front end (record/sort/dispatch) + **software backend** (sprites & tilemaps, per-channel tint + dest scaling, texture load/**unload** w/ slot recycling) — the golden reference — **a desktop GL backend** (GL 1.1 immediate-mode port of the same geometry, behind `PHX_HAVE_GL`), **and a GBA-native PPU backend** (`src/gba/gba_ppu.cpp`): quantizes RGBA8 atlases into 4bpp paletted 8×8 tiles + a 16-colour BGR555 palette, tilemaps→text-BG screen entries, sprites→OAM, then `ppu_model.h`'s pure `ppu_compose` rasterizes the exact frame the silicon would scan out — enforcing the real GBA limits (≤16 colours, 8px tile alignment, the 128-OBJ ceiling). Verified headlessly through the same `Renderer` (`make ppu`) and cross-compiles for ARM7TDMI. **And a PSP-native GU backend** (`src/gu/gu_backend.cpp` + `gu_model.h`): records the frame as GU sprite quads (textured, nearest-sampled, alpha-tested, vertex-colour modulated) and `gu_compose` rasterizes them — full-colour, so its output is **bit-identical to the software reference** (verified by `make gu` rendering the render_test scene + cross-compiles for MIPS Allegrex). Adds a **budget-bounded LRU `TextureCache`** (keyed by an opaque id; evicts least-recently-used to a byte budget, recycling renderer slots) — kept in `render` so `resource` stays render-free per the dependency law. All four backends (soft/GL/GBA-PPU/PSP-GU) share the one front end. The **GBA PPU now has its real hardware submission path** (`submit_hardware()` under `PHX_TARGET_GBA`: 4bpp tiles→VRAM, palette→PALRAM, map→screenblock, OBJ→OAM, Mode-0 DISPCNT; shipped as `make gba-ppu`, every byte verified against the model via the mGBA GDB stub — see #29). The **PSP GU likewise has its real sceGu display-list path** (`submit_gu()`: `sceGuDrawArray(GU_SPRITES,…)`, `make psp-gu`, pixel-verified on PPSSPP by an on-PSP eDRAM readback vs `gu_compose` — see #32). **All four backends now run on their native targets**; no GPU-submission work remains |
| **input** | ✅ | `phx_input_raw` → semantic `Button`/held/pressed/released edges + axis normalize; tier-agnostic |
| **physics** | ✅ | `Transform`/`Body`/`AABBColl` components; `PhysicsWorld`: axis-separated swept AABB-vs-tilemap (gravity/land/wall/bonk) + n² overlap pass + point query. No alloc; depends only on core+ecs |
| **anim** | ✅ | `AnimClip`/`SpriteSheet`/`Animator` components + `AnimStateMachine` (data-driven edges); `AnimationSystem::tick` advances frames (loop/clamp) and writes the source rect. Scalar timing; depends only on core+ecs |
| **scene** | ✅ | LIFO `SceneStack` (deferred push/pop/replace, enter/exit/pause/resume, transparent update/render runs) + persistent `Blackboard` + per-scene `StackAllocator` rollback. Opaque `EngineCtx*` keeps it off `runtime`; depends only on core+memory |
| **ui** | ✅ | immediate-mode `UI`: bitmap-font `text`, `image`, `rect`, HUD `bar`, focus-ring `button` (D-pad nav, no pointer needed). Emits `DrawSprite`s (tinted/scaled); depends on core+render+input |
| **resource** | ✅ | `.phxp` bundle format + **`ResourceCache`: mount via seam, binary-search TOC, zero-copy texture/tilemap/sprite/blob views**, **+ per-asset LZSS compression** (decompressed once into the arena on first access, cached; uncompressed assets stay zero-copy). The `Sprite`/`Spawns`/`Sound` assets carry anim-clip / spawn-point / PCM data as POD the game maps onto `anim`/`ecs`/`audio` (so `resource` depends on none of them) |
| **tools/phxpack** | ✅ | bundle writer lib (**`--compress`: LZSS each blob, kept only where it shrinks**) + **CLI: PNG/PPM→texture, tilemap-CSV→tilemap, `.sprdef`→sheet + anim clips, `.tmj`→Tiled tilemap + spawns, `.wav`→sound → `.phxp`**. Ships dependency-free decoders for **DEFLATE/zlib + PNG** (8-bit gray/RGB/palette/RGBA, all 5 filters), **JSON + Tiled**, and **RIFF/WAV** (8/16-bit, mono/stereo → mono16). The asset converters are complete |
| **core/pixel** | ✅ | shared `Rgba`/`rgba()`/`PixelFormat` (so render & resource don't depend on each other) |
| **audio** | ✅ | software `AudioMixer`: SoA voices (count from `caps.audio_channels`), per-voice gain+pan, Q16 nearest-sample resampling, loop, music bus, generation-guarded handles. **+ streaming**: an SPSC `RingBuffer` + `AudioStream` (pump() resamples a source — incl. a zero-copy bundle blob — into the ring off the audio path; the music bus drains it 1:1, silence on underrun). All-integer → byte-identical on both tiers. `mix()` fills a stereo buffer (pure; no device). **+ a lock-free SPSC `AudioCommandQueue`** (game thread push → audio-thread drain into the mixer) so device playback never races; `stop_music()`. The SDL backend's audio device runs a game-supplied fill callback that drains the queue + mixes. Depends only on core+memory |
| **examples/platformer** | ✅ | full game on real modules **driven entirely by the asset pipeline**: its level geometry + entity placement come from a baked **Tiled map (`Tilemap` + `Spawns`)**, the hero animation from a **`Sprite`** asset, and jump/coin SFX from **`Sound`** assets through an `AudioMixer` — nothing gameplay-relevant is hardcoded. Title/level/pause `SceneStack`, player + physics + anim + coins + HUD. Builds as a production binary (`make`) and a headless test (`make platformer`) on both tiers |

**Tests:** `tests/` — unit: fixed (7), memory (9), ecs (8), time (5), input (4), physics (6),
anim (6), scene (8), ui (2), audio (12), stream (9 — ring SPSC + AudioStream pump/loop/
resample/underrun + mixer streaming), lz (10 — codec round-trip + corrupt-input guards),
png (5 — DEFLATE inflate + PNG decode/reject), json (4 — parse/escapes/reject-malformed),
wav (4 — 16-bit mono/stereo downmix + 8-bit + reject), cmdqueue (4 — lock-free audio command
queue drains into the mixer, full-without-overrun); plus fourteen headless integration
binaries: **app smoke**
(100-frame loop), **render smoke** (→ `build/render_out.ppm`), **playable** (entity driven by
input), **physics** (body falls onto a tile floor), **anim** (animator advances a sprite-sheet
frame), **scene** (menu overlay over gameplay), **ui** (text + HUD bar + focus-ring menu, with
scripted D-pad/A nav), **platformer** (the whole example game played by a scripted controller),
**audio** (bundle PCM blob → mount → mix), **texcache** (drive a real Renderer through the
budget-bounded LRU cache: hits skip re-upload, the byte budget evicts LRU, freed slots are
recycled across 1000 cycles, oversize assets rejected), **png** (decode a real PNG → bake →
mount → render), **sprite** (decode a sheet PNG → bake Texture+Sprite → mount → build an
Animator → render a chosen frame), **tiled** (parse a Tiled `.tmj` → bake Tilemap+Spawns →
mount → render tiles + resolve spawn points by type), and **resource** (bake raw + LZSS →
mount → decompress → views → render; asserts the compressed bundle is smaller and decodes
identically) — each verified by reading the framebuffer, samples, and/or ECS. `phxpack` is
exercised by an end-to-end CLI bake (both raw and `--compress`). All green on both `TIER`s.

---

## What was verified this session

1. `core` + `memory` + `ecs` compile clean with `-Wall -Wextra -Wpedantic` on g++ 15.
2. The **same source** compiles and passes with `scalar = float` (PC) and
   `scalar = fixed16` (GBA tier) — the portability thesis holds at the foundation.
3. Allocators are O(1) and non-growing: a 200k-op pool churn and a 20k-op ECS
   spawn/despawn churn both stay flat (anti-fragmentation proof).
4. ECS generation handles correctly reject stale entities after slot recycling.
5. The module dependency graph is acyclic and respects the documented layering
   (gate: `tools/common/depcheck.py`).
6. **Tile physics** resolves correctly on both tiers: a body under gravity lands flush on
   a tile floor (`on_ground`, no tunnelling), stops dead against a wall, and bonks its head
   on a ceiling; a collider-less body free-falls through; the overlap pass and point query
   report the right hits. Fixed and float land on the same resting position here.
7. **Sprite animation** is frame-correct on both tiers: an Animator advances at its clip's
   fps, loops or clamps-and-finishes as authored, the state machine transitions on triggers,
   and the computed source rect drives the exact on-screen frame. Frame timing is `scalar`,
   so fixed and float reach the same frame at the same step.
8. **Scene stack** behaves correctly: push/pop fire enter/exit/pause/resume in the right
   order, deferred ops applied from inside a scene's own update are safe, transparent scenes
   route update/render to the run beneath, replace swaps the top, the Blackboard survives
   scene changes, and leaving a scene rolls its scene-arena back to the mark (O(1), no leak).
   An App-loop integration pushes a menu overlay over gameplay and both composite on screen.
9. **UI** renders and navigates: bitmap text lands the right tinted glyph pixels (space draws
   nothing), the HUD bar fills fg/bg correctly, and a scripted D-pad tap moves the focus ring
   while A activates the focused button — all read back from the framebuffer + UI state. The
   software backend gained per-channel tint + dest scaling (used by UI; the gameplay golden
   tests still pass unchanged), and the null backend gained a per-frame input script for
   deterministic edge-driven UI tests.
10. **The whole game works end to end** (capstone): from a baked bundle, a scripted controller
    enters the level, walks right and **collects 2 coins** (physics overlap → score), **jumps**
    (input → physics), **pauses and resumes** (scene stack), and sees a **HUD** — finishing on
    the ground at x=59 with the stack back to depth 1. The float and fixed builds reach the
    **same** score, position, and ground state: determinism holds across the entire game, not
    just isolated systems (docs/00 R2 closed for the MVP feature set).
11. **SDL backend** implements the full C seam and was syntax-checked against SDL2's API (via
    a faithful stub) and compiles with **zero warnings**; with `PHX_HAVE_SDL` undefined the
    translation unit is empty (0 symbols), so it's harmless in headless builds and `make check`
    is unchanged (depcheck still acyclic). It cannot be *run* in this env (no SDL2/display) —
    `make sdl` builds + runs it where SDL2 is installed. Its texture format (`ABGR8888`) matches
    `phx::Rgba` byte order, so the soft framebuffer uploads with no per-pixel conversion.
12. **GL backend** implements the `IRenderBackend` seam and was syntax-checked against
    `<GL/gl.h>` (faithful stub) + the SDL GL context path — both compile with **zero warnings**;
    with `PHX_HAVE_GL` undefined the TU is empty (0 `make_render_backend` symbols), so headless
    builds are unaffected and `make check` is unchanged (depcheck still acyclic, 26 edges). It
    is a 1:1 geometry port of the software backend, so the soft golden images remain the GL
    correctness oracle. Not runnable here (no SDL2/GL/display); `make gl` builds it where the
    libraries exist.
13. **Audio mixer** is verified headlessly and deterministically: silence-when-idle, unity
    passthrough, volume scaling, hard pan, voice summing, non-loop end (voice auto-frees),
    loop wrap, half-rate resampling (samples doubled), the voice ceiling (extra play → no
    voice), generation-guarded stop (stale handle is a no-op), and music-volume scaling — 12
    unit cases, plus a bundle→`SoundView`→`mix` integration (content path feeds audio). The
    pipeline is all-integer, so PC and GBA tiers mix **byte-identical** output.
14. **Bundle compression** (LZSS) round-trips exactly across input shapes — long runs (via
    overlapping matches = RLE for free), periodic structure, repeated text, and incompressible
    high-entropy bytes — and the decoder rejects hostile/corrupt input (input underrun, a
    back-reference before the output start, a match that would overrun the output) by returning
    0 without reading or writing out of bounds (10 unit cases). End to end, the `resource` test
    bakes the *same* assets raw and `--compress`ed: the compressed bundle is smaller on disk yet
    every view decodes byte-identically and renders the same framebuffer. The codec is
    all-integer, so the stored bytes and the decode are **identical on both scalar tiers**; the
    decoder is allocation-free and the cache decompresses each compressed asset once into the
    arena (uncompressed assets stay zero-copy).
15. **Budget-bounded LRU texture cache** works against a *real* Renderer: a cache hit returns
    the same `TextureId` without re-uploading; under a byte budget that holds two 8×8 textures,
    requesting a third evicts the least-recently-used one (resident bytes stay ≤ budget);
    1000 churning uploads all succeed, proving the renderer **recycles freed texture slots**
    rather than leaking them (the 256-slot backend would otherwise exhaust); an asset larger
    than the whole budget is rejected and surfaced in `stats().oversized`; and `evict_unused()`
    drops textures not touched since the previous frame boundary. The renderer's independent
    `live_textures()` count confirms eviction frees GPU resources, not just bookkeeping. The
    cache lives in `render` (keyed by an opaque id), so `resource` stays render-free and the
    dependency law holds (28 edges, still acyclic).
16. **Audio streaming** works end to end: the SPSC `RingBuffer` round-trips across a wrap
    boundary and clamps over/under-fill; `AudioStream::pump()` resamples a source into the ring
    (1:1 at matched rates, 2× on a half-rate source via the same nearest-sample Q16 cursor the
    mixer uses), loops to keep the ring full, and on a non-loop source ends production yet stays
    `active()` until the buffered tail is drained; a consumer read past the buffered count
    returns a partial count (no garbage). The mixer's music bus drains a stream 1:1 honouring
    `set_music_volume`, and an underrun mixes clean silence. The integration streams a *mounted
    bundle blob* (zero-copy source → ring → mix), so the content path bake → mount → stream →
    mix is proven without a resident copy. All-integer → identical on both tiers.
17. **PNG asset decoding** works end to end. The pipeline ships a complete, dependency-free
    **DEFLATE/zlib inflate** (stored + fixed- + dynamic-Huffman blocks, LZ77 back-references)
    verified against a real zlib stream (dynamic Huffman) and a hand-built stored block, and a
    **PNG decoder** (chunk parse → inflate IDAT → unfilter → RGBA8). The decoder was confirmed
    against python ground truth across **all five scanline filters** (None/Sub/Up/Average/Paeth)
    and 8-bit gray/RGB/palette/RGBA color types; malformed inflate/PNG inputs are rejected, not
    crashed. The integration decodes a real PNG, **bakes it into a `.phxp`, mounts it, and draws
    it**, asserting the PNG's colours reach the framebuffer; the `phxpack` CLI bakes a real `.png`
    file too. Both are host-only tools (STL), outside the engine dependency graph.
18. **Sprite-sheet baking (`phxsprite`)** closes the authored-animation loop. A `.sprdef`
    (sheet PNG + frame size + named clips) bakes to a `Texture` + a `Sprite` asset; the runtime
    `SpriteView` exposes the frame grid + clip table, and the game builds an `anim::Animator`
    straight from it. Verified end to end: decode the sheet → bake → mount → resolve clips by
    name ("walk"/"idle") → drive the `anim` system to frame 2 of "walk" → assert the computed
    source rect (4,0,2,2) and that **the blue frame renders to the framebuffer** from the baked
    sheet. The `Sprite` blob is POD in `resource`, so it carries animation data with **no
    `resource`→`anim` dependency** (the game does the mapping, like TextureView→renderer). The
    CLI bakes a `.sprdef` end to end. Depcheck unchanged (28 edges, acyclic).
19. **Tiled map import (`phxtile`)** brings authored levels in. A dependency-free **JSON parser**
    (objects/arrays/numbers/strings/bools/null, string escapes incl. `\u`, malformed-input
    rejection — 4 unit cases) feeds a **Tiled `.tmj` importer**: tile layers become the engine's
    `Tilemap` (Tiled's `firstgid==1` GIDs map straight onto the 0=empty/tile+1 convention; flip
    bits stripped), and object groups become a `Spawns` table (typed placement rects). Verified
    end to end: parse the `.tmj` → bake Tilemap + Spawns + a tileset → mount → assert the tile
    indices + tileset ref, **render the map** (correct tile colours on the framebuffer), and
    **resolve spawn points by type** ("player"/"coin" at the authored pixel coords). `Spawns` is
    POD in `resource` (no anim/ecs dependency); the CLI bakes a `.tmj` end to end. Collision needs
    no separate asset — the physics TileGrid treats solid tiles in the map directly.
20. **WAV audio import** completes the converter set. A dependency-free **RIFF/WAVE decoder**
    (uncompressed PCM, 8-bit unsigned or 16-bit signed, mono or stereo) downmixes to mono 16-bit
    at the file rate — verified by 4 unit cases (mono passthrough, stereo downmix, 8-bit→16-bit
    centering, non-RIFF rejection) and independently against a python `wave`-generated stereo WAV
    (byte-exact, incl. truncation-toward-zero downmix). The audio integration **decodes a WAV,
    bakes it as a `Sound` asset, mounts it, wraps the `SoundDataView` as an audio `SoundView`, and
    mixes it** (L/R == 1500); the CLI bakes a `.wav` end to end. `Sound` is POD in `resource`, so
    there is no `resource`→`audio` dependency (the game wraps the view, like the others).
21. **The example game now runs entirely off the asset pipeline.** Its bundle is baked through
    the *real importers*: the level is authored as a **Tiled `.tmj`** (tile layer → `Tilemap`,
    object group → a `Spawns` table of player/coin points), the hero animation is a **`Sprite`**
    asset (frame grid + idle/walk clips), and the jump/coin SFX are **WAVs → `Sound`** assets.
    At runtime the game spawns its entities from the `Spawns` table, builds the `Animator` from
    the `SpriteView`, and plays SFX through an `AudioMixer` on jump/coin (a 128-frame block is
    pulled each render, standing in for the device callback). The headless capstone reaches the
    **same** outcome as the old hardcoded version (score=2, player_x=59, on_ground, depth=1) on
    **both tiers**, and now additionally asserts the SFX fired (`sfx=2`) and the mixer produced
    non-silent output (`audio_peak=9000`) — so the pipeline isn't just unit-tested, it *feeds the
    shipping game*. Depcheck unchanged (28 edges; the example is outside the engine graph).
22. **The engine cross-compiles to a real Game Boy Advance ROM.** Every portable module (core,
    memory, ecs, render front + software rasterizer, physics, anim, scene, ui, audio, resource,
    runtime — 15 TUs) compiles clean with **devkitARM g++ 15.2** for ARM7TDMI (`-mthumb`, the
    GBA fixed-point tier). A new **GBA platform backend** (`engine/platform/src/gba/`) implements
    the C seam over raw MMIO — BG Mode 3, the soft framebuffer converted RGBA8→BGR555 and blitted
    (integer-scaled) to VRAM, keypad→canonical buttons, VBlank-locked clock, ROM-embedded bundle
    for the FS-less console. `make gba` links the portable engine + backend + a render smoke into
    a **19 KB `.gba`** with a valid header (gbafix logo + checksum). Not *run* here (mGBA is GUI
    and there's no display), but it is a structurally-valid bootable ROM.
23. **…and to a real PSP EBOOT.** The same portable engine compiles with **pspsdk psp-g++ 15.2**
    for MIPS Allegrex; a **PSP platform backend** (`engine/platform/src/psp/`) implements the seam
    via sceDisplay/sceCtrl (480×272, 8888 — byte-identical to the soft fb, so no conversion;
    VBlank sync). `make psp` produces a **194 KB `EBOOT.PBP`** (PRX link flow + mksfo/pack-pbp).
    Three architectures (x86, ARM7TDMI, MIPS), one unchanged codebase.
24. **Audio device glue is complete.** A lock-free SPSC `AudioCommandQueue` lets the game thread
    push play/stop/volume intents that the audio thread drains into the mixer right before mixing,
    so all mixer mutation is single-threaded — verified by 4 unit cases (drain→voice state→mixed
    output; reports full without overrun). The **SDL backend** gained a real audio device
    (`SDL_OpenAudioDevice` + a fill trampoline that runs a game-supplied callback), authored and
    syntax-checked against the SDL2 API (zero warnings, exports `phx_sdl_audio_start/stop`); like
    the rest of the SDL/GL backends it can't be *run* here (no SDL2). Layering holds: the platform
    owns the device but not the mixer (the game supplies the fill).

25. **A GBA-native PPU render backend — verified headlessly.** The first hardware render
    backend (render tier 0) speaks the GBA's actual language instead of CPU-rasterizing 24-bit
    sprites: `upload_tex` quantizes each RGBA8 atlas into 4bpp paletted 8×8 tiles sharing one
    16-colour BGR555 palette, `upload_map` turns tilemaps into text-BG screen entries, and
    `submit_sprites` turns the sorted draw list into OAM (OBJ) entries — all honouring the real
    machine limits. The crux for verifiability is `engine/render/src/gba/ppu_model.h`: a pure,
    host-compilable **behavioural model of the PPU + `ppu_compose()`** that rasterizes those
    tiles/map/OAM/palette to RGBA8 *exactly as the silicon would* (colour index 0 = transparent,
    scrolled+wrapped text BG under 2D-mapped OBJ, 15-bit colour). So the entire GBA visual model
    is exercised on the host: the `ppu` integration drives the **same `Renderer` front end** the
    soft backend uses (just linked against the PPU backend) and asserts tile colours, transparency,
    sprite compositing, H/V flips, scrolled wrap, **and every constraint that would bite on
    hardware** — a >16-colour palette is rejected, non-8px-aligned art is rejected, and a 130-sprite
    frame drops 2 at the 128-OBJ ceiling — 28 checks, identical on both scalar tiers. The backend
    also **cross-compiles clean for ARM7TDMI** (devkitARM, 2840 B text, zero warnings). Because it
    composes into the platform's RGBA8 framebuffer, it runs today on any software-tier platform
    (null/SDL, and the GBA via the existing Mode-3 blit); the remaining step is the unverifiable-
    here optimization of DMAing the same tile/OAM/palette data into VRAM/OAM/PALRAM so the PPU
    scans it out in silicon (`ppu_compose` is that path's golden oracle). Depcheck unchanged (28
    edges — the backend lives in `render`, touching only `render`+`platform` like the soft backend).

26. **A PSP-native GU render backend — verified headlessly, bit-identical to soft.** The
    second hardware render backend (render tier 1) targets the PSP Graphics Unit. Unlike the
    GBA PPU (which must quantize to 4bpp/15-bit), the PSP GU is a full-colour textured-triangle
    GPU, so the backend records the frame as **GU sprite quads** (a textured, nearest-sampled,
    alpha-tested, vertex-colour-modulated source→dest rect per tile/sprite) and `engine/render/
    src/gu/gu_model.h`'s pure **`gu_compose()`** rasterizes them. Its rasterization is
    deliberately bit-identical to the software backend's blit (same nearest source stepping,
    alpha-0 skip, per-channel tint), so the **soft golden images are its exact oracle**: the `gu`
    integration renders the *same scene as render_test* through the GU backend and asserts the
    *same pixels* (blue/yellow tiles + a red sprite + clear background), then additionally checks
    dest scaling, tint modulate, and H-flip — 16 checks, both scalar tiers. It **cross-compiles
    clean for MIPS Allegrex** (pspsdk psp-g++, 3203 B text, zero warnings) and, like the PPU
    backend, composes into the platform framebuffer so it runs today on any software-tier
    platform; the remaining step is emitting the same quads as a `sceGuDrawArray(GU_SPRITES,…)`
    display list on hardware (`gu_compose` is its golden reference). Depcheck unchanged (28 edges
    — the backend lives in `render`, touching only `render`+`platform`). **Both console-native
    render backends (GBA PPU tier 0, PSP GU tier 1) now exist and are verified on the host**;
    every target shares the one Renderer front end.

27. **The full example platformer now builds as a real GBA ROM** (`make gba-platformer` →
    `build/gba/phx-platformer.gba`, ~123 KB). It links the **same** `examples/platformer`
    game + every engine system (ecs, render+soft rasterizer, physics, anim, scene, ui, audio
    mixer, resource cache, runtime loop) for ARM7TDMI — only the entry point differs from the
    host `main.cpp`: (1) the `.phxp` bundle is baked offline on the host (`bake_main` →
    `build/gba/platformer.phxp`, 18.6 KB) and linked into cartridge ROM via `bin2s` (a GBA has no
    filesystem), then handed to the platform seam with `phx_gba_set_bundle()` so the game's
    `res->mount()` finds it; (2) budgets are sized for the GBA's 256 KB EWRAM — a 120×80
    framebuffer (the backend 2×-scales to 240×160), a 160 KB engine arena, 256 entities, and a
    16 KB scene-scratch (the host used 32 MB / 256 KB). The only example change was making the
    scene-scratch size a field (default 256 KB; the GBA entry sets 16 KB) — the host suite is
    unchanged (PLATFORMER PASS, both tiers). Footprint fits: statics are 8.8 KB of 32 KB IWRAM,
    runtime EWRAM use ≈202 KB of 256 KB. Compiles + links clean (zero warnings) and gbafix
    validates the header.

28. **The GBA platformer ROM is now *verified actually running* — headlessly, on the emulated
    hardware.** The only emulator here (`mgba-qt` 0.10.5) is GUI-only, but it runs under
    `QT_QPA_PLATFORM=offscreen` and exposes a **GDB stub** (`mgba-qt -g`). Driving it with
    `arm-none-eabi-gdb` (symbols from `platformer.elf`) + dumping Mode-3 VRAM (`0x06000000`,
    240×160 BGR555) to a PNG gives a fully headless run-and-screenshot loop. With this we
    confirmed on emulated ARM7TDMI: the title renders ("PRESS START"), the fixed-step loop +
    `gba_poll_input` run every frame, and **injecting a Start press transitions title→level and
    renders the level** (SCORE HUD + health bar + player + 3 coins — verified by reading the
    framebuffer back as an image).
    This surfaced — and fixed — a **real GBA-only hang invisible to every headless host test**:
    `phx::type_id<T>()` (core/types.h) holds a function-local `static` with a *runtime*
    initializer, so GCC wrapped it in `__cxa_guard_acquire`, whose devkitARM newlib lock/pthread
    stubs **deadlock on bare metal** — the *first* `World::add<>()` in `LevelScene::on_enter` spun
    forever (title frozen, "Start does nothing"). The host build uses real pthreads, so it never
    hit this. Fix: build the console targets with **`-fno-threadsafe-statics`** (GBA + PSP — both
    single-threaded; statics still init on first use, just without the lock). ROM shrank to
    ~114 KB; host suite unchanged (PLATFORMER PASS, both tiers). The methodology (offscreen Qt +
    GDB stub + VRAM dump) is now the way to verify any console ROM here without a display.

29. **The GBA PPU backend's hardware submission path is implemented — and verified on emulated
    silicon.** Previously `gba_ppu.cpp` only *composed* the frame on the CPU (`ppu_compose` into
    the soft framebuffer); its `end()` carried a "HARDWARE NOTE" for the unwritten real path. That
    path now exists, under `#if defined(PHX_TARGET_GBA)`: `submit_hardware()` programs the actual
    PPU — the 4bpp tiles packed into BG charblock 0 (`0x06000000`) and duplicated into OBJ tile
    VRAM (`0x06010000`), the shared 16-colour palette into both the BG and OBJ palette banks
    (`0x05000000`/`0x05000200`), the text-BG map into screenblock 24 (`0x0600C000`), sprites into
    OAM (`0x07000000`) with hardware shape/size, and `DISPCNT = Mode 0 | BG0 | OBJ | 1D` + `BG0CNT`
    + scroll — using only 16/32-bit writes (VRAM/PALRAM/OAM forbid byte writes). A platform hook
    (`phx_gba_set_direct`) makes `present()` skip its Mode-3 blit so the silicon scans VRAM/OAM out
    directly. A new ROM (`make gba-ppu` → `build/gba/phx-ppu.gba`, ~20 KB) draws a 30×20
    checkerboard + an OBJ sprite through the **same portable `Renderer`** front end, now linking the
    PPU backend instead of the software rasterizer. **Verified headlessly** with the mGBA GDB stub:
    every byte of hardware state — DISPCNT `0x1140`, BG0CNT `0x1800`, the four palette entries
    (`1463 6545 173b 14bb`), the packed tiles (`0/11111111/22222222/33333333`), the screenblock
    checkerboard (`2 1 2 1`), and the OAM entry (`y=0x4C x=0x74 tile=3`) — exactly matches what the
    `ppu_compose` model specifies. Since `ppu_compose` is already proven bit-identical to the
    software golden reference (`make ppu`, 28 checks) and mGBA's PPU scan-out is well-tested,
    correct hardware state ⟹ correct scanned-out frame. The `#else` (host/software-tier) compose
    path is unchanged — `make check` and `make ppu` still pass; depcheck unchanged (28 edges). This
    closes the GBA half of next-step #6; only the PSP `sceGu` display-list path remains (no PSP
    emulator here to verify it).

30. **The full example platformer now runs on the GBA PPU hardware backend — verified end to end
    on emulated silicon.** Where #29 proved the PPU submission path on a synthetic smoke, this
    drives the *whole game* through it: `make gba-platformer-ppu` → `build/gba/phx-platformer-ppu.gba`
    (~115 KB) links the **same** `examples/platformer` + engine + baked bundle but swaps the
    software rasterizer for `gba_ppu.cpp` at link time, so the game's tilemap → BG screenblock and
    its hero/coin/HUD sprites → OAM are programmed straight into the PPU. A dedicated entry
    (`gba_ppu_main.cpp`) renders at the native 240×160 (the PPU has no 2× upscale step, so the
    camera viewport is 240×160) and calls `phx_gba_set_direct(1)` *before* boot — which makes the
    platform both skip its Mode-3 blit and allocate only a 1×1 stub software framebuffer (the PPU
    path never touches it), keeping native resolution within the 256 KB EWRAM budget. To fit, the
    PPU backend's char/map stores were right-sized (`kTileCap` 1024→256, `kMapCap` 64²→32²; still
    ample — `make ppu`'s 28 checks unchanged). **Verified headlessly** via the mGBA GDB stub: the
    title boots with `DISPCNT = 0x1140` (Mode 0 + BG0 + OBJ), injecting Start transitions into the
    level and reaches `LevelScene::render` with **no hang**, and the level's hardware state is
    correct — the ground row of screenblock 24 reads `1 1 1 1 1 1` (ground tile) over an empty sky
    row `0 0 0 0`, and OAM holds the active hero/coin/glyph sprites. So the entire game — scene
    stack, physics, animation, HUD — runs on the GBA's native graphics hardware, not the CPU
    rasterizer. (Known PPU limits still apply: plain OBJ has no RGB tint or free scaling, so the
    tint/scaled bits of the HUD degrade — documented in `gba_ppu.cpp`.) Host suite unchanged
    (`make check`, 120336 checks; depcheck 28 edges); the soft `make gba-platformer` ROM still
    builds and runs.

31. **The PSP EBOOT now boots and runs on a real emulator (PPSSPP) — and two PSP-only bring-up
    bugs were found + fixed.** With PPSSPP 1.20.4 (the 2021 snap was abandoned at 1.12.3 and
    host-crashed every run; a current flatpak build is stable), `make psp`'s EBOOT loads as
    "Phoenix PSP Smoke" and runs on emulated MIPS Allegrex. Driving it headlessly (`flatpak run …
    --env=SDL_VIDEODRIVER=x11 -d`, debug log + PPSSPP's `IgnoreBadMemAccess=False` to surface the
    faulting PC) surfaced and fixed: (a) **no heap declaration** — pspsdk's default newlib heap is
    small, so the 130 KB framebuffer `malloc` succeeded but a later 256 KB `malloc` could return
    null; added `PSP_HEAP_SIZE_KB(8192)` + a null check (the heap is now 8 MB in-emulator). (b) the
    real crash: **`memset`/`memcpy`/`memmove`/`strlen` were linked to the kernel `sysclib` SYSCALL
    stubs in `libpspkernel.a`** (searched before newlib's `libc.a`); the compiler emits a `memset`
    to zero-init objects in placement-new, and PPSSPP's HLE of `sysclib_memset` returns 0 instead
    of `dest`, so a constructor wrote its result through NULL (`Write at 0x0, PC in
    phx_make_render_backend`) — a hard crash on boot. Fixed by **providing our own
    mem/str primitives** in `psp_platform.cpp` (a user module shouldn't call kernel sysclib anyway;
    correct on real hardware too), built with `-fno-tree-loop-distribute-patterns` so the loops
    aren't turned back into self-calls. After the fix PPSSPP runs the EBOOT with **0 bad accesses**,
    the fixed-step render loop spinning ~60 fps (819 `sceDisplayWaitVblankStart` + `sceCtrlRead`
    calls over 14 s, framebuffer set). The PSP **software** render path is thus verified running on
    emulated hardware (it's the same `soft_renderer.cpp` already proven pixel-exact on host/GBA).
    Note: external screen-grab can't capture PPSSPP's accelerated GL output, so the PSP `sceGu`
    path (next) is verified by an on-PSP framebuffer readback compared to the `gu_compose` model.

32. **The PSP GU backend's sceGu display-list path is implemented — and pixel-verified on emulated
    hardware (PPSSPP).** This closes the last hardware-render item (#6b). `gu_backend.cpp`'s `end()`
    now has a `#if defined(PHX_TARGET_PSP)` path (`submit_gu()`) that programs the real GU: a
    single-buffered 8888 eDRAM framebuffer, nearest sampling, `GU_TFX_MODULATE` (the per-quad tint
    as vertex colour), and an alpha *test* (cutout, no blend) — chosen to match `gu_compose`
    exactly. Each recorded quad becomes a 2-vertex `sceGuDrawArray(GU_SPRITES, …)` with texel UVs
    (per the pspsdk blit sample) sampling the RAM texture; `sceKernelDcacheWritebackInvalidateAll`
    makes the GU see the textures. A platform hook (`phx_psp_set_direct`) makes the PSP backend skip
    its `sceDisplay` setup + software blit so the GU owns the display. `make psp-gu` →
    `build/psp/gu/EBOOT.PBP` links the **same portable `Renderer`** with the GU backend instead of
    the soft rasterizer. **Verified pixel-exact on PPSSPP 1.20.4**: since external capture can't see
    PPSSPP's GL output, the entry (`examples/psp_gu/main.cpp`) renders a known scene, then **reads
    the eDRAM framebuffer back on-PSP** and checks tile/sprite/clear pixels against the colours
    `gu_compose` (the proven golden model) defines, reporting the verdict via a thread **name** in
    PPSSPP's log — the run produced `sceKernelCreateThread(GU_VERIFY_PASS, …)` with **0 bad
    accesses**, i.e. the real `GU_SPRITES` display list rasterized blue/yellow tiles + a red sprite
    on the dark-blue clear exactly as the model specifies. The `#else` (host/software-tier) compose
    path is unchanged — `make check`/`make gu` still pass; depcheck unchanged (28 edges).
    **All four render backends now run on their native targets** (soft everywhere, GBA PPU on
    ARM7TDMI silicon, PSP GU on Allegrec) — render next-step #6 is complete.

## Next steps (ordered — pick up here)

Done so far: ✅ null backend (framebuffer + scripted input + file I/O) · ✅ runtime/app
loop (owns World+Renderer+Input) · ✅ software renderer · ✅ input module · ✅ headless
playable slice · ✅ **asset pipeline** (`.phxp` writer/`phxpack` + `ResourceCache` reader,
render-from-bundle verified) · ✅ **tile physics** (AABB-vs-tilemap + overlap, gravity/land/
wall/bonk verified on both tiers) · ✅ **sprite animation** (clips + state machine →
source rect, verified on both tiers) · ✅ **scene stack** (push/pop/replace + overlay +
Blackboard + scene-arena rollback, verified on both tiers) · ✅ **immediate-mode UI**
(text/bar/menu + focus ring, verified on both tiers) · ✅ **example platformer** (the M1
capstone: full game on every system, headless + production builds, both tiers). **M1 is done.**

Toward M2 — making it visible + audible (the engine is feature-complete for 2D gameplay):

1. ✅ **platform: `sdl` backend (`src/sdl/`)** — DONE. Real window: it owns the software
   framebuffer, streams it to an SDL texture each `present()` (logical-size integer upscale),
   maps keyboard→canonical buttons (arrows/WASD, Z=A/jump, Enter=Start), and uses a monotonic
   perf-counter clock. Behind `PHX_HAVE_SDL`, linked INSTEAD OF `null`; `make sdl` builds the
   windowed example. **Authored + syntax-checked against SDL2's API; not run in THIS env (no
   SDL2/display here) — build it where SDL2 is installed to play.**
2. ✅ **render: GL backend (`src/gl/gl_backend.cpp`)** — DONE. A GL 1.1 immediate-mode port of
   the software rasterizer's geometry (textured, tinted quads; ortho pixel projection; nearest
   filtering; alpha blend) — no glad/glew, just `libGL`. Selected via `PHX_HAVE_GL` (linked
   INSTEAD OF the soft backend); the SDL platform creates a GL context + swaps buffers in that
   mode. `make gl` builds the GPU-rendered window. The software backend stays the golden
   reference. **Authored + syntax-checked against `<GL/gl.h>`/SDL's API; not run here (no
   SDL2/GL/display) — build with SDL2 + libGL to see it.**
3. ✅ **audio mixer** (`docs/10` §2) — DONE. Software `AudioMixer` (SoA voices, gain/pan,
   resampling, loop, music bus) verified headlessly (12 unit cases + a bundle→mix integration).
   *Remaining glue:* feed it to a real device via an SDL audio callback + a lock-free play/stop
   command queue (so the audio thread and game thread don't race), and trigger SFX from the
   example on coin/jump — both unverifiable in this env, deferred with the threading done right.
4. **resource compression + LRU caching** (`docs/06` §4, §6) — ✅ DONE. (a) Per-asset LZSS in
   the `.phxp` format (writer `--compress`; reader decompresses once into the arena, caches the
   result, leaves uncompressed assets zero-copy) — 10 codec unit cases + the raw-vs-compressed
   resource integration, byte-identical on both tiers. (b) A budget-bounded LRU `TextureCache`
   over the Renderer's load/**unload** (slot recycling), verified by the `texcache` integration
   (hits, budget eviction, slot reuse across 1000 cycles, oversize reject). (c) **Audio
   streaming** (`docs/06` §5): an SPSC `RingBuffer` + `AudioStream` whose `pump()` resamples a
   source (incl. a zero-copy bundle blob) into the ring off the audio path, drained 1:1 by the
   mixer's music bus — verified by 9 unit cases + the audio integration (stream a mounted blob).
   (d) **PNG decoding** (`docs/08`): a dependency-free DEFLATE/zlib inflate + PNG decoder wired
   into the `phxpack` CLI (`.png` → texture), verified by 5 unit cases + a decode→bake→render
   integration (and against python ground truth for all filter/color types). (e) **`phxsprite`**
   (`docs/08`): a `.sprdef` (sheet PNG + frame size + named clips) bakes a `Texture` + `Sprite`
   asset; the runtime `SpriteView` feeds an `anim::Animator`, verified by the `sprite` integration
   (decode → bake → mount → animate → render a frame). (f) **`phxtile`** (`docs/08`): a JSON
   parser + Tiled `.tmj` importer (tile layers → `Tilemap`, object groups → a `Spawns` table),
   verified by the `tiled` integration (render + spawn resolution). (g) **WAV decoder**
   (`docs/08`): RIFF/PCM → mono16 `Sound` asset, verified by 4 unit cases + the audio integration
   (decode → bake → mount → mix). **The asset converters (image, sprite, map, audio) are all
   complete.** Nothing converter-related remains; the open items are now purely environmental.
5. **toolchain bring-up** — ✅ DONE. devkitARM + pspsdk are installed here, so `make gba` and
   `make psp` cross-compile the portable engine into a real `.gba` ROM and a real `EBOOT.PBP`
   (each driving the software renderer through a new GBA/PSP platform backend). The fixed-point
   tier is validated on the actual ARM toolchain. *Remaining:* run them on emulator/hardware
   (needs a display or PPSSPP — absent here).

6. **hardware render backends** — 🟡 IN PROGRESS. Both console-native backends are built and
   verified headlessly via host-compilable model + compositor pairs: the **GBA PPU** (tier 0,
   `make ppu`, 28 checks; ARM7TDMI; see #25) and the **PSP GU** (tier 1, `make gu`, 16 checks,
   bit-identical to soft; MIPS Allegrex; see #26), both on both scalar tiers. (a) ✅ **GBA hardware
   submission path — DONE + verified** (see #29): `submit_hardware()` programs real VRAM/OAM/PALRAM
   + Mode-0 DISPCNT, shipped as `make gba-ppu`, every byte confirmed against the model via the mGBA
   GDB stub — and the **whole platformer now runs on it** (`make gba-platformer-ppu`, see #30),
   verified booting/transitioning/rendering its level through real VRAM/OAM. (b) ✅ **PSP `sceGu`
   display-list path — DONE + pixel-verified** (see #32): `submit_gu()` emits real
   `sceGuDrawArray(GU_SPRITES,…)` quads, shipped as `make psp-gu`, verified on PPSSPP 1.20.4 by an
   on-PSP eDRAM readback matching the `gu_compose` model (`GU_VERIFY_PASS`, 0 faults).
   **Render next-step #6 is complete** — every backend runs on its native target.

Bigger picture: **M2's goals are met.** Every engine system, the full offline asset pipeline, the
example game consuming all of it, and the audio-device glue are built and verified on the host
(both scalar tiers); and the one codebase now **cross-compiles to real GBA and PSP binaries**.
What remains is genuinely beyond this sandbox's reach: *executing* the ROM/EBOOT (no GBA/PSP
emulator with a display, no PPSSPP) and the SDL window/GL/audio backends (no SDL2 installed) —
all authored + (cross-)compiled, awaiting a machine that can run them. The next *new* engineering is
well underway: **both** console-native render backends now exist and are verified headlessly via
model + compositor pairs — the **GBA PPU** (tier 0, 4bpp tiles + OAM) and the **PSP GU** (tier 1,
textured quads, bit-identical to the software reference). What remains for them is only the GPU
submission paths (GBA VRAM/OAM DMA; PSP `sceGu` display list) so the consoles draw via their native
graphics units instead of the CPU rasterizer — finishable once a machine that can run the binaries
is available; both compose on-CPU and run on every software-tier platform today.

### Build commands (current)
```
make check                # full host suite (both via TIER), all integrations, dep gate
make ppu                  # GBA-native PPU backend: quantize->tiles+OAM->compose, verified headlessly
make gu                   # PSP-native GU backend: record sprite quads->compose (bit-identical to soft)
make test TIER=gba_sim    # foundation suite under the GBA fixed-point tier
make gba                  # devkitARM -> build/gba/phx-smoke.gba       (render smoke ROM, software rasterizer)
make gba-ppu              # devkitARM -> build/gba/phx-ppu.gba             (PPU hardware smoke: Mode-0 tiles+OBJ)
make gba-platformer       # devkitARM -> build/gba/phx-platformer.gba      (the FULL example game, software render)
make gba-platformer-ppu   # devkitARM -> build/gba/phx-platformer-ppu.gba  (the FULL example game on the PPU hardware)
make psp                  # pspsdk   -> build/psp/EBOOT.PBP            (real PSP EBOOT, software render)
make psp-gu               # pspsdk   -> build/psp/gu/EBOOT.PBP         (PSP GU hardware: sceGu display list)
make sdl / make gl        # windowed example (needs SDL2 [+libGL]; not installed here)
make depcheck             # architectural dependency-law gate
make clean
```
