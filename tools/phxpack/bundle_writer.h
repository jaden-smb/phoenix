// tools/phxpack/bundle_writer.h — host-side `.phxp` assembler. STL is fine here (tools
// never ship to console). Accumulates assets, then writes header + sorted TOC + blobs.
// Shares the format definition (phx/resource/bundle.h) with the runtime reader.
#ifndef PHX_TOOLS_BUNDLE_WRITER_H
#define PHX_TOOLS_BUNDLE_WRITER_H

#include "phx/resource/bundle.h"
#include "phx/core/types.h"
#include "phx/core/pixel.h"
#include "phx/core/crc32.h"
#include "lz_encode.h"
#include "tex_encode.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace phxtool {

class BundleWriter {
public:
    // target_tier: 0=GBA PPU, 1=PSP GU, 2=PC GL/soft (phx/resource/bundle.h). Anything else is
    // an authoring mistake (a made-up tier the runtime's target guard could never match) —
    // fail loudly at construction rather than silently baking an unloadable bundle.
    explicit BundleWriter(uint8_t target_tier = 2) : target_(target_tier) {
        if (target_tier > 2) {
            std::fprintf(stderr, "BundleWriter: invalid target tier %u (want 0=GBA/1=PSP/2=PC)\n",
                         unsigned(target_tier));
            std::abort();
        }
    }

    // Enable LZSS compression of blobs. Each asset is compressed independently at write()
    // time and stored compressed ONLY if that actually shrinks it (else stored verbatim), so
    // turning this on never makes a bundle bigger and the reader handles a mix transparently.
    void set_compression(bool on) { compress_ = on; }

    // PER-TARGET ENCODE (docs/06 §4, docs/08 §2), the texture counterpart of add_sound's
    // tier-0 resample: tier 0 bakes the PPU's native PAL4_TILES (4bpp tiles + palettes,
    // ~8× less texel data in the ROM, upload = claim banks + remap nibbles instead of a
    // full quantize), tier 1 bakes the GU's swizzled RGBA8 (pure reorder, sampled zero-copy
    // with the swizzle bit). Sources a tier can't express stay RGBA8 — the runtime then
    // behaves exactly as it did for v1 bundles (upload-time quantize on GBA, linear on PSP).
    void add_texture(const std::string& name, const uint32_t* rgba,
                     uint16_t w, uint16_t h) {
        phx::TextureBlobHeader th{};
        th.width = w; th.height = h; th.format = uint8_t(phx::PixelFormat::RGBA8);

        std::vector<uint8_t> payload;
        if (target_ == 0 && pal4_encode(rgba, w, h, payload)) {
            th.format = uint8_t(phx::PixelFormat::PAL4_TILES);
            std::printf("  ~ texture %-12s tier-0 encode: RGBA8 -> 4bpp tiles (%u -> %u bytes)\n",
                        name.c_str(), unsigned(uint32_t(w) * h * 4), unsigned(payload.size()));
        } else if (target_ == 1 && swz_encode(rgba, w, h, payload)) {
            th.format = uint8_t(phx::PixelFormat::RGBA8_SWZ);
            std::printf("  ~ texture %-12s tier-1 encode: RGBA8 swizzled (GU block order)\n",
                        name.c_str());
        } else {
            if (target_ != 2)
                std::printf("  ~ texture %-12s kept RGBA8 (source not tier-%u encodable)\n",
                            name.c_str(), unsigned(target_));
            payload.resize(size_t(w) * h * 4);
            std::memcpy(payload.data(), rgba, payload.size());
        }

        std::vector<uint8_t> blob(sizeof(th) + payload.size());
        std::memcpy(blob.data(), &th, sizeof(th));
        std::memcpy(blob.data() + sizeof(th), payload.data(), payload.size());
        push(name, phx::AssetType::Texture, std::move(blob));
    }

