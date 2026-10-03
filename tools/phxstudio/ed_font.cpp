// tools/phxstudio/ed_font.cpp — the FONT editor panel: a `.font` document (tools/phxpack/font.h)
// over a grid sheet PNG. Set the cell size, the first character and how the glyphs advance
// (proportional: each glyph's opaque width + spacing, measured from the pixels exactly as the bake
// measures them; or fixed); see the glyph boxes on the sheet and a sample line laid out the way
// phx::UI will draw it. Click a glyph to paint it in the pixel editor. The bake (phxsprite /
// bake_project.py) turns it into the sheet texture + a Font asset named after the file, which
// phx::load_font (phx/runtime/font.h) and the game flow use.
#include "host.h"
#include "pixeldoc.h"
#include "font.h"             // tools/phxpack

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace phxstudio {
namespace {

using namespace twk;
using phxtool::FontDef;

class FontView final : public DocView {
public:
    FontDef def;
    bool dirty_ = false;

    bool load(Host& h, const std::string& path_, std::string* err) {
        path = path_;
        std::string text;
        if (FILE* f = std::fopen(path.c_str(), "rb")) {
            char buf[8192];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
            std::fclose(f);
        } else {
            if (err) *err = "cannot open " + path;
            return false;
        }
        if (!phxtool::load_fontdef(text, path, def, err)) return false;
        disk_stamp = file_stamp(path);
        dirty_ = false;
        load_sheet(h);
        return true;
    }

    FileKind kind() const override { return FileKind::Font; }
    int icon() const override { return kIconFileImage; }
    bool dirty() const override { return dirty_; }
    bool save(Host& h, std::string* err) override {
        const std::string t = phxtool::fontdef_to_json(def);
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) { if (err) *err = "cannot write " + path; return false; }
        const bool ok = std::fwrite(t.data(), 1, t.size(), f) == t.size();
        std::fclose(f);
        if (!ok) { if (err) *err = "short write to " + path; return false; }
        dirty_ = false;
        disk_stamp = file_stamp(path);
        h.file_saved(path);
        return true;
    }
    bool reload(Host& h, std::string* err) override {
        FontView fresh;
        if (!fresh.load(h, path, err)) return false;
        def = fresh.def; dirty_ = false; disk_stamp = fresh.disk_stamp;
        sheet_stamp_ = -1;
        load_sheet(h);
        return true;
    }
    bool undo() override {
        if (undo_.empty()) return false;
        redo_.push_back(def); def = undo_.back(); undo_.pop_back(); dirty_ = true; measured_ = false;
        return true;
    }
    bool redo() override {
        if (redo_.empty()) return false;
        undo_.push_back(def); def = redo_.back(); redo_.pop_back(); dirty_ = true; measured_ = false;
        return true;
    }
    std::string status() const override {
        return fmt("%u glyphs from '%c'  line %u  %s  sheet %dx%d", unsigned(glyphs_.size()),
                   def.first >= 32 && def.first < 127 ? char(def.first) : '?', unsigned(hdr_.line_h),
                   def.proportional ? "proportional" : "fixed", sheet_.w, sheet_.h);
    }
    void release(Host& h) override {
        h.renderer().unload_texture(tex_);
        tex_ = kNoTexture;
    }

    void draw(Host& h, const Rect& area) override {
        Gui& g = h.gui();
        if (!id_) id_ = Gui::hash(path.c_str());
        g.push_id(id_);
        if (h.ticks() - sheet_checked_ >= 30) { sheet_checked_ = h.ticks(); load_sheet(h); }   // the sheet was painted
        measure();
        upload(h);
        Rect r = area;
        g.rect(r, g.th.bg, kSubBg);
        const Rect bar = cut_top(r, 18);
        Rect side = cut_right(r, std::min(240, std::max(200, area.w / 3)));
        draw_bar(h, bar);
        draw_settings(h, side);
        const Rect sample = cut_bottom(r, std::min(90, std::max(56, r.h / 3)));
        draw_sheet(h, r);
        draw_sample(h, sample);
        g.pop_id();
    }

private:
    uint32_t id_ = 0;
    std::vector<FontDef> undo_, redo_;
    PixelDoc sheet_;
    std::string sheet_path_, sheet_err_;
    int64_t sheet_stamp_ = -1;
    uint64_t sheet_checked_ = 0;
    TextureId tex_ = kNoTexture;
    const uint32_t* tex_ptr_ = nullptr;
    int tex_w_ = 0, tex_h_ = 0;
    bool measured_ = false;
    FontDef measured_def_;
    phx::FontBlobHeader hdr_{};
    std::vector<phx::FontGlyphDef> glyphs_;
    std::string measure_err_;
    std::string sample_ = "The quick brown fox jumps over the lazy dog. 0123456789!";
    int hover_ = -1;

    template <class F> void edit(F f) {
        FontDef before = def;
        f();
        if (def.image == before.image && def.cell_w == before.cell_w && def.cell_h == before.cell_h &&
            def.first == before.first && def.count == before.count && def.proportional == before.proportional &&
            def.spacing == before.spacing && def.space == before.space && def.advance == before.advance &&
            def.line_h == before.line_h)
            return;
        undo_.push_back(before);
        if (undo_.size() > 200) undo_.erase(undo_.begin());
        redo_.clear();
        dirty_ = true;
        measured_ = false;
        if (def.image != before.image) sheet_stamp_ = -1;
    }

    void load_sheet(Host& h) {
        const std::string p = phxtool::font_image_path(path, def.image);
        const int64_t st = file_stamp(p);
        if (p == sheet_path_ && st == sheet_stamp_) return;
        sheet_path_ = p; sheet_stamp_ = st; sheet_err_.clear();
        measured_ = false;
        if (h.access(p) == Access::None) { sheet_ = PixelDoc{}; sheet_err_ = "the sheet '" + def.image + "' is outside the project"; return; }
        PixelDoc d;
        std::string err;
        if (!PixelDoc::load_png(p, d, &err)) { sheet_ = PixelDoc{}; sheet_err_ = "cannot load the sheet: " + err; return; }
        sheet_ = std::move(d);
    }
    void upload(Host& h) {
        if (sheet_.px.empty()) return;
        if (tex_ != kNoTexture && tex_ptr_ == sheet_.px.data() && tex_w_ == sheet_.w && tex_h_ == sheet_.h) return;
        phx::Renderer& r = h.renderer();
        r.unload_texture(tex_);
        phx::TextureDesc d{};
        d.pixels = sheet_.px.data();
        d.size = uint32_t(sheet_.px.size() * 4);
        d.width = uint16_t(sheet_.w); d.height = uint16_t(sheet_.h);
        tex_ = r.load_texture(d);
        tex_ptr_ = sheet_.px.data(); tex_w_ = sheet_.w; tex_h_ = sheet_.h;
    }
    void measure() {
        if (measured_) return;
        measured_ = true;
        glyphs_.clear();
        hdr_ = phx::FontBlobHeader{};
        measure_err_.clear();
        if (sheet_.px.empty()) return;
        if (!phxtool::font_glyphs_from_grid(def, sheet_.px, sheet_.w, sheet_.h, hdr_, glyphs_, &measure_err_)) glyphs_.clear();
    }

    void draw_bar(Host& h, const Rect& bar) {
        Gui& g = h.gui();
        g.rect(bar, g.th.panel, kSubFill);
        Gui::Row row(Rect{ bar.x + 4, bar.y + 3, bar.w - 8, 12 }, 4);
        Btn eb; eb.icon = kIconPencil; eb.help = "Open the sheet in the pixel editor (the font updates when you save it)";
        if (g.button(row.take(84), "edit sheet", eb) && !sheet_.px.empty()) h.open_file(sheet_path_);
        g.text(row.x + 6, bar.y + 5, fmt("-> load_font(r, *res, \"%s\"_hash, font)", stem_of(path).c_str()), g.th.faint,
               kSubText, row.rest().w - 6);
    }

    void draw_settings(Host& h, Rect col) {
        Gui& g = h.gui();
        Rect b = panel_section(g, col, std::max(120, col.h), "FONT");
        int y = b.y;
        const int lw = 70, fw = b.w - lw;
        auto label = [&](const char* t, const char* help) { g.text(b.x, y + 2, t, g.th.dim); g.tip(Rect{ b.x, y, lw, 12 }, help); };
        {
            label("sheet", "The PNG (a bare name sits next to this file)");
            std::string img = def.image;
            if (g.text_field(g.id("img"), Rect{ b.x + lw, y, fw, 12 }, img, "font.png", 0, "The sheet PNG") && img != def.image)
                edit([&] { def.image = img; });
            y += 15;
        }
        auto num = [&](const char* name, int& v, int lo, int hi, const char* help) {
            label(name, help);
            int t = v;
            if (g.int_field(g.id(name), Rect{ b.x + lw, y, 50, 12 }, t, lo, hi, 1, help) && t != v) edit([&] { v = t; });
            y += 15;
        };
        num("cell w", def.cell_w, 1, 255, "Width of one grid cell (px)");
        num("cell h", def.cell_h, 1, 255, "Height of one grid cell (px)");
        num("first", def.first, 0, 255, "The character in cell 0 (32 = space)");
        num("count", def.count, 0, 256, "Glyphs to use (0 = every cell)");
        {
            bool prop = def.proportional;
            if (g.checkbox(Rect{ b.x, y, b.w, 12 }, "proportional", prop,
                           "Each glyph advances by its own width (measured from its pixels) + spacing; off = fixed width"))
                edit([&] { def.proportional = prop; });
            y += 16;
        }
        if (def.proportional) {
            num("spacing", def.spacing, 0, 16, "Pixels between glyphs");
            num("space", def.space, 0, 64, "The advance of an empty cell such as space (0 = half a cell)");
        } else {
            num("advance", def.advance, 0, 255, "Pixels per character (0 = the cell width)");
        }
        num("line h", def.line_h, 0, 255, "Pixels per line (0 = cell height + 1)");
        y += 4;
        if (!sheet_err_.empty()) { g.text(b.x, y, sheet_err_, g.th.bad, kSubText, b.w); y += 11; }
        if (!measure_err_.empty()) { g.text(b.x, y, measure_err_, g.th.bad, kSubText, b.w); y += 11; }
        if (sheet_.w && (sheet_.w % def.cell_w || sheet_.h % def.cell_h))
            { g.text(b.x, y, fmt("the %dx%d sheet is not whole cells", sheet_.w, sheet_.h), g.th.warn, kSubText, b.w); y += 11; }
        if (!glyphs_.empty()) {
            g.text(b.x, y, fmt("%u glyphs  '%c'..'%c'", unsigned(glyphs_.size()), char(std::max(32, def.first)),
                               char(std::min(126, def.first + int(glyphs_.size()) - 1))), g.th.good, kSubText, b.w);
            y += 11;
            g.text(b.x, y, fmt("line %u  widest advance %u", unsigned(hdr_.line_h), unsigned(hdr_.advance)), g.th.dim, kSubText, b.w);
            y += 14;
        }
        if (hover_ >= 0 && hover_ < int(glyphs_.size())) {
            const phx::FontGlyphDef& gl = glyphs_[size_t(hover_)];
            const int c = def.first + hover_;
            g.text(b.x, y, fmt("'%c' (%d): %ux%u at %u,%u  advance %u", c >= 33 && c < 127 ? char(c) : ' ', c, unsigned(gl.w),
                               unsigned(gl.h), unsigned(gl.sx), unsigned(gl.sy), unsigned(gl.advance)),
                   g.th.text, kSubText, b.w);
        }
    }

    // The sheet, scaled up, with the cell grid and each glyph's measured box.
    void draw_sheet(Host& h, Rect r) {
        Gui& g = h.gui();
        r = r.inset(6);
        hover_ = -1;
        if (sheet_.px.empty() || tex_ == kNoTexture) { g.text(r.x, r.y, sheet_err_.empty() ? "no sheet" : sheet_err_, g.th.warn); return; }
        const int z = std::max(1, std::min((r.w) / std::max(1, sheet_.w), (r.h) / std::max(1, sheet_.h)));
        const Rect img{ r.x, r.y, sheet_.w * z, sheet_.h * z };
        g.rect(img, g.th.panel2, kSubWidget);
        g.image(img, tex_, 0, 0, sheet_.w, sheet_.h);
        const int cols = def.cell_w ? sheet_.w / def.cell_w : 0;
        for (int x = 0; x <= sheet_.w; x += std::max(1, def.cell_w)) g.rect(Rect{ img.x + x * z, img.y, 1, img.h }, g.th.line, kSubOver);
        for (int y = 0; y <= sheet_.h; y += std::max(1, def.cell_h)) g.rect(Rect{ img.x, img.y + y * z, img.w, 1 }, g.th.line, kSubOver);
        for (size_t i = 0; i < glyphs_.size(); ++i) {
            const phx::FontGlyphDef& gl = glyphs_[i];
            if (!gl.w) continue;
            const Rect gr{ img.x + gl.sx * z, img.y + gl.sy * z, gl.w * z, gl.h * z };
            g.frame_rect(gr, g.th.accent, kSubOver);
        }
        if (g.hover(img) && cols > 0) {
            const int cx = (g.mx() - img.x) / z / std::max(1, def.cell_w), cy = (g.my() - img.y) / z / std::max(1, def.cell_h);
            const int idx = cy * cols + cx;
            if (idx >= 0 && idx < int(glyphs_.size())) {
                hover_ = idx;
                g.frame_rect(Rect{ img.x + cx * def.cell_w * z, img.y + cy * def.cell_h * z, def.cell_w * z, def.cell_h * z }, g.th.text, kSubTop);
                const int c = def.first + idx;
                g.hint = fmt("'%c' (%d) - click to paint it", c >= 33 && c < 127 ? char(c) : ' ', c);
                if (g.clicked(img)) h.open_tile(sheet_path_, cy * cols + cx, def.cell_w, def.cell_h);
            }
        }
    }

    // A sample line laid out exactly as phx::UI draws it (glyph rects, offsets, advances).
    void draw_sample(Host& h, Rect r) {
        Gui& g = h.gui();
        Rect b = panel_section(g, r, r.h, "SAMPLE");
        g.text_field(g.id("sample"), Rect{ b.x, b.y, b.w, 12 }, sample_, "type to preview", kFieldLive, "Preview text");
        if (glyphs_.empty() || tex_ == kNoTexture) return;
        const int z = 2;
        int x = b.x, w = 0;
        const int y = b.y + 18;
        for (unsigned char c : sample_) {
            const int i = int(c) - def.first;
            const bool in = i >= 0 && i < int(glyphs_.size());
            const int adv = in ? glyphs_[size_t(i)].advance : hdr_.advance;
            if (in) {
                const phx::FontGlyphDef& gl = glyphs_[size_t(i)];
                if (gl.w && x + (gl.xoff + gl.w) * z <= b.right())
                    g.image(Rect{ x + gl.xoff * z, y + gl.yoff * z, gl.w * z, gl.h * z }, tex_, gl.sx, gl.sy, gl.w, gl.h);
            }
            x += adv * z; w += adv;
        }
        g.text(b.x, y + int(hdr_.line_h) * z + 4, fmt("%d px wide (UI::text_width)", w), g.th.faint);
    }
};

} // namespace

std::unique_ptr<DocView> make_font_view(Host& h, const std::string& path, std::string* err) {
    std::unique_ptr<FontView> v(new FontView);
    if (!v->load(h, path, err)) return nullptr;
    return v;
}

} // namespace phxstudio
