# Phoenix Engine — Tooling & Asset Pipeline

> `tools/` — host-only C++17 (STL freely allowed here; these never ship to console).
> They turn author-friendly formats into the baked `.phxp` blobs the runtime mmaps.
> Philosophy: **do all the expensive, fallible work offline.**

## 1. Tool inventory

| Tool        | Input → Output                         | Role                                  |
|-------------|----------------------------------------|---------------------------------------|
| `phxsprite` | PNG (+ slice json) → `.phxspr`         | sprite/atlas + animation slicing      |
| `phxtile`   | Tiled `.tmj`/`.tmx` → `.phxtmap`       | tilemap + layers + collision baking   |
| `phxsnd`    | WAV → `.phxsnd`                        | audio: PCM/ADPCM/8-bit per target     |
| `phxbin`    | JSON/XML → `.phxbin`                   | data tables → optimized binary         |
| `phxpack`   | the above `+` → `assets.phxp`          | bundle assembler (per-target encode)  |
| `phxtmap`   | GUI tilemap editor → `.tmj`            | authoring (wraps Tiled-compatible fmt)|
| `phxentity` | GUI entity/prefab editor → `.json`     | component/prefab authoring             |

`phxsprite/phxtile/phxsnd/phxbin` are the **converters**; `phxpack` is the
**assembler**; `phxtmap/phxentity` are the **editors**. Editors output author formats
that the converters then bake — editors never write engine blobs directly (keeps the
bake path single and testable).

## 2. `phxpack` — the bundle assembler

```
 phxpack --target {gba|psp|pc} --in assets/ --out assets.phxp [--compress auto]

 assets/                         build graph                    assets.phxp
 ├── hero.png ──► phxsprite ──► hero.phxspr ─┐
 ├── tiles.png ─► phxsprite ──► tiles.phxspr ┤
 ├── level1.tmj ► phxtile  ──► level1.phxtmap┼─► phxpack ─► [hdr][TOC][blobs]
 ├── music.wav ─► phxsnd   ──► music.phxsnd  ┤    (per-target encode + optional LZ77)
 └── items.json ► phxbin   ──► items.phxbin ─┘
```

Responsibilities:
- Resolve names → FNV-1a hashes, build the **sorted TOC** (§`docs/06`).
- Invoke the right per-target encoder (4bpp tiles for GBA, swizzle for PSP, RGBA8 PC).
- Apply compression where it pays (`--compress auto` measures and keeps the smaller).
- Write `assets.phxp.lock` (source hashes + tool versions) for reproducible/incremental
  rebuilds.
- Emit `manifest.txt` (hash↔path) for dev-build debugging.

## 3. `phxsprite` — sprites, atlases, animation

Input: a PNG plus an optional sidecar describing slices/animations:

```json
{
  "image": "hero.png",
  "tile":  16,
  "palette": "auto",                 // build a shared 16-color palette (GBA)
  "animations": {
    "idle": { "frames": [0,1],       "fps": 4,  "loop": true },
    "run":  { "frames": [2,3,4,5],   "fps": 12, "loop": true },
    "jump": { "frames": [6],         "fps": 1,  "loop": false }
  }
}
```

Output `.phxspr` blob: atlas pixels (target-encoded) + a frame table + an animation
table consumed directly by `engine/anim`. For GBA it also **quantizes to ≤16 colors**
and emits the palette, failing loudly if the art exceeds the palette budget (caught
offline, not on hardware).

## 4. `phxtile` — tilemaps & collision

Reads the open **Tiled** JSON/XML (so artists can use a mature editor) and bakes:
- tile index layers (one per BG layer; GBA caps at 4),
- a **collision layer** → a packed bitset / per-tile collision-shape id,
- object layer → entity spawn list (positions + prefab refs for `phxentity`).

```
 Tiled .tmj                     .phxtmap
 ┌─────────────┐                ┌────────────────────────────┐
 │ layer: bg    │               │ hdr: w,h,tilew,tileh,layers │
 │ layer: main  │  phxtile ──►  │ layer[i]: u16 indices       │
 │ layer: coll  │               │ collision: bitset/shapeids  │
 │ objects      │               │ spawns: {prefab_hash, x,y}  │
 └─────────────┘                └────────────────────────────┘
```

## 5. `phxsnd` — audio bake

| Target | Encoding (target design)          | Why                                  |
|--------|-----------------------------------|--------------------------------------|
| GBA    | 8-bit signed PCM, downsampled     | DirectSound DMA wants raw 8-bit; RAM |
| PSP    | ADPCM (4:1)                       | GE/audio-friendly, fits 32 MB        |
| PC     | 16-bit PCM (SFX), OGG ref (music) | quality, ample RAM                   |

