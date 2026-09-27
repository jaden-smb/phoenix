// tools/phxstudio/main.cpp — Phoenix Studio: the one editor for the whole engine. A graphical hub
// over the repository, built (like the games) ON the engine itself — the same App loop, SDL
// window, software golden renderer and phx::ui primitives (docs/gui-editor-feasibility.md,
// Option A), with the tool widget kit (tools/common/twk.h) on top. Four views:
//
//   OVERVIEW  the module dependency graph exactly as built (layers from depcheck.py, edges from
//             real #includes) and the GBA/PSP/PC capability tiers parsed from caps.h.
//   ASSETS    every .phxp in the tree, validated like ResourceCache::mount, with live previews
//             through the real renderer (textures per render tier, sprites, tilemaps with
//             parallax/collision/spawns, sounds on the real mixer, spawn tables, blob hex).
//   EDITOR    an Explorer over the repo + tabs of documents, each in its own editor: CODE (a
//             syntax-highlighted text editor), SPRITE / PIXEL (paint PNG sheets, edit frames and
//             named clips with a live preview), TILEMAP (Tiled .tmj with the real tileset,
//             layers, collision, spawns, parallax) and DATA TABLE (phxbin records / prefabs).
//             New sprites, maps, tables, images and files from templates; quick open (Ctrl+P).
//   RUN       one-click games, editors, gates, every `make check` suite and the console builds,
//             run as child processes with live, colour-classified output; compiler errors are
//             clickable and show up in the code editor's gutter.
//
//   phxstudio [--root DIR] [--scale N] [--tab overview|assets|editor|run] [--bundle FILE.phxp]
//             [--asset NAME] [--run TARGET]... [--open FILE]... [--fresh]
//             [--shot OUT.ppm [--shot-frame N]] [--script "CMD; CMD..." | FILE]
//
// The document logic (model.h, textdoc.h, syntax.h, pixeldoc.h, project.h, the map/table models)
// is headless and unit-tested in the editors + pipeline suites; the editor panels are ed_*.cpp,
// the workspace is workspace.cpp, process control is jobs.h. No SDL header is included here: the
// window/keyboard/clipboard extras come through phx/platform/desktop.h, and the audio/readback
// hooks are the SDL backend's extern "C" extension symbols (the emberwing precedent).
#include "phx/runtime/app.h"
#include "phx/ui/ui.h"
#include "phx/render/renderer.h"
#include "phx/input/input.h"
#include "phx/audio/mixer.h"
#include "phx/audio/command_queue.h"
#include "phx/core/version.h"
#include "phx/platform/platform.h"
#include "phx/platform/desktop.h"

#include "twk.h"          // tools/common: the tool widget kit
#include "model.h"
#include "jobs.h"
#include "host.h"
#include "workspace.h"
#include "../phxentity/editor.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

extern "C" int  phx_sdl_audio_start(int rate, void (*fill)(void*, int16_t*, int), void* user);
extern "C" void phx_sdl_audio_stop(void);
extern "C" int  phx_sdl_readback(uint32_t* out, int lw, int lh);

using namespace phx;
using namespace phxstudio;

