# phxtmap — the GUI tilemap editor

## What it is for

A mouse-driven editor for **level layout**: tile layers and entity spawn points, saved as the
open **Tiled `.tmj`** author format (never engine blobs — `phxtile`/`phxpack` bake what it
saves, docs/08 §1). Use it to author or touch up maps without leaving the repo; anything it
writes also opens in the full Tiled editor and vice versa.

## How it works

It **dogfoods the engine**: the same App loop, SDL window, software renderer, and
immediate-mode UI the games use — no separate UI toolkit. The document model
(`editor.h: TmapDoc`) is separated from this GUI shell and unit-tested headlessly in the
pipeline suite (load → edit → save → re-import round-trip, including per-layer parallax
factors and spawn objects). Edits stream zero-copy into the software renderer, so painting is
live. Tiles render as **procedural colour swatches** (GIDs 1–16) — layout editing needs
positions, not final art; the baked game shows the real tileset.

## Build & run

Needs SDL2 and a display (not part of `make check`).

```bash
make tmap                                  # -> build/phxtmap

./build/phxtmap level.tmj                  # edit an existing map (saves in place)
./build/phxtmap --out new.tmj --size 30x20 # start a blank 30x20 map
./build/phxtmap --out copy.tmj level.tmj   # edit one file, save to another
```

`PHX_MAX_FRAMES=120 ./build/phxtmap …` runs a bounded smoke (boots, runs, exits) for scripts.

## Controls

| Input | Action |
|-------|--------|
| **LMB** (click/drag) | paint the selected tile — or pick a tile from the palette strip |
| **hold X + LMB** | erase tiles (entity mode: remove the spawn under the cursor) |
| **C** | cycle the selected tile GID (1–16) |
| **Tab** | cycle the edited layer (parallax layers keep their factors) |
| **E** | toggle TILE / ENTITY mode |
| **Q** | cycle the spawn type (player / coin / enemy / spike) — shown in the status line |
| **LMB** (entity mode) | place a spawn of the selected type at the cursor |
| **arrows / WASD** | scroll the camera |
| **Enter** | **save** to `--out` (a `*` in the status line = unsaved edits) |
| window close | quit |

The status line (bottom right) shows the mode, current layer/GID or spawn type, and the dirty
flag. Spawn markers draw as small boxes with the type's initial.

## Conventions it preserves

- The **last tile layer is the gameplay/solid layer** (the platformer's physics reads it);
  earlier layers are backdrops and may carry `parallaxx`/`parallaxy` factors — both survive
  load → save.
- Spawn objects keep their `type` (baked to the type hash the game switches on).

## Typical workflow

```bash
make tmap
./build/phxtmap --out mylevel.tmj --size 32x20     # paint ground on the last layer,
                                                   # E → place player/coins/enemies, Enter
./build/phxtile --out mylevel.phxtmap mylevel.tmj  # bake
./build/phxpack --out assets.phxp tiles.png mylevel.phxtmap
```
