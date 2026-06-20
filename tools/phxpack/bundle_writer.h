// tools/phxpack/bundle_writer.h — host-side `.phxp` assembler. STL is fine here (tools
// never ship to console). Accumulates assets, then writes header + sorted TOC + blobs.
// Shares the format definition (phx/resource/bundle.h) with the runtime reader.
#ifndef PHX_TOOLS_BUNDLE_WRITER_H
#define PHX_TOOLS_BUNDLE_WRITER_H

#include "phx/resource/bundle.h"
#include "phx/core/types.h"
#include "phx/core/pixel.h"
#include "lz_encode.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace phxtool {

class BundleWriter {
public:
    explicit BundleWriter(uint8_t target_tier = 2) : target_(target_tier) {}

    // Enable LZSS compression of blobs. Each asset is compressed independently at write()
    // time and stored compressed ONLY if that actually shrinks it (else stored verbatim), so
    // turning this on never makes a bundle bigger and the reader handles a mix transparently.
    void set_compression(bool on) { compress_ = on; }

    void add_texture(const std::string& name, const uint32_t* rgba,
                     uint16_t w, uint16_t h) {
        phx::TextureBlobHeader th{};
        th.width = w; th.height = h; th.format = uint8_t(phx::PixelFormat::RGBA8);
        std::vector<uint8_t> blob(sizeof(th) + size_t(w) * h * 4);
        std::memcpy(blob.data(), &th, sizeof(th));
        std::memcpy(blob.data() + sizeof(th), rgba, size_t(w) * h * 4);
        push(name, phx::AssetType::Texture, std::move(blob));
    }

    void add_tilemap(const std::string& name, const uint16_t* indices,
                     uint16_t w, uint16_t h, uint8_t layers,
                     uint8_t tile_w, uint8_t tile_h, const std::string& tileset) {
        phx::TilemapBlobHeader mh{};
        mh.width = w; mh.height = h; mh.layers = layers;
        mh.tile_w = tile_w; mh.tile_h = tile_h;
        mh.tileset = phx::fnv1a(tileset.c_str());
        const size_t n = size_t(w) * h * layers;
        std::vector<uint8_t> blob(sizeof(mh) + n * sizeof(uint16_t));
        std::memcpy(blob.data(), &mh, sizeof(mh));
        std::memcpy(blob.data() + sizeof(mh), indices, n * sizeof(uint16_t));
        push(name, phx::AssetType::Tilemap, std::move(blob));
    }

    // Sprite-sheet metadata: a frame grid over a (separately added) texture + named clips.
    void add_sprite(const std::string& name, const std::string& texture,
                    uint16_t frame_w, uint16_t frame_h, uint16_t cols,
                    const std::vector<phx::SpriteClipDef>& clips) {
        phx::SpriteBlobHeader sh{};
        sh.texture = phx::fnv1a(texture.c_str());
        sh.frame_w = frame_w; sh.frame_h = frame_h; sh.cols = cols;
        sh.clip_count = uint16_t(clips.size());
        std::vector<uint8_t> blob(sizeof(sh) + clips.size() * sizeof(phx::SpriteClipDef));
        std::memcpy(blob.data(), &sh, sizeof(sh));
        if (!clips.empty())
            std::memcpy(blob.data() + sizeof(sh), clips.data(), clips.size() * sizeof(phx::SpriteClipDef));
        push(name, phx::AssetType::Sprite, std::move(blob));
    }

    // Mono 16-bit PCM sound at `rate` Hz (e.g. decoded from a WAV).
    void add_sound(const std::string& name, const int16_t* samples, uint32_t frames, uint32_t rate) {
        phx::SoundBlobHeader sh{}; sh.frames = frames; sh.rate = rate;
        std::vector<uint8_t> blob(sizeof(sh) + size_t(frames) * sizeof(int16_t));
        std::memcpy(blob.data(), &sh, sizeof(sh));
        if (frames) std::memcpy(blob.data() + sizeof(sh), samples, size_t(frames) * sizeof(int16_t));
        push(name, phx::AssetType::Sound, std::move(blob));
    }

