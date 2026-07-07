// examples/emberwing/level.h — HOST-ONLY level authoring for Cinder Hollow. The level is ten
// 32×20 ASCII sections (one char per 8px tile, designed beat by beat in README.md §3),
// composed into a real Tiled `.tmj` and run through the engine's actual Tiled importer at
// bake time — so this is exactly the artefact the phxtile converter / phxtmap editor consume.
//
// Legend (gameplay layer):
//   '.' air        '#' ground       'B' basalt block    '|' obsidian column   '-' platform
//   '~' lava (VISUAL on the deco layer + a merged rect hazard object; never solid)
// Entities (become centred Tiled point objects; grounded types snap to the ground below):
//   'P' player  'o' ember  'S' shard  'H' heart  'c' cinderling  't' thornback
//   'w' ashwisp 'g' geyser 'C' waystone  'G' the Sungate
//
// Backdrop layers (sky bands/stars, crag silhouettes, deco lava/ruins/vents) are generated
// deterministically here rather than hand-gridded per cell.
#ifndef EMBERWING_LEVEL_H
#define EMBERWING_LEVEL_H

#include "art.h"   // Tile enum — the deco/solid layers reference atlas cells by name

#include <cstdint>
#include <string>
#include <vector>

namespace game {
namespace level {

constexpr int kSectW = 32, kSectH = 20, kSections = 10;
constexpr int kW = kSectW * kSections;   // 320 tiles = 2560 px
constexpr int kH = kSectH;               // 20 tiles  = 160 px

// ---- the ten sections (see README.md §3 for the design intent of each) ----------------------
inline const char* const* sections(int i) {
    // S0 — The Hearth: safe flat start; ember line teaches collecting; one cinderling.
    static const char* s0[kSectH] = {
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "..........oooo..................",
        "................................",
        "...P.....................c.....",
        "################################",
        "################################",
        "################################",
        "################################",
    };
    // S1 — Columns: rising jump heights (2/3/3), rewards on top, cinderlings between.
    static const char* s1[kSectH] = {
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        ".....................o..........",
        "..............|......|..........",
        "......|..o....|......|....o.....",
        "......|.......|...c..|.....c....",
        "################################",
        "################################",
        "################################",
        "################################",
    };
    // S2 — First Blood: a 3-wide lava pit under an ember arc; blocks with a hidden-height
    // shard; a cinderling pair beyond.
    static const char* s2[kSectH] = {
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        ".................S..............",
        "................................",
        "................................",
        "................................",
        "................o.o.............",
        ".........o.....BBB..............",
        "........o.o.....................",
        ".......o...o....................",
        "........................c..c....",
        "########...#####################",
        "########...#####################",
        "########~~~#####################",
        "########~~~#####################",
    };
    // S3 — Waystone I, then an ashwisp guarding a small pit and a 4-wide pit with an arc.
    static const char* s3[kSectH] = {
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        ".....................o..........",
        "............w.......o.o.........",
        "...................o...o........",
        "...........................H...",
        "....C...........................",
        "############..######....########",
        "############..######....########",
        "############~~######~~~~########",
        "############~~######~~~~########",
    };
    // S4 — Geyser Alley: three phase-offset vents, ember bait, a thornback at the exit,
    // and the block stair up toward the high road.
    static const char* s4[kSectH] = {
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "...............................B",
        "..........o.......o...........BB",
        "......g.......g.......g.t....BBB",
        "################################",
        "################################",
        "################################",
        "################################",
    };
    // S5 — The Split: low road (lava pools, thornback, geyser — faster) vs high road
    // (platforms, ember trail, a shard at the apex).
    static const char* s5[kSectH] = {
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "..................S.............",
        "................................",
        "................................",
        "...........o......o.............",
        "..........---....---............",
        "....o....................o......",
        "...---..................---.....",
        "................................",
        "................................",
        "...........o..o.................",
        "...........t..........g........",
        "######...######....#############",
        "######...######....#############",
        "######~~~######~~~~#############",
        "######~~~######~~~~#############",
    };
    // S6 — The Lava Lake: a 20-wide lake crossed on column tops and a plank, two ashwisps,
    // the shard at the apex, a heart bloom on the far shore.
    static const char* s6[kSectH] = {
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "............S...................",
        "...............w................",
        "..........w.........o...........",
        "............o.......--..........",
        ".......o....|...................",
        ".......|....|....|..........H...",
        ".......|....|....|.......ooo....",
        "####...|....|....|......########",
        "####~~~|~~~~|~~~~|~~~~~~########",
        "####~~~~~~~~~~~~~~~~~~~~########",
        "####~~~~~~~~~~~~~~~~~~~~########",
    };
    // S7 — Waystone II + Stair Valley: the classic ascending/descending stair-gap rhythm,
    // cinderlings marching the floor between.
    static const char* s7[kSectH] = {
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        ".............o.................",
        "...........o...o..............o",
        "...........B...B............o..",
        "..........BB...BB...........B..",
        ".........BBB...BBB.........BB..",
        "....C...BBBB...BBBB..c..c.BBB..",
        "############...##############..#",
        "############...##############..#",
        "############~~~##############~~#",
        "############~~~##############~~#",
    };
    // S8 — The Sprint: a speed section; short pits, a cinderling column, a wisp overhead,
    // one last geyser.
    static const char* s8[kSectH] = {
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        "................................",
        ".....................w..........",
        "................................",
        "...ooo...ooo....o.......o.......",
        "..................c.c.c...g.....",
        "######..####..##################",
        "######..####..##################",
        "######~~####~~##################",
        "######~~####~~##################",
    };
    // S9 — The Sungate: the final staircase, an ember crown over the last jump, the gate on
    // its plateau, dormant columns beyond, and the end-cap wall.
    static const char* s9[kSectH] = {
        "..............................##",
        "..............................##",
        "..............................##",
        "..............................##",
        "..............................##",
        "..............................##",
        "..............................##",
        "..............................##",
        "..............................##",
        "..............................##",
        ".......o............|...|.....##",
        "......o.......G.....|...|.....##",
        ".....o.B########################",
        "....o.BB########################",
        ".....BBB########################",
        "....BBBB########################",
        "################################",
        "################################",
        "################################",
        "################################",
    };
    static const char* const* all[kSections] = { s0, s1, s2, s3, s4, s5, s6, s7, s8, s9 };
    return all[i];
}

// ---- helpers ---------------------------------------------------------------------------------
inline char cell(const std::vector<std::string>& g, int x, int y) {
    if (x < 0 || y < 0 || x >= kW || y >= kH) return '.';
    return g[y][x];
}
inline bool solid_char(char c) { return c == '#' || c == 'B' || c == '|' || c == '-'; }
inline bool entity_char(char c) {
    return c == 'P' || c == 'o' || c == 'S' || c == 'H' || c == 'c' || c == 't'
        || c == 'w' || c == 'g' || c == 'C' || c == 'G';
}

// Compose the ten sections into one kW×kH char grid.
inline std::vector<std::string> compose() {
    std::vector<std::string> g(kH);
    for (int y = 0; y < kH; ++y) {
        g[y].reserve(kW);
        for (int s = 0; s < kSections; ++s) {
            std::string row = sections(s)[y];
            row.resize(kSectW, '.');            // tolerate trailing-space slips
            g[y] += row;
        }
    }
    return g;
}

// ---- the .tmj emitter --------------------------------------------------------------------------
// Produces the same artefact a designer would save from Tiled: four tile layers (sky, crags,
// deco, solid — the LAST is the gameplay/collision layer by engine convention) with per-layer
// parallax factors, plus one object group of centred spawn points and merged lava rects.
inline std::string build_tmj() {
    using namespace art;
    const std::vector<std::string> g = compose();

    // Ground row below (x, y): the snap target for grounded entities. kH when over a pit.
    auto ground_row = [&](int x, int y) {
        for (int gy = y + 1; gy < kH; ++gy)
            if (solid_char(cell(g, x, gy))) return gy;
        return kH;
    };

    // -- solid (gameplay) layer --
    std::vector<uint16_t> solid(size_t(kW) * kH, 0);
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            const char c = g[y][x];
            uint16_t t = 0;
            if (c == '#') {
                if (cell(g, x, y - 1) != '#')       t = kTGroundTop + 1;
                else if (((x * 31 + y * 17) % 7) == 0) t = kTGroundFleck + 1;
                else                                 t = kTGroundFill + 1;
            } else if (c == 'B') t = kTBlock + 1;
            else if (c == '|')   t = (cell(g, x, y - 1) == '|') ? kTColumnShaft + 1
                                                                : kTColumnCap + 1;
            else if (c == '-')   t = kTPlatform + 1;
            solid[size_t(y) * kW + x] = t;
        }

