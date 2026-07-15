# Phoenix project assessment

*Repository-wide review — updated 15 Jul 2026.*

Phoenix is a compact, from-scratch C++17 retro 2D engine whose real differentiator is one
gameplay codebase spanning a 256 KB GBA, PSP, Windows and Linux. It is already a convincing
vertical-slice engine; it is not yet a polished, externally consumable 1.0 product.

## Current verified state

`make check` passes (120,383 unit checks across 113 cases plus all Make integration gates —
the pipeline suite is at 135 checks and the platformer capstone at five scripted runs);
`make determinism` passes with 11 suite outcomes and a byte-identical rendered frame across
float and Q16 tiers (an earlier run in this series also caught and fixed a real GBA-tier `Mat3`
bug — see below); a fresh Release CMake build passes 21/21 CTest tests; `make release` passes the
whole check suite built with `PHX_BUILD_RELEASE=1`; `make sanitize` (ASan+UBSan) is clean. All
GBA ROMs (smoke, platformer, platformer-PPU, Emberwing soft+PPU) and PSP EBOOTs (smoke, GU,
platformer, Emberwing) rebuild warning-free after this update, and the GBA size-gate passes
(`make win` not re-run locally — MinGW is not installed on this machine; CI covers it). The
working tree began clean at the original review and after every workstream below.

| Metric | Value |
|---|---|
| Advertised platforms | 4 (GBA, PSP, Windows, Linux) |
| Render paths | 4 (software, GL, PSP GU, GBA PPU) |
| Registered unit cases | 113 |
| Practical release maturity | Pre-0.1 |

## Progress so far

- [x] **Align Make, CMake, CTest and CI** — Emberwing + Emberwing-PPU joined the CMake/CTest
  suite, CI now builds Emberwing on Windows/GBA/PSP and runs its size-gate.
- [x] **Harden bundle loading** — `mount()` now checks magic/version, full structural bounds, a
  real CRC32 (`BundleHeader.blob_crc32`), and target-tier match; added `unmount()`/
  `unmount_all()` lifecycle.
- [x] **Wire `PHX_BUILD_RELEASE`** — unconditionally set (+`NDEBUG`) for GBA/PSP in Make and
  CMake; `NDEBUG` now also strips asserts/logs on host Release; new `make release` gate runs the
  whole check suite under it.
- [x] **Correct documentation drift** — all six doc-vs-code gaps below (audio rate, GL/Vulkan,
  texture specialization, pipeline guarantees, tilemap formats, license status) fixed across
  `docs/00`, `02`, `03`, `06`, `07`, `08` and the diagrams.
- [x] **Resolve API footguns** — ECS deferred despawns now auto-flush once per fixed step in
  `App::run`; `Mat3::translate`/`scale` are defined (and a real GBA-tier identity-matrix bug the
  generic template was hiding is fixed); scene `Transition` is documented as a recorded-only
  hint, not a rendered effect.
- [x] **Make the drift-corrected designs real** — per-target texture bake (tier-0 4bpp
  paletted tiles + palettes, tier-1 GU-swizzled RGBA8; bundle format v2), the
  lock-file/incremental/manifest/`--upgrade` pipeline in `phxpack`, and the GL 3.3/Vulkan
  ambition dropped from the docs as a decided non-goal. Details below.
- [x] **Finish the 2D authoring loop** — done across two sessions. Per-tile collision
  metadata (solid/one-way/hazard) in physics + bundle + `phxtmap` authoring; SDL gamepad +
  engine-side `InputMap`; and now (this update) the four remaining gaps: **fill/rect/picker
  paint tools** in `phxtmap`, **prefab schemas** shared between `phxentity` and `phxtmap`
  (phxbin string fields + `--prefabs`), a **remap options UI with save persistence** in the
  shipped platformer, and **hazard tiles wired into shipped gameplay** via
  `tile_flags_in()`. Details below.
- [ ] **Physical target sign-off** — in progress. See below.

## What it is

The engine is intentionally narrow: sprites, tilemaps, parallax, fixed-step platformer physics,
frame animation, scenes, menus/HUD/dialogue, a software mixer and baked assets. It prioritizes
deterministic memory and behavior over general-purpose engine breadth.

**Architecture (dependencies point downward; `tools/common/depcheck.py` enforces the graph):**

