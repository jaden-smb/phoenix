# tools/common — shared tool infrastructure

Not an asset tool: this folder holds pieces shared by the build system and the other tools.

## depcheck.py — the architectural dependency gate

Enforces the engine's acyclic, strictly-layered module graph (CLAUDE.md / docs/00): every
`#include` between engine modules must point at a **lower** layer's public `include/phx/<mod>/`
directory. A violation is a build break — this is what keeps `core` closed and the platform
seam the only place platform code lives.

```bash
make depcheck                         # or directly:
python3 tools/common/depcheck.py engine
# -> "depcheck: OK (28 edges, acyclic, layering respected)" or a named violation
```

Runs as part of `make check` and in CI.

## size_gate.py — the GBA budget gate (MVP gate, docs/09)

Classifies the GBA ELF's static sections into IWRAM (32 KB) / EWRAM (256 KB) by load address
and checks the ROM file size; fails the build when over budget.

```bash
make size-gate    # builds build/gba/phx-platformer-ppu.gba, then enforces the budgets
```

Needs devkitARM. Runs in the `gba-size` CI job.

## bin2s.py — portable asset embedding

A devkitPro-`bin2s`-compatible generator (same symbol names: `<name>`, `<name>_size`) that
turns any binary file into an assembly `.rodata` object. Used to embed the baked `.phxp`
bundle into the PSP EBOOT so the PSP CI job needs only pspsdk + a host compiler (the GBA build
uses devkitPro's own `bin2s`).

```bash
python3 tools/common/bin2s.py bundle.phxp > bundle.s   # then assemble with the target CC
```

## debug_font.h — the shared 5×7 tool font

A host-only header that renders a 5×7 bitmap font (digits, punctuation, A–Z) into a 128×32
RGBA atlas matching the engine's `BitmapFont` defaults (`first_char=32`, `cols=16`, 8×8
cells). Used by the example's asset bake ("font" texture) and by both GUI editors
(`phxtmap`/`phxentity`), so every tool draws the same text through the engine UI without
shipping a font asset. Call `phxtool::build_debug_font(buf)` with a
`kDebugFontW * kDebugFontH` `uint32_t` buffer.
