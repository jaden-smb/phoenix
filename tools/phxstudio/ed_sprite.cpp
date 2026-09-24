// tools/phxstudio/ed_sprite.cpp — the SPRITE / PIXEL editor panel. Opens a .png (paint it) or a
// sprite definition (.sprdef / sprite .json: paint its sheet AND edit its frame grid + named
// clips with a live animated preview). Saves real PNGs (png_write.h) and the def the bake reads;
// the documents (PixelDoc / SprDoc in pixeldoc.h) are headless and unit-tested.
//
//   B pencil  E eraser  G fill (Shift: every pixel of that colour)  I picker  L line
//   R rectangle  O ellipse (Shift: filled)  M marquee select  H hand       X swap colours
//   [ / ]  brush size    right button = paint with the secondary colour    Alt+click = pick
//   selection: Ctrl+C / X / V, Delete, drag inside to move, arrows nudge, Enter/Esc drop
//   Ctrl+wheel zoom · wheel scroll · middle-drag or Space+drag pan · F fit
//   , / .  previous / next frame or tile      P play / pause the preview
//
// A plain PNG (a tileset, a texture) is edited in TILE mode: a tile grid (the size is detected from
// a map or sprite def that uses the image, or set in the Image tab), numbered tiles (GID = index
// + 1, as a map references them) and a tile strip — click a tile to zoom in on it. The map
// editor's "Edit tile" opens the tileset here focused on that tile (focus_tile()).
#include "host.h"
#include "pixeldoc.h"

#include "phx/render/renderer.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace phxstudio {
namespace {

using namespace twk;

enum class Tool : uint8_t { Pencil, Eraser, Fill, Picker, Line, Rect, Ellipse, Select, Hand, Count };

struct ToolInfo { Tool t; int icon; const char* name; int key; const char* help; };
const ToolInfo kTools[] = {
    { Tool::Pencil,  kIconPencil,  "pencil",  'b', "Pencil (B) - left: primary colour, right: secondary" },
    { Tool::Eraser,  kIconEraser,  "eraser",  'e', "Eraser (E) - paints transparent" },
    { Tool::Fill,    kIconBucket,  "fill",    'g', "Fill (G) - Shift+click replaces that colour everywhere" },
    { Tool::Picker,  kIconPicker,  "picker",  'i', "Colour picker (I, or Alt+click with any tool)" },
    { Tool::Line,    kIconLine,    "line",    'l', "Line (L)" },
    { Tool::Rect,    kIconRect,    "rect",    'r', "Rectangle (R) - hold Shift for filled" },
    { Tool::Ellipse, kIconEllipse, "ellipse", 'o', "Ellipse (O) - hold Shift for filled" },
    { Tool::Select,  kIconSelect,  "select",  'm', "Marquee select (M) - Ctrl+C/X/V, Delete, drag to move" },
    { Tool::Hand,    kIconHand,    "hand",    'h', "Pan (H, or Space / middle-drag)" },
};

class SpriteView final : public DocView {
public:
    PixelDoc img;
    SprDoc spr;
    bool has_def = false;
    std::string img_path;        // the PNG on disk

    bool load(Host& h, const std::string& p, std::string* err) {
        path = p;
        const std::string e = lower_ext(p);
        if (e == ".png") {
            img_path = p;
            if (!PixelDoc::load_png(p, img, err)) return false;
            has_def = false;
            detect_tile_size();
            frame_grid_ = img.w % tile_w_ == 0 && img.h % tile_h_ == 0 && (img.w > tile_w_ || img.h > tile_h_);
        } else {
            if (!SprDoc::load(p, spr, err)) return false;
            has_def = true;
            // a bare name sits next to the def; a path with '/' is relative to where the bake runs
            // (the repo root in the studio)
            img_path = SprDoc::resolve_sheet(p, spr.sheet);
            if (spr.sheet[0] != '/' && spr.sheet.find('/') != std::string::npos) img_path = join_path(h.root(), spr.sheet);
            if (h.access(img_path) == Access::None) {       // the project boundary covers the sheet too
                if (err) *err = "the sheet '" + spr.sheet + "' is outside the project";
                return false;
            }
            std::string e2;
            if (!PixelDoc::load_png(img_path, img, &e2)) {
                // a def whose sheet is missing: start a blank sheet of 4 frames (saved on Ctrl+S)
                img = PixelDoc::blank(spr.frame_w * 4, spr.frame_h, 0);
                img.dirty = true;
                h.toast("sheet '" + spr.sheet + "' not found - started a blank one", Toast::Warn);
            }
        }
        disk_stamp = file_stamp(p);
        fg_ = px_rgba(255, 255, 255);
        bg_ = 0;
        if (!img.unique_colors(1).empty()) fg_ = img.unique_colors(1)[0];
        return true;
    }

    FileKind kind() const override { return has_def ? FileKind::Sprite : FileKind::Image; }
    int icon() const override { return has_def ? kIconSprite : kIconFileImage; }
    bool dirty() const override { return img.dirty || spr.dirty; }
    bool save(Host& h, std::string* err) override {
        commit_float();
        if (h.access(img_path) != Access::Write || h.access(path) != Access::Write) {
            if (err) *err = base_name(img_path) + " is not part of the project (read-only)";
            return false;
        }
        if (img.dirty || !pfs::exists(img_path)) {
            if (!img.save_png(img_path, err)) return false;
            h.file_saved(img_path);
        }
        if (has_def && spr.dirty) {
            if (!spr.save(path, err)) return false;
            h.file_saved(path);
        }
        disk_stamp = file_stamp(path);
        return true;
    }
    bool reload(Host& h, std::string* err) override {
        SpriteView fresh;
        if (!fresh.load(h, path, err)) return false;
        img = fresh.img; spr = fresh.spr; has_def = fresh.has_def; img_path = fresh.img_path;
        disk_stamp = fresh.disk_stamp;
        return true;
    }
    void release(Host& h) override {
        h.renderer().unload_texture(tex_);
        tex_ = kNoTexture;
        tex_ptr_ = nullptr;
    }
    bool undo() override {
        commit_float();
        if (hist_.empty()) return false;
        const char k = hist_.back();
        hist_.pop_back();
        bool ok = false;
        if (k == 'i') ok = img.undo();
        else if (!spr_undo_.empty()) { spr_redo_.push_back(spr); spr = spr_undo_.back(); spr_undo_.pop_back(); spr.dirty = true; ok = true; }
        if (ok) redo_hist_.push_back(k);
        return ok;
    }
    bool redo() override {
        if (redo_hist_.empty()) return false;
        const char k = redo_hist_.back();
        redo_hist_.pop_back();
        bool ok = false;
        if (k == 'i') ok = img.redo();
        else if (!spr_redo_.empty()) { spr_undo_.push_back(spr); spr = spr_redo_.back(); spr_redo_.pop_back(); spr.dirty = true; ok = true; }
        if (ok) hist_.push_back(k);
        return ok;
    }
    std::string status() const override {
        std::string s = fmt("%dx%d", img.w, img.h);
        if (has_def) s += fmt("  %d frames of %dx%d", spr.frame_count(img.w, img.h), spr.frame_w, spr.frame_h);
        else if (frame_grid_) s += fmt("  %d tiles of %dx%d (tile %d = GID %d)", frame_count(), tile_w_, tile_h_, frame_, frame_ + 1);
        s += fmt("  zoom %dx  %zu colours", cv_.zoom, colour_count_);
        if (hover_x_ >= 0) s += fmt("  (%d,%d)", hover_x_, hover_y_);
        return s;
    }
    // Select cell `index` (a tile; a frame for sprite defs) and zoom the canvas onto it. For a plain
    // image the caller's tile size becomes the grid (the map editor passes its tile size).
    void focus_tile(int index, int tw, int th) override {
        if (!has_def && tw > 0 && th > 0) { tile_w_ = tw; tile_h_ = th; }
        frame_grid_ = true;
        frame_ = std::max(0, std::min(index, frame_count() - 1));
        center_frame_ = true;
    }

