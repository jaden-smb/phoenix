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

// A Tiled `.tmj`: 16x12 map (bottom two rows solid) + an object group with the player and
// three coins as point objects. Authored as JSON so the bake runs the real Tiled importer.
inline std::string build_level_tmj() {
    std::string j = "{ \"width\":16, \"height\":12, \"tilewidth\":8, \"tileheight\":8,";
    j += "\"tilesets\":[{\"firstgid\":1,\"name\":\"tiles\"}],";
    j += "\"layers\":[{\"type\":\"tilelayer\",\"name\":\"ground\",\"width\":16,\"height\":12,\"data\":[";
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
    j += "{\"name\":\"c2\",\"type\":\"coin\",\"point\":true,\"x\":88,\"y\":76}";
    j += "]}]}";
    return j;
}

inline bool bake_platformer_assets(const char* path) {
    using namespace phx;
    phxtool::BundleWriter w{2 /*PC tier*/};

    // tileset: one 8x8 solid ground tile (tilemap index 1 -> atlas cell 0)
    uint32_t tiles[8 * 8];
    for (auto& p : tiles) p = rgba(150, 90, 50);
    w.add_texture("tiles", tiles, 8, 8);

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

    // font: a real 5x7 bitmap font in a 128x32 atlas (16 cols x 4 rows of 8x8 cells), so the
    // glyph for ASCII c sits at cell (c-32) and the engine's font config (first_char=32, cols=16)
    // samples it directly. Covers ASCII 32..95 (space, digits, punctuation, A-Z) — every char the
    // example draws; undefined cells stay transparent. Each glyph row is 5 bits, MSB = leftmost.
    struct GlyphDef { char c; uint8_t r[7]; };
    static const GlyphDef kGlyphs[] = {
        {'0',{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}}, {'1',{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
        {'2',{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}}, {'3',{0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}},
        {'4',{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}}, {'5',{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
        {'6',{0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}}, {'7',{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
        {'8',{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}}, {'9',{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},
        {':',{0x00,0x04,0x00,0x00,0x00,0x04,0x00}}, {'!',{0x04,0x04,0x04,0x04,0x04,0x00,0x04}},
        {'.',{0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}}, {'-',{0x00,0x00,0x00,0x1F,0x00,0x00,0x00}},
        {'A',{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}}, {'B',{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}},
        {'C',{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}}, {'D',{0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}},
        {'E',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}}, {'F',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
        {'G',{0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}}, {'H',{0x11,0x11,0x11,0x1F,0x11,0x11,0x11}},
        {'I',{0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}}, {'J',{0x07,0x02,0x02,0x02,0x12,0x12,0x0C}},
        {'K',{0x11,0x12,0x14,0x18,0x14,0x12,0x11}}, {'L',{0x10,0x10,0x10,0x10,0x10,0x10,0x1F}},
        {'M',{0x11,0x1B,0x15,0x15,0x11,0x11,0x11}}, {'N',{0x11,0x11,0x19,0x15,0x13,0x11,0x11}},
        {'O',{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}}, {'P',{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}},
        {'Q',{0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}}, {'R',{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
        {'S',{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}}, {'T',{0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
        {'U',{0x11,0x11,0x11,0x11,0x11,0x11,0x0E}}, {'V',{0x11,0x11,0x11,0x11,0x11,0x0A,0x04}},
        {'W',{0x11,0x11,0x11,0x15,0x15,0x1B,0x11}}, {'X',{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}},
        {'Y',{0x11,0x11,0x0A,0x04,0x04,0x04,0x04}}, {'Z',{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}},
    };
    uint32_t font[128 * 32];
    for (auto& p : font) p = 0u;                              // transparent everywhere
    for (const auto& g : kGlyphs) {
        const int cell = int(g.c) - 32;                      // cell 0 == ASCII 32 (space)
        const int cx = (cell % 16) * 8, cy = (cell / 16) * 8;
        for (int ry = 0; ry < 7; ++ry)
            for (int rx = 0; rx < 5; ++rx)
                if (g.r[ry] & (1 << (4 - rx)))               // MSB of the 5-bit row = leftmost
                    font[(cy + ry + 1) * 128 + (cx + rx + 1)] = 0xFFFFFFFFu;  // 1px top/left pad
    }
    w.add_texture("font", font, 128, 32);

    // level + spawns: author a Tiled map and run the real importer (tile layer -> Tilemap,
    // object group -> Spawns table).
    phxtool::TiledMap tm;
    if (!phxtool::tiled_load(build_level_tmj(), tm)) return false;
    std::vector<uint16_t> flat;
    for (auto& L : tm.layers) flat.insert(flat.end(), L.begin(), L.end());
    w.add_tilemap("level", flat.data(), uint16_t(tm.width), uint16_t(tm.height),
                  uint8_t(tm.layers.size()), uint8_t(tm.tile_w), uint8_t(tm.tile_h), tm.tileset);
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
