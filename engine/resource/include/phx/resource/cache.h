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
class ResourceCache {
public:
    static Result<ResourceCache*> create(ArenaAllocator&);

    // Mount a bundle via the platform's open/map. The mapped view stays valid until
    // unmount()/shutdown. Multiple bundles can be mounted (searched in mount order).
    Status mount(const phx_platform*, const char* path);

    Result<TextureView> texture(NameHash);
    Result<TilemapView> tilemap(NameHash);
    Result<SpriteView>  sprite(NameHash);
    Result<SpawnsView>  spawns(NameHash);
    Result<SoundDataView> sound(NameHash);
    Result<BlobView>    blob(NameHash);

    uint32_t asset_count() const { return total_assets_; }

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
