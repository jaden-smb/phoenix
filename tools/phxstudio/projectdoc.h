// tools/phxstudio/projectdoc.h — a Phoenix GAME PROJECT and the rules for what Phoenix Studio may
// touch while one is open. Headless and unit-tested in the editors suite.
//
// A project is a folder holding a `phxproject.json`:
//
//   { "name": "Emberwing",
//     "source":  ["src"],                         // code folders (hints for the Explorer/templates)
//     "assets":  ["assets"],                      // author-asset folders (what `make game-assets` bakes)
//     "bundles": ["build/emberwing.phxp"],        // baked bundles the Assets view shows (may be ../..)
//     "launches": [ { "label": "Play", "group": "play", "command": "make -C \"$PHX_ROOT\" play PROJECT=\"$PHX_PROJECT\"",
//                     "blurb": "...", "needs": ["sdl2-config"], "windowed": true } ] }
//
// Launch commands run from the PROJECT folder with $PHX_ROOT (the Phoenix checkout) and
// $PHX_PROJECT (the project folder) exported. Paths in the file are relative to the project.
//
// The AccessPolicy is the "professional project" boundary: with a project open, the Studio may
// WRITE only inside the project folder, may READ (never edit) the engine's public API headers
// (engine/<module>/include/...) and its documentation, and may not open anything else — least of
// all the engine's own sources, tools or tests. Paths are canonicalised first, so `..` and
// symlinks cannot step outside. `--engine-dev` (engine maintenance) lifts the boundary.
// Host-only.
#ifndef PHX_TOOLS_PHXSTUDIO_PROJECTDOC_H
#define PHX_TOOLS_PHXSTUDIO_PROJECTDOC_H

#include "json.h"                  // tools/phxpack — the one JSON parser
#include "project.h"
#include "pixeldoc.h"
#include "../phxtmap/editor.h"     // TmapDoc (the template level)

#include <cstdio>
#include <string>
#include <system_error>
#include <vector>

namespace phxstudio {

// ---- paths ----
// Absolute, symlink-resolved (for the parts that exist), normalised, without a trailing '/'.
inline std::string canon_path(const std::string& p) {
    std::error_code ec;
    pfs::path a = pfs::absolute(p, ec);
    if (ec) return p;
    const pfs::path c = pfs::weakly_canonical(a, ec);
    std::string s = (ec ? a : c).lexically_normal().generic_string();
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}
// Is `p` the folder `dir` or inside it? (both canonical)
inline bool path_within(const std::string& p, const std::string& dir) {
    if (dir.empty()) return false;
    if (p == dir) return true;
    return p.size() > dir.size() && p.compare(0, dir.size(), dir) == 0 && p[dir.size()] == '/';
}

// ---- the access boundary ----
enum class Access : uint8_t { None, Read, Write };

struct AccessPolicy {
    bool engine_dev = false;     // engine maintenance: everything is editable
    std::string engine_root;     // canonical Phoenix checkout
    std::string project_dir;     // canonical project folder ("" = no project open)

