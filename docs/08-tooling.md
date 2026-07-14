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
- Stamp the bundle `target` byte for the mount-time tier check (`docs/06` §2), and
  refuse to merge an intermediate baked for a *different* tier (blobs are per-target
  encoded, so a tier mix-up fails at pack time, not on a console).
- **Per-target texture encode** (`tools/phxpack/tex_encode.h`, docs/06 §4): `--target 0`
  bakes 4bpp paletted tiles + palettes (the GBA PPU's native layout), `--target 1` bakes
  GU-swizzled RGBA8, `--target 2` keeps RGBA8. The sound counterpart is the tier-0
  18157 Hz resample (§5). Both run inside `BundleWriter`, so converters get them too.
- **Lock file** (`<out>.lock`, `tools/phxpack/lock.h`): tool + format versions, per-input
  content hashes, per-input asset lists, output CRC32 — drives the incremental rebake
  (unchanged inputs are reused from the previous bundle), the "up to date" skip, CI
  stale-bundle detection, and `--upgrade` (re-bake from the recorded source list).
- **Manifest** (`--manifest` → `<out>.manifest.txt`): human-readable
  hash ↔ name ↔ source-path table for dev builds (hashes are one-way; this is the map).

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

Output `.phxspr` blob: the atlas texture **per-target encoded** (shared `BundleWriter`
path, so `--target 0` emits 4bpp paletted tiles, `--target 1` swizzled RGBA8 — see §2 and
docs/06 §4) + a frame table + an animation table consumed directly by `engine/anim`.

**Bake-time GBA palette quantization is implemented** (`tools/phxpack/tex_encode.h`): the
tier-0 encoder runs the same quantizer as the GBA PPU upload path (≤15 opaque colours per
16-colour palette, whole-texture palette when possible for OBJ use, per-tile palettes
otherwise) and its output composes the exact same frame (asserted by `make ppu`). Art the
tier can't express (a non-8px-aligned atlas, >15 colours in one 8×8 tile) is reported at
bake time and kept as RGBA8 — the PPU backend then applies its upload-time quantizer or
rejects it exactly as before, so nothing that would fail on hardware slips through
silently.

## 4. `phxtile` — tilemaps & collision

Reads the open **Tiled JSON export** (`.tmj` — so artists can use a mature editor) and
bakes:
- tile index layers (one per BG layer; GBA caps at 4),
- an optional per-layer Q16 parallax factor (imported from Tiled's `parallaxx`/`parallaxy`),
- object layer → entity spawn list (positions + prefab refs for `phxentity`).

**Collision comes from the map two ways.** The zero-setup convention is that **the last
tile layer IS the collision layer**: `engine/physics` treats any index `>= solid_from` in
that layer as solid (`docs/10-gameplay-systems.md`; both example games set
`solid_from = 1`). On top of that, **per-tile collision metadata** is now baked when the
tileset carries it: Tiled tileset per-tile boolean `properties` (`solid`, `oneway`,
`hazard`) or per-tile `class` strings of the same names become an optional per-tile flags
table in the Tilemap blob (`kTilemapHasTileFlags`, `phx/resource/bundle.h`), which
`TilemapView.tile_flags` serves straight into the physics `TileGrid`. That lets decorative
non-solid tiles, one-way platforms, and hazards share the gameplay layer — the thing the
bare "last layer wins" convention couldn't express. Maps without metadata behave exactly
as before. (`.tmx`/XML input remains unbuilt — export `.tmj` from Tiled.)

```
 Tiled .tmj                        .phxtmap
 ┌────────────────┐                ┌──────────────────────────────┐
 │ tileset        │                │ hdr: w,h,tilew,tileh,layers   │
 │  ├ tile props  │               │ layer[i]: u16 indices         │
 │ layer: bg      │  phxtile ──►  │ (+ optional per-layer parallax│
 │ layer: main    │               │    factor table)              │
 │ objects        │               │ (+ optional per-tile collision│
 │                │               │    flags: solid/oneway/hazard)│
 └────────────────┘                │ spawns: {type_hash, x,y,w,h}  │
                                   └──────────────────────────────┘
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

Built today: multi-layer tile paint/erase (drag paints; parallax layer factors AND layer
names round-trip), a clickable tile palette, **bounded undo/redo** (one step per gesture —
a whole paint stroke undoes at once), **collision-flag authoring** (cycle a GID through
solid/oneway/hazard; flags show as coloured palette underlines and save as Tiled tileset
per-tile properties), **add-layer**, an **entity placement** mode dropping typed spawn
objects (the placeable types come from `--types` plus whatever the loaded map uses — not
hardcoded), camera scroll, save-with-dirty-flag, and positioned load-error messages
(`line L, col C`). Planned on top: fill/rect/picker tools and prefab refs into `phxentity`.
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
  type**, record clone/delete, save — plus **schema authoring**: `--new NAME --fields
  a:type,b:type` starts a fresh table with no hand-written JSON, and the document model
  supports add/remove-field (every record keeps its shape). Malformed input is refused
  with a positioned `line L, col C` parse error. Same engine shell as `phxtmap`, with the
  document model (`tools/phxentity/editor.h`) unit-tested headlessly and its output
  proven to re-bake through the real `phxbin` builder. See
  `tools/phxentity/instructions.md`.
- **Planned on top:** component schemas **introspected** from a reflection table
  (`PHX_REFLECT(Component, fields...)`) so prefabs become named component lists with
  defaults, and placing one in `phxtmap` writes `{prefab_hash, x, y, overrides}`.
- Outputs `.json` → `phxbin` bakes it into tables → the game reads them zero-copy.

## 9. Pipeline guarantees

**True today, and asserted:**
- **Determinism, checked:** the bake path is a pure function of its inputs (no clock
  reads, no nondeterministic ordering) — same sources in, same bundle bytes out. The
  pipeline suite diffs two independent bakes of the same fixture byte-for-byte, and
  `make phxpack` proves an incremental rebake equals a `--full` rebake byte-for-byte.
- **Offline validation:** a broken prefab ref, a malformed `.tmj`/JSON, a missing
  referenced file, or GBA palette overflow in a single 8×8 tile (§3 — now caught by the
  tier-0 bake encoder) fails or warns at the *bake*, never surprises the *game*.
- **Round-trip tested:** the pipeline/resource suites (`make pipeline`, `make resource`,
  `make tools`) bake fixtures and assert the runtime `ResourceCache` reads back identical
  views for every type, on every target encoding (including PAL4/swizzled textures).
- **Incremental, via the lock file** (§2, `docs/06` §8): unchanged inputs are reused from
  the previous bundle; an unchanged input list skips the bake ("up to date"); the
  `<out>.lock`'s recorded output CRC32 lets CI flag a stale/hand-edited bundle; and
  `--upgrade` re-bakes a bundle from its own recorded source list. `--full` opts out.
