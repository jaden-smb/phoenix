// tools/phxpack/builders.h — the shared bake logic (host-only, STL allowed). Turns an
// author-friendly source file (.png/.ppm/.tmcsv/.tmj/.wav/.sprdef/.json) into the matching
// engine asset(s) inside a BundleWriter. Both the `phxpack` assembler and the per-format
// converter front-ends (phxsprite/phxtile/phxsnd/phxbin) call these, so there is exactly ONE
// bake path (docs/08 §9 — "keeps the bake path single and testable").
#ifndef PHX_TOOLS_BUILDERS_H
#define PHX_TOOLS_BUILDERS_H

#include "bundle_writer.h"
#include "png.h"
#include "tiled.h"
#include "wav.h"
#include "json.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace phxtool {

// ---- path helpers ---------------------------------------------------------
inline std::string stem(const std::string& path) {
    size_t s = path.find_last_of("/\\");
    size_t d = path.find_last_of('.');
    size_t b = (s == std::string::npos) ? 0 : s + 1;
    size_t e = (d == std::string::npos || d < b) ? path.size() : d;
    return path.substr(b, e - b);
}
inline std::string dir_of(const std::string& path) {
    size_t s = path.find_last_of("/\\");
    return s == std::string::npos ? std::string() : path.substr(0, s + 1);
}
inline bool ends_with(const std::string& s, const char* suf) {
    size_t n = std::strlen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

inline bool read_file(const std::string& path, std::vector<uint8_t>& out) {
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

// ---- textures: PNG / PPM --------------------------------------------------
inline bool build_png(BundleWriter& w, const std::string& in, const std::string& name = "") {
    std::vector<uint8_t> bytes;
    if (!read_file(in, bytes)) { std::fprintf(stderr, "phx: cannot read '%s'\n", in.c_str()); return false; }
    std::vector<uint32_t> px; uint16_t iw, ih;
    if (!png_decode(bytes.data(), bytes.size(), px, iw, ih)) {
        std::fprintf(stderr, "phx: bad/unsupported PNG '%s'\n", in.c_str()); return false; }
    const std::string nm = name.empty() ? stem(in) : name;
    w.add_texture(nm, px.data(), iw, ih);
    std::printf("  + texture %-12s %dx%d  (%s, PNG)\n", nm.c_str(), iw, ih, in.c_str());
    return true;
}

inline bool load_ppm(const std::string& path, std::vector<uint32_t>& out, uint16_t& w, uint16_t& h) {
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
inline bool build_ppm(BundleWriter& w, const std::string& in, const std::string& name = "") {
    std::vector<uint32_t> px; uint16_t iw, ih;
    if (!load_ppm(in, px, iw, ih)) { std::fprintf(stderr, "phx: bad PPM '%s'\n", in.c_str()); return false; }
    const std::string nm = name.empty() ? stem(in) : name;
    w.add_texture(nm, px.data(), iw, ih);
    std::printf("  + texture %-12s %dx%d  (%s)\n", nm.c_str(), iw, ih, in.c_str());
    return true;
}

// ---- tilemaps: .tmcsv (stand-in) / Tiled .tmj -----------------------------
inline bool load_tmcsv(const std::string& path, std::vector<uint16_t>& idx,
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
    for (auto& row : rows)
        for (uint16_t x = 0; x < w; ++x) idx.push_back(x < row.size() ? row[x] : 0);
    return true;
}
inline bool build_tmcsv(BundleWriter& w, const std::string& in, const std::string& name = "") {
    std::vector<uint16_t> idx; uint16_t iw, ih; std::string ts; int tw, th;
    if (!load_tmcsv(in, idx, iw, ih, ts, tw, th)) { std::fprintf(stderr, "phx: bad tmcsv '%s'\n", in.c_str()); return false; }
    const std::string nm = name.empty() ? stem(in) : name;
    w.add_tilemap(nm, idx.data(), iw, ih, 1, uint8_t(tw), uint8_t(th), ts);
    std::printf("  + tilemap %-12s %dx%d tiles, tileset '%s'  (%s)\n", nm.c_str(), iw, ih, ts.c_str(), in.c_str());
    return true;
}

// Tiled .tmj -> Tilemap (+ Spawns). Both assets share `name` (matches the runtime lookup).
inline bool build_tmj(BundleWriter& w, const std::string& in, const std::string& name = "") {
    std::vector<uint8_t> bytes;
    if (!read_file(in, bytes)) { std::fprintf(stderr, "phx: cannot read '%s'\n", in.c_str()); return false; }
    TiledMap tm;
    std::string terr;
    if (!tiled_load(std::string(bytes.begin(), bytes.end()), tm, &terr)) {
        std::fprintf(stderr, "phx: bad Tiled map '%s': %s\n", in.c_str(), terr.c_str()); return false; }
    const std::string nm = name.empty() ? stem(in) : name;
    std::vector<uint16_t> flat;
    flat.reserve(tm.layers.size() * size_t(tm.width) * size_t(tm.height));
    for (const auto& L : tm.layers) flat.insert(flat.end(), L.begin(), L.end());
    w.add_tilemap(nm, flat.data(), uint16_t(tm.width), uint16_t(tm.height),
                  uint8_t(tm.layers.size()), uint8_t(tm.tile_w), uint8_t(tm.tile_h), tm.tileset,
                  tm.has_parallax() ? &tm.layer_parallax : nullptr,
                  tm.has_tile_flags() ? &tm.tile_flags : nullptr);
    std::printf("  + tilemap %-12s %dx%d tiles x%u layers%s%s, tileset '%s'  (%s, Tiled)\n",
                nm.c_str(), tm.width, tm.height, unsigned(tm.layers.size()),
                tm.has_parallax() ? " (parallax)" : "",
                tm.has_tile_flags() ? " (tile collision flags)" : "", tm.tileset.c_str(), in.c_str());
    if (!tm.spawns.empty()) {
        std::vector<phx::SpawnDef> sd;
        for (const auto& s : tm.spawns)
            sd.push_back(phx::SpawnDef{ phx::fnv1a((s.type.empty() ? s.name : s.type).c_str()),
                                        int16_t(s.x), int16_t(s.y), uint16_t(s.w), uint16_t(s.h) });
        w.add_spawns(nm, sd);
        std::printf("  + spawns  %-12s %u objects  (%s)\n", nm.c_str(), unsigned(sd.size()), in.c_str());
    }
    return true;
}

// ---- sound: WAV -> mono16 -------------------------------------------------
inline bool build_wav(BundleWriter& w, const std::string& in, const std::string& name = "") {
    std::vector<uint8_t> bytes;
    if (!read_file(in, bytes)) { std::fprintf(stderr, "phx: cannot read '%s'\n", in.c_str()); return false; }
    std::vector<int16_t> mono; uint32_t rate = 0;
    if (!wav_decode(bytes.data(), bytes.size(), mono, rate)) {
        std::fprintf(stderr, "phx: bad/unsupported WAV '%s'\n", in.c_str()); return false; }
    const std::string nm = name.empty() ? stem(in) : name;
    w.add_sound(nm, mono.data(), uint32_t(mono.size()), rate);
    std::printf("  + sound   %-12s %u frames @ %u Hz  (%s, WAV)\n",
                nm.c_str(), unsigned(mono.size()), rate, in.c_str());
    return true;
}

// ---- sprite sheets: .sprdef text OR a JSON sidecar ------------------------
struct SprDef {
    std::string sheet;                 // PNG path (resolved relative to the def's dir)
    int fw = 0, fh = 0;
    std::vector<phx::SpriteClipDef> clips;
};

inline bool load_sprdef(const std::string& path, SprDef& out) {
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
            out.clips.push_back(c);
        }
    }
    std::fclose(f);
    if (!out.sheet.empty() && out.sheet[0] != '/' && out.sheet.find('/') == std::string::npos)
        out.sheet = dir_of(path) + out.sheet;
    return !out.sheet.empty() && out.fw > 0 && out.fh > 0;
}

// JSON sidecar (docs/08 §3): { "image", "tile" | ["fw","fh"], "animations": { name: {frames,fps,loop} } }.
// `frames` is a list of frame indices; we bake it as a [first,count] run (contiguous), matching
// the SpriteClipDef model. Non-contiguous frame lists are rejected (caught offline).
inline bool load_sprjson(const std::string& path, SprDef& out, std::string* err = nullptr) {
    auto fail = [&](const std::string& why) { if (err) *err = why; return false; };
    std::vector<uint8_t> bytes;
    if (!read_file(path, bytes)) return fail("cannot read file");
    JsonValue root;
    std::string jerr;
    if (!JsonParser::parse(std::string(bytes.begin(), bytes.end()), root, &jerr))
        return fail("malformed JSON: " + jerr);
    if (!root.is_obj()) return fail("top level is not a JSON object");
    out.sheet = root.str_at("image");
    if (out.sheet.empty()) return fail("missing \"image\" (the sheet PNG path)");
    if (const JsonValue* t = root.find("tile")) { out.fw = out.fh = t->as_int(); }
    out.fw = root.int_at("frame_w", out.fw);
    out.fh = root.int_at("frame_h", out.fh);
    const JsonValue* anims = root.find("animations");
    if (anims && anims->is_obj()) {
        for (const auto& kv : anims->members) {
            const JsonValue& a = kv.second;
            const JsonValue* frames = a.find("frames");
            if (!frames || !frames->is_arr() || frames->arr.empty())
                return fail("animation '" + kv.first + "' needs a non-empty \"frames\" array");
            int first = frames->arr.front().as_int();
            for (size_t k = 0; k < frames->arr.size(); ++k)        // require a contiguous run
                if (frames->arr[k].as_int() != first + int(k))
                    return fail("animation '" + kv.first + "': \"frames\" must be a contiguous run "
                                "(e.g. [4,5,6]) — the baked clip model is [first,count]");
            phx::SpriteClipDef c{};
            c.name  = phx::fnv1a(kv.first.c_str());
            c.first = uint16_t(first);
            c.count = uint16_t(frames->arr.size());
            c.fps   = uint8_t(a.int_at("fps", 0));
            c.loop  = uint8_t(a.find("loop") && a.find("loop")->boolean ? 1 : 0);
            out.clips.push_back(c);
        }
    }
    if (!out.sheet.empty() && out.sheet[0] != '/' && out.sheet.find('/') == std::string::npos)
        out.sheet = dir_of(path) + out.sheet;
    if (out.fw <= 0 || out.fh <= 0)
        return fail("missing/invalid frame size (\"tile\" or \"frame_w\"/\"frame_h\" must be > 0)");
    return true;
}

// Bakes the sheet texture + the Sprite metadata. The sprite asset is named `name` (default: the
// def's stem); the texture is named after the sheet PNG's stem (so several sprites can share it).
inline bool build_sprite(BundleWriter& w, const std::string& in, const std::string& name = "") {
    SprDef sd;
    std::string serr;
    const bool ok = ends_with(in, ".json") ? load_sprjson(in, sd, &serr) : load_sprdef(in, sd);
    if (!ok) {
        std::fprintf(stderr, "phx: bad sprite def '%s'%s%s\n", in.c_str(),
                     serr.empty() ? "" : ": ", serr.c_str());
        return false;
    }
    std::vector<uint8_t> bytes;
    if (!read_file(sd.sheet, bytes)) { std::fprintf(stderr, "phx: sprite def '%s' cannot read sheet '%s'\n", in.c_str(), sd.sheet.c_str()); return false; }
    std::vector<uint32_t> px; uint16_t iw, ih;
    if (!png_decode(bytes.data(), bytes.size(), px, iw, ih)) {
        std::fprintf(stderr, "phx: sprite def '%s' bad sheet PNG '%s'\n", in.c_str(), sd.sheet.c_str()); return false; }
    const std::string texname = stem(sd.sheet);
    const std::string nm = name.empty() ? stem(in) : name;
    w.add_texture(texname, px.data(), iw, ih);
    const uint16_t cols = uint16_t(iw / uint16_t(sd.fw));
    w.add_sprite(nm, texname, uint16_t(sd.fw), uint16_t(sd.fh), cols, sd.clips);
    std::printf("  + sprite  %-12s sheet '%s' %dx%d, %u clips  (%s)\n",
                nm.c_str(), texname.c_str(), sd.fw, sd.fh, unsigned(sd.clips.size()), in.c_str());
    return true;
}

// ---- data tables: JSON -> flat binary + generated accessor header (phxbin) ----
// Schema: { "struct": "<Name>", "fields": [ {"name","type"} ... ], "records": [ {field: value} ] }.
// Field types: u8/i8/u16/i16/u32/i32/f32, plus str8/str16/str32 — an inline NUL-terminated
// char[N] (values truncate to N-1). A string field names a record (a prefab's spawn type, an
// item's id string): the game hashes it (fnv1a) to match spawn types, and `phxtmap --prefabs`
// reads the same table as its placeable-entity vocabulary. Records are packed at natural C
// alignment so the generated POD struct matches the blob by construction (a static_assert in
// the header guards it).
struct BinField { std::string name, type; uint32_t size = 0, align = 0, offset = 0; };

// str8/str16/str32 -> 8/16/32; 0 for every other type.
inline uint32_t bin_str_size(const std::string& t) {
    if (t == "str8")  return 8;
    if (t == "str16") return 16;
    if (t == "str32") return 32;
    return 0;
}
inline bool bin_type_info(const std::string& t, uint32_t& size, uint32_t& align) {
    if (t == "u8"  || t == "i8")  { size = 1; align = 1; return true; }
    if (t == "u16" || t == "i16") { size = 2; align = 2; return true; }
    if (t == "u32" || t == "i32" || t == "f32") { size = 4; align = 4; return true; }
    if (const uint32_t n = bin_str_size(t)) { size = n; align = 1; return true; }
    return false;
}
inline const char* bin_ctype(const std::string& t) {
    if (t == "u8")  return "uint8_t";
    if (t == "i8")  return "int8_t";
    if (t == "u16") return "uint16_t";
    if (t == "i16") return "int16_t";
    if (t == "u32") return "uint32_t";
    if (t == "i32") return "int32_t";
    if (t == "f32") return "float";
    return "uint8_t";
}
inline void bin_write_field(std::vector<uint8_t>& rec, uint32_t off, const BinField& f, const JsonValue& v) {
    if (f.type == "f32") { float x = float(v.as_num()); std::memcpy(rec.data() + off, &x, 4); return; }
    if (bin_str_size(f.type)) {                       // char[N], always NUL-terminated
        const std::string& s = v.as_str();
        const uint32_t n = uint32_t(s.size()) < f.size - 1 ? uint32_t(s.size()) : f.size - 1;
        std::memcpy(rec.data() + off, s.data(), n);   // rec is zero-filled: rest stays NUL
        return;
    }
    int64_t n = int64_t(v.as_num());
    for (uint32_t b = 0; b < f.size; ++b) rec[off + b] = uint8_t((n >> (8 * b)) & 0xFF);  // little-endian
}

// Builds the table blob (named `name`) and, if `header_out` is non-empty, writes the .gen.h.
inline bool build_bin(BundleWriter& w, const std::string& in, const std::string& name = "",
                      const std::string& header_out = "") {
    std::vector<uint8_t> bytes;
    if (!read_file(in, bytes)) { std::fprintf(stderr, "phx: cannot read '%s'\n", in.c_str()); return false; }
    JsonValue root;
    std::string jerr;
    if (!JsonParser::parse(std::string(bytes.begin(), bytes.end()), root, &jerr)) {
        std::fprintf(stderr, "phx: malformed JSON '%s': %s\n", in.c_str(), jerr.c_str()); return false; }
    if (!root.is_obj()) {
        std::fprintf(stderr, "phx: '%s': top level is not a JSON object\n", in.c_str()); return false; }
    const std::string sname = root.str_at("struct");
    const JsonValue* fields = root.find("fields");
    const JsonValue* recs   = root.find("records");
    if (sname.empty() || !fields || !fields->is_arr() || fields->arr.empty()) {
        std::fprintf(stderr, "phx: '%s' needs \"struct\" + non-empty \"fields\"\n", in.c_str()); return false; }

    std::vector<BinField> fs;
    uint32_t off = 0, maxa = 1;
    for (const JsonValue& fv : fields->arr) {
        BinField f; f.name = fv.str_at("name"); f.type = fv.str_at("type");
        if (f.name.empty() || !bin_type_info(f.type, f.size, f.align)) {
            std::fprintf(stderr, "phx: '%s' bad field (name/type)\n", in.c_str()); return false; }
        off = (off + f.align - 1) & ~(f.align - 1);   // natural alignment
        f.offset = off; off += f.size;
        if (f.align > maxa) maxa = f.align;
        fs.push_back(f);
    }
    const uint32_t stride = (off + maxa - 1) & ~(maxa - 1);   // pad struct to its alignment

    const uint32_t count = (recs && recs->is_arr()) ? uint32_t(recs->arr.size()) : 0;
    std::vector<uint8_t> blob(8 + size_t(count) * stride, 0);
    std::memcpy(blob.data() + 0, &count, 4);
    std::memcpy(blob.data() + 4, &stride, 4);
    for (uint32_t r = 0; r < count; ++r) {
        std::vector<uint8_t> rec(stride, 0);
        const JsonValue& rv = recs->arr[r];
        for (const BinField& f : fs) if (const JsonValue* v = rv.find(f.name.c_str())) bin_write_field(rec, f.offset, f, *v);
        std::memcpy(blob.data() + 8 + size_t(r) * stride, rec.data(), stride);
    }
    const std::string nm = name.empty() ? stem(in) : name;
    w.add_blob(nm, blob.data(), uint32_t(blob.size()));
    std::printf("  + table   %-12s %u x %s (%u-byte stride)  (%s)\n",
                nm.c_str(), count, sname.c_str(), stride, in.c_str());

    if (!header_out.empty()) {
        FILE* h = std::fopen(header_out.c_str(), "wb");
        if (!h) { std::fprintf(stderr, "phx: cannot write header '%s'\n", header_out.c_str()); return false; }
        std::fprintf(h, "// generated by phxbin from %s — do not edit.\n#pragma once\n#include <cstdint>\n\n", in.c_str());
        std::fprintf(h, "namespace phxbin {\n\nstruct %s {\n", sname.c_str());
        for (const BinField& f : fs) {
            if (const uint32_t n = bin_str_size(f.type))
                std::fprintf(h, "    %-9s %s[%u];   // NUL-terminated\n", "char", f.name.c_str(), n);
            else
                std::fprintf(h, "    %-9s %s;\n", bin_ctype(f.type), f.name.c_str());
        }
        std::fprintf(h, "};\nstatic_assert(sizeof(%s) == %u, \"%s stride drifted from the baked blob\");\n\n",
                     sname.c_str(), stride, sname.c_str());
        std::fprintf(h, "// Blob layout: [uint32 count][uint32 stride][%s records[count]].\n", sname.c_str());
        std::fprintf(h, "struct %sTable {\n    uint32_t count;\n    uint32_t stride;\n    %s records[];\n};\n\n",
                     sname.c_str(), sname.c_str());
        std::fprintf(h, "} // namespace phxbin\n");
        std::fclose(h);
        std::printf("  + header  %s\n", header_out.c_str());
    }
    return true;
}

// ---- dispatch a source file by extension (used by the phxpack assembler) ---
inline bool build_from_source(BundleWriter& w, const std::string& in) {
    if (ends_with(in, ".png"))    return build_png(w, in);
    if (ends_with(in, ".ppm"))    return build_ppm(w, in);
    if (ends_with(in, ".tmcsv"))  return build_tmcsv(w, in);
    if (ends_with(in, ".tmj"))    return build_tmj(w, in);
    if (ends_with(in, ".wav"))    return build_wav(w, in);
    if (ends_with(in, ".sprdef")) return build_sprite(w, in);
    std::fprintf(stderr, "phx: unknown source type '%s'\n", in.c_str());
    return false;
}

} // namespace phxtool
#endif // PHX_TOOLS_BUILDERS_H