    // The engine's public API: engine/<module>/include/**.h(pp)
    bool is_api_header(const std::string& c) const {
        const std::string eng = engine_root + "/engine";
        if (!path_within(c, eng)) return false;
        const std::string rel = c.substr(eng.size() + 1);             // <module>/include/...
        const size_t sl = rel.find('/');
        if (sl == std::string::npos || rel.compare(sl + 1, 8, "include/") != 0) return false;
        const std::string e = lower_ext(c);
        return e == ".h" || e == ".hpp";
    }
    // Documentation (never code): docs/*.md, README.md, and each tool's instructions.md.
    bool is_engine_doc(const std::string& c) const {
        if (c == engine_root + "/README.md") return true;
        if (path_within(c, engine_root + "/docs") && lower_ext(c) == ".md") return true;
        return path_within(c, engine_root + "/tools") && base_name(c) == "instructions.md";
    }
    Access access(const std::string& path) const {
        if (engine_dev) return Access::Write;
        const std::string c = canon_path(path);
        if (!project_dir.empty() && path_within(c, project_dir)) return Access::Write;
        if (is_api_header(c) || is_engine_doc(c)) return Access::Read;
        return Access::None;
    }
    // Why a folder can't be a project (empty = it can): it must not contain the engine and must
    // not sit inside the engine's own source folders.
    std::string project_dir_problem(const std::string& dir) const {
        const std::string c = canon_path(dir);
        if (path_within(engine_root, c)) return "a project can't contain the Phoenix engine itself";
        for (const char* sub : { "engine", "tools", "tests", "cmake", "docs", ".github" })
            if (path_within(c, engine_root + "/" + sub))
                return std::string("a project can't live inside the engine's ") + sub + "/ folder";
        return "";
    }
};

// ---- the project file ----
struct ProjectLaunch {
    std::string label, group = "play", command, blurb;
    std::vector<std::string> needs;
    bool windowed = false;
};

class ProjectDoc {
public:
    static constexpr const char* kFile = "phxproject.json";

    std::string dir;             // canonical project folder
    std::string name, description;
    std::vector<std::string> source{ "src" }, assets{ "assets" }, bundles;
    std::vector<ProjectLaunch> launches;

    std::string file() const { return dir + "/" + kFile; }
    static bool is_project_dir(const std::string& d) {
        std::error_code ec;
        return pfs::is_regular_file(pfs::path(d) / kFile, ec);
    }

    // Load from a project folder or its phxproject.json.
    static bool load(const std::string& dir_or_file, ProjectDoc& out, std::string* err = nullptr) {
        auto fail = [&](const std::string& why) { if (err) *err = why; return false; };
        std::string d = dir_or_file;
        if (base_name(d) == kFile) d = dir_name(d);
        if (d.empty()) d = ".";
        std::string text;
        {
            FILE* f = std::fopen((d + "/" + kFile).c_str(), "rb");
            if (!f) return fail("no " + std::string(kFile) + " in " + d);
            char buf[8192];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
            std::fclose(f);
        }
        return parse(text, canon_path(d), out, err);
    }
    static bool parse(const std::string& text, const std::string& dir, ProjectDoc& out, std::string* err = nullptr) {
        auto fail = [&](const std::string& why) { if (err) *err = std::string(kFile) + ": " + why; return false; };
        phxtool::JsonValue root;
        std::string jerr;
        if (!phxtool::JsonParser::parse(text, root, &jerr)) return fail("malformed JSON: " + jerr);
        if (!root.is_obj()) return fail("top level is not a JSON object");
        out = ProjectDoc{};
        out.dir = dir;
        out.name = root.str_at("name");
        if (out.name.empty()) out.name = base_name(dir);
        out.description = root.str_at("description");
        auto strings = [&](const char* key, std::vector<std::string>& v) {
            if (const phxtool::JsonValue* a = root.find(key); a && a->is_arr()) {
                v.clear();
                for (const phxtool::JsonValue& s : a->arr) if (!s.as_str().empty()) v.push_back(s.as_str());
            }
        };
        strings("source", out.source);
        strings("assets", out.assets);
        strings("bundles", out.bundles);
        if (const phxtool::JsonValue* ls = root.find("launches"); ls && ls->is_arr())
            for (const phxtool::JsonValue& l : ls->arr) {
                ProjectLaunch pl;
                pl.label = l.str_at("label");
                pl.command = l.str_at("command");
                if (pl.label.empty() || pl.command.empty()) return fail("every launch needs a \"label\" and a \"command\"");
                const std::string g = l.str_at("group");
                if (!g.empty()) pl.group = g;
                pl.blurb = l.str_at("blurb");
                if (const phxtool::JsonValue* nd = l.find("needs"); nd && nd->is_arr())
                    for (const phxtool::JsonValue& s : nd->arr) pl.needs.push_back(s.as_str());
                pl.windowed = l.find("windowed") && l.find("windowed")->boolean;
                out.launches.push_back(pl);
            }
        return true;
    }