    // Object-layer spawn points (e.g. from a Tiled object group).
    void add_spawns(const std::string& name, const std::vector<phx::SpawnDef>& spawns) {
        phx::SpawnBlobHeader sh{}; sh.count = uint32_t(spawns.size());
        std::vector<uint8_t> blob(sizeof(sh) + spawns.size() * sizeof(phx::SpawnDef));
        std::memcpy(blob.data(), &sh, sizeof(sh));
        if (!spawns.empty())
            std::memcpy(blob.data() + sizeof(sh), spawns.data(), spawns.size() * sizeof(phx::SpawnDef));
        push(name, phx::AssetType::Spawns, std::move(blob));
    }

    void add_blob(const std::string& name, const void* data, uint32_t size) {
        std::vector<uint8_t> blob(size);
        std::memcpy(blob.data(), data, size);
        push(name, phx::AssetType::Blob, std::move(blob));
    }

    // Assemble and write. Returns false on I/O error.
    bool write(const std::string& path) {
        // sort by name hash so the runtime can binary-search the TOC
        std::sort(entries_.begin(), entries_.end(),
                  [](const Entry& a, const Entry& b) { return a.hash < b.hash; });

        const uint32_t count    = uint32_t(entries_.size());
        const uint32_t toc_off  = uint32_t(sizeof(phx::BundleHeader));
        const uint32_t blob_off = toc_off + count * uint32_t(sizeof(phx::TocEntry));

        // Pick each blob's STORED bytes (compressed if smaller, else the raw blob). The TOC
        // records `usize` (decompressed) and `size` (stored) so the reader knows both.
        std::vector<const std::vector<uint8_t>*> stored(count);
        std::vector<std::vector<uint8_t>> packed(count);
        std::vector<uint16_t> flags(count, phx::kTocNone);
        for (uint32_t i = 0; i < count; ++i) {
            const std::vector<uint8_t>& raw = entries_[i].blob;
            stored[i] = &raw;
            if (compress_ && !raw.empty()) {
                lz_encode(raw.data(), uint32_t(raw.size()), packed[i]);
                if (packed[i].size() < raw.size()) { stored[i] = &packed[i]; flags[i] = phx::kTocLZ; }
            }
        }

        std::vector<phx::TocEntry> toc(count);
        uint32_t cursor = blob_off;
        for (uint32_t i = 0; i < count; ++i) {
            cursor = align16(cursor);
            toc[i].name_hash = entries_[i].hash;
            toc[i].type      = uint16_t(entries_[i].type);
            toc[i].flags     = flags[i];
            toc[i].offset    = cursor;
            toc[i].size      = uint32_t(stored[i]->size());        // stored (maybe compressed)
            toc[i].usize     = uint32_t(entries_[i].blob.size());  // decompressed
            cursor += uint32_t(stored[i]->size());
        }
        const uint32_t total = cursor;

        phx::BundleHeader h{};
        h.magic = phx::kBundleMagic; h.version = phx::kBundleVersion;
        h.target = target_; h.flags = 0;
        h.asset_count = count; h.toc_offset = toc_off; h.total_size = total; h.reserved = 0;

        FILE* fp = std::fopen(path.c_str(), "wb");
        if (!fp) return false;
        std::fwrite(&h, sizeof(h), 1, fp);
        std::fwrite(toc.data(), sizeof(phx::TocEntry), count, fp);
        // blobs at their padded offsets
        uint32_t pos = blob_off;
        for (uint32_t i = 0; i < count; ++i) {
            while (pos < toc[i].offset) { std::fputc(0, fp); ++pos; }   // 16-byte alignment pad
            std::fwrite(stored[i]->data(), 1, stored[i]->size(), fp);
            pos += uint32_t(stored[i]->size());
        }
        std::fclose(fp);
        return true;
    }

    uint32_t count() const { return uint32_t(entries_.size()); }

private:
    struct Entry { phx::NameHash hash; phx::AssetType type; std::vector<uint8_t> blob; };

    static uint32_t align16(uint32_t v) { return (v + 15u) & ~15u; }

    void push(const std::string& name, phx::AssetType type, std::vector<uint8_t>&& blob) {
        entries_.push_back(Entry{ phx::fnv1a(name.c_str()), type, std::move(blob) });
    }

    uint8_t target_;
    bool    compress_ = false;
    std::vector<Entry> entries_;
};

} // namespace phxtool
#endif // PHX_TOOLS_BUNDLE_WRITER_H