namespace {

// ---- canvas + type metrics -------------------------------------------------------------------
// The logical canvas FOLLOWS the window (resizable; the SDL backend re-derives the framebuffer from
// the window size at the integer UI scale). 640x360 is the starting size (a 1280x720 window at 2x).
int kW = 640, kH = 360;
constexpr int kTopH = 18, kBotH = 12;
constexpr int kBodyY = kTopH;
int kBodyH = kH - kTopH - kBotH;
constexpr int kAdv = 6, kLineH = 10;            // 5x7 glyphs on a 6px pitch
constexpr int kMixRate = 44100;

// ---- sprite layers (tilemaps always draw beneath every sprite) --------------------------------
enum : uint8_t {
    kLyCover = 120,   // opaque background around the tilemap preview hole
    kLyEdge = 140, kLyPanel = 150, kLyWidget = 160, kLyImage = 170, kLyOver = 180,
    kLyText = 200, kLyBar = 230, kLyBarText = 240,
};

namespace pal {
const Rgba bg     = rgba(30, 30, 46);     // == the soft backend's clear colour
const Rgba bar    = rgba(19, 19, 29);
const Rgba panel  = rgba(40, 41, 58);
const Rgba panel2 = rgba(50, 52, 72);
const Rgba hover  = rgba(68, 70, 98);
const Rgba line   = rgba(72, 74, 100);
const Rgba text   = rgba(230, 230, 240);
const Rgba dim    = rgba(146, 148, 172);
const Rgba faint  = rgba(96, 98, 124);
const Rgba accent = rgba(255, 138, 48);   // phoenix orange
const Rgba good   = rgba(96, 206, 126);
const Rgba bad    = rgba(240, 86, 76);
const Rgba warn   = rgba(238, 194, 76);
const Rgba info   = rgba(112, 174, 246);
const Rgba violet = rgba(182, 134, 242);
} // namespace pal

Rgba scale_rgb(Rgba c, int num, int den) {
    auto ch = [&](uint32_t v) { return uint8_t(std::min<uint32_t>(255u, v * uint32_t(num) / uint32_t(den))); };
    return rgba(ch(rgba_r(c)), ch(rgba_g(c)), ch(rgba_b(c)));
}

Rgba layer_colour(int l) {
    static const Rgba k[5] = { rgba(140, 148, 178), rgba(104, 164, 238), rgba(84, 198, 166),
                               rgba(234, 178, 86), rgba(242, 114, 94) };
    return k[std::max(0, std::min(4, l))];
}

Rgba type_colour(AssetType t) {
    switch (t) {
    case AssetType::Texture: return pal::info;
    case AssetType::Tilemap: return pal::good;
    case AssetType::Sprite:  return pal::accent;
    case AssetType::Sound:   return pal::violet;
    case AssetType::Spawns:  return pal::warn;
    case AssetType::Font:    return rgba(96, 212, 220);
    case AssetType::Blob:    return pal::dim;
    }
    return pal::dim;
}

Rgba tone_colour(Tone t) {
    switch (t) {
    case Tone::Dim:  return pal::faint;
    case Tone::Good: return pal::good;
    case Tone::Bad:  return pal::bad;
    case Tone::Warn: return pal::warn;
    case Tone::Info: return pal::info;
    case Tone::Cmd:  return pal::accent;
    default:         return pal::text;
    }
}

std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* f, ...) {
    char b[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(b, sizeof(b), f, ap);
    va_end(ap);
    return b;
}

// ============================================================================================
// Gui — the Overview / Assets / Run views' drawing vocabulary, as a thin adapter over the tool
// widget kit (twk::Gui). Those views were written against absolute sprite layers (kLy*); the
// adapter maps them onto twk's sub-layers so they share hit-testing, popups and modals with the
// Editor workspace and the menu bar (a menu open over a view blocks the view beneath it).
// ============================================================================================
//
// Sprites are CAMERA-RELATIVE in the renderer, and the tilemap preview moves the camera to use the
// real parallax path, so twk adds the camera back to every draw (its ox/oy): chrome stays put
// while the map scrolls under it.
struct Gui {
    twk::Gui* g = nullptr;
    int  mx = -1, my = -1;
    bool down = false;
    std::string hint;            // status-bar help for whatever is hovered this frame

    void sync() {
        mx = g->mx(); my = g->my();
        down = (g->in->held & twk::kMouseL) != 0;
        hint.clear();
    }
    void flush_hint() { if (!hint.empty()) g->hint = hint; }

    static twk::Rect tr(const Rect& r) { return twk::Rect{ r.x, r.y, r.w, r.h }; }
    // kLy* (absolute, 120..240) -> twk sub-layer (monotonic, so relative order is preserved).
    static uint8_t sub(uint8_t ly) {
        if (ly < kLyEdge)   return twk::kSubBg;
        if (ly < kLyPanel)  return uint8_t(twk::kSubBg + 1);
        if (ly < kLyWidget) return twk::kSubFill;
        if (ly < kLyImage)  return uint8_t(twk::kSubWidget + (ly > kLyWidget ? 1 : 0));
        if (ly < kLyOver)   return uint8_t(twk::kSubImage + (ly > kLyImage ? 1 : 0));
        if (ly < kLyText)   return uint8_t(twk::kSubOver + (ly > kLyOver ? 1 : 0));
        if (ly < kLyBar)    return uint8_t(twk::kSubText + (ly > kLyText ? 1 : 0));
        return twk::kSubTop;
    }
    static vec2 v2(int x, int y) { return vec2{ s_from_int(x), s_from_int(y) }; }

    void rect(const Rect& r, Rgba c, uint8_t layer = kLyPanel) { g->rect(tr(r), c, sub(layer)); }
    void frame(const Rect& r, Rgba c, uint8_t layer = kLyWidget) { g->frame_rect(tr(r), c, sub(layer)); }
    void image(const Rect& dst, TextureId t, int sx, int sy, int sw, int sh,
               Rgba tint = rgba(255, 255, 255), uint8_t layer = kLyImage) {
        g->image(tr(dst), t, sx, sy, sw, sh, tint, sub(layer));
    }
    int text(int x, int y, const std::string& s, Rgba c, uint8_t layer = kLyText, int max_w = 1 << 20, int scale = 1) {
        return g->text(x, y, s, c, sub(layer), max_w >= (1 << 20) ? INT_MAX : max_w, scale);
    }
    static int text_w(const std::string& s, int scale = 1) { return twk::Gui::text_w(s, scale); }
    void text_center(const Rect& r, const std::string& s, Rgba c, uint8_t layer = kLyText) { g->text_center(tr(r), s, c, sub(layer)); }
    bool hover(const Rect& r) const { return g->hover(tr(r)); }
    bool clicked(const Rect& r) { return g->clicked(tr(r)); }
    bool button(const Rect& r, const std::string& label, bool on = false, bool enabled = true,
                const std::string& help = "", Rgba tint = pal::accent) {
        twk::Btn b;
        b.on = on; b.enabled = enabled; b.tint = tint;
        b.help = help.empty() ? nullptr : help.c_str();
        return g->button(tr(r), label, b);
    }
    bool scrollbar(int id, const Rect& track, int count, int visible, int& scroll) {
        g->wheel_scroll(twk::Rect{ track.x - 400, track.y, 400 + track.w, track.h }, scroll, count, visible);
        return g->scrollbar_v(0x5C0B0000u + uint32_t(id), tr(track), count, visible, scroll);
    }
    void line_px(int x0, int y0, int x1, int y1, Rgba c, uint8_t layer) { g->line(x0, y0, x1, y1, c, sub(layer)); }
};

// ---- the app icon: a tiny flame, drawn from an ASCII grid (tools draw their own art) ---------
const char* const kFlame[12] = {
    ".....o......",
    ".....oo.....",
    "....oyo.....",
    "...oyyo..o..",
    "...oyyoo.oo.",
    "..oyywyooyo.",
    ".ooyywwyoyo.",
    ".oyywwwyyoo.",
    ".oyywwwwyyo.",
    "..oyywwwyo..",
    "...ooyyyo...",
    ".....ooo....",
};

// ---- audio device glue (the SDL audio thread drains our intents into the mixer) -------------
struct AudioGlue { AudioMixer* mixer; AudioCommandQueue* queue; };
void audio_fill(void* user, int16_t* out, int frames) {
    AudioGlue* g = static_cast<AudioGlue*>(user);
    g->queue->drain(*g->mixer);
    g->mixer->mix(out, uint32_t(frames));
}

enum class Tab : uint8_t { Overview, Assets, Editor, Run, Count };
const char* const kTabNames[] = { "Overview", "Assets", "Editor", "Run" };

// A console display line (a LogLine wrapped to the panel width). `diag` >= 0 indexes the parsed
// compiler diagnostic it came from (clicking the line opens the file there).
struct ConLine { std::string text; Tone tone; int diag = -1; };

struct StudioGame;

// The Host the editor panels talk to (host.h), forwarding into the StudioGame (a separate object
// so the game's own members — `root`, `ticks` — don't collide with the interface's methods).
struct StudioHost final : Host {
    StudioGame* s = nullptr;
    twk::Gui& gui() override;
    phx::Renderer& renderer() override;
    const std::string& root() const override;
    Access access(const std::string& path_abs) override;
    std::vector<std::pair<std::string, std::string>> api_dirs() override;
    std::string project_name() const override;
    uint64_t ticks() const override;
    void toast(const std::string& msg, Toast t) override;
    void modal(const std::string& title, int w, int h, std::function<bool(twk::Gui&, twk::Rect)> body) override;
    bool modal_open() const override;
    void open_file(const std::string& path_abs, int line, int col) override;
    void open_tile(const std::string& path_abs, int index, int tw, int th) override;
    bool can_open_files() const override { return true; }
    std::vector<std::string> prefab_types() override;
    std::vector<std::string> files_with(const std::vector<std::string>& exts) override;
    bool run_make(const std::string& target) override;
    bool play_from(int x, int y) override;
    void file_saved(const std::string& path_abs) override;
    std::vector<Diag> diagnostics_for(const std::string& path_abs) override;
};

// ============================================================================================
// StudioGame
// ============================================================================================
struct StudioGame final : Game {
    std::string root;
    Tab tab = Tab::Editor;
    std::string initial_bundle;
    std::string initial_asset;
    std::vector<std::string> initial_runs;            // --run: queue these launches at startup
    std::vector<std::string> initial_opens;           // --open: documents to open at startup
    // Project mode (the default): the Studio edits ONE game project's folder; the engine's public
    // API is readable, nothing else of the engine is. --engine-dev opens the engine checkout itself.
    bool engine_dev = false;
    std::string project_arg;                          // --project DIR
    ProjectDoc project;
    bool has_project = false;
    AccessPolicy policy;
    std::string ws_root;                              // the workspace root: the project (or the engine)
    std::vector<std::string> recent_projects;
    bool fresh = false;                               // --fresh: don't restore the last session
    bool auto_size = true;                            // pick the window size from the display
    struct ScriptCmd { std::string op, arg, rest; int a = 0, b = 0, c = 0, d = 0, e = 0; };
    std::vector<ScriptCmd> script;                    // --script: "click X Y; wait N; shot F; quit"
    size_t script_pc = 0;
    int  script_wait = 0;
    // scripted pointer (overrides the real mouse while a script runs)
    int  sc_x = -1, sc_y = -1;
    uint32_t sc_held = 0;
    std::vector<std::function<void(twk::Input&)>> sc_frames;   // per-frame queued input
    std::string pending_shot;
    std::string shot_path;                            // --shot: write the frame, then quit
    int shot_frame = 45;

    // model
    EngineMap engine;
    std::string engine_err;
    std::vector<TierCaps> tiers;
    NameBook names;
    std::string makefile;
    std::vector<Launch> launches;
    std::vector<std::string> launch_missing;          // per launch: first missing need
    std::unique_ptr<JobRunner> jobs;

    // gui
    twk::Gui tg;                                      // the widget kit (every view + the chrome)
    twk::Input tin;                                   // this frame's pointer + keyboard
    Gui gui;                                          // legacy-view adapter over tg
    StudioHost host;
    Workspace ws;
    App* app_ = nullptr;
    Renderer* renderer = nullptr;                     // the App's (lives as long as the app)
    struct Modal { std::string title; int w, h; std::function<bool(twk::Gui&, twk::Rect)> body; };
    std::vector<Modal> modals;                        // a stack; only the top one is live
    struct ToastMsg { std::string text; Toast tone; uint64_t until; };
    std::vector<ToastMsg> toasts;
    std::vector<Diag> diags;                          // compiler diagnostics of the current build
    std::vector<std::string> prefab_cache;
    bool prefab_dirty = true;
    std::string title_shown;
    bool quitting = false;
    TextureId flame_tex = kNoTexture;
    uint64_t ticks = 0;                               // fixed steps (animation clock, 60 Hz)
    RenderStats last_stats{};
    ProfileAvg render_avg;
    uint64_t frames_rendered = 0, fps_tick = 0;
    unsigned fps = 0;
    int key_hold = 0;
    static uint32_t g_flame_px[12 * 12];

    // overview
    int mod_sel = -1;

    // assets
    std::vector<std::string> bundle_paths;
    int bundle_sel = -1, bundle_scroll = 0;
    std::map<std::string, std::unique_ptr<BundleDoc>> docs;   // cache: keeps blobs alive (zero-copy)
    BundleDoc* doc = nullptr;
    std::vector<int> order;                           // asset indices sorted by type, then name
    int asset_sel = -1, asset_scroll = 0;
    bool wide = false;                                // preview takes the whole body

    // texture preview
    int tex_asset = -1;
    std::vector<uint32_t> tex_rgba;
    TierReport tex_rep;
    int tex_tier = 2;
    TextureId tex_id = kNoTexture;
    bool tex_grid = false;

    // sprite preview
    int spr_asset = -1;
    SpriteInfo spr;
    TextureId spr_tex = kNoTexture;
    uint16_t spr_tex_w = 0, spr_tex_h = 0;
    int spr_clip = 0;
    uint64_t spr_t0 = 0;

    // tilemap preview (map slots cannot be freed, so uploads are cached per bundle+asset)
    struct MapSlot { TilemapId id = kNoTilemap; TextureId tileset = kNoTexture; MapView view; };
    std::map<std::string, MapSlot> map_slots;
    int map_asset = -1;
    MapSlot* map = nullptr;
    int view_x = 0, view_y = 0;
    bool map_parallax = true, map_collision = false, map_spawns = true, map_fly = true;
    int  fly_dir = 1;
    uint32_t layer_hidden = 0;                        // bit per layer
    std::string map_err;
    Rect map_rect;                                    // where the map shows this frame
    bool map_visible = false;

    // sound
    AudioMixer* mixer = nullptr;
    AudioCommandQueue queue;
    AudioCommand queue_storage[32];
    AudioGlue glue{};
    int audio_state = 0;                              // 0 not started, 1 running, -1 no device
    int snd_asset = -1;
    uint64_t snd_t0 = 0;
    bool snd_playing = false;
    std::vector<int16_t> wave_mn, wave_mx;
    int wave_asset = -1, wave_cols = 0;

    // blob / spawns scroll
    int list_scroll = 0;

    // run
    LogRing log{ 6000 };
    uint64_t log_consumed = 0;
    std::vector<ConLine> con;
    int con_scroll = 0;
    bool con_follow = true;
    int last_launch = -1;

    // ---------------------------------------------------------------------------------------
    void on_start(App& app) override {
        Renderer& r = app.render();
        renderer = &r;
        app_ = &app;
        host.s = this;
        tg.init(r);
        gui.g = &tg;
        // The desktop extension: Esc belongs to dialogs/editors, closing the window asks about
        // unsaved work, and the canvas follows the window size.
        phx_desktop_set_quit_on_escape(0);
        phx_desktop_set_confirm_quit(1);
        phx_desktop_set_resizable(1);
        // Open bigger than the 640x360 minimum when the screen has room (an 800x450 canvas at the
        // chosen scale), unless --scale / a script / a shot asked for a fixed, reproducible size.
        int dw = 0, dh = 0;
        if (auto_size && phx_desktop_display_size(&dw, &dh)) {
            if (dh >= 2000) phx_desktop_set_scale(3);
            const int sc = phx_desktop_scale();
            if (dw >= 820 * sc && dh >= 480 * sc) phx_desktop_set_window_size(800 * sc, 450 * sc);
        }
        int fw = kW, fh = kH;
        if (phx_desktop_fb_size(&fw, &fh) == 0 && fw > 0) { kW = fw; kH = fh; kBodyH = kH - kTopH - kBotH; }

        for (int y = 0; y < 12; ++y)
            for (int x = 0; x < 12; ++x) {
                const char c = kFlame[y][x];
                g_flame_px[y * 12 + x] = c == 'o' ? rgba(236, 84, 40) : c == 'y' ? rgba(255, 170, 50)
                                       : c == 'w' ? rgba(255, 240, 180) : 0u;
            }
        TextureDesc id{};
        id.pixels = g_flame_px; id.width = 12; id.height = 12;
        flame_tex = r.load_texture(id);

        policy.engine_root = canon_path(root);
        policy.engine_dev = engine_dev;
        mixer = AudioMixer::create(app.mem().persistent(), caps(), kMixRate).unwrap();
        queue.init(queue_storage, 32);
        glue = AudioGlue{ mixer, &queue };
        jobs.reset(new JobRunner(root));
        load_recent_projects();

        if (engine_dev) {
            // Engine maintenance: the whole checkout is the workspace (the Studio as it always was).
            ws_root = root;
            if (!scan_engine(root, engine, &engine_err))
                std::fprintf(stderr, "phxstudio: %s\n", engine_err.c_str());
            for (size_t i = 0; i < engine.modules.size(); ++i)
                if (engine.modules[i].name == "render") mod_sel = int(i);
            std::string caps_h;
            if (read_text(root + "/engine/core/include/phx/core/caps.h", caps_h)) parse_caps(caps_h, tiers);
            scan_names(root, names);
            read_text(root + "/Makefile", makefile);
            const char* dka = std::getenv("DEVKITARM");
            const char* dkp = std::getenv("DEVKITPRO");
            const std::string devkitarm = dka ? dka : (dkp ? std::string(dkp) + "/devkitARM" : "/opt/devkitpro/devkitARM");
            launches = default_launches(makefile, devkitarm);
            for (const Launch& l : launches) launch_missing.push_back(missing_need(l));
            refresh_bundles();
            select_initial_asset();
            ws.init(host);
            ws.load_session(host, !fresh && initial_opens.empty());
            open_initial_files();
            run_initial_launches();
            std::printf("phxstudio: engine development — %s: %zu modules, %d edges, %zu bundles, %zu names, %zu launches\n",
                        root.c_str(), engine.modules.size(), engine.edges, bundle_paths.size(), names.size(),
                        launches.size());
            return;
        }
        // Project mode: nothing of the engine is read until a project is open, and then only its
        // public API headers (read-only) and the project's own folder.
        if (tab == Tab::Overview) tab = Tab::Editor;
        if (!project_arg.empty()) {
            if (!open_project(project_arg)) std::fprintf(stderr, "phxstudio: cannot open project '%s'\n", project_arg.c_str());
        }
        if (!has_project) std::printf("phxstudio: no project open — pick one (or --project DIR; --engine-dev for the engine)\n");
    }

    // ---------------------------------------------------------------------------------------
    // projects
    // ---------------------------------------------------------------------------------------
    std::vector<Tab> visible_tabs() const {
        if (engine_dev) return { Tab::Overview, Tab::Assets, Tab::Editor, Tab::Run };
        if (!has_project) return {};
        return { Tab::Editor, Tab::Assets, Tab::Run };      // the engine's module graph is not a project's business
    }
    bool tab_visible(Tab t) const {
        for (Tab v : visible_tabs()) if (v == t) return true;
        return false;
    }

    // The engine's public API folders (engine/<module>/include), read-only in project mode.
    std::vector<std::pair<std::string, std::string>> api_dirs() const {
        std::vector<std::pair<std::string, std::string>> out;
        if (engine_dev) return out;                          // the whole checkout is already visible
        std::error_code ec;
        for (pfs::directory_iterator it(pfs::path(root) / "engine", ec), end; !ec && it != end; it.increment(ec)) {
            const pfs::path inc = it->path() / "include";
            if (it->is_directory(ec) && pfs::is_directory(inc, ec))
                out.push_back({ it->path().filename().string(), canon_path(inc.string()) });
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    static std::string projects_file() {
        const char* xdg = std::getenv("XDG_CONFIG_HOME");
        const char* home = std::getenv("HOME");
        if (xdg && *xdg) return std::string(xdg) + "/phxstudio/projects.txt";
        const char* appdata = std::getenv("APPDATA");    // Windows, started outside a Unix shell
        if (home && *home) return std::string(home) + "/.config/phxstudio/projects.txt";
        if (appdata && *appdata) return std::string(appdata) + "/phxstudio/projects.txt";
        return "";
    }
    void load_recent_projects() {
        recent_projects.clear();
        std::string text;
        if (projects_file().empty() || !read_text(projects_file(), text)) return;
        size_t p = 0;
        while (p < text.size()) {
            size_t e = text.find('\n', p);
            if (e == std::string::npos) e = text.size();
            const std::string line = text.substr(p, e - p);
            p = e + 1;
            if (!line.empty() && ProjectDoc::is_project_dir(line)) recent_projects.push_back(line);
        }
    }
    void note_recent_project(const std::string& dir) {
        recent_projects.erase(std::remove(recent_projects.begin(), recent_projects.end(), dir), recent_projects.end());
        recent_projects.insert(recent_projects.begin(), dir);
        if (recent_projects.size() > 10) recent_projects.resize(10);
        if (!script.empty() || !shot_path.empty()) return;  // scripted runs leave no trace
        const std::string f = projects_file();
        if (f.empty()) return;
        std::error_code ec;
        pfs::create_directories(pfs::path(f).parent_path(), ec);
        if (FILE* fp = std::fopen(f.c_str(), "wb")) {
            for (const std::string& r : recent_projects) std::fprintf(fp, "%s\n", r.c_str());
            std::fclose(fp);
        }
    }

    // Open (or switch to) a project. The caller has already dealt with unsaved documents.
    bool open_project(const std::string& path) {
        std::string err;
        ProjectDoc p;
        if (!ProjectDoc::load(path, p, &err)) { toast(err, Toast::Bad); return false; }
        const std::string problem = policy.project_dir_problem(p.dir);
        if (!problem.empty()) { toast(p.dir + ": " + problem, Toast::Bad); return false; }
        if (has_project && script.empty() && shot_path.empty()) ws.save_session();
        ws.close_all(host);
        clear_asset_caches();
        project = p;
        has_project = true;
        policy.project_dir = p.dir;
        ws_root = p.dir;
        // launches: the project's own, run from its folder with $PHX_ROOT/$PHX_PROJECT
        launches.clear();
        launch_missing.clear();
        for (const ProjectLaunch& pl : p.launches) {
            Launch l;
            l.label = pl.label;
            l.group = pl.group == "build" ? Group::Build : pl.group == "test" ? Group::Test
                    : pl.group == "console" ? Group::Cross : pl.group == "tool" ? Group::Edit : Group::Play;
            l.command = launch_shell_command(pl, canon_path(root), p.dir);
            l.blurb = pl.blurb.empty() ? pl.command : pl.blurb;
            l.needs = pl.needs;
            l.windowed = pl.windowed;
            launches.push_back(l);
            launch_missing.push_back(missing_need(l));
        }
        last_launch = -1;
        diags.clear();
        names = NameBook{};
        scan_names_in(p.dir, p.bundle_paths(), names);
        refresh_bundles();
        select_initial_asset();
        ws.init(host);
        ws.load_session(host, !fresh && initial_opens.empty());
        open_initial_files();
        initial_opens.clear();
        run_initial_launches();
        initial_runs.clear();
        prefab_dirty = true;
        if (!tab_visible(tab)) tab = Tab::Editor;
        note_recent_project(p.dir);
        std::printf("phxstudio: project '%s' (%s) — %zu launches, %zu bundles\n", p.name.c_str(), p.dir.c_str(),
                    launches.size(), bundle_paths.size());
        return true;
    }
    void close_project() {
        if (!has_project) return;
        if (script.empty() && shot_path.empty()) ws.save_session();
        ws.close_all(host);
        clear_asset_caches();
        has_project = false;
        policy.project_dir.clear();
        project = ProjectDoc{};
        ws_root.clear();
        launches.clear();
        launch_missing.clear();
        bundle_paths.clear();
        diags.clear();
    }
    // Run `then` once the open documents are saved or discarded (or right away when none is dirty).
    void with_saved_docs(const std::string& what, std::function<void()> then) {
        if (!ws.any_dirty()) { then(); return; }
        const int n = ws.dirty_count();
        push_modal(what, 330, 80, [this, n, then](twk::Gui& g, twk::Rect body) {
            g.text(body.x, body.y + 2, twk::fmt("%d file%s ha%s unsaved changes.", n, n == 1 ? "" : "s", n == 1 ? "s" : "ve"), pal::text);
            g.text(body.x, body.y + 14, "Save them first?", pal::dim);
            const int y = body.bottom() - 14;
            twk::Btn sb; sb.on = true;
            if (g.button(twk::Rect{ body.x, y, 80, 14 }, "Save all", sb) || g.key(PHX_KEY_ENTER)) {
                if (ws.save_all(host)) then();
                return false;
            }
            if (g.button(twk::Rect{ body.x + 84, y, 90, 14 }, "Don't save")) { then(); return false; }
            if (g.button(twk::Rect{ body.x + 178, y, 70, 14 }, "Cancel") || g.key(PHX_KEY_ESCAPE)) return false;
            return true;
        });
    }
    // Drop everything the Assets view holds for the previous workspace (zero-copy uploads first).
    void clear_asset_caches() {
        if (!renderer) return;
        for (auto& kv : map_slots) renderer->unload_texture(kv.second.tileset);
        renderer->unload_texture(spr_tex);
        spr_tex = kNoTexture;
        renderer->unload_texture(tex_id);
        tex_id = kNoTexture;
        if (snd_playing) { queue.stop_all(); snd_playing = false; }
        docs.clear();
        map_slots.clear();
        map = nullptr;
        map_asset = tex_asset = spr_asset = snd_asset = wave_asset = -1;
        doc = nullptr;
        order.clear();
        bundle_sel = asset_sel = -1;
        bundle_paths.clear();
    }

    // A compiler message's file -> an absolute, canonical path. Relative paths are relative to
    // the directory make ran in: the project folder, or (make -C "$PHX_ROOT") the engine checkout.
    std::string resolve_diag_file(const std::string& f) const {
        if (f.empty() || is_abs_path(f)) return canon_path(f);
        std::error_code ec;
        for (const std::string& base : { ws_root, root })
            if (!base.empty() && pfs::exists(join_path(base, f), ec)) return canon_path(join_path(base, f));
        return canon_path(join_path(root, f));
    }
    // Re-read phxproject.json (the Assets "scan" button, after editing the project file).
    void project_reload_file() {
        ProjectDoc p;
        std::string err;
        if (!ProjectDoc::load(project.dir, p, &err)) { toast(err, Toast::Bad); return; }
        project.bundles = p.bundles;
        project.name = p.name;
    }

    std::string resolve_user_path(const std::string& p) const {
        if (p.empty() || is_abs_path(p)) return p;
        std::error_code ec;
        if (pfs::exists(p, ec)) return canon_path(p);         // relative to the working directory
        return join_path(ws_root.empty() ? root : ws_root, p);
    }
    void open_initial_files() {
        for (const std::string& p : initial_opens) ws.open(host, resolve_user_path(p));
    }
    void run_initial_launches() {
        for (const std::string& want : initial_runs) {
            bool found = false;
            for (size_t i = 0; i < launches.size() && !found; ++i)
                if (launches[i].make_target == want || launches[i].label == want) {
                    found = true;
                    if (!launch_missing[i].empty())
                        std::fprintf(stderr, "phxstudio: --run %s needs %s\n", want.c_str(), launch_missing[i].c_str());
                    run_launch(int(i));
                }
            if (!found) std::fprintf(stderr, "phxstudio: --run: no launch named '%s'\n", want.c_str());
        }
    }
    void select_initial_asset() {
        if (!initial_bundle.empty()) {
            const std::string want = canon_path(resolve_user_path(initial_bundle));
            for (size_t i = 0; i < bundle_paths.size(); ++i)
                if (bundle_paths[i] == initial_bundle || bundle_abs(bundle_paths[i]) == want) select_bundle(int(i));
        }
        if (!initial_asset.empty() && doc) {                 // "name" or "type:name" (e.g. sprite:hero)
            const size_t colon = initial_asset.find(':');
            const std::string want_t = colon == std::string::npos ? "" : initial_asset.substr(0, colon);
            const std::string want_n = colon == std::string::npos ? initial_asset : initial_asset.substr(colon + 1);
            for (size_t k = 0; k < order.size(); ++k) {
                const AssetEntry& a = doc->assets[size_t(order[k])];
                if (names.label(a.hash) == want_n && (want_t.empty() || want_t == type_name(a.type))) { select_asset(int(k)); break; }
            }
        }
        if (bundle_sel < 0) {                            // prefer a rich, valid bundle
            for (const char* pref : { "build/emberwing.phxp", "build/platformer.phxp" })
                for (size_t i = 0; i < bundle_paths.size() && bundle_sel < 0; ++i)
                    if (bundle_paths[i] == pref) select_bundle(int(i));
            for (size_t i = 0; i < bundle_paths.size() && bundle_sel < 0; ++i)
                if (docs[bundle_paths[i]] && docs[bundle_paths[i]]->ok) select_bundle(int(i));
        }
    }
    // Bundle list entries are engine-root-relative (engine dev) or absolute (a project's).
    std::string bundle_abs(const std::string& p) const { return p.empty() || is_abs_path(p) ? p : root + "/" + p; }
    std::string bundle_label(const std::string& p) const {
        if (has_project && path_within(p, project.dir)) return p.substr(project.dir.size() + 1);
        return p;
    }

    // ---- the project picker (no project open) ----
    void draw_picker() {
        const twk::Rect r{ 0, kBodyY, kW, kBodyH };
        tg.rect(r, pal::bg, twk::kSubBg);
        const int cx = r.x + std::max(12, (r.w - 460) / 2);
        int y = r.y + std::max(12, (r.h - 300) / 4);
        tg.image(twk::Rect{ cx, y, 24, 24 }, flame_tex, 0, 0, 12, 12);
        tg.text(cx + 32, y + 4, "Phoenix Studio", pal::accent, twk::kSubText, r.w, 2);
        y += 32;
        tg.text(cx, y, "Open a game project to start. A project is a folder with a phxproject.json:", pal::dim, twk::kSubText, r.w - 20);
        tg.text(cx, y + 10, "the Studio edits that folder only (the engine's public API is readable).", pal::dim, twk::kSubText, r.w - 20);
        y += 28;
        twk::Btn nb; nb.icon = twk::kIconPlus; nb.on = true; nb.help = "Create a project from the template: a small game with a sprite, a tileset and a level";
        if (tg.button(twk::Rect{ cx, y, 140, 16 }, "New project...", nb)) new_project_dialog();
        twk::Btn ob; ob.icon = twk::kIconFolderOpen; ob.help = "Open a folder that has a phxproject.json";
        if (tg.button(twk::Rect{ cx + 148, y, 150, 16 }, "Open project...", ob)) open_project_dialog();
        y += 28;
        auto list = [&](const char* title, const std::vector<std::string>& dirs) {
            tg.section(cx, y, 460, title, pal::faint);
            y += 13;
            if (dirs.empty()) { tg.text(cx + 4, y, "none yet", pal::faint); y += 12; }
            for (const std::string& d : dirs) {
                const twk::Rect rr{ cx, y, 460, 13 };
                ProjectDoc pd;
                const bool ok = ProjectDoc::load(d, pd);
                if (tg.hover(rr)) { tg.rect(rr, pal::hover, twk::kSubWidget); tg.hint = d; }
                tg.icon(rr.x + 3, rr.y + 1, twk::kIconFileMap, ok ? pal::accent : pal::bad);
                tg.text(rr.x + 17, rr.y + 3, ok ? pd.name : base_name(d), ok ? pal::text : pal::bad, twk::kSubText, 150);
                tg.text(rr.x + 175, rr.y + 3, d, pal::faint, twk::kSubText, rr.w - 177);
                if (ok && tg.clicked(rr)) { open_project(d); return; }
                y += 14;
            }
            y += 8;
        };
        list("RECENT PROJECTS", recent_projects);
        if (has_project) return;
        list("EXAMPLE PROJECTS (in this Phoenix checkout)", discover_projects(root));
        tg.text(cx, std::min(y + 4, r.bottom() - 14), "Working on the engine itself? Start the Studio with --engine-dev.", pal::faint, twk::kSubText, r.w - 20);
    }

    void new_project_dialog() {
        struct St { std::string name = "My Game", where = "examples"; std::string err; bool first = true; };
        auto st = std::make_shared<St>();
        push_modal("New project", 380, 130, [this, st](twk::Gui& g, twk::Rect body) {
            const uint32_t nid = g.id("np-name");
            if (st->first) { g.set_focus(nid, true); g.edit().set(st->name, true); st->first = false; }
            g.text(body.x, body.y + 3, "name", pal::dim);
            g.text_field(nid, twk::Rect{ body.x + 70, body.y, body.w - 70, 13 }, st->name, "My Game");
            g.text(body.x, body.y + 20, "location", pal::dim);
            g.text_field(g.id("np-where"), twk::Rect{ body.x + 70, body.y + 17, body.w - 70, 13 }, st->where, "examples",
                         0, "The parent folder: relative to the Phoenix checkout, or an absolute path");
            const std::string slug = project_slug(st->name);
            const std::string parent = st->where.empty() ? root : (is_abs_path(st->where) ? st->where : join_path(root, st->where));
            const std::string dir = join_path(parent, slug);
            g.text(body.x, body.y + 38, "creates " + dir, pal::faint, twk::kSubText, body.w);
            g.text(body.x, body.y + 48, "src/main.cpp, assets/ (sprites, tileset, level, prefabs, sounds), phxproject.json", pal::faint, twk::kSubText, body.w);
            if (!st->err.empty()) g.text(body.x, body.y + 62, st->err, pal::bad, twk::kSubText, body.w);
            const int y = body.bottom() - 14;
            twk::Btn cb; cb.on = true; cb.enabled = !slug.empty();
            if (g.button(twk::Rect{ body.x, y, 80, 14 }, "Create", cb) || (!slug.empty() && g.focus() == 0 && g.key(PHX_KEY_ENTER))) {
                const std::string problem = policy.project_dir_problem(dir);
                if (!problem.empty()) { st->err = problem; return true; }
                std::string err;
                if (!create_project(dir, st->name, &err)) { st->err = err; return true; }
                const std::string d = dir;
                with_saved_docs("Switch project", [this, d] { open_project(d); });
                toast("created project " + d, Toast::Good);
                return false;
            }
            if (g.button(twk::Rect{ body.x + 86, y, 70, 14 }, "Cancel") || g.key(PHX_KEY_ESCAPE)) return false;
            return true;
        });
    }
    void open_project_dialog() {
        struct St { std::string path; std::string err; bool first = true; std::vector<std::string> known; };
        auto st = std::make_shared<St>();
        st->known = recent_projects;
        for (const std::string& d : discover_projects(root))
            if (std::find(st->known.begin(), st->known.end(), d) == st->known.end()) st->known.push_back(d);
        push_modal("Open project", 420, 190, [this, st](twk::Gui& g, twk::Rect body) {
            const uint32_t pid = g.id("op-path");
            if (st->first) { g.set_focus(pid, true); g.edit().set("", false); st->first = false; }
            g.text(body.x, body.y + 3, "folder", pal::dim);
            bool go = false;
            if (g.focus() == pid && g.key(PHX_KEY_ENTER)) { st->path = g.edit().buf; go = true; }
            g.text_field(pid, twk::Rect{ body.x + 44, body.y, body.w - 44, 13 }, st->path, "path/to/project (holds phxproject.json)");
            int y = body.y + 18;
            for (size_t i = 0; i < st->known.size() && y + 12 < body.bottom() - 18; ++i) {
                const twk::Rect rr{ body.x, y, body.w, 12 };
                if (g.hover(rr)) g.rect(rr, pal::hover, twk::kSubWidget);
                g.icon(rr.x + 2, rr.y + 1, twk::kIconFileMap, pal::accent);
                g.text(rr.x + 16, rr.y + 2, st->known[i], pal::text, twk::kSubText, rr.w - 18);
                if (g.clicked(rr)) { st->path = st->known[i]; go = true; }
                y += 12;
            }
            if (!st->err.empty()) g.text(body.x, body.bottom() - 28, st->err, pal::bad, twk::kSubText, body.w);
            const int by = body.bottom() - 14;
            twk::Btn ob; ob.on = true; ob.enabled = !st->path.empty();
            go |= g.button(twk::Rect{ body.x, by, 70, 14 }, "Open", ob);
            if (go) {
                const std::string p = resolve_user_path(st->path);
                if (!ProjectDoc::is_project_dir(p)) { st->err = "no phxproject.json in " + p; return true; }
                const std::string problem = policy.project_dir_problem(p);
                if (!problem.empty()) { st->err = problem; return true; }
                with_saved_docs("Switch project", [this, p] { open_project(p); });
                return false;
            }
            if (g.button(twk::Rect{ body.x + 76, by, 70, 14 }, "Cancel") || g.key(PHX_KEY_ESCAPE)) return false;
            return true;
        });
    }

    void on_stop(App&) override {
        if (audio_state == 1) phx_sdl_audio_stop();
        if (jobs) jobs->shutdown();
        if (script.empty() && shot_path.empty() && (engine_dev || has_project)) ws.save_session();
        for (int i = int(ws.docs.size()) - 1; i >= 0; --i) ws.docs[size_t(i)]->release(host);
    }

    void on_fixed_update(App&, scalar) override {
        ++ticks;
        if (map && map_fly && tab == Tab::Assets) {         // the "fly-through" camera
            const int max_x = std::max(0, int(map->view.w) * map->view.tile_w - map_rect.w);
            view_x += fly_dir;
            if (view_x >= max_x) { view_x = max_x; fly_dir = -1; }
            if (view_x <= 0)     { view_x = 0;     fly_dir = 1; }
        }
    }

    // ---------------------------------------------------------------------------------------
    // assets: selection + preview resources
    // ---------------------------------------------------------------------------------------
    void refresh_bundles() {
        const std::string cur = bundle_sel >= 0 && bundle_sel < int(bundle_paths.size()) ? bundle_paths[size_t(bundle_sel)] : "";
        if (engine_dev) bundle_paths = find_bundles(root);
        else if (has_project) bundle_paths = project.bundle_paths();   // absolute; the project's only
        else bundle_paths.clear();
        bundle_sel = -1;
        for (size_t i = 0; i < bundle_paths.size(); ++i) {
            if (bundle_paths[i] == cur) bundle_sel = int(i);
            // pre-validate every bundle so the list can show a status dot for each
            if (!docs.count(bundle_paths[i])) {
                std::unique_ptr<BundleDoc> d(new BundleDoc);
                BundleDoc::load(bundle_abs(bundle_paths[i]), *d);
                docs[bundle_paths[i]] = std::move(d);
            }
        }
    }

    // Drop every bundle and preview (zero-copy uploads first) and scan again.
    void rescan_bundles() {
        // Everything uploaded zero-copy from a cached BundleDoc must go before the cache does.
        // (Tilemap slots have no unload API: those stay allocated but are never drawn again.)
        for (auto& kv : map_slots) renderer->unload_texture(kv.second.tileset);
        renderer->unload_texture(spr_tex);
        spr_tex = kNoTexture;
        renderer->unload_texture(tex_id);
        tex_id = kNoTexture;
        if (snd_playing) { queue.stop_all(); snd_playing = false; }
        const int keep_asset = asset_sel;
        src_hash_ = 0;
        docs.clear(); map_slots.clear();
        map = nullptr; map_asset = tex_asset = spr_asset = snd_asset = wave_asset = -1;
        doc = nullptr;
        if (has_project) {
            project_reload_file();
            names = NameBook{};
            scan_names_in(project.dir, project.bundle_paths(), names);
        }
        refresh_bundles();
        if (bundle_sel >= 0) {
            select_bundle(bundle_sel);
            if (keep_asset >= 0 && keep_asset < int(order.size())) select_asset(keep_asset);
        }
    }
    // The project's bake launch (its command runs `game-assets`, not a tier-0 variant), or -1.
    int bake_launch() const {
        for (size_t i = 0; i < launches.size(); ++i)
            if (launches[i].command.find("game-assets") != std::string::npos && launches[i].command.find("TIER=0") == std::string::npos &&
                launch_missing[i].empty())
                return int(i);
        return -1;
    }
    int pending_bake_ = -1;

    // The author file an asset was baked from, found by name in the workspace (the bake names
    // assets by file stem): texture -> .png, sprite -> .sprdef / sprite .json, tilemap + its spawns
    // -> .tmj, blob -> phxbin table .json, sound -> .wav. "" when there is none (or the name hash
    // was never recovered).
    std::string asset_source(const AssetEntry& a) {
        // cached per selected asset (the preview header asks every frame; this touches the disk)
        if (a.hash == src_hash_ && a.type == src_type_ && ticks - src_tick_ < 60) return src_cache_;
        src_hash_ = a.hash; src_type_ = a.type; src_tick_ = ticks; src_cache_.clear();
        const std::string* nm = names.find(a.hash);
        if (!nm) return src_cache_;
        FileKind want = FileKind::Image;
        switch (a.type) {
        case AssetType::Texture: case AssetType::Font: want = FileKind::Image; break;
        case AssetType::Sprite:  want = FileKind::Sprite; break;
        case AssetType::Tilemap: case AssetType::Spawns: want = FileKind::Map; break;
        case AssetType::Blob:    want = FileKind::Table; break;
        case AssetType::Sound:   want = FileKind::Sound; break;
        }
        for (const std::string& rel : asset_source_candidates(ws.all_files(), ws.tree.root, *nm, want))
            if (policy.access(ws.tree.abs(rel)) == Access::Write) { src_cache_ = ws.tree.abs(rel); break; }
        return src_cache_;
    }
    phx::NameHash src_hash_ = 0;
    AssetType src_type_ = AssetType::Texture;
    uint64_t src_tick_ = 0;
    std::string src_cache_;
    void edit_asset_source(const AssetEntry& a) {
        const std::string src = asset_source(a);
        const std::string* nm = names.find(a.hash);
        if (src.empty()) {
            toast(nm ? "no source file for '" + *nm + "' in the " + std::string(has_project ? "project" : "checkout")
                     : std::string("this asset's name was not recovered, so its source can't be found"), Toast::Warn);
            return;
        }
        if (a.type == AssetType::Sound) { toast("no sound editor yet - the source is " + src, Toast::Info); return; }
        switch_tab(Tab::Editor);
        ws.open(host, src);
    }

    void select_bundle(int i) {
        if (i < 0 || i >= int(bundle_paths.size())) return;
        bundle_sel = i;
        doc = docs[bundle_paths[size_t(i)]].get();
        order.clear();
        for (size_t k = 0; k < doc->assets.size(); ++k) order.push_back(int(k));
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            const AssetEntry& A = doc->assets[size_t(a)];
            const AssetEntry& B = doc->assets[size_t(b)];
            if (A.type != B.type) return uint16_t(A.type) < uint16_t(B.type);
            return names.label(A.hash) < names.label(B.hash);
        });
        asset_scroll = 0;
        select_asset(order.empty() ? -1 : 0);
    }

    void select_asset(int row) {
        asset_sel = row;
        list_scroll = 0;
        wide = wide && current_asset() && current_asset()->type == AssetType::Tilemap;
    }

    const AssetEntry* current_asset() const {
        if (!doc || asset_sel < 0 || asset_sel >= int(order.size())) return nullptr;
        return &doc->assets[size_t(order[size_t(asset_sel)])];
    }
    int current_index() const { return asset_sel >= 0 && asset_sel < int(order.size()) ? order[size_t(asset_sel)] : -1; }

    // Upload the texture preview for `tex_tier` from the per-tier re-encodes.
    void load_tex_tier(Renderer& r) {
        r.unload_texture(tex_id);
        tex_id = kNoTexture;
        TextureDesc d{};
        d.width = tex_rep.w; d.height = tex_rep.h;
        if (tex_tier == 0 && tex_rep.pal4_ok) {
            d.pixels = tex_rep.pal4.data(); d.size = uint32_t(tex_rep.pal4.size()); d.format = PixelFormat::PAL4_TILES;
        } else if (tex_tier == 1 && tex_rep.swz_ok) {
            d.pixels = tex_rep.swz.data(); d.size = uint32_t(tex_rep.swz.size()); d.format = PixelFormat::RGBA8_SWZ;
        } else {
            d.pixels = tex_rgba.data(); d.size = uint32_t(tex_rgba.size() * 4); d.format = PixelFormat::RGBA8;
        }
        tex_id = r.load_texture(d);
    }

    void prepare_texture(Renderer& r, int idx) {
        if (tex_asset == idx && doc) return;
        tex_asset = idx;
        TexView v;
        tex_rgba.clear();
        tex_rep = TierReport{};
        if (!view_texture(doc->assets[size_t(idx)], v) || !decode_rgba8(v, tex_rgba)) {
            r.unload_texture(tex_id); tex_id = kNoTexture;
            return;
        }
        tex_rep = analyse_tiers(tex_rgba, v.w, v.h);
        tex_tier = int(doc->hdr.target) <= 2 ? int(doc->hdr.target) : 2;   // start as the bundle ships it
        load_tex_tier(r);
    }

    // The baked blob of texture `h` in the current bundle, uploaded zero-copy in its own format.
    TextureId upload_native(Renderer& r, NameHash h, uint16_t* w = nullptr, uint16_t* ht = nullptr) {
        const int ti = doc->find(h, AssetType::Texture);
        if (ti < 0) return kNoTexture;
        TexView v;
        if (!view_texture(doc->assets[size_t(ti)], v)) return kNoTexture;
        TextureDesc d{};
        d.pixels = v.payload; d.size = v.payload_size; d.width = v.w; d.height = v.h; d.format = v.fmt;
        if (w) *w = v.w;
        if (ht) *ht = v.h;
        return r.load_texture(d);
    }

    void prepare_sprite(Renderer& r, int idx) {
        if (spr_asset == idx) return;
        spr_asset = idx;
        r.unload_texture(spr_tex);
        spr_tex = kNoTexture;
        spr_clip = 0;
        spr_t0 = ticks;
        if (!view_sprite(doc->assets[size_t(idx)], spr)) return;
        spr_tex = upload_native(r, spr.texture, &spr_tex_w, &spr_tex_h);
    }

    void prepare_map(Renderer& r, int idx) {
        if (map_asset == idx && map) return;
        map_asset = idx;
        map = nullptr;
        map_err.clear();
        view_x = view_y = 0;
        layer_hidden = 0;
        const std::string key = doc->path + "#" + std::to_string(doc->assets[size_t(idx)].hash);
        auto it = map_slots.find(key);
        if (it == map_slots.end()) {
            MapSlot s;
            if (!view_tilemap(doc->assets[size_t(idx)], s.view)) { map_err = "malformed tilemap blob"; return; }
            s.tileset = upload_native(r, s.view.tileset);
            if (s.tileset == kNoTexture) {
                map_err = "tileset '" + names.label(s.view.tileset) + "' is not in this bundle";
                return;
            }
            if (map_slots.size() >= 24) { map_err = "tilemap slot budget reached - restart the studio"; return; }
            TilemapDesc md{};
            md.indices = s.view.indices; md.width = s.view.w; md.height = s.view.h;
            md.layers = s.view.layers; md.tile_w = s.view.tile_w; md.tile_h = s.view.tile_h;
            md.tileset = s.tileset;
            s.id = r.upload_tilemap(md);          // zero-copy: points into the cached BundleDoc
            if (s.id == kNoTilemap) { map_err = "renderer is out of tilemap slots"; return; }
            it = map_slots.emplace(key, s).first;
        }
        map = &it->second;
    }

    bool ensure_audio() {
        if (audio_state == 0)
            audio_state = phx_sdl_audio_start(kMixRate, audio_fill, &glue) == 0 ? 1 : -1;
        return audio_state == 1;
    }

    // ---------------------------------------------------------------------------------------
    // frame
    // ---------------------------------------------------------------------------------------
    void on_render(App& app, scalar) override {
        Renderer& r = app.render();
        const InputState& in = app.input();
        last_stats = r.stats();                          // the previous frame's (begin_frame resets them)
        const phx_platform* plat = app.platform();
        const uint64_t t0 = plat->clock_ns();
        ++frames_rendered;
        if (ticks >= fps_tick + 60) {                    // measured, not derived: frames per 60 sim ticks
            fps = unsigned(frames_rendered * 60 / std::max<uint64_t>(1, ticks - fps_tick));
            frames_rendered = 0;
            fps_tick = ticks;
        }

        // ---- input: the desktop event stream (or the --script driver) ----
        twk::collect_desktop(tin);
        step_script(app);
        int fw = kW, fh = kH;
        if (phx_desktop_fb_size(&fw, &fh) == 0 && fw > 0 && (fw != kW || fh != kH)) {
            kW = fw; kH = fh; kBodyH = kH - kTopH - kBotH;
        }
        if (jobs) { jobs->drain(log); pull_console(); }
        if (pending_bake_ >= 0 && jobs) {
            const JobState st = jobs->result(pending_bake_).state;
            if (st == JobState::Pass || st == JobState::Fail || st == JobState::Stopped) {
                if (st == JobState::Pass) { rescan_bundles(); toast("baked - the Assets view shows the new bundle", Toast::Good); }
                else toast("the bake failed - see the Run view", Toast::Bad);
                pending_bake_ = -1;
            }
        }
        if (ticks % 60 == 0) ws.poll_disk(host);
        if (ticks % 600 == 0 && script.empty()) ws.save_session();

        // legacy views read buttons (arrows/WASD/QE) only while nothing else owns the keyboard
        static const InputState kNoInput{};
        const bool kb_free = !tg.text_focus() && modals.empty() && !tg.any_popup();
        const InputState& lin = kb_free && tab != Tab::Editor ? in : kNoInput;

        // decide the camera first: the tilemap preview (if any) sets it
        map_visible = false;
        if (tab == Tab::Assets && (engine_dev || has_project)) {
            const AssetEntry* a = current_asset();
            if (a && a->type == AssetType::Tilemap) {
                prepare_map(r, current_index());
                if (map) map_visible = true;
            }
        }
        map_rect = preview_rect();
        map_rect.y += 30; map_rect.h -= 30 + 14;       // under the header + toolbar, above the layer strip
        Camera2D cam{};
        if (map_visible) {
            pan_map(lin);
            cam.pos = Gui::v2(view_x - map_rect.x, view_y - map_rect.y);
            for (uint8_t l = 0; l < map->view.layers && l < 4; ++l) {
                scalar fx = s_from_int(1), fy = s_from_int(1);
                if (map_parallax && !map->view.parallax_q16.empty()) {
                    fx = s_from_q16(map->view.parallax_q16[size_t(l) * 2]);
                    fy = s_from_q16(map->view.parallax_q16[size_t(l) * 2 + 1]);
                }
                r.set_tilemap_parallax(map->id, l, fx, fy);
            }
        }
        r.begin_frame(cam);
        if (map_visible)
            for (uint8_t l = 0; l < map->view.layers; ++l)
                if (!(layer_hidden & (1u << l))) r.draw_tilemap(map->id, l);

        tg.begin(&r, &in, tin, kW, kH, map_visible ? view_x - map_rect.x : 0, map_visible ? view_y - map_rect.y : 0);
        gui.sync();
        global_keys();
        if (map_visible) {                               // opaque cover around the map "window"
            const Rect h = map_rect;
            gui.rect(Rect{ 0, 0, kW, h.y }, pal::bg, kLyCover);
            gui.rect(Rect{ 0, h.y + h.h, kW, kH - h.y - h.h }, pal::bg, kLyCover);
            gui.rect(Rect{ 0, h.y, h.x, h.h }, pal::bg, kLyCover);
            gui.rect(Rect{ h.x + h.w, h.y, kW - h.x - h.w, h.h }, pal::bg, kLyCover);
        }

        if (!engine_dev && !has_project) draw_picker();
        else switch (tab) {
        case Tab::Overview: if (engine_dev) draw_overview(); break;
        case Tab::Assets:   draw_assets(r, lin); break;
        case Tab::Editor:   ws.draw(host, twk::Rect{ 0, kBodyY, kW, kBodyH }); break;
        case Tab::Run:      draw_run(lin); break;
        default: break;
        }
        gui.flush_hint();
        draw_top_bar();
        draw_status_bar(app);
        draw_toasts();
        draw_modal();
        handle_quit(app);
        update_title();
        tg.end();
        r.end_frame();
        render_avg.push(uint32_t((plat->clock_ns() - t0) / 1000u));   // record + rasterize, in µs
        if (!pending_shot.empty()) { write_shot(pending_shot); pending_shot.clear(); }
        if (!shot_path.empty() && app.frame() >= uint64_t(shot_frame)) {
            write_shot(shot_path);
            app.request_quit();
        }
    }

    // ---------------------------------------------------------------------------------------
    // shortcuts, modals, toasts, quit
    // ---------------------------------------------------------------------------------------
    void global_keys() {
        DocView* d = tab == Tab::Editor ? ws.current() : nullptr;
        using twk::kCtrl; using twk::kShift;
        if (tg.key('s', kCtrl) && d) {
            if (d->dirty()) ws.save(host, ws.active);
            else toast("no changes to save");         // never rewrite an untouched file
        }
        if (tg.key('s', kCtrl | kShift)) ws.save_all(host);
        const bool workspace = engine_dev || has_project;
        const std::vector<Tab> vt = visible_tabs();
        if (tg.key('p', kCtrl) && workspace) { switch_tab(Tab::Editor); ws.quick_open(host); }
        if (tg.key('w', kCtrl) && d) ws.close(host, ws.active);
        if (tg.key(PHX_KEY_TAB, kCtrl) && workspace) {
            if (tab == Tab::Editor) ws.next_tab(1);
            else for (size_t t = 0; t < vt.size(); ++t) if (vt[t] == tab) { switch_tab(vt[(t + 1) % vt.size()]); break; }
        }
        if (tg.key(PHX_KEY_TAB, kCtrl | kShift)) { if (tab == Tab::Editor) ws.next_tab(-1); }
        if (tg.key('q', kCtrl)) request_quit();
        if (tg.key('b', kCtrl) && workspace) { ws.side_open = !ws.side_open; switch_tab(Tab::Editor); }
        for (size_t t = 0; t < vt.size(); ++t) if (tg.key(int32_t('1' + t), kCtrl)) switch_tab(vt[t]);
        if (tg.key('=', kCtrl) || tg.key('+', kCtrl) || tg.key('=', kCtrl | kShift)) phx_desktop_set_scale(phx_desktop_scale() + 1);
        if (tg.key('-', kCtrl)) phx_desktop_set_scale(std::max(1, phx_desktop_scale() - 1));
        if (tg.key(PHX_KEY_F1)) show_shortcuts();
        if (tg.key(PHX_KEY_F5)) { if (last_launch >= 0) run_launch(last_launch); }
        if (d && modals.empty() && !tg.text_focus()) {
            // Ctrl+Z / Ctrl+Y route to the document (a text editor handles its own when focused)
            if (tg.key('z', kCtrl)) d->undo();
            if (tg.key('y', kCtrl) || tg.key('z', kCtrl | kShift)) d->redo();
        }
        if (tin.drops.size() && workspace) { switch_tab(Tab::Editor); for (const std::string& p : tin.drops) ws.open(host, p); }
    }

    void push_modal(const std::string& title, int w, int h, std::function<bool(twk::Gui&, twk::Rect)> body) {
        modals.push_back(Modal{ title, w, h, std::move(body) });
    }
    void draw_modal() {
        if (modals.empty()) return;
        const size_t top = modals.size() - 1;
        Modal m = modals[top];                           // (the body may push another modal)
        const twk::Rect body = tg.begin_modal(m.title, std::min(m.w, kW - 16), std::min(m.h, kH - 16));
        const bool keep = m.body(tg, body);
        tg.end_modal();
        if (!keep) {
            modals.erase(modals.begin() + long(top));
            tg.blur();
        }
    }
    void toast(const std::string& msg, Toast t = Toast::Info) {
        toasts.push_back(ToastMsg{ msg, t, ticks + (t == Toast::Bad ? 360u : 210u) });
        if (toasts.size() > 5) toasts.erase(toasts.begin());
        std::printf("phxstudio: %s\n", msg.c_str());
    }
    void draw_toasts() {
        toasts.erase(std::remove_if(toasts.begin(), toasts.end(), [&](const ToastMsg& t) { return t.until < ticks; }), toasts.end());
        int y = kH - kBotH - 4;
        for (size_t i = toasts.size(); i-- > 0; ) {
            const ToastMsg& t = toasts[i];
            const int w = std::min(kW - 20, twk::Gui::text_w(t.text) + 16);
            const twk::Rect r{ kW - w - 8, y - 15, w, 15 };
            const Rgba c = t.tone == Toast::Good ? pal::good : t.tone == Toast::Warn ? pal::warn : t.tone == Toast::Bad ? pal::bad : pal::info;
            const int saved = tg.plane();
            tg.set_plane(twk::kPlanePopup);
            tg.rect(r, pal::bar, twk::kSubFill);
            tg.frame_rect(r, c, twk::kSubWidget);
            tg.rect(twk::Rect{ r.x, r.y, 3, r.h }, c, twk::kSubImage);
            tg.text(r.x + 8, r.y + 4, t.text, pal::text, twk::kSubText, r.w - 10);
            if (tg.clicked(r)) toasts[i].until = 0;
            tg.set_plane(saved);
            y -= 18;
        }
    }
    void request_quit() {
        if (!ws.any_dirty()) { quitting = true; return; }
        if (!modals.empty()) return;
        const int n = ws.dirty_count();
        push_modal("Quit Phoenix Studio", 330, 80, [this, n](twk::Gui& g, twk::Rect body) {
            g.text(body.x, body.y + 2, twk::fmt("%d file%s ha%s unsaved changes.", n, n == 1 ? "" : "s", n == 1 ? "s" : "ve"), pal::text);
            g.text(body.x, body.y + 14, "Save them before quitting?", pal::dim);
            const int y = body.bottom() - 14;
            twk::Btn sb; sb.on = true;
            if (g.button(twk::Rect{ body.x, y, 96, 14 }, "Save all & quit", sb) || g.key(PHX_KEY_ENTER)) {
                if (ws.save_all(host)) quitting = true;
                return false;
            }
            if (g.button(twk::Rect{ body.x + 100, y, 110, 14 }, "Quit without saving")) { quitting = true; return false; }
            if (g.button(twk::Rect{ body.x + 214, y, 70, 14 }, "Cancel") || g.key(PHX_KEY_ESCAPE)) return false;
            return true;
        });
    }
    void handle_quit(App& app) {
        if (tin.quit) request_quit();
        if (quitting) app.request_quit();
    }
    void update_title() {
        std::string t = engine_dev ? "Phoenix Studio (engine development)" : has_project ? project.name + " - Phoenix Studio" : "Phoenix Studio";
        if (tab == Tab::Editor && (engine_dev || has_project))
            if (DocView* d = ws.current()) t = base_name(d->path) + (d->dirty() ? " *" : "") + " - " + t;
        if (t != title_shown) { phx_desktop_set_title(t.c_str()); title_shown = t; }
    }

    void show_shortcuts() {
        push_modal("Keyboard shortcuts", 520, 300, [](twk::Gui& g, twk::Rect body) {
            struct K { const char* k; const char* v; };
            const K cols[2][14] = {
                { { "STUDIO", "" }, { "Ctrl+1..4", "Overview / Assets / Editor / Run" }, { "Ctrl+P", "quick open a file" },
                  { "Ctrl+S / Ctrl+Shift+S", "save / save all" }, { "Ctrl+W", "close the tab" }, { "Ctrl+Tab", "next tab / view" },
                  { "Ctrl+Z / Ctrl+Y", "undo / redo" }, { "Ctrl+B", "toggle the Explorer" }, { "Ctrl+= / Ctrl+-", "bigger / smaller UI" },
                  { "F5", "re-run the last launch" }, { "Ctrl+Q", "quit" }, { "", "" },
                  { "CODE", "" }, { "Ctrl+F/H/G, Ctrl+/", "find, replace, go to line, comment" } },
                { { "SPRITE", "" }, { "B E G I L R O M H", "pencil eraser fill pick line rect ellipse select pan" },
                  { "X, [ ]", "swap colours, brush size" }, { ", .  P", "prev / next frame, play" },
                  { "right button / Alt", "secondary colour / pick" }, { "", "" },
                  { "MAP", "" }, { "B E G R I S T H", "brush eraser fill rect pick select spawn pan" },
                  { "Tab / V", "next layer / cycle collision" }, { "Ctrl+C", "copy the selection as a stamp" },
                  { "", "" }, { "TABLE", "" }, { "Enter/F2, typing", "edit a cell (Tab moves right)" },
                  { "+ / -, Ctrl+D", "step a number, duplicate record" } },
            };
            for (int c = 0; c < 2; ++c) {
                int y = body.y;
                const int x = body.x + c * (body.w / 2);
                for (const K& k : cols[c]) {
                    if (!k.v[0] && k.k[0]) g.text(x, y, k.k, pal::accent);
                    else if (k.k[0]) { g.text(x, y, k.k, pal::text, twk::kSubText, 118); g.text(x + 122, y, k.v, pal::dim, twk::kSubText, body.w / 2 - 126); }
                    y += 11;
                }
            }
            g.text(body.x, body.bottom() - 9, "Ctrl+wheel zooms, middle-drag pans, right-click for context menus.", pal::faint);
            return !(g.key(PHX_KEY_ESCAPE) || g.key(PHX_KEY_F1) || g.key(PHX_KEY_ENTER) || (g.in->pressed & twk::kMouseL && !body.contains(g.mx(), g.my())));
        });
    }
    void show_about() {
        push_modal("About Phoenix Studio", 330, 110, [this](twk::Gui& g, twk::Rect body) {
            g.image(twk::Rect{ body.x, body.y, 24, 24 }, flame_tex, 0, 0, 12, 12);
            g.text(body.x + 32, body.y + 2, "Phoenix Studio", pal::accent, twk::kSubText, body.w, 2);
            g.text(body.x + 32, body.y + 16, "engine v" PHX_VERSION_STRING, pal::dim);
            g.text(body.x, body.y + 32, "One editor for one engine that runs on GBA, PSP, Windows", pal::text, twk::kSubText, body.w);
            g.text(body.x, body.y + 42, "and Linux - drawn by the engine's own software renderer.", pal::text, twk::kSubText, body.w);
            g.text(body.x, body.y + 58, twk::fmt("repo: %s", root.c_str()), pal::faint, twk::kSubText, body.w);
            return !(g.key(PHX_KEY_ESCAPE) || g.key(PHX_KEY_ENTER) || (g.in->pressed & twk::kMouseL && !body.contains(g.mx(), g.my())));
        });
    }

    // --script: one command list, stepped once per frame. The pointer commands move a scripted
    // pointer (the real mouse is ignored while a script runs); key/type inject keyboard events.
    //   click X Y | rclick X Y | dclick X Y | move X Y | drag X0 Y0 X1 Y1 [FRAMES]
    //   wheel X Y N | key [ctrl+][shift+][alt+]NAME | type TEXT | open PATH | wait N
    //   shot FILE.ppm | quit
    static std::vector<ScriptCmd> parse_script(const std::string& text) {
        std::vector<ScriptCmd> out;
        size_t p = 0;
        while (p <= text.size()) {
            size_t e = text.find_first_of(";\n", p);
            if (e == std::string::npos) e = text.size();
            const std::string c = trim(text.substr(p, e - p));
            p = e + 1;
            if (c.empty() || c[0] == '#') continue;
            ScriptCmd sc;
            const size_t sp = c.find(' ');
            sc.op = c.substr(0, sp);
            sc.rest = sp == std::string::npos ? "" : c.substr(sp + 1);
            char arg[256] = { 0 };
            if (std::sscanf(sc.rest.c_str(), "%255s", arg) == 1) sc.arg = arg;
            std::sscanf(sc.rest.c_str(), "%d %d %d %d %d", &sc.a, &sc.b, &sc.c, &sc.d, &sc.e);
            out.push_back(sc);
        }
        return out;
    }
    static bool parse_key(const std::string& spec, int32_t& key, uint16_t& mods) {
        std::string s = lower(spec);
        mods = 0;
        for (;;) {
            if (s.compare(0, 5, "ctrl+") == 0) { mods |= twk::kCtrl; s = s.substr(5); }
            else if (s.compare(0, 6, "shift+") == 0) { mods |= twk::kShift; s = s.substr(6); }
            else if (s.compare(0, 4, "alt+") == 0) { mods |= twk::kAlt; s = s.substr(4); }
            else break;
        }
        static const struct { const char* n; int32_t k; } names[] = {
            { "enter", PHX_KEY_ENTER }, { "return", PHX_KEY_ENTER }, { "tab", PHX_KEY_TAB }, { "esc", PHX_KEY_ESCAPE },
            { "escape", PHX_KEY_ESCAPE }, { "backspace", PHX_KEY_BACKSPACE }, { "delete", PHX_KEY_DELETE },
            { "left", PHX_KEY_LEFT }, { "right", PHX_KEY_RIGHT }, { "up", PHX_KEY_UP }, { "down", PHX_KEY_DOWN },
            { "home", PHX_KEY_HOME }, { "end", PHX_KEY_END }, { "pgup", PHX_KEY_PAGE_UP }, { "pgdn", PHX_KEY_PAGE_DOWN },
            { "space", ' ' }, { "f1", PHX_KEY_F1 }, { "f2", PHX_KEY_F2 }, { "f3", PHX_KEY_F3 }, { "f5", PHX_KEY_F5 } };
        for (const auto& n : names) if (s == n.n) { key = n.k; return true; }
        if (s.size() == 1) { key = int32_t(static_cast<unsigned char>(s[0])); return true; }
        return false;
    }

    void step_script(App& app) {
        if (script.empty()) return;
        // the scripted pointer replaces the real one
        tin.mx = sc_x; tin.my = sc_y; tin.held = sc_held;
        tin.pressed = tin.released = 0; tin.clicks = 0; tin.wheel_x = tin.wheel_y = 0;
        tin.keys.clear(); tin.text.clear();
        if (!sc_frames.empty()) {
            auto f = sc_frames.front();
            sc_frames.erase(sc_frames.begin());
            f(tin);
            sc_x = tin.mx; sc_y = tin.my; sc_held = tin.held;
            return;
        }
        if (script_wait > 0) { --script_wait; return; }
        while (script_pc < script.size()) {
            const ScriptCmd& c = script[script_pc++];
            auto press = [this](int x, int y, uint32_t b, int clicks) {
                sc_frames.push_back([=](twk::Input& in) { in.mx = x; in.my = y; in.pressed |= b; in.held |= b; in.clicks = clicks; });
                sc_frames.push_back([=](twk::Input& in) { in.mx = x; in.my = y; in.released |= b; in.held &= ~b; });
            };
            if (c.op == "move")  { sc_x = c.a; sc_y = c.b; tin.mx = sc_x; tin.my = sc_y; continue; }
            if (c.op == "click") { press(c.a, c.b, twk::kMouseL, 1); break; }
            if (c.op == "rclick") { press(c.a, c.b, twk::kMouseR, 1); break; }
            if (c.op == "dclick") {
                press(c.a, c.b, twk::kMouseL, 1);
                sc_frames.push_back([=](twk::Input& in) { in.mx = c.a; in.my = c.b; in.pressed |= twk::kMouseL; in.held |= twk::kMouseL; in.clicks = 2; });
                sc_frames.push_back([=](twk::Input& in) { in.mx = c.a; in.my = c.b; in.released |= twk::kMouseL; in.held &= ~twk::kMouseL; });
                break;
            }
            if (c.op == "drag") {
                const int n = c.e > 0 ? c.e : 8;
                const int x0 = c.a, y0 = c.b, x1 = c.c, y1 = c.d;
                sc_frames.push_back([=](twk::Input& in) { in.mx = x0; in.my = y0; in.pressed |= twk::kMouseL; in.held |= twk::kMouseL; in.clicks = 1; });
                for (int k = 1; k <= n; ++k)
                    sc_frames.push_back([=](twk::Input& in) { in.mx = x0 + (x1 - x0) * k / n; in.my = y0 + (y1 - y0) * k / n; in.held |= twk::kMouseL; });
                sc_frames.push_back([=](twk::Input& in) { in.mx = x1; in.my = y1; in.released |= twk::kMouseL; in.held &= ~twk::kMouseL; });
                break;
            }
            if (c.op == "wheel") { const int n = c.c; sc_frames.push_back([=](twk::Input& in) { in.mx = c.a; in.my = c.b; in.wheel_y = n; }); break; }
            if (c.op == "key") {
                int32_t k = 0; uint16_t m = 0;
                if (!parse_key(c.arg, k, m)) { std::fprintf(stderr, "phxstudio: --script: bad key '%s'\n", c.arg.c_str()); continue; }
                sc_frames.push_back([=](twk::Input& in) { in.key(k, m); in.mods = m; });
                break;
            }
            if (c.op == "type") { const std::string t = c.rest; sc_frames.push_back([=](twk::Input& in) { in.text += t; }); break; }
            if (c.op == "open") { switch_tab(Tab::Editor); ws.open(host, resolve_user_path(c.arg)); continue; }
            if (c.op == "project") { open_project(resolve_user_path(c.arg)); continue; }
            if (c.op == "tab") { for (int t = 0; t < int(Tab::Count); ++t) if (lower(kTabNames[t]) == lower(c.arg)) switch_tab(Tab(t)); continue; }
            if (c.op == "wait")  { script_wait = std::max(0, c.a - 1); return; }
            if (c.op == "shot")  { pending_shot = c.arg; return; }
            if (c.op == "quit")  { app.request_quit(); return; }
            std::fprintf(stderr, "phxstudio: --script: unknown command '%s'\n", c.op.c_str());
        }
        if (!sc_frames.empty()) {
            auto f = sc_frames.front();
            sc_frames.erase(sc_frames.begin());
            f(tin);
            sc_x = tin.mx; sc_y = tin.my; sc_held = tin.held;
        }
    }

    // --shot: read the PRESENTED frame back through the SDL backend and write a binary PPM
    // (docs, bug reports, and a scriptable visual check of every view).
    static void write_shot(const std::string& path) {
        std::vector<uint32_t> px(size_t(kW) * size_t(kH));
        if (phx_sdl_readback(px.data(), kW, kH) != 0) { std::fprintf(stderr, "phxstudio: readback failed\n"); return; }
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) { std::fprintf(stderr, "phxstudio: cannot write %s\n", path.c_str()); return; }
        std::fprintf(f, "P6\n%d %d\n255\n", kW, kH);
        for (uint32_t c : px) { const uint8_t rgb[3] = { rgba_r(c), rgba_g(c), rgba_b(c) }; std::fwrite(rgb, 1, 3, f); }
        std::fclose(f);
        std::printf("phxstudio: wrote %s\n", path.c_str());
    }

    void switch_tab(Tab t) {
        if (!tab_visible(t)) return;                     // e.g. Overview outside --engine-dev
        tab = t;
        if (t != Tab::Assets && snd_playing) { queue.stop_all(); snd_playing = false; }
    }

    // key auto-repeat: true on the press, then every 3 frames after a 18-frame hold
    bool repeat(const InputState& in, Button b) {
        if (in.just(b)) { key_hold = 0; return true; }
        if (in.down(b)) { ++key_hold; return key_hold > 18 && key_hold % 3 == 0; }
        return false;
    }

    // ---------------------------------------------------------------------------------------
    // chrome
    // ---------------------------------------------------------------------------------------
    void draw_top_bar() {
        using twk::MenuItem;
        tg.rect(twk::Rect{ 0, 0, kW, kTopH }, pal::bar, twk::kSubFill);
        tg.rect(twk::Rect{ 0, kTopH - 1, kW, 1 }, pal::line, twk::kSubWidget);
        tg.image(twk::Rect{ 4, 3, 12, 12 }, flame_tex, 0, 0, 12, 12);
        tg.tip(twk::Rect{ 2, 0, 16, kTopH }, "Phoenix Studio v" PHX_VERSION_STRING);
        // ---- menus ----
        int x = 20;
        auto menu_at = [&](const char* label, uint32_t id) {
            const twk::Rect r{ x, 1, twk::Gui::text_w(label) + 12, kTopH - 2 };
            tg.menu_button(id, r, label);
            x += r.w;
            return id;
        };
        DocView* d = ws.current();
        const bool ed = tab == Tab::Editor && d && (engine_dev || has_project);
        const std::string dir = ws.selected_dir();
        const uint32_t m_file = menu_at("File", tg.id("m-file"));
        const uint32_t m_edit = menu_at("Edit", tg.id("m-edit"));
        const uint32_t m_view = menu_at("View", tg.id("m-view"));
        const uint32_t m_build = menu_at("Build", tg.id("m-build"));
        const uint32_t m_help = menu_at("Help", tg.id("m-help"));
        {
            // File menu: (item, action). Asset creation needs an open workspace; projects can be
            // created / opened / closed from anywhere.
            const bool wsp = engine_dev || has_project;
            std::vector<MenuItem> items;
            std::vector<std::function<void()>> acts;
            auto add = [&](MenuItem m, std::function<void()> f) { items.push_back(std::move(m)); acts.push_back(std::move(f)); };
            auto sep = [&] { items.push_back(MenuItem::sep()); acts.push_back(nullptr); };
            add(MenuItem{ "New project...", "", true, false, false, twk::kIconPlus }, [this] { new_project_dialog(); });
            add(MenuItem{ "Open project...", "", true, false, false, twk::kIconFolderOpen }, [this] { open_project_dialog(); });
            if (has_project)
                add(MenuItem{ "Close project", "", true, false, false, twk::kIconClose },
                    [this] { with_saved_docs("Close project", [this] { close_project(); }); });
            sep();
            add(MenuItem{ "New sprite...", "", wsp, false, false, twk::kIconSprite }, [this, dir] { switch_tab(Tab::Editor); ws.new_sprite(host, dir); });
            add(MenuItem{ "New tilemap...", "", wsp, false, false, twk::kIconFileMap }, [this, dir] { switch_tab(Tab::Editor); ws.new_map(host, dir); });
            add(MenuItem{ "New data table...", "", wsp, false, false, twk::kIconFileTable }, [this, dir] { switch_tab(Tab::Editor); ws.new_table(host, dir); });
            add(MenuItem{ "New image / tileset...", "", wsp, false, false, twk::kIconFileImage }, [this, dir] { switch_tab(Tab::Editor); ws.new_image(host, dir); });
            add(MenuItem{ "New file...", "", wsp, false, false, twk::kIconFileCode }, [this, dir] { switch_tab(Tab::Editor); ws.new_code(host, dir); });
            sep();
            add(MenuItem{ "Quick open...", "Ctrl+P", wsp, false, false, twk::kIconSearch }, [this] { switch_tab(Tab::Editor); ws.quick_open(host); });
            sep();
            add(MenuItem{ "Save", "Ctrl+S", ed && d->dirty(), false, false, twk::kIconSave }, [this] { ws.save(host, ws.active); });
            add(MenuItem{ "Save all", "Ctrl+Shift+S", ws.any_dirty() }, [this] { ws.save_all(host); });
            add(MenuItem{ "Revert to disk", "", ed }, [this, d] { std::string err; if (d && !d->reload(host, &err)) toast("revert failed: " + err, Toast::Bad); });
            add(MenuItem{ "Close tab", "Ctrl+W", ed, false, false, twk::kIconClose }, [this] { ws.close(host, ws.active); });
            sep();
            add(MenuItem{ "Quit", "Ctrl+Q" }, [this] { request_quit(); });
            const int hit = tg.menu(m_file, items, 180);
            if (hit >= 0 && size_t(hit) < acts.size() && acts[size_t(hit)]) acts[size_t(hit)]();
        }
        {
            std::vector<MenuItem> items = { MenuItem{ "Undo", "Ctrl+Z", ed, false, false, twk::kIconUndo },
                                            MenuItem{ "Redo", "Ctrl+Y", ed, false, false, twk::kIconRedo } };
            std::vector<DocView::Action> acts = ed ? d->actions() : std::vector<DocView::Action>{};
            if (!acts.empty()) items.push_back(MenuItem::sep());
            for (const DocView::Action& a : acts) items.push_back(MenuItem{ a.label, a.shortcut, a.enabled });
            const int hit = tg.menu(m_edit, items, 180);
            if (hit == 0 && d) d->undo();
            if (hit == 1 && d) d->redo();
            if (hit >= 3 && size_t(hit - 3) < acts.size()) acts[size_t(hit - 3)].run();
        }
        const std::vector<Tab> vt = visible_tabs();
        {
            std::vector<MenuItem> items;
            std::vector<std::function<void()>> acts;
            for (size_t t = 0; t < vt.size(); ++t) {
                const Tab tv = vt[t];
                items.push_back(MenuItem{ kTabNames[int(tv)], twk::fmt("Ctrl+%d", int(t) + 1), true, tab == tv });
                acts.push_back([this, tv] { switch_tab(tv); });
            }
            if (!vt.empty()) { items.push_back(MenuItem::sep()); acts.push_back(nullptr); }
            items.push_back(MenuItem{ "Explorer", "Ctrl+B", !vt.empty(), ws.side_open });
            acts.push_back([this] { ws.side_open = !ws.side_open; switch_tab(Tab::Editor); });
            items.push_back(MenuItem{ "Bigger UI", "Ctrl+=", phx_desktop_scale() < 6, false, false, twk::kIconZoomIn });
            acts.push_back([] { phx_desktop_set_scale(phx_desktop_scale() + 1); });
            items.push_back(MenuItem{ "Smaller UI", "Ctrl+-", phx_desktop_scale() > 1, false, false, twk::kIconZoomOut });
            acts.push_back([] { phx_desktop_set_scale(std::max(1, phx_desktop_scale() - 1)); });
            const int hit = tg.menu(m_view, items, 150);
            if (hit >= 0 && size_t(hit) < acts.size() && acts[size_t(hit)]) acts[size_t(hit)]();
        }
        // ---- view tabs (the project's name first, in project mode) ----
        x += 10;
        if (has_project) {
            const std::string pn = project.name;
            const twk::Rect pr{ x, 2, std::min(120, twk::Gui::text_w(pn) + 16), kTopH - 3 };
            tg.icon(pr.x, 5, twk::kIconFileMap, pal::accent);
            tg.text(pr.x + 13, 6, pn, pal::accent, twk::kSubText, pr.w - 13);
            tg.tip(pr, "Project: " + project.dir + " - only this folder is editable");
            x += pr.w + 6;
        }
        for (Tab tv : vt) {
            const int t = int(tv);
            const twk::Rect r{ x, 2, twk::Gui::text_w(kTabNames[t]) + 16, kTopH - 3 };
            const bool on = int(tab) == t;
            const bool h = tg.hover(r);
            tg.rect(r, on ? pal::panel2 : h ? pal::panel : pal::bar, twk::kSubWidget);
            if (on) tg.rect(twk::Rect{ r.x, r.bottom() - 2, r.w, 2 }, pal::accent, twk::kSubImage);
            tg.text(r.x + 8, 6, kTabNames[t], on ? pal::text : pal::dim);
            int pos = 0;
            for (size_t k = 0; k < vt.size(); ++k) if (vt[k] == tv) pos = int(k);
            tg.tip(r, twk::fmt("%s (Ctrl+%d)", kTabNames[t], pos + 1));
            if (tg.clicked(r)) switch_tab(Tab(t));
            x += r.w + 2;
        }
        // ---- job indicator (right) ----
        const int rid = jobs ? jobs->running_id() : -1;
        std::string s;
        Rgba c = pal::faint;
        if (rid >= 0) {
            static const char kSpin[4] = { '|', '/', '-', '\\' };
            s = twk::fmt("%c %s  %.0fs", kSpin[(ticks / 8) % 4], jobs->running_label().c_str(), jobs->running_seconds());
            const size_t q = jobs->queued();
            if (q) s += twk::fmt("  +%zu queued", q);
            c = pal::accent;
        } else if (!diags.empty()) {
            int e = 0, w = 0;
            for (const Diag& dg : diags) { e += dg.severity == 2; w += dg.severity == 1; }
            s = twk::fmt("%d error%s, %d warning%s", e, e == 1 ? "" : "s", w, w == 1 ? "" : "s");
            c = e ? pal::bad : pal::warn;
        }
        const int avail = kW - x - 10;
        if (!s.empty() && avail > 40) {
            const int w = std::min(avail, twk::Gui::text_w(s));
            const twk::Rect jr{ kW - 6 - w, 2, w, kTopH - 3 };
            tg.text(jr.x, 6, s, c, twk::kSubText, w);
            tg.tip(jr, rid >= 0 ? "Jobs run one at a time; the Run view has the output" : "From the last build - click to see them in the Run view");
            if (tg.clicked(jr)) switch_tab(Tab::Run);
        }
        // Build menu (games + gates from the launch catalog)
        {
            std::vector<MenuItem> items;
            std::vector<int> ids;
            for (size_t i = 0; i < launches.size(); ++i) {
                const Launch& l = launches[i];
                if (l.group == Group::Suite) continue;
                items.push_back(MenuItem{ l.label, "", launch_missing[i].empty(), false, false,
                                          l.group == Group::Play ? twk::kIconPlay : twk::kIconBuild });
                ids.push_back(int(i));
                if (items.size() >= 18) break;
            }
            items.push_back(MenuItem::sep());
            items.push_back(MenuItem{ "Re-run the last launch", "F5", last_launch >= 0, false, false, twk::kIconRefresh });
            items.push_back(MenuItem{ "Stop the running job", "", rid >= 0 && jobs && jobs->can_stop(), false, false, twk::kIconStop });
            const int hit = tg.menu(m_build, items, 200);
            if (hit >= 0 && hit < int(ids.size())) { run_launch(ids[size_t(hit)]); switch_tab(Tab::Run); }
            else if (hit == int(ids.size()) + 1 && last_launch >= 0) { run_launch(last_launch); switch_tab(Tab::Run); }
            else if (hit == int(ids.size()) + 2 && jobs) { jobs->cancel_queue(); jobs->stop_current(); }
        }
        switch (tg.menu(m_help, { MenuItem{ "Keyboard shortcuts", "F1", true, false, false, twk::kIconGear },
                                  MenuItem{ "Studio guide (instructions.md)", "", engine_dev || has_project, false, false, twk::kIconFile },
                                  MenuItem{ "About", "" } }, 170)) {
        case 0: show_shortcuts(); break;
        case 1: switch_tab(Tab::Editor); ws.open(host, join_path(root, "tools/phxstudio/instructions.md")); break;
        case 2: show_about(); break;
        default: break;
        }
    }

    void draw_status_bar(App&) {
        const int y = kH - kBotH;
        tg.rect(twk::Rect{ 0, y, kW, kBotH }, pal::bar, twk::kSubFill);
        tg.rect(twk::Rect{ 0, y, kW, 1 }, pal::line, twk::kSubWidget);
        std::string right = twk::fmt("%u fps  %.1f ms", fps, double(render_avg.avg_us) / 1000.0);
        if (tab != Tab::Editor) right += twk::fmt("  %u sprites", unsigned(last_stats.sprites_submitted));
        std::string left = tg.hint;
        Rgba lc = pal::text;
        if (left.empty()) {
            DocView* d = tab == Tab::Editor ? ws.current() : nullptr;
            if (d) { left = d->status(); lc = pal::dim; }
            else if (!engine_dev && !has_project) { left = "Open or create a project to start   F1: shortcuts"; lc = pal::faint; }
            else { left = twk::fmt("Ctrl+P: open a file   F1: shortcuts   Ctrl+1..%zu: views", visible_tabs().size()); lc = pal::faint; }
        }
        const int pw = twk::Gui::text_w(right);
        tg.text(kW - 6 - pw, y + 3, right, pal::faint);
        tg.text(6, y + 3, left, lc, twk::kSubText, kW - pw - 20);
    }

    void section(int x, int y, int w, const std::string& title, Rgba c = pal::dim) {
        gui.text(x, y, title, c, kLyText);
        gui.rect(Rect{ x + Gui::text_w(title) + 4, y + 4, std::max(0, w - Gui::text_w(title) - 4), 1 }, pal::line, kLyWidget);
    }

    // ---------------------------------------------------------------------------------------
    // OVERVIEW
    // ---------------------------------------------------------------------------------------
    struct Box { Rect r; int mod; };
    static constexpr int kGraphLabelW = 76;

    std::vector<Box> layout_graph(int x0, int y0, int w, int row_h) const {
        std::vector<Box> boxes;
        const int nl = int(engine.layers.size());
        const int label_w = kGraphLabelW;
        for (int l = 0; l < nl; ++l) {
            const auto& mods = engine.layers[size_t(l)];
            const int n = int(mods.size());
            const int avail = w - label_w;
            const int bw = std::min(76, (avail - (n - 1) * 6) / std::max(1, n));
            const int total = n * bw + (n - 1) * 6;
            int bx = x0 + label_w + (avail - total) / 2;
            const int by = y0 + (nl - 1 - l) * row_h;
            for (const std::string& m : mods) {
                boxes.push_back(Box{ Rect{ bx, by, bw, 22 }, engine.find(m) });
                bx += bw + 6;
            }
        }
        return boxes;
    }

    void draw_overview() {
        const int gx = 8, gy = kBodyY + 18, gw = 396, row_h = 40;
        section(gx, kBodyY + 5, gw, "MODULE GRAPH  (layers: depcheck.py, edges: real #includes)");
        if (engine.modules.empty()) {
            gui.text(gx, gy + 10, "could not scan the engine: " + engine_err, pal::bad);
            return;
        }
        const std::vector<Box> boxes = layout_graph(gx, gy, gw, row_h);
        auto box_of = [&](int m) -> const Rect* {
            for (const Box& b : boxes) if (b.mod == m) return &b.r;
            return nullptr;
        };
        int hover_mod = -1;
        for (const Box& b : boxes) if (gui.hover(b.r)) hover_mod = b.mod;
        const int focus = hover_mod >= 0 ? hover_mod : mod_sel;

        // layer labels
        const int nl = int(engine.layers.size());
        for (int l = 0; l < nl; ++l) {
            const int by = gy + (nl - 1 - l) * row_h;
            const Rect lr{ gx, by - 2, kGraphLabelW - 4, 30 };
            gui.rect(Rect{ gx, by, 3, 22 }, layer_colour(l), kLyWidget);
            gui.text(gx + 7, by, fmt("L%d", l), layer_colour(l));
            // depcheck's note, up to its first '(' / ';', as two short lines under the badge
            const std::string note = l < int(engine.layer_notes.size()) ? engine.layer_notes[size_t(l)] : "";
            const auto lines = wrap(trim(note.substr(0, note.find_first_of("(;"))), 11);
            for (size_t k = 0; k < lines.size() && k < 2; ++k)
                gui.text(gx + 7, by + 9 + int(k) * 8, lines[k], pal::faint, kLyText, kGraphLabelW - 6);
            if (gui.hover(lr)) gui.hint = fmt("L%d: %s", l, note.c_str());
        }

        // edges: all faint, the focused module's in colour (drawn under the boxes)
        for (const Module& m : engine.modules) {
            const int mi = engine.find(m.name);
            for (int d : m.deps) {
                const Rect* a = box_of(mi);
                const Rect* b = box_of(d);
                if (!a || !b) continue;
                Rgba c = pal::line;
                uint8_t ly = kLyEdge;
                if (mi == focus)      { c = pal::good;   ly = kLyEdge + 2; }
                else if (d == focus)  { c = pal::accent; ly = kLyEdge + 2; }
                else if (focus >= 0)  { c = scale_rgb(pal::line, 2, 3); }
                gui.line_px(a->x + a->w / 2, a->y + a->h, b->x + b->w / 2, b->y - 1, c, ly);
            }
        }

        // boxes
        for (const Box& b : boxes) {
            if (b.mod < 0) continue;
            const Module& m = engine.modules[size_t(b.mod)];
            const bool sel = b.mod == mod_sel;
            const bool uses = focus >= 0 && std::find(engine.modules[size_t(focus)].deps.begin(),
                                                      engine.modules[size_t(focus)].deps.end(), b.mod) !=
                                                engine.modules[size_t(focus)].deps.end();
            const bool used = focus >= 0 && std::find(m.deps.begin(), m.deps.end(), focus) != m.deps.end();
            const Rgba lc = layer_colour(m.layer);
            const Rgba fill = b.mod == focus ? scale_rgb(lc, 1, 2) : scale_rgb(lc, 1, 4);
            gui.rect(b.r, fill, kLyPanel);
            Rgba border = b.mod == focus ? pal::text : uses ? pal::good : used ? pal::accent : scale_rgb(lc, 2, 3);
            gui.frame(b.r, border, kLyWidget);
            if (sel) gui.frame(b.r.inset(-2), pal::accent, kLyWidget);
            gui.text_center(Rect{ b.r.x, b.r.y + 1, b.r.w, 12 }, m.name, pal::text);
            gui.text_center(Rect{ b.r.x, b.r.y + 11, b.r.w, 10 }, fmt("%dL", m.lines), scale_rgb(lc, 3, 4));
            if (gui.hover(b.r)) gui.hint = fmt("%s (L%d): %zu deps, %zu dependents, %d files - click for details",
                                              m.name.c_str(), m.layer, m.deps.size(), m.users.size(), m.files);
            if (gui.clicked(b.r)) mod_sel = b.mod;
        }

        // legend
        const int ly = gy + nl * row_h - 12;
        int lx = gx + kGraphLabelW;
        gui.rect(Rect{ lx, ly + 3, 10, 2 }, pal::good, kLyWidget);   lx += 14;
        lx += gui.text(lx, ly, "uses", pal::dim) + 12;
        gui.rect(Rect{ lx, ly + 3, 10, 2 }, pal::accent, kLyWidget); lx += 14;
        lx += gui.text(lx, ly, "used by", pal::dim) + 16;
        const bool law_ok = engine.violations.empty();
        gui.text(lx, ly, fmt("%d edges, %zu violations", engine.edges, engine.violations.size()),
                 law_ok ? pal::good : pal::bad);

        draw_caps(gx, ly + 16, gw);
        draw_module_card(412, kBodyY + 5, kW - 412 - 8);
    }

    void draw_caps(int x, int y, int w) {
        section(x, y, w, "CAPABILITY TIERS  (phx/core/caps.h - one codebase, three machines)");
        if (tiers.size() < 3) { gui.text(x, y + 14, "caps.h could not be parsed", pal::bad); return; }
        y += 13;
        const int label_w = 76, col_w = (w - label_w) / 3;
        for (size_t t = 0; t < 3; ++t) {
            const Rgba c = t == 0 ? pal::good : t == 1 ? pal::info : pal::accent;
            gui.text(x + label_w + int(t) * col_w, y, fmt("%s (tier %d)", tiers[t].name.c_str(), tiers[t].render_tier), c);
        }
        y += 11;
        struct Row { const char* name; uint64_t v[3]; bool bytes; bool bar; const char* help; };
        Row rows[] = {
            { "main RAM",  { tiers[0].total_ram, tiers[1].total_ram, tiers[2].total_ram }, true, true,
              "Engine-available main RAM: the budget the root arena is carved from" },
            { "scratch",   { tiers[0].scratch_ram, tiers[1].scratch_ram, tiers[2].scratch_ram }, true, false,
              "Fast scratch RAM (GBA IWRAM) for hot data" },
            { "sprites",   { tiers[0].max_sprites, tiers[1].max_sprites, tiers[2].max_sprites }, false, true,
              "Sprites per frame: the GBA's 128 is the OAM hardware ceiling" },
            { "entities",  { tiers[0].max_entities, tiers[1].max_entities, tiers[2].max_entities }, false, true,
              "ECS capacity: the same World code, sized per tier" },
            { "audio ch",  { tiers[0].audio_channels, tiers[1].audio_channels, tiers[2].audio_channels }, false, true,
              "Mixer voices" },
        };
        for (const Row& row : rows) {
            const Rect rr{ x, y - 1, w, 10 };
            if (gui.hover(rr)) { gui.rect(rr, pal::panel, kLyPanel); gui.hint = row.help; }
            gui.text(x, y, row.name, pal::dim);
            uint64_t mx = 1;
            for (uint64_t v : row.v) mx = std::max(mx, v);
            for (int t = 0; t < 3; ++t) {
                const uint64_t v = row.v[t];
                const int cx = x + label_w + t * col_w;
                if (row.bar && v) {                       // log2 bar: 17,000x gaps stay visible
                    int lv = 0, lm = 0;
                    for (uint64_t k = v; k > 1; k >>= 1) ++lv;
                    for (uint64_t k = mx; k > 1; k >>= 1) ++lm;
                    const int bw = std::max(2, (col_w - 58) * (lv + 1) / (lm + 1));
                    gui.rect(Rect{ cx + 54, y + 2, bw, 4 }, t == 0 ? pal::good : t == 1 ? pal::info : pal::accent, kLyWidget);
                }
                const std::string s = row.bytes ? (v ? human_bytes(v) : "-") : std::to_string(v);
                gui.text(cx, y, s, pal::text, kLyText, 52);
            }
            y += 10;
        }
        const char* flags[3] = { tiers[0].has_float_hw ? "float" : "fixed16", tiers[1].has_float_hw ? "float" : "fixed16",
                                 tiers[2].has_float_hw ? "float" : "fixed16" };
        gui.text(x, y, "scalar", pal::dim);
        for (int t = 0; t < 3; ++t) gui.text(x + label_w + t * col_w, y, flags[t], pal::text);
        if (gui.hover(Rect{ x, y - 1, w, 10 }))
            gui.hint = "phx::scalar: fixed16 (Q16.16) on the FPU-less GBA - `make determinism` proves both agree";
        y += 10;
        gui.text(x, y, "filesystem", pal::dim);
        for (int t = 0; t < 3; ++t) gui.text(x + label_w + t * col_w, y, tiers[size_t(t)].has_filesystem ? "yes" : "no (ROM)", pal::text);
        if (tiers[0].total_ram)
            gui.text(x, y + 12, fmt("PC: %llux the GBA's RAM - the gap the design is built around",
                                   (unsigned long long)(tiers[2].total_ram / tiers[0].total_ram)), pal::faint, kLyText, w);
    }

    void draw_module_card(int x, int y, int w) {
        section(x, y, w, "MODULE");
        if (mod_sel < 0 || mod_sel >= int(engine.modules.size())) {
            gui.text(x, y + 16, "click a module", pal::dim);
            return;
        }
        const Module& m = engine.modules[size_t(mod_sel)];
        y += 14;
        gui.rect(Rect{ x, y, 3, 18 }, layer_colour(m.layer), kLyWidget);
        gui.text(x + 8, y + 1, m.name, pal::text, kLyText, w, 2);
        const std::string note = m.layer < int(engine.layer_notes.size()) ? engine.layer_notes[size_t(m.layer)] : "";
        y += 22;
        gui.text(x, y, fmt("L%d  %s", m.layer, note.c_str()), layer_colour(m.layer), kLyText, w);
        y += 13;
        const int cols = w / kAdv;
        const auto para = wrap(m.summary, size_t(cols));
        for (size_t i = 0; i < para.size() && i < 9; ++i, y += 9) gui.text(x, y, para[i], pal::text);
        if (para.size() > 9) { gui.text(x, y, "...", pal::dim); y += 9; }
        y += 5;
        auto list_line = [&](const std::string& label, const std::string& body, Rgba c) {
            gui.text(x, y, label, pal::dim);
            const auto lines = wrap(body.empty() ? "-" : body, size_t(cols - 10));
            for (size_t i = 0; i < lines.size() && i < 3; ++i, y += 9)
                gui.text(x + 60, y, lines[i], c);
            if (lines.empty()) y += 9;
            y += 2;
        };
        list_line("size", fmt("%d files, %d lines", m.files, m.lines), pal::text);
        std::string hs;
        for (const std::string& h : m.headers) hs += (hs.empty() ? "" : " ") + h;
        list_line("headers", hs, pal::info);
        std::string bs;
        for (const std::string& b : m.backends) bs += (bs.empty() ? "" : " ") + b;
        list_line("backends", bs.empty() ? "" : bs + "  (one linked)", pal::violet);
        std::string us, ub;
        for (int d : m.deps)  us += (us.empty() ? "" : " ") + engine.modules[size_t(d)].name;
        for (int d : m.users) ub += (ub.empty() ? "" : " ") + engine.modules[size_t(d)].name;
        list_line("uses", us, pal::good);
        list_line("used by", ub, pal::accent);

        // repository totals at the card's foot
        const int fy = kH - kBotH - 32;
        gui.rect(Rect{ x, fy - 4, w, 1 }, pal::line, kLyWidget);
        gui.text(x, fy, fmt("%zu modules, %d files, %d lines", engine.modules.size(),
                            engine.total_files, engine.total_lines), pal::dim, kLyText, w);
        gui.text(x, fy + 10, fmt("%zu bundles, %zu names indexed", bundle_paths.size(), names.size()),
                 pal::dim, kLyText, w);
    }

    // ---------------------------------------------------------------------------------------
    // ASSETS
    // ---------------------------------------------------------------------------------------
    static constexpr int kBundleW = 150, kAssetW = 184;

    Rect preview_rect() const {
        if (wide) return Rect{ 4, kBodyY + 4, kW - 8, kBodyH - 8 };
        const int x = kBundleW + kAssetW + 6;
        return Rect{ x, kBodyY + 4, kW - x - 4, kBodyH - 8 };
    }

    void draw_assets(Renderer& r, const InputState& in) {
        if (!wide) {
            draw_bundle_list();
            draw_asset_list(in);
        }
        draw_preview(r, in);
    }

    void draw_bundle_list() {
        const Rect area{ 4, kBodyY + 4, kBundleW - 4, kBodyH - 8 };
        gui.rect(area, pal::panel, kLyPanel);
        gui.text(area.x + 5, area.y + 4, fmt("BUNDLES (%zu)", bundle_paths.size()), pal::dim);
        const Rect rescan{ area.x + area.w - 40, area.y + 2, 36, 11 };
        if (gui.button(rescan, "scan", false, true, "Re-scan the bundles (after a bake)")) rescan_bundles();
        if (has_project) {
            // Bake: the project's own bake launch, then a rescan when it finishes (edit an asset,
            // bake, see the result here).
            const int bl = bake_launch();
            const Rect bake{ rescan.x - 33, rescan.y, 30, rescan.h };
            const bool running = pending_bake_ >= 0;
            if (gui.button(bake, running ? "..." : "bake", running, bl >= 0 && !running,
                           bl >= 0 ? "Bake the project's assets (" + launches[size_t(bl)].label + ") and reload the bundle"
                                   : "This project has no bake launch (a launch whose command runs game-assets)"))
                if (bl >= 0) { run_launch(bl); pending_bake_ = bl; }
        }
        const int row_h = 11, top = area.y + 17;
        const int visible = (area.h - 20) / row_h;
        const int n = int(bundle_paths.size());
        bundle_scroll = clamp_scroll(bundle_scroll, n, visible);
        for (int i = 0; i < visible && bundle_scroll + i < n; ++i) {
            const int bi = bundle_scroll + i;
            const Rect rr{ area.x + 2, top + i * row_h, area.w - 10, row_h };
            const BundleDoc* d = docs[bundle_paths[size_t(bi)]].get();
            const bool sel = bi == bundle_sel;
            if (sel) gui.rect(rr, pal::panel2, kLyWidget);
            else if (gui.hover(rr)) gui.rect(rr, pal::hover, kLyWidget);
            const Rgba dot = !d->ok ? pal::bad : d->crc < 0 ? pal::warn : pal::good;
            gui.rect(Rect{ rr.x + 3, rr.y + 4, 4, 4 }, dot, kLyOver);
            gui.text(rr.x + 11, rr.y + 2, bundle_label(bundle_paths[size_t(bi)]), sel ? pal::text : pal::dim, kLyText, rr.w - 13);
            if (gui.hover(rr))
                gui.hint = d->ok ? fmt("%s: %zu assets, %s, %s", bundle_paths[size_t(bi)].c_str(), d->assets.size(),
                                       human_bytes(d->file_size).c_str(), tier_name(d->hdr.target))
                                 : fmt("%s: INVALID - %s", bundle_paths[size_t(bi)].c_str(), d->error.c_str());
            if (gui.clicked(rr)) select_bundle(bi);
        }
        gui.scrollbar(1, Rect{ area.x + area.w - 7, top, 5, visible * row_h }, n, visible, bundle_scroll);
    }

    void draw_asset_list(const InputState& in) {
        const Rect area{ kBundleW + 2, kBodyY + 4, kAssetW, kBodyH - 8 };
        gui.rect(area, pal::panel, kLyPanel);
        if (!doc) { gui.text(area.x + 5, area.y + 4, "no bundle", pal::dim); return; }
        int y = area.y + 4;
        gui.text(area.x + 5, y, "ASSETS", pal::dim);
        y += 11;
        if (!doc->ok) {
            gui.text(area.x + 5, y, "refused at mount():", pal::bad);
            y += 10;
            for (const std::string& l : wrap(doc->error, size_t((area.w - 10) / kAdv))) { gui.text(area.x + 5, y, l, pal::text); y += 9; }
            return;
        }
        // header card
        const Rgba tc = doc->hdr.target == 0 ? pal::good : doc->hdr.target == 1 ? pal::info : pal::accent;
        gui.text(area.x + 5, y, tier_name(doc->hdr.target), tc, kLyText, area.w - 10);
        y += 10;
        uint32_t lz = 0;
        for (const AssetEntry& a : doc->assets) lz += (a.flags & kTocLZ) ? 1 : 0;
        gui.text(area.x + 5, y, fmt("%s  %zu assets  LZ %u", human_bytes(doc->file_size).c_str(), doc->assets.size(), lz),
                 pal::dim, kLyText, area.w - 10);
        y += 10;
        gui.text(area.x + 5, y, doc->crc > 0 ? "CRC32 ok" : doc->crc < 0 ? "CRC32 MISMATCH" : "CRC32 not recorded",
                 doc->crc > 0 ? pal::good : doc->crc < 0 ? pal::warn : pal::dim);
        if (doc->raw_bytes()) {
            const std::string ratio = fmt("stored %d%%", int(doc->stored_bytes() * 100 / doc->raw_bytes()));
            gui.text(area.x + area.w - 6 - Gui::text_w(ratio), y, ratio, pal::faint);
        }
        y += 13;

        // keyboard: Up/Down (except when a map pans) and Q/E step the selection
        const AssetEntry* cur = current_asset();
        const bool arrows_pan = cur && cur->type == AssetType::Tilemap;
        const int n = int(order.size());
        int step = 0;
        if (repeat(in, Button::L) || (!arrows_pan && repeat(in, Button::Up)))   step = -1;
        if (repeat(in, Button::R) || (!arrows_pan && repeat(in, Button::Down))) step = 1;
        const int row_h = 11;
        const int visible = (area.y + area.h - y - 2) / row_h;
        if (step && n) {
            select_asset(std::max(0, std::min(n - 1, asset_sel + step)));
            if (asset_sel < asset_scroll) asset_scroll = asset_sel;
            if (asset_sel >= asset_scroll + visible) asset_scroll = asset_sel - visible + 1;
        }
        asset_scroll = clamp_scroll(asset_scroll, n, visible);
        for (int i = 0; i < visible && asset_scroll + i < n; ++i) {
            const int row = asset_scroll + i;
            const AssetEntry& a = doc->assets[size_t(order[size_t(row)])];
            const Rect rr{ area.x + 2, y + i * row_h, area.w - 10, row_h };
            const bool sel = row == asset_sel;
            if (sel) gui.rect(rr, pal::panel2, kLyWidget);
            else if (gui.hover(rr)) gui.rect(rr, pal::hover, kLyWidget);
            const Rgba tcol = type_colour(a.type);
            gui.rect(Rect{ rr.x + 2, rr.y + 2, 2, 7 }, tcol, kLyOver);
            gui.text(rr.x + 8, rr.y + 2, type_name(a.type), scale_rgb(tcol, 3, 4), kLyText, 44);
            gui.text(rr.x + 52, rr.y + 2, names.label(a.hash), sel ? pal::text : pal::dim, kLyText, rr.w - 54);
            if (gui.hover(rr))
                gui.hint = fmt("%s '%s': %s, %s%s", type_name(a.type), names.label(a.hash).c_str(), describe(a).c_str(),
                               human_bytes(a.usize).c_str(), (a.flags & kTocLZ) ? fmt(" (LZ -> %s)", human_bytes(a.size).c_str()).c_str() : "");
            if (tg.double_clicked(Gui::tr(rr))) { select_asset(row); edit_asset_source(a); }
            else if (gui.clicked(rr)) select_asset(row);
        }
        gui.scrollbar(2, Rect{ area.x + area.w - 7, y, 5, visible * row_h }, n, visible, asset_scroll);
    }

    // Header + toolbar row of the preview; returns the body rect below it.
    Rect preview_header(const Rect& p, const AssetEntry& a) {
        const std::string label = names.label(a.hash);
        const int lw = std::min(Gui::text_w(label), p.w - 240);
        gui.text(p.x + 6, p.y + 5, label, pal::text, kLyText, p.w - 240, 1);
        if (a.type != AssetType::Sound) {
            const std::string src = asset_source(a);
            twk::Btn eb;
            eb.icon = twk::kIconPencil;
            eb.enabled = !src.empty();
            const std::string help = src.empty() ? "No source file with this asset's name in the " + std::string(has_project ? "project" : "checkout")
                                                 : "Open " + (path_within(src, ws_root) ? src.substr(ws_root.size() + 1) : src) +
                                                   " in its editor (double-click the asset too); bake to see the change here";
            eb.help = help.c_str();
            if (tg.button(twk::Rect{ p.x + 12 + lw, p.y + 2, Gui::text_w("edit source") + 24, 13 }, "edit source", eb)) edit_asset_source(a);
        }
        const std::string d = describe(a);
        gui.text(p.x + p.w - 6 - std::min(Gui::text_w(d), 150), p.y + 5, d, type_colour(a.type), kLyText, 150);
        return Rect{ p.x + 4, p.y + 30, p.w - 8, p.h - 34 };
    }

    void draw_preview(Renderer& r, const InputState& in) {
        const Rect p = preview_rect();
        const AssetEntry* a = current_asset();
        // the map preview shows THROUGH the panel: draw the panel as a frame, not a fill
        if (!(map_visible && a && a->type == AssetType::Tilemap)) gui.rect(p, pal::panel, kLyPanel);
        else {
            gui.rect(Rect{ p.x, p.y, p.w, 30 }, pal::panel, kLyPanel);
            gui.frame(map_rect.inset(-1), pal::line, kLyOver);
        }
        if (!doc) return;
        if (!doc->ok) {
            gui.text(p.x + 10, p.y + 10, "This bundle would be REFUSED by ResourceCache::mount():", pal::bad);
            gui.text(p.x + 10, p.y + 24, doc->error, pal::text, kLyText, p.w - 20);
            const char* why = doc->hdr.magic != kBundleMagic ? "The first 4 bytes are not 'PHXP'."
                            : doc->hdr.version != kBundleVersion ? "Rebake it with the current tools (e.g. run the game that bakes it)."
                            : "The runtime bounds-checks every TOC entry before trusting it.";
            gui.text(p.x + 10, p.y + 40, why, pal::dim, kLyText, p.w - 20);
            return;
        }
        if (!a) { gui.text(p.x + 10, p.y + 10, "empty bundle", pal::dim); return; }
        const int idx = current_index();
        const Rect body = preview_header(p, *a);
        switch (a->type) {
        case AssetType::Texture:
        case AssetType::Font:    preview_texture(r, idx, p, body); break;
        case AssetType::Sprite:  preview_sprite(r, idx, p, body); break;
        case AssetType::Tilemap: preview_tilemap(in, p, body); break;
        case AssetType::Sound:   preview_sound(idx, p, body); break;
        case AssetType::Spawns:  preview_spawns(*a, p, body); break;
        case AssetType::Blob:    preview_blob(*a, body); break;
        }
    }

    void checker(const Rect& r) {
        const int c = 8;
        for (int y = r.y; y < r.y + r.h; y += c)
            for (int x = r.x; x < r.x + r.w; x += c) {
                const bool odd = (((x - r.x) / c) + ((y - r.y) / c)) & 1;
                gui.rect(Rect{ x, y, std::min(c, r.x + r.w - x), std::min(c, r.y + r.h - y) },
                         odd ? rgba(58, 58, 74) : rgba(48, 48, 62), kLyWidget);
            }
    }

    void preview_texture(Renderer& r, int idx, const Rect& p, const Rect& body) {
        prepare_texture(r, idx);
        if (tex_rgba.empty()) { gui.text(body.x + 4, body.y + 4, "cannot decode this texture", pal::bad); return; }
        // tier switch
        const char* labels[3] = { "GBA", "PSP", "PC" };
        const char* helps[3] = {
            "Tier 0: PAL4_TILES - 4bpp 8x8 tiles, 16-colour BGR555 palettes (the PPU's native format)",
            "Tier 1: RGBA8_SWZ - the GU's swizzled block order (a pure reorder: pixel-identical)",
            "Tier 2: RGBA8 - uploaded as-is" };
        int bx = p.x + 6;
        gui.text(bx, p.y + 18, "as baked for:", pal::dim);
        bx += Gui::text_w("as baked for: ");
        for (int t = 2; t >= 0; --t) {
            const Rect br{ bx, p.y + 16, 34, 11 };
            if (gui.button(br, labels[t], tex_tier == t, true, helps[t])) { tex_tier = t; load_tex_tier(r); }
            bx += 37;
        }
        const bool pal4_mode = tex_tier == 0;
        if (pal4_mode && tex_rep.tiles_x) {
            if (gui.button(Rect{ bx + 6, p.y + 16, 40, 11 }, "tiles", tex_grid, true,
                           "The GBA's 8x8 tile grid by colour count: grey <= 8, yellow 9-15, red > 15 (impossible on GBA)"))
                tex_grid = !tex_grid;
        }

        // image area (leave room for the tier report below)
        const Rect img_box{ body.x, body.y, body.w, body.h - 64 };
        const Rect fit = fit_rect(tex_rep.w, tex_rep.h, img_box, 8);
        checker(fit);
        gui.image(fit, tex_id, 0, 0, tex_rep.w, tex_rep.h);
        if (pal4_mode && tex_grid && tex_rep.tiles_x) {
            for (int ty = 0; ty < tex_rep.tiles_y; ++ty)
                for (int tx = 0; tx < tex_rep.tiles_x; ++tx) {
                    const int n = tex_rep.tile_colors[size_t(ty * tex_rep.tiles_x + tx)];
                    if (n == 0) continue;                                  // fully transparent tile
                    const Rect tr{ fit.x + tx * 8 * fit.w / tex_rep.w, fit.y + ty * 8 * fit.h / tex_rep.h,
                                   std::max(1, 8 * fit.w / tex_rep.w), std::max(1, 8 * fit.h / tex_rep.h) };
                    gui.frame(tr, n > 15 ? pal::bad : n > 8 ? pal::warn : pal::faint, uint8_t(n > 8 ? kLyOver + 1 : kLyOver));
                }
        }
        if (gui.hover(fit)) {
            const int tx = (gui.mx - fit.x) * tex_rep.w / std::max(1, fit.w);
            const int ty = (gui.my - fit.y) * tex_rep.h / std::max(1, fit.h);
            if (tx >= 0 && ty >= 0 && tx < tex_rep.w && ty < tex_rep.h) {
                const uint32_t c = tex_rgba[size_t(ty) * tex_rep.w + size_t(tx)];
                gui.hint = fmt("texel (%d,%d) = #%02x%02x%02x a=%u  BGR555 0x%04x", tx, ty, rgba_r(c), rgba_g(c), rgba_b(c),
                               unsigned(rgba_a(c)), unsigned(rgba8_to_bgr555(c)));
                if (tex_rep.tiles_x)
                    gui.hint += fmt("  tile %d,%d: %d colours", tx / 8, ty / 8,
                                    int(tex_rep.tile_colors[size_t((ty / 8) * tex_rep.tiles_x + tx / 8)]));
            }
        }
        if (fit.w != tex_rep.w)
            gui.text(img_box.x, img_box.y + img_box.h - 9, fmt("x%.2g", double(fit.w) / tex_rep.w), pal::faint);

        // tier report
        int y = body.y + body.h - 60;
        gui.rect(Rect{ body.x, y - 3, body.w, 1 }, pal::line, kLyWidget);
        const std::string rgba_sz = human_bytes(tex_rep.rgba_bytes);
        gui.text(body.x, y, "PC ", pal::accent);
        gui.text(body.x + 24, y, "RGBA8 " + rgba_sz, pal::text);
        y += 10;
        gui.text(body.x, y, "PSP", pal::info);
        gui.text(body.x + 24, y, tex_rep.swz_ok ? "RGBA8_SWZ " + human_bytes(tex_rep.swz.size()) + " (same bytes, GU block order)"
                                                : std::string("RGBA8 fallback (size not 4x8-aligned)"),
                 tex_rep.swz_ok ? pal::text : pal::warn, kLyText, body.w - 24);
        y += 10;
        gui.text(body.x, y, "GBA", pal::good);
        if (tex_rep.pal4_ok) {
            const double ratio = double(tex_rep.rgba_bytes) / double(std::max<size_t>(1, tex_rep.pal4.size()));
            gui.text(body.x + 24, y, fmt("PAL4_TILES %s  %.1fx smaller  %u palette%s", human_bytes(tex_rep.pal4.size()).c_str(),
                                         ratio, unsigned(tex_rep.pal4_palettes), tex_rep.pal4_palettes == 1 ? "" : "s"),
                     pal::text, kLyText, body.w - 24);
        } else {
            gui.text(body.x + 24, y, tex_rep.pal4_reason, pal::warn, kLyText, body.w - 24);
        }
        y += 11;
        // the GBA palettes as swatches
        if (tex_rep.pal4_ok && tex_rep.pal4_palettes) {
            const int sw = 5, max_rows = 2;
            const int per_row = std::max(1, (body.w) / (16 * sw + 4));
            int shown = 0;
            for (int pi = 0; pi < tex_rep.pal4_palettes && shown < per_row * max_rows; ++pi, ++shown) {
                const int px = body.x + (shown % per_row) * (16 * sw + 4);
                const int py = y + (shown / per_row) * (sw + 3);
                for (int k = 0; k < 16; ++k) {
                    uint16_t c = 0;
                    std::memcpy(&c, tex_rep.pal4.data() + pal4_palettes_off() + (size_t(pi) * 16 + size_t(k)) * 2, 2);
                    const Rect sr{ px + k * sw, py, sw, sw };
                    if (k == 0) gui.frame(sr, pal::faint, kLyWidget);   // slot 0 = transparent
                    else gui.rect(sr, bgr555_to_rgba8(c), kLyWidget);
                }
                if (gui.hover(Rect{ px, py, 16 * sw, sw })) gui.hint = fmt("GBA palette %d of %u (slot 0 = transparent)", pi, unsigned(tex_rep.pal4_palettes));
            }
            if (tex_rep.pal4_palettes > per_row * max_rows)
                gui.text(body.x + body.w - 60, y + 12, fmt("+%d more", tex_rep.pal4_palettes - per_row * max_rows), pal::faint);
        }
    }

    void preview_sprite(Renderer& r, int idx, const Rect& p, const Rect& body) {
        prepare_sprite(r, idx);
        if (spr_tex == kNoTexture) { gui.text(body.x + 4, body.y + 4, "sprite sheet texture not found in this bundle", pal::bad); return; }
        // clip buttons
        int bx = p.x + 6, by = p.y + 16;
        for (size_t c = 0; c < spr.clips.size(); ++c) {
            const std::string nm = names.label(spr.clips[c].name);
            const Rect br{ bx, by, Gui::text_w(nm) + 10, 11 };
            if (br.x + br.w > p.x + p.w - 4) break;
            const phx::SpriteClipDef& cd = spr.clips[c];
            if (gui.button(br, nm, int(c) == spr_clip, true,
                           fmt("clip '%s': frames %u..%u at %u fps, %s", nm.c_str(), unsigned(cd.first),
                               unsigned(cd.first + cd.count - 1), unsigned(cd.fps), cd.loop ? "looping" : "clamps on the last frame"))) {
                spr_clip = int(c); spr_t0 = ticks;
            }
            bx += br.w + 3;
        }
        const int cols = spr.cols ? spr.cols : std::max(1, spr_tex_w / std::max<int>(1, spr.frame_w));
        int frame = 0;
        if (!spr.clips.empty()) {
            const phx::SpriteClipDef& cd = spr.clips[size_t(std::min<int>(spr_clip, int(spr.clips.size()) - 1))];
            const uint64_t k = cd.fps ? (ticks - spr_t0) * cd.fps / 60 : 0;
            const uint64_t cnt = std::max<uint16_t>(1, cd.count);
            frame = cd.first + int(cd.loop ? k % cnt : std::min<uint64_t>(k, cnt - 1));
        }
        const int fx = (frame % cols) * spr.frame_w, fy = (frame / cols) * spr.frame_h;
        // the animated frame, big
        const Rect stage{ body.x, body.y, body.w, body.h / 2 };
        const Rect fit = fit_rect(spr.frame_w, spr.frame_h, stage.inset(6), 10);
        checker(fit);
        gui.image(fit, spr_tex, fx, fy, spr.frame_w, spr.frame_h);
        gui.text(stage.x, stage.y + stage.h - 9, fmt("frame %d", frame), pal::faint);
        // the whole sheet with the frame grid and the current frame highlighted
        const Rect sheet_box{ body.x, body.y + body.h / 2 + 6, body.w, body.h / 2 - 6 };
        const Rect sf = fit_rect(spr_tex_w, spr_tex_h, sheet_box, 4);
        checker(sf);
        gui.image(sf, spr_tex, 0, 0, spr_tex_w, spr_tex_h);
        const Rect cur{ sf.x + fx * sf.w / std::max<int>(1, spr_tex_w), sf.y + fy * sf.h / std::max<int>(1, spr_tex_h),
                        std::max(2, spr.frame_w * sf.w / std::max<int>(1, spr_tex_w)),
                        std::max(2, spr.frame_h * sf.h / std::max<int>(1, spr_tex_h)) };
        gui.frame(cur, pal::accent, kLyOver);
        if (gui.hover(sf)) gui.hint = fmt("sheet %ux%u, %ux%u frames, %d per row", unsigned(spr_tex_w), unsigned(spr_tex_h),
                                          unsigned(spr.frame_w), unsigned(spr.frame_h), cols);
    }

    void pan_map(const InputState& in) {
        const int step = in.down(Button::B) ? 8 : 3;       // hold X to pan faster
        if (in.down(Button::Left))  { view_x -= step; map_fly = false; }
        if (in.down(Button::Right)) { view_x += step; map_fly = false; }
        if (in.down(Button::Up))    view_y -= step;
        if (in.down(Button::Down))  view_y += step;
        if (map) {
            const int mw = int(map->view.w) * map->view.tile_w, mh = int(map->view.h) * map->view.tile_h;
            view_x = std::max(0, std::min(view_x, std::max(0, mw - map_rect.w)));
            view_y = std::max(0, std::min(view_y, std::max(0, mh - map_rect.h)));
        }
    }

    void preview_tilemap(const InputState&, const Rect& p, const Rect&) {
        if (!map) { gui.text(p.x + 10, p.y + 40, map_err.empty() ? "cannot show this tilemap" : map_err, pal::bad, kLyText, p.w - 20); return; }
        const MapView& v = map->view;
        // toolbar
        int bx = p.x + 6;
        const int by = p.y + 16;
        auto toggle = [&](const char* label, bool& flag, const char* help) {
            const Rect br{ bx, by, Gui::text_w(label) + 10, 11 };
            if (gui.button(br, label, flag, true, help)) flag = !flag;
            bx += br.w + 3;
        };
        if (gui.button(Rect{ bx, by, 34, 11 }, "wide", wide, true, "Toggle the wide preview (hide the lists)")) wide = !wide;
        bx += 37;
        toggle("fly", map_fly, "Auto-scroll the camera to see the parallax layers move");
        toggle("parallax", map_parallax, v.parallax_q16.empty() ? "This map has no parallax factors (all layers 1:1)"
                                                                 : "Per-layer factors from the Tiled map, via Renderer::set_tilemap_parallax");
        toggle("collision", map_collision, "Overlay the gameplay layer's collision: white=solid, yellow=one-way, red=hazard");
        const int spawn_i = doc->find(doc->assets[size_t(map_asset)].hash, AssetType::Spawns);
        if (spawn_i >= 0) toggle("spawns", map_spawns, "Spawn points from the map's object layer");
        const Rect& m = map_rect;
        bx = m.x;
        const int ly = m.y + m.h + 2;
        gui.text(bx, ly + 2, "layers", pal::dim);
        bx += Gui::text_w("layers ");
        for (uint8_t l = 0; l < v.layers && l < 8; ++l) {
            const Rect br{ bx, ly, 16, 11 };
            if (br.x + br.w > p.x + p.w - 4) break;
            bool on = !(layer_hidden & (1u << l));
            std::string help = fmt("layer %u", unsigned(l));
            if (!v.parallax_q16.empty())
                help += fmt(": parallax %.2f x %.2f", double(v.parallax_q16[size_t(l) * 2]) / 65536.0,
                            double(v.parallax_q16[size_t(l) * 2 + 1]) / 65536.0);
            if (l + 1 == v.layers) help += " (the gameplay/solid layer)";
            if (gui.button(br, std::to_string(l), on, true, help)) layer_hidden ^= (1u << l);
            bx += 18;
        }
        // overlays in map space: the gameplay (last) layer scrolls 1:1 with the camera
        auto to_screen_x = [&](int wx) { return m.x + wx - view_x; };
        auto to_screen_y = [&](int wy) { return m.y + wy - view_y; };
        if (map_collision && v.layers) {
            const uint16_t* g = v.indices + size_t(v.layers - 1) * v.w * v.h;
            const int tx0 = view_x / v.tile_w, ty0 = view_y / v.tile_h;
            const int tx1 = std::min<int>(v.w - 1, (view_x + m.w) / v.tile_w);
            const int ty1 = std::min<int>(v.h - 1, (view_y + m.h) / v.tile_h);
            for (int ty = ty0; ty <= ty1; ++ty)
                for (int tx = tx0; tx <= tx1; ++tx) {
                    const uint16_t cell = g[size_t(ty) * v.w + size_t(tx)];
                    if (!cell) continue;
                    uint8_t f = kTileFlagSolid;                          // solid_from fallback
                    if (v.tile_flags) f = cell < v.tile_flag_count ? v.tile_flags[cell] : 0;
                    if (!f) continue;
                    Rect tr{ to_screen_x(tx * v.tile_w), to_screen_y(ty * v.tile_h), v.tile_w, v.tile_h };
                    const Rgba c = (f & kTileFlagHazard) ? pal::bad : (f & kTileFlagSolid) ? rgba(240, 240, 240) : pal::warn;
                    if ((f & kTileFlagOneWay) && !(f & kTileFlagSolid)) gui.rect(Rect{ tr.x, tr.y, tr.w, 2 }, c, kLyOver);
                    else gui.frame(tr, c, kLyOver);
                }
        }
        if (map_spawns && spawn_i >= 0) {
            std::vector<SpawnDef> sp;
            view_spawns(doc->assets[size_t(spawn_i)], sp);
            std::vector<Rect> labels;                          // skip labels that would collide
            for (const SpawnDef& s : sp) {
                const int sx = to_screen_x(s.x), sy = to_screen_y(s.y);
                if (sx < m.x - 4 || sy < m.y - 4 || sx > m.x + m.w - 4 || sy > m.y + m.h - 8) continue;
                const Rect mr{ sx, sy, std::max(6, int(s.w)), std::max(6, int(s.h)) };
                gui.frame(mr, pal::warn, kLyOver);
                const std::string nm = names.label(s.type);
                const Rect lr{ sx, sy - 9, Gui::text_w(nm), 8 };
                bool clash = false;
                for (const Rect& o : labels)
                    if (lr.x < o.x + o.w + 2 && o.x < lr.x + lr.w + 2 && lr.y < o.y + o.h && o.y < lr.y + lr.h) clash = true;
                if (!clash && lr.y > m.y) {
                    gui.text(sx, sy - 9, nm, pal::warn, kLyText, std::max(0, m.x + m.w - sx));
                    labels.push_back(lr);
                }
                if (gui.hover(mr)) gui.hint = fmt("spawn '%s' at (%d, %d) %ux%u", nm.c_str(), s.x, s.y, unsigned(s.w), unsigned(s.h));
            }
        }
        // info line
        const int mw = int(v.w) * v.tile_w, mh = int(v.h) * v.tile_h;
        gui.text(bx + 6, ly + 2,
                 fmt("%dx%d px  view %d,%d  tileset '%s'  %s", mw, mh, view_x, view_y, names.label(v.tileset).c_str(),
                     v.tile_flags ? "tile flags" : "solid_from"),
                 pal::faint, kLyText, m.x + m.w - bx - 6);
        if (gui.hover(m) && gui.hint.empty())
            gui.hint = "Arrows pan (hold X faster) - drawn by the real renderer with the real parallax path";
        // mini-map scrub bar along the top of the view
        const Rect scrub{ m.x, m.y - 4, m.w, 3 };
        gui.rect(scrub, pal::bar, kLyOver);
        if (mw > 0) gui.rect(Rect{ m.x + view_x * m.w / mw, scrub.y, std::max(3, m.w * m.w / mw), 3 }, pal::accent, kLyOver + 1);
        if (gui.down && gui.hover(Rect{ scrub.x, scrub.y - 2, scrub.w, 7 }) && mw > m.w) {
            view_x = std::max(0, std::min(mw - m.w, (gui.mx - m.x) * mw / m.w - m.w / 2));
            map_fly = false;
        }
    }

    void preview_sound(int idx, const Rect& p, const Rect& body) {
        SoundInfo s;
        if (!view_sound(doc->assets[size_t(idx)], s) || !s.rate) { gui.text(body.x, body.y, "malformed sound", pal::bad); return; }
        if (snd_asset != idx) { snd_asset = idx; if (snd_playing) queue.stop_all(); snd_playing = false; }
        const double secs = double(s.frames) / double(s.rate);
        if (snd_playing && double(ticks - snd_t0) / 60.0 > secs) snd_playing = false;
        const Rect play{ p.x + 6, p.y + 16, 44, 11 };
        if (gui.button(play, snd_playing ? "stop" : "play", snd_playing, audio_state >= 0,
                       audio_state < 0 ? "No audio device could be opened" : "Play through the engine's AudioMixer on the SDL device")) {
            if (snd_playing) { queue.stop_all(); snd_playing = false; }
            else if (ensure_audio()) {
                queue.stop_all();
                queue.play_sfx(SoundView{ s.samples, s.frames, s.rate });
                snd_playing = true;
                snd_t0 = ticks;
            }
        }
        gui.text(play.x + play.w + 8, p.y + 18,
                 fmt("%u frames  mono 16-bit  %.2f s  %s", unsigned(s.frames), secs,
                     s.rate == 18157 ? "(GBA device rate - resampled at bake)" : ""), pal::dim, kLyText, body.w - 60);
        // waveform
        const Rect wv{ body.x, body.y + 10, body.w, std::min(body.h - 30, 180) };
        gui.rect(wv, pal::bar, kLyWidget);
        gui.rect(Rect{ wv.x, wv.y + wv.h / 2, wv.w, 1 }, pal::line, kLyWidget);
        if (wave_asset != idx || wave_cols != wv.w) {
            waveform(s.samples, s.frames, wv.w, wave_mn, wave_mx);
            wave_asset = idx; wave_cols = wv.w;
        }
        const int half = wv.h / 2 - 2;
        const int play_col = snd_playing ? int(double(ticks - snd_t0) / 60.0 / secs * wv.w) : -1;
        for (int c = 0; c < wv.w; ++c) {
            const int top = wv.y + wv.h / 2 - wave_mx[size_t(c)] * half / 32768;
            const int bot = wv.y + wv.h / 2 - wave_mn[size_t(c)] * half / 32768;
            gui.rect(Rect{ wv.x + c, top, 1, std::max(1, bot - top + 1) }, c < play_col ? pal::accent : pal::violet, kLyImage);
        }
        if (play_col >= 0) gui.rect(Rect{ wv.x + play_col, wv.y, 1, wv.h }, pal::text, kLyOver);
        if (gui.hover(wv)) {
            const double t = double(gui.mx - wv.x) / wv.w * secs;
            gui.hint = fmt("t = %.3f s  (sample %u)", t, unsigned(t * s.rate));
        }
        // time ruler
        const int ticks_n = std::max(1, std::min(10, int(secs * 4)));
        int label_end = -1000;
        for (int k = 0; k <= ticks_n; ++k) {
            const int x = wv.x + k * (wv.w - 1) / ticks_n;
            gui.rect(Rect{ x, wv.y + wv.h, 1, 3 }, pal::faint, kLyWidget);
            const std::string t = fmt("%.2fs", secs * k / ticks_n);
            const int lx = std::min(x, wv.x + wv.w - Gui::text_w(t));
            const bool last = k == ticks_n;
            if (lx >= label_end + 8 && (last || lx + Gui::text_w(t) + 8 < wv.x + wv.w - Gui::text_w(t))) {
                gui.text(lx, wv.y + wv.h + 5, t, pal::faint);
                label_end = lx + Gui::text_w(t);
            }
        }
        // peak / RMS
        int64_t sq = 0;
        int peak = 0;
        for (uint32_t i = 0; i < s.frames; ++i) { sq += int64_t(s.samples[i]) * s.samples[i]; peak = std::max(peak, std::abs(int(s.samples[i]))); }
        const double rms = s.frames ? std::sqrt(double(sq) / s.frames) : 0.0;
        gui.text(body.x, wv.y + wv.h + 16, fmt("peak %d%%   rms %d%%   %s", peak * 100 / 32768, int(rms * 100 / 32768),
                                               human_bytes(uint64_t(s.frames) * 2).c_str()), pal::dim);
    }

    void preview_spawns(const AssetEntry& a, const Rect&, const Rect& body) {
        std::vector<SpawnDef> sp;
        if (!view_spawns(a, sp)) { gui.text(body.x, body.y, "malformed spawns", pal::bad); return; }
        // histogram by type
        std::map<std::string, int> by;
        for (const SpawnDef& s : sp) ++by[names.label(s.type)];
        int y = body.y;
        section(body.x, y, body.w, "BY TYPE");
        y += 12;
        int mx = 1;
        for (const auto& kv : by) mx = std::max(mx, kv.second);
        int shown = 0;
        for (const auto& kv : by) {
            if (shown++ >= 10) break;
            gui.text(body.x, y, kv.first, pal::text, kLyText, 80);
            const int bw = std::max(2, (body.w - 120) * kv.second / mx);
            gui.rect(Rect{ body.x + 84, y + 1, bw, 6 }, pal::warn, kLyWidget);
            gui.text(body.x + 88 + bw, y, std::to_string(kv.second), pal::dim);
            y += 10;
        }
        y += 6;
        section(body.x, y, body.w, "POINTS");
        y += 12;
        const int row_h = 9;
        const int visible = (body.y + body.h - y) / row_h;
        const int n = int(sp.size());
        list_scroll = clamp_scroll(list_scroll, n, visible);
        for (int i = 0; i < visible && list_scroll + i < n; ++i) {
            const SpawnDef& s = sp[size_t(list_scroll + i)];
            gui.text(body.x, y + i * row_h, fmt("%4d  %-12s x %5d  y %5d  %ux%u", list_scroll + i, names.label(s.type).c_str(),
                                                s.x, s.y, unsigned(s.w), unsigned(s.h)), pal::text, kLyText, body.w - 10);
        }
        gui.scrollbar(3, Rect{ body.x + body.w - 5, y, 5, visible * row_h }, n, visible, list_scroll);
    }

    void preview_blob(const AssetEntry& a, const Rect& body) {
        const int row_h = 9, per = 16;
        const int n = int((a.data.size() + per - 1) / per);
        const int visible = body.h / row_h;
        list_scroll = clamp_scroll(list_scroll, n, visible);
        for (int i = 0; i < visible && list_scroll + i < n; ++i) {
            const size_t off = size_t(list_scroll + i) * per;
            std::string hex = fmt("%06zx ", off), asc;
            for (int k = 0; k < per; ++k) {
                if (off + size_t(k) < a.data.size()) {
                    const uint8_t b = a.data[off + size_t(k)];
                    hex += fmt("%02x", b);
                    asc += (b >= 32 && b < 127) ? char(b) : '.';
                } else hex += "  ";
                if (k % 4 == 3) hex += ' ';
            }
            gui.text(body.x, body.y + i * row_h, hex, pal::dim, kLyText, body.w);
            gui.text(body.x + Gui::text_w(hex) + 2, body.y + i * row_h, asc, pal::text, kLyText, body.w - Gui::text_w(hex));
        }
        gui.scrollbar(4, Rect{ body.x + body.w - 5, body.y, 5, visible * row_h }, n, visible, list_scroll);
    }

    // ---------------------------------------------------------------------------------------
    // RUN
    // ---------------------------------------------------------------------------------------
    static constexpr int kCatW = 290;

    Rgba state_colour(JobState s) const {
        switch (s) {
        case JobState::Queued:  return pal::info;
        case JobState::Running: return pal::accent;
        case JobState::Pass:    return pal::good;
        case JobState::Fail:    return pal::bad;
        case JobState::Stopped: return pal::faint;
        default:                return pal::line;
        }
    }

    void run_launch(int i) {
        if (!launch_missing[size_t(i)].empty() || !jobs) return;
        diags.clear();                                   // errors now come from this build
        jobs->enqueue(i, launches[size_t(i)].label, launches[size_t(i)].command);
        last_launch = i;
        con_follow = true;
    }

    void draw_launch(int i, const Rect& r) {
        const Launch& l = launches[size_t(i)];
        const JobResult jr = jobs ? jobs->result(i) : JobResult{};
        const bool missing = !launch_missing[size_t(i)].empty();
        const bool h = gui.hover(r);
        const Rgba sc = state_colour(jr.state);
        Rgba fill = missing ? pal::panel : h ? pal::hover : pal::panel2;
        if (jr.state == JobState::Running) fill = (ticks / 20) % 2 ? scale_rgb(pal::accent, 1, 3) : scale_rgb(pal::accent, 1, 4);
        gui.rect(r, fill, kLyWidget);
        gui.rect(Rect{ r.x, r.y, 3, r.h }, missing ? pal::panel2 : sc, kLyOver);
        gui.text(r.x + 6, r.y + (r.h - 8) / 2 + 1, l.label, missing ? pal::faint : pal::text, kLyText, r.w - 7);
        if (h) {
            std::string s = l.blurb;
            if (missing) s = "needs " + launch_missing[size_t(i)] + " - " + l.blurb;
            else if (jr.state == JobState::Pass || jr.state == JobState::Fail)
                s += fmt("   [last: %s, %.1f s]", jr.state == JobState::Pass ? "pass" : fmt("exit %d", jr.exit_code).c_str(), jr.seconds);
            gui.hint = s;
        }
        if (!missing && gui.clicked(r)) run_launch(i);
    }

    void draw_run(const InputState& in) {
        // catalog
        const Rect cat{ 4, kBodyY + 4, kCatW, kBodyH - 8 };
        gui.rect(cat, pal::panel, kLyPanel);
        int y = cat.y + 4;
        for (int g = 0; g < int(Group::Count); ++g) {
            std::vector<int> ids;
            for (size_t i = 0; i < launches.size(); ++i) if (int(launches[i].group) == g) ids.push_back(int(i));
            if (ids.empty()) continue;
            const bool suites = Group(g) == Group::Suite;
            section(cat.x + 5, y, cat.w - 10, group_name(Group(g)));
            if (suites) {
                int pass = 0, fail = 0;
                for (int i : ids) {
                    const JobState s = jobs ? jobs->result(i).state : JobState::Idle;
                    pass += s == JobState::Pass; fail += s == JobState::Fail;
                }
                const Rect all{ cat.x + cat.w - 62, y - 1, 56, 10 };
                gui.rect(Rect{ all.x - 4, y, 64, 9 }, pal::panel, kLyWidget);
                if (gui.button(all, "run all", false, true, "Queue every suite of `make check`, one after another"))
                    for (int i : ids) run_launch(i);
                if (pass + fail)
                    gui.text(all.x - 70, y, fmt("%d/%zu ok", pass, ids.size()), fail ? pal::bad : pal::good, kLyText + 1);
            }
            y += 11;
            const int per = 3;
            const int gap = 3, bh = 12;
            const int bw = (cat.w - 10 - (per - 1) * gap) / per;
            for (size_t k = 0; k < ids.size(); ++k) {
                const Rect r{ cat.x + 5 + int(k % size_t(per)) * (bw + gap), y + int(k / size_t(per)) * (bh + 2), bw, bh };
                draw_launch(ids[k], r);
            }
            y += int((ids.size() + size_t(per) - 1) / size_t(per)) * (bh + 2) + 4;
        }

        // console
        const Rect con_r{ kCatW + 8, kBodyY + 4, kW - kCatW - 12, kBodyH - 8 };
        gui.rect(con_r, pal::bar, kLyPanel);
        gui.frame(con_r, pal::line, kLyWidget);
        const int rid = jobs ? jobs->running_id() : -1;
        std::string head = "CONSOLE";
        if (rid >= 0) head += fmt("  running %s  %.1f s", jobs->running_label().c_str(), jobs->running_seconds());
        gui.text(con_r.x + 6, con_r.y + 4, head, rid >= 0 ? pal::accent : pal::dim, kLyText, con_r.w - 150);
        int bx = con_r.x + con_r.w - 6;
        auto tool = [&](const char* label, bool on, bool enabled, const char* help) {
            const int w = Gui::text_w(label) + 10;
            bx -= w;
            const bool hit = gui.button(Rect{ bx, con_r.y + 2, w, 11 }, label, on, enabled, help);
            bx -= 3;
            return hit;
        };
        if (tool("follow", con_follow, true, "Keep the newest output in view")) con_follow = !con_follow;
        if (tool("clear", false, true, "Clear the console")) { con.clear(); log.clear(); log_consumed = log.total(); con_scroll = 0; }
        if (tool("stop", false, rid >= 0 && jobs && jobs->can_stop(),
                 jobs && jobs->can_stop() ? "Stop the running job and everything it started (and drop the queue)"
                                          : "Stopping needs `setsid` (util-linux)")) {
            jobs->cancel_queue();
            jobs->stop_current();
        }

        const int top = con_r.y + 16, row_h = 9;
        const int visible = (con_r.h - 20) / row_h;
        const int n = int(con.size());
        if (in.just(Button::Up) || repeat(in, Button::Up))     { con_scroll -= 1; con_follow = false; }
        if (in.just(Button::Down) || repeat(in, Button::Down)) { con_scroll += 1; }
        if (con_follow) con_scroll = n - visible;
        con_scroll = clamp_scroll(con_scroll, n, visible);
        if (!con_follow && con_scroll >= n - visible && n > visible && in.down(Button::Down)) con_follow = true;
        if (con.empty()) {
            gui.text(con_r.x + 10, top + 10, "Click anything on the left to build and run it.", pal::dim);
            gui.text(con_r.x + 10, top + 22, "Output streams here, coloured: PASS, FAIL, errors,", pal::faint);
            gui.text(con_r.x + 10, top + 32, "warnings; compiler command lines are dimmed.", pal::faint);
            gui.text(con_r.x + 10, top + 50, "Games and editors open their own windows;", pal::faint);
            gui.text(con_r.x + 10, top + 60, "the job ends when you close them.", pal::faint);
        }
        for (int i = 0; i < visible && con_scroll + i < n; ++i) {
            const ConLine& l = con[size_t(con_scroll + i)];
            const Rect lr{ con_r.x + 2, top + i * row_h, con_r.w - 12, row_h };
            if (l.diag >= 0 && l.diag < int(diags.size())) {
                // compiler diagnostics are links: click to open the file at the line
                const Diag& dg = diags[size_t(l.diag)];
                const Access acc = policy.access(dg.file);
                if (gui.hover(lr)) {
                    if (acc != Access::None) {
                        gui.rect(lr, pal::hover, kLyWidget);
                        gui.hint = twk::fmt("open %s at line %d%s", dg.file.c_str(), dg.line, acc == Access::Read ? " (read-only)" : "");
                        tg.cursor = PHX_CURSOR_HAND;
                    } else {
                        gui.hint = "this message is about engine code, which is not part of the project";
                    }
                }
                if (acc != Access::None && gui.clicked(lr)) {
                    switch_tab(Tab::Editor);
                    ws.open(host, dg.file, false, dg.line, dg.col);
                }
            }
            gui.text(con_r.x + 5, top + i * row_h, l.text, tone_colour(l.tone), kLyText, con_r.w - 16);
        }
        if (gui.scrollbar(5, Rect{ con_r.x + con_r.w - 7, top, 5, visible * row_h }, n, visible, con_scroll))
            con_follow = con_scroll >= n - visible;
    }

    // Compiler output quotes names with U+2018/U+2019 (and U+201C/D); the tool font is ASCII.
    static std::string ascii_quotes(const std::string& t) {
        std::string o;
        o.reserve(t.size());
        for (size_t i = 0; i < t.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(t[i]);
            if (c == 0xE2 && i + 2 < t.size() && static_cast<unsigned char>(t[i + 1]) == 0x80) {
                const unsigned char d = static_cast<unsigned char>(t[i + 2]);
                if (d == 0x98 || d == 0x99) { o += '\''; i += 2; continue; }
                if (d == 0x9C || d == 0x9D) { o += '"'; i += 2; continue; }
                if (d == 0x93 || d == 0x94) { o += '-'; i += 2; continue; }
            }
            o += char(c);
        }
        return o;
    }

    // Wrap newly logged lines to the console width (compiler command lines stay one truncated row).
    void pull_console() {
        const uint64_t total = log.total();
        if (total == log_consumed) return;
        uint64_t fresh = total - log_consumed;
        log_consumed = total;
        if (fresh > log.size()) fresh = log.size();
        const size_t cols = size_t((kW - kCatW - 12 - 16) / kAdv);
        for (size_t i = log.size() - size_t(fresh); i < log.size(); ++i) {
            LogLine l = log.at(i);
            l.text = ascii_quotes(l.text);                  // GCC's curly quotes -> the ASCII font's
            int di = -1;
            Diag dg;
            if (parse_diag(l.text, dg) && diags.size() < 2000) {
                dg.file = resolve_diag_file(dg.file);
                diags.push_back(dg);
                di = int(diags.size()) - 1;
            }
            if (l.tone == Tone::Dim || l.text.size() <= cols) { con.push_back(ConLine{ l.text, l.tone, di }); continue; }
            bool first = true;
            for (const std::string& part : wrap(l.text, cols - 2)) {
                con.push_back(ConLine{ first ? part : "  " + part, l.tone, di });
                first = false;
            }
        }
        if (con.size() > 12000) con.erase(con.begin(), con.begin() + long(con.size() - 10000));
    }
};

