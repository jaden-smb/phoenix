// examples/emberwing/bake.h — HOST-ONLY bundle assembly. Paints the ASCII pixel art (art.h),
// synthesizes the SFX/music (audio_gen.h), generates the level's Tiled `.tmj` (level.h), and
// feeds it all through the SAME BundleWriter/importers the phxpack CLI uses — per-target
// encoded (`tier` 0 = GBA: sounds resampled to the 18157 Hz Direct Sound rate at bake time).
// NOT compiled into the game binary (STL allowed).
#ifndef EMBERWING_BAKE_H
#define EMBERWING_BAKE_H

#include "art.h"
#include "audio_gen.h"
#include "level.h"

#include "bundle_writer.h"   // tools/phxpack
#include "tiled.h"           // the real Tiled importer
#include "debug_font.h"      // tools/common — shared 5x7 font atlas

#include <cstdint>
#include <string>
#include <vector>

namespace game {

inline bool bake_emberwing_assets(const char* path, uint8_t tier = 2) {
    using namespace art;
    phxtool::BundleWriter w{ tier };

    auto tex = [&](const char* name, const std::vector<uint32_t>& px, int tw, int th) {
        w.add_texture(name, px.data(), uint16_t(tw), uint16_t(th));
    };
    auto clip = [](const char* name, uint16_t first, uint16_t count, uint8_t fps,
                   uint8_t loop) {
        return phx::SpriteClipDef{ phx::fnv1a(name), first, count, fps, loop, 0 };
    };

    // -- world + fx textures --
    tex("tiles", make_tileset(), kTileAtlasW, kTileAtlasH);
    tex("spark", make_spark(), 8, 8);

    // -- animated sprites (texture + frame grid + clip tables; clip ORDER = the game's
    //    *Clip enums in game.h) --
    tex("hero", make_hero(), kHeroCols * kHeroW, ((kHeroFrames + 3) / 4) * kHeroH);
    w.add_sprite("hero", "hero", kHeroW, kHeroH, kHeroCols, {
        clip("idle", 0, 2, 3, 1), clip("run", 4, 4, 10, 1),
        clip("jump", 2, 1, 1, 1), clip("fall", 3, 1, 1, 1), clip("hurt", 8, 1, 1, 1),
    });

    tex("cinder", make_cinderling(), kCinderFrames * kEnemyW, kEnemyH);
    w.add_sprite("cinder", "cinder", kEnemyW, kEnemyH, kCinderFrames,
                 { clip("walk", 0, 2, 6, 1) });

    tex("thorn", make_thornback(), kThornFrames * kEnemyW, kEnemyH);
    w.add_sprite("thorn", "thorn", kEnemyW, kEnemyH, kThornFrames,
                 { clip("walk", 0, 2, 5, 1) });

    tex("wisp", make_ashwisp(), kWispFrames * kEnemyW, kEnemyH);
    w.add_sprite("wisp", "wisp", kEnemyW, kEnemyH, kWispFrames,
                 { clip("float", 0, 2, 4, 1) });

    tex("geyser", make_geyser(), kGeyserFrames * kGeyserW, kGeyserH);
    w.add_sprite("geyser", "geyser", kGeyserW, kGeyserH, kGeyserFrames, {
        clip("warn", 0, 2, 8, 1), clip("erupt", 2, 3, 12, 1),
    });

    tex("ember", make_ember_pickup(), kEmberFrames * 8, 8);
    w.add_sprite("ember", "ember", 8, 8, kEmberFrames, { clip("spin", 0, 4, 8, 1) });

    tex("shard", make_shard(), kShardFrames * 8, 8);
    w.add_sprite("shard", "shard", 8, 8, kShardFrames, { clip("sparkle", 0, 4, 6, 1) });

    tex("heart", make_heart(), kHeartFrames * 8, 8);
    w.add_sprite("heart", "heart", 8, 8, kHeartFrames, { clip("pulse", 0, 2, 3, 1) });

    tex("waystone", make_waystone(), kStoneFrames * kStoneW, kStoneH);
    w.add_sprite("waystone", "waystone", kStoneW, kStoneH, kStoneFrames, {
        clip("dormant", 0, 1, 1, 1), clip("lit", 1, 2, 4, 1),
    });

    tex("gate", make_gate(), kGateFrames * kGateW, kGateH);
    w.add_sprite("gate", "gate", kGateW, kGateH, kGateFrames,
                 { clip("shimmer", 0, 2, 3, 1) });

    // -- font: the shared host-side debug atlas (same glyphs the GUI editors use) --
    {
        std::vector<uint32_t> font(size_t(phxtool::kDebugFontW) * phxtool::kDebugFontH);
        phxtool::build_debug_font(font.data());
        tex("font", font, phxtool::kDebugFontW, phxtool::kDebugFontH);
    }

    // -- the title credit, pre-rendered at 2× ("big text"). Baking the STRING as one strip
    //    instead of shipping a whole 2× font atlas keeps the PPU char-store cost at 28 tiles
    //    (a full 2× atlas would be 256 of the 512-tile budget). The gold is baked into the
    //    texels because plain OBJs cannot tint. Drawn by TitleScene in 32×16 chunks — the
    //    widest hardware OBJ shape that tiles a 224×16 strip. --
    {
        static const char* kCredit = "BY JADEN HAIWYRE";
        constexpr int kAdv = 12, kCw = 224, kCh = 16;              // strip = 7 OBJ-able 32px chunks
        // The glyphs span (16-1) advances + one 2x-scaled 5px-wide glyph = 190px, NOT the full
        // 224px strip. Center the text inside the strip, or the strip-centering draw in
        // TitleScene shows the text ~17px left of the other (text-centered) title lines.
        constexpr int kTextW = 15 * kAdv + 2 * 5;
        constexpr uint32_t kGold = 255u | (200u << 8) | (90u << 16) | (255u << 24);
        std::vector<uint32_t> atlas(size_t(phxtool::kDebugFontW) * phxtool::kDebugFontH);
        phxtool::build_debug_font(atlas.data());
        std::vector<uint32_t> img(size_t(kCw) * kCh, 0u);
        int x0 = (kCw - kTextW) / 2;
        for (const char* p = kCredit; *p; ++p, x0 += kAdv) {
            if (*p == ' ') continue;
            const int cell = *p - 32;                              // atlas cell (ASCII 32 first)
            const int sx = (cell % 16) * 8, sy = (cell / 16) * 8;
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x) {
                    if (!(atlas[size_t(sy + y) * phxtool::kDebugFontW + (sx + x)] >> 24))
                        continue;                                  // transparent atlas texel
                    for (int dy = 0; dy < 2; ++dy)
                        for (int dx = 0; dx < 2; ++dx) {
                            const int px = x0 + 2 * x + dx, py = 2 * y + dy + 1;   // +1: center 14px glyphs in the 16px strip
                            if (px < kCw && py < kCh) img[size_t(py) * kCw + px] = kGold;
                        }
                }
        }
        tex("credit", img, kCw, kCh);
    }