    std::vector<Action> actions() override {
        std::vector<Action> a;
        a.push_back({ "Resize canvas...", "", [this] { want_resize_ = true; } });
        if (!has_def) a.push_back({ "Add a row of tiles", "", [this] { edit_img([&] { img.resize(img.w, img.h + tile_h_, 0, 0); }); } });
        a.push_back({ "Scale image x2", "", [this] { edit_img([&] { img.scale_up(2); }); if (has_def) { spr_edit(); spr.frame_w *= 2; spr.frame_h *= 2; } } });
        a.push_back({ "Flip horizontal", "", [this] { flip(true); } });
        a.push_back({ "Flip vertical", "", [this] { flip(false); } });
        a.push_back({ "Rotate 90 (square selection/frame)", "", [this] { rotate(); } });
        a.push_back({ "Replace primary with secondary colour", "", [this] { const uint32_t f = fg_, b = bg_; edit_img([&] { img.replace_color(f, b); }); } });
        a.push_back({ "Snap every colour to GBA (BGR555)", "", [this] { edit_img([&] { img.snap_all_555(); }); } });
        if (has_def) a.push_back({ "Append a frame", "", [this] { append_frame(); } });
        else a.push_back({ "Make a sprite definition for this PNG...", "", [this] { want_mkdef_ = true; } });
        return a;
    }

    void draw(Host& h, const Rect& area) override {
        Gui& g = h.gui();
        if (!id_) id_ = Gui::hash(path.c_str());
        g.push_id(id_);
        upload(h);
        if (g.frame % 20 == 1 || colour_dirty_) { colour_count_ = img.unique_colors(4096).size(); colour_dirty_ = false; }
        if (want_resize_) { want_resize_ = false; resize_dialog(h); }
        if (want_mkdef_) { want_mkdef_ = false; mkdef_dialog(h); }

        Rect r = area;
        g.rect(r, g.th.bg, kSubBg);
        const Rect tools = cut_left(r, 22);
        const Rect side = cut_right(r, std::min(190, std::max(150, area.w / 4)));
        const Rect top = cut_top(r, 16);
        Rect strip{};
        if (show_strip()) strip = cut_bottom(r, std::max(44, std::min(84, cell_h() * 2 + 24)));
        const Rect canvas = r;

        draw_tools(g, tools);
        draw_topbar(h, top);
        keys(h);
        draw_canvas(h, canvas);
        if (show_strip()) draw_strip(h, strip);
        draw_side(h, side);
        g.pop_id();
    }

private:
    uint32_t id_ = 0;
    TextureId tex_ = kNoTexture;
    const void* tex_ptr_ = nullptr;
    int tex_w_ = 0, tex_h_ = 0;
    Canvas cv_;
    Tool tool_ = Tool::Pencil;
    uint32_t fg_ = 0xFFFFFFFFu, bg_ = 0;
    int brush_ = 1;
    bool grid_ = true, frame_grid_ = true, onion_ = false, mirror_ = false, gba_ = false, tiles_ = false;
    bool playing_ = true;
    int frame_ = 0, clip_ = 0;
    int tile_w_ = 8, tile_h_ = 8;          // the tile grid of a plain image
    int pal_sel_ = 0, pal_scroll_ = 0;
    size_t colour_count_ = 0;
    bool colour_dirty_ = true;
    int hover_x_ = -1, hover_y_ = -1;
    // stroke state
    bool stroking_ = false;
    int sx0_ = 0, sy0_ = 0, lastx_ = 0, lasty_ = 0;
    uint32_t stroke_c_ = 0;
    std::vector<uint32_t> before_;
    // selection + floating paste
    bool has_sel_ = false;
    int selx_ = 0, sely_ = 0, selw_ = 0, selh_ = 0;
    bool floating_ = false;
    PixelDoc::Region float_;
    int fx_ = 0, fy_ = 0, fdx_ = 0, fdy_ = 0;
    PixelDoc::Region clip_board_;
    // history routing ('i' image, 's' sprite def)
    std::vector<char> hist_, redo_hist_;
    std::vector<SprDoc> spr_undo_, spr_redo_;
    bool want_resize_ = false, want_mkdef_ = false;
    std::string hex_;
    int clip_scroll_ = 0;

    // ---- document plumbing ----
    void push_img_undo() { img.push_undo(); hist_.push_back('i'); redo_hist_.clear(); }
    template <class F> void edit_img(F f) {
        commit_float();
        push_img_undo();
        f();
        if (img.same_as_last_snapshot()) { img.drop_undo(); hist_.pop_back(); }
        colour_dirty_ = true;
    }
    void spr_edit() {
        spr_undo_.push_back(spr);
        if (spr_undo_.size() > 200) spr_undo_.erase(spr_undo_.begin());
        spr_redo_.clear();
        hist_.push_back('s');
        redo_hist_.clear();
        spr.dirty = true;
    }

    void upload(Host& h) {
        if (tex_ != kNoTexture && tex_ptr_ == img.px.data() && tex_w_ == img.w && tex_h_ == img.h) return;
        phx::Renderer& r = h.renderer();
        r.unload_texture(tex_);
        phx::TextureDesc d{};
        d.pixels = img.px.data();
        d.size = uint32_t(img.px.size() * 4);
        d.width = uint16_t(img.w); d.height = uint16_t(img.h);
        tex_ = r.load_texture(d);          // zero-copy: painting shows up on the next frame
        tex_ptr_ = img.px.data(); tex_w_ = img.w; tex_h_ = img.h;
    }

    // Frame rect of frame i (sprite mode).
    // Cells: a sprite def's frames, or a plain image's tiles.
    int cell_w() const { return std::max(1, has_def ? spr.frame_w : tile_w_); }
    int cell_h() const { return std::max(1, has_def ? spr.frame_h : tile_h_); }
    int cell_cols() const { return std::max(1, img.w / cell_w()); }
    void frame_rect(int i, int& x, int& y) const { x = (i % cell_cols()) * cell_w(); y = (i / cell_cols()) * cell_h(); }
    int frame_count() const { return std::max(1, cell_cols() * std::max(1, img.h / cell_h())); }
    bool show_strip() const { return has_def || (frame_grid_ && frame_count() > 1); }

