// tools/phxpack/main.cpp — the `.phxp` bundle assembler CLI. Host-only (STL allowed).
// Converts author-friendly inputs into baked, target-encoded blobs the engine mmaps.
//
//   phxpack --out assets.phxp [--target 0|1|2] [--compress] <inputs...>
//     *.png    -> Texture  (8-bit gray/RGB/palette/RGBA, non-interlaced -> RGBA8)
//     *.ppm    -> Texture  (P6 binary PPM, RGB -> RGBA8; name = file stem)
//     *.tmcsv  -> Tilemap  (see format below; name = file stem)
//     *.sprdef -> Texture + Sprite  (sheet PNG sliced into a frame grid + named anim clips)
//     *.tmj    -> Tilemap (+ Spawns) (Tiled JSON map: tile layers + object-group spawns)
//     *.wav    -> Sound  (RIFF PCM 8/16-bit, mono/stereo -> mono 16-bit at file rate)
//   --compress (-z): LZSS-compress each blob (kept only where it shrinks); the runtime
//                    ResourceCache decompresses on first access (see phx/resource/lz.h).
//
// .tmcsv format (lines starting with '#' are comments):
//     <tileset_name> <tile_w> <tile_h>
//     1,2,1
//     2,1,2
//
// WAV/JSON inputs plug in later behind decoders (docs/08); PPM/CSV remain as the
// dependency-free stand-ins alongside the real PNG decoder.
#include "bundle_writer.h"
#include "png.h"
#include "tiled.h"
#include "wav.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::string stem(const std::string& path) {
    size_t s = path.find_last_of("/\\");
    size_t d = path.find_last_of('.');
    size_t b = (s == std::string::npos) ? 0 : s + 1;
    size_t e = (d == std::string::npos || d < b) ? path.size() : d;
    return path.substr(b, e - b);
}
bool ends_with(const std::string& s, const char* suf) {
    size_t n = std::strlen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

// --- read a whole file into a byte vector ---
bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz < 0) { std::fclose(f); return false; }
    out.resize(size_t(sz));
    size_t got = std::fread(out.data(), 1, size_t(sz), f);
    std::fclose(f);
    return got == size_t(sz);
}

// --- P6 binary PPM -> RGBA8 ---
bool load_ppm(const std::string& path, std::vector<uint32_t>& out, uint16_t& w, uint16_t& h) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char magic[3] = {0};
    if (std::fscanf(f, "%2s", magic) != 1 || std::strcmp(magic, "P6") != 0) { std::fclose(f); return false; }
    int iw = 0, ih = 0, maxv = 0;
    if (std::fscanf(f, "%d %d %d", &iw, &ih, &maxv) != 3 || iw <= 0 || ih <= 0) { std::fclose(f); return false; }
    std::fgetc(f);                       // single whitespace after maxval
    w = uint16_t(iw); h = uint16_t(ih);
    out.resize(size_t(iw) * ih);
    for (size_t i = 0; i < out.size(); ++i) {
        int r = std::fgetc(f), g = std::fgetc(f), b = std::fgetc(f);
        if (b == EOF) { std::fclose(f); return false; }
        out[i] = uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | (uint32_t(255) << 24);
    }
    std::fclose(f);
    return true;
}

// --- .tmcsv -> indices + meta ---
bool load_tmcsv(const std::string& path, std::vector<uint16_t>& idx,
                uint16_t& w, uint16_t& h, std::string& tileset, int& tw, int& th) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char line[1024];
    bool have_header = false;
    std::vector<std::vector<uint16_t>> rows;
    while (std::fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        if (!have_header) {
            char name[256];
            if (std::sscanf(line, "%255s %d %d", name, &tw, &th) != 3) { std::fclose(f); return false; }
            tileset = name; have_header = true; continue;
        }
        std::vector<uint16_t> row;
        for (char* p = line; *p; ) {
            while (*p && !std::isdigit((unsigned char)*p)) ++p;
            if (!*p) break;
            row.push_back(uint16_t(std::strtol(p, &p, 10)));
        }
        if (!row.empty()) rows.push_back(std::move(row));
    }
    std::fclose(f);
    if (!have_header || rows.empty()) return false;
    h = uint16_t(rows.size());
    w = uint16_t(rows[0].size());
    idx.clear(); idx.reserve(size_t(w) * h);
    for (auto& row : rows) {
        for (uint16_t x = 0; x < w; ++x) idx.push_back(x < row.size() ? row[x] : 0);
    }
    return true;
}

