// phx/render/src/texture_cache.cpp — LRU texture cache. Linear scan over a small fixed
// table (max_entries is tens, not thousands), so no hashing/heap: simple and predictable,
// in the engine's spirit. LRU victim = the resident entry with the smallest use-clock.
#include "phx/render/texture_cache.h"

namespace phx {

namespace {
// Bytes an uploaded texture costs, by encoding: 4 bytes/texel for the RGBA8 layouts,
// ~0.5 for the tier-0 4bpp bake (palettes/tile tables are noise next to the texel data).
uint32_t tex_bytes(const TextureDesc& d) {
    const uint32_t texels = uint32_t(d.width) * uint32_t(d.height);
    return d.format == PixelFormat::PAL4_TILES ? texels / 2u : texels * 4u;
}
} // namespace

Result<TextureCache*> TextureCache::create(ArenaAllocator& a, Renderer& r,
                                           size_t budget_bytes, uint16_t max_entries) {
    if (max_entries == 0) return Result<TextureCache*>::fail(Status::BadArg);
    TextureCache* c = a.make<TextureCache>();
    if (!c) return Result<TextureCache*>::fail(Status::OutOfMemory);
    c->r_   = &r;
    c->cap_ = max_entries;
    c->budget_ = budget_bytes;
    c->e_ = a.alloc_array<Entry>(max_entries);
    if (!c->e_) return Result<TextureCache*>::fail(Status::OutOfMemory);
    for (uint16_t i = 0; i < max_entries; ++i) c->e_[i] = Entry{};
    return Result<TextureCache*>::good(c);
}

void TextureCache::evict_index(uint16_t i) {
    if (!e_[i].used) return;
    r_->unload_texture(e_[i].id);
    stats_.resident_bytes -= e_[i].bytes;
    e_[i].used = false;
    ++stats_.evictions;
    --stats_.live_entries;
}

bool TextureCache::make_room(uint32_t need_bytes) {
    // Evict the global LRU entry until a slot is free AND the new asset's bytes fit the budget.
    for (;;) {
        bool slot_free = false;
        for (uint16_t i = 0; i < cap_; ++i) if (!e_[i].used) { slot_free = true; break; }
        const bool fits = (budget_ == 0) || (stats_.resident_bytes + need_bytes <= budget_);
        if (slot_free && fits) return true;

        int victim = -1; uint64_t best = ~uint64_t(0);
        for (uint16_t i = 0; i < cap_; ++i)
            if (e_[i].used && e_[i].tick < best) { best = e_[i].tick; victim = i; }
        if (victim < 0) return slot_free;     // nothing left to evict (set was already empty)
        evict_index(uint16_t(victim));
    }
}

TextureId TextureCache::get_or_upload(uint32_t key, const TextureDesc& desc) {
    for (uint16_t i = 0; i < cap_; ++i) {
        if (e_[i].used && e_[i].key == key) {
            ++stats_.hits;
            e_[i].tick = ++clock_;            // touch -> most recently used
            return e_[i].id;
        }
    }

    ++stats_.misses;
    const uint32_t bytes = tex_bytes(desc);
    if (budget_ != 0 && bytes > budget_) { ++stats_.oversized; return kNoTexture; }

    if (!make_room(bytes)) return kNoTexture;          // no slot could be freed
    int slot = -1;
    for (uint16_t i = 0; i < cap_; ++i) if (!e_[i].used) { slot = i; break; }
    if (slot < 0) return kNoTexture;                   // defensive (make_room guarantees one)

    TextureId id = r_->load_texture(desc);
    if (id == kNoTexture) return kNoTexture;            // renderer out of slots

    Entry& e = e_[slot];
    e.key = key; e.id = id; e.bytes = bytes; e.tick = ++clock_; e.used = true;
    stats_.resident_bytes += bytes;
    ++stats_.uploads;
    ++stats_.live_entries;
    return id;
}

void TextureCache::evict_unused() {
    // Anything not requested since the previous evict_unused() boundary is dropped.
    for (uint16_t i = 0; i < cap_; ++i)
        if (e_[i].used && e_[i].tick <= frame_mark_) evict_index(i);
    frame_mark_ = clock_;
}

void TextureCache::evict_all() {
    for (uint16_t i = 0; i < cap_; ++i) if (e_[i].used) evict_index(i);
    frame_mark_ = clock_;
}

} // namespace phx
