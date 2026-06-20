// tests/sprite_test.cpp — the phxsprite path end to end: decode a sheet PNG, bake a Texture +
// a Sprite asset (frame grid + named clips) into a `.phxp`, mount it, read the SpriteView, and
// BUILD AN ANIMATOR FROM IT — then drive the anim module to a chosen frame and verify that
// frame's pixels reach the framebuffer. Proves authored sprite art flows decode → bake → mount
// → anim → render, closing the loop the engine's animation system was designed around.
#include "phx/platform/platform.h"
#include "phx/platform/gfx_soft.h"
#include "phx/resource/cache.h"
#include "phx/render/renderer.h"
#include "phx/anim/anim.h"
#include "phx/core/caps.h"
#include "bundle_writer.h"
#include "png.h"
#include "png_fixtures.h"

#include <cstdio>
#include <vector>

using namespace phx;

namespace {
int g_checks = 0, g_fail = 0;
void check(bool ok, const char* what) { ++g_checks; if (!ok) { ++g_fail; std::printf("    FAIL %s\n", what); } }

int find_clip(const SpriteView& sv, NameHash name) {
    for (uint16_t i = 0; i < sv.clip_count; ++i) if (sv.clips[i].name == name) return int(i);
    return -1;
}
} // namespace

int main() {
    // Decode the sheet and bake Texture("hero_sheet") + Sprite("hero") referencing it, with two
    // clips: walk (frames 0..3 @ 8fps, loop) and idle (frame 0, static).
    // Also drop the sheet + a .sprdef to disk so the phxpack CLI can bake the .sprdef path.
    if (FILE* pf = std::fopen("build/hero_sheet.png", "wb")) { std::fwrite(kSheet8x2, 1, sizeof(kSheet8x2), pf); std::fclose(pf); }
    if (FILE* df = std::fopen("build/hero.sprdef", "wb")) {
        std::fputs("# hero sprite\nsheet hero_sheet.png 2 2\nclip walk 0 4 8 1\nclip idle 0 1 1 0\n", df);
        std::fclose(df);
    }

    std::vector<uint32_t> px; uint16_t w = 0, h = 0;
    check(phxtool::png_decode(kSheet8x2, sizeof(kSheet8x2), px, w, h), "decode sheet PNG");
    check(w == 8 && h == 2, "sheet dimensions");

    const char* bundle = "build/test_sprite.phxp";
    {
        phxtool::BundleWriter bw(/*tier*/2);
        bw.add_texture("hero_sheet", px.data(), w, h);
        std::vector<phx::SpriteClipDef> clips = {
            { phx::fnv1a("walk"), 0, 4, 8, 1, 0 },
            { phx::fnv1a("idle"), 0, 1, 1, 0, 0 },
        };
        bw.add_sprite("hero", "hero_sheet", /*fw*/2, /*fh*/2, /*cols*/4, clips);
        check(bw.write(bundle), "bake texture + sprite");
    }

    const phx_platform* plat = phx_platform_get();
    phx_platform_desc desc{}; desc.title = "sprite_test"; desc.width = 8; desc.height = 8;
    if (plat->init(&desc) != 0) { std::printf("platform init failed\n"); return 1; }

    static uint8_t arena_buf[4 << 20];
    ArenaAllocator arena; arena.init(arena_buf, sizeof(arena_buf));

    ResourceCache* cache = ResourceCache::create(arena).unwrap();
    check(cache->mount(plat, bundle) == Status::Ok, "mount bundle");

    auto svr = cache->sprite("hero"_hash);
    check(svr.ok(), "sprite('hero') found");
    SpriteView sv = svr.unwrap();
    check(sv.frame_w == 2 && sv.frame_h == 2 && sv.cols == 4, "sprite sheet geometry");
    check(sv.clip_count == 2, "two clips");
    check(sv.texture == "hero_sheet"_hash, "sprite references its sheet texture");
    const int walk = find_clip(sv, "walk"_hash);
    const int idle = find_clip(sv, "idle"_hash);
    check(walk >= 0 && idle >= 0, "named clips resolve");
    check(walk >= 0 && sv.clips[walk].count == 4 && sv.clips[walk].fps == 8, "walk clip data");
    check(idle >= 0 && sv.clips[idle].loop == 0, "idle clip is non-looping");

    // Build an anim::Animator straight from the SpriteView (the game's job).
    std::vector<AnimClip> aclips(sv.clip_count);
    for (uint16_t i = 0; i < sv.clip_count; ++i)
        aclips[i] = AnimClip{ sv.clips[i].first, sv.clips[i].count, sv.clips[i].fps, sv.clips[i].loop != 0 };
    Animator an;
    an.sheet = SpriteSheet{ sv.frame_w, sv.frame_h, sv.cols };
    an.clips = Span<const AnimClip>{ aclips.data(), aclips.size() };

    // Frame 2 of 'walk' -> sheet frame 2 -> source rect at (4,0), size 2x2 (the BLUE frame).
    an.play(uint16_t(walk));
    an.frame = 2;
    AnimationSystem::apply_rect(an);
    check(an.cur_sx == 4 && an.cur_sy == 0 && an.cur_sw == 2 && an.cur_sh == 2, "frame 2 source rect");

    // Render that frame from the baked sheet and confirm the colour on screen.
    auto rr = Renderer::create(plat->gfx(), arena, caps());
    Renderer* r = rr.unwrap();
    TextureView t = cache->texture(sv.texture).unwrap();
    TextureDesc td{}; td.pixels = t.pixels; td.width = t.width; td.height = t.height; td.format = t.format;
    TextureId tex = r->load_texture(td);

    r->begin_frame(Camera2D{});
    DrawSprite s{}; s.tex = tex;
    s.sx = an.cur_sx; s.sy = an.cur_sy; s.sw = an.cur_sw; s.sh = an.cur_sh; s.pos = vec2{};
    r->draw_sprite(s);
    r->end_frame();

    phx_soft_fb fb = phx_gfx_soft_lock(plat->gfx());
    check(fb.pixels[0] == 0xFFFF0000u, "walk frame 2 renders BLUE from the baked sheet");

    plat->shutdown();
    std::printf("\nsprite_test: %d checks, %d failures\n", g_checks, g_fail);
    std::printf(g_fail == 0 ? "SPRITE PASS\n\n" : "SPRITE FAIL\n\n");
    return g_fail == 0 ? 0 : 1;
}
