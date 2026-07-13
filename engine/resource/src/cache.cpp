// phx/resource/src/cache.cpp — bundle mount + zero-copy view extraction. The only work at
// runtime is: validate the header, binary-search the TOC, and pointer-cast into the blob.
#include "phx/resource/cache.h"
#include "phx/resource/lz.h"
#include "phx/platform/platform.h"
#include "phx/core/crc32.h"

namespace phx {

namespace {
// The bundle `target` tier this BINARY expects, or 0xFF to skip the check entirely. Gated on
// the HARDWARE macros, never PHX_TARGET_GBA/PHX_TARGET_PSP alone: `TIER=gba_sim` (the host
// fixed-point-scalar simulation, see CLAUDE.md) sets PHX_TARGET_GBA but still runs the soft
// renderer against ordinary tier-2 test bundles, so it must NOT be held to the GBA-tier target.
#if defined(PHX_GBA_HW)
constexpr uint8_t kExpectedTarget = 0;
#elif defined(PHX_TARGET_PSP)
constexpr uint8_t kExpectedTarget = 1;
#else
constexpr uint8_t kExpectedTarget = 0xFF;   // host/PC: any target may be mounted (tools, tests)
#endif
} // namespace

Result<ResourceCache*> ResourceCache::create(ArenaAllocator& a) {
    ResourceCache* c = a.make<ResourceCache>();
    if (!c) return Result<ResourceCache*>::fail(Status::OutOfMemory);
    c->arena_ = &a;                              // kept for lazy decompression buffers
    return Result<ResourceCache*>::good(c);
}

Status ResourceCache::mount(const phx_platform* plat, const char* path, bool verify_checksum) {
    const NameHash path_hash = fnv1a(path);
    for (uint32_t i = 0; i < mount_count_; ++i)
        if (mounts_[i].path_hash == path_hash) return Status::Ok;   // idempotent re-mount

    if (mount_count_ >= kMaxMounts) return Status::OutOfMemory;

    size_t size = 0;
    phx_file* f = plat->open(path, &size);
    if (!f) return Status::NotFound;

    const uint8_t* base = static_cast<const uint8_t*>(plat->map(f));
    if (!base || size < sizeof(BundleHeader)) { plat->close(f); return Status::IoError; }

    const BundleHeader* h = reinterpret_cast<const BundleHeader*>(base);
    if (h->magic != kBundleMagic) { plat->close(f); return Status::IoError; }       // wrong format / endianness
    if (h->version > kBundleVersion) { plat->close(f); return Status::Unsupported; } // newer major
    if (h->total_size > size) { plat->close(f); return Status::IoError; }           // truncated file
    // toc_offset arithmetic below can't overflow uint32: asset_count is bounded by total_size
    // (each TocEntry is >= 1 byte of blob), and total_size <= the mapped file size.
    const uint64_t toc_end = uint64_t(h->toc_offset) + uint64_t(h->asset_count) * sizeof(TocEntry);
    if (h->toc_offset < sizeof(BundleHeader) || toc_end > h->total_size) {
        plat->close(f); return Status::IoError;
    }

    const TocEntry* toc = reinterpret_cast<const TocEntry*>(base + h->toc_offset);
    for (uint32_t i = 0; i < h->asset_count; ++i) {
        const uint64_t blob_end = uint64_t(toc[i].offset) + toc[i].size;
        if (toc[i].offset < toc_end || blob_end > h->total_size || toc[i].usize < toc[i].size) {
            plat->close(f); return Status::IoError;   // an asset points outside the bundle
        }
    }

    if (verify_checksum && h->blob_crc32 != 0) {
        const uint32_t got = crc32_of(base + h->toc_offset, h->total_size - h->toc_offset);
        if (got != h->blob_crc32) { plat->close(f); return Status::Corrupt; }
    }

    if (kExpectedTarget != 0xFF && h->target != kExpectedTarget) {
        plat->close(f); return Status::Unsupported;   // baked for a different console target
    }

    Mounted& m = mounts_[mount_count_++];
    m.plat  = plat;
    m.file  = f;
    m.base  = base;
    m.toc   = toc;
    m.count = h->asset_count;
    m.path_hash = path_hash;
    // Lazy-decompression cache: one pointer per asset, null until first touched. Only
    // compressed assets ever populate a slot; uncompressed ones stay zero-copy into the map.
    m.decoded = m.count ? arena_->alloc_array<const uint8_t*>(m.count) : nullptr;
    for (uint32_t i = 0; i < m.count; ++i) m.decoded[i] = nullptr;
    total_assets_ += m.count;
    return Status::Ok;
}

Status ResourceCache::unmount(const phx_platform* plat, const char* path) {
    const NameHash path_hash = fnv1a(path);
    for (uint32_t i = 0; i < mount_count_; ++i) {
        if (mounts_[i].path_hash != path_hash) continue;
        (plat ? plat : mounts_[i].plat)->close(mounts_[i].file);
        total_assets_ -= mounts_[i].count;
        for (uint32_t j = i + 1; j < mount_count_; ++j) mounts_[j - 1] = mounts_[j];
        --mount_count_;
        return Status::Ok;
    }
    return Status::NotFound;
}

Status ResourceCache::unmount_all(const phx_platform* plat) {
    for (uint32_t i = 0; i < mount_count_; ++i)
        (plat ? plat : mounts_[i].plat)->close(mounts_[i].file);
    mount_count_ = 0;
    total_assets_ = 0;
    return Status::Ok;
}

const uint8_t* ResourceCache::resolve(const TocEntry* e) {
    for (uint32_t mi = 0; mi < mount_count_; ++mi) {
        Mounted& m = mounts_[mi];
        if (e >= m.toc && e < m.toc + m.count) {
            if (!(e->flags & kTocLZ)) return m.base + e->offset;   // uncompressed: zero-copy
            const uint32_t idx = uint32_t(e - m.toc);
            if (m.decoded[idx]) return m.decoded[idx];             // already decompressed
            uint8_t* buf = static_cast<uint8_t*>(arena_->alloc(e->usize, 16));
            if (!buf) return nullptr;                              // arena exhausted
            if (lz_decode(m.base + e->offset, e->size, buf, e->usize) != e->usize)
                return nullptr;                                    // corrupt / truncated
            m.decoded[idx] = buf;
            return buf;
        }
    }
    return nullptr;
}

const TocEntry* ResourceCache::find(NameHash name, AssetType type) const {
    // search mounts in order; TOC within a mount is sorted by name_hash -> binary search.
    for (uint32_t mi = 0; mi < mount_count_; ++mi) {
        const Mounted& m = mounts_[mi];
        uint32_t lo = 0, hi = m.count;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            NameHash h = m.toc[mid].name_hash;
            if (h < name)      lo = mid + 1;
            else if (h > name) hi = mid;
            else {
                if (m.toc[mid].type == uint16_t(type)) return &m.toc[mid];
                // hash match but wrong type: scan neighbours sharing the hash
                for (uint32_t j = mid; j-- > 0 && m.toc[j].name_hash == name; )
                    if (m.toc[j].type == uint16_t(type)) return &m.toc[j];
                for (uint32_t j = mid + 1; j < m.count && m.toc[j].name_hash == name; ++j)
                    if (m.toc[j].type == uint16_t(type)) return &m.toc[j];
                break;
            }
        }
    }
    return nullptr;
}

