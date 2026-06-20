// engine/render/src/renderer.cpp — the backend-agnostic render front end. Records sprite
// draws into a fixed per-frame list (sized to caps.max_sprites — the honest ceiling),
// sorts by (layer, z, tex), and dispatches to the one linked backend. Tilemaps are drawn
// immediately as background; sprites are drawn on top after sorting.
#include "phx/render/renderer.h"
#include "backend.h"

#include <cstdlib>   // qsort — C, portable, no STL/heap-per-frame

namespace phx {

namespace {
// Sort key: layer (high) -> z -> texture, so batches of the same texture stay adjacent.
inline uint32_t sort_key(const DrawSprite& s) {
    return (uint32_t(s.layer) << 24) | (uint32_t(s.z) << 16) | uint32_t(s.tex);
}
int cmp_sprite(const void* a, const void* b) {
    uint32_t ka = sort_key(*static_cast<const DrawSprite*>(a));
    uint32_t kb = sort_key(*static_cast<const DrawSprite*>(b));
    return (ka > kb) - (ka < kb);
}
} // namespace

Result<Renderer*> Renderer::create(phx_gfx* gfx, ArenaAllocator& arena, const Caps& caps) {
    Renderer* r = arena.make<Renderer>();
    if (!r) return Result<Renderer*>::fail(Status::OutOfMemory);

    r->arena_ = &arena;
    r->be_    = phx_make_render_backend(gfx, arena, caps);
    if (!r->be_) return Result<Renderer*>::fail(Status::Unsupported);

    r->cap_     = caps.max_sprites ? caps.max_sprites : 256;
    r->sprites_ = arena.alloc_array<DrawSprite>(r->cap_);
    if (!r->sprites_) return Result<Renderer*>::fail(Status::OutOfMemory);

    return Result<Renderer*>::good(r);
}

void Renderer::begin_frame(const Camera2D& cam) {
    cam_   = cam;
    count_ = 0;
    stats_ = RenderStats{};
    be_->stats() = RenderStats{};
    be_->begin(cam);
}

TextureId Renderer::load_texture(const TextureDesc& d) {
    TextureId id = be_->upload_tex(d);
    if (id != kNoTexture) ++live_tex_;
    return id;
}
void Renderer::unload_texture(TextureId id) {
    if (id == kNoTexture) return;
    be_->free_tex(id);
    if (live_tex_) --live_tex_;
}
TilemapId Renderer::upload_tilemap(const TilemapDesc& d){ return be_->upload_map(d); }
void      Renderer::set_tilemap_scroll(TilemapId id, vec2 px) { be_->set_scroll(id, px); }
void      Renderer::draw_tilemap(TilemapId id, uint8_t layer) { be_->draw_tilemap(id, layer); }

void Renderer::draw_sprite(const DrawSprite& s) {
    if (count_ >= cap_) { ++stats_.sprites_dropped; return; }   // honest ceiling, graceful
    sprites_[count_++] = s;
}

void Renderer::end_frame() {
    if (count_ > 1)
        std::qsort(sprites_, count_, sizeof(DrawSprite), cmp_sprite);

    be_->submit_sprites(sprites_, count_);
    be_->end();

    // merge front-end + backend stats for the public view
    RenderStats bs = be_->stats();
    bs.sprites_submitted = count_;
    bs.sprites_dropped  += stats_.sprites_dropped;
    stats_ = bs;
}

} // namespace phx
