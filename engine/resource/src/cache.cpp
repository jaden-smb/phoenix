// phx/resource/src/cache.cpp — bundle mount + zero-copy view extraction. The only work at
// runtime is: validate the header, binary-search the TOC, and pointer-cast into the blob.
#include "phx/resource/cache.h"
#include "phx/resource/lz.h"
#include "phx/platform/platform.h"
#include "phx/core/crc32.h"
#include "phx/core/log.h"

#include <cstring>

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

    if (mount_count_ >= kMaxMounts) {
        PHX_LOG_ERROR("resource: mount '%s': all %u mount slots in use", path, kMaxMounts);
        return Status::OutOfMemory;
    }

    size_t size = 0;
    phx_file* f = plat->open(path, &size);
    if (!f) {
        PHX_LOG_ERROR("resource: mount '%s': file not found/openable", path);
        return Status::NotFound;
    }

    const uint8_t* base = static_cast<const uint8_t*>(plat->map(f));
    if (!base || size < sizeof(BundleHeader)) {
        PHX_LOG_ERROR("resource: mount '%s': too small to be a bundle (%u bytes)", path, uint32_t(size));
        plat->close(f); return Status::IoError;
    }

    const BundleHeader* h = reinterpret_cast<const BundleHeader*>(base);
    if (h->magic != kBundleMagic) {                                  // wrong format / endianness
        PHX_LOG_ERROR("resource: mount '%s': bad magic (not a .phxp bundle)", path);
        plat->close(f); return Status::IoError;
    }
    if (h->version > kBundleVersion) {                               // newer major
        PHX_LOG_ERROR("resource: mount '%s': bundle version %u, this runtime reads <= %u (re-bake)",
                      path, h->version, kBundleVersion);
        plat->close(f); return Status::Unsupported;
    }
    if (h->total_size > size) {                                      // truncated file
        PHX_LOG_ERROR("resource: mount '%s': truncated (header says %u bytes, file has %u)",
                      path, h->total_size, uint32_t(size));
        plat->close(f); return Status::IoError;
    }
    // toc_offset arithmetic below can't overflow uint32: asset_count is bounded by total_size
    // (each TocEntry is >= 1 byte of blob), and total_size <= the mapped file size.
    const uint64_t toc_end = uint64_t(h->toc_offset) + uint64_t(h->asset_count) * sizeof(TocEntry);
    if (h->toc_offset < sizeof(BundleHeader) || toc_end > h->total_size) {
        PHX_LOG_ERROR("resource: mount '%s': TOC out of bounds", path);
        plat->close(f); return Status::IoError;
    }

    const TocEntry* toc = reinterpret_cast<const TocEntry*>(base + h->toc_offset);
    for (uint32_t i = 0; i < h->asset_count; ++i) {
        const uint64_t blob_end = uint64_t(toc[i].offset) + toc[i].size;
        if (toc[i].offset < toc_end || blob_end > h->total_size || toc[i].usize < toc[i].size) {
            PHX_LOG_ERROR("resource: mount '%s': asset %u points outside the bundle", path, i);
            plat->close(f); return Status::IoError;
        }
    }

    if (verify_checksum && h->blob_crc32 != 0) {
        const uint32_t got = crc32_of(base + h->toc_offset, h->total_size - h->toc_offset);
        if (got != h->blob_crc32) {
            PHX_LOG_ERROR("resource: mount '%s': CRC mismatch (corrupt/hand-edited bundle)", path);
            plat->close(f); return Status::Corrupt;
        }
    }

    if (kExpectedTarget != 0xFF && h->target != kExpectedTarget) {   // baked for a different console
        PHX_LOG_ERROR("resource: mount '%s': baked for target tier %u, this build ships tier %u "
                      "(re-bake with --target %u)", path, h->target, kExpectedTarget, kExpectedTarget);
        plat->close(f); return Status::Unsupported;
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

bool ResourceCache::has(NameHash name, AssetType type) const {
    uint16_t other = 0;
    bool saw = false;
    return lookup(name, type, saw, other) != nullptr;
}

const TocEntry* ResourceCache::find(NameHash name, AssetType type) const {
    uint16_t other_type = 0;   // a hash hit under a different asset type (kept for the miss log)
    bool     saw_other  = false;
    if (const TocEntry* e = lookup(name, type, saw_other, other_type)) return e;
    if (saw_other)
        // The name exists but only as another type — almost always a call-site bug
        // (texture("level") for a tilemap asset), so say so instead of a bare miss.
        PHX_LOG_WARN("resource: asset 0x%08x exists but as type %u, not the requested %u",
                     uint32_t(name), unsigned(other_type), unsigned(type));
    else
        PHX_LOG_DEBUG("resource: asset 0x%08x (type %u) not found in %u mounted bundle(s)",
                      uint32_t(name), unsigned(type), mount_count_);
    return nullptr;
}

const TocEntry* ResourceCache::lookup(NameHash name, AssetType type, bool& saw_other, uint16_t& other_type) const {
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
                other_type = m.toc[mid].type;
                saw_other  = true;
                break;                              // try the next mount
            }
        }
    }
    return nullptr;                                         // silent: find() reports the miss
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
    // Optional sections in a fixed order after the indices, each 4-aligned (bundle.h).
    size_t end = sizeof(TilemapBlobHeader) +
                 size_t(mh->width) * mh->height * mh->layers * sizeof(uint16_t);
    if (mh->flags & kTilemapHasParallax) {
        const size_t par_off = (end + 3) & ~size_t(3);
        v.parallax_q16 = reinterpret_cast<const int32_t*>(p + par_off);
        end = par_off + size_t(mh->layers) * 2 * sizeof(int32_t);
    }
    if (mh->flags & kTilemapHasTileFlags) {
        const size_t flg_off = (end + 3) & ~size_t(3);
        v.tile_flag_count = *reinterpret_cast<const uint32_t*>(p + flg_off);
        v.tile_flags = p + flg_off + sizeof(uint32_t);
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

    // The optional transitions trailer (bundle.h), bounds-checked like the spawn extension.
    const uint64_t end = sizeof(SpriteBlobHeader) + uint64_t(v.clip_count) * sizeof(SpriteClipDef);
    if (end + 8u <= e->usize) {
        uint32_t hdr[2];
        std::memcpy(hdr, p + end, sizeof(hdr));
        if (hdr[0] == kSpriteTransMagic && end + 8u + uint64_t(hdr[1]) * sizeof(SpriteTransDef) <= e->usize) {
            v.trans_count = hdr[1];
            v.trans       = reinterpret_cast<const SpriteTransDef*>(p + end + 8u);
        }
    }
    return Result<SpriteView>::good(v);
}

