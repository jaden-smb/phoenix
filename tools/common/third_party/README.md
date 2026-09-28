# tools/common/third_party

Vendored single-header libraries for the host-only tools (never linked into engine/ or a game).

| file | version | licence | used by |
|---|---|---|---|
| `stb_truetype.h` | v1.26 | public domain (or MIT, at your option; see the file's end) | `ttf_text.cpp`: rasterizes the Studio's JetBrains Mono |

The typeface itself is in `../fonts/` with its licence (SIL OFL 1.1).