uint32_t StudioGame::g_flame_px[12 * 12];

// ---- StudioHost: the editor panels' view of the studio ----
twk::Gui& StudioHost::gui() { return s->tg; }
phx::Renderer& StudioHost::renderer() { return *s->renderer; }
const std::string& StudioHost::root() const { return s->ws_root; }
Access StudioHost::access(const std::string& path_abs) { return s->policy.access(path_abs); }
std::vector<std::pair<std::string, std::string>> StudioHost::api_dirs() { return s->api_dirs(); }
std::string StudioHost::project_name() const { return s->has_project ? s->project.name : std::string(); }
uint64_t StudioHost::ticks() const { return s->ticks; }
void StudioHost::toast(const std::string& msg, Toast t) { s->toast(msg, t); }
void StudioHost::modal(const std::string& title, int w, int h, std::function<bool(twk::Gui&, twk::Rect)> body) {
    s->push_modal(title, w, h, std::move(body));
}
bool StudioHost::modal_open() const { return !s->modals.empty(); }
void StudioHost::open_file(const std::string& path_abs, int line, int col) {
    s->switch_tab(Tab::Editor);
    s->ws.open(*this, path_abs, false, line, col);
}
void StudioHost::open_tile(const std::string& path_abs, int index, int tw, int th) {
    s->switch_tab(Tab::Editor);
    s->ws.open_tile(*this, path_abs, index, tw, th);
}
std::vector<std::string> StudioHost::prefab_types() {
    // Every phxbin table's NAME column (a str 'type'/'name' field): the shared prefab vocabulary.
    if (s->prefab_dirty) {
        s->prefab_dirty = false;
        s->prefab_cache.clear();
        for (const std::string& rel : s->ws.all_files()) {
            if (lower_ext(rel) != ".json" || rel.compare(0, 6, "build/") == 0) continue;
            const std::string abs = join_path(s->root, rel);
            const std::string head = read_head(abs, 4096);
            if (kind_for(rel, head) != FileKind::Table) continue;
            std::string text;
            if (!read_text(abs, text)) continue;
            phxtool::BinDoc d;
            if (!phxtool::BinDoc::load(text, d)) continue;
            // only prefab tables (a `type` column, what the level loader matches spawns by): a
            // flow table's `name`s or a dialogue table's lines are not placeable
            const size_t nf = d.name_field();
            if (nf >= d.fields.size() || d.fields[nf].name != "type") continue;
            for (const std::string& n : d.name_column()) s->prefab_cache.push_back(n);
        }
    }
    return s->prefab_cache;
}
std::vector<std::string> StudioHost::files_with(const std::vector<std::string>& exts) {
    std::vector<std::string> out;
    for (const std::string& rel : s->ws.all_files())
        for (const std::string& e : exts) if (lower_ext(rel) == e) { out.push_back(rel); break; }
    return out;
}
bool StudioHost::play_from(int x, int y) {
    if (!s->jobs || !s->has_project) return false;
    for (size_t i = 0; i < s->launches.size(); ++i) {
        const Launch& l = s->launches[i];
        if (l.group != Group::Play || l.label != "Play") continue;
        if (!s->launch_missing[i].empty()) { s->toast("Play needs " + s->launch_missing[i], Toast::Warn); return true; }
        s->diags.clear();
        s->jobs->enqueue(int(i), "Play from " + std::to_string(x) + "," + std::to_string(y),
                         "export PHX_PLAY_FROM='" + std::to_string(x) + "," + std::to_string(y) + "'; " + l.command);
        s->last_launch = int(i);
        s->con_follow = true;
        s->toast(fmt("playing from %d,%d - see the Run view", x, y), Toast::Good);
        return true;
    }
    return false;
}
bool StudioHost::run_make(const std::string& target) {
    if (!s->jobs) return false;
    s->diags.clear();
    s->jobs->enqueue(100000, "make " + target, "make " + target);
    s->toast("make " + target + " - see the Run view");
    return true;
}
void StudioHost::file_saved(const std::string& path_abs) {
    if (lower_ext(path_abs) == ".json") s->prefab_dirty = true;
    s->ws.refresh_tree();
}
std::vector<Diag> StudioHost::diagnostics_for(const std::string& path_abs) {
    std::vector<Diag> out;
    const std::string want = canon_path(path_abs);
    for (const Diag& d : s->diags) if (d.file == want) out.push_back(d);
    return out;
}