std::string dir_of(const std::string& path) {
    size_t s = path.find_last_of("/\\");
    return s == std::string::npos ? std::string() : path.substr(0, s + 1);
}

// --- .sprdef -> sheet PNG + frame size + named clips ---
//   sheet <png> <frame_w> <frame_h>
//   clip  <name> <first> <count> <fps> <loop 0|1>
struct SprDef {
    std::string sheet;                 // PNG path (resolved relative to the .sprdef dir)
    int fw = 0, fh = 0;
    std::vector<std::pair<std::string, phx::SpriteClipDef>> clips;  // (name, def)
};
bool load_sprdef(const std::string& path, SprDef& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char line[1024];
    while (std::fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        char a[256], b[256]; int v1, v2, v3, v4;
        if (std::sscanf(line, "sheet %255s %d %d", a, &v1, &v2) == 3) {
            out.sheet = a; out.fw = v1; out.fh = v2;
        } else if (std::sscanf(line, "clip %255s %d %d %d %d", b, &v1, &v2, &v3, &v4) == 5) {
            phx::SpriteClipDef c{};
            c.name  = phx::fnv1a(b);
            c.first = uint16_t(v1); c.count = uint16_t(v2);
            c.fps   = uint8_t(v3);  c.loop  = uint8_t(v4 ? 1 : 0);
            out.clips.emplace_back(b, c);
        }
    }
    std::fclose(f);
    // resolve the sheet path relative to the .sprdef's directory if it isn't absolute
    if (!out.sheet.empty() && out.sheet[0] != '/' && out.sheet.find('/') == std::string::npos)
        out.sheet = dir_of(path) + out.sheet;
    return !out.sheet.empty() && out.fw > 0 && out.fh > 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string out;
    int target = 2;
    bool compress = false;
    std::vector<std::string> inputs;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--out" && i + 1 < argc)         out = argv[++i];
        else if (a == "--target" && i + 1 < argc) target = std::atoi(argv[++i]);
        else if (a == "--compress" || a == "-z")  compress = true;
        else if (a == "--help" || a == "-h") {
            std::printf("usage: phxpack --out FILE [--target 0|1|2] [--compress] <inputs.png|.ppm|.tmcsv|.sprdef|.tmj|.wav ...>\n");
            return 0;
        } else inputs.push_back(a);
    }
    if (out.empty()) { std::fprintf(stderr, "phxpack: --out is required (try --help)\n"); return 2; }

    phxtool::BundleWriter w{uint8_t(target)};
    w.set_compression(compress);
    for (const std::string& in : inputs) {
        if (ends_with(in, ".png")) {
            std::vector<uint8_t> bytes;
            if (!read_file(in, bytes)) { std::fprintf(stderr, "phxpack: cannot read '%s'\n", in.c_str()); return 1; }
            std::vector<uint32_t> px; uint16_t iw, ih;
            if (!phxtool::png_decode(bytes.data(), bytes.size(), px, iw, ih)) {
                std::fprintf(stderr, "phxpack: bad/unsupported PNG '%s'\n", in.c_str()); return 1; }
            w.add_texture(stem(in), px.data(), iw, ih);
            std::printf("  + texture %-12s %dx%d  (%s, PNG)\n", stem(in).c_str(), iw, ih, in.c_str());
        } else if (ends_with(in, ".ppm")) {
            std::vector<uint32_t> px; uint16_t iw, ih;
            if (!load_ppm(in, px, iw, ih)) { std::fprintf(stderr, "phxpack: bad PPM '%s'\n", in.c_str()); return 1; }
            w.add_texture(stem(in), px.data(), iw, ih);
            std::printf("  + texture %-12s %dx%d  (%s)\n", stem(in).c_str(), iw, ih, in.c_str());
        } else if (ends_with(in, ".tmcsv")) {
            std::vector<uint16_t> idx; uint16_t iw, ih; std::string ts; int tw, th;
            if (!load_tmcsv(in, idx, iw, ih, ts, tw, th)) { std::fprintf(stderr, "phxpack: bad tmcsv '%s'\n", in.c_str()); return 1; }
            w.add_tilemap(stem(in), idx.data(), iw, ih, 1, uint8_t(tw), uint8_t(th), ts);
            std::printf("  + tilemap %-12s %dx%d tiles, tileset '%s'  (%s)\n", stem(in).c_str(), iw, ih, ts.c_str(), in.c_str());
        } else if (ends_with(in, ".tmj")) {
            std::vector<uint8_t> bytes;
            if (!read_file(in, bytes)) { std::fprintf(stderr, "phxpack: cannot read '%s'\n", in.c_str()); return 1; }
            phxtool::TiledMap tm;
            if (!phxtool::tiled_load(std::string(bytes.begin(), bytes.end()), tm)) {
                std::fprintf(stderr, "phxpack: bad/unsupported Tiled map '%s'\n", in.c_str()); return 1; }
            // Flatten the tile layers into one [layer][y*w+x] buffer for the engine tilemap.
            std::vector<uint16_t> flat;
            flat.reserve(tm.layers.size() * size_t(tm.width) * size_t(tm.height));
            for (const auto& L : tm.layers) flat.insert(flat.end(), L.begin(), L.end());
            w.add_tilemap(stem(in), flat.data(), uint16_t(tm.width), uint16_t(tm.height),
                          uint8_t(tm.layers.size()), uint8_t(tm.tile_w), uint8_t(tm.tile_h), tm.tileset);
            std::printf("  + tilemap %-12s %dx%d tiles x%u layers, tileset '%s'  (%s, Tiled)\n",
                        stem(in).c_str(), tm.width, tm.height, unsigned(tm.layers.size()), tm.tileset.c_str(), in.c_str());
            if (!tm.spawns.empty()) {
                std::vector<phx::SpawnDef> sd;
                for (const auto& s : tm.spawns)
                    sd.push_back(phx::SpawnDef{ phx::fnv1a((s.type.empty()? s.name : s.type).c_str()),
                                               int16_t(s.x), int16_t(s.y), uint16_t(s.w), uint16_t(s.h) });
                w.add_spawns(stem(in), sd);
                std::printf("  + spawns  %-12s %u objects  (%s)\n", stem(in).c_str(), unsigned(sd.size()), in.c_str());
            }
        } else if (ends_with(in, ".wav")) {
            std::vector<uint8_t> bytes;
            if (!read_file(in, bytes)) { std::fprintf(stderr, "phxpack: cannot read '%s'\n", in.c_str()); return 1; }
            std::vector<int16_t> mono; uint32_t rate = 0;
            if (!phxtool::wav_decode(bytes.data(), bytes.size(), mono, rate)) {
                std::fprintf(stderr, "phxpack: bad/unsupported WAV '%s'\n", in.c_str()); return 1; }
            w.add_sound(stem(in), mono.data(), uint32_t(mono.size()), rate);
            std::printf("  + sound   %-12s %u frames @ %u Hz  (%s, WAV)\n",
                        stem(in).c_str(), unsigned(mono.size()), rate, in.c_str());
        } else if (ends_with(in, ".sprdef")) {
            SprDef sd;
            if (!load_sprdef(in, sd)) { std::fprintf(stderr, "phxpack: bad sprdef '%s'\n", in.c_str()); return 1; }
            std::vector<uint8_t> bytes;
            if (!read_file(sd.sheet, bytes)) { std::fprintf(stderr, "phxpack: sprdef '%s' cannot read sheet '%s'\n", in.c_str(), sd.sheet.c_str()); return 1; }
            std::vector<uint32_t> px; uint16_t iw, ih;
            if (!phxtool::png_decode(bytes.data(), bytes.size(), px, iw, ih)) {
                std::fprintf(stderr, "phxpack: sprdef '%s' bad sheet PNG '%s'\n", in.c_str(), sd.sheet.c_str()); return 1; }
            const std::string texname = stem(sd.sheet);
            w.add_texture(texname, px.data(), iw, ih);
            const uint16_t cols = uint16_t(iw / uint16_t(sd.fw));
            std::vector<phx::SpriteClipDef> clips;
            for (auto& c : sd.clips) clips.push_back(c.second);
            w.add_sprite(stem(in), texname, uint16_t(sd.fw), uint16_t(sd.fh), cols, clips);
            std::printf("  + sprite  %-12s sheet '%s' %dx%d, %u clips  (%s)\n",
                        stem(in).c_str(), texname.c_str(), sd.fw, sd.fh, unsigned(clips.size()), in.c_str());
        } else {
            std::fprintf(stderr, "phxpack: unknown input type '%s' (want .ppm/.tmcsv)\n", in.c_str());
            return 1;
        }
    }

    if (!w.write(out)) { std::fprintf(stderr, "phxpack: cannot write '%s'\n", out.c_str()); return 1; }
    std::printf("phxpack: wrote %s  (%u assets, target tier %d%s)\n",
                out.c_str(), w.count(), target, compress ? ", LZ-compressed" : "");
    return 0;
}