    std::string to_json() const {
        auto q = [](const std::string& s) {
            std::string o = "\"";
            for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; }
            return o + "\"";
        };
        auto list = [&](const std::vector<std::string>& v) {
            std::string o = "[";
            for (size_t i = 0; i < v.size(); ++i) o += (i ? ", " : "") + q(v[i]);
            return o + "]";
        };
        std::string j = "{\n  \"name\": " + q(name) + ",\n";
        if (!description.empty()) j += "  \"description\": " + q(description) + ",\n";
        j += "  \"source\": " + list(source) + ",\n  \"assets\": " + list(assets) + ",\n  \"bundles\": " + list(bundles) + ",\n";
        j += "  \"launches\": [";
        for (size_t i = 0; i < launches.size(); ++i) {
            const ProjectLaunch& l = launches[i];
            j += i ? ",\n    { " : "\n    { ";
            j += "\"label\": " + q(l.label) + ", \"group\": " + q(l.group) + ",\n      \"command\": " + q(l.command);
            if (!l.blurb.empty()) j += ",\n      \"blurb\": " + q(l.blurb);
            if (!l.needs.empty()) j += ", \"needs\": " + list(l.needs);
            if (l.windowed) j += ", \"windowed\": true";
            j += " }";
        }
        j += launches.empty() ? "]\n}\n" : "\n  ]\n}\n";
        return j;
    }
    bool save(std::string* err = nullptr) const {
        const std::string t = to_json();
        FILE* f = std::fopen(file().c_str(), "wb");
        if (!f) { if (err) *err = "cannot write " + file(); return false; }
        const bool ok = std::fwrite(t.data(), 1, t.size(), f) == t.size();
        std::fclose(f);
        if (!ok && err) *err = "short write to " + file();
        return ok;
    }

    // The bundles to show (absolute): the listed ones, plus every .phxp in the project's folder
    // and its build/ folder.
    std::vector<std::string> bundle_paths() const {
        std::vector<std::string> out;
        for (const std::string& b : bundles) out.push_back(canon_path(b[0] == '/' ? b : dir + "/" + b));
        std::error_code ec;
        for (const pfs::path& d : { pfs::path(dir), pfs::path(dir) / "build" })
            for (pfs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec))
                if (it->is_regular_file(ec) && it->path().extension() == ".phxp") {
                    const std::string p = canon_path(it->path().string());
                    if (std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
                }
        return out;
    }

    // The standard launches of a project built by the generic rules (`make game|game-assets|play`).
    static std::vector<ProjectLaunch> standard_launches() {
        const std::string mk = "make -C \"$PHX_ROOT\" ";
        return {
            ProjectLaunch{ "Play", "play", mk + "-s play PROJECT=\"$PHX_PROJECT\"",
                           "Build the game, bake its assets and run it in a window", { "sdl2-config" }, true },
            ProjectLaunch{ "Build", "build", mk + "game PROJECT=\"$PHX_PROJECT\"",
                           "Compile src/*.cpp against the engine into build/<name>", { "sdl2-config" }, false },
            ProjectLaunch{ "Bake assets", "build", mk + "game-assets PROJECT=\"$PHX_PROJECT\"",
                           "Bake assets/ into build/<name>.phxp (sprites, maps, sounds, tables, images)", {}, false },
            ProjectLaunch{ "Bake for GBA", "build", mk + "game-assets PROJECT=\"$PHX_PROJECT\" TIER=0",
                           "The same bake encoded for the GBA PPU (tier 0): build/<name>.t0.phxp", {}, false },
        };
    }
};

// The shell command a project launch runs: from the project folder, with $PHX_ROOT (the engine
// checkout) and $PHX_PROJECT (the project) exported. The job runner itself starts in the engine
// root, so the `cd` is what makes relative paths in the command mean the project.
inline std::string launch_shell_command(const ProjectLaunch& l, const std::string& engine_root, const std::string& dir) {
    auto q = [](const std::string& s) {
        std::string o = "'";
        for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; }
        return o + "'";
    };
    return "export PHX_ROOT=" + q(engine_root) + " PHX_PROJECT=" + q(dir) + "; cd " + q(dir) + " && " + l.command;
}