    // `parallax`: optional per-layer camera factors (one {fx,fy} pair per layer, 1.0 = moves
    // with the world), e.g. from Tiled's parallaxx/parallaxy. The Q16 table is appended — and
    // the header flag set — only when some factor differs from 1, so bundles without parallax
    // stay byte-identical to the old format.
    void add_tilemap(const std::string& name, const uint16_t* indices,
                     uint16_t w, uint16_t h, uint8_t layers,
                     uint8_t tile_w, uint8_t tile_h, const std::string& tileset,
                     const std::vector<std::pair<double,double>>* parallax = nullptr) {
        phx::TilemapBlobHeader mh{};
        mh.width = w; mh.height = h; mh.layers = layers;
        mh.tile_w = tile_w; mh.tile_h = tile_h;
        mh.tileset = phx::fnv1a(tileset.c_str());
        bool want_par = false;
        if (parallax)
            for (auto& p : *parallax) if (p.first != 1.0 || p.second != 1.0) want_par = true;
        if (want_par) mh.flags |= phx::kTilemapHasParallax;
        const size_t n = size_t(w) * h * layers;
        const size_t par_off = (sizeof(mh) + n * sizeof(uint16_t) + 3) & ~size_t(3);  // 4-align
        std::vector<uint8_t> blob(want_par ? par_off + size_t(layers) * 2 * sizeof(int32_t)
                                           : sizeof(mh) + n * sizeof(uint16_t));
        std::memcpy(blob.data(), &mh, sizeof(mh));
        std::memcpy(blob.data() + sizeof(mh), indices, n * sizeof(uint16_t));
        if (want_par) {
            int32_t* pq = reinterpret_cast<int32_t*>(blob.data() + par_off);
            for (uint8_t l = 0; l < layers; ++l) {
                const auto pr = l < parallax->size() ? (*parallax)[l] : std::make_pair(1.0, 1.0);
                pq[l * 2 + 0] = int32_t(pr.first  * 65536.0 + (pr.first  < 0 ? -0.5 : 0.5));
                pq[l * 2 + 1] = int32_t(pr.second * 65536.0 + (pr.second < 0 ? -0.5 : 0.5));
            }
        }
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

    // Mono 16-bit PCM sound at `rate` Hz (e.g. decoded from a WAV). PER-TARGET ENCODE
    // (docs/06): on tier 0 (GBA) the PCM is resampled down to the GBA device rate at BAKE
    // time — the ROM carries ~2.4× less sample data (for 44.1 kHz sources) and the 16 MHz
    // CPU mixes 1:1 instead of resampling every voice. Q16 linear interpolation, all-integer
    // (deterministic).
    // Tiers 1/2 keep the source rate: their mixers run at 44.1 kHz and resample cheaply.
    // == the GBA Direct Sound device rate. 18157 Hz is the vblank-locked rate (924 CPU
    // cycles/sample; exactly 304 samples per 280896-cycle video frame) — the GBA backend's
    // per-frame double-buffer pump needs integer samples/frame (16384 Hz gave 274.3125,
    // i.e. a discontinuity every frame). Must match kGbaAudioRate in the GBA game entries.
    static constexpr uint32_t kTier0Rate = 18157;
    void add_sound(const std::string& name, const int16_t* samples, uint32_t frames, uint32_t rate) {
        std::vector<int16_t> enc;
        if (target_ == 0 && rate > kTier0Rate && frames) {
            const uint64_t step = (uint64_t(rate) << 16) / kTier0Rate;   // src frames per out, Q16
            const uint32_t out_n = uint32_t((uint64_t(frames) << 16) / step);
            enc.resize(out_n);
            uint64_t pos = 0;
            for (uint32_t i = 0; i < out_n; ++i, pos += step) {
                const uint32_t i0 = uint32_t(pos >> 16);
                const uint32_t i1 = i0 + 1 < frames ? i0 + 1 : i0;
                const int32_t  f  = int32_t(pos & 0xFFFF);
                enc[i] = int16_t((int32_t(samples[i0]) * (65536 - f) + int32_t(samples[i1]) * f) >> 16);
            }
            std::printf("  ~ sound   %-12s tier-0 encode: %u Hz -> %u Hz (%u -> %u frames)\n",
                        name.c_str(), rate, kTier0Rate, frames, out_n);
            samples = enc.data(); frames = out_n; rate = kTier0Rate;
        }
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

    // Add an already-baked asset with its name hash intact (used to MERGE a pre-baked bundle —
    // e.g. converter output — whose TOC stores hashes, not the original asset names).
    void add_raw(phx::NameHash hash, phx::AssetType type, std::vector<uint8_t> blob) {
        entries_.push_back(Entry{ hash, type, std::move(blob), std::string(), source_ });
    }

    // Author-source attribution for the assets added AFTER this call (the phxpack assembler
    // sets it per input). Feeds the manifest sidecar and the lock file's per-input asset map.
    void set_source(const std::string& src) { source_ = src; }

    // Introspection for the manifest/lock writers (tools/phxpack/lock.h). Note: write()
    // sorts entries by name hash, so index/order differs before and after write().
    uint32_t         entry_count() const        { return uint32_t(entries_.size()); }
    phx::NameHash    entry_hash(uint32_t i) const   { return entries_[i].hash; }
    phx::AssetType   entry_type(uint32_t i) const   { return entries_[i].type; }
    const std::string& entry_name(uint32_t i) const { return entries_[i].name; }    // "" if merged raw
    const std::string& entry_source(uint32_t i) const { return entries_[i].source; }

    // CRC32 of the COMPLETE file written by the last successful write() (header included).
    // The lock file records it so a later invocation can prove the output on disk is exactly
    // the bundle this bake produced (not stale, not hand-edited).
    uint32_t file_crc32() const { return file_crc32_; }

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

        // Assemble the whole file in memory first (TOC + blobs; zero-filled alignment padding
        // comes for free from vector's default-init) so the blob-region CRC32 can be computed
        // before the header is written — a bundle is small enough on any target's dev machine
        // that this costs nothing, and it means the header is ALWAYS written with the real
        // checksum of the bytes that follow it, never patched after the fact on disk.
        std::vector<uint8_t> buf(total - toc_off, 0);
        std::memcpy(buf.data(), toc.data(), count * sizeof(phx::TocEntry));
        for (uint32_t i = 0; i < count; ++i)
            std::memcpy(buf.data() + (toc[i].offset - toc_off), stored[i]->data(), stored[i]->size());

        phx::BundleHeader h{};
        h.magic = phx::kBundleMagic; h.version = phx::kBundleVersion;
        h.target = target_; h.flags = 0;
        h.asset_count = count; h.toc_offset = toc_off; h.total_size = total;
        h.blob_crc32 = phx::crc32_of(buf.data(), buf.size());   // covers TOC + every blob

        FILE* fp = std::fopen(path.c_str(), "wb");
        if (!fp) return false;
        std::fwrite(&h, sizeof(h), 1, fp);
        std::fwrite(buf.data(), 1, buf.size(), fp);
        std::fclose(fp);
        file_crc32_ = phx::crc32_finish(phx::crc32(buf.data(), buf.size(),
                                                   phx::crc32(&h, sizeof(h))));
        return true;
    }

    uint32_t count() const { return uint32_t(entries_.size()); }

private:
    // name/source are host-side metadata only (manifest + lock); they never reach the bundle.
    struct Entry {
        phx::NameHash hash; phx::AssetType type; std::vector<uint8_t> blob;
        std::string name, source;
    };

    static uint32_t align16(uint32_t v) { return (v + 15u) & ~15u; }

    void push(const std::string& name, phx::AssetType type, std::vector<uint8_t>&& blob) {
        entries_.push_back(Entry{ phx::fnv1a(name.c_str()), type, std::move(blob), name, source_ });
    }

    uint8_t target_;
    bool    compress_ = false;
    std::string source_;
    uint32_t file_crc32_ = 0;
    std::vector<Entry> entries_;
};

} // namespace phxtool
#endif // PHX_TOOLS_BUNDLE_WRITER_H
