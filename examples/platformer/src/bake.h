// examples/platformer/bake.h — HOST-ONLY asset baking for the example. In a real project this
// is an offline `phxpack` step on authored PNG/Tiled/WAV files; here we synthesize the source
// bytes inline and run them through the SAME importers the CLI uses, so the example exercises
// the real pipeline end to end: a Tiled `.tmj` -> Tilemap + Spawns, a Sprite sheet -> Sprite
// (anim clips), and WAVs -> Sound assets. Produces the .phxp the game mounts. NOT compiled into
// the game binary (uses the host BundleWriter / importers / STL).
#ifndef PLATFORMER_BAKE_H
#define PLATFORMER_BAKE_H

#include "phx/core/pixel.h"
#include "bundle_writer.h"   // tools/phxpack
#include "tiled.h"           // JSON + Tiled importer
#include "wav.h"             // RIFF/WAV decoder
#include "debug_font.h"      // tools/common — shared 5x7 font atlas builder

#include <cstdint>
#include <string>
#include <vector>

namespace game {

// A tiny mono-16 WAV (RIFF) of a square wave — distinct, short, audibly non-silent SFX.
inline std::vector<uint8_t> make_sfx_wav(uint32_t rate, uint32_t frames, int16_t amp, uint32_t period) {
    auto put16 = [](std::vector<uint8_t>& v, uint16_t x){ v.push_back(uint8_t(x)); v.push_back(uint8_t(x>>8)); };
    auto put32 = [](std::vector<uint8_t>& v, uint32_t x){ for (int i=0;i<4;++i) v.push_back(uint8_t(x>>(8*i))); };
    auto tag   = [](std::vector<uint8_t>& v, const char* t){ for (int i=0;i<4;++i) v.push_back(uint8_t(t[i])); };
    std::vector<uint8_t> w; const uint32_t dataSize = frames * 2;
    tag(w,"RIFF"); put32(w,36+dataSize); tag(w,"WAVE");
    tag(w,"fmt "); put32(w,16); put16(w,1); put16(w,1); put32(w,rate); put32(w,rate*2); put16(w,2); put16(w,16);
    tag(w,"data"); put32(w,dataSize);
    for (uint32_t i=0;i<frames;++i) put16(w, uint16_t(((i / period) & 1u) ? amp : int16_t(-amp)));
    return w;
}

inline bool add_wav_sound(phxtool::BundleWriter& w, const char* name, const std::vector<uint8_t>& wav) {
    std::vector<int16_t> mono; uint32_t rate = 0;
    if (!phxtool::wav_decode(wav.data(), wav.size(), mono, rate)) return false;
    w.add_sound(name, mono.data(), uint32_t(mono.size()), rate);
    return true;
}

// A Tiled `.tmj`: 16x12 map — a decorative parallax backdrop layer (clouds at half camera
// speed, Tiled's native `parallaxx`) UNDER the solid ground layer — + an object group with
// the player, coins, an enemy, and a spike. Authored as JSON so the bake runs the real Tiled
// importer. Convention: the LAST tile layer is the gameplay/solid one (physics reads it).
inline std::string build_level_tmj() {
    std::string j = "{ \"width\":16, \"height\":12, \"tilewidth\":8, \"tileheight\":8,";
    j += "\"tilesets\":[{\"firstgid\":1,\"name\":\"tiles\"}],";
    j += "\"layers\":[";
    j += "{\"type\":\"tilelayer\",\"name\":\"backdrop\",\"width\":16,\"height\":12,";
    j += "\"parallaxx\":0.5,\"data\":[";
    for (int y = 0; y < 12; ++y)
        for (int x = 0; x < 16; ++x) {
            if (!(x == 0 && y == 0)) j += ",";
            j += (y >= 3 && y <= 4 && (x % 5) == 2) ? "2" : "0";   // sparse clouds (tile 2)
        }
    j += "]},";
    j += "{\"type\":\"tilelayer\",\"name\":\"ground\",\"width\":16,\"height\":12,\"data\":[";
    for (int y = 0; y < 12; ++y)
        for (int x = 0; x < 16; ++x) {
            if (!(x == 0 && y == 0)) j += ",";
            j += (y >= 10) ? "1" : "0";        // bottom two rows are the ground (tile 1)
        }
    j += "]},";
    j += "{\"type\":\"objectgroup\",\"name\":\"entities\",\"objects\":[";
    j += "{\"name\":\"player\",\"type\":\"player\",\"point\":true,\"x\":16,\"y\":70},";
    j += "{\"name\":\"c0\",\"type\":\"coin\",\"point\":true,\"x\":40,\"y\":76},";
    j += "{\"name\":\"c1\",\"type\":\"coin\",\"point\":true,\"x\":64,\"y\":76},";
    j += "{\"name\":\"c2\",\"type\":\"coin\",\"point\":true,\"x\":88,\"y\":76},";
    j += "{\"name\":\"e0\",\"type\":\"enemy\",\"point\":true,\"x\":104,\"y\":74},";  // patroller
    j += "{\"name\":\"s0\",\"type\":\"spike\",\"point\":true,\"x\":120,\"y\":76}";   // hazard
    j += "]}]}";
    return j;
}

inline bool bake_platformer_assets(const char* path, uint8_t tier = 2) {
    using namespace phx;
    phxtool::BundleWriter w{tier};   // tier drives per-target encoding (e.g. sound rates)

    // tileset: two 8x8 tiles side by side — cell 0 the solid ground (tilemap index 1),
    // cell 1 a pale cloud puff for the parallax backdrop (tilemap index 2).
    uint32_t tiles[16 * 8];
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 16; ++x) {
            if (x < 8) { tiles[y * 16 + x] = rgba(150, 90, 50); continue; }   // ground
            const int cx = x - 8;
            const bool puff = (y >= 2 && y <= 5 && cx >= 1 && cx <= 6);       // rounded blob
            tiles[y * 16 + x] = puff ? rgba(200, 205, 220) : 0u;              // rest transparent
        }
    w.add_texture("tiles", tiles, 16, 8);