```
Games and tools     Platformer · Emberwing · phxpack/converters · phxtmap · phxentity
L4 composition      runtime — App lifecycle and fixed-step loop
L3 gameplay systems  scene · physics · animation · immediate-mode UI
L2 services         render · input · audio · resource · ECS
L1 machine boundary  memory · platform C ABI
L0 closed foundation types · fixed/Q16 math · caps · logging · time · profiling
```

**Best evidence — Emberwing: Cinder Hollow.** A 320×20-tile, four-layer platformer level with
three enemy behaviors, checkpoints, hazards, collectibles, generated pixel art and chiptune
audio. It uses the same game sources across host, GBA and PSP entries and exercises the engine
more honestly than isolated subsystem demos — the restrictive architecture has been forced
through a real content and performance problem, not just a design document.

Remaining proof gap: its art, level and audio are generated from C++/ASCII sources, so the game
does not demonstrate the documented external editor → converter → pack workflow. Some mechanics
described as covered also need stronger scripted assertions. See `examples/emberwing/README.md`,
`examples/emberwing/src/game.h`, `STATUS.md`.

## Maturity by area

| Area | Assessment | What is already real | Main next step |
|---|---|---|---|
| Core + memory | Strong | Fixed/Q16 math, arenas, bounded allocators, fixed-step loop; `PHX_BUILD_RELEASE` wired (asserts/logs strip on GBA/PSP/host Release); Mat3 identity bug on the fixed16 tier fixed | Physical-hardware sign-off; enforce failure behavior |
| Architecture | Strong | 13-module acyclic graph; dependency law is a build gate | Add explicit platform conformance contract |
| Rendering | Strong for 2D | Software golden + GL + PSP GU + GBA PPU | Screen-space path, transitions, modern PC batching only if needed |
| Platform ports | Advanced | Linux/Windows/GBA/PSP paths exist and emulator/device seams were exercised; SDL now reads the first connected gamepad (hot-plug, OR'd with keyboard) | Physical GBA sign-off; PSP save/render smoke on real hardware; multi-pad support; pin toolchains |
| Gameplay systems | Functional, growing | Input (engine-side `InputMap` remapping + stick-to-dpad, plus `raw_pressed` physical edges for rebind capture), mixer, scenes, tile physics, animation, UI; ECS deferred despawns auto-flush every fixed step; per-tile collision flags (solid/one-way/hazard, `solid_from` still supported); **the platformer consumes hazard tiles via `tile_flags_in()` and ships a working remap options scene persisted in its save** | An actual transition-render implementation (honestly documented as unbuilt) |
| Resources | Strong | Packed typed assets, LZSS, zero-copy views; CRC32/bounds/tier validation + mount/unmount; per-target texture encode (tier-0 4bpp paletted, tier-1 swizzled; format v2); **tilemap blob now carries an optional per-tile-index collision-flags section**, byte-identical to before when unused | ADPCM/tracker audio codecs if ever needed |
| Asset tools | Usable, reproducible | Five CLIs, direct/2-stage bake, Tiled `.tmj`/PNG/WAV/JSON; lock-file incremental rebakes, manifest sidecar, `--upgrade`; Tiled collision-flag import; positioned JSON parse errors; `phxentity` schema API; **phxbin string fields (`str8/16/32` → NUL-terminated `char[N]` + `.gen.h` char arrays) enabling named prefab records** | Editor usability polish (below) |
| Editors | MVP, growing | Tile and table editors dogfood engine UI; collision authoring, undo/redo, add-layer, `--types` harvesting, positioned load errors; **fill/rect/pick paint tools** (4-connected flood fill, drag-marquee rectangle, eyedropper — all honouring the erase modifier); **`--prefabs` reads the placeable spawn vocabulary from a shared `phxentity` record table** | GUI tests, >8-layer editing (currently render-capped, though it still saves correctly), text entry for string cells (author them in JSON for now) |
| Games | Strong proof | Reference platformer plus Emberwing polished vertical slice; Emberwing now also proven on Windows/GBA/PSP CI | Content/polish if the goal is a shipped game, not only an engine proof |
| Release product | Pre-0.1 | MIT license, broad build surface, and a release-mode gate (`make release`) | Versioning, install/package flow, artifacts, contributor docs, release tags |

## Most important findings

**Resolved — build systems are now at feature parity.** CMake/CTest gained Emberwing +
Emberwing-PPU; CI now builds Emberwing on Windows, GBA and PSP and runs its size-gate; the docs
are explicit that the two GUI editors remain Makefile-only by design, not an oversight.

**Resolved — bundle loading no longer over-trusts.** `mount()` now checks magic/version, full
structural bounds on every TOC entry and blob before any pointer cast, a real CRC32
(`BundleHeader.blob_crc32`) over the TOC+blobs, and a target-tier match; `unmount()`/
`unmount_all()` exist and invalidate outstanding views.

**Resolved — release mode is wired.** `PHX_BUILD_RELEASE` is unconditionally defined
(+`NDEBUG`) for GBA/PSP in both Make and CMake; `assert.h`/`log.h` now also treat `NDEBUG` as a
synonym, so a CMake Release build gets the release assertion policy for free. A new `make
release` gate proves the whole check suite is clean under it.

**Resolved — documentation now matches the code.** All six drift points in the table below
(GBA audio rate, GL/Vulkan, texture specialization, pipeline guarantees, TMX/collision-shape
claims, license status) are corrected across `docs/00`, `02`, `03`, `06`, `07`, `08` and the
diagrams doc — each now distinguishes "as built" from "original design, not implemented"
instead of stating the design as fact.

**Resolved — the three concrete API footguns are closed.** ECS deferred despawns now
auto-flush every fixed step (no more silent no-op trap); Mat3 helpers are defined, which
surfaced and fixed a real near-zero-identity bug on the GBA fixed16 tier; scene `Transition`
is now documented as a recorded-only hint instead of a claimed working fade system. Detail in
the table below.

**Resolved — the drift-corrected designs are real (docs and code in lockstep).** The three
"designed, not implemented" pipeline items are now code, not aspiration:
per-target texture bake (`tools/phxpack/tex_encode.h` inside `BundleWriter`, bundle format
v2 — tier 0 bakes the PPU's native 4bpp paletted tiles + palettes, mirroring the upload
quantizer exactly so `make ppu` proves the baked path composes the byte-identical frame,
and shrinking Emberwing's texture data ~7.5× in the ROM; tier 1 bakes GU-swizzled RGBA8,
a pure reorder that keeps the GU bit-identical to the soft golden while binding zero-copy
with the swizzle bit); the lock-file pipeline (`<out>.lock` with tool/format versions +
per-input content hashes + output CRC32 → per-asset incremental rebakes proven
byte-identical to `--full`, "up to date" skip, `--manifest` sidecar, `--upgrade` re-bake
from the recorded source list, and a new pack-time refusal to merge intermediates baked
for a different tier); and the GL 3.3/Vulkan ambition **dropped from the docs for good**
(docs/03 §6 records the non-goal decision; docs/00/02/09 + graphics-engine.md follow).
Docs/00, 02, 03, 06, 07, 08, 09, graphics-engine and the diagrams now describe the
implemented behavior with no "designed, not built" texture/pipeline caveats left.

**Resolved — explicit per-tile collision metadata is real AND consumed by a shipped game.**
`TileGrid` supports two collision modes: the original `solid_from` index threshold (used
when no flags table is baked — fully back-compatible, byte-identical bundles for maps without
metadata), and an optional per-tile-index `TileFlags` byte (`kTileSolid`, `kTileOneWay`,
`kTileHazard`) imported from Tiled tileset `class`/`type`/boolean properties. One-way platforms
and hazard queries are proven through the *real* physics system in `tests/unit/test_physics.cpp`.
This update closed the remaining gap: **the platformer's level now bakes tileset collision
classes (ground solid, a lava tile hazard-not-solid) and its gameplay hurts the player through
`PhysicsWorld::tile_flags_in()`** — no spawned entity; the level data itself is the hazard —
proven by a scripted walk-into-the-lava-pit run (platformer suite run 3) plus a guard that the
golden playthrough never touches it. The spike stays an *entity* hazard deliberately, so both
authoring styles are exercised; Emberwing also keeps entity hazards by design (merged lava
rects with instant-kill semantics and timed geysers don't map onto a boolean tile byte).
**Caveat (unchanged):** once a map bakes any flag metadata, every solid tile must be
explicitly flagged — an unflagged non-empty GID becomes non-solid.

**Resolved — SDL gamepad input, an engine-side remap layer, and now a shipped options UI.**
The SDL backend reads the first connected controller (hot-plug aware, OR'd with keyboard).
`InputMap` on `App` remaps logical buttons and tunes the stick deadzone; `InputState` now also
exposes `raw_pressed`/`raw_held` — the pre-remap physical edge mask a rebind screen's "press
any button" capture needs (the remap shift is masked so even a corrupt saved map cannot be UB).
The platformer ships a working **options scene** (pause → OPTIONS): rebind JUMP/PAUSE by
pressing any button, cycle the stick deadzone, reset — and persists the `InputMap` in its
SaveData (v2), restored at boot. Proven end-to-end by two scripted suite runs driving the real
focus-ring UI: rebind JUMP to R, jump with R, save, "power cycle", assert the layout came back.
The exercise also caught a real immediate-mode UI trap now documented in docs/10: an options
overlay must not `render_below` over a menu with focus buttons, or the menu underneath eats
the same confirm press. **Caveat:** first-pad-only (no multiplayer), no rumble, GBA/PSP
backends untouched (their fixed layouts still flow through the same remap layer).

**Resolved — `phxtmap` has the full paint toolset and a shared prefab schema.** The
fill/rect/pick tools landed in the headlessly-tested document model (4-connected flood fill
and marquee-rectangle fill both report changed-cell counts so no-op gestures drop their undo
step; the eyedropper hops back to the brush) with GUI wiring (X+C cycles the tool, marquee
preview, status-line tool name) — the docs' "Planned" label is gone. **Prefab schemas** are
real via phxbin string fields: one record table with a `type` column is simultaneously
`phxentity`'s editable stats table, `phxtmap --prefabs`'s placeable-entity vocabulary
(`BinDoc::name_column()`), and a baked blob whose `char[N]` name the game hashes to match
spawn types — a third party adds an entity kind by adding one JSON record. **Still missing:**
GUI tests; >8-layer maps save correctly but only the first 8 render in the editor; string
cells display but aren't editable in the GUI (author them in JSON); prefab *component
introspection* (`PHX_REFLECT`) remains future work.

**Still open — memory discipline is strong but overstated.** The hot path is bounded and
arena-oriented, but the platform descriptor's root arena is passed as null and backends
allocate framebuffers/files separately. Desktop "map" is a loaded heap buffer, not an OS
memory map.

**Still open — several public seams remain placeholders.** Font is an asset enum without a
typed resource path; UI has no pointer widgets; physics remains intentionally
platformer-minimal (no ladders, slopes, or shape IDs — flags are boolean-per-tile-index only).
Scene transitions are still recorded but not rendered — that gap is now honestly documented
rather than silently overstated, but building the actual fade/slide render step is still open
work.

**In progress — physical target sign-off.** Real PSP hardware bring-up has already happened in
earlier sessions (see STATUS.md #41–43): a real user on real PSP hardware found and fixed three
emulator-invisible bugs — a kernel-import boot refusal (`8002013C LIBRARY_NOTFOUND`), a black
screen from `sceDisplaySetFrameBuf(..., SETBUF_IMMEDIATE)`, and a 5x game-speed bug from a stale
virtual clock (now `sceKernelGetSystemTimeWide`). What's still unconfirmed on real hardware:
PSP save persistence and render correctness specifically (audio+input were incidentally
confirmed working while chasing the display bug), and the entire GBA physical-cart path,
including whether battery SRAM actually survives a power cycle on a real cart — something
emulation structurally cannot prove. A `psp-fixup-imports` warning ("stubs out of order") shows
up on every current PSP build target and needs to be classified (benign toolchain quirk vs. a
recurrence of the #41 import-table class of bug) before treating PSP as fully signed off.

## API footguns — resolved this session

| Status | API | What was wrong | What changed |
|---|---|---|---|
| Fixed (wired) | ECS deferred despawns | `world.defer().despawn(e)` was tested in isolation (`tests/unit/test_ecs.cpp`) but never flushed by `App` or either game — a silent no-op trap for anyone who trusted the header comment | `App::run` now calls `flush_deferred()` automatically once per fixed step, right after `on_fixed_update`; proven end-to-end by a new assertion in `tests/suites/smoke_app.cpp` |
| Fixed (defined) | Mat3 helpers | `translate()`/`scale()` were declared with no definition anywhere (a link error waiting to happen) and had zero call sites | Defined inline on `scalar` directly; new `tests/unit/test_math.cpp` covers Vec2/AABB/Mat3 on both scalar tiers |
| Bug found + fixed | Mat3 identity (GBA tier) | The generic `template<class T> struct Mat3`'s identity default used a raw `T(1)`, which is `fixed16`'s RAW-bits constructor — the "identity" matrix was actually near-zero on the GBA fixed16 tier | Caught by `make determinism` once the new tests exercised both tiers; `Mat3` now uses the tier-safe `s_from_int(1)` idiom already used by `Camera2D::zoom` |
| Documented | Scene transition hint | Docs described a working "coroutine-free state machine" rendering a fading quad on every backend; `replace()`'s doc default (`Fade`) didn't even match the code default (`None`) | `Transition`/`last_transition()` are now documented as a recorded-only hint with zero consumers; the doc/code default mismatch is fixed |

## Documentation drift — fixed

The gaps below were the concrete evidence behind "documentation is ahead of the code." All six
are now corrected in place (docs rewritten to state current reality, with the original design
kept as an explicitly-labeled future-work note, not deleted).

| Status | Topic | Documentation said | Repository reality — docs now match |
|---|---|---|---|
| Fixed | GBA audio rate | Docs repeatedly said 16,384 Hz | Code/tests use 18,157 Hz (304 samples/video frame) — docs now match |
| Fixed | Desktop renderer | Architecture docs described GL 3.3/Vulkan | GL 1.1 immediate mode, no Vulkan — docs now say so, with the GL 3.3/Vulkan design kept as an explicitly-unbuilt note |
| Fixed | Asset specialization | Docs promised GBA 4bpp and PSP swizzled blobs at bake time | Texture blobs stay RGBA8 at bake; GBA quantizes during renderer upload — docs now distinguish target design from as-built |
| Fixed | Pipeline guarantees | Lock files, manifests and incremental content hashing were described as existing | None implemented in phxpack — docs now mark them designed-not-built |
| Fixed | Formats | TMX/XML and collision bitsets/shapes were described as supported | Tools accept TMJ/JSON only and use the last tile layer + `solid_from` as the collision convention — docs corrected |
| Fixed | Release state | STATUS said a license decision remained | Repository already contains an MIT LICENSE — STATUS's stale "still open" bullets corrected |

## Recommended development order

| Status | Priority | Workstream | Concrete scope | Exit condition |
|---|---|---|---|---|
| Done | P0 | Make release claims true | Aligned CMake/Make/CTest/CI; defined `PHX_BUILD_RELEASE`; corrected stale docs | A reproducible release candidate |
| Done | P0 | Harden bundle loading | Bounds-checked TOC/blobs, CRC32 checksum, target-tier validation, mount/unmount lifecycle | Malformed data fails safely |
| Done | P1 | Resolve API footguns | Wired ECS deferred-command flushing into `App::run`; defined `Mat3::translate`/`scale` (fixed a GBA-tier identity bug found in the process); documented scene `Transition` as a recorded-only hint | Public APIs cannot silently require hidden lifecycle steps or fail at link time |
| Open (in progress) | P1 | Physical target sign-off | Real GBA cart including SRAM power-cycle persistence; physical PSP input/audio/save/render smoke | 0.1 hardware evidence |
| Done | P1 | Make the drift-corrected designs real | Per-target texture bake (tier-0 PAL4 tiles, tier-1 swizzle; format v2), lock-file incremental pipeline (+manifest/`--upgrade`), GL 3.3/Vulkan dropped from the docs as a decided non-goal | Docs and code in lockstep; equivalence + reproducibility asserted by `make ppu`/`gu`/`render`/`pipeline`/`phxpack` |
| Done | P2 | Finish the 2D authoring loop | Collision metadata + `phxtmap` authoring; SDL pad + `InputMap`; positioned JSON errors; fill/rect/picker tools; prefab schemas (phxbin str fields + `--prefabs`); remap options UI persisted in the save; hazard tiles consumed by shipped gameplay (`tile_flags_in()`) | A third party can build a game without source-level workarounds |
| Open | P2 | Ship 0.1 properly | Version macros, changelog, install/export package, API reference, binary artifacts and tagged release | Consumable engine release |
| Later | Later | Expand scope carefully | Particles/palette FX first; then 2.5D/affine, jobs, scripting, new platforms | Growth without weakening the GBA-first design |

## Do not prioritize yet

Vulkan, scripting, jobs and many new platforms are attractive, but they would widen the
maintenance matrix before the current four-target release path is coherent. The best return is
hardening the existing small engine and making one third-party workflow excellent.

## Best next feature after hardening

The 2D authoring loop is closed: a third party can lay out a level with real paint tools and
per-tile collision in `phxtmap` (or Tiled), define entity kinds and stats in one shared prefab
table `phxentity` edits and `phxtmap` places from, bake it all with the reproducible pipeline,
have the engine's physics honour the authored collision (including hazard tiles with no code),
and ship a game whose players can rebind controls in-game and keep that layout across power
cycles. The two workstreams that remain P1/P2 are **physical target sign-off** (GBA cart SRAM
persistence; PSP save/render on real hardware; classify the `psp-fixup-imports` warning) and
**shipping 0.1 properly** (version macros, changelog, install/export package, API reference,
tagged release). Those, not more features, are the gate to a consumable release.

## Evidence index

**Architecture and contracts:** `README.md` · `docs/00-architecture.md` · `STRUCTURE.md` ·
`tools/common/depcheck.py`

**Build and verification:** `Makefile` · `CMakeLists.txt` · `tests/CMakeLists.txt` ·
`.github/workflows/ci.yml` · `tools/common/size_gate.py`

**Hardening targets:** `engine/resource/src/cache.cpp` ·
`engine/resource/include/phx/resource/bundle.h` · `engine/ecs/include/phx/ecs/world.h` ·
`engine/core/include/phx/core/assert.h` · `engine/runtime/src/app.cpp`

**Tools and roadmap:** `docs/08-tooling.md` · `tools/phxtmap/instructions.md` ·
`tools/phxentity/instructions.md` · `docs/09-roadmap.md`

**Documentation drift fixes:** `docs/00-architecture.md` (GPU/asset table) ·
`docs/02-platform-layer.md` (SDL GL ctx) · `docs/03-rendering.md` (GL/Vulkan, swizzle) ·
`docs/06-resources.md` (spec table, versioning) · `docs/07-build-system.md` (asset integration) ·
`docs/08-tooling.md` (formats, pipeline guarantees)

**API footgun fixes:** `engine/runtime/src/app.cpp` (`App::run` auto-flush wiring) ·
`engine/ecs/include/phx/ecs/world.h` (`defer()`/`flush_deferred()`) ·
`engine/core/include/phx/core/math.h` (Mat3, defined + tier bug fix) ·
`tests/unit/test_math.cpp` (new Vec2/AABB/Mat3 tests) ·
`tests/suites/smoke_app.cpp` (auto-flush end-to-end proof) ·
`docs/10-gameplay-systems.md` (scene transitions, documented)

**Physical hardware bring-up (real PSP, prior sessions):** `STATUS.md` entries #41–43 ·
`engine/platform/src/psp/psp_platform.cpp` · `engine/memory/src/memory_root.cpp`

**Collision metadata + controller/remap (prior update):**
`engine/physics/include/phx/physics/physics.h` · `engine/physics/src/physics.cpp` ·
`engine/resource/include/phx/resource/bundle.h` · `tools/phxpack/tiled.h` ·
`tools/phxtmap/editor.h` · `tools/phxtmap/main.cpp` ·
`engine/input/include/phx/input/input.h` · `engine/platform/src/sdl/sdl_platform.cpp` ·
`engine/runtime/include/phx/runtime/app.h` · `tests/unit/test_physics.cpp` ·
`tests/unit/test_input.cpp` · `tests/suites/tiled_test.cpp`

**Authoring-loop completion (this update):**
`tools/phxtmap/editor.h` (`flood_fill`/`fill_rect`) · `tools/phxtmap/main.cpp` (tool modes,
marquee, `--prefabs`) · `tools/phxentity/editor.h` (string cells, `name_column()`) ·
`tools/phxpack/builders.h` (`str8/16/32` bake + `.gen.h` char arrays) ·
`engine/input/include/phx/input/input.h` (`raw_pressed`/`raw_held`, masked remap shift) ·
`examples/platformer/src/systems.cpp` (`OptionsScene`, tile-hazard damage) ·
`examples/platformer/src/bake.h` (tileset collision classes, lava tile) ·
`examples/platformer/src/components.h` (SaveData v2 with `InputMap`) ·
`tests/suites/pipeline_test.cpp` (area tools, prefab seam) ·
`tests/suites/platformer_test.cpp` (runs 3–5: lava, remap, persistence) ·
`tests/unit/test_input.cpp` · `docs/08-tooling.md` · `docs/10-gameplay-systems.md` ·
`tools/phxtmap/instructions.md` · `tools/phxentity/instructions.md`

---

*Assessment basis: source, public headers, build files, tests, documentation and current local
verification. Emulator claims in the engineering journal are treated as evidence distinct from
physical-hardware validation. `make check`, `make determinism`, `make release` and `make
sanitize` were re-verified clean as of this update.*
