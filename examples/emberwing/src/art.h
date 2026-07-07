// examples/emberwing/art.h — HOST-ONLY original pixel art, authored as ASCII grids (one char
// per pixel, '.' = transparent) with a small palette table per texture, so the art is
// reviewable and editable in source form. bake.h paints these into RGBA8 buffers and feeds
// them through the same BundleWriter path the CLI converters use. Never compiled into the
// game binary (STL allowed).
#ifndef EMBERWING_ART_H
#define EMBERWING_ART_H

#include "phx/core/pixel.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace game {
namespace art {

using phx::Rgba;
using phx::rgba;

struct Pal { char ch; Rgba c; };

inline Rgba pal_lookup(const Pal* pal, size_t n, char ch) {
    if (ch == '.') return 0u;
    for (size_t i = 0; i < n; ++i)
        if (pal[i].ch == ch) return pal[i].c;
    return rgba(255, 0, 255);   // loud magenta = a typo in a grid
}

// Paint a w×h ASCII grid into `tex` (texw wide) at (x0, y0). Rows shorter than `w`
// (an authoring slip) read as transparent rather than out of bounds.
inline void blit(std::vector<uint32_t>& tex, int texw, int x0, int y0,
                 const char* const* rows, int w, int h, const Pal* pal, size_t npal) {
    for (int y = 0; y < h; ++y) {
        const int len = int(std::strlen(rows[y]));
        for (int x = 0; x < w; ++x)
            tex[(y0 + y) * texw + (x0 + x)] =
                x < len ? pal_lookup(pal, npal, rows[y][x]) : 0u;
    }
}

// ==== the shared world palette ==============================================================
// Cinder Hollow: dusk purples for stone and sky, ember oranges for everything alive or hot.
namespace col {
inline const Rgba kOutline   = rgba(24, 16, 24);
// basalt / stone
inline const Rgba kBasalt    = rgba(58, 42, 52);
inline const Rgba kBasaltDk  = rgba(36, 26, 34);
inline const Rgba kBasaltLt  = rgba(78, 58, 70);
inline const Rgba kStoneHi   = rgba(106, 82, 96);
// ember moss (the "grass" of the hollow)
inline const Rgba kMossHi    = rgba(232, 144, 60);
inline const Rgba kMoss      = rgba(196, 106, 40);
inline const Rgba kMossDk    = rgba(138, 74, 32);
// lava
inline const Rgba kLavaHi    = rgba(255, 200, 56);
inline const Rgba kLava      = rgba(255, 120, 32);
inline const Rgba kLavaMd    = rgba(224, 74, 16);
inline const Rgba kLavaDk    = rgba(160, 40, 8);
inline const Rgba kLavaDeep  = rgba(112, 28, 8);
// dusk sky bands
inline const Rgba kSkyDeep   = rgba(24, 15, 44);
inline const Rgba kSkyMid    = rgba(36, 22, 64);
inline const Rgba kSkyLow    = rgba(50, 32, 82);
inline const Rgba kSkyGlow   = rgba(74, 44, 94);
inline const Rgba kSkyEmber  = rgba(106, 58, 94);
inline const Rgba kStar      = rgba(216, 200, 244);
inline const Rgba kStarDim   = rgba(140, 124, 190);
// crag silhouettes (far parallax)
inline const Rgba kCrag      = rgba(42, 28, 56);
inline const Rgba kCragRim   = rgba(74, 52, 84);
inline const Rgba kCragGlow  = rgba(255, 112, 32);
// ruins (deco layer)
inline const Rgba kRuin      = rgba(85, 64, 92);
inline const Rgba kRuinHi    = rgba(112, 88, 118);
inline const Rgba kRuinDk    = rgba(60, 44, 68);
// ember-wood planks
inline const Rgba kWood      = rgba(200, 120, 64);
inline const Rgba kWoodDk    = rgba(160, 88, 40);
inline const Rgba kWoodEdge  = rgba(64, 40, 24);
} // namespace col

// ==== tileset ================================================================================
// 8×8 tiles in an 8-wide atlas. The enum is the atlas cell index; tilemap values are +1
// (0 = empty). level.h maps level characters onto these.
enum Tile : uint16_t {
    kTGroundTop = 0,   // ember-moss lip over basalt
    kTGroundFill,
    kTGroundFleck,     // fill variant with an ember fleck
    kTBlock,           // free-standing basalt block (stairs, ledges)
    kTColumnCap,
    kTColumnShaft,
    kTPlatform,        // thin ember-wood platform
    kTVent,            // geyser mouth (deco)
    kTLavaTop,
    kTLavaFill,
    kTSkyDeep,
    kTSkyMid,
    kTSkyLow,
    kTSkyGlow,
    kTSkyEmber,        // horizon band
    kTStars,
    kTHaze,            // drifting ash streaks (sky deco)
    kTCragTop,
    kTCragFill,
    kTCragGlow,        // fill with a glowing crack
    kTRuinShaft,       // weathered ruin column (deco)
    kTRuinCap,
    kTileCount
};
constexpr int kTileAtlasCols = 8;
constexpr int kTileAtlasRows = 3;   // 24 cells >= kTileCount
constexpr int kTileAtlasW = kTileAtlasCols * 8;
constexpr int kTileAtlasH = kTileAtlasRows * 8;

inline std::vector<uint32_t> make_tileset() {
    using namespace col;
    std::vector<uint32_t> tex(size_t(kTileAtlasW) * kTileAtlasH, 0u);

    const Pal pal[] = {
        { 'M', kMossHi },  { 'm', kMoss },     { 'd', kMossDk },
        { 'B', kBasalt },  { 'b', kBasaltDk }, { 'L', kBasaltLt }, { 'H', kStoneHi },
        { 'K', kOutline },
        { 'O', kLavaHi },  { 'l', kLava },     { 'v', kLavaMd },   { 'D', kLavaDk },
        { 'e', kLavaDeep },
        { 'S', kSkyDeep }, { 's', kSkyMid },   { 'z', kSkyLow },   { 'g', kSkyGlow },
        { 'E', kSkyEmber },{ '*', kStar },     { '+', kStarDim },
        { 'C', kCrag },    { 'R', kCragRim },  { 'G', kCragGlow },
        { 'U', kRuin },    { 'u', kRuinHi },   { 'x', kRuinDk },
        { 'W', kWood },    { 'w', kWoodDk },   { 'k', kWoodEdge },
    };
    const size_t np = sizeof pal / sizeof pal[0];

    auto put = [&](Tile t, const char* const* rows) {
        blit(tex, kTileAtlasW, (t % kTileAtlasCols) * 8, (t / kTileAtlasCols) * 8,
             rows, 8, 8, pal, np);
    };

    { const char* r[8] = { "mMmmMmmM",
                           "dmdmmdmd",
                           "BdBBdBBd",
                           "BbBBbBBB",
                           "bBbbBbbB",
                           "BBbBBbBB",
                           "bBBbbBBb",
                           "BbbBBbbB" }; put(kTGroundTop, r); }
    { const char* r[8] = { "BbBBbBBb",
                           "bBbbBbbB",
                           "BBbBBbBB",
                           "bBbbBLbb",
                           "BbBBbBBb",
                           "bLbBBbbB",
                           "BBbbBbBb",
                           "bBBbBbbB" }; put(kTGroundFill, r); }
    { const char* r[8] = { "BbBBbBBb",
                           "bBbbBbbB",
                           "BBbBvBBB",
                           "bBbvlvbb",
                           "BbBBvBBb",
                           "bBbBBbbB",
                           "BBbbBbBb",
                           "bBBbBbbB" }; put(kTGroundFleck, r); }
    { const char* r[8] = { "KKKKKKKK",
                           "KHLLBLBK",
                           "KLBBBBbK",
                           "KBBLBBbK",
                           "KBBBBLbK",
                           "KLBBBBbK",
                           "KbbbbbbK",
                           "KKKKKKKK" }; put(kTBlock, r); }
    { const char* r[8] = { "KKKKKKKK",
                           "KHHLHHbK",
                           "KLBBBBbK",
                           "KKKKKKKK",
                           ".KBLBbK.",
                           ".KBLBbK.",
                           ".KBbBbK.",
                           ".KBLBbK." }; put(kTColumnCap, r); }
    { const char* r[8] = { ".KBLBbK.",
                           ".KBLBbK.",
                           ".KBbBbK.",
                           ".KBLBbK.",
                           ".KBLBbK.",
                           ".KBbBbK.",
                           ".KBLBbK.",
                           ".KBLBbK." }; put(kTColumnShaft, r); }
    { const char* r[8] = { "kkkkkkkk",
                           "kWwWWwWk",
                           "kwwwwwwk",
                           "kkkkkkkk",
                           "..k..k..",
                           "..k..k..",
                           "........",
                           "........" }; put(kTPlatform, r); }
    { const char* r[8] = { "........",
                           "........",
                           "........",
                           "...vv...",
                           "..vllv..",
                           ".KvlLvK.",
                           "KBvvvvBK",
                           "KKKKKKKK" }; put(kTVent, r); }
    { const char* r[8] = { "OllOOllO",
                           "lvllvllv",
                           "vlvvvlvv",
                           "lvDvlvDv",
                           "vDvDvDvD",
                           "DvDDvDDv",
                           "vDDvvDDv",
                           "DDvDDvDD" }; put(kTLavaTop, r); }
    { const char* r[8] = { "vDDvvDDv",
                           "DvDDvDDv",
                           "DDvDDvDD",
                           "eDDeeDDe",
                           "DeDDeDDe",
                           "eDeeeDee",
                           "DeeDDeeD",
                           "eeeDeeee" }; put(kTLavaFill, r); }
    { const char* r[8] = { "SSSSSSSS","SSSSSSSS","SSSSSSSS","SSSSSSSS",
                           "SSSSSSSS","SSSSSSSS","SSSSSSSS","SSSSSSSS" }; put(kTSkyDeep, r); }
    { const char* r[8] = { "ssssssss","ssssssss","sSssssSs","ssssssss",
                           "ssssssss","sssSssss","ssssssss","ssssssss" }; put(kTSkyMid, r); }
    { const char* r[8] = { "zzzzzzzz","zzzzzzzz","zzszzzzz","zzzzzzzz",
                           "zzzzzszz","zzzzzzzz","zzzzzzzz","zzzzzzzz" }; put(kTSkyLow, r); }
    { const char* r[8] = { "gggggggg","gggzgggg","gggggggg","ggggggzg",
                           "gggggggg","gzgggggg","gggggggg","gggggggg" }; put(kTSkyGlow, r); }
    { const char* r[8] = { "EgEEgEEg","gEggEggE","EEgEEgEE","gEggEggE",
                           "EgEEgEEg","gEggEggE","EEgEEgEE","gEgEgEgE" }; put(kTSkyEmber, r); }
    { const char* r[8] = { "SSSSS*SS",
                           "SS+SSSSS",
                           "SSSSSSSS",
                           "S*SSSS+S",
                           "SSSSSSSS",
                           "SSS+SSSS",
                           "SSSSSS*S",
                           "SSSSSSSS" }; put(kTStars, r); }
    { const char* r[8] = { "ssssssss",
                           "sszzzsss",
                           "ssssszzs",
                           "ssssssss",
                           "zzssssss",
                           "sssszzzs",
                           "ssssssss",
                           "ssssssss" }; put(kTHaze, r); }
    { const char* r[8] = { ".......R",
                           "..R...RC",
                           ".RC..RCC",
                           "RCC.RCCC",
                           "CCCRCCCC",
                           "CCCCCCCC",
                           "CCCCCCCC",
                           "CCCCCCCC" }; put(kTCragTop, r); }
    { const char* r[8] = { "CCCCCCCC","CCCRCCCC","CCCCCCCC","CCCCCRCC",
                           "CRCCCCCC","CCCCCCCC","CCCCRCCC","CCCCCCCC" }; put(kTCragFill, r); }
    { const char* r[8] = { "CCCCCCCC",
                           "CCCGCCCC",
                           "CCGCCCCC",
                           "CCGCCCCC",
                           "CCCGCCCC",
                           "CCCGGCCC",
                           "CCCCGCCC",
                           "CCCCCCCC" }; put(kTCragGlow, r); }
    { const char* r[8] = { ".xUuUUx.",
                           ".xUuUUx.",
                           ".xUxUUx.",
                           ".xUuUUx.",
                           ".xUuUUx.",
                           ".xUxUUx.",
                           ".xUuUUx.",
                           ".xUuUUx." }; put(kTRuinShaft, r); }
    { const char* r[8] = { "xxxxxxxx",
                           "xuUuUuUx",
                           "xUUUUUUx",
                           "xxxxxxxx",
                           ".xUuUUx.",
                           ".xUuUUx.",
                           ".xUxUUx.",
                           ".xUuUUx." }; put(kTRuinCap, r); }

    return tex;
}

// ==== hero: Ember the phoenix hatchling ======================================================
// 16×16 frames in a 4-column sheet, drawn facing RIGHT (the renderer flips for left):
//   0,1 idle (bob + tuft flicker) · 2 jump · 3 fall · 4..7 run · 8 hurt
constexpr int kHeroFrames = 9;
constexpr int kHeroCols   = 4;
constexpr int kHeroW = 16, kHeroH = 16;

inline std::vector<uint32_t> make_hero() {
    const Pal pal[] = {
        { 'K', rgba(32, 16, 24) },     // outline
        { 'O', rgba(240, 128, 48) },   // body
        { 'D', rgba(192, 72, 24) },    // body shade / wing
        { 'Y', rgba(255, 200, 96) },   // belly
        { 'y', rgba(232, 160, 72) },   // belly shade
        { 'B', rgba(255, 224, 144) },  // beak
        { 'W', rgba(255, 248, 240) },  // eye shine
        { 'F', rgba(255, 208, 64) },   // tuft flame bright
        { 'R', rgba(255, 112, 32) },   // tuft flame deep
        { 'T', rgba(255, 152, 40) },   // tail flame
        { 'd', rgba(176, 64, 16) },    // legs
        { 'X', rgba(120, 24, 24) },    // hurt flush
    };
    const size_t np = sizeof pal / sizeof pal[0];

    const char* idle0[16] = {
        "................",
        ".......F........",
        "......FR........",
        ".....KKKK.......",
        "....KOOOOK......",
        "...KOOOKWKK.....",
        "...KOOOOOKKBB...",
        "...KODOOOOKKB...",
        "..KODOOOOYYK....",
        ".TKODDOOYYYK....",
        "TTKODDOYYYYK....",
        ".TKKDOOYYYK.....",
        "..KKOOYYYKK.....",
        "...KKKKKKK......",
        "....dK..Kd......",
        "....dd..dd......",
    };
    const char* idle1[16] = {
        "................",
        ".......R........",
        "......RF........",
        ".....KKKK.......",
        "....KOOOOK......",
        "...KOOOKWKK.....",
        "...KOOOOOKKBB...",
        "...KODOOOOKKB...",
        "..KODOOOOYYK....",
        ".TKODDOOYYYK....",
        "TTKODDOYYYYK....",
        ".TKKDOOYYYK.....",
        "..KKOOYYYKK.....",
        "...KKKKKKK......",
        "....Kd..dK......",
        "....dd..dd......",
    };
    const char* jump[16] = {
        ".......F........",
        "......FR........",
        ".....KKKK.......",
        "....KOOOOK......",
        "...KOOOKWKK.....",
        "...KOOOOOKKBB...",
        "..KKODOOOOKKB...",
        ".KDDODOOOYYK....",
        "KDDKODOOYYYK....",
        "KDKKODDYYYYK....",
        ".KKODDOYYYK.....",
        "T.KKOOYYYKK.....",
        "TT.KKKKKKK......",
        ".T...dKKd.......",
        ".....dd.dd......",
        "................",
    };
    const char* fall[16] = {
        "................",
        ".......R........",
        "......RF........",
        ".....KKKK.......",
        "..KKKOOOOK......",
        ".KDDKOOKWKK.....",
        "KDDKOOOOOKKBB...",
        "KDKKODOOOOKKB...",
        ".KKODOOOOYYK....",
        ".TKODDOOYYYK....",
        "TTKODDOYYYYK....",
        ".TKKDOOYYYK.....",
        "..KKOOYYYKK.....",
        "...KKKKKKK......",
        "...dK....Kd.....",
        "...dd....dd.....",
    };
    const char* run0[16] = {
        "................",
        ".......F........",
        "......FR........",
        ".....KKKK.......",
        "....KOOOOK......",
        "...KOOOKWKK.....",
        "...KOOOOOKKBB...",
        "...KODOOOOKKB...",
        "..KODOOOOYYK....",
        ".TKODDOOYYYK....",
        "TTKODDOYYYYK....",
        ".TKKDOOYYYK.....",
        "..KKOOYYYKK.....",
        "...KKKKKKK......",
        "..dK....Kd......",
        "..dd....dd......",
    };
    const char* run1[16] = {
        "................",
        ".......R........",
        "......RF........",
        ".....KKKK.......",
        "....KOOOOK......",
        "...KOOOKWKK.....",
        "...KOOOOOKKBB...",
        "...KODOOOOKKB...",
        "..KODOOOOYYK....",
        ".TKODDOOYYYK....",
        "TTKODDOYYYYK....",
        ".TKKDOOYYYK.....",
        "..KKOOYYYKK.....",
        "...KKKKKKK......",
        "....dKKd........",
        "....dddd........",
    };
    const char* run2[16] = {
        "................",
        ".......F........",
        "......FR........",
        ".....KKKK.......",
        "....KOOOOK......",
        "...KOOOKWKK.....",
        "...KOOOOOKKBB...",
        "...KODOOOOKKB...",
        "..KODOOOOYYK....",
        ".TKODDOOYYYK....",
        "TTKODDOYYYYK....",
        ".TKKDOOYYYK.....",
        "..KKOOYYYKK.....",
        "...KKKKKKK......",
        "....Kd....Kd....",
        "....dd....dd....",
    };
    const char* run3[16] = {
        "................",
        ".......R........",
        "......RF........",
        ".....KKKK.......",
        "....KOOOOK......",
        "...KOOOKWKK.....",
        "...KOOOOOKKBB...",
        "...KODOOOOKKB...",
        "..KODOOOOYYK....",
        ".TKODDOOYYYK....",
        "TTKODDOYYYYK....",
        ".TKKDOOYYYK.....",
        "..KKOOYYYKK.....",
        "...KKKKKKK......",
        ".....dKKd.......",
        ".....dd.dd......",
    };
    const char* hurt[16] = {
        "................",
        "......R.F.......",
        ".......R........",
        ".....KKKK.......",
        "....KOXOOK......",
        "...KOXOKKKK.....",
        "...KOOOOXKKBB...",
        "...KOXOOOOKKB...",
        "..KOXOOOOYyK....",
        ".TKOXXOOYyyK....",
        ".TKOXXOYyyyK....",
        ".TKKXOOYyyK.....",
        "..KKOOYyyKK.....",
        "...KKKKKKK......",
        "...dK....Kd.....",
        "...dd....dd.....",
    };

    const char* const* frames[kHeroFrames] =
        { idle0, idle1, jump, fall, run0, run1, run2, run3, hurt };

    const int rows = (kHeroFrames + kHeroCols - 1) / kHeroCols;
    std::vector<uint32_t> tex(size_t(kHeroCols * kHeroW) * (rows * kHeroH), 0u);
    for (int f = 0; f < kHeroFrames; ++f)
        blit(tex, kHeroCols * kHeroW, (f % kHeroCols) * kHeroW, (f / kHeroCols) * kHeroH,
             frames[f], kHeroW, kHeroH, pal, np);
    return tex;
}

// ==== cinderling: a stubby walking coal (2 walk frames, facing right) ========================
constexpr int kCinderFrames = 2;
constexpr int kEnemyW = 16, kEnemyH = 16;

inline std::vector<uint32_t> make_cinderling() {
    const Pal pal[] = {
        { 'K', rgba(20, 12, 18) },     // outline
        { 'B', rgba(52, 34, 44) },     // coal body
        { 'b', rgba(36, 24, 32) },     // coal shadow
        { 'e', rgba(255, 104, 32) },   // glowing cracks
        { 'E', rgba(255, 168, 64) },   // eyes
        { 'd', rgba(120, 52, 28) },    // feet
    };
    const size_t np = sizeof pal / sizeof pal[0];

    const char* walk0[16] = {
        "................",
        "................",
        "................",
        "....KKKKKK......",
        "...KBBBBBBK.....",
        "..KBBEKBBEK.....",
        "..KBBEKBBEK.....",
        ".KBBBBBBBBBK....",
        ".KBeBBBBeBBK....",
        ".KBBeBBBBeBK....",
        ".KBBBeBBBBBK....",
        ".KbBBBBBbBBK....",
        "..KbBBBBBbK.....",
        "...KKKKKKK......",
        "..Kdd..Kdd......",
        "..ddd..ddd......",
    };
    const char* walk1[16] = {
        "................",
        "................",
        "................",
        "....KKKKKK......",
        "...KBBBBBBK.....",
        "..KBBEKBBEK.....",
        "..KBBEKBBEK.....",
        ".KBBBBBBBBBK....",
        ".KBBeBBBeBBK....",
        ".KBeBBBBBeBK....",
        ".KBBBeBBBBBK....",
        ".KbBBBBBbBBK....",
        "..KbBBBBBbK.....",
        "...KKKKKKK......",
        "....KddKdd......",
        "....ddd.ddd.....",
    };

    const char* const* frames[kCinderFrames] = { walk0, walk1 };
    std::vector<uint32_t> tex(size_t(kCinderFrames * kEnemyW) * kEnemyH, 0u);
    for (int f = 0; f < kCinderFrames; ++f)
        blit(tex, kCinderFrames * kEnemyW, f * kEnemyW, 0, frames[f], kEnemyW, kEnemyH, pal, np);
    return tex;
}

// ==== thornback: an obsidian-spined creeper — the "don't stomp me" silhouette ================
constexpr int kThornFrames = 2;

inline std::vector<uint32_t> make_thornback() {
    const Pal pal[] = {
        { 'K', rgba(18, 12, 22) },     // outline
        { 'P', rgba(74, 48, 88) },     // body
        { 'p', rgba(51, 31, 64) },     // body shadow
        { 'S', rgba(200, 184, 224) },  // spikes
        { 's', rgba(148, 128, 180) },  // spike shade
        { 'R', rgba(255, 80, 64) },    // eye
        { 'd', rgba(60, 38, 72) },     // feet
    };
    const size_t np = sizeof pal / sizeof pal[0];

    const char* walk0[16] = {
        "................",
        "................",
        "....S....S......",
        "...KsK..KsK.....",
        "..S.KS..KS.S....",
        ".KsK.KsKKsKsK...",
        ".KPsKPPPPsPPK...",
        "KPPPPPPPPPPPPK..",
        "KPPPPPPPPPKRRK..",
        "KpPPPPPPPPPKKK..",
        "KPpPPPpPPPPPK...",
        "KpPPpPPPpPPpK...",
        ".KKKKKKKKKKK....",
        "..Kdd..Kdd......",
        "..ddd..ddd......",
        "................",
    };
    const char* walk1[16] = {
        "................",
        "................",
        "....S....S......",
        "...KsK..KsK.....",
        "..S.KS..KS.S....",
        ".KsK.KsKKsKsK...",
        ".KPsKPPPPsPPK...",
        "KPPPPPPPPPPPPK..",
        "KPPPPPPPPPKRRK..",
        "KpPPPPPPPPPKKK..",
        "KPpPPPpPPPPPK...",
        "KpPPpPPPpPPpK...",
        ".KKKKKKKKKKK....",
        "....KddKdd......",
        "....ddd.ddd.....",
        "................",
    };

    const char* const* frames[kThornFrames] = { walk0, walk1 };
    std::vector<uint32_t> tex(size_t(kThornFrames * kEnemyW) * kEnemyH, 0u);
    for (int f = 0; f < kThornFrames; ++f)
        blit(tex, kThornFrames * kEnemyW, f * kEnemyW, 0, frames[f], kEnemyW, kEnemyH, pal, np);
    return tex;
}

// ==== ashwisp: a floating ash puff with an ember core (2 float frames) =======================
constexpr int kWispFrames = 2;

inline std::vector<uint32_t> make_ashwisp() {
    const Pal pal[] = {
        { 'K', rgba(30, 24, 36) },     // outline
        { 'A', rgba(178, 168, 188) },  // ash
        { 'a', rgba(136, 126, 150) },  // ash shade
        { 'E', rgba(255, 144, 64) },   // ember core
        { 'e', rgba(255, 200, 96) },   // core hot spot
        { 'W', rgba(240, 236, 246) },  // highlight
    };
    const size_t np = sizeof pal / sizeof pal[0];

    const char* f0[16] = {
        "................",
        "....KKKK........",
        "..KKAAAAKK......",
        ".KAAWAAAAAK.....",
        ".KAWAAAAAAAK....",
        "KAAAAAEEAAAAK...",
        "KAAAAEeeEAAAK...",
        "KAAAAEeeEAAaK...",
        "KAAAAAEEAAaaK...",
        ".KAAAAAAAaaK....",
        ".KaAAAAAaaK.....",
        "..KKaAAaKK......",
        "...K.KK.K.......",
        "..a...a...a.....",
        "................",
        "................",
    };
    const char* f1[16] = {
        "................",
        "....KKKK........",
        "..KKAAAAKK......",
        ".KAAWAAAAAK.....",
        ".KAWAAAAAAAK....",
        "KAAAAAEEAAAAK...",
        "KAAAEeeeEAAAK...",
        "KAAAAEeEAAAaK...",
        "KAAAAAEAAAaaK...",
        ".KAAAAAAAaaK....",
        ".KaAAAAAaaK.....",
        "..KKaAAaKK......",
        "..K..KK..K......",
        "...a....a.......",
        ".....a..........",
        "................",
    };

    const char* const* frames[kWispFrames] = { f0, f1 };
    std::vector<uint32_t> tex(size_t(kWispFrames * kEnemyW) * kEnemyH, 0u);
    for (int f = 0; f < kWispFrames; ++f)
        blit(tex, kWispFrames * kEnemyW, f * kEnemyW, 0, frames[f], kEnemyW, kEnemyH, pal, np);
    return tex;
}

// ==== geyser: 16×32 frames — 0,1 warn puff (bottom), 2,3,4 erupting flame column =============
// The flame frames are painted procedurally (banded core/mid/edge with a phase-shifted wavy
// width) — flames read better generated than hand-gridded, and the wave phase gives free
// animation. Deterministic: pure integer tables.
constexpr int kGeyserFrames = 5;
constexpr int kGeyserW = 16, kGeyserH = 32;

inline std::vector<uint32_t> make_geyser() {
    using namespace col;
    std::vector<uint32_t> tex(size_t(kGeyserFrames * kGeyserW) * kGeyserH, 0u);

    const Pal pal[] = {
        { 'A', rgba(178, 168, 188) }, { 'a', rgba(136, 126, 150) },
        { 'E', rgba(255, 144, 64) },
    };
    const char* puff0[8] = {
        "................",
        "................",
        "......aa........",
        "....aAAAa.......",
        "...aAAEAAa......",
        "....aAAAa.......",
        "......aa........",
        "....a.....a.....",
    };
    const char* puff1[8] = {
        "................",
        ".....a..a.......",
        "...aAAaAAa......",
        "..aAAEAAEAa.....",
        "...aAAAAAa......",
        "..a..aa...a.....",
        "................",
        "................",
    };
    blit(tex, kGeyserFrames * kGeyserW, 0 * kGeyserW, kGeyserH - 8, puff0, 16, 8,
         pal, sizeof pal / sizeof pal[0]);
    blit(tex, kGeyserFrames * kGeyserW, 1 * kGeyserW, kGeyserH - 8, puff1, 16, 8,
         pal, sizeof pal / sizeof pal[0]);

    // erupt frames: a column whose half-width waves with height; three colour bands.
    static const int wave[8] = { 0, 1, 1, 0, -1, 0, 1, 0 };   // subtle edge wobble
    for (int f = 0; f < 3; ++f) {
        const int fx = (2 + f) * kGeyserW;
        for (int y = 0; y < kGeyserH; ++y) {
            // wider at the base, narrow at the tip, plus the animated wobble
            int half = 3 + (y * 3) / kGeyserH + wave[(y + f * 3) & 7];
            if (y < 3) half = 1 + y;                       // licking tip
            if (half > 6) half = 6;
            for (int x = 8 - half; x < 8 + half; ++x) {
                const int dx = x < 8 ? 8 - 1 - x : x - 8;  // distance from the core
                Rgba c = kLavaMd;
                if      (dx <= half - 3) c = kLavaHi;
                else if (dx <= half - 2) c = kLava;
                // sparse hot sparks riding the column
                if (((x * 7 + y * 13 + f * 5) % 29) == 0) c = rgba(255, 240, 168);
                tex[size_t(y) * (kGeyserFrames * kGeyserW) + fx + x] = c;
            }
        }
    }
    return tex;
}

// ==== collectibles ===========================================================================
constexpr int kEmberFrames = 4;   // 8×8 spinning ember mote
inline std::vector<uint32_t> make_ember_pickup() {
    const Pal pal[] = {
        { 'O', rgba(255, 208, 64) }, { 'o', rgba(255, 144, 40) },
        { 'W', rgba(255, 244, 176) }, { 'r', rgba(216, 84, 24) },
    };
    const size_t np = sizeof pal / sizeof pal[0];
    const char* f0[8] = { "...oo...",
                          "..oOOo..",
                          ".oOWOOo.",
                          ".oOOOOr.",
                          ".oOOOOr.",
                          ".oOOOrr.",
                          "..orrr..",
                          "...rr..." };
    const char* f1[8] = { "...o....",
                          "..oOo...",
                          ".oOWOo..",
                          ".oOOOr..",
                          ".oOOOr..",
                          "..oOrr..",
                          "..orr...",
                          "...r...." };
    const char* f2[8] = { "...o....",
                          "...Oo...",
                          "..oWO...",
                          "..oOO...",
                          "..oOr...",
                          "..oOr...",
                          "...rr...",
                          "...r...." };
    const char* f3[8] = { "....o...",
                          "..oOOo..",
                          ".oOWOo..",
                          ".oOOOor.",
                          ".oOOOr..",
                          "..oOrr..",
                          "..orr...",
                          "....r..." };
    const char* const frames[kEmberFrames][8] =
        { { f0[0],f0[1],f0[2],f0[3],f0[4],f0[5],f0[6],f0[7] },
          { f1[0],f1[1],f1[2],f1[3],f1[4],f1[5],f1[6],f1[7] },
          { f2[0],f2[1],f2[2],f2[3],f2[4],f2[5],f2[6],f2[7] },
          { f3[0],f3[1],f3[2],f3[3],f3[4],f3[5],f3[6],f3[7] } };
    std::vector<uint32_t> tex(size_t(kEmberFrames * 8) * 8, 0u);
    for (int f = 0; f < kEmberFrames; ++f)
        blit(tex, kEmberFrames * 8, f * 8, 0, frames[f], 8, 8, pal, np);
    return tex;
}

constexpr int kShardFrames = 5;   // 8×8 sunstone shard: 4 sparkle frames + a dim HUD pip
inline std::vector<uint32_t> make_shard() {
    const Pal pal[] = {
        { 'C', rgba(112, 232, 255) }, { 'c', rgba(48, 160, 208) },
        { 'W', rgba(232, 252, 255) }, { 'd', rgba(28, 104, 152) },
        { '*', rgba(255, 255, 255) },
        { 'D', rgba(58, 70, 88) },    { 'e', rgba(38, 48, 62) },   // dim pip (uncollected)
    };
    const size_t np = sizeof pal / sizeof pal[0];
    const char* f0[8] = { "...*....",
                          "...C....",
                          "..WCc...",
                          "..CCc...",
                          ".CCCcc..",
                          ".Cccdd..",
                          "..cdd...",
                          "...d...." };
    const char* f1[8] = { "...C....",
                          "..WCc*..",
                          "..CCc...",
                          ".CCCcc..",
                          ".Cccdd..",
                          "..cdd...",
                          "...d....",
                          "........" };
    const char* f2[8] = { "...C....",
                          "..WCc...",
                          "..CCc...",
                          ".CCCcc..",
                          ".Cccdd..",
                          "..cdd*..",
                          "...d....",
                          "........" };
    const char* f3[8] = { "...C....",
                          "..WCc...",
                          "..CCc...",
                          ".CCCcc..",
                          "*Cccdd..",
                          "..cdd...",
                          "...d....",
                          "........" };
    const char* f4[8] = { "...D....",   // dim HUD pip (uncollected slot; no tint on GBA OBJ)
                          "..DDe...",
                          "..DDe...",
                          ".DDDee..",
                          ".Deeee..",
                          "..eee...",
                          "...e....",
                          "........" };
    const char* const* frames[kShardFrames] = { f0, f1, f2, f3, f4 };
    std::vector<uint32_t> tex(size_t(kShardFrames * 8) * 8, 0u);
    for (int f = 0; f < kShardFrames; ++f)
        blit(tex, kShardFrames * 8, f * 8, 0, frames[f], 8, 8, pal, np);
    return tex;
}

constexpr int kHeartFrames = 3;   // 8×8 heart bloom: 2 pulse frames + a hollow HUD frame
inline std::vector<uint32_t> make_heart() {
    const Pal pal[] = {
        { 'R', rgba(255, 80, 96) }, { 'r', rgba(200, 40, 64) },
        { 'W', rgba(255, 176, 184) }, { 'g', rgba(72, 140, 72) },
        { 'D', rgba(74, 52, 62) },                                 // hollow HUD outline
    };
    const size_t np = sizeof pal / sizeof pal[0];
    const char* f0[8] = { ".rr..rr.",
                          "rWRrrRRr",
                          "rWRRRRRr",
                          "rRRRRRRr",
                          ".rRRRRr.",
                          "..rRRr..",
                          "...rr...",
                          "...g...." };
    const char* f1[8] = { "........",
                          ".rr..rr.",
                          "rWRrrRRr",
                          "rRRRRRRr",
                          ".rRRRRr.",
                          "..rRRr..",
                          "...rr...",
                          "...g...." };
    const char* f2[8] = { ".DD..DD.",   // hollow (lost HP; the GBA OBJ path has no tint)
                          "D..DD..D",
                          "D......D",
                          "D......D",
                          ".D....D.",
                          "..D..D..",
                          "...DD...",
                          "........" };
    const char* const* frames[kHeartFrames] = { f0, f1, f2 };
    std::vector<uint32_t> tex(size_t(kHeartFrames * 8) * 8, 0u);
    for (int f = 0; f < kHeartFrames; ++f)
        blit(tex, kHeartFrames * 8, f * 8, 0, frames[f], 8, 8, pal, np);
    return tex;
}

// ==== waystone: 16×32 — frame 0 dormant, frames 1,2 lit (rune + aura pulsing) ================
// (16×32 = a real GBA OBJ shape (2×4 tiles); the stone occupies the lower 24 rows.)
constexpr int kStoneFrames = 3;
constexpr int kStoneW = 16, kStoneH = 32;

inline std::vector<uint32_t> make_waystone() {
    // Same grid for all three frames; the rune/aura chars map to different colours per frame.
    const char* rows[kStoneH] = {
        "................",
        "................",
        "................",
        "................",
        "................",
        "................",
        "................",
        "................",
        "................",
        ".....a..a.......",
        "....KKKKKK......",
        "...KSSSSSSK.....",
        "..KSSHrrHSSK....",
        "..KSHrRRrHSK.a..",
        ".aKSHRssRHSK....",
        "..KSHRssRHSK....",
        "..KSHrRRrHSK....",
        "..KSSHrrHSSK....",
        "..KSSSSSSSSK....",
        "..KsSSSSSSsK....",
        "..KSSSSSSSSK.a..",
        ".aKsSSSSSSsK....",
        "..KSSSSSSSSK....",
        "..KsSSSSSSsK....",
        "..KSSSSSSSSK....",
        "..KKSSSSSSKK....",
        "...KKSSSSKK.....",
        "..KKSSSSSSKK....",
        ".KSSSSSSSSSSK...",
        ".KKKKKKKKKKKK...",
        "................",
        "................",
    };
    const Pal dormant[] = {
        { 'K', rgba(20, 16, 26) },  { 'S', rgba(80, 96, 104) },  { 's', rgba(58, 70, 78) },
        { 'H', rgba(104, 124, 130) },
        { 'r', rgba(64, 78, 86) },  { 'R', rgba(54, 66, 74) },   // rune: unlit, near-stone
        { 'a', 0u },                                             // aura: invisible
    };
    const Pal lit0[] = {
        { 'K', rgba(20, 16, 26) },  { 'S', rgba(88, 106, 114) }, { 's', rgba(58, 70, 78) },
        { 'H', rgba(112, 134, 140) },
        { 'r', rgba(88, 224, 192) },  { 'R', rgba(160, 255, 228) },
        { 'a', rgba(120, 240, 208) },
    };
    const Pal lit1[] = {
        { 'K', rgba(20, 16, 26) },  { 'S', rgba(88, 106, 114) }, { 's', rgba(58, 70, 78) },
        { 'H', rgba(112, 134, 140) },
        { 'r', rgba(64, 192, 164) },  { 'R', rgba(120, 244, 210) },
        { 'a', 0u },                                             // aura blinks off
    };

    std::vector<uint32_t> tex(size_t(kStoneFrames * kStoneW) * kStoneH, 0u);
    blit(tex, kStoneFrames * kStoneW, 0 * kStoneW, 0, rows, kStoneW, kStoneH,
         dormant, sizeof dormant / sizeof dormant[0]);
    blit(tex, kStoneFrames * kStoneW, 1 * kStoneW, 0, rows, kStoneW, kStoneH,
         lit0, sizeof lit0 / sizeof lit0[0]);
    blit(tex, kStoneFrames * kStoneW, 2 * kStoneW, 0, rows, kStoneW, kStoneH,
         lit1, sizeof lit1 / sizeof lit1[0]);
    return tex;
}

// ==== the Sungate: 32×32, 2 shimmer frames (same grid, two palettes) =========================
constexpr int kGateFrames = 2;
constexpr int kGateW = 32, kGateH = 32;

inline std::vector<uint32_t> make_gate() {
    const char* rows[32] = {
        "................................",
        "..........KKKKKKKKKK...........",
        "........KKGGGGGGGGGGKK.........",
        ".......KGGgggggggggGGGK........",
        "......KGGg..........gGGK.......",
        ".....KGGg....*.......gGGK......",
        "....KGGg..............gGGK.....",
        "....KGg......hhhh......gGK.....",
        "...KGGg....hhHHHHhh....gGGK....",
        "...KGg....hHHIIIIHHh....gGK....",
        "...KGg...hHHIIIIIIHh.....GK....",
        "..KGGg...hHIIWWIIIHh....gGGK...",
        "..KGg...hHHIIWWWIIHHh....gGK...",
        "..KGg...hHIIIWWWIIIHh....gGK...",
        "..KGg...hHIIIIWIIIIHh....gGK...",
        "..KGg...hHHIIIIIIIHHh....gGK...",
        "..KGg....hHHIIIIIHHh.....gGK...",
        "..KGg.....hHHHIHHHh......gGK...",
        "..KGGg.....hhhHhhh......gGGK...",
        "...KGg........h.........gGK....",
        "...KGg.............*....gGK....",
        "...KGGg................gGGK....",
        "....KGg................gGK.....",
        "....KGGg..............gGGK.....",
        ".....KGGg............gGGK......",
        "......KGGgggg....ggggGGK.......",
        ".......KGGGGKKKKKKGGGGK........",
        "......KKKKKK......KKKKKK.......",
        "......KGGSK........KSGGK.......",
        "......KGSSK........KSSGK.......",
        ".....KKKKKKK......KKKKKKK......",
        "................................",
    };
    // The two shimmer frames live in ONE texture, and on the GBA an OBJ indexes a single
    // 16-colour palette bank — so the union of both palettes stays ≤ 15 opaque colours
    // (K/S/h/* are shared between frames).
    const Pal dim[] = {
        { 'K', rgba(28, 20, 24) },
        { 'G', rgba(200, 144, 48) },   // gold ring
        { 'g', rgba(144, 100, 40) },   // ring inner edge
        { 'S', rgba(96, 76, 64) },     // pedestal feet
        { 'h', rgba(120, 64, 40) },    // halo, faint
        { 'H', rgba(200, 112, 48) },   // inner halo
        { 'I', rgba(255, 176, 72) },   // core glow
        { 'W', rgba(255, 232, 160) },  // hot core
        { '*', rgba(255, 244, 200) },  // stray spark
    };
    const Pal hot[] = {
        { 'K', rgba(28, 20, 24) },
        { 'G', rgba(224, 168, 64) },
        { 'g', rgba(168, 120, 48) },
        { 'S', rgba(96, 76, 64) },
        { 'h', rgba(120, 64, 40) },    // shared with dim
        { 'H', rgba(240, 144, 56) },
        { 'I', rgba(255, 208, 96) },
        { 'W', rgba(255, 248, 208) },
        { '*', rgba(255, 244, 200) },  // shared with dim
    };
    std::vector<uint32_t> tex(size_t(kGateFrames * kGateW) * kGateH, 0u);
    blit(tex, kGateFrames * kGateW, 0, 0, rows, kGateW, kGateH, dim, sizeof dim / sizeof dim[0]);
    blit(tex, kGateFrames * kGateW, kGateW, 0, rows, kGateW, kGateH, hot, sizeof hot / sizeof hot[0]);
    return tex;
}

// ==== spark particle: 8×8 (the GBA's smallest OBJ) ============================================
inline std::vector<uint32_t> make_spark() {
    const Pal pal[] = { { 'W', rgba(255, 244, 176) }, { 'O', rgba(255, 176, 64) } };
    const char* rows[8] = { "........",
                            "...O....",
                            "..OWO...",
                            ".OWWWO..",
                            "..OWO...",
                            "...OO...",
                            "....O...",
                            "........" };
    std::vector<uint32_t> tex(64, 0u);
    blit(tex, 8, 0, 0, rows, 8, 8, pal, 2);
    return tex;
}

} // namespace art
} // namespace game
#endif // EMBERWING_ART_H
