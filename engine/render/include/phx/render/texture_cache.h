// phx/render/texture_cache.h — a budget-bounded, LRU texture cache over the Renderer's
// upload/unload (docs/06 §4). Decoded/uploaded textures are the one resource class that
// costs real device memory, so they live here under a byte budget; on PC/PSP this bounds
// VRAM churn for streamed/transient textures, while on GBA everything fits and nothing
// evicts. It is keyed by an OPAQUE uint32 (the caller uses an asset NameHash) and is handed
// the pixels only on a miss — so this stays in the render layer with NO resource dependency
// (the asset↔texture glue is the caller's job, which legitimately sees both). See the
// dependency law in docs/00 §3: resource and render are siblings and must not cross-depend.
#ifndef PHX_RENDER_TEXTURE_CACHE_H
#define PHX_RENDER_TEXTURE_CACHE_H

#include "phx/render/renderer.h"

namespace phx {

struct TextureCacheStats {
    uint32_t hits      = 0;     // get_or_upload found the key resident
    uint32_t misses    = 0;     // key absent (led to an upload or an oversize reject)
    uint32_t uploads   = 0;     // textures uploaded to the renderer
    uint32_t evictions = 0;     // textures unloaded to reclaim budget / slots
    uint32_t oversized = 0;     // assets that alone exceed the whole budget (a budgeting bug)
    size_t   resident_bytes = 0;
    uint16_t live_entries   = 0;
};

class TextureCache {
public:
    // budget_bytes == 0 => unbounded (caches without ever evicting for budget; still bounded
    // by max_entries). max_entries caps the resident set and the backing table.
    static Result<TextureCache*> create(ArenaAllocator&, Renderer&,
                                        size_t budget_bytes, uint16_t max_entries = 64);

    // Cached TextureId for `key`; on a miss, upload `desc` (counting its bytes against the
    // budget, evicting least-recently-used entries until it fits) and cache it. Returns
    // kNoTexture if the asset alone is larger than the whole budget (surfaced in stats), or
    // if the underlying renderer is out of texture slots.
    TextureId get_or_upload(uint32_t key, const TextureDesc& desc);

    void evict_unused();          // evict entries not requested since the previous call
    void evict_all();             // free every resident texture (e.g. on scene teardown)

    const TextureCacheStats& stats() const { return stats_; }

private:
    TextureCache() = default;
    friend class ArenaAllocator;

    struct Entry {
        uint32_t  key   = 0;
        TextureId id    = kNoTexture;
        uint32_t  bytes = 0;
        uint64_t  tick  = 0;        // last-use clock for LRU ordering
        bool      used  = false;
    };

    void evict_index(uint16_t i);
    bool make_room(uint32_t need_bytes);   // evict until a slot is free and bytes fit

    Renderer* r_   = nullptr;
    Entry*    e_   = nullptr;
    uint16_t  cap_ = 0;
    size_t    budget_     = 0;       // 0 = unbounded
    uint64_t  clock_      = 0;       // monotonic use counter
    uint64_t  frame_mark_ = 0;       // evict_unused() boundary
    TextureCacheStats stats_{};
};

} // namespace phx
#endif // PHX_RENDER_TEXTURE_CACHE_H