Result<TextureView> ResourceCache::texture(NameHash name) {
    const TocEntry* e = find(name, AssetType::Texture);
    if (!e) return Result<TextureView>::fail(Status::NotFound);
    const uint8_t* p = resolve(e);
    if (!p) return Result<TextureView>::fail(Status::IoError);
    const TextureBlobHeader* th = reinterpret_cast<const TextureBlobHeader*>(p);
    TextureView v;
    v.width  = th->width;
    v.height = th->height;
    v.format = PixelFormat(th->format);
    v.pixels = p + sizeof(TextureBlobHeader);
    return Result<TextureView>::good(v);
}

Result<TilemapView> ResourceCache::tilemap(NameHash name) {
    const TocEntry* e = find(name, AssetType::Tilemap);
    if (!e) return Result<TilemapView>::fail(Status::NotFound);
    const uint8_t* p = resolve(e);
    if (!p) return Result<TilemapView>::fail(Status::IoError);
    const TilemapBlobHeader* mh = reinterpret_cast<const TilemapBlobHeader*>(p);
    TilemapView v;
    v.width   = mh->width;
    v.height  = mh->height;
    v.layers  = mh->layers;
    v.tile_w  = mh->tile_w;
    v.tile_h  = mh->tile_h;
    v.tileset = mh->tileset;
    v.indices = reinterpret_cast<const uint16_t*>(p + sizeof(TilemapBlobHeader));
    if (mh->flags & kTilemapHasParallax) {
        // The Q16 table sits after the indices, 4-aligned from the blob start (bundle.h).
        const size_t idx_end = sizeof(TilemapBlobHeader) +
                               size_t(mh->width) * mh->height * mh->layers * sizeof(uint16_t);
        v.parallax_q16 = reinterpret_cast<const int32_t*>(p + ((idx_end + 3) & ~size_t(3)));
    }
    return Result<TilemapView>::good(v);
}

