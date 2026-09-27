// tools/phxstudio/spawnart.h — what each spawn type LOOKS like, for the map editor: the spawn's
// prefab row (a phxbin table with `type` and `sprite` columns, the same ones phx::Level reads) ->
// that sprite's source in the project (a .sprdef / sprite .json, or a plain .png) -> the frame
// the game shows first (the "idle" clip's first frame, else frame 0). Headless, unit-tested in
// the editors suite; the map editor draws the resolved frame at each spawn instead of a box.
#ifndef PHX_TOOLS_PHXSTUDIO_SPAWNART_H
#define PHX_TOOLS_PHXSTUDIO_SPAWNART_H

#include "pixeldoc.h"
#include "project.h"
#include "../phxentity/editor.h"   // BinDoc

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace phxstudio {

struct SpawnArt {
    std::string sheet;               // absolute path of the sheet PNG
    int sx = 0, sy = 0, w = 0, h = 0;  // the frame, in sheet pixels (w = 0: the whole image)
};

namespace spawnart_detail {
inline bool slurp(const std::string& path, std::string& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[16384];
    size_t n;
    out.clear();
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return true;
}
// Width/height of a PNG from its IHDR (no decode).
inline bool png_size(const std::string& path, int& w, int& h) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t b[24];
    const bool ok = std::fread(b, 1, 24, f) == 24 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G';
    std::fclose(f);
    if (!ok) return false;
    w = int((uint32_t(b[16]) << 24) | (uint32_t(b[17]) << 16) | (uint32_t(b[18]) << 8) | b[19]);
    h = int((uint32_t(b[20]) << 24) | (uint32_t(b[21]) << 16) | (uint32_t(b[22]) << 8) | b[23]);
    return w > 0 && h > 0;
}
} // namespace spawnart_detail

// Every spawn type a prefab table gives a sprite -> its first frame. `files` are root-relative
// (the Studio's file list); build/ is ignored.
inline std::map<std::string, SpawnArt> resolve_spawn_art(const std::string& root, const std::vector<std::string>& files) {
    using namespace spawnart_detail;
    auto in_build = [](const std::string& rel) { return rel.compare(0, 6, "build/") == 0; };
    // 1. type -> sprite name, from every prefab table
    std::map<std::string, std::string> sprite_of;
    for (const std::string& rel : files) {
        if (lower_ext(rel) != ".json" || in_build(rel)) continue;
        std::string text;
        if (!slurp(join_path(root, rel), text) || text.find("\"records\"") == std::string::npos) continue;
        phxtool::BinDoc d;
        if (!phxtool::BinDoc::load(text, d)) continue;
        size_t tf = d.fields.size(), sf = d.fields.size();
        for (size_t f = 0; f < d.fields.size(); ++f) {
            if (d.fields[f].name == "type" && d.field_is_str(f)) tf = f;
            if (d.fields[f].name == "sprite" && d.field_is_str(f)) sf = f;
        }
        if (tf == d.fields.size() || sf == d.fields.size()) continue;
        for (size_t r = 0; r < d.records.size(); ++r)
            if (!d.str_cell(r, tf).empty() && !d.str_cell(r, sf).empty() && !sprite_of.count(d.str_cell(r, tf)))
                sprite_of[d.str_cell(r, tf)] = d.str_cell(r, sf);
    }
    // 2. sprite name -> its source -> the first frame
    std::map<std::string, SpawnArt> out;
    for (const auto& ts : sprite_of) {
        std::string def, png;
        for (const std::string& rel : files) {
            if (in_build(rel) || stem_of(rel) != ts.second) continue;
            const std::string e = lower_ext(rel);
            if (e == ".sprdef") def = rel;
            else if (e == ".json" && def.empty()) {
                std::string t;
                if (slurp(join_path(root, rel), t) && SprDoc::is_sprite_json(t)) def = rel;
            } else if (e == ".png") png = rel;
        }
        SpawnArt a;
        if (!def.empty()) {
            SprDoc sd;
            const std::string dabs = join_path(root, def);
            if (!SprDoc::load(dabs, sd) || sd.frame_w <= 0 || sd.frame_h <= 0) continue;
            std::string sheet = SprDoc::resolve_sheet(dabs, sd.sheet);
            if (!is_abs_path(sheet)) sheet = join_path(root, sheet);
            int sw = 0, sh = 0;
            if (!png_size(sheet, sw, sh)) continue;
            int frame = 0;
            for (const SprClip& c : sd.clips) if (c.name == "idle") { frame = c.first; break; }
            const int cols = std::max(1, sw / sd.frame_w);
            a.sheet = sheet;
            a.sx = (frame % cols) * sd.frame_w; a.sy = (frame / cols) * sd.frame_h;
            a.w = sd.frame_w; a.h = sd.frame_h;
        } else if (!png.empty()) {
            a.sheet = join_path(root, png);
            if (!png_size(a.sheet, a.w, a.h)) continue;
        } else {
            continue;
        }
        out[ts.first] = a;
    }
    return out;
}

} // namespace phxstudio
#endif // PHX_TOOLS_PHXSTUDIO_SPAWNART_H