Result<FontView> ResourceCache::font(NameHash name) {
    const TocEntry* e = find(name, AssetType::Font);
    if (!e) return Result<FontView>::fail(Status::NotFound);
    const uint8_t* p = resolve(e);
    if (!p) return Result<FontView>::fail(Status::IoError);
    if (e->usize < sizeof(FontBlobHeader)) return Result<FontView>::fail(Status::Corrupt);
    FontBlobHeader h;
    std::memcpy(&h, p, sizeof(h));
    if (sizeof(FontBlobHeader) + uint64_t(h.glyph_count) * sizeof(FontGlyphDef) > e->usize)
        return Result<FontView>::fail(Status::Corrupt);
    FontView v;
    v.texture = h.texture; v.glyph_count = h.glyph_count; v.first_char = h.first_char;
    v.line_h = h.line_h; v.cell_w = h.cell_w; v.cell_h = h.cell_h; v.advance = h.advance; v.flags = h.flags;
    v.glyphs = reinterpret_cast<const FontGlyphDef*>(p + sizeof(FontBlobHeader));
    return Result<FontView>::good(v);
}

Result<DialogueData> ResourceCache::dialogue(NameHash name) {
    const TocEntry* e = find(name, AssetType::Dialogue);
    if (!e) return Result<DialogueData>::fail(Status::NotFound);
    const uint8_t* p = resolve(e);
    if (!p) return Result<DialogueData>::fail(Status::IoError);
    if (e->usize < sizeof(DialogueHeader)) return Result<DialogueData>::fail(Status::Corrupt);
    DialogueHeader h;
    std::memcpy(&h, p, sizeof(h));
    if (h.magic != kDialogueMagic) return Result<DialogueData>::fail(Status::Corrupt);
    uint64_t at = sizeof(DialogueHeader);
    DialogueData d;
    d.convs    = reinterpret_cast<const DlgConvDef*>(p + at);    at += uint64_t(h.conv_count) * sizeof(DlgConvDef);
    d.speakers = reinterpret_cast<const DlgSpeakerDef*>(p + at); at += uint64_t(h.speaker_count) * sizeof(DlgSpeakerDef);
    d.nodes    = reinterpret_cast<const DlgNodeDef*>(p + at);    at += uint64_t(h.node_count) * sizeof(DlgNodeDef);
    d.choices  = reinterpret_cast<const DlgChoiceDef*>(p + at);  at += uint64_t(h.choice_count) * sizeof(DlgChoiceDef);
    d.ops      = reinterpret_cast<const DlgOp*>(p + at);         at += uint64_t(h.op_count) * sizeof(DlgOp);
    d.strings  = reinterpret_cast<const char*>(p + at);          at += h.strings_size;
    if (at > e->usize) return Result<DialogueData>::fail(Status::Corrupt);
    d.conv_count = h.conv_count; d.speaker_count = h.speaker_count; d.node_count = h.node_count;
    d.choice_count = h.choice_count; d.op_count = h.op_count; d.strings_size = h.strings_size;
    // every index inside its section (a hand-edited or truncated asset never drives a bad read)
    auto next_ok = [&](uint16_t n) { return n == kDlgEnd || n < d.node_count; };
    for (uint16_t i = 0; i < d.conv_count; ++i)
        if (d.convs[i].first_node >= d.node_count && d.convs[i].node_count) return Result<DialogueData>::fail(Status::Corrupt);
    for (uint16_t i = 0; i < d.node_count; ++i) {
        const DlgNodeDef& n = d.nodes[i];
        if (!next_ok(n.next) || (n.speaker != kDlgNoSpeaker && n.speaker >= d.speaker_count) ||
            uint32_t(n.first_choice) + n.choice_count > d.choice_count || uint32_t(n.first_op) + n.op_count > d.op_count ||
            n.text >= d.strings_size)
            return Result<DialogueData>::fail(Status::Corrupt);
    }
    for (uint16_t i = 0; i < d.choice_count; ++i) {
        const DlgChoiceDef& c = d.choices[i];
        if (!next_ok(c.next) || uint32_t(c.first_op) + c.op_count > d.op_count || c.text >= d.strings_size)
            return Result<DialogueData>::fail(Status::Corrupt);
    }
    for (uint16_t i = 0; i < d.speaker_count; ++i)
        if (d.speakers[i].name >= d.strings_size) return Result<DialogueData>::fail(Status::Corrupt);
    if (d.strings_size && d.strings[d.strings_size - 1] != 0) return Result<DialogueData>::fail(Status::Corrupt);
    return Result<DialogueData>::good(d);
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

    // The optional extension (bundle.h): names + per-instance properties, bounds-checked against
    // the asset's size so a truncated or foreign trailer is simply ignored.
    const uint64_t end = sizeof(SpawnBlobHeader) + uint64_t(v.count) * sizeof(SpawnDef);
    const uint64_t ext = (end + 3u) & ~uint64_t(3);
    if (ext + 12u <= e->usize) {
        uint32_t hdr[3];
        std::memcpy(hdr, p + ext, sizeof(hdr));
        const uint64_t names_at = ext + 12u;
        const uint64_t props_at = names_at + uint64_t(v.count) * 4u;
        const uint64_t strs_at  = props_at + uint64_t(hdr[1]) * sizeof(SpawnPropDef);
        if (hdr[0] == kSpawnExtMagic && strs_at + hdr[2] <= e->usize) {
            v.names = p + names_at;
            v.props = p + props_at;
            v.prop_count = hdr[1];
            v.strings = reinterpret_cast<const char*>(p + strs_at);
            v.strings_size = hdr[2];
        }
    }
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