// Project folders under <engine>/examples (one level) — the examples that ship as projects.
inline std::vector<std::string> discover_projects(const std::string& engine_root) {
    std::vector<std::string> out;
    std::error_code ec;
    for (pfs::directory_iterator it(pfs::path(engine_root) / "examples", ec), end; !ec && it != end; it.increment(ec))
        if (it->is_directory(ec) && ProjectDoc::is_project_dir(it->path().string())) out.push_back(canon_path(it->path().string()));
    std::sort(out.begin(), out.end());
    return out;
}

// A project name -> a folder / binary / bundle name (letters, digits, '_' and '-').
inline std::string project_slug(const std::string& name) {
    std::string o;
    for (char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-') o += c;
        else if (c >= 'A' && c <= 'Z') o += char(c - 'A' + 'a');
        else if (c == ' ') o += '_';
    }
    return o;
}

// ---- New project: a small, complete, buildable game ----
namespace tmpl {

inline const char* main_cpp() {
    return R"CPP(// src/main.cpp — @NAME@: a Phoenix game (made from the Phoenix Studio project template).
//
// One source for every target: gameplay code talks to the engine's public API only (phx/...),
// never to a platform header. Build + run from Phoenix Studio (Run > Play) or from the engine
// checkout:  make play PROJECT=path/to/this/project
//
// The assets in assets/ (hero sprite, tileset, level map) are baked into build/@SLUG@.phxp and
// read here zero-copy by name: "hero"_hash is the sprite baked from assets/hero.sprdef.
#include "phx/runtime/app.h"
#include "phx/resource/cache.h"
#include "phx/render/renderer.h"
#include "phx/input/input.h"
#include "phx/core/log.h"

using namespace phx;

namespace {

TextureId load_texture(ResourceCache& res, Renderer& r, NameHash name) {
    auto t = res.texture(name);
    if (!t) return kNoTexture;
    const TextureView v = t.unwrap();
    TextureDesc d{};
    d.pixels = v.pixels; d.width = v.width; d.height = v.height; d.format = v.format;
    return r.load_texture(d);
}

struct @TYPE@ final : Game {
    ResourceCache* res = nullptr;
    SpriteView hero{};
    TextureId hero_tex = kNoTexture;
    const SpriteClipDef* walk = nullptr;
    TilemapView level{};
    TilemapId map = kNoTilemap;
    vec2 pos{ s_from_int(40), s_from_int(112) };
    bool facing_left = false, moving = false;
    uint32_t ticks = 0;

    void on_start(App& app) override {
        res = ResourceCache::create(app.mem().persistent()).unwrap();
        if (res->mount(app.platform(), "build/@SLUG@.phxp") != Status::Ok) {
            PHX_LOG_ERROR("@SLUG@: no bundle - bake the assets first (Run > Bake assets)");
            return;
        }
        Renderer& r = app.render();
        if (auto s = res->sprite("hero"_hash)) {
            hero = s.unwrap();
            hero_tex = load_texture(*res, r, hero.texture);
            for (uint16_t i = 0; i < hero.clip_count; ++i)
                if (hero.clips[i].name == "walk"_hash) walk = &hero.clips[i];
        }
        if (auto m = res->tilemap("level"_hash)) {
            level = m.unwrap();
            TilemapDesc d{};
            d.indices = level.indices; d.width = level.width; d.height = level.height;
            d.layers = level.layers; d.tile_w = level.tile_w; d.tile_h = level.tile_h;
            d.tileset = load_texture(*res, r, level.tileset);
            map = r.upload_tilemap(d);
        }
    }

    void on_fixed_update(App& app, scalar) override {
        const InputState& in = app.input();
        const scalar speed = s_from_int(1);
        moving = false;
        if (in.down(Button::Left))  { pos.x = pos.x - speed; facing_left = true;  moving = true; }
        if (in.down(Button::Right)) { pos.x = pos.x + speed; facing_left = false; moving = true; }
        if (in.down(Button::Up))    { pos.y = pos.y - speed; moving = true; }
        if (in.down(Button::Down))  { pos.y = pos.y + speed; moving = true; }
        ++ticks;
    }

    void on_render(App& app, scalar) override {
        Renderer& r = app.render();
        r.begin_frame(Camera2D{});
        if (map != kNoTilemap)
            for (uint8_t l = 0; l < level.layers; ++l) r.draw_tilemap(map, l);
        if (hero_tex != kNoTexture && hero.cols) {
            int frame = walk ? walk->first : 0;
            if (walk && moving && walk->fps && walk->count)
                frame += int(ticks * walk->fps / 60u % walk->count);
            DrawSprite s{};
            s.tex = hero_tex;
            s.sx = int16_t((frame % hero.cols) * hero.frame_w);
            s.sy = int16_t((frame / hero.cols) * hero.frame_h);
            s.sw = int16_t(hero.frame_w);
            s.sh = int16_t(hero.frame_h);
            s.pos = pos;
            s.flags = facing_left ? uint16_t(kFlipX) : uint16_t(0);
            s.layer = 10;
            r.draw_sprite(s);
        }
        r.end_frame();
    }
};

} // namespace

int main() {
    Config cfg = Config::from_defaults();        // budgets from the target's capability tier
    cfg.title  = "@NAME@";
    cfg.width  = 240;                            // the GBA's screen: it fits every target
    cfg.height = 160;
    cfg.sim_hz = 60;
    App app(cfg);
    @TYPE@ game;
    return app.run(&game);
}
)CPP";
}