    // -- the level: generate the Tiled .tmj and run the REAL importer --
    {
        phxtool::TiledMap tm;
        if (!phxtool::tiled_load(level::build_tmj(), tm)) return false;
        std::vector<uint16_t> flat;
        for (auto& L : tm.layers) flat.insert(flat.end(), L.begin(), L.end());
        w.add_tilemap("level", flat.data(), uint16_t(tm.width), uint16_t(tm.height),
                      uint8_t(tm.layers.size()), uint8_t(tm.tile_w), uint8_t(tm.tile_h),
                      tm.tileset, tm.has_parallax() ? &tm.layer_parallax : nullptr);
        std::vector<phx::SpawnDef> spawns;
        for (auto& s : tm.spawns)
            spawns.push_back(phx::SpawnDef{ phx::fnv1a(s.type.c_str()),
                                            int16_t(s.x), int16_t(s.y),
                                            uint16_t(s.w), uint16_t(s.h) });
        w.add_spawns("level", spawns);
    }

    // -- audio: synthesized SFX + the looping level theme --
    auto snd = [&](const char* name, const audio_gen::Buf& b) {
        w.add_sound(name, b.data(), uint32_t(b.size()), audio_gen::kRate);
    };
    snd("jump",       audio_gen::sfx_jump());
    snd("ember_sfx",  audio_gen::sfx_ember());
    snd("stomp",      audio_gen::sfx_stomp());
    snd("hurt",       audio_gen::sfx_hurt());
    snd("checkpoint", audio_gen::sfx_checkpoint());
    snd("shard_sfx",  audio_gen::sfx_shard());
    snd("heart_sfx",  audio_gen::sfx_heart());
    snd("goal",       audio_gen::sfx_goal());
    snd("geyser_sfx", audio_gen::sfx_geyser());
    snd("music",      audio_gen::music_theme());

    return w.write(path);
}

} // namespace game
#endif // EMBERWING_BAKE_H
