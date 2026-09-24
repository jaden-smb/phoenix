# Changelog

All notable changes to Phoenix are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[semantic versioning](https://semver.org) (pre-1.0: MINOR may break, PATCH never does — see
[RELEASING.md](RELEASING.md)).

## [Unreleased]

### Added
- **Editing assets after they're made, in Phoenix Studio.**
  - The Assets view has **edit source**: it opens the file an asset was baked from (a texture's
    PNG, a sprite's def, a map's `.tmj`, a table's `.json`). Double-clicking an asset does the
    same. In a project, **bake** rebakes the assets and reloads the bundle.
  - Plain PNGs such as tilesets open in **tile mode**. The tile grid is labelled with GIDs, and
    its size is detected from the map or sprite that uses the image. The strip steps and zooms
    through tiles, and the Image tab can add a row of tiles.
  - In the map editor, double-click a palette tile (or right-click > Edit tile) to paint it,
    zoomed on that tile. Open maps redraw when the tileset PNG is saved. A map drawn with swatch
    colours gets **create tileset…**, which writes a real tileset PNG to paint.
- **Game projects and a project boundary in Phoenix Studio.**
  - A project is a folder with a `phxproject.json` (name, code and asset folders, bundles, and the
    Run view's launches). The Studio opens one project at a time (`--project DIR`, a project
    picker, or File > New / Open / Close project).
  - The Studio can **write only inside the project folder**. It can **read the engine's public API
    headers** (`engine/*/include`, under ENGINE API in the Explorer and quick open, in a locked
    editor) and its docs. Everything else is never opened: the engine's sources, tools, tests,
    Makefile and other projects. This covers the Explorer, quick open, `--open`, drops,
    compiler-error links and sprite/tileset references.
  - Paths are canonicalised, so `../` and symlinks can't escape.
  - The module graph and the engine's gates and suites are hidden in a project. `--engine-dev`
    restores the whole-checkout Studio for engine work.
- **File > New project** creates a small, complete game from a template (`src/main.cpp`, a hero
  sprite, a tileset, a level). New generic Make rules build any project with no Makefile edit:
  `make game`, `make game-assets` (via `tools/common/bake_project.py`, `TIER=0|1|2`) and
  `make play PROJECT=path`. Games compile against the engine's **public headers only**.
- `phxproject.json` for the examples (`emberwing`, `platformer`, `miracle-player`, `tinyllm`), so
  they open as projects.
- **Phoenix Studio is now the editor for the engine.** A new **Editor** view (the default)
  has an Explorer over the repository, tabs of documents, quick open (Ctrl+P), New-asset
  templates, save / save-all with confirm-on-close, reload-on-external-change and session
  restore. Each document opens in its own editor:
  - **Code**: syntax highlighting for C/C++, JSON, Makefile, shell, Python, CMake, Markdown and
    `.sprdef`; auto-indent; word-granular undo; find/replace; go-to-line; comment, duplicate
    and move lines; UTF-8-safe editing. Compiler errors from the Run view become clickable links
    and gutter markers.
  - **Sprite / pixel**: the pencil, eraser, fill, line, rectangle, ellipse, picker and marquee
    tools; floating selections, mirror painting and palettes. GBA BGR555 snapping and the
    8×8-tile 15-colour check. For sprite defs: frame grid, onion skin, named clips and a live
    preview. It saves real PNGs and `.sprdef` / sprite `.json`.
  - **Tilemap**: see the rebuilt `phxtmap` below.
  - **Data table**: see the rebuilt `phxentity` below.

  The Studio window is now resizable, with an integer UI scale (Ctrl+= / Ctrl+-). It adds menus
  (File/Edit/View/Build/Help), toasts, modal dialogs, tooltips and a shortcut sheet (F1).
  `--open`, `--fresh`, and a much richer `--script` (key, type, drag, wheel, dclick, open)
  cover the new features.
- **`phxtmap` and `phxentity` rebuilt** as the Studio's map and table panels in one-document
  windows, so there is one implementation of each.
  - The **map editor** draws the real tileset art. It adds brush, eraser, fill, rectangle,
    picker and select tools with multi-tile stamps; collision flags with an overlay; spawns you
    can place, drag and inspect; layer add/delete/reorder/rename/parallax with a parallax
    preview; map resize with an anchor; and undo for all of it. The tileset `image` round-trips,
    so Tiled opens the map with its art. New flags: `--tile`, `--tileset`, `--scale`, `--shot`.
  - The **table editor** is a spreadsheet with in-cell typed editing (strings are finally
    editable) and schema editing (rename, retype, reorder fields). It adds a record inspector,
    duplicate-name and over-long-text checks, and undo.
- **`phx/platform/desktop.h`**: a desktop-only extension of the platform seam for tools. It
  covers key events with modifiers, typed text, right/middle buttons and the wheel, the
  clipboard, a resizable window with an integer UI scale, cursors, titles, dropped files, and a
  vetoable window close. It is implemented by the SDL backend and as a scripted queue in the
  null backend (`phx_null_desktop_push`). No console backend or game uses it, so ROMs and game
  behaviour are unchanged.
- **The tool widget kit `tools/common/twk.h`** (+ `twk_geom.h`, `twk_icons.h`): an immediate-mode
  GUI over `phx::UI` with clipping, planes, text fields, number fields, dropdowns, menus, modals
  and pixel-art icons. **`tools/common/png_write.h`**: a dependency-free PNG encoder (indexed or
  RGBA, deflate).
- **`make editors`** (on `check`): a new headless suite that covers the desktop seam queue, the
  widget kit, and every editor document model. Each saved form is re-read by the bake's own
  loaders (`load_sprdef`, `load_sprjson`, `tiled_load`, `build_bin`).
- **Phoenix Studio (`tools/phxstudio`, `make studio`) — a graphical hub over the whole engine**,
  built on the engine itself (App loop, SDL window, software golden renderer, `phx::ui`).
  *Overview*: the module dependency graph as built (layers from `depcheck.py`, edges from real
  `#include`s) with per-module details, and the GBA/PSP/PC capability tiers parsed from
  `caps.h`. *Assets*: every `.phxp` validated like `ResourceCache::mount()`, with live previews
  — textures re-encoded per render tier by the bake's own encoders (GBA palettes, 8×8
  colour-budget grid), animated sprite clips, tilemaps through the real parallax path with
  collision/spawn overlays, sounds on the real mixer, spawn tables, blob hex. *Run*: one-click
  games, editors, gates, every `make check` suite (pass/fail chips) and console builds, with
  live colour-classified output and a stop that kills the whole process tree. `--shot`,
  `--script` and `--run` make it scriptable. Its headless model is covered by `make pipeline`.