void usage() {
    std::printf("usage: phxstudio [--project DIR | --engine-dev] [--root DIR] [--scale N] [--tab editor|assets|run|overview]\n"
                "                 [--open FILE]... [--bundle FILE.phxp] [--asset NAME] [--run LAUNCH]... [--fresh]\n"
                "                 [--shot OUT.ppm [--shot-frame N]] [--script \"CMD; CMD...\" | FILE]\n"
                "  --project DIR  open a game project (a folder with a phxproject.json). The Studio then\n"
                "                 edits ONLY that folder; the engine's public API headers are read-only and\n"
                "                 nothing else of the engine can be opened. Without it: a project picker.\n"
                "  --engine-dev   work on the Phoenix engine itself: the whole checkout, the module graph,\n"
                "                 every gate and suite (the Studio's engine-maintenance mode)\n"
                "  --root    the Phoenix repository (default: found from the current directory)\n"
                "  --scale   integer UI scale (default 2: the 640x360 canvas opens as 1280x720)\n"
                "  --tab     the view to open first (default: editor)\n"
                "  --open    open a file in the Editor (repeatable; relative to the project)\n"
                "  --fresh   don't reopen the last session's documents\n"
                "  --bundle  preselect a bundle in the Assets view (path relative to the root)\n"
                "  --asset   preselect an asset of that bundle: NAME or TYPE:NAME (e.g. sprite:hero)\n"
                "  --run     queue a launch at startup by make target or label (repeatable)\n"
                "  --shot    render N frames (default 45), write the window to a PPM, and quit\n"
                "  --script  drive the studio from commands (inline or a file):\n"
                "            click|rclick|dclick|move X Y; drag X0 Y0 X1 Y1 [N]; wheel X Y N;\n"
                "            key [ctrl+][shift+][alt+]NAME; type TEXT; open PATH; tab NAME;\n"
                "            project DIR; wait N; shot FILE.ppm; quit   (canvas coordinates)\n");
}

} // namespace

