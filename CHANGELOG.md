# Changelog

All notable changes to Phoenix are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[semantic versioning](https://semver.org) (pre-1.0: MINOR may break, PATCH never does — see
[RELEASING.md](RELEASING.md)).

## [Unreleased]

### Added
- `make gba-miracle-ppu`: the "A Small Miracle" visualizer as a native-PPU GBA ROM (the shipping
  console build). The spectrogram renders as a **BG tilemap** (the PPU can't scale/tint OBJ, so
  ui.rect bars can't work there — the tilemap is the native answer), particles are 8×8 OBJ sparks,
  text is OBJ glyphs; the resident tier-0 song (18157 Hz, ~9.5 MB in cartridge ROM) streams
  through the mixer from the VBLANK audio IRQ. Verified on mGBA (boots, audio DMA streaming, the
  BG spectrogram reacts frame-to-frame). `make size-gate-miracle` passes (ROM 9.8/16 MB, IWRAM
  18.7/28 KB). CMake gains the `miracle` example + gba hook. Mounts the bundle with
  `verify_checksum=false` — CRC32-ing the ~10 MB ROM-resident song took ~27 s of black screen at
  boot; it's self-baked and immutable, and mount still does every structural check.
- `Renderer::refresh_tilemap()` (backend `invalidate_map`): mark a retained tilemap's cells as
  changed in place so the GBA PPU re-streams its cached BG window — enabling a per-frame-rebuilt
  map like the spectrogram. Non-pure default no-op; the software backend reads cells live so it
  needs nothing, and other backends are unaffected.
- `tools/common/size_gate.py --rom-budget-mb`: override the ROM cartridge budget for a ROM that
  legitimately links a large baked asset (the RAM budgets, which guard scarce on-chip memory,
  are not overridable).
- `examples/miracle-player/`: the "A Small Miracle" music visualizer app (engine-only). Streams
  the resident song PCM through the mixer (`AudioStream` + `play_music_stream`, pumped off the
  audio path each frame) and drives a 16-band bars spectrogram (two styles), a beat-spawned
  integer particle pool, bass/loudness-reactive background + beat-synced screen shake/zoom, a
  transport UI, and the intro/outro dedication cards entirely from the precomputed viz track — the
  record index is derived from the audio sample cursor, so audio and video stay locked. Controls:
  START play/pause, LEFT/RIGHT seek ±5 s (re-seeks the stream cursor and viz index together via a
  lock-free handshake), A style, B chrome, SELECT profiler; looping is configurable. `make
  miracle` opens the SDL window (software renderer + real audio); the headless
  `tests/suites/miracle_test.cpp` (in `check` + `determinism`) proves A/V lock across a seek, loop
  continuity, and non-silent streaming on both scalar tiers, and unit tests pin the particle-pool
  bound and the streaming ring's no-underrun invariant.
- `tools/phxviz`: a host-only visualization-track converter (WAV → per-video-frame `.phxviz`
  stream: log-spaced FFT bands, RMS, spectral-flux onset/beat, quantised to `uint8`). The GBA
  reads it zero-copy as a generic `Blob` asset so no FFT runs on the ARM7; A/V lock is a pure
  integer `samples_consumed / hop_samples` map (tier-identical). Format is the engine-free POD
  in `examples/miracle-player/src/viz.h`; `phxpack` merges `.phxviz` intermediates. First slice
  of the "A Small Miracle" music-visualizer ROM (`examples/miracle-player/`).
- `make docs`: Doxygen API reference generated from the public headers (`engine/*/include`
  only — the same boundary depcheck enforces) plus the `docs/` manual, output in
  `build/docs/html`. `tools/common/doxyfilter.py` promotes the house `//` header comments
  to doc comments so the existing prose is the reference; the version is injected from
  `phx/core/version.h` (first slice of the roadmap's v1.0 "written manual" item).

### Fixed
- GBA PPU: sprite flicker and a fixed horizontal cut through static OBJ content (e.g. the
  Emberwing title text sliced in half). The per-frame hardware push (OAM hide-all + rewrite,
  OBJ char VRAM, scroll registers, screenblocks) ran from `end()`, *before* `present()`'s
  vblank wait — i.e. mid-scanout at whatever raster line the game frame happened to finish
  on, so scanlines drawn inside the rewrite window showed no sprites (a fixed band on a
  static scene, a roaming band under frame-time jitter) and scrolling BGs could tear.
  `submit_hardware()` now waits for vblank before touching PPU memory; verified on mGBA via
  the GDB stub that the push starts at VCOUNT≈187 (inside vblank, after the audio ISR) on
  both the title and in-level frames, with pacing still 60 fps.
- GBA: periodic stutter (~once a minute) caused by the virtual clock leaking the loop's five
  1 µs profiler-read ticks per frame into the fixed-step accumulator — after ~3,334 frames the
  residue crossed a whole step and that frame ran two sim steps (a visible hitch, and a likely
  missed vblank on a 16.78 MHz ARM7). `pump_events()` now subtracts the read ticks accrued
  since the last pump from its step advance, so the accumulator sees exactly one step per
  frame; reads still tick so intra-frame phase deltas stay strictly ordered. Same fix in the
  null backend (same convention), where the leak would have broken any exact-step-count test
  running ≥3,334 frames; the smoke suite now soaks 4,000 frames to pin this.
- Docs/comments corrected to match the implementation: desktop `map()` is a load-once heap
  buffer (there is no OS mmap), PSP bundles are linked into the EBOOT (not read via `sceIo` at
  mount), platform backends allocate init-time state outside the root arena
  (`phx_platform_desc.root_arena` is reserved/unused today), and the PC bundle image counts
  against RAM — it is not OS-backed as `docs/06-resources.md` previously claimed.

## [0.1.0] - 2026-07-15

First tagged release: the complete engine slice proven on all four targets.

### Added
- **Engine core** — fixed-step App loop; one-arena memory model (arena/stack/pool/object
  allocators, zero hot-path heap); sparse-set ECS; scene, physics (tile collision incl.
  per-tile flags), animation, and UI systems; deterministic `phx::scalar` dual-tier math
  (`float` on PC/PSP, Q16.16 `fixed16` on GBA) held byte-identical by a determinism gate.
- **Rendering** — one 2D-intent API (sprites, tilemaps, per-layer parallax, palettes, zoom,
  shake) with four backends: software golden reference, OpenGL, PSP GU, GBA PPU (Mode-0 tiles +
  OAM, streamed backgrounds).
- **Audio** — block mixer + streaming, per-target sample-rate baking, SDL/PSP/GBA output paths.
- **Asset pipeline** — `.phxp` bundles, zero-copy/LZSS loading, per-target encoding; converters
  (`phxsprite`, `phxtile`, `phxsnd`, `phxbin`) + the `phxpack` assembler; save/load on every
  target including GBA SRAM and PSP savedata.
- **Editors** — `phxtmap` (tilemap, incl. collision-flag painting and prefab palettes) and
  `phxentity` (record tables), both built on the engine itself.
- **Games** — `examples/platformer` (the MVP gate) and **Emberwing — Cinder Hollow**, a
  complete level shipping as GBA ROM, PSP EBOOT, Windows exe, and Linux binary from one tree.
- **Targets** — Linux, Windows (MinGW-w64, static), GBA (devkitARM, ROM size gates), PSP
  (pspsdk); CI gates: full suite, cross-tier determinism, ASan+UBSan, release config, Wine-run
  Windows suite, console cross builds.
- **Release plumbing** — single-source version header (`phx/core/version.h`), `make dist*`
  packaging, CMake install/`find_package(phoenix)`/CPack SDK, tag-driven release workflow.

[Unreleased]: https://github.com/jaden-smb/phoenix/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/jaden-smb/phoenix/releases/tag/v0.1.0
