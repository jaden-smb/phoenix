# Phoenix Engine — Tooling & Asset Pipeline

> `tools/` — host-only C++17 (STL freely allowed here; these never ship to console).
> They turn author-friendly formats into the baked `.phxp` blobs the runtime mmaps.
> Philosophy: **do all the expensive, fallible work offline.**

## 1. Tool inventory

| Tool        | Input → Output                         | Role                                  |
|-------------|----------------------------------------|---------------------------------------|
| `phxsprite` | PNG (+ slice json) → `.phxspr`         | sprite/atlas + animation slicing      |
| `phxtile`   | Tiled `.tmj` → `.phxtmap`               | tilemap + layers + collision baking   |
| `phxsnd`    | WAV → `.phxsnd`                        | audio: mono 16-bit PCM (GBA resampled at bake, see §5) |
| `phxbin`    | JSON → `.phxbin`                       | data tables → optimized binary         |
| `phxpack`   | the above `+` → `assets.phxp`          | bundle assembler (sorted TOC, optional LZ77; see §2 for what's actually per-target) |
| `phxtmap`   | GUI tilemap editor → `.tmj`            | authoring (wraps Tiled-compatible fmt)|
| `phxentity` | GUI entity/prefab editor → `.json`     | component/prefab authoring             |

(No tool accepts XML/`.tmx` input today, despite some of the design language below — Tiled
maps are `.tmj`/JSON only. ADPCM/8-bit-at-bake for `phxsnd` is target design, not built either
— see §5's "As built" note.)

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
- Apply compression where it pays (`--compress auto` measures and keeps the smaller).
- Stamp the bundle `target` byte for the mount-time tier check (`docs/06` §2) —
  **implemented**, and today it's the *only* real per-target divergence for most asset
  types: textures/tilemaps/bin tables are byte-identical across `--target 0|1|2`.

**Designed, not implemented:** the original design called for `phxpack` to invoke a
per-target texture encoder (4bpp tiles for GBA, swizzle for PSP, RGBA8 PC), write an
`assets.phxp.lock` (source hashes + tool versions, for reproducible/incremental rebuilds),
and emit a `manifest.txt` (hash↔path) for dev-build debugging. None of the three exist in
`tools/phxpack/` — every invocation is a full, non-incremental re-bake, and the only
per-target *asset* encode implemented anywhere in the pipeline is `phxsnd`'s GBA sound
resample (§5). See §9 for the full list of pipeline guarantees this affects.

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

Output `.phxspr` blob: RGBA8 atlas pixels (same bytes regardless of `--target` — no
per-target texture encode is implemented anywhere in the pipeline yet, see §2) + a frame
table + an animation table consumed directly by `engine/anim`.

**Designed, not implemented:** bake-time GBA palette quantization (≤16 colors, failing
loudly offline if the art exceeds the budget). Today that quantization happens instead at
*upload* time in the GBA PPU render backend (`engine/render/src/gba/gba_ppu.cpp`), on every
run, not just at bake — so a too-large palette is currently a runtime/test-time failure,
not a bake-time one. Moving it to bake time (catching it offline, before it ever reaches a
console) is open work.

## 4. `phxtile` — tilemaps & collision

Reads the open **Tiled JSON export** (`.tmj` — so artists can use a mature editor) and
bakes:
- tile index layers (one per BG layer; GBA caps at 4),
- an optional per-layer Q16 parallax factor (imported from Tiled's `parallaxx`/`parallaxy`),
- object layer → entity spawn list (positions + prefab refs for `phxentity`).

There is **no separate collision layer or per-tile collision-shape/bitset field** — the
convention (enforced by convention, not the file format) is that **the last tile layer IS
the collision layer**: `engine/physics` treats any index `>= solid_from` in that last layer
as solid (`docs/10-gameplay-systems.md`; `examples/platformer/src/systems.cpp` and
`examples/emberwing/src/systems.cpp` both set `solid_from = 1`). A dedicated collision
bitset/shape-id field, and `.tmx`/XML input, were the original design (below) — neither
is built.

```
 Tiled .tmj                     .phxtmap
 ┌─────────────┐                ┌──────────────────────────────┐
 │ layer: bg    │               │ hdr: w,h,tilew,tileh,layers   │
 │ layer: main  │  phxtile ──►  │ layer[i]: u16 indices         │
 │ layer: coll  │ (as built:    │ (+ optional per-layer parallax│
 │ objects      │  last layer   │    factor table)              │
 │              │  = solid)     │ spawns: {prefab_hash, x,y}    │
 └─────────────┘                └──────────────────────────────┘
```

The design sketch below (a dedicated `collision: bitset/shapeids` blob field, independent
of layer order) is not implemented — it would let a level keep a visible-but-non-solid
top layer after the solid one, which the current "last layer wins" convention can't do:

```
 Tiled .tmj/.tmx (design)       .phxtmap (design)
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
> far is the **tier-0 (GBA) bake-time downsample to the 18157 Hz vblank-locked device rate** (Q16
> linear, deterministic) — the runtime downmixes 16→8-bit at the DMA buffer. ADPCM
> and OGG music refs are future encoders behind the same `--target` switch.

Output `.phxsnd`: header (rate, frames) + samples. Music can be streamed by the
runtime instead of fully residing (`docs/06` §5).

## 6. `phxbin` — JSON → binary tables

Game data (item stats, dialogue, tuning) authored as **JSON only** (an XML input path was
part of the original design — "the same backend via a small XML→intermediate step" — but
was never built; `tools/phxbin/main.cpp` takes a `.json` argument, full stop), baked to a
flat binary the engine reads as a `BlobView` with a generated accessor struct:

```
 items.json ──► phxbin ──► items.phxbin   (array<ItemRecord>, fixed stride)
                              + items.gen.h (POD struct matching the layout)
```

No runtime JSON parser ships. The generated header guarantees the struct and the blob
agree (versioned).

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

**True today:**
- **Determinism, informally:** the bake path is a pure function of its inputs (no clock
  reads, no nondeterministic ordering) — same sources in, same bundle bytes out — but
  nothing *asserts* this; there's no lock file, and the pipeline suites don't diff two
  independent bakes of the same fixtures byte-for-byte to prove it.
- **Offline validation, for what's implemented:** a broken prefab ref, a malformed
  `.tmj`/JSON, or a missing referenced file fails the *bake*, never the *game*. GBA
  palette overflow is the one design-time check that currently only fires at *upload*
  time (§3), not bake time, so it isn't in this category yet.
- **Round-trip tested:** the pipeline/resource suites (`make pipeline`, `make resource`,
  `make tools`) bake fixtures and assert the runtime `ResourceCache` reads back identical
  views for every type, on every target encoding.

**Designed, not implemented** (all of §2's "designed, not implemented" list applies here
too):
- **No incremental bake.** Every `phxpack`/converter invocation re-does the full
  decode+encode+write; there's no content-hash-based skip of unchanged assets. Fine at
  this project's asset volume; would need the lock-file work below to scale further.
- **No lock file, no reproducibility guarantee that's actually checked.** `.phxp.lock`
  (recording source hashes + tool versions so CI could flag a stale bundle) is unbuilt.