- **`tools/common/ascii_font.h`**: a full printable-ASCII 5×7 tool font (the shared
  `debug_font.h` is uppercase-only).
- **`phx_sdl_set_window_scale()`** (SDL platform backend): a desktop-only, pre-init hook to pick
  the window's integer scale (default unchanged at 3×), for tools with larger canvases (now
  also reachable as `phx_desktop_set_scale()`).

### Fixed
- **`BinDoc` (phxbin tables in the editors) truncated `f32` fields to integers on load.** A float
  column opened in `phxentity` (or the Studio) and saved lost its fractions. Floats are now kept,
  edited and saved exactly.
- **`examples/tinyllm` — a quantized transformer language model running on Game Boy Advance
  hardware.** A 260K-parameter llama2-architecture model (RMSNorm, RoPE, grouped-query attention,
  SwiGLU) generates English text on screen at **2.17 tokens/sec**, measured on mGBA. The int8
  weights live in cartridge ROM and are streamed in place — the console's 256 KB of EWRAM never
  holds a single weight. The inference core is float-free, heap-free and STL-free, so `make
  determinism` proves the PC (float) and GBA (fixed16) builds emit byte-identical tokens.
  New host exporter `examples/tinyllm/tools/export_model.py` (llama2.c `.bin` or PyTorch
  `state_dict` → a versioned `.phxllm` blob) which also carries a bit-exact Python mirror of the
  device arithmetic, used to generate the golden tokens the suite asserts against. Targets:
  `make tinyllm`, `tinyllm-sdl`, `gba-tinyllm-ppu`, `size-gate-tinyllm`, `tinyllm-test` (on
  `check` and `determinism`).
- **`phx_gba_vblank_clock_start()`** (GBA platform backend): installs the VBlank IRQ — and with
  it the true elapsed-frame counter the fixed-step accumulator uses — *without* starting audio.
  Previously only `phx_gba_audio_start()` did, so a compute-heavy silent ROM had its sim clock
  dilate (one step per loop iteration regardless of how many vblanks really passed).

## [0.1.0] - 2026-08-02

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
- GBA: eliminated the periodic scroll stutter on PPU builds. The four BG screenblock windows
  were each fully re-streamed (~700 cells + a 1024-entry VRAM rewrite) whenever their layer
  crossed an 8-px tile boundary — and with stacked parallax factors (0.25/0.5/1.0) the
  crossings coincide every 32 camera px, spiking one frame past the vblank budget into a
  visible dropped frame. `draw_tilemap` now streams only the entering rows/columns and the
  hardware push writes only those slots (~32–64 entries per crossed tile instead of 1024).
- GBA: a dropped frame no longer plays as unrecoverable slow motion. The virtual clock now
  advances one sim step per **elapsed hardware vblank** (counted by the VBlank ISR) instead of
  one per game frame, so an over-budget frame is repaid with a catch-up step — matching the
  PSP's real-time-clock behaviour. Without the ISR (no audio started) the old one-step-per-frame
  behaviour is unchanged.
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

[Unreleased]: https://github.com/jaden-smb/phoenix/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/jaden-smb/phoenix/releases/tag/v0.1.0