Result<SpriteView> ResourceCache::sprite(NameHash name) {
    const TocEntry* e = find(name, AssetType::Sprite);
    if (!e) return Result<SpriteView>::fail(Status::NotFound);
    const uint8_t* p = resolve(e);
    if (!p) return Result<SpriteView>::fail(Status::IoError);
    const SpriteBlobHeader* sh = reinterpret_cast<const SpriteBlobHeader*>(p);
    SpriteView v;
    v.texture    = sh->texture;
    v.frame_w    = sh->frame_w;
    v.frame_h    = sh->frame_h;
    v.cols       = sh->cols;
    v.clip_count = sh->clip_count;
    v.clips      = reinterpret_cast<const SpriteClipDef*>(p + sizeof(SpriteBlobHeader));
    return Result<SpriteView>::good(v);
}

Result<SpawnsView> ResourceCache::spawns(NameHash name) {
    const TocEntry* e = find(name, AssetType::Spawns);
    if (!e) return Result<SpawnsView>::fail(Status::NotFound);
    const uint8_t* p = resolve(e);
    if (!p) return Result<SpawnsView>::fail(Status::IoError);
    const SpawnBlobHeader* sh = reinterpret_cast<const SpawnBlobHeader*>(p);
    SpawnsView v;
    v.count  = sh->count;
    v.spawns = reinterpret_cast<const SpawnDef*>(p + sizeof(SpawnBlobHeader));
    return Result<SpawnsView>::good(v);
}

Result<SoundDataView> ResourceCache::sound(NameHash name) {
    const TocEntry* e = find(name, AssetType::Sound);
    if (!e) return Result<SoundDataView>::fail(Status::NotFound);
    const uint8_t* p = resolve(e);
    if (!p) return Result<SoundDataView>::fail(Status::IoError);
    const SoundBlobHeader* sh = reinterpret_cast<const SoundBlobHeader*>(p);
    SoundDataView v;
    v.frames  = sh->frames;
    v.rate    = sh->rate;
    v.samples = reinterpret_cast<const int16_t*>(p + sizeof(SoundBlobHeader));
    return Result<SoundDataView>::good(v);
}

Result<BlobView> ResourceCache::blob(NameHash name) {
    const TocEntry* e = find(name, AssetType::Blob);
    if (!e) return Result<BlobView>::fail(Status::NotFound);
    const uint8_t* p = resolve(e);
    if (!p) return Result<BlobView>::fail(Status::IoError);
    BlobView v;
    v.data = p;
    v.size = e->usize;            // logical (decompressed) size; == size when uncompressed
    return Result<BlobView>::good(v);
}

} // namespace phx
