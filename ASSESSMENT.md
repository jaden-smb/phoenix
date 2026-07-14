# Phoenix project assessment

*Repository-wide review — updated 13 Jul 2026.*

Phoenix is a compact, from-scratch C++17 retro 2D engine whose real differentiator is one
gameplay codebase spanning a 256 KB GBA, PSP, Windows and Linux. It is already a convincing
vertical-slice engine; it is not yet a polished, externally consumable 1.0 product.

## Current verified state

`make check` passes (120,354 unit checks across 106 cases plus all Make integration gates);
`make determinism` passes with 11 suite outcomes and a byte-identical rendered frame across
float and Q16 tiers (that run also caught and fixed a real GBA-tier `Mat3` bug — see below); a
fresh Release CMake build passes 21/21 CTest tests; `make release` passes the whole check suite
built with `PHX_BUILD_RELEASE=1`; `make sanitize` (ASan+UBSan) is clean. The working tree began
clean at the original review and after every workstream below.

| Metric | Value |
|---|---|
| Advertised platforms | 4 (GBA, PSP, Windows, Linux) |
| Render paths | 4 (software, GL, PSP GU, GBA PPU) |
| Registered unit cases | 106 |
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
| Platform ports | Advanced | Linux/Windows/GBA/PSP paths exist and emulator/device seams were exercised | Physical GBA sign-off; PSP save/render smoke on real hardware; pin toolchains |
| Gameplay systems | Functional/minimal | Input, mixer, scenes, tile physics, animation, UI; ECS deferred despawns now auto-flush every fixed step | Remapping/controllers, richer collision, an actual transition-render implementation (honestly documented as unbuilt for now) |
| Resources | Strong | Packed typed assets, LZSS, zero-copy views; CRC32/bounds/tier validation + mount/unmount; **per-target texture encode implemented** (tier-0 4bpp paletted, tier-1 swizzled; format v2) | ADPCM/tracker audio codecs if ever needed |
| Asset tools | Usable, reproducible | Five CLIs, direct/2-stage bake, Tiled `.tmj`/PNG/WAV/JSON; **lock-file incremental rebakes, manifest sidecar, `--upgrade`, byte-reproducibility asserted in CI suites** | Editor usability (P2 below) |
| Editors | Prototype/MVP | Tile and table editors dogfood engine UI | Real tilesets, undo/redo, fill/rect/picker, prefab schemas, GUI tests |
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

**Still open — memory discipline is strong but overstated.** The hot path is bounded and
arena-oriented, but the platform descriptor's root arena is passed as null and backends
allocate framebuffers/files separately. Desktop "map" is a loaded heap buffer, not an OS
memory map.

**Still open — several public seams remain placeholders.** Font is an asset enum without a
typed resource path; `connected_pads` is unused; SDL has no controller path; UI has no pointer
widgets; physics remains intentionally platformer-minimal. Scene transitions are still recorded
but not rendered — that gap is now honestly documented rather than silently overstated, but
building the actual fade/slide render step is still open work.

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
| Open | P2 | Finish the 2D authoring loop | Editor usability, controller/remap support, collision metadata, better diagnostics | A third party can build a game without source-level workarounds |
| Open | P2 | Ship 0.1 properly | Version macros, changelog, install/export package, API reference, binary artifacts and tagged release | Consumable engine release |
| Later | Later | Expand scope carefully | Particles/palette FX first; then 2.5D/affine, jobs, scripting, new platforms | Growth without weakening the GBA-first design |

## Do not prioritize yet

Vulkan, scripting, jobs and many new platforms are attractive, but they would widen the
maintenance matrix before the current four-target release path is coherent. The best return is
hardening the existing small engine and making one third-party workflow excellent.

## Best next feature after hardening

Finish the 2D authoring loop: real tileset rendering, undo/redo and fill/rect/picker in
`phxtmap`; prefab schemas shared with `phxentity`; controller/remapping support; explicit
collision metadata. These directly improve the ability to make games without diluting the
architecture.

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

---

*Assessment basis: source, public headers, build files, tests, documentation and current local
verification. Emulator claims in the engineering journal are treated as evidence distinct from
physical-hardware validation. `make check`, `make determinism`, `make release` and `make
sanitize` were re-verified clean as of this update.*