inline std::string replace_all(std::string s, const std::string& a, const std::string& b) {
    for (size_t p = s.find(a); p != std::string::npos; p = s.find(a, p + b.size())) s.replace(p, a.size(), b);
    return s;
}

// The tileset: 8 tiles of 8x8 (grass, dirt, brick, plank, spikes, cloud, bush, flower).
inline PixelDoc tileset() {
    PixelDoc t = PixelDoc::blank(64, 8, 0);
    const uint32_t grass = px_rgba(88, 176, 72), grass_d = px_rgba(52, 120, 52), dirt = px_rgba(140, 92, 56),
                   dirt_d = px_rgba(104, 66, 40), brick = px_rgba(150, 150, 170), mortar = px_rgba(96, 96, 116),
                   wood = px_rgba(190, 136, 76), wood_d = px_rgba(130, 86, 44), spike = px_rgba(210, 210, 224),
                   cloud = px_rgba(236, 240, 255), leaf = px_rgba(64, 150, 70), petal = px_rgba(240, 110, 150);
    t.rect(0, 0, 7, 7, dirt, true);  t.rect(0, 0, 7, 2, grass, true);  t.set(2, 3, grass_d); t.set(6, 3, grass_d);
    t.set(3, 5, dirt_d); t.set(6, 6, dirt_d);
    t.rect(8, 0, 15, 7, dirt, true); t.set(9, 2, dirt_d); t.set(13, 4, dirt_d); t.set(11, 6, dirt_d);
    t.rect(16, 0, 23, 7, brick, true); t.rect(16, 3, 23, 3, mortar, true); t.rect(16, 7, 23, 7, mortar, true);
    t.set(19, 0, mortar); t.set(19, 1, mortar); t.set(19, 2, mortar); t.set(21, 4, mortar); t.set(21, 5, mortar); t.set(21, 6, mortar);
    t.rect(24, 0, 31, 2, wood, true); t.rect(24, 2, 31, 2, wood_d, true); t.set(25, 3, wood_d); t.set(30, 3, wood_d);
    for (int k = 0; k < 4; ++k) { t.line(32 + k * 2, 7, 32 + k * 2 + 1, 3, spike); t.set(32 + k * 2 + 1, 7, spike); }
    t.ellipse(40, 2, 47, 7, cloud, true); t.ellipse(42, 0, 46, 5, cloud, true);
    t.ellipse(48, 2, 55, 7, leaf, true); t.set(50, 4, grass_d); t.set(53, 5, grass_d);
    t.line(59, 3, 59, 7, leaf); t.rect(58, 1, 60, 3, petal, true); t.set(59, 2, px_rgba(250, 220, 90));
    return t;
}

