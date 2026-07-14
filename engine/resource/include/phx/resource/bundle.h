// phx/resource/bundle.h — the on-disk `.phxp` bundle format. Shared by the offline writer
// (tools/phxpack) and the runtime reader (ResourceCache). Zero dependencies beyond core
// types so the host tool can include it directly. All fields little-endian (every target
// is LE; the loader asserts the magic to catch mismatches). See docs/06-resources.md.
#ifndef PHX_RESOURCE_BUNDLE_H
#define PHX_RESOURCE_BUNDLE_H

#include "phx/core/types.h"

namespace phx {

// "PHXP" in little-endian byte order ('P'=0x50, 'H'=0x48, 'X'=0x58, 'P'=0x50).
constexpr uint32_t kBundleMagic   = 0x50584850u;
// v2: per-target texture encodes (tier 0 bakes PAL4_TILES, tier 1 bakes RGBA8_SWZ — see the
// texture section below). v1 bundles (all textures RGBA8) still mount; a v1 runtime refuses
// v2 bundles as "newer major", which is the correct failure for blobs it cannot decode.
constexpr uint16_t kBundleVersion = 2;

enum class AssetType : uint16_t {
    Texture = 1,
    Tilemap = 2,
    Sound   = 3,
    Font    = 4,
    Blob    = 5,
    Sprite  = 6,    // sprite sheet metadata: frame grid + named animation clips (-> anim)
    Spawns  = 7,    // object-layer spawn points (type hash + rect), e.g. from a Tiled map
};

enum BundleFlags : uint8_t {
    kBundleNone = 0,
};

// Per-asset flags (TocEntry.flags). See phx/resource/lz.h for the codec.
enum TocFlags : uint16_t {
    kTocNone = 0,
    kTocLZ   = 1 << 0,     // blob is LZSS-compressed: `size` is stored, `usize` decompressed
};

// Fixed-size file header (see static_asserts below for the exact byte layout).
struct BundleHeader {
    uint32_t magic;        // kBundleMagic
    uint16_t version;      // kBundleVersion
    uint8_t  target;       // render tier the blobs were baked for (0=PPU,1=GU,2=GL)
    uint8_t  flags;
    uint32_t asset_count;
    uint32_t toc_offset;   // byte offset of TocEntry[asset_count]
    uint32_t total_size;   // whole bundle size in bytes
    uint32_t blob_crc32;   // CRC32 (phx/core/crc32.h) of [toc_offset, total_size) — TOC + blobs.
                           // 0 means "unchecked" (an explicit opt-out some tooling may still
                           // produce); the writer (tools/phxpack) always fills in a real value.
};

// TOC entries are sorted by name_hash for O(log n) lookup.
struct TocEntry {
    NameHash name_hash;
    uint16_t type;         // AssetType
    uint16_t flags;        // per-asset (compression…)
    uint32_t offset;       // from bundle start, to the blob (incl. its blob header)
    uint32_t size;         // stored size of the blob
    uint32_t usize;        // uncompressed size (== size when uncompressed)
};

// ---- per-asset blob headers (first bytes of each blob) ----

// Texture blobs are PER-TARGET ENCODED at bake time (tools/phxpack, docs/06 §4):
//   tier 2 (PC)  — format RGBA8:      width*height*4 bytes, row-major.
//   tier 1 (PSP) — format RGBA8_SWZ:  the same RGBA8 texels reordered into the GU's swizzled
//                  block layout (16-byte × 8-row blocks; see swz_texel_index below) so the
//                  GU samples the blob zero-copy with the swizzle bit set. Falls back to
//                  RGBA8 when the size doesn't block-align (width % 4 || height % 8).
//   tier 0 (GBA) — format PAL4_TILES: 4bpp paletted 8×8 tiles (the PPU's native layout; see
//                  TexturePal4Header below). Falls back to RGBA8 when the art can't be
//                  expressed (not 8px-aligned, or one tile needs >15 opaque colours) — the
//                  PPU backend then applies its upload-time quantizer (or rejects, exactly
//                  as it would have rejected the same art baked or not).
struct TextureBlobHeader {
    uint16_t width;
    uint16_t height;
    uint8_t  format;       // PixelFormat
    uint8_t  pad[3];
    // followed by the format-specific payload: raw RGBA8 rows, or the PAL4_TILES/RGBA8_SWZ
    // layouts defined next to PixelFormat in phx/core/pixel.h (kept in core so render can
    // consume them without a resource dependency — the same reason PixelFormat lives there)
};

struct TilemapBlobHeader {
    uint16_t width;        // in tiles
    uint16_t height;
    uint8_t  layers;
    uint8_t  tile_w;
    uint8_t  tile_h;
    uint8_t  flags;        // TilemapFlags (was pad — old bundles read as 0 = no extras)
    NameHash tileset;      // name hash of the tileset texture asset
    // followed by width*height*layers uint16 tile indices;
    // if (flags & kTilemapHasParallax): then — 4-byte aligned from the blob start (zero
    // padding after an odd index count; unaligned int32 loads fault on ARM) — `layers`
    // pairs of int32 Q16.16 {fx, fy} per-layer camera factors (1<<16 = moves with the
    // world), imported from Tiled's native parallaxx/parallaxy layer properties;
    // if (flags & kTilemapHasTileFlags): then — 4-byte aligned after the previous section —
    // a uint32 count followed by count uint8 per-tile collision flags (kTileFlag*, indexed
    // by tile index; index 0 = empty), imported from Tiled tileset per-tile properties.
};
enum : uint8_t {
    kTilemapHasParallax  = 1 << 0,
    kTilemapHasTileFlags = 1 << 1,
};

// Per-tile collision flags (the Tilemap blob's optional tile-flags table). Authored as Tiled
// tileset per-tile properties or class strings (tools/phxpack/tiled.h). The physics module's
// TileGrid constants (kTileSolid/kTileOneWay/kTileHazard, phx/physics/physics.h) mirror these
// values — physics stays resource-free by design, so the pair is guarded by a unit-test
// static_assert instead of an include.
enum : uint8_t {
    kTileFlagSolid  = 1 << 0,   // blocks all movement
    kTileFlagOneWay = 1 << 1,   // blocks only downward motion entering from above (platforms)
    kTileFlagHazard = 1 << 2,   // gameplay-defined (spikes/damage); physics only reports it
};

// Sound asset: mono 16-bit signed PCM at `rate` Hz. The runtime wraps it as an audio SoundView
// (kept POD here so `resource` needs no dependency on `audio`).
struct SoundBlobHeader {
    uint32_t frames;       // number of mono samples
    uint32_t rate;         // sample rate in Hz
    // followed by frames * int16
};

// Sprite asset: a sheet-frame grid + a table of named animation clips. The runtime maps this
// straight onto the `anim` module's SpriteSheet + AnimClip (the game builds an Animator from
// it); kept as POD here so `resource` (L2) needs no dependency on `anim` (L3).
struct SpriteClipDef {
    NameHash name;         // clip name hash ("walk"_hash) for lookup
    uint16_t first;        // first frame index into the sheet
    uint16_t count;        // number of frames
    uint8_t  fps;          // playback rate (0 = static)
    uint8_t  loop;         // 1 = loop, 0 = clamp on last frame
    uint16_t pad;
};

struct SpriteBlobHeader {
    NameHash texture;      // name hash of the sheet texture asset
    uint16_t frame_w;
    uint16_t frame_h;
    uint16_t cols;         // frames per atlas row
    uint16_t clip_count;
    // followed by clip_count * SpriteClipDef
};

// Spawn points authored in an object layer: a typed entity placement (the game switches on
// `type` to spawn the right entity). Coordinates are in pixels (Tiled's object space).
struct SpawnDef {
    NameHash type;         // object type/class hash ("coin"_hash)
    int16_t  x, y;         // position in pixels
    uint16_t w, h;         // size in pixels (0 if a point)
};

struct SpawnBlobHeader {
    uint32_t count;
    // followed by count * SpawnDef
};

// Lock the on-disk layout: the offline writer and the runtime reader must agree exactly,
// and the layout must be identical across all targets (all LE, same natural alignment).
static_assert(sizeof(BundleHeader)      == 24, "BundleHeader layout changed");
static_assert(sizeof(TocEntry)          == 20, "TocEntry layout changed");
static_assert(sizeof(TextureBlobHeader) == 8,  "TextureBlobHeader layout changed");
static_assert(sizeof(TilemapBlobHeader) == 12, "TilemapBlobHeader layout changed");
static_assert(sizeof(SpriteClipDef)     == 12, "SpriteClipDef layout changed");
static_assert(sizeof(SpriteBlobHeader)  == 12, "SpriteBlobHeader layout changed");
static_assert(sizeof(SpawnDef)          == 12, "SpawnDef layout changed");
static_assert(sizeof(SpawnBlobHeader)   == 4,  "SpawnBlobHeader layout changed");
static_assert(sizeof(SoundBlobHeader)   == 8,  "SoundBlobHeader layout changed");

} // namespace phx
#endif // PHX_RESOURCE_BUNDLE_H
