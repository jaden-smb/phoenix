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

## ttf_text.h / text_raster.h — the Studio's TrueType text

`phxstudio` draws its text in **JetBrains Mono** (SIL OFL 1.1), anti-aliased at *window*
resolution rather than as pixels of the 640×360 canvas. `text_raster.h` is the small interface
(`twk::TextRaster::glyph(ch, px_scale)` → an 8-bit coverage bitmap); `ttf_text.h/.cpp` implements it
with the vendored `third_party/stb_truetype.h` (public domain) over `jetbrains_mono_data.h`, the
face embedded as a C array. A tool opts in with `Gui::set_text_raster(&ttf)` and links
`ttf_text.cpp`; `phxtmap`, `phxentity` and the headless tests do not, and keep the bitmap font.

- **Same layout.** The em is 10 canvas pixels × the UI scale, and JetBrains Mono's advance is 0.6 em,
  so a character advances exactly `kAdv` (6) canvas pixels and a line is `kLineH` (10): the font
  swap moves nothing.
- **How it draws.** `Gui::glyph` queues the glyph; `Gui::end()` blends it into the platform's
  native-resolution overlay (`phx_desktop_overlay_begin`, `phx/platform/desktop.h`), minus every
  rect/image recorded on a higher plane, or a higher sub-layer of the same plane, and at half
  strength under a stipple (a modal's backdrop). A canvas size that disagrees with the overlay, or a
  platform without one, draws that frame with the bitmap font instead.
- **The face** (`fonts/JetBrainsMono-ASCII.ttf`) is JetBrains Mono Regular subset to printable ASCII
  with fontTools; `python tools/common/gen_font_data.py` regenerates `jetbrains_mono_data.h` from it
  (the subset command is in that script). `fonts/JetBrainsMono-OFL.txt` is its licence and must ship
  with it. `third_party/` holds vendored single-header code (`stb_truetype.h` v1.26, public domain).
- Tests: the editors suite (`ttf_text_*`, `twk_native_text_*`) reads the null backend's overlay back.

## ascii_font.h — the full-ASCII 5×7 tool font

The same idea for **all printable ASCII** (32..126, lowercase and punctuation included, 127 = a
hollow "unknown" box): a 128×48 RGBA atlas of 8×8 cells, 16 columns, `first_char=32`. The
glyphs sit at a 1px pad, so an advance of 6 gives a dense text pitch. Used by `phxtmap` / `phxentity`, by
`phxstudio` where no native-resolution overlay exists (see above), and for the new-project template's
font sheet. Call `phxtool::build_ascii_font(buf)` with a
`kAsciiFontW * kAsciiFontH` `uint32_t` buffer. `debug_font.h` stays as is, since existing bakes
depend on its exact atlas.