    // hero: 32x8 four-frame walk sheet, each frame a distinct blue so motion is visible
    uint32_t hero[32 * 8];
    const Rgba fr[4] = { rgba(80,160,240), rgba(110,180,250), rgba(70,150,230), rgba(120,190,255) };
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 32; ++x) hero[y * 32 + x] = fr[x / 8];
    w.add_texture("hero", hero, 32, 8);

    // hero Sprite asset: frame grid over the "hero" texture + named clips (idle, walk).
    // Order matters: the game's AnimState enum uses kIdle=0, kWalk=1.
    std::vector<phx::SpriteClipDef> clips = {
        { phx::fnv1a("idle"), /*first*/0, /*count*/1, /*fps*/1,  /*loop*/1, 0 },
        { phx::fnv1a("walk"), /*first*/0, /*count*/4, /*fps*/10, /*loop*/1, 0 },
    };
    w.add_sprite("hero", "hero", /*frame_w*/8, /*frame_h*/8, /*cols*/4, clips);

    // coin: 8x8 yellow
    uint32_t coin[8 * 8];
    for (auto& p : coin) p = rgba(245, 220, 60);
    w.add_texture("coin", coin, 8, 8);

    // enemy: 8x8 red patroller
    uint32_t enemy[8 * 8];
    for (auto& p : enemy) p = rgba(210, 60, 60);
    w.add_texture("enemy", enemy, 8, 8);

    // spike: 8x8 grey hazard (a simple up-triangle so it reads as spikes; rest transparent)
    uint32_t spike[8 * 8];
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            const int half = (x < 4) ? x : (7 - x);     // 0..3 rising to the centre
            spike[y * 8 + x] = (y >= 7 - half) ? rgba(190, 190, 205) : 0u;
        }
    w.add_texture("spike", spike, 8, 8);

    // font: the shared host-side 5x7 debug font atlas (tools/common/debug_font.h) — the same
    // glyphs the GUI editors draw with, so cell (c-32) matches the engine's font defaults.
    uint32_t font[phxtool::kDebugFontW * phxtool::kDebugFontH];
    phxtool::build_debug_font(font);
    w.add_texture("font", font, phxtool::kDebugFontW, phxtool::kDebugFontH);

    // level + spawns: author a Tiled map and run the real importer (tile layer -> Tilemap,
    // object group -> Spawns table).
    phxtool::TiledMap tm;
    if (!phxtool::tiled_load(build_level_tmj(), tm)) return false;
    std::vector<uint16_t> flat;
    for (auto& L : tm.layers) flat.insert(flat.end(), L.begin(), L.end());
    w.add_tilemap("level", flat.data(), uint16_t(tm.width), uint16_t(tm.height),
                  uint8_t(tm.layers.size()), uint8_t(tm.tile_w), uint8_t(tm.tile_h), tm.tileset,
                  tm.has_parallax() ? &tm.layer_parallax : nullptr);
    std::vector<phx::SpawnDef> spawns;
    for (auto& s : tm.spawns)
        spawns.push_back(phx::SpawnDef{ phx::fnv1a(s.type.c_str()), int16_t(s.x), int16_t(s.y),
                                        uint16_t(s.w), uint16_t(s.h) });
    w.add_spawns("level", spawns);

    // SFX: WAVs decoded by the pipeline and baked as Sound assets.
    if (!add_wav_sound(w, "jump", make_sfx_wav(44100, 1200, 7000, 40))) return false;
    if (!add_wav_sound(w, "coin", make_sfx_wav(44100,  800, 9000, 20))) return false;

    return w.write(path);
}

} // namespace game
#endif // PLATFORMER_BAKE_H