// The hero: 4 frames of 16x16 (a walk cycle; frame 0 doubles as the idle pose).
inline PixelDoc hero() {
    PixelDoc h = PixelDoc::blank(64, 16, 0);
    const uint32_t body = px_rgba(255, 138, 48), dark = px_rgba(170, 70, 30), eye = px_rgba(250, 250, 250),
                   pupil = px_rgba(30, 20, 30), foot = px_rgba(120, 50, 30);
    for (int f = 0; f < 4; ++f) {
        const int x = f * 16, bob = (f % 2) ? 1 : 0;
        h.ellipse(x + 3, 2 + bob, x + 12, 12 + bob, body, true);
        h.ellipse(x + 3, 2 + bob, x + 12, 12 + bob, dark, false);
        h.rect(x + 8, 5 + bob, x + 10, 7 + bob, eye, true);
        h.set(x + 10, 6 + bob, pupil);
        h.set(x + 5, 1 + bob, body); h.set(x + 6, 0 + bob, body);          // a little crest
        const int step = (f == 1) ? 2 : (f == 3) ? -2 : 0;
        h.rect(x + 5 + step, 13, x + 6 + step, 15, foot, true);
        h.rect(x + 9 - step, 13, x + 10 - step, 15, foot, true);
    }
    return h;
}

// The level: 30x20 tiles of 8x8 (one 240x160 screen), a backdrop layer + the gameplay layer.
inline phxtool::TmapDoc level() {
    phxtool::TmapDoc d = phxtool::TmapDoc::blank(30, 20, 8, 8, "tiles");
    d.tileset_image = "tiles.png";
    d.tileset_cols = 8; d.tileset_count = 8; d.tileset_img_w = 64; d.tileset_img_h = 8;
    d.layer_names[0] = "back";
    d.layer_parallax[0] = { 0.5, 1.0 };
    d.add_layer("main");
    // clouds + bushes behind
    for (int x : { 3, 11, 20, 26 }) { d.set_tile(0, x, 3 + (x % 3), 6); d.set_tile(0, x + 1, 3 + (x % 3), 6); }
    for (int x : { 6, 14, 23 }) d.set_tile(0, x, 16, 7);
    // ground, a raised block, a plank platform, spikes, flowers
    for (int x = 0; x < 30; ++x) { d.set_tile(1, x, 17, 1); d.set_tile(1, x, 18, 2); d.set_tile(1, x, 19, 2); }
    for (int x = 18; x < 22; ++x) { d.set_tile(1, x, 15, 3); d.set_tile(1, x, 16, 3); }
    for (int x = 8; x < 13; ++x) d.set_tile(1, x, 12, 4);
    d.set_tile(1, 25, 16, 5); d.set_tile(1, 26, 16, 5);
    d.set_tile(1, 3, 16, 8); d.set_tile(1, 15, 16, 8);
    d.set_tile_flag(1, phx::kTileFlagSolid); d.set_tile_flag(2, phx::kTileFlagSolid); d.set_tile_flag(3, phx::kTileFlagSolid);
    d.set_tile_flag(4, phx::kTileFlagOneWay); d.set_tile_flag(5, phx::kTileFlagHazard);
    d.add_spawn("player", 40, 120);
    d.spawns.back().name = "player";
    d.add_spawn("coin", 80, 80);
    d.spawns.back().name = "coin";
    return d;
}

} // namespace tmpl