    // A plain PNG: take the tile size from a map in the same folder that uses it as its tileset,
    // or a sprite def that uses it as its sheet; 8x8 (the GBA tile) otherwise.
    void detect_tile_size() {
        tile_w_ = tile_h_ = 8;
        const std::string dir = dir_name(img_path), me = base_name(img_path);
        std::error_code ec;
        for (pfs::directory_iterator it(dir.empty() ? "." : dir, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string f = it->path().string(), ext = lower_ext(f);
            if (ext == ".tmj") {
                phxtool::TmapDoc m;
                std::string text = read_head(f, 1u << 22);
                if (phxtool::TmapDoc::load(text, m) && base_name(m.tileset_image) == me) { tile_w_ = m.tile_w; tile_h_ = m.tile_h; return; }
            } else if (ext == ".sprdef" || ext == ".json") {
                SprDoc sd;
                if (SprDoc::load(f, sd) && base_name(sd.sheet) == me) { tile_w_ = sd.frame_w; tile_h_ = sd.frame_h; return; }
            }
        }
    }

    uint32_t paint_colour(bool secondary) const {
        const uint32_t c = secondary ? bg_ : fg_;
        return gba_ ? snap555(c) : c;
    }

    void dab_mirrored(int x, int y, uint32_t c) {
        img.dab(x, y, c, brush_);
        if (mirror_) img.dab(mirror_x(x) - (brush_ - 1) + 2 * ((brush_ - 1) / 2), y, c, brush_);
    }
    void line_mirrored(int x0, int y0, int x1, int y1, uint32_t c) {
        img.line(x0, y0, x1, y1, c, brush_);
        if (mirror_) img.line(mirror_x(x0), y0, mirror_x(x1), y1, c, brush_);
    }
    // Mirror about the current frame's vertical centre (sprite) or the image's.
    int mirror_x(int x) const {
        if (has_def || frame_grid_) {
            int fx, fy;
            frame_rect(frame_, fx, fy);
            (void)fy;
            return fx + cell_w() - 1 - (x - fx);
        }
        return img.w - 1 - x;
    }

    // ---- selection / floating ----
    void lift_selection(bool cut) {
        if (!has_sel_ || floating_) return;
        push_img_undo();
        float_ = img.copy(selx_, sely_, selw_, selh_);
        if (cut) img.clear_rect(selx_, sely_, selw_, selh_, 0);
        floating_ = true;
        fx_ = float_.x; fy_ = float_.y;
    }
    void commit_float() {
        if (!floating_) return;
        img.paste(float_, fx_, fy_, true);
        floating_ = false;
        selx_ = fx_; sely_ = fy_; selw_ = float_.w; selh_ = float_.h;
        if (img.same_as_last_snapshot()) { img.drop_undo(); if (!hist_.empty()) hist_.pop_back(); }
        colour_dirty_ = true;
    }
    void paste_clipboard() {
        if (clip_board_.w <= 0) return;
        commit_float();
        push_img_undo();
        float_ = clip_board_;
        int x = has_sel_ ? selx_ : std::max(0, cv_.to_cx(cv_.ox + 8));
        int y = has_sel_ ? sely_ : std::max(0, cv_.to_cy(cv_.oy + 8));
        fx_ = std::min(x, std::max(0, img.w - float_.w));
        fy_ = std::min(y, std::max(0, img.h - float_.h));
        floating_ = true;
        has_sel_ = true;
        selx_ = fx_; sely_ = fy_; selw_ = float_.w; selh_ = float_.h;
        tool_ = Tool::Select;
    }
    // The region ops act on: the selection, else the current frame (sprite), else the image.
    void op_region(int& x, int& y, int& w, int& hh) const {
        if (has_sel_) { x = selx_; y = sely_; w = selw_; hh = selh_; return; }
        if (has_def || frame_grid_) { frame_rect(frame_, x, y); w = cell_w(); hh = cell_h(); return; }
        x = 0; y = 0; w = img.w; hh = img.h;
    }
    void flip(bool horiz) {
        if (floating_) {
            PixelDoc t = PixelDoc::blank(float_.w, float_.h);
            t.px = float_.px;
            if (horiz) t.flip_h(0, 0, t.w, t.h); else t.flip_v(0, 0, t.w, t.h);
            float_.px = t.px;
            return;
        }
        int x, y, w, hh;
        op_region(x, y, w, hh);
        edit_img([&] { if (horiz) img.flip_h(x, y, w, hh); else img.flip_v(x, y, w, hh); });
    }
    void rotate() {
        int x, y, w, hh;
        op_region(x, y, w, hh);
        if (w != hh) return;
        edit_img([&] { img.rotate90(x, y, w, true); });
    }
    void append_frame() {
        edit_img([&] { img.resize(img.w + spr.frame_w, img.h, 0, 0); });
    }

    // ---- UI pieces ----
    void draw_tools(Gui& g, const Rect& r) {
        g.rect(r, g.th.panel, kSubFill);
        int y = r.y + 3;
        for (const ToolInfo& t : kTools) {
            if (g.icon_button(Rect{ r.x + 3, y, 16, 15 }, t.icon, t.help, tool_ == t.t)) { commit_float(); tool_ = t.t; }
            y += 17;
        }
        y += 4;
        g.hline(r.x + 3, y, 16, g.th.line);
        y += 5;
        // colour swatches: primary over secondary, click to swap
        const Rect a{ r.x + 3, y, 12, 12 }, b{ r.x + 8, y + 6, 12, 12 };
        checkerboard(g, b, 3, kSubWidget);
        if (px_a(bg_)) g.rect(b, bg_, kSubImage);
        g.frame_rect(b, g.th.line, kSubOver);
        checkerboard(g, a, 3, kSubOver);
        if (px_a(fg_)) g.rect(a, fg_, kSubText);
        g.frame_rect(a, g.th.text, kSubTop);
        if (g.clicked(Rect{ r.x, y, r.w, 20 })) std::swap(fg_, bg_);
        g.tip(Rect{ r.x, y, r.w, 20 }, "Primary / secondary colour (X swaps)");
        y += 24;
        g.text(r.x + 4, y, std::to_string(brush_), g.th.text);
        g.tip(Rect{ r.x, y - 2, r.w, 12 }, "Brush size ( [ and ] )");
        if (const int w = g.wheel(Rect{ r.x, y - 2, r.w, 12 })) brush_ = std::max(1, std::min(16, brush_ + w));
    }

    void draw_topbar(Host& h, const Rect& r) {
        Gui& g = h.gui();
        g.rect(r, g.th.panel, kSubFill);
        Gui::Row row(Rect{ r.x + 4, r.y + 2, r.w - 8, 12 }, 2);
        auto toggle = [&](int icon, bool& v, const char* help) {
            if (g.icon_button(row.take(15), icon, help, v)) v = !v;
        };
        toggle(kIconGrid, grid_, "Pixel grid (shows at zoom >= 6)");
        toggle(kIconAnim, frame_grid_, has_def ? "Frame grid" : "Tile grid + the tile strip (the tile size is in the Image tab)");
        if (has_def) toggle(kIconOnion, onion_, "Onion skin: the previous frame shows through the current one");
        toggle(kIconMirror, mirror_, "Mirror painting around the frame's (or image's) centre line");
        toggle(kIconGba, gba_, "GBA colours: snap every painted colour to BGR555, what the GBA can show");
        toggle(kIconTileCheck, tiles_, "GBA tile check: 8x8 tiles with > 15 colours (the 4bpp limit) outlined red");
        // right: zoom
        if (g.icon_button(row.take_right(14), kIconZoomIn, "Zoom in (Ctrl+wheel)")) cv_.zoom_at(cv_.to_sx(img.w / 2), cv_.to_sy(img.h / 2), Canvas::step_zoom(cv_.zoom, 1));
        const std::string z = fmt("%dx", cv_.zoom);
        g.text(row.take_right(Gui::text_w(z)).x, r.y + 4, z, g.th.dim);
        if (g.icon_button(row.take_right(14), kIconZoomOut, "Zoom out (Ctrl+wheel)")) cv_.zoom_at(cv_.to_sx(img.w / 2), cv_.to_sy(img.h / 2), Canvas::step_zoom(cv_.zoom, -1));
        if (g.button(row.take_right(24), "fit", Btn{ false, true, false, 0, "Fit the image in view (F)" })) cv_.fitted = false;
        // middle: the sheet's name (and size)
        const Rect nm = row.rest();
        const std::string label = base_name(img_path) + fmt("  %dx%d", img.w, img.h);
        g.text(nm.x + 4, r.y + 4, label, img.dirty ? g.th.accent : g.th.dim, kSubText, nm.w - 6);
    }

    void keys(Host& h) {
        Gui& g = h.gui();
        if (g.text_focus() || h.modal_open()) return;
        for (const ToolInfo& t : kTools) if (g.hotkey(t.key)) { commit_float(); tool_ = t.t; }
        if (g.hotkey('x')) std::swap(fg_, bg_);
        if (g.hotkey('[')) brush_ = std::max(1, brush_ - 1);
        if (g.hotkey(']')) brush_ = std::min(16, brush_ + 1);
        if (g.hotkey('f')) cv_.fitted = false;
        if (show_strip()) {
            if (g.hotkey(',')) { frame_ = (frame_ + frame_count() - 1) % frame_count(); if (!has_def) center_frame_ = true; }
            if (g.hotkey('.')) { frame_ = (frame_ + 1) % frame_count(); if (!has_def) center_frame_ = true; }
        }
        if (has_def && g.hotkey('p')) playing_ = !playing_;
        if (g.key('c', kCtrl) && (has_sel_ || floating_)) {
            clip_board_ = floating_ ? float_ : img.copy(selx_, sely_, selw_, selh_);
            h.toast(fmt("copied %dx%d", clip_board_.w, clip_board_.h));
        }
        if (g.key('x', kCtrl) && has_sel_) {
            if (floating_) { clip_board_ = float_; floating_ = false; }
            else { clip_board_ = img.copy(selx_, sely_, selw_, selh_); edit_img([&] { img.clear_rect(selx_, sely_, selw_, selh_, 0); }); }
        }
        if (g.key('v', kCtrl)) paste_clipboard();
        if (g.key('a', kCtrl)) { commit_float(); has_sel_ = true; selx_ = sely_ = 0; selw_ = img.w; selh_ = img.h; tool_ = Tool::Select; }
        if (g.key('d', kCtrl)) { commit_float(); has_sel_ = false; }
        if (has_sel_ && (g.hotkey(PHX_KEY_DELETE) || g.hotkey(PHX_KEY_BACKSPACE))) {
            if (floating_) floating_ = false;                // discard the floating pixels
            else edit_img([&] { img.clear_rect(selx_, sely_, selw_, selh_, 0); });
        }
        if (floating_ && (g.hotkey(PHX_KEY_ENTER))) commit_float();
        if (g.hotkey(PHX_KEY_ESCAPE)) { commit_float(); has_sel_ = false; }
        if (has_sel_) {
            int dx = 0, dy = 0;
            if (g.hotkey(PHX_KEY_LEFT)) dx = -1;
            if (g.hotkey(PHX_KEY_RIGHT)) dx = 1;
            if (g.hotkey(PHX_KEY_UP)) dy = -1;
            if (g.hotkey(PHX_KEY_DOWN)) dy = 1;
            if (dx || dy) { lift_selection(true); fx_ += dx; fy_ += dy; selx_ = fx_; sely_ = fy_; }
        }
    }

    void draw_canvas(Host& h, const Rect& area) {
        Gui& g = h.gui();
        const uint32_t cid = g.id("canvas");
        if (!cv_.fitted) {
            if (has_def) {                                       // fit the whole sheet
                cv_.fit(area, img.w, img.h, 24);
            } else cv_.fit(area, img.w, img.h, 24);
        }
        const bool space = false;   // (Space is a text key; the hand tool / middle drag pan instead)
        if (center_frame_) {                                     // zoom onto the selected tile/frame
            int fx, fy;
            frame_rect(frame_, fx, fy);
            const int z = std::max(1, std::min(32, std::min((area.w - 48) / cell_w(), (area.h - 48) / cell_h())));
            cv_.zoom = z;
            cv_.ox = area.x + area.w / 2 - (fx * z + cell_w() * z / 2);
            cv_.oy = area.y + area.h / 2 - (fy * z + cell_h() * z / 2);
            cv_.fitted = true;
            center_frame_ = false;
        }
        g.push_clip(area);
        cv_.navigate(g, area, g.id("pan"), space || tool_ == Tool::Hand);
        const Rect img_r{ cv_.ox, cv_.oy, img.w * cv_.zoom, img.h * cv_.zoom };
        checkerboard(g, intersect(img_r, area), std::max(4, std::min(16, cv_.zoom * 2)), kSubFill);

        // onion skin: the previous frame, tinted, beneath the current frame
        if (has_def && onion_ && frame_count() > 1) {
            int cx, cy, px, py;
            frame_rect(frame_, cx, cy);
            frame_rect((frame_ + frame_count() - 1) % frame_count(), px, py);
            g.image(Rect{ cv_.to_sx(cx), cv_.to_sy(cy), spr.frame_w * cv_.zoom, spr.frame_h * cv_.zoom }, tex_,
                    px, py, spr.frame_w, spr.frame_h, rgba(120, 60, 80), uint8_t(kSubImage - 1));
        }
        g.image(img_r, tex_, 0, 0, img.w, img.h, rgba(255, 255, 255), kSubImage);

        // floating selection pixels (drawn on top; not yet in the image)
        if (floating_) {
            // draw pixel runs as rects (cheap for small selections)
            for (int y = 0; y < float_.h; ++y) {
                int x = 0;
                while (x < float_.w) {
                    const uint32_t c = float_.px[size_t(y) * size_t(float_.w) + size_t(x)];
                    int run = 1;
                    while (x + run < float_.w && float_.px[size_t(y) * size_t(float_.w) + size_t(x + run)] == c) ++run;
                    if (px_a(c)) g.rect(Rect{ cv_.to_sx(fx_ + x), cv_.to_sy(fy_ + y), run * cv_.zoom, cv_.zoom }, c, kSubOver);
                    x += run;
                }
            }
        }

        // grids
        if (grid_ && cv_.zoom >= 6) {
            const Rect vis = intersect(img_r, area);
            const Rgba gc = rgba(70, 70, 92);
            for (int x = cv_.to_cx(vis.x); x <= cv_.to_cx(vis.right()); ++x) g.vline(cv_.to_sx(x), vis.y, vis.h, gc, kSubOver);
            for (int y = cv_.to_cy(vis.y); y <= cv_.to_cy(vis.bottom()); ++y) g.hline(vis.x, cv_.to_sy(y), vis.w, gc, kSubOver);
        }
        if (frame_grid_) {
            const Rgba fc = rgba(120, 96, 150);
            for (int x = 0; x <= img.w; x += cell_w()) g.vline(cv_.to_sx(x), img_r.y, img_r.h, fc, kSubOver);
            for (int y = 0; y <= img.h; y += cell_h()) g.hline(img_r.x, cv_.to_sy(y), img_r.w, fc, kSubOver);
            int fx, fy;
            frame_rect(frame_, fx, fy);
            g.frame_rect(Rect{ cv_.to_sx(fx) - 1, cv_.to_sy(fy) - 1, cell_w() * cv_.zoom + 2, cell_h() * cv_.zoom + 2 }, g.th.accent, kSubText);
            // tile numbers (GIDs, as a map references them) once the tiles are big enough to label
            if (!has_def && cell_w() * cv_.zoom >= 24 && cell_h() * cv_.zoom >= 14)
                for (int i = 0; i < frame_count(); ++i) {
                    int tx, ty;
                    frame_rect(i, tx, ty);
                    const Rect lr{ cv_.to_sx(tx) + 1, cv_.to_sy(ty) + 1, Gui::text_w(std::to_string(i + 1)) + 2, 9 };
                    if (!intersect(lr, area).empty()) {
                        g.rect(lr, rgba(20, 20, 30), kSubText);
                        g.text(lr.x + 1, lr.y + 1, std::to_string(i + 1), i == frame_ ? g.th.accent : g.th.dim, kSubTop);
                    }
                }
        }
        if (tiles_) {
            for (int ty = 0; ty < (img.h + 7) / 8; ++ty)
                for (int tx = 0; tx < (img.w + 7) / 8; ++tx) {
                    const int n = img.tile_colors(tx, ty, 8);
                    if (n > 15) g.frame_rect(Rect{ cv_.to_sx(tx * 8), cv_.to_sy(ty * 8), 8 * cv_.zoom, 8 * cv_.zoom }, g.th.bad, kSubText);
                    else if (n > 8 && cv_.zoom >= 2) g.frame_rect(Rect{ cv_.to_sx(tx * 8), cv_.to_sy(ty * 8), 8 * cv_.zoom, 8 * cv_.zoom }, rgba(120, 110, 60), kSubText);
                }
        }
        g.frame_rect(Rect{ img_r.x - 1, img_r.y - 1, img_r.w + 2, img_r.h + 2 }, g.th.line, kSubOver);

        // selection outline (marching ants)
        if (has_sel_) {
            const int sx = floating_ ? fx_ : selx_, sy = floating_ ? fy_ : sely_;
            const int sw = floating_ ? float_.w : selw_, sh = floating_ ? float_.h : selh_;
            g.dashed(Rect{ cv_.to_sx(sx), cv_.to_sy(sy), sw * cv_.zoom + 1, sh * cv_.zoom + 1 }, g.th.text, rgba(20, 20, 30), int(g.frame / 6), kSubTop);
        }

        // hover + tools
        hover_x_ = hover_y_ = -1;
        const bool over = g.hover(area);
        if (over) {
            const int cx = cv_.to_cx(g.mx()), cy = cv_.to_cy(g.my());
            if (img.in(cx, cy)) {
                hover_x_ = cx; hover_y_ = cy;
                const uint32_t c = img.get(cx, cy);
                g.hint = fmt("(%d, %d)  #%s  a=%d", cx, cy, hex_rgb(c).c_str(), px_a(c));
                const int cell = (cy / cell_h()) * cell_cols() + cx / cell_w();
                if (has_def) g.hint += fmt("  frame %d", cell);
                else if (frame_grid_) g.hint += fmt("  tile %d (GID %d)", cell, cell + 1);
            }
            if (tool_ == Tool::Pencil || tool_ == Tool::Eraser) {
                const int lo = -((brush_ - 1) / 2);
                g.frame_rect(Rect{ cv_.to_sx(cx + lo), cv_.to_sy(cy + lo), brush_ * cv_.zoom, brush_ * cv_.zoom }, g.th.accent, kSubTop);
            }
            if (tool_ != Tool::Hand) g.cursor = PHX_CURSOR_CROSSHAIR;
        }
        g.pop_clip();
        if (tool_ == Tool::Hand) return;
        use_tool(h, area, cid);
    }

    void use_tool(Host& h, const Rect& area, uint32_t cid) {
        Gui& g = h.gui();
        const bool alt = (g.in->mods & kAlt) != 0;
        const bool shift = (g.in->mods & kShift) != 0;
        const uint32_t button = (g.in->pressed & kMouseR) || (stroking_ && (g.in->held & kMouseR)) ? kMouseR : kMouseL;
        const bool secondary = button == kMouseR;
        if (!g.drag(cid, area, button)) {
            if (stroking_) end_stroke();
            return;
        }
        const int cx = cv_.to_cx(g.mx()), cy = cv_.to_cy(g.my());
        const bool first = g.just_pressed(cid);
        Tool t = tool_;
        if (alt) t = Tool::Picker;
        if (t == Tool::Picker) {
            if (img.in(cx, cy)) { if (secondary) bg_ = img.get(cx, cy); else fg_ = img.get(cx, cy); }
            return;
        }
        if (t == Tool::Select) { select_drag(cx, cy, first); return; }
        if (first) {
            commit_float();
            push_img_undo();
            stroking_ = true;
            sx0_ = lastx_ = cx; sy0_ = lasty_ = cy;
            stroke_c_ = t == Tool::Eraser ? 0u : paint_colour(secondary);
            before_ = img.px;
        }
        switch (t) {
        case Tool::Pencil: case Tool::Eraser:
            if (first) dab_mirrored(cx, cy, stroke_c_);
            else line_mirrored(lastx_, lasty_, cx, cy, stroke_c_);
            lastx_ = cx; lasty_ = cy;
            break;
        case Tool::Fill:
            if (first) img.fill(cx, cy, stroke_c_, !shift);
            break;
        case Tool::Line: case Tool::Rect: case Tool::Ellipse:
            img.px = before_;                                       // live preview of the shape
            img.dirty = true;
            if (t == Tool::Line) line_mirrored(sx0_, sy0_, cx, cy, stroke_c_);
            else if (t == Tool::Rect) img.rect(sx0_, sy0_, cx, cy, stroke_c_, shift, brush_);
            else img.ellipse(sx0_, sy0_, cx, cy, stroke_c_, shift);
            g.hint = fmt("%d x %d", std::abs(cx - sx0_) + 1, std::abs(cy - sy0_) + 1);
            break;
        default: break;
        }
    }
    void end_stroke() {
        stroking_ = false;
        before_.clear();
        if (img.same_as_last_snapshot()) { img.drop_undo(); if (!hist_.empty()) hist_.pop_back(); }
        colour_dirty_ = true;
    }

    void select_drag(int cx, int cy, bool first) {
        if (first) {
            const int sx = floating_ ? fx_ : selx_, sy = floating_ ? fy_ : sely_;
            const int sw = floating_ ? float_.w : selw_, sh = floating_ ? float_.h : selh_;
            if (has_sel_ && cx >= sx && cy >= sy && cx < sx + sw && cy < sy + sh) {
                lift_selection(true);                                // drag inside = move the pixels
                fdx_ = cx - fx_; fdy_ = cy - fy_;
                moving_ = true;
            } else {
                commit_float();
                has_sel_ = false;
                moving_ = false;
                sx0_ = cx; sy0_ = cy;
            }
            return;
        }
        if (moving_) { fx_ = cx - fdx_; fy_ = cy - fdy_; selx_ = fx_; sely_ = fy_; return; }
        const int x0 = std::max(0, std::min(sx0_, cx)), y0 = std::max(0, std::min(sy0_, cy));
        const int x1 = std::min(img.w - 1, std::max(sx0_, cx)), y1 = std::min(img.h - 1, std::max(sy0_, cy));
        if (x1 >= x0 && y1 >= y0) { has_sel_ = true; selx_ = x0; sely_ = y0; selw_ = x1 - x0 + 1; selh_ = y1 - y0 + 1; }
    }
    bool moving_ = false;

    // The strip: every frame (sprite def) or tile (image) as a thumbnail — click to select (and, for
    // tiles, zoom onto it); a sprite's selected clip's frames are outlined blue. On the right: the
    // selected clip playing at its fps, or the selected tile at a readable scale with its GID.
    void draw_strip(Host& h, const Rect& r0) {
        Gui& g = h.gui();
        Rect r = r0;
        g.rect(r, g.th.panel, kSubFill);
        g.hline(r.x, r.y, r.w, g.th.line);
        const int pv_h = r.h - 6;
        const int k = std::max(1, std::min(pv_h / cell_h(), 6));
        const int pv_w = std::max(cell_w() * k, 56);
        Rect pvr = cut_right(r, pv_w + 60);
        draw_preview(h, pvr, k);
        g.vline(pvr.x, pvr.y, pvr.h, g.th.line);
        const int n = frame_count();
        const int kt = std::max(1, std::min(4, (r.h - 20) / cell_h()));
        const int tw = cell_w() * kt, tth = cell_h() * kt;
        g.text(r.x + 4, r.y + 3, has_def ? fmt("FRAMES  %d/%d", frame_ + 1, n) : fmt("TILES  %d  (GID %d of %d)", frame_, frame_ + 1, n), g.th.dim);
        const Rect list{ r.x + 4, r.y + 13, r.w - 8, tth + 4 };
        g.push_clip(list);
        int x = list.x - strip_scroll_;
        int c0 = -1, c1 = -1;                    // frames of the selected clip
        if (has_def && clip_ >= 0 && clip_ < int(spr.clips.size())) { c0 = spr.clips[size_t(clip_)].first; c1 = c0 + spr.clips[size_t(clip_)].count; }
        for (int i = 0; i < n; ++i) {
            const Rect fr{ x, list.y + 1, tw + 2, tth + 2 };
            if (fr.right() >= list.x && fr.x <= list.right()) {
                int sx, sy;
                frame_rect(i, sx, sy);
                checkerboard(g, fr.inset(1), 4, kSubWidget);
                g.image(fr.inset(1), tex_, sx, sy, cell_w(), cell_h(), rgba(255, 255, 255), kSubImage);
                const bool in_clip = i >= c0 && i < c1;
                g.frame_rect(fr, i == frame_ ? g.th.accent : in_clip ? g.th.info : g.th.line, kSubOver);
                if (g.hover(fr)) g.hint = has_def ? fmt("frame %d%s  (click to select; , and . step)", i, in_clip ? " - in the selected clip" : "")
                                                  : fmt("tile %d = GID %d in a map  (click to zoom in and edit it; , and . step)", i, i + 1);
                if (g.clicked(fr)) { frame_ = i; center_frame_ = !has_def; }
                if (has_def && g.double_clicked(fr)) center_frame_ = true;
            }
            x += tw + 5;
        }
        // keep the selected cell in view (keyboard stepping)
        const int sel_x = frame_ * (tw + 5);
        if (sel_x < strip_scroll_) strip_scroll_ = sel_x;
        if (sel_x + tw > strip_scroll_ + list.w) strip_scroll_ = sel_x + tw - list.w;
        const int total = n * (tw + 5);
        if (const int w = g.wheel(list)) strip_scroll_ -= w * 20;
        strip_scroll_ = std::max(0, std::min(std::max(0, total - list.w), strip_scroll_));
        g.pop_clip();
    }
    int strip_scroll_ = 0;
    bool center_frame_ = false;

    void draw_preview(Host& h, const Rect& r, int k) {
        Gui& g = h.gui();
        int f = frame_;
        std::string title, sub;
        if (has_def) {
            if (clip_ < 0 || clip_ >= int(spr.clips.size())) { g.text(r.x + 6, r.y + 6, "no clip", g.th.faint); return; }
            const SprClip& pc = spr.clips[size_t(clip_)];
            f = pc.first;
            if (playing_ && pc.fps > 0 && pc.count > 0) {
                const uint64_t step = h.ticks() * uint64_t(pc.fps) / 60u;
                f = pc.first + (pc.loop ? int(step % uint64_t(pc.count))
                                        : int(std::min<uint64_t>(step % uint64_t(pc.count + pc.fps), uint64_t(pc.count - 1))));
            }
            title = pc.name;
            sub = fmt("f%d  %dfps", f, pc.fps);
        } else {
            title = fmt("tile %d", frame_);
            sub = fmt("GID %d", frame_ + 1);
        }
        const Rect pv{ r.x + 6 + (std::max(cell_w() * k, 56) - cell_w() * k) / 2, r.y + 3, cell_w() * k, cell_h() * k };
        int sx, sy;
        frame_rect(f, sx, sy);
        checkerboard(g, pv, 4);
        g.image(pv, tex_, sx, sy, cell_w(), cell_h(), rgba(255, 255, 255), kSubImage);
        g.frame_rect(Rect{ pv.x - 1, pv.y - 1, pv.w + 2, pv.h + 2 }, g.th.line, kSubOver);
        const int bx = r.right() - 54;
        g.text(bx, r.y + 4, title, g.th.text, kSubText, 52);
        g.text(bx, r.y + 14, sub, g.th.faint, kSubText, 52);
        if (has_def) {
            if (g.button(Rect{ bx, r.y + 26, 50, 12 }, playing_ ? "pause" : "play", Btn{ playing_, true, false, 0, "Play / pause the preview (P)" })) playing_ = !playing_;
        } else if (g.button(Rect{ bx, r.y + 26, 50, 12 }, "zoom", Btn{ false, true, false, kIconZoomIn, "Zoom the canvas onto this tile" })) {
            center_frame_ = true;
        }
    }

    void draw_side(Host& h, const Rect& r0) {
        Gui& g = h.gui();
        Rect col = r0;
        g.rect(col, g.th.panel, kSubFill);
        g.vline(col.x, col.y, col.h, g.th.line);
        cut_left(col, 1);
        std::vector<std::string> tabs = { "Colour" };
        if (has_def) tabs.push_back("Clips");
        tabs.push_back("Image");
        side_tab_ = std::min(side_tab_, int(tabs.size()) - 1);
        const int t = g.tabs(cut_top(col, 14), tabs, side_tab_);
        if (t >= 0) side_tab_ = t;
        const std::string& which = tabs[size_t(side_tab_)];
        cut_top(col, 2);
        if (which == "Colour") draw_colour(h, col);
        else if (which == "Clips") draw_anim(h, col);
        else draw_image_info(h, col);
    }
    int side_tab_ = 0;

    void draw_colour(Host& h, Rect& col) {
        Gui& g = h.gui();
        {
            Rect b = panel_section(g, col, 84, "COLOUR");
            const Rect sw{ b.x, b.y, 22, 22 };
            checkerboard(g, sw, 4);
            if (px_a(fg_)) g.rect(sw, fg_, kSubImage);
            g.frame_rect(sw, g.th.line, kSubOver);
            if (hex_ != hex_rgb(fg_) && g.focus() != g.id("hex")) hex_ = px_a(fg_) ? hex_rgb(fg_) : "";
            if (g.text_field(g.id("hex"), Rect{ b.x + 26, b.y, b.w - 26 - 36, 12 }, hex_, "clear", 0, "Hex colour RRGGBB (Enter)")) {
                uint32_t c;
                if (parse_hex_rgb(hex_, c)) fg_ = gba_ ? snap555(c) : c;
            }
            if (g.button(Rect{ b.right() - 34, b.y, 34, 12 }, "none", Btn{ px_a(fg_) == 0, true, false, 0, "Transparent (what the eraser paints)" })) fg_ = 0;
            int rr = px_r(fg_), gg = px_g(fg_), bb = px_b(fg_);
            const int sy = b.y + 15;
            bool ch = false;
            const int sw_ = b.w - 26;
            ch |= g.slider(g.id("r"), Rect{ b.x + 26, sy, sw_, 6 }, rr, 0, 255, rgba(220, 70, 70), "Red");
            ch |= g.slider(g.id("g"), Rect{ b.x + 26, sy + 9, sw_, 6 }, gg, 0, 255, rgba(70, 200, 90), "Green");
            ch |= g.slider(g.id("b"), Rect{ b.x + 26, sy + 18, sw_, 6 }, bb, 0, 255, rgba(80, 120, 230), "Blue");
            if (ch) { fg_ = px_rgba(rr, gg, bb, 255); if (gba_) fg_ = snap555(fg_); }
            // hue strip: click/drag to pick a hue at the current saturation/brightness
            const Rect hue{ b.x, sy + 28, b.w, 8 };
            for (int i = 0; i < hue.w; i += 2) g.rect(Rect{ hue.x + i, hue.y, 2, hue.h }, hsv_to_rgb(i * 360 / std::max(1, hue.w), 230, 240), kSubWidget);
            g.tip(hue, "Pick a hue");
            if (g.drag(g.id("hue"), hue)) {
                int hh, sat, v;
                rgb_to_hsv(fg_, hh, sat, v);
                fg_ = hsv_to_rgb((g.mx() - hue.x) * 360 / std::max(1, hue.w), std::max(sat, 160), std::max(v, 160));
                if (gba_) fg_ = snap555(fg_);
            }
            g.text(b.x, sy + 40, gba_ ? fmt("BGR555 0x%04X", ((px_b(fg_) >> 3) << 10) | ((px_g(fg_) >> 3) << 5) | (px_r(fg_) >> 3))
                                      : std::string("right-click a swatch: secondary"), g.th.faint, kSubText, b.w);
        }
        {
            std::vector<std::string> names = { "this image" };
            for (const auto& p : builtin_palettes()) names.push_back(p.name);
            Rect b = panel_section(g, col, std::max(40, col.h), "PALETTE");
            g.dropdown(g.id("palsel"), Rect{ b.x, b.y, b.w, 12 }, names, pal_sel_, "Swatches: this image's colours or a classic palette");
            std::vector<uint32_t> colours = pal_sel_ == 0 ? img.unique_colors(512) : builtin_palettes()[size_t(pal_sel_ - 1)].colors;
            const int cell = 12, cols = std::max(1, b.w / cell);
            const Rect area{ b.x, b.y + 15, b.w, b.h - 15 };
            const int rows_vis = std::max(1, area.h / cell);
            const int rows = (int(colours.size()) + cols - 1) / cols;
            g.wheel_scroll(area, pal_scroll_, rows, rows_vis, 1);
            g.push_clip(area);
            for (int i = pal_scroll_ * cols; i < int(colours.size()) && i < (pal_scroll_ + rows_vis) * cols; ++i) {
                const int k = i - pal_scroll_ * cols;
                const Rect c{ area.x + (k % cols) * cell, area.y + (k / cols) * cell, cell - 1, cell - 1 };
                g.rect(c, colours[size_t(i)], kSubImage);
                if (colours[size_t(i)] == fg_) g.frame_rect(c.inset(-1), g.th.text, kSubOver);
                else if (colours[size_t(i)] == bg_) g.frame_rect(c.inset(-1), g.th.faint, kSubOver);
                if (g.hover(c)) g.hint = "#" + hex_rgb(colours[size_t(i)]) + "  (left: primary, right: secondary)";
                if (g.clicked(c)) fg_ = colours[size_t(i)];
                if (g.right_clicked(c)) bg_ = colours[size_t(i)];
            }
            g.pop_clip();
            if (colours.empty()) g.text(area.x, area.y + 2, "no colours yet - paint something", g.th.faint, kSubText, area.w);
        }
    }

    void draw_image_info(Host& h, Rect& col) {
        Gui& g = h.gui();
        Rect b = panel_section(g, col, std::max(60, col.h), "IMAGE");
        int y = b.y;
        g.text(b.x, y, fmt("%d x %d px, %zu colours", img.w, img.h, colour_count_), g.th.text, kSubText, b.w);
        y += 12;
        const int over = img.tiles_over_budget(8, 15);
        g.text(b.x, y, over ? fmt("%d tile(s) > 15 colours", over) : std::string("GBA 4bpp: every 8x8 tile fits"), over ? g.th.bad : g.th.good, kSubText, b.w);
        y += 10;
        if (img.w % 8 || img.h % 8) { g.text(b.x, y, "size is not a multiple of 8 (GBA tiles)", g.th.warn, kSubText, b.w); y += 10; }
        y += 6;
        if (!has_def) {
            g.text(b.x, y + 3, "tile size", g.th.dim);
            int tw = tile_w_, th = tile_h_;
            const bool c1 = g.int_field(g.id("tw"), Rect{ b.x + 56, y, 36, 13 }, tw, 1, 256, 1, "Tile width: the grid a tileset is cut into (a map's tile size)");
            g.text(b.x + 95, y + 3, "x", g.th.dim);
            const bool c2 = g.int_field(g.id("th"), Rect{ b.x + 103, y, 36, 13 }, th, 1, 256, 1, "Tile height");
            if (c1 || c2) { tile_w_ = tw; tile_h_ = th; frame_grid_ = true; frame_ = std::min(frame_, frame_count() - 1); }
            y += 17;
            g.text(b.x, y, fmt("%d tiles (GID 1..%d)", frame_count(), frame_count()), g.th.faint, kSubText, b.w);
            y += 12;
            Btn tb; tb.help = "Grow the image by one row of tiles (more tiles for a tileset)"; tb.left = true; tb.icon = kIconPlus;
            if (g.button(Rect{ b.x, y, b.w, 14 }, "add a row of tiles", tb)) edit_img([&] { img.resize(img.w, img.h + tile_h_, 0, 0); });
            y += 17;
        }
        Btn rb; rb.help = "Change the canvas size (keeps the pixels, anchored)"; rb.left = true; rb.icon = kIconRect;
        if (g.button(Rect{ b.x, y, b.w, 14 }, "resize canvas...", rb)) resize_dialog(h);
        y += 17;
        Btn sb; sb.help = "Scale the whole image up 2x (nearest neighbour)"; sb.left = true; sb.icon = kIconZoomIn;
        if (g.button(Rect{ b.x, y, b.w, 14 }, "scale x2", sb)) { edit_img([&] { img.scale_up(2); }); if (has_def) { spr_edit(); spr.frame_w *= 2; spr.frame_h *= 2; } cv_.fitted = false; }
        y += 17;
        Btn gb; gb.help = "Snap every pixel to a colour the GBA can show (BGR555)"; gb.left = true; gb.icon = kIconGba;
        if (g.button(Rect{ b.x, y, b.w, 14 }, "snap to GBA colours", gb)) edit_img([&] { img.snap_all_555(); });
        y += 17;
        if (has_def) {
            Btn ab; ab.help = "Widen the sheet by one frame (a blank frame at the end)"; ab.left = true; ab.icon = kIconPlus;
            if (g.button(Rect{ b.x, y, b.w, 14 }, "append a frame", ab)) append_frame();
        } else {
            Btn bb; bb.icon = kIconSprite; bb.left = true; bb.help = "Write a .sprdef next to this PNG (frame size + clips) and edit it as a sprite";
            if (g.button(Rect{ b.x, y, b.w, 14 }, "make sprite def...", bb)) mkdef_dialog(h);
        }
        y += 22;
        g.text(b.x, y, "saved as", g.th.faint);
        g.text(b.x, y + 10, base_name(img_path), g.th.dim, kSubText, b.w);
        g.text(b.x, y + 20, "(an indexed PNG when <= 256 colours)", g.th.faint, kSubText, b.w);
    }

    void draw_anim(Host& h, Rect& col) {
        Gui& g = h.gui();
        {
            Rect b = panel_section(g, col, 32, "FRAME SIZE");
            int fw = spr.frame_w, fh = spr.frame_h;
            const bool c1 = g.int_field(g.id("fw"), Rect{ b.x, b.y, 44, 12 }, fw, 1, 512, 1, "Frame width (the grid the sheet is cut into)");
            g.text(b.x + 48, b.y + 2, "x", g.th.dim);
            const bool c2 = g.int_field(g.id("fh"), Rect{ b.x + 56, b.y, 44, 12 }, fh, 1, 512, 1, "Frame height");
            g.text(b.x + 106, b.y + 2, fmt("%d frames", frame_count()), g.th.faint, kSubText, b.w - 106);
            if (c1 || c2) { spr_edit(); spr.frame_w = fw; spr.frame_h = fh; frame_ = std::min(frame_, frame_count() - 1); }
        }
        Rect b = panel_section(g, col, std::max(120, col.h), "CLIPS");
        const int rows = std::max(2, std::min(int(spr.clips.size()), 6));
        const Rect list{ b.x, b.y, b.w, rows * 11 + 2 };
        g.rect(list, g.th.field, kSubWidget);
        g.wheel_scroll(list, clip_scroll_, int(spr.clips.size()), rows, 1);
        for (int i = 0; i < rows && clip_scroll_ + i < int(spr.clips.size()); ++i) {
            const int k = clip_scroll_ + i;
            const SprClip& c = spr.clips[size_t(k)];
            const Rect rr{ list.x + 1, list.y + 1 + i * 11, list.w - 2, 11 };
            if (k == clip_) g.rect(rr, g.th.sel, kSubImage);
            else if (g.hover(rr)) g.rect(rr, g.th.hover, kSubImage);
            g.text(rr.x + 3, rr.y + 2, c.name, g.th.text, kSubText, rr.w - 50);
            g.text(rr.right() - 46, rr.y + 2, fmt("%d+%d", c.first, c.count), g.th.faint);
            if (g.clicked(rr)) { clip_ = k; frame_ = c.first; }
        }
        if (spr.clips.empty()) g.text(list.x + 3, list.y + 3, "no clips", g.th.faint);
        int y = list.bottom() + 3;
        {
            Gui::Row row(Rect{ b.x, y, b.w, 12 }, 3);
            if (g.button(row.take(44), "+ clip", Btn{ false, true, false, 0, "Add a clip starting at the selected frame" })) {
                spr_edit();
                spr.clips.push_back(SprClip{ spr.fresh_name("clip"), frame_, 1, 8, true });
                clip_ = int(spr.clips.size()) - 1;
            }
            const bool have = clip_ >= 0 && clip_ < int(spr.clips.size());
            if (g.button(row.take(40), "delete", Btn{ false, have, false, 0, "Remove the selected clip" }) && have) {
                spr_edit();
                spr.clips.erase(spr.clips.begin() + clip_);
                clip_ = std::min(clip_, int(spr.clips.size()) - 1);
            }
            if (g.icon_button(row.take(12), kIconUp, "Move clip up", false, have && clip_ > 0) && have && clip_ > 0) { spr_edit(); std::swap(spr.clips[size_t(clip_)], spr.clips[size_t(clip_ - 1)]); --clip_; }
            if (g.icon_button(row.take(12), kIconDown, "Move clip down", false, have && clip_ + 1 < int(spr.clips.size())) && have && clip_ + 1 < int(spr.clips.size())) { spr_edit(); std::swap(spr.clips[size_t(clip_)], spr.clips[size_t(clip_ + 1)]); ++clip_; }
        }
        y += 17;
        if (clip_ >= 0 && clip_ < int(spr.clips.size())) {
            SprClip c = spr.clips[size_t(clip_)];
            bool ch = false;
            g.text(b.x, y + 2, "name", g.th.dim);
            std::string nm = c.name;
            if (g.text_field(g.id("cname"), Rect{ b.x + 38, y, b.w - 38, 12 }, nm, "clip name", 0, "The name the game plays: animator.play(\"walk\"_hash)")) {
                std::string clean;
                for (char ch2 : nm) if (ch2 != ' ' && ch2 != '"' && ch2 != '#') clean += ch2;
                if (!clean.empty() && (clean == c.name || spr.find_clip(clean) < 0)) { c.name = clean; ch = true; }
                else h.toast("clip names must be unique and non-empty", Toast::Warn);
            }
            y += 15;
            g.text(b.x, y + 2, "frames", g.th.dim);
            ch |= g.int_field(g.id("cfirst"), Rect{ b.x + 38, y, 34, 12 }, c.first, 0, std::max(0, frame_count() - 1), 1, "First frame");
            g.text(b.x + 75, y + 2, "+", g.th.dim);
            ch |= g.int_field(g.id("ccount"), Rect{ b.x + 82, y, 34, 12 }, c.count, 1, std::max(1, frame_count() - c.first), 1, "Frame count (a contiguous run)");
            y += 15;
            g.text(b.x, y + 2, "fps", g.th.dim);
            ch |= g.int_field(g.id("cfps"), Rect{ b.x + 38, y, 34, 12 }, c.fps, 0, 60, 1, "Playback rate (baked as a u8)");
            bool loop = c.loop;
            if (g.checkbox(Rect{ b.x + 82, y, 50, 12 }, "loop", loop)) { c.loop = loop; ch = true; }
            y += 16;
            if (ch) { spr_edit(); spr.clips[size_t(clip_)] = c; }
            if (g.button(Rect{ b.x, y, b.w, 12 }, "set frames to the selection in the strip", Btn{ false, true, false, 0, "first = the selected frame" })) {
                spr_edit(); spr.clips[size_t(clip_)].first = frame_; spr.clips[size_t(clip_)].count = std::max(1, std::min(spr.clips[size_t(clip_)].count, frame_count() - frame_));
            }
            y += 16;
        }
        // problems the bake would hit
        const std::vector<std::string> probs = spr.validate(img.w, img.h);
        for (size_t i = 0; i < probs.size() && y + 10 < b.bottom(); ++i) {
            g.icon(b.x, y, kIconWarning, g.th.warn);
            g.text(b.x + 13, y + 1, probs[i], g.th.warn, kSubText, b.w - 14);
            g.tip(Rect{ b.x, y, b.w, 11 }, probs[i]);
            y += 11;
        }
        if (probs.empty() && y + 10 < b.bottom()) g.text(b.x, y, "bakes clean (phxsprite)", g.th.good, kSubText, b.w);
    }

    // ---- dialogs ----
    void resize_dialog(Host& h) {
        struct St { int w, hh, ax = 1, ay = 1; };
        auto st = std::make_shared<St>();
        st->w = img.w; st->hh = img.h;
        h.modal("Resize canvas", 260, 120, [this, st](Gui& g, Rect body) {
            g.text(body.x, body.y + 3, "size", g.th.dim);
            g.int_field(g.id("rw"), Rect{ body.x + 50, body.y, 50, 13 }, st->w, 1, 4096, 1);
            g.text(body.x + 104, body.y + 3, "x", g.th.dim);
            g.int_field(g.id("rh"), Rect{ body.x + 112, body.y, 50, 13 }, st->hh, 1, 4096, 1);
            g.text(body.x, body.y + 22, "anchor", g.th.dim);
            for (int ay = 0; ay < 3; ++ay)
                for (int ax = 0; ax < 3; ++ax)
                    if (g.button(Rect{ body.x + 50 + ax * 14, body.y + 20 + ay * 12, 13, 11 }, "", Btn{ st->ax == ax && st->ay == ay }))
                        { st->ax = ax; st->ay = ay; }
            const int y = body.bottom() - 14;
            if (g.button(Rect{ body.x, y, 70, 14 }, "Resize", Btn{ true }) ) {
                edit_img([&] { img.resize(st->w, st->hh, st->ax, st->ay); });
                cv_.fitted = false;
                has_sel_ = false;
                return false;
            }
            if (g.button(Rect{ body.x + 76, y, 70, 14 }, "Cancel") || g.key(PHX_KEY_ESCAPE)) return false;
            return true;
        });
    }

    void mkdef_dialog(Host& h) {
        struct St { int fw, fh; std::string err; };
        auto st = std::make_shared<St>();
        st->fw = std::min(16, img.w); st->fh = std::min(16, img.h);
        h.modal("Make a sprite definition", 300, 100, [this, st, &h](Gui& g, Rect body) {
            g.text(body.x, body.y + 3, "frame", g.th.dim);
            g.int_field(g.id("mw"), Rect{ body.x + 50, body.y, 50, 13 }, st->fw, 1, 1024, 1);
            g.text(body.x + 104, body.y + 3, "x", g.th.dim);
            g.int_field(g.id("mh"), Rect{ body.x + 112, body.y, 50, 13 }, st->fh, 1, 1024, 1);
            const std::string def = dir_name(img_path) + "/" + stem_of(img_path) + ".sprdef";
            g.text(body.x, body.y + 20, "writes " + base_name(def), g.th.faint, kSubText, body.w);
            if (!st->err.empty()) g.text(body.x, body.y + 32, st->err, g.th.bad, kSubText, body.w);
            const int y = body.bottom() - 14;
            if (g.button(Rect{ body.x, y, 70, 14 }, "Create", Btn{ true })) {
                if (pfs::exists(def)) { st->err = base_name(def) + " already exists"; return true; }
                SprDoc sd;
                sd.sheet = base_name(img_path); sd.frame_w = st->fw; sd.frame_h = st->fh;
                sd.clips.push_back(SprClip{ "idle", 0, std::max(1, sd.frame_count(img.w, img.h)), 8, true });
                std::string err;
                if (img.dirty && !img.save_png(img_path, &err)) { st->err = err; return true; }
                if (!sd.save(def, &err)) { st->err = err; return true; }
                h.file_saved(def);
                h.open_file(def);
                return false;
            }
            if (g.button(Rect{ body.x + 76, y, 70, 14 }, "Cancel") || g.key(PHX_KEY_ESCAPE)) return false;
            return true;
        });
    }
};

} // namespace

std::unique_ptr<DocView> make_sprite_view(Host& h, const std::string& path, std::string* err) {
    std::unique_ptr<SpriteView> v(new SpriteView);
    if (!v->load(h, path, err)) return nullptr;
    return v;
}

} // namespace phxstudio
