// tests/texcache_test.cpp — the budget-bounded LRU texture cache (docs/06 §4). Drives a real
// Renderer (software backend) through TextureCache: verifies cache hits avoid re-upload, the
// byte budget evicts least-recently-used textures, evicted slots are RECYCLED by the renderer
// (no slot leak across many cycles), oversize assets are rejected, and evict_unused/evict_all
// reclaim correctly. The renderer's live_textures() count is the independent oracle that the
// cache and backend actually freed GPU resources rather than just dropping bookkeeping.
#include "phx/platform/platform.h"
#include "phx/render/renderer.h"
#include "phx/render/texture_cache.h"
#include "phx/core/caps.h"

#include <cstdio>

using namespace phx;

namespace {
int g_checks = 0, g_fail = 0;
void check(bool ok, const char* what) { ++g_checks; if (!ok) { ++g_fail; std::printf("    FAIL %s\n", what); } }

// A WxH RGBA8 texture description over a persistent pixel buffer (soft backend is zero-copy).
TextureDesc make_desc(uint16_t w, uint16_t h) {
    uint32_t* px = new uint32_t[size_t(w) * h];           // leaked intentionally (test lifetime)
    for (size_t i = 0; i < size_t(w) * h; ++i) px[i] = 0xFF808080u;
    TextureDesc d{}; d.pixels = px; d.width = w; d.height = h; d.format = PixelFormat::RGBA8;
    return d;
}
} // namespace

int main() {
    const phx_platform* plat = phx_platform_get();
    phx_platform_desc desc{}; desc.title = "texcache_test"; desc.width = 64; desc.height = 64;
    if (plat->init(&desc) != 0) { std::printf("platform init failed\n"); return 1; }

    static uint8_t arena_buf[8 << 20];
    ArenaAllocator arena; arena.init(arena_buf, sizeof(arena_buf));

    auto rr = Renderer::create(plat->gfx(), arena, caps());
    check(rr.ok(), "Renderer::create");
    Renderer* r = rr.unwrap();

    // 8x8 RGBA8 = 256 bytes each. Budget 560 holds two, so a third forces one eviction.
    auto tcr = TextureCache::create(arena, *r, /*budget*/560, /*max_entries*/16);
    check(tcr.ok(), "TextureCache::create");
    TextureCache* tc = tcr.unwrap();

    TextureDesc d8 = make_desc(8, 8);

    // --- cache hit avoids a second upload ---
    TextureId a = tc->get_or_upload(1, d8);
    TextureId a2 = tc->get_or_upload(1, d8);
    check(a != kNoTexture && a == a2, "same key returns same id");
    check(tc->stats().hits == 1 && tc->stats().uploads == 1, "second get is a hit, not an upload");
    check(r->live_textures() == 1, "one texture live in the renderer");

    // --- second key fits within budget (2*256 <= 560) ---
    TextureId b = tc->get_or_upload(2, d8);
    check(b != kNoTexture && b != a, "second key uploads a distinct texture");
    check(tc->stats().live_entries == 2 && r->live_textures() == 2, "two live");
    check(tc->stats().resident_bytes == 512, "resident bytes = 2*256");

    // --- third key exceeds budget -> LRU (key 1, least recently used) is evicted ---
    TextureId c = tc->get_or_upload(3, d8);
    check(c != kNoTexture, "third key uploads");
    check(tc->stats().evictions == 1, "one eviction to make room");
    check(tc->stats().live_entries == 2 && r->live_textures() == 2, "still two live after evict");
    check(tc->stats().resident_bytes == 512, "resident bytes stay within budget");

    // key 1 was the LRU and should now be a miss again (re-upload, evicting the new LRU = key 2)
    uint32_t up_before = tc->stats().uploads;
    TextureId a3 = tc->get_or_upload(1, d8);
    check(a3 != kNoTexture, "re-request evicted key re-uploads");
    check(tc->stats().uploads == up_before + 1, "evicted key caused a fresh upload");

    // --- slot recycling: many evicting cycles must keep succeeding (256-slot backend would
    //     otherwise exhaust). Proves freed renderer slots are reused, not leaked. ---
    bool all_ok = true;
    for (uint32_t k = 100; k < 1100; ++k) {
        TextureId id = tc->get_or_upload(k, d8);
        all_ok &= (id != kNoTexture);
    }
    check(all_ok, "1000 churning uploads all succeed (slots recycled)");
    check(r->live_textures() <= 2, "renderer never holds more than the budget allows");
    check(tc->stats().resident_bytes <= 560, "resident bytes never exceed the budget");

    // --- oversize asset (16x16 = 1024 bytes > 560 budget) is rejected and counted ---
    TextureDesc d16 = make_desc(16, 16);
    uint32_t over_before = tc->stats().oversized;
    TextureId big = tc->get_or_upload(9999, d16);
    check(big == kNoTexture, "asset larger than the budget is rejected");
    check(tc->stats().oversized == over_before + 1, "oversize is surfaced in stats");

    // --- evict_unused: touch one key, then a boundary drops the untouched one ---
    tc->evict_all();
    check(r->live_textures() == 0, "evict_all frees every texture");
    TextureId x = tc->get_or_upload(10, d8);
    TextureId y = tc->get_or_upload(11, d8);
    check(x != kNoTexture && y != kNoTexture, "two fresh uploads");
    tc->evict_unused();                       // boundary: both were used since last boundary -> survive
    check(r->live_textures() == 2, "both survive their first frame");
    tc->get_or_upload(10, d8);                // touch only key 10
    tc->evict_unused();                       // key 11 untouched since boundary -> evicted
    check(r->live_textures() == 1, "untouched texture evicted at the frame boundary");

    // unbounded cache (budget 0) never evicts for budget
    auto ubr = TextureCache::create(arena, *r, /*budget*/0, /*max_entries*/8);
    check(ubr.ok(), "unbounded cache create");
    TextureCache* ub = ubr.unwrap();
    for (uint32_t k = 0; k < 8; ++k) ub->get_or_upload(k, d8);
    check(ub->stats().evictions == 0 && ub->stats().live_entries == 8, "budget 0 never evicts for budget");

    plat->shutdown();
    std::printf("\ntexcache_test: %d checks, %d failures\n", g_checks, g_fail);
    std::printf(g_fail == 0 ? "TEXCACHE PASS\n\n" : "TEXCACHE FAIL\n\n");
    return g_fail == 0 ? 0 : 1;
}
