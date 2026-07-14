# phxsprite — the sprite/animation converter

## What it is for

Bakes a **sprite sheet + its animation clips** into a `.phxspr` intermediate: one Texture asset
(the decoded sheet PNG) plus one Sprite asset (the frame grid + named clips). At runtime the
game gets a `SpriteView` from the bundle and builds an `anim::Animator` from it — nothing about
animation is hardcoded in game code. `phxpack` merges the `.phxspr` into the final bundle.

## How it works

Reads a small definition file, decodes the referenced sheet PNG with the pipeline's own
dependency-free decoder (8-bit gray/RGB/palette/RGBA, all five filters), and emits the sheet
texture **per-target encoded** (`--target 0` → 4bpp paletted tiles for the GBA PPU,
`--target 1` → GU-swizzled RGBA8, `--target 2` → plain RGBA8; docs/06 §4 — shared
`BundleWriter` path, same as phxpack) plus a clip table (`first`, `count`, `fps`, `loop` per clip; clip names are FNV-1a hashed
for the runtime's `animator.play("walk"_hash)`). The texture asset is named after the sheet
PNG's stem, so several sprites can share one sheet; the sprite asset is named by `--name` or
the def file's stem.

## Build & run

```bash
make check              # builds build/phxsprite along the way (or: make tools)

./build/phxsprite --out FILE.phxspr [--name N] [--target 0|1|2] <def.sprdef | def.json>
```

## Input formats

**`.sprdef`** (line-based; `#` starts a comment):

```
sheet hero.png 8 8          # sheet PNG (relative paths resolve next to the def), frame W, frame H
clip idle 0 1 1 1           # clip <name> <first-frame> <frame-count> <fps> <loop 0|1>
clip walk 0 4 10 1
```

**sprite `.json`** (Aseprite-style):

```json
{ "image": "hero.png", "tile": 8,
  "animations": { "walk": { "frames": [0,1,2,3], "fps": 10, "loop": true } } }
```

(`"tile"` sets square frames; `"frame_w"`/`"frame_h"` override. Frame lists must be a
contiguous run — the runtime clip format is `first + count`.)

Frames are numbered row-major across the sheet grid (`sheet_width / frame_w` columns).

## Example

```bash
./build/phxsprite --out hero.phxspr --name hero hero.sprdef
./build/phxpack   --out assets.phxp hero.phxspr
```

Try it on the fixture `make check` drops: `./build/phxsprite --out /tmp/h.phxspr build/p_hero.sprdef`.
Verified by `make sprite` (decode → bake → mount → Animator → frame on screen) and `make pipeline`.
