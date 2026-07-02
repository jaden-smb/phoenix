# phxpack — the bundle assembler

## What it is for

`phxpack` produces the **`.phxp` bundle** — the single packed asset file a Phoenix game mounts
at runtime (zero-copy on PC/PSP, linked into the ROM on GBA). It is the final stage of the
two-stage pipeline (docs/08): the per-format converters (`phxsprite`/`phxtile`/`phxsnd`/
`phxbin`) bake author sources into intermediate `.phx*` files, and `phxpack` **merges** those
into one bundle. It can also **bake author sources directly** (it shares the same builder code),
which is convenient for small projects and tests.

## How it works

Every asset becomes a `[name-hash → typed blob]` entry in a sorted TOC. Names are FNV-1a hashes
of the asset name (games look assets up with `"hero"_hash`). With `--compress`, each blob is
LZSS-compressed independently and kept compressed **only where that shrinks it** — the runtime
reader decompresses once into its arena and serves uncompressed blobs zero-copy, so compression
never costs correctness or makes a bundle bigger. `--target` selects the capability tier and
drives per-target encoding (e.g. tier 0 resamples sounds to the GBA device rate — see phxsnd).

## Build & run

```bash
make phxpack            # builds build/phxpack (and runs its CLI smoke)

./build/phxpack --out FILE.phxp [--target 0|1|2] [--compress|-z] <inputs...>
```

| Flag | Meaning |
|------|---------|
| `--out FILE` | (required) the bundle to write |
| `--target N` | capability tier: 0 = GBA, 1 = PSP, 2 = PC (default 2) |
| `--compress`, `-z` | LZSS each blob, kept only where it shrinks |

## Accepted inputs (by extension)

| Kind | Extensions | Becomes |
|------|-----------|---------|
| sources (baked directly) | `.png` `.ppm` → Texture · `.tmj` (Tiled) → Tilemap + Spawns · `.tmcsv` → Tilemap · `.sprdef`/sprite `.json` → Texture + Sprite · `.wav` → Sound | engine assets |
| intermediates (merged) | `.phxspr` `.phxtmap` `.phxsnd` `.phxbin` `.phxp` | their contained assets, re-added by hash |

Asset names default to the input file's stem (e.g. `hero.sprdef` → `"hero"`).

## Examples

```bash
# canonical two-stage flow: merge converter outputs
./build/phxpack --out assets.phxp --target 2 hero.phxspr level.phxtmap tone.phxsnd items.phxbin

# direct bake, compressed, for a GBA ROM
./build/phxpack --out assets.phxp --target 0 -z tiles.png level.tmj jump.wav
```

Verified end to end by `make phxpack` (CLI smoke over every input type), `make pipeline`
(in-process), and `make tools` (real binaries, converters → merge).