int main(int argc, char** argv) {
    StudioGame game;
    std::string root_arg;
    int scale = 2;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--root" && i + 1 < argc)        root_arg = argv[++i];
        else if (a == "--scale" && i + 1 < argc)  { scale = std::atoi(argv[++i]); game.auto_size = false; }
        else if (a == "--bundle" && i + 1 < argc) game.initial_bundle = argv[++i];
        else if (a == "--asset" && i + 1 < argc)  game.initial_asset = argv[++i];
        else if (a == "--shot" && i + 1 < argc)   game.shot_path = argv[++i];
        else if (a == "--run" && i + 1 < argc)    game.initial_runs.push_back(argv[++i]);
        else if (a == "--open" && i + 1 < argc)   game.initial_opens.push_back(argv[++i]);
        else if (a == "--project" && i + 1 < argc) game.project_arg = argv[++i];
        else if (a == "--engine-dev")             game.engine_dev = true;
        else if (a == "--fresh")                  game.fresh = true;
        else if (a == "--script" && i + 1 < argc) {
            std::string text = argv[++i];
            std::string file;
            if (read_text(text, file)) text = file;       // a path, or the commands inline
            game.script = StudioGame::parse_script(text);
        }
        else if (a == "--shot-frame" && i + 1 < argc) game.shot_frame = std::atoi(argv[++i]);
        else if (a == "--tab" && i + 1 < argc) {
            const std::string t = argv[++i];
            game.tab = t == "assets" ? Tab::Assets : t == "run" ? Tab::Run : t == "overview" ? Tab::Overview : Tab::Editor;
        } else if (a == "--help" || a == "-h") { usage(); return 0; }
        else { std::fprintf(stderr, "phxstudio: unknown argument '%s'\n", a.c_str()); usage(); return 1; }
    }
    game.root = find_repo_root(root_arg.empty() ? "." : root_arg);
    if (game.root.empty()) {
        std::fprintf(stderr, "phxstudio: not inside the Phoenix repository (no Makefile + engine/ + "
                             "tools/common/depcheck.py above '%s'); pass --root DIR\n",
                     root_arg.empty() ? "." : root_arg.c_str());
        return 1;
    }

    phx_desktop_set_scale(scale);
    if (!game.script.empty() || !game.shot_path.empty()) { game.fresh = true; game.auto_size = false; }   // reproducible runs
    Config cfg = Config::from_defaults();
    cfg.title = "Phoenix Studio";
    cfg.width = kW; cfg.height = kH;
    cfg.sim_hz = 60; cfg.vsync = true;
    App app(cfg);
    return app.run(&game);
}
