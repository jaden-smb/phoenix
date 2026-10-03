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
clip land 4 1 12 0
trans idle walk move        # trans <from-clip|*> <to-clip> <trigger>: the animation state machine
trans walk idle stop
trans * land land
trans land idle done        # "done" fires by itself when a non-looping clip ends
```

**sprite `.json`** (Aseprite-style):

```json
{ "image": "hero.png", "tile": 8,
  "animations": { "walk": { "frames": [0,1,2,3], "fps": 10, "loop": true } },
  "transitions": [ { "from": "idle", "to": "walk", "on": "move" } ] }
```

(`"tile"` sets square frames; `"frame_w"`/`"frame_h"` override. Frame lists must be a
contiguous run — the runtime clip format is `first + count`.)

Frames are numbered row-major across the sheet grid (`sheet_width / frame_w` columns).

**Fonts.** `phxsprite` also bakes a font: the sheet Texture plus a Font asset (the glyph table)
named after the def. `load_font(r, *res, "font"_hash, font)` (`phx/runtime/font.h`) makes it a
`phx::UI` `BitmapFont`. Two inputs:

```json
{ "font": 1, "image": "font.png", "cell_w": 8, "cell_h": 8, "first": 32, "count": 0,
  "proportional": true, "spacing": 1, "space": 3, "advance": 0, "line_h": 9 }
```

A **`.font`** is a grid sheet: cell 0 is character `first`, row by row, and `count` 0 means
every cell.
- Proportional: each glyph is trimmed to its opaque columns and advances by its width plus
  `spacing`. An empty cell (space) advances by `space`, which defaults to half a cell.
- Fixed: whole cells with one `advance`, which defaults to the cell width.
- `line_h` 0 means the cell height + 1.

A **`.fnt`** is BMFont's **text** export (from BMFont, Hiero, Littera...). Each char's
`x y width height xoffset yoffset xadvance` from page 0 is baked; ids 32..255 are kept.

```bash
./build/phxsprite --out font.phxspr --name font font.font
```

**Transitions** make the clips a state machine: in clip `from` (`*` = any clip), the trigger
switches to clip `to`. Clip names are resolved to indices at bake time, so an edge naming a clip
the sprite lacks is a bake error. They bake into an optional trailer after the clips
(`SpriteTransDef`, `kSpriteTransMagic` in `phx/resource/bundle.h`; `SpriteView::trans`), which
`Level` hands to each entity's `Animator` (`Animator::edges`). `PlatformerController` sends
`jump`/`fall`/`land`/`move`/`stop`/`hurt`, the anim system sends `done`, and a game sends its own
with `phx::anim_trigger(world, e, "attack"_hash)` (see `phx/runtime/behaviours.h`). An edge from
the current clip beats a `*` edge; a `*` edge into the clip already playing does nothing, so a
trigger can be sent every step. A sprite with no transitions is played by clip name.

## Example

```bash
./build/phxsprite --out hero.phxspr --name hero hero.sprdef
./build/phxpack   --out assets.phxp hero.phxspr
```

Try it on the fixture `make check` drops: `./build/phxsprite --out /tmp/h.phxspr build/p_hero.sprdef`.
Verified by `make sprite` (decode → bake → mount → Animator → frame on screen) and `make pipeline`.