> **As built:** all targets store mono 16-bit PCM; the per-target step implemented so
> far is the **tier-0 (GBA) bake-time downsample to the 16384 Hz device rate** (Q16
> linear, deterministic) — the runtime downmixes 16→8-bit at the DMA buffer. ADPCM
> and OGG music refs are future encoders behind the same `--target` switch.

Output `.phxsnd`: header (rate, frames) + samples. Music can be streamed by the
runtime instead of fully residing (`docs/06` §5).

## 6. `phxbin` — JSON/XML → binary tables

Game data (item stats, dialogue, tuning) authored as JSON/XML, baked to a flat binary
the engine reads as a `BlobView` with a generated accessor struct:

```
 items.json ──► phxbin ──► items.phxbin   (array<ItemRecord>, fixed stride)
                              + items.gen.h (POD struct matching the layout)
```

No runtime JSON parser ships. The generated header guarantees the struct and the blob
agree (versioned). XML path uses the same backend via a small XML→intermediate step.

## 7. `phxtmap` — Tilemap Editor (GUI)

Lightweight desktop tool built **on the engine itself** — the same App loop, SDL
window, software renderer, and immediate-mode UI the games use (dogfooding; no
external UI toolkit, not even ImGui, ended up necessary). The document model
(`tools/phxtmap/editor.h`) is separated from the GUI shell and unit-tested
headlessly; see `tools/phxtmap/instructions.md` for usage and controls.

```
 ┌──────────────────────────────────────────────────────────┐
 │ [Layers]   ┌───────────────────────────────┐  [Tileset]  │
 │ ▣ bg       │                               │  ┌─┬─┬─┬─┐  │
 │ ▣ main  ◄  │      tile canvas (paint)      │  ├─┼─┼─┼─┤  │
 │ ▢ coll     │                               │  └─┴─┴─┴─┘  │
 │ ▢ objects  └───────────────────────────────┘  brush: [▣] │
 │ [Tools] paint · fill · rect · pick · collision-paint     │
 └──────────────────────────────────────────────────────────┘
```

Built today: multi-layer tile paint/erase (drag paints; parallax layer factors
round-trip), a clickable tile palette, an **entity placement** mode dropping typed
spawn objects, camera scroll, and save-with-dirty-flag. Planned on top: fill/rect/
picker tools, collision-shape paint, and prefab refs into `phxentity`.
Saves Tiled-compatible `.tmj` → `phxtile` bakes it. Compatibility with Tiled means
users aren't locked into our editor.

## 8. `phxentity` — Entity / Prefab Editor (GUI)

```
 ┌─────────────────────┬────────────────────────────────────┐
 │ Prefabs             │ Components of "Goomba"              │
 │  • Player           │  ▸ Position   { x:0 y:0 }           │
 │  • Goomba       ◄    │  ▸ Velocity   { x:-1 y:0 }          │
 │  • Coin             │  ▸ SpriteRef  { atlas: enemies …}    │
 │  • Spring           │  ▸ AABBColl   { half: {8,8} mask:E } │
 │  [+ new prefab]     │  ▸ AI         { kind: patrol }       │
 │                     │  [+ add component ▾]                 │
 └─────────────────────┴────────────────────────────────────┘
```

- **Built today:** a keyboard-driven grid editor over the phxbin author JSON (typed
  record tables): cell cursor, ±1/±10 stepping **clamped to each field's declared
  type**, record clone/delete, save — on the same engine shell as `phxtmap`, with the
  document model (`tools/phxentity/editor.h`) unit-tested headlessly and its output
  proven to re-bake through the real `phxbin` builder. See
  `tools/phxentity/instructions.md`.
- **Planned on top:** component schemas **introspected** from a reflection table
  (`PHX_REFLECT(Component, fields...)`) so prefabs become named component lists with
  defaults, and placing one in `phxtmap` writes `{prefab_hash, x, y, overrides}`.
- Outputs `.json` → `phxbin` bakes it into tables → the game reads them zero-copy.

## 9. Pipeline guarantees

- **Determinism:** same sources + same tool versions → byte-identical bundle (lock
  file asserts it). Enables reproducible builds and meaningful binary diffs.
- **Offline validation:** palette overflow, oversized atlas, missing tile, broken
  prefab ref → all fail the *bake*, never the *game*. The console build is incapable of
  malformed assets.
- **Incremental:** content hashing → only changed assets re-bake; CI re-pack is fast.
- **Round-trip tested:** the pipeline/resource suites (`make pipeline`, `make resource`,
  `make tools`) bake fixtures and assert the runtime
  `ResourceCache` reads back identical views for every type, on every target encoding.
