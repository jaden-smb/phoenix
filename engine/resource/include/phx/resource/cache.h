// phx/resource/cache.h — runtime resource access. Mounts a `.phxp` bundle through the
// platform seam (mmap on PC, load-once on PSP, ROM pointer on GBA) and returns ZERO-COPY
// typed views into it — assets are never parsed at runtime, only pointer-cast. See
// docs/06-resources.md. Depends only on core + memory + platform (no render dependency:
// the caller turns a TextureView into a renderer texture, keeping layering clean).
#ifndef PHX_RESOURCE_CACHE_H
#define PHX_RESOURCE_CACHE_H

#include "phx/core/types.h"
#include "phx/core/pixel.h"
#include "phx/memory/allocators.h"
#include "phx/resource/bundle.h"

struct phx_platform;
struct phx_file;

namespace phx {

struct TextureView {
    const void* pixels = nullptr;
    uint16_t    width  = 0;
    uint16_t    height = 0;
    PixelFormat format = PixelFormat::RGBA8;
};

struct TilemapView {
    const uint16_t* indices = nullptr;
    uint16_t        width   = 0;     // in tiles
    uint16_t        height  = 0;
    uint8_t         layers  = 1;
    uint8_t         tile_w  = 8;
    uint8_t         tile_h  = 8;
    NameHash        tileset = 0;
    // Per-layer camera parallax factors: `layers` pairs of Q16.16 {fx, fy}, or nullptr when
    // the map has none (every layer moves with the world). Feed to set_tilemap_parallax.
    const int32_t*  parallax_q16 = nullptr;
    // Per-tile collision flags (kTileFlag*, bundle.h), indexed by tile index; nullptr when
    // the map was authored without tileset collision metadata (then the physics TileGrid's
    // solid_from fallback applies). Feed to TileGrid.flags / flag_count.
    const uint8_t*  tile_flags = nullptr;
    uint32_t        tile_flag_count = 0;
};

struct BlobView {
    const void* data = nullptr;
    uint32_t    size = 0;
};

// Object-layer spawn points (e.g. imported from a Tiled map). Zero-copy view over the table.
struct SpawnsView {
    uint32_t        count   = 0;
    const SpawnDef* spawns  = nullptr;
};

// Mono 16-bit PCM, ready to wrap as an audio SoundView (kept render/audio-free here).
struct SoundDataView {
    const int16_t* samples = nullptr;
    uint32_t       frames  = 0;
    uint32_t       rate    = 0;
};

// Sprite-sheet metadata: the frame grid + a table of named clips. The caller maps this onto
// the anim module (SpriteSheet + AnimClip) and resolves `texture` via texture(). Zero-copy
// (or decompressed once), exactly like the other views.
struct SpriteView {
    NameHash             texture    = 0;
    uint16_t             frame_w    = 0;
    uint16_t             frame_h    = 0;
    uint16_t             cols       = 0;
    uint16_t             clip_count = 0;
    const SpriteClipDef* clips      = nullptr;   // clip_count entries
};

// Compile-time name hashing so call sites cost nothing: cache.texture("hero"_hash)
//
// Validation performed at mount() (docs/06-resources.md §1/§7): magic + version (refuses a
// newer major), every TOC entry's [offset, offset+size) bounds-checked against the mapped
// file so a corrupt/truncated bundle can never drive an out-of-bounds read, an optional
// (default on) CRC32 of the TOC+blob region against BundleHeader.blob_crc32, and — ONLY on a
// real console build (PHX_GBA_HW / PHX_TARGET_PSP; never the host, incl. `TIER=gba_sim`,
// which only simulates the fixed-point *scalar* tier, not real hardware — see CLAUDE.md) — that
// the bundle's `target` byte matches the tier the binary actually ships on, so a bundle baked
// for the wrong console can never load silently.
//
// Lifecycle: mount() is idempotent — mounting the same path twice is a no-op, not a wasted
// slot. unmount()/unmount_all() close the platform file/mapping and free the mount slot for
// reuse; ANY view/pointer obtained from this cache before that call (TextureView, a raw TOC
// blob pointer, …) is invalidated the instant it returns — zero-copy views alias the mapping,
// they are never copied out.
class ResourceCache {
public:
    static Result<ResourceCache*> create(ArenaAllocator&);

    // Mount a bundle via the platform's open/map. The mapped view stays valid until
    // unmount()/unmount_all()/shutdown. Multiple bundles can be mounted (searched in mount
    // order; kMaxMounts slots, reused after unmount()). `verify_checksum=false` skips the
    // CRC32 pass (still does every structural check) when mount-time latency matters more
    // than catching bit-rot/bad-flash — the default favors safety.
    Status mount(const phx_platform*, const char* path, bool verify_checksum = true);

    // Unmount a previously-mounted bundle by path (compacts the mount table). NotFound if no
    // mount matches. Invalidates every view/pointer this cache handed out from that bundle.
    Status unmount(const phx_platform*, const char* path);
    // Unmount everything (app shutdown, or a clean slate before mounting a new level's set).
    Status unmount_all(const phx_platform*);

    Result<TextureView> texture(NameHash);
    Result<TilemapView> tilemap(NameHash);
    Result<SpriteView>  sprite(NameHash);
    Result<SpawnsView>  spawns(NameHash);
    Result<SoundDataView> sound(NameHash);
    Result<BlobView>    blob(NameHash);

    uint32_t asset_count() const { return total_assets_; }
    uint32_t mount_count() const { return mount_count_; }

private:
    ResourceCache() = default;
    friend class ArenaAllocator;

    struct Mounted {
        const phx_platform* plat = nullptr;
        ::phx_file*         file = nullptr;     // global C type from the platform seam
        const uint8_t*      base = nullptr;     // mapped bundle start
        const TocEntry*     toc  = nullptr;
        uint32_t            count = 0;
        const uint8_t**     decoded = nullptr;  // per-asset decompressed buffer (lazy; null=raw/zero-copy)
        NameHash            path_hash = 0;      // fnv1a(path); identifies the mount for unmount()
    };

    const TocEntry* find(NameHash, AssetType) const;
    // Resolve a TOC entry to the bytes of its blob, decompressing once into the arena and
    // caching the result if the asset is stored compressed (uncompressed stays zero-copy).
    const uint8_t* resolve(const TocEntry*);

    static constexpr uint32_t kMaxMounts = 4;
    ArenaAllocator* arena_ = nullptr;           // for lazy decompression buffers
    Mounted  mounts_[kMaxMounts];
    uint32_t mount_count_  = 0;
    uint32_t total_assets_ = 0;
};

} // namespace phx
#endif // PHX_RESOURCE_CACHE_H