    // -- deco layer: lava visuals, geyser vents, background ruins --
    std::vector<uint16_t> deco(size_t(kW) * kH, 0);
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x)
            if (g[y][x] == '~')
                deco[size_t(y) * kW + x] = (cell(g, x, y - 1) == '~') ? kTLavaFill + 1
                                                                      : kTLavaTop + 1;
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x)
            if (g[y][x] == 'g') {
                const int gy = ground_row(x, y);
                if (gy > 0 && gy < kH) deco[size_t(gy - 1) * kW + x] = kTVent + 1;
            }
    // ruined columns every ~37 tiles where there's ground (deterministic scatter)
    for (int x = 11; x < kW; x += 37) {
        const int gy = ground_row(x, -1);
        if (gy >= kH || gy < 4) continue;
        deco[size_t(gy - 3) * kW + x] = kTRuinCap + 1;
        deco[size_t(gy - 2) * kW + x] = kTRuinShaft + 1;
        deco[size_t(gy - 1) * kW + x] = kTRuinShaft + 1;
    }

    // -- sky layer: banded dusk gradient with stars and drifting haze --
    std::vector<uint16_t> sky(size_t(kW) * kH, 0);
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            uint16_t t;
            if      (y < 3)  t = ((x * 13 + y * 7) % 19) == 0 ? kTStars + 1 : kTSkyDeep + 1;
            else if (y < 6)  t = ((x * 11 + y * 5) % 23) == 0 ? kTHaze + 1  : kTSkyMid + 1;
            else if (y < 10) t = kTSkyLow + 1;
            else if (y < 13) t = kTSkyGlow + 1;
            else             t = kTSkyEmber + 1;
            sky[size_t(y) * kW + x] = t;
        }

    // -- crag layer: a jagged silhouette ridge with glowing cracks --
    std::vector<uint16_t> crag(size_t(kW) * kH, 0);
    static const int kRidge[16] = { 4, 5, 6, 7, 8, 8, 7, 6, 5, 5, 6, 7, 9, 8, 6, 5 };
    for (int x = 0; x < kW; ++x) {
        const int h = kRidge[(x / 2) % 16];
        const int base = kH - 1 - h;
        for (int y = base; y < kH; ++y)
            crag[size_t(y) * kW + x] =
                (y == base)                        ? kTCragTop + 1
              : ((x * 7 + y * 11) % 37) == 0       ? kTCragGlow + 1
                                                   : kTCragFill + 1;
    }

    // -- objects: entities (centred, ground-snapped) + merged lava hazard rects --
    struct Obj { std::string type; int x, y, w, h; };
    std::vector<Obj> objs;
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            const char c = g[y][x];
            if (!entity_char(c)) continue;
            const int cx = x * 8 + 4;
            const int gt = ground_row(x, y) * 8;         // ground top, px
            switch (c) {
                case 'P': objs.push_back({ "player",   cx, gt - 8,  0, 0 }); break;
                case 'c': objs.push_back({ "cinder",   cx, gt - 8,  0, 0 }); break;
                case 't': objs.push_back({ "thorn",    cx, gt - 8,  0, 0 }); break;
                case 'g': objs.push_back({ "geyser",   cx, gt - 16, 0, 0 }); break;
                case 'C': objs.push_back({ "waystone", cx, gt - 12, 0, 0 }); break;
                case 'G': objs.push_back({ "gate",     cx, gt - 16, 0, 0 }); break;
                case 'o': objs.push_back({ "ember",    cx, y * 8 + 4, 0, 0 }); break;
                case 'S': objs.push_back({ "shard",    cx, y * 8 + 4, 0, 0 }); break;
                case 'H': objs.push_back({ "heart",    cx, y * 8 + 4, 0, 0 }); break;
                case 'w': objs.push_back({ "wisp",     cx, y * 8 + 4, 0, 0 }); break;
            }
        }
    for (int y = 0; y < kH; ++y) {                       // lava surface runs -> one rect each
        int x = 0;
        while (x < kW) {
            if (g[y][x] == '~' && cell(g, x, y - 1) != '~') {
                int x1 = x;
                while (x1 + 1 < kW && g[y][x1 + 1] == '~' && cell(g, x1 + 1, y - 1) != '~') ++x1;
                objs.push_back({ "lava", (x + x1 + 1) * 8 / 2, y * 8 + 4, (x1 - x + 1) * 8, 8 });
                x = x1 + 1;
            } else ++x;
        }
    }

    // -- JSON --
    auto emit_layer = [&](std::string& j, const char* name, const std::vector<uint16_t>& d,
                          const char* parallax) {
        j += "{\"type\":\"tilelayer\",\"name\":\"";
        j += name;
        j += "\",\"width\":" + std::to_string(kW) + ",\"height\":" + std::to_string(kH) + ",";
        j += parallax;
        j += "\"data\":[";
        for (size_t i = 0; i < d.size(); ++i) {
            if (i) j += ",";
            j += std::to_string(d[i]);
        }
        j += "]}";
    };

    std::string j = "{\"width\":" + std::to_string(kW) + ",\"height\":" + std::to_string(kH);
    j += ",\"tilewidth\":8,\"tileheight\":8,";
    j += "\"tilesets\":[{\"firstgid\":1,\"name\":\"tiles\"}],\"layers\":[";
    emit_layer(j, "sky",   sky,   "\"parallaxx\":0.25,\"parallaxy\":0.5,");
    j += ",";
    emit_layer(j, "crags", crag,  "\"parallaxx\":0.5,\"parallaxy\":1.0,");
    j += ",";
    emit_layer(j, "deco",  deco,  "");
    j += ",";
    emit_layer(j, "solid", solid, "");
    j += ",{\"type\":\"objectgroup\",\"name\":\"entities\",\"objects\":[";
    for (size_t i = 0; i < objs.size(); ++i) {
        if (i) j += ",";
        j += "{\"name\":\"e" + std::to_string(i) + "\",\"type\":\"" + objs[i].type + "\"";
        if (objs[i].w) {
            j += ",\"x\":" + std::to_string(objs[i].x) + ",\"y\":" + std::to_string(objs[i].y);
            j += ",\"width\":" + std::to_string(objs[i].w)
               + ",\"height\":" + std::to_string(objs[i].h) + "}";
        } else {
            j += ",\"point\":true,\"x\":" + std::to_string(objs[i].x)
               + ",\"y\":" + std::to_string(objs[i].y) + "}";
        }
    }
    j += "]}]}";
    return j;
}

} // namespace level
} // namespace game
#endif // EMBERWING_LEVEL_H
