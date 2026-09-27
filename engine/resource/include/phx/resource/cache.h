// phx/resource/cache.h — runtime resource access. Mounts a `.phxp` bundle through the
// platform seam (load-once heap image on PC, linked-in EBOOT data on PSP, ROM pointer
// on GBA — no OS mmap anywhere, the seam contract is just a stable view) and returns ZERO-COPY
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
// When the map gave its spawns names or per-instance properties (bundle.h: the spawn extension),
// they read by spawn index; otherwise name() is 0 and every property is absent (the default).
struct SpawnsView {
    uint32_t        count   = 0;
    const SpawnDef* spawns  = nullptr;
    const uint8_t*  names   = nullptr;     // count * NameHash (unaligned-safe reads)
    const uint8_t*  props   = nullptr;     // prop_count * SpawnPropDef
    uint32_t        prop_count = 0;
    const char*     strings = nullptr;     // the string table
    uint32_t        strings_size = 0;

    // The spawn's name hash ("door_a"_hash), 0 when unnamed.
    NameHash name(uint32_t i) const {
        NameHash h = 0;
        if (names && i < count) for (int b = 0; b < 4; ++b) h |= NameHash(names[i * 4 + b]) << (8 * b);
        return h;
    }
    // The first spawn called `name`, else -1.
    int32_t find_named(NameHash n) const {
        for (uint32_t i = 0; n && i < count; ++i) if (name(i) == n) return int32_t(i);
        return -1;
    }
    // Spawn i's property `key`.
    bool prop(uint32_t i, NameHash key, SpawnPropDef& out) const {
        for (uint32_t p = 0; p < prop_count; ++p) {
            const uint8_t* d = props + size_t(p) * sizeof(SpawnPropDef);
            SpawnPropDef def;
            for (size_t b = 0; b < sizeof(def); ++b) reinterpret_cast<uint8_t*>(&def)[b] = d[b];
            if (def.spawn == i && def.key == key) { out = def; return true; }
        }
        return false;
    }
    bool has(uint32_t i, NameHash key) const { SpawnPropDef d; return prop(i, key, d); }
    // An int / bool / float (truncated) property, else `def`.
    int32_t get_int(uint32_t i, NameHash key, int32_t def = 0) const {
        SpawnPropDef d;
        if (!prop(i, key, d) || d.type == kPropStr) return def;
        if (d.type == kPropFloat) { float f; const int32_t v = d.value; for (size_t b = 0; b < 4; ++b) reinterpret_cast<uint8_t*>(&f)[b] = reinterpret_cast<const uint8_t*>(&v)[b]; return int32_t(f); }
        return d.value;
    }
    // A numeric property as Q16.16 (a float keeps its fraction). False when absent or a string.
    bool get_q16(uint32_t i, NameHash key, int32_t& out) const {
        SpawnPropDef d;
        if (!prop(i, key, d) || d.type == kPropStr) return false;
        if (d.type == kPropFloat) {
            float f; const int32_t v = d.value;
            for (size_t b = 0; b < 4; ++b) reinterpret_cast<uint8_t*>(&f)[b] = reinterpret_cast<const uint8_t*>(&v)[b];
            const float q = f * 65536.0f;
            out = q >= 2147483520.0f ? INT32_MAX : q <= -2147483648.0f ? INT32_MIN : int32_t(q);
            return true;
        }
        const int64_t v = int64_t(d.value) * 65536;               // clamped to the Q16 range
        out = int32_t(v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : v);
        return true;
    }
    // A string property (NUL-terminated, in the bundle), else nullptr.
    const char* get_str(uint32_t i, NameHash key) const {
        SpawnPropDef d;
        if (!prop(i, key, d) || d.type != kPropStr || d.value < 0 || uint32_t(d.value) >= strings_size) return nullptr;
        return strings + d.value;
    }
    // The FNV-1a hash of a string property, 0 when absent or empty.
    NameHash get_hash(uint32_t i, NameHash key) const {
        const char* s = get_str(i, key);
        if (!s || !*s) return 0;
        NameHash h = kFnvOffset;
        for (; *s; ++s) h = (h ^ NameHash(uint8_t(*s))) * kFnvPrime;
        return h;
    }
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
    // Is there an asset `name` of `type`? Quiet: unlike the typed getters, a name that exists
    // only as another type logs nothing (for loaders that try one type, then another).
    bool has(NameHash name, AssetType type) const;

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
    const TocEntry* lookup(NameHash, AssetType, bool& saw_other, uint16_t& other_type) const;
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
