# Phoenix Engine — Resources & Asset Format

> `engine/resource/` (runtime) + the pack format produced by `tools/` (offline).
> Principle: **assets are baked offline, loaded by mmap/zero-copy, never parsed at
> runtime.** PNG/JSON/XML/WAV exist only on the developer's machine.

## 1. The `.phxp` bundle format

A bundle is a flat, append-only container the engine memory-maps. Layout:

```
 offset 0
 ┌──────────────────────────────────────────────────────────────┐
 │ Header (32 B)                                                  │
 │  magic "PHXP" | version u16 | platform u8 | flags u8           │
 │  asset_count u32 | toc_offset u32 | total_size u32 | crc32 u32 │
 ├──────────────────────────────────────────────────────────────┤
 │ TOC[asset_count]   (sorted by name_hash for binary search)     │
 │  name_hash u32 | type u16 | flags u16 | offset u32 | size u32  │
 │  uncompressed_size u32                                         │
 ├──────────────────────────────────────────────────────────────┤
 │ Blob region  (each blob aligned to 16 B; type-specific layout) │
 │  [texture][texture][tilemap][sound][bin]...                    │
 └──────────────────────────────────────────────────────────────┘
```

- **Names are 32-bit FNV-1a hashes**, not strings — no string table at runtime, no
  `std::string`. A human-readable `manifest.txt` (hash↔path) ships only in dev builds.
- **TOC is sorted** → `O(log n)` lookup, or `O(1)` if the game caches the handle at
  load (it should).
- **Platform-specialized at pack time.** The same logical asset is encoded
  differently per target (see §3); the bundle's `platform` byte guards mismatches.

## 2. Runtime API (`phx/resource/cache.h`)

```cpp
namespace phx {

enum class AssetType : uint16_t { Texture, Tilemap, Sprite, Sound, Music, Font, Bin };

class ResourceCache {
public:
    static Result<ResourceCache*> create(ArenaAllocator&, size_t budget_bytes);

    Status   mount(const char* bundle_path);     // mmap (PC) or load-once (PSP/GBA)
    // O(1) typed views into the mmap'd blob — no copy, no parse:
    Result<TextureView> texture(NameHash);
    Result<TilemapView> tilemap(NameHash);
    Result<SoundView>   sound(NameHash);
    Result<BlobView>    blob(NameHash);

    // for assets that DO need decode/upload (e.g. GPU texture), cache the result:
    TextureId           gpu_texture(NameHash, Renderer&);  // LRU, budget-bounded
    void                evict_unused();
    const CacheStats&   stats() const;
};

// compile-time name hashing so call sites cost nothing:
constexpr NameHash operator""_hash(const char* s, size_t n);   // "player.png"_hash
} // namespace phx
```

Usage:

```cpp
TextureId player = ctx->res->gpu_texture("hero_atlas"_hash, *ctx->render);
auto map = ctx->res->tilemap("level1"_hash).unwrap();   // zero-copy view
```

## 3. Per-platform specialization (same logical asset, different bytes)

| Asset    | GBA blob                            | PSP blob                  | PC blob          |
|----------|-------------------------------------|---------------------------|------------------|
| Texture  | 4bpp paletted 8×8 tiles + palette    | swizzled CLUT/RGBA4444    | RGBA8 (or BCn)   |
| Tilemap  | tile indices + map base (VRAM-ready) | indexed grid + atlas ref  | indexed + atlas  |
| Sound SFX| 8-bit PCM @ low rate (DMA-ready)     | ADPCM (VAG-like)          | 16-bit PCM       |
| Music    | tracker module (.mod-style) or PCM   | ADPCM stream              | OGG/PCM stream   |
| Font     | 4bpp tile glyphs                     | atlas                     | atlas            |

`phxpack --target gba|psp|pc` chooses the encoder. The **runtime view structs are
identical**; only the decoder behind `gpu_texture`/audio-upload differs, selected by
the same capability tier. A game's code is byte-for-byte identical.

