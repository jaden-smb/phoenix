// phx/resource/src/cache.cpp — bundle mount + zero-copy view extraction. The only work at
// runtime is: validate the header, binary-search the TOC, and pointer-cast into the blob.
#include "phx/resource/cache.h"
#include "phx/resource/lz.h"
#include "phx/platform/platform.h"

namespace phx {

Result<ResourceCache*> ResourceCache::create(ArenaAllocator& a) {
    ResourceCache* c = a.make<ResourceCache>();
    if (!c) return Result<ResourceCache*>::fail(Status::OutOfMemory);
    c->arena_ = &a;                              // kept for lazy decompression buffers
    return Result<ResourceCache*>::good(c);
}

Status ResourceCache::mount(const phx_platform* plat, const char* path) {
    if (mount_count_ >= kMaxMounts) return Status::OutOfMemory;

    size_t size = 0;
    phx_file* f = plat->open(path, &size);
    if (!f) return Status::NotFound;

    const uint8_t* base = static_cast<const uint8_t*>(plat->map(f));
    if (!base || size < sizeof(BundleHeader)) { plat->close(f); return Status::IoError; }

    const BundleHeader* h = reinterpret_cast<const BundleHeader*>(base);
    if (h->magic != kBundleMagic) { plat->close(f); return Status::IoError; }       // wrong format / endianness
    if (h->version > kBundleVersion) { plat->close(f); return Status::Unsupported; } // newer major
    if (h->toc_offset + h->asset_count * sizeof(TocEntry) > size) { plat->close(f); return Status::IoError; }

    Mounted& m = mounts_[mount_count_++];
    m.plat  = plat;
    m.file  = f;
    m.base  = base;
    m.toc   = reinterpret_cast<const TocEntry*>(base + h->toc_offset);
    m.count = h->asset_count;
    // Lazy-decompression cache: one pointer per asset, null until first touched. Only
    // compressed assets ever populate a slot; uncompressed ones stay zero-copy into the map.
    m.decoded = m.count ? arena_->alloc_array<const uint8_t*>(m.count) : nullptr;
    for (uint32_t i = 0; i < m.count; ++i) m.decoded[i] = nullptr;
    total_assets_ += m.count;
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