// Create a new project folder `dir` named `name` from the template: phxproject.json, src/main.cpp,
// assets/ (hero sprite, tileset, level map), README.md. Refuses a non-empty existing folder.
inline bool create_project(const std::string& dir, const std::string& name, std::string* err = nullptr) {
    auto fail = [&](const std::string& why) { if (err) *err = why; return false; };
    const std::string slug = project_slug(name.empty() ? base_name(dir) : name);
    if (slug.empty()) return fail("the project needs a name made of letters or digits");
    std::error_code ec;
    if (pfs::exists(dir, ec) && !pfs::is_empty(dir, ec)) return fail(dir + " already exists and is not empty");
    pfs::create_directories(pfs::path(dir) / "src", ec);
    pfs::create_directories(pfs::path(dir) / "assets", ec);
    if (ec) return fail("cannot create " + dir + ": " + ec.message());
    auto write = [&](const std::string& rel, const std::string& text) {
        FILE* f = std::fopen((dir + "/" + rel).c_str(), "wb");
        if (!f) return false;
        const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
        std::fclose(f);
        return ok;
    };
    std::string type;                            // a C++ type name for the game: MyGameGame
    bool up = true;
    for (char c : slug) {
        if (c == '_' || c == '-') { up = true; continue; }
        type += up && c >= 'a' && c <= 'z' ? char(c - 'a' + 'A') : c;
        up = false;
    }
    if (type.empty() || (type[0] >= '0' && type[0] <= '9')) type = "G" + type;
    type += "Game";
    std::string main_cpp = tmpl::replace_all(tmpl::main_cpp(), "@NAME@", name.empty() ? slug : name);
    main_cpp = tmpl::replace_all(main_cpp, "@SLUG@", slug);
    main_cpp = tmpl::replace_all(main_cpp, "@TYPE@", type);
    if (!write("src/main.cpp", main_cpp)) return fail("cannot write src/main.cpp");

    std::string e;
    PixelDoc ts = tmpl::tileset();
    if (!ts.save_png(dir + "/assets/tiles.png", &e)) return fail(e);
    PixelDoc hero = tmpl::hero();
    if (!hero.save_png(dir + "/assets/hero.png", &e)) return fail(e);
    SprDoc spr;
    spr.sheet = "hero.png"; spr.frame_w = 16; spr.frame_h = 16;
    spr.clips = { SprClip{ "idle", 0, 1, 1, true }, SprClip{ "walk", 0, 4, 8, true } };
    if (!spr.save(dir + "/assets/hero.sprdef", &e)) return fail(e);
    phxtool::TmapDoc lvl = tmpl::level();
    if (!lvl.save_file(dir + "/assets/level.tmj")) return fail("cannot write assets/level.tmj");

    ProjectDoc p;
    p.dir = canon_path(dir);
    p.name = name.empty() ? slug : name;
    p.description = "A Phoenix game";
    p.bundles = { "build/" + slug + ".phxp" };
    p.launches = ProjectDoc::standard_launches();
    if (!p.save(&e)) return fail(e);
    const std::string readme =
        "# " + p.name + "\n\n"
        "A Phoenix game project (open it in Phoenix Studio: `phxstudio --project " + dir + "`).\n\n"
        "| Folder | What lives there |\n|---|---|\n"
        "| `src/` | the game's C++ (it uses the engine's public API, `phx/...`, only) |\n"
        "| `assets/` | author files: sprites (`.png` + `.sprdef`), maps (`.tmj`), sounds (`.wav`), data tables (`.json`) |\n"
        "| `build/` | what the build makes: the game (`build/" + slug + "`) and its bundle (`build/" + slug + ".phxp`) |\n\n"
        "From the Phoenix checkout: `make play PROJECT=" + dir + "` builds, bakes and runs it;\n"
        "`make game` / `make game-assets` do one step each.\n";
    if (!write("README.md", readme)) return fail("cannot write README.md");
    return true;
}

} // namespace phxstudio
#endif // PHX_TOOLS_PHXSTUDIO_PROJECTDOC_H
