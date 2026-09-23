# phxstudio — Phoenix Studio, the engine made visible

## What it is for

One window over the whole repository, so you can **see** what the engine is and does instead of
reading it out of headers, bundle hex dumps and terminal logs:

| View | What you see | Where the data comes from |
|---|---|---|
| **Overview** | The module dependency graph (L0 core to L4 runtime). Click a module for its summary, public headers, per-target backends, size, and what it uses / what uses it. Below it, the GBA / PSP / PC capability tiers side by side (RAM, sprites, entities, voices, scalar type). | Layers from `tools/common/depcheck.py`, edges from the real `#include`s under `engine/`, the tiers from `engine/core/include/phx/core/caps.h`. Nothing is copied, so the view can't drift from the build. |
| **Assets** | Every `.phxp` bundle in the root and `build/`, validated the way `ResourceCache::mount()` does it (a bundle the runtime would refuse is red and says why). Live previews of each asset type (below). | `BundleDoc` in `model.h` reads the real bundle format. Asset names come back from their FNV-1a hashes by hashing every string literal in the example/tool sources plus any phxpack `--manifest`. |
| **Run** | One-click **games**, **editors**, the **gates** (`check`, `determinism`, `sanitize`, `release`, `depcheck`, the golden frame), **every suite** of `make check` as a pass/fail chip, and the **console builds** (GBA ROMs opened in mGBA, PSP EBOOTs, Windows). Output streams into a colour-coded console: PASS, FAIL, errors and warnings stand out, and compiler command lines are dimmed. | The suite chips are the Makefile's `check:` prerequisites. A launch whose toolchain is missing (devkitARM, pspsdk, MinGW) is greyed out and names what it needs. |

Asset previews:

- **Textures.** Shown as each render tier receives them: **PC** (RGBA8), **PSP** (GU-swizzled)
  or **GBA** (4bpp paletted tiles). The studio re-encodes the texture with the bake's own
  encoders (`tools/phxpack/tex_encode.h`) and the software golden renderer samples the result,
  so the GBA view shows the real BGR555 quantization, the palettes it produced and the byte
  savings. **tiles** outlines the 8×8 grid by colour count, with red marking tiles that need
  more than 15 colours (the GBA can't show those). Hover a texel for its RGBA and BGR555 value.
- **Sprites.** Every animation clip plays at its baked fps. The sheet below it marks the
  current frame.
- **Tilemaps.** Drawn by the real renderer through the real `set_tilemap_parallax` path. **fly**
  auto-scrolls the camera so the parallax layers visibly move. Toggles: **collision** (white =
  solid, yellow = one-way, red = hazard; with no flag table, the `solid_from` fallback),
  **spawns** (the map's object layer), per-layer visibility, and **wide**, which hides the lists.
  Arrow keys pan (hold X to pan faster); drag the orange bar above the map to scrub.
- **Sounds.** Peak waveform with a time ruler, peak and RMS, and **play** through the engine's
  `AudioMixer` on the SDL audio device.
- **Spawns** show a per-type histogram and the full table. **Blobs** show a hex dump.

## How it works

It **dogfoods the engine**, like `phxtmap` and `phxentity`: it is one `phx::Game` on the same
App loop, SDL window, software renderer and `phx::ui` primitives the games use, with no
separate UI toolkit. The feasibility study (`docs/gui-editor-feasibility.md`) calls this
Option A. The code splits three ways:

- `model.h` is the **headless document model**: engine map, caps parser, name recovery,
  bundle reader and validator, typed views, per-tier analysis, launch catalog, log
  classification, and layout math. It is unit-tested in the pipeline suite (`make pipeline`)
  against the real tree and a fixture bundle.
- `jobs.h` is the **process runner**. It runs one job at a time from a FIFO queue on a worker
  thread. Each job runs as `setsid sh -c '…'` from the repo root, so **stop** can signal the
  whole process tree (make, g++, the test binary, or a game window). Output is line-buffered
  with `stdbuf`.
- `main.cpp` is the **GUI shell**: a tiny pointer-aware immediate-mode layer (`Gui`: buttons,
  lists, scrollbars, clipped text) over `UI::rect`/`UI::image`, plus the three views.

A few engine facts shape the shell. They are written down because the next tool will hit them
too:

- **Sprites are camera-relative** in the renderer, including `phx::ui` draws. The tilemap
  preview moves the camera, so the shell adds the camera back to every chrome draw. That keeps
  the chrome fixed while the map scrolls under it.
- **Tilemaps always draw beneath every sprite**, and there is no scissor. The map shows
  through a "window": the preview panel is drawn as a frame, and opaque rects cover everything
  outside it.
- **Tilemap slots can't be freed**, so a tilemap preview is uploaded once per bundle and asset,
  then cached.
- Uploads point **zero-copy** into the cached bundle blobs. The **scan** button unloads those
  textures before it drops the cache.

No SDL header is included. The studio uses three desktop-only symbols that the SDL backend
exports, the same pattern as emberwing's audio glue: `phx_sdl_audio_start/stop`,
`phx_sdl_readback` (for `--shot`) and `phx_sdl_set_window_scale`. The last one lets the
640×360 canvas open as a 1280×720 window; the default 3× would not fit a 1080p screen.

## Build & run

Needs SDL2 and a display (not part of `make check`; its model is). Linux/macOS: the Run view
shells out through `sh`, and **stop** needs `setsid` (util-linux).

```bash
make studio                    # -> build/phxstudio
./build/phxstudio              # run from anywhere inside the repo (it finds the root)
```

Options:

```bash
./build/phxstudio --scale 3                     # bigger window (integer scale of 640x360; default 2)
./build/phxstudio --tab assets --bundle build/emberwing.phxp --asset sprite:hero
./build/phxstudio --tab run --run check         # open on Run and start `make check` right away
./build/phxstudio --run physics --run anim      # queue several launches (make target or label)
./build/phxstudio --root ~/src/phoenix          # point it at a checkout explicitly
./build/phxstudio --shot out.ppm                # render 45 frames, save the window as PPM, quit
./build/phxstudio --script "click 262 9; wait 20; shot a.ppm; quit"   # scripted pointer
```

`--script` takes `click X Y`, `move X Y`, `wait N` (frames), `shot FILE.ppm` and `quit`. Put
the commands inline (separated by `;`) or in a file. Coordinates are on the 640×360 canvas.
With `--shot`, this gives a repeatable visual check of any click flow, and it is how the views
were verified under ASan:

```bash
make studio BUILD=build/asan EXTRA_CXXFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -O1"
ASAN_OPTIONS=detect_leaks=0 ./build/asan/phxstudio --tab assets --script "…; quit"
```

## Controls

| Input | Action |
|---|---|
| mouse | everything is clickable; drag scrollbars; hover anything for help in the status bar |
| Tab | next view |
| Up / Down | previous / next asset (Assets) · scroll the console (Run) |
| Q / E | previous / next asset (also while a tilemap owns the arrows) |
| arrows (tilemap) | pan the map (hold X for speed) |
| Esc / close window | quit (a running job is stopped with its whole process tree) |

## Notes & limits

- Games and editors opened from **Run** get their own windows. Their job ends when you close
  them, and only one job runs at a time; others queue.
- The root-level bundles that predate format v2 (e.g. an old `emberwing.phxp`) show as
  **refused** (red). That is correct: the runtime refuses them too. Running the game rebakes
  them.
- Texture tier previews re-encode in memory for display. The studio never writes a bundle; the
  bake stays the single writer (docs/08 §1).