> **As built:** the table above is the *target* encoding matrix. Implemented today:
> textures ship RGBA8 (the GBA PPU backend quantizes to 4bpp tiles + palette at
> upload); tilemaps carry `uint16` index layers **plus an optional per-layer Q16
> parallax-factor table** (flag bit in `TilemapBlobHeader.flags`, 4-byte aligned,
> imported from Tiled's `parallaxx`/`parallaxy`); sounds are mono 16-bit PCM, with
> the **tier-0 (GBA) bake resampling to the 16384 Hz device rate** at encode time —
> ADPCM, tracker music, and the remaining per-target texture codecs are future work.

## 4. Caching & memory policy

```
 ResourceCache budget (from Config.cache_bytes)
 ┌───────────────────────────────────────────────────────────┐
 │ mmap'd bundle (PC: not counted — OS-backed; PSP/GBA: in-arena)│
 │ ───────────────────────────────────────────────────────────│
 │ decoded GPU resources (LRU)   │ free                         │
 │  tex#1  tex#2  tex#3  ...      │                              │
 └───────────────────────────────────────────────────────────┘
```

- **Zero-copy assets** (tilemaps, fonts, PCM on PC) cost nothing beyond the mmap.
- **Decoded/uploaded assets** (GPU textures, decompressed audio) live in an LRU bounded
  by `cache_bytes`. Eviction frees the least-recently-used until the new asset fits;
  if a single asset can't fit, that's a boot-time budgeting bug, surfaced loudly.
- On GBA there is no LRU churn — everything fits in VRAM/ROM by design; the "cache" is
  just the VRAM allocation map.

## 5. Streaming (PSP/PC)

Large music and big maps stream rather than fully resident:

```cpp
class AudioStream {           // double-buffered, filled on a low-priority tick
    SoundView src; uint32_t cursor; RingBuffer<int16_t> ring;
    void pump(StackAllocator& scratch);   // decode next chunk into ring
};
```

The mixer pulls from the ring; the resource system refills it ahead of the read
cursor. GBA does not stream (no FS); its music is a resident tracker module played by
the audio mixer's pattern player.

## 6. Compression

- **Per-blob, optional, type-aware.** TOC `flags` bit marks compression; `size` is
  compressed, `uncompressed_size` is the mmap-decode target.
- **Algorithm:** LZ77-family (we ship a tiny in-tree decompressor, ~1 KB, no external
  dep) — decompresses fast on ARM7TDMI. The GBA BIOS also has built-in LZ77/Huffman
  decoders (`SWI LZ77UnCompVram`) which the GBA backend uses for free.
- **What we compress:** tilemaps, large binary blobs, music. **What we don't:** GPU
  textures the driver wants raw, small assets where header overhead dominates.
- Compression is a *pack-time* decision per asset; the runtime just sees a flag.

## 7. Versioning

- Bundle `version u16` gates format compatibility; the loader refuses a newer major.
- Each `AssetType` has its own `struct` version embedded in its blob header, so a
  texture format change doesn't invalidate sounds.
- `phxpack --upgrade old.phxp` re-bakes from a recorded **source manifest** (the build
  records which source files + tool versions produced each blob), enabling
  reproducible rebuilds and incremental re-pack (only changed sources re-encode).
- A `.phxp.lock` records tool versions + source hashes → CI fails if a bundle is stale
  relative to its sources.

## 8. Why baked + mmap (justification)

| Approach          | Runtime cost            | RAM            | GBA viable | Determinism |
|-------------------|-------------------------|----------------|------------|-------------|
| Parse PNG/JSON at runtime | decode + allocate | high, transient | no (no libpng/heap) | no |
| **Baked + mmap (Phoenix)** | pointer cast      | zero extra (OS-backed) | yes (ROM ptr) | yes |

Baking moves all parsing/validation to the developer's machine where errors are
caught early and where we can afford slow, thorough optimization. The console does the
minimum: map and cast. This is the RenderWare/classic-SDK discipline, updated.

See `docs/08-tooling.md` for the converters that produce these blobs.
