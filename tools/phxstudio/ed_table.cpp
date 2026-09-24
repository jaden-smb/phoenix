// tools/phxstudio/ed_table.cpp — the DATA TABLE editor panel (a rebuild of phxentity's GUI on the
// tool widget kit): a spreadsheet over a phxbin record table (tools/phxentity/editor.h, unit-
// tested). Every value you can type is one the baked struct can hold — integers clamp to their
// type, strings clip to their char[N], names must be C identifiers.
//
//   arrows / Tab / Shift+Tab / Home / End / PgUp / PgDn   move the cell cursor
//   Enter or F2 or double-click    edit the cell (Enter commits and moves down, Tab moves right)
//   typing                         starts editing with what you type      Esc   cancel the edit
//   + / -  (Shift: 10)             step a number                          Delete clear the cell
//   Ctrl+D duplicate record · Ctrl+Enter insert a record · right-click a header / row number
#include "host.h"
#include "../phxentity/editor.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace phxstudio {
namespace {

using namespace twk;
using phxtool::BinDoc;

class TableView final : public DocView {
public:
    BinDoc doc;

    bool load(const std::string& p, std::string* err) {
        path = p;
        std::string text;
        FILE* f = std::fopen(p.c_str(), "rb");
        if (!f) { if (err) *err = "cannot open " + p; return false; }
        char buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
        std::fclose(f);
        std::string e;
        if (!BinDoc::load(text, doc, &e)) { if (err) *err = base_name(p) + ": " + e; return false; }
        disk_stamp = file_stamp(p);
        return true;
    }

    FileKind kind() const override { return FileKind::Table; }
    int icon() const override { return kIconFileTable; }
    bool dirty() const override { return doc.dirty; }
    bool save(Host& h, std::string* err) override {
        if (!doc.save_file(path)) { if (err) *err = "cannot write " + path; return false; }
        disk_stamp = file_stamp(path);
        h.file_saved(path);
        return true;
    }
    bool reload(Host&, std::string* err) override {
        TableView fresh;
        if (!fresh.load(path, err)) return false;
        doc = fresh.doc;
        disk_stamp = fresh.disk_stamp;
        clamp_cursor();
        return true;
    }
    bool undo() override { editing_ = false; const bool r = doc.undo(); clamp_cursor(); return r; }
    bool redo() override { editing_ = false; const bool r = doc.redo(); clamp_cursor(); return r; }
    std::string status() const override {
        std::string s = fmt("%s  %zu fields  %zu records", doc.struct_name.c_str(), doc.fields.size(), doc.records.size());
        if (col_ < int(doc.fields.size())) {
            const BinDoc::Field& f = doc.fields[size_t(col_)];
            s += "   " + f.name + ": " + f.type + "  " + type_help(f.type);
        }
        return s;
    }
    std::vector<Action> actions() override {
        std::vector<Action> a;
        a.push_back({ "Add record", "", [this] { edit([&] { doc.add_record(size_t(row_)); }); row_ = int(doc.records.size()) - 1; } });
        a.push_back({ "Insert record above", "Ctrl+Enter", [this] { edit([&] { doc.insert_record(size_t(row_), false); }); } });
        a.push_back({ "Duplicate record", "Ctrl+D", [this] { dup(); } });
        a.push_back({ "Delete record", "", [this] { del_row(); } });
        a.push_back({ "Add field...", "", [this] { want_add_field_ = true; } });
        return a;
    }

    void draw(Host& h, const Rect& area) override {
        Gui& g = h.gui();
        if (!id_) id_ = Gui::hash(path.c_str());
        g.push_id(id_);
        if (want_add_field_) { want_add_field_ = false; add_field_dialog(h); }
        clamp_cursor();
        Rect r = area;
        g.rect(r, g.th.bg, kSubBg);
        const Rect bar = cut_top(r, 18);
        const Rect side = cut_right(r, std::min(210, std::max(160, area.w / 4)));
        draw_bar(h, bar);
        keys(h);
        draw_grid(h, r);
        draw_side(h, side);
        g.pop_id();
    }

private:
    uint32_t id_ = 0;
    int row_ = 0, col_ = 0;
    int top_ = 0, left_ = 0;                  // first visible row, horizontal scroll (px)
    bool editing_ = false;
    std::string edit_text_;
    bool follow_ = false;
    bool want_add_field_ = false;
    int ctx_col_ = -1, ctx_row_ = -1;

    template <class F> void edit(F f) { doc.push_undo(); f(); }
    void clamp_cursor() {
        row_ = std::max(0, std::min(row_, int(doc.records.size()) - 1));
        col_ = std::max(0, std::min(col_, int(doc.fields.size()) - 1));
    }
    void dup() { if (doc.records.empty()) return; edit([&] { doc.insert_record(size_t(row_ + 1), true); }); ++row_; follow_ = true; }
    void del_row() { if (doc.records.empty()) return; edit([&] { doc.remove_record(size_t(row_)); }); clamp_cursor(); }

    static std::string type_help(const std::string& t) {
        if (BinDoc::str_type(t)) return fmt("text, up to %zu chars", BinDoc::str_capacity(t));
        if (BinDoc::flt_type(t)) return "32-bit float";
        int64_t lo, hi;
        BinDoc::type_range(t, lo, hi);
        return fmt("integer %lld..%lld", (long long)lo, (long long)hi);
    }

    int col_width(size_t f) const {
        size_t m = std::max(doc.fields[f].name.size(), doc.fields[f].type.size() + 0);
        for (size_t r = 0; r < doc.records.size() && r < 400; ++r) m = std::max(m, doc.cell_text(r, f).size());
        return int(std::max<size_t>(5, std::min<size_t>(26, m))) * kAdv + 10;
    }

    void begin_edit(Gui& g, const std::string& initial, bool select_all) {
        if (doc.records.empty() || doc.fields.empty()) return;
        editing_ = true;
        edit_text_ = doc.cell_text(size_t(row_), size_t(col_));
        g.set_focus(g.id("cell"), true);
        g.edit().set(initial, select_all);
        if (!select_all) g.edit().move_to(int(initial.size()), false);
    }

    void step(int64_t d) {
        if (doc.records.empty() || doc.field_is_str(size_t(col_))) return;
        edit([&] { doc.step(size_t(row_), size_t(col_), d); });
    }

    // Is there an unused key-down for k (without consuming it)?
    static bool peek(Gui& g, int32_t k, uint16_t mods = 0) {
        for (const KeyEvent& e : g.in->keys) if (!e.used && e.key == k && norm_mods(e.mods) == mods) return true;
        return false;
    }

    void keys(Host& h) {
        Gui& g = h.gui();
        if (h.modal_open() || editing_) return;
        if (g.text_focus()) return;                          // struct-name field etc.
        const int nr = int(doc.records.size()), nc = int(doc.fields.size());
        auto mv = [&](int dr, int dc) { row_ = std::max(0, std::min(nr - 1, row_ + dr)); col_ = std::max(0, std::min(nc - 1, col_ + dc)); follow_ = true; };
        if (g.key(PHX_KEY_UP)) mv(-1, 0);
        if (g.key(PHX_KEY_DOWN)) mv(1, 0);
        if (g.key(PHX_KEY_LEFT)) mv(0, -1);
        if (g.key(PHX_KEY_RIGHT)) mv(0, 1);
        if (g.key(PHX_KEY_TAB)) { if (col_ + 1 < nc) mv(0, 1); else { col_ = 0; mv(1, 0); } }
        if (g.key(PHX_KEY_TAB, kShift)) { if (col_ > 0) mv(0, -1); else { col_ = nc - 1; mv(-1, 0); } }
        if (g.key(PHX_KEY_HOME)) mv(0, -nc);
        if (g.key(PHX_KEY_END)) mv(0, nc);
        if (g.key(PHX_KEY_HOME, kCtrl)) mv(-nr, -nc);
        if (g.key(PHX_KEY_END, kCtrl)) mv(nr, nc);
        if (g.key(PHX_KEY_PAGE_UP)) mv(-10, 0);
        if (g.key(PHX_KEY_PAGE_DOWN)) mv(10, 0);
        if (g.key(PHX_KEY_ENTER) || g.key(PHX_KEY_F2)) begin_edit(g, doc.cell_text(size_t(row_), size_t(col_)), false);
        if (g.key(PHX_KEY_ENTER, kCtrl)) { edit([&] { doc.insert_record(size_t(row_), false); }); follow_ = true; }
        if (g.key('d', kCtrl)) dup();
        if (g.key(PHX_KEY_DELETE) && nr) {
            edit([&] { doc.set_cell_text(size_t(row_), size_t(col_), doc.field_is_str(size_t(col_)) ? "" : "0"); });
        }
        if (g.key('+') || g.key('=')) step(1);
        if (g.key('-')) step(-1);
        if (g.key('+', kShift) || g.key('=', kShift)) step(10);
        if (g.key('_', kShift) || g.key('-', kShift)) step(-10);
        // typing starts an edit that replaces the cell
        if (!g.in->text.empty() && nr) {
            const std::string t = g.in->text;
            g.in->text.clear();
            if (!(t == "+" || t == "-" || t == "=" || t == "_")) begin_edit(g, t, false);
        }
    }

    void draw_bar(Host& h, const Rect& r) {
        Gui& g = h.gui();
        g.rect(r, g.th.panel, kSubFill);
        Gui::Row row(Rect{ r.x + 4, r.y + 3, r.w - 8, 12 }, 3);
        g.text(row.take(36).x, r.y + 5, "struct", g.th.dim);
        std::string sn = doc.struct_name;
        if (g.text_field(g.id("sname"), row.take(110), sn, "Name", 0, "The C struct phxbin generates (a C identifier)")) {
            if (!BinDoc::valid_ident(sn)) h.toast("struct name must be a C identifier", Toast::Warn);
            else edit([&] { doc.set_struct_name(sn); });
        }
        row.take(6);
        const bool have = !doc.records.empty();
        auto btn = [&](const char* label, int icon, const char* help, bool en = true) {
            Btn b; b.icon = icon; b.help = help; b.enabled = en;
            return g.button(row.take(Gui::text_w(label) + (icon ? 16 : 10)), label, b);
        };
        if (btn("record", kIconPlus, "Add a record at the end (a copy of the cursor row)")) { edit([&] { doc.add_record(size_t(row_)); }); row_ = int(doc.records.size()) - 1; follow_ = true; }
        if (btn("insert", 0, "Insert a zeroed record above the cursor (Ctrl+Enter)")) { edit([&] { doc.insert_record(size_t(row_), false); }); follow_ = true; }
        if (btn("dup", kIconCopy, "Duplicate the cursor record (Ctrl+D)", have)) dup();
        if (btn("", kIconTrash, "Delete the cursor record", have)) del_row();
        if (g.icon_button(row.take(12), kIconUp, "Move the record up", false, have && row_ > 0)) { edit([&] { doc.move_record(size_t(row_), -1); }); --row_; }
        if (g.icon_button(row.take(12), kIconDown, "Move the record down", false, have && row_ + 1 < int(doc.records.size()))) { edit([&] { doc.move_record(size_t(row_), 1); }); ++row_; }
        row.take(6);
        if (btn("field", kIconPlus, "Add a column (name + type)")) add_field_dialog(h);
    }

    void draw_grid(Host& h, const Rect& area) {
        Gui& g = h.gui();
        const int nr = int(doc.records.size()), nf = int(doc.fields.size());
        const int rn_w = std::max(3, int(std::to_string(nr).size()) + 1) * kAdv + 8;
        const int head_h = 22, row_h = 12;
        Rect r = area;
        cut_right(r, 6);
        cut_bottom(r, 6);
        std::vector<int> cw(static_cast<size_t>(std::max(0, nf))), cx(static_cast<size_t>(std::max(0, nf)));
        int total = 0;
        for (int f = 0; f < nf; ++f) { cw[size_t(f)] = col_width(size_t(f)); cx[size_t(f)] = total; total += cw[size_t(f)]; }
        const Rect cells{ r.x + rn_w, r.y + head_h, r.w - rn_w, r.h - head_h };
        const int vis = std::max(1, cells.h / row_h);

        // follow the cursor
        if (follow_) {
            if (row_ < top_) top_ = row_;
            if (row_ >= top_ + vis) top_ = row_ - vis + 1;
            if (nf) {
                if (cx[size_t(col_)] < left_) left_ = cx[size_t(col_)];
                if (cx[size_t(col_)] + cw[size_t(col_)] > left_ + cells.w) left_ = cx[size_t(col_)] + cw[size_t(col_)] - cells.w;
            }
            follow_ = false;
        }
        g.wheel_scroll(cells, top_, nr, vis, 3);
        if (const int w = g.wheel_x(cells)) left_ -= w * 24;
        left_ = std::max(0, std::min(left_, std::max(0, total - cells.w)));
        top_ = clamp_scroll(top_, nr, vis);

        g.rect(r, rgba(26, 26, 38), kSubFill);
        // header
        g.push_clip(Rect{ cells.x, r.y, cells.w, head_h });
        const size_t name_col = doc.name_field();
        for (int f = 0; f < nf; ++f) {
            const Rect hr{ cells.x + cx[size_t(f)] - left_, r.y, cw[size_t(f)], head_h };
            const bool sel = f == col_;
            g.rect(hr, sel ? g.th.panel2 : g.th.panel, kSubWidget);
            g.vline(hr.right() - 1, hr.y, hr.h, g.th.line, kSubImage);
            g.text(hr.x + 4, hr.y + 3, doc.fields[size_t(f)].name, sel ? g.th.accent : g.th.text, kSubText, hr.w - 6);
            g.text(hr.x + 4, hr.y + 12, doc.fields[size_t(f)].type + (size_t(f) == name_col ? " name" : ""),
                   size_t(f) == name_col ? g.th.good : g.th.faint, kSubText, hr.w - 6);
            if (g.hover(hr)) g.hint = doc.fields[size_t(f)].name + ": " + type_help(doc.fields[size_t(f)].type) + "  (right-click: rename, type, move, delete)";
            if (g.clicked(hr)) { col_ = f; }
            if (g.right_clicked(hr)) { col_ = f; ctx_col_ = f; g.open_context(g.id("col-ctx")); }
        }
        g.pop_clip();
        g.rect(Rect{ r.x, r.y, rn_w, head_h }, g.th.panel, kSubWidget);
        g.text(r.x + 3, r.y + 7, "#", g.th.faint);
        g.hline(r.x, r.y + head_h - 1, r.w, g.th.line, kSubImage);

        // rows
        const uint32_t cell_id = g.id("cell");
        for (int i = 0; i < vis + 1 && top_ + i < nr; ++i) {
            const int rec = top_ + i;
            const int y = cells.y + i * row_h;
            const Rect num{ r.x, y, rn_w, row_h };
            g.rect(num, rec == row_ ? g.th.panel2 : g.th.panel, kSubWidget);
            g.text(num.x + 3, y + 2, std::to_string(rec), rec == row_ ? g.th.accent : g.th.faint);
            if (g.clicked(num)) { row_ = rec; editing_ = false; }
            if (g.right_clicked(num)) { row_ = rec; ctx_row_ = rec; g.open_context(g.id("row-ctx")); }
            g.push_clip(Rect{ cells.x, y, cells.w, row_h });
            for (int f = 0; f < nf; ++f) {
                const Rect cr{ cells.x + cx[size_t(f)] - left_, y, cw[size_t(f)], row_h };
                if (cr.right() < cells.x || cr.x > cells.right()) continue;
                const bool cur = rec == row_ && f == col_;
                if (cur) g.rect(cr, g.th.sel, kSubWidget);
                else if ((rec & 1)) g.rect(cr, rgba(30, 30, 44), kSubWidget);
                g.vline(cr.right() - 1, y, row_h, rgba(40, 40, 56), kSubImage);
                if (cur && editing_) continue;                       // the edit field draws here
                const std::string t = doc.cell_text(size_t(rec), size_t(f));
                const bool is_str = doc.field_is_str(size_t(f));
                const bool too_long = is_str && t.size() > BinDoc::str_capacity(doc.fields[size_t(f)].type);
                const Rgba c = too_long ? g.th.bad : is_str ? (size_t(f) == name_col ? g.th.good : rgba(150, 214, 120)) : g.th.text;
                if (too_long && g.hover(cr)) g.hint = fmt("'%s' is %zu chars: a %s holds %zu (the bake cuts it)", t.c_str(), t.size(),
                                                           doc.fields[size_t(f)].type.c_str(), BinDoc::str_capacity(doc.fields[size_t(f)].type));
                if (is_str) g.text(cr.x + 4, y + 2, t, c, kSubText, cr.w - 6);
                else g.text(cr.right() - 4 - std::min(Gui::text_w(t), cr.w - 6), y + 2, t, c, kSubText, cr.w - 6);   // numbers right-aligned
                if (g.double_clicked(cr)) { row_ = rec; col_ = f; begin_edit(g, t, false); g.in->pressed &= ~kMouseL; }
                else if (g.clicked(cr)) { row_ = rec; col_ = f; editing_ = false; }
            }
            g.pop_clip();
        }
        if (nr == 0) g.text(cells.x + 6, cells.y + 6, "no records - press '+ record'", g.th.faint);

        // the in-cell editor
        if (editing_) {
            if (row_ < top_ || row_ >= top_ + vis || nf == 0) editing_ = false;
            else {
                const Rect cr{ cells.x + cx[size_t(col_)] - left_, cells.y + (row_ - top_) * row_h, cw[size_t(col_)], row_h };
                const bool tab = peek(g, PHX_KEY_TAB), stab = peek(g, PHX_KEY_TAB, kShift), enter = peek(g, PHX_KEY_ENTER), esc = peek(g, PHX_KEY_ESCAPE);
                g.push_clip(cells);
                std::string v = edit_text_;
                const bool committed = g.text_field(cell_id, cr, v, nullptr, 0);
                g.pop_clip();
                if (committed || (g.focus() != cell_id && !esc)) {
                    if (v != doc.cell_text(size_t(row_), size_t(col_))) {
                        doc.push_undo();
                        if (!doc.set_cell_text(size_t(row_), size_t(col_), v)) { doc.drop_undo(); h.toast("'" + v + "' is not a number", Toast::Warn); }
                        else if (doc.cell_text(size_t(row_), size_t(col_)) != v && !doc.field_is_flt(size_t(col_)))
                            h.toast(fmt("clamped to %s (%s)", doc.cell_text(size_t(row_), size_t(col_)).c_str(), type_help(doc.fields[size_t(col_)].type).c_str()), Toast::Info);
                    }
                }
                if (g.focus() != cell_id) {
                    editing_ = false;
                    if (tab) { if (col_ + 1 < nf) ++col_; follow_ = true; }
                    else if (stab) { if (col_ > 0) --col_; follow_ = true; }
                    else if (enter) { if (row_ + 1 < nr) ++row_; follow_ = true; }
                }
            }
        }

        g.scrollbar_v(g.id("vs"), Rect{ r.right(), cells.y, 6, cells.h }, nr, vis, top_);
        int lx = left_;
        if (g.scrollbar_h(g.id("hs"), Rect{ cells.x, r.bottom(), cells.w, 6 }, total, cells.w, lx)) left_ = lx;

        // context menus
        switch (g.menu(g.id("col-ctx"), { MenuItem{ "Rename field...", "", true, false, false, kIconFileCode },
                                          MenuItem{ "Change type...", "", true, false, false, kIconGear },
                                          MenuItem::sep(),
                                          MenuItem{ "Move left", "", ctx_col_ > 0, false, false, kIconLeft },
                                          MenuItem{ "Move right", "", ctx_col_ + 1 < nf, false, false, kIconRight },
                                          MenuItem::sep(),
                                          MenuItem{ "Add field...", "", true, false, false, kIconPlus },
                                          MenuItem{ "Delete field", "", nf > 1, false, false, kIconTrash } })) {
        case 0: rename_dialog(h, ctx_col_); break;
        case 1: type_dialog(h, ctx_col_); break;
        case 3: edit([&] { doc.move_field(size_t(ctx_col_), -1); }); --col_; break;
        case 4: edit([&] { doc.move_field(size_t(ctx_col_), 1); }); ++col_; break;
        case 6: add_field_dialog(h); break;
        case 7: edit([&] { doc.remove_field(size_t(ctx_col_)); }); clamp_cursor(); break;
        default: break;
        }
        switch (g.menu(g.id("row-ctx"), { MenuItem{ "Insert above", "Ctrl+Enter", true, false, false, kIconUp },
                                          MenuItem{ "Insert below", "", true, false, false, kIconDown },
                                          MenuItem{ "Duplicate", "Ctrl+D", true, false, false, kIconCopy },
                                          MenuItem::sep(),
                                          MenuItem{ "Move up", "", ctx_row_ > 0, false, false, kIconUp },
                                          MenuItem{ "Move down", "", ctx_row_ + 1 < nr, false, false, kIconDown },
                                          MenuItem::sep(),
                                          MenuItem{ "Delete record", "", true, false, false, kIconTrash } })) {
        case 0: edit([&] { doc.insert_record(size_t(ctx_row_), false); }); break;
        case 1: edit([&] { doc.insert_record(size_t(ctx_row_ + 1), false); }); ++row_; break;
        case 2: dup(); break;
        case 4: edit([&] { doc.move_record(size_t(ctx_row_), -1); }); --row_; break;
        case 5: edit([&] { doc.move_record(size_t(ctx_row_), 1); }); ++row_; break;
        case 7: del_row(); break;
        default: break;
        }
    }

    void draw_side(Host& h, const Rect& r0) {
        Gui& g = h.gui();
        Rect col = r0;
        g.rect(col, g.th.panel, kSubFill);
        g.vline(col.x, col.y, col.h, g.th.line);
        cut_left(col, 1);
        // record inspector
        const int nf = int(doc.fields.size());
        Rect b = panel_section(g, col, std::min(col.h - 70, 18 + nf * 16 + 4), doc.records.empty() ? "RECORD" : fmt("RECORD %d", row_));
        if (!doc.records.empty()) {
            int y = b.y;
            for (int f = 0; f < nf && y + 13 <= b.bottom(); ++f) {
                const BinDoc::Field& fl = doc.fields[size_t(f)];
                g.text(b.x, y + 3, fl.name, f == col_ ? g.th.accent : g.th.dim, kSubText, 60);
                const Rect fr{ b.x + 62, y, b.w - 62, 13 };
                const uint32_t fid = g.id("insp", f);
                if (doc.field_is_str(size_t(f))) {
                    std::string v = doc.str_cell(size_t(row_), size_t(f));
                    if (g.text_field(fid, fr, v, "", 0, type_help(fl.type).c_str())) edit([&] { doc.set_str(size_t(row_), size_t(f), v); });
                } else if (doc.field_is_flt(size_t(f))) {
                    double v = doc.flt_cell(size_t(row_), size_t(f));
                    if (g.float_field(fid, fr, v, -3.4e38, 3.4e38, 0.1, "f32 (wheel steps 0.1)")) edit([&] { doc.set_cell_text(size_t(row_), size_t(f), BinDoc::fmt_float(v)); });
                } else {
                    int64_t lo, hi;
                    BinDoc::type_range(fl.type, lo, hi);
                    int64_t v = doc.records[size_t(row_)][size_t(f)];
                    if (g.int_field(fid, fr, v, lo, hi, 1, (type_help(fl.type) + " - drag, wheel or type").c_str()))
                        edit([&] { doc.set_cell_text(size_t(row_), size_t(f), std::to_string(v)); });
                }
                if (g.clicked(Rect{ b.x, y, 60, 13 })) col_ = f;
                y += 16;
            }
        }
        // prefab / validation
        Rect pb = panel_section(g, col, std::max(40, col.h), "TABLE");
        int y = pb.y;
        const std::vector<std::string> names = doc.name_column();
        const size_t nc = doc.name_field();
        if (nc < doc.fields.size()) {
            g.text(pb.x, y, fmt("prefab table: %zu types", names.size()), g.th.good);
            y += 10;
            g.text(pb.x, y, "(the '" + doc.fields[nc].name + "' column names them)", g.th.faint, kSubText, pb.w);
            y += 12;
            std::string list;
            for (const std::string& n : names) list += (list.empty() ? "" : ", ") + n;
            for (const std::string& line : wrap_list(list, size_t(std::max(8, pb.w / kAdv)))) {
                if (y + 9 > pb.bottom() - 24) break;
                g.text(pb.x, y, line, g.th.text);
                y += 9;
            }
            y += 3;
            const std::vector<std::string> dup_names = doc.duplicate_names();
            if (!dup_names.empty()) { g.text(pb.x, y, "duplicate names: " + dup_names[0], g.th.bad, kSubText, pb.w); y += 11; }
        } else {
            g.text(pb.x, y, "a plain data table", g.th.dim);
            y += 10;
            g.text(pb.x, y, "add a str 'type' field to make", g.th.faint, kSubText, pb.w);
            g.text(pb.x, y + 9, "it the map editor's prefabs", g.th.faint, kSubText, pb.w);
            y += 22;
        }
        {
            int over = 0;
            for (size_t r = 0; r < doc.records.size(); ++r)
                for (size_t f = 0; f < doc.fields.size(); ++f)
                    if (doc.field_is_str(f) && doc.str_cell(r, f).size() > BinDoc::str_capacity(doc.fields[f].type)) ++over;
            if (over) { g.text(pb.x, y, fmt("%d text value%s too long for its field", over, over == 1 ? "" : "s"), g.th.bad, kSubText, pb.w); y += 11; }
        }
        size_t bytes = 0;
        for (const BinDoc::Field& f : doc.fields) {
            const std::string& t = f.type;
            bytes += BinDoc::str_type(t) ? BinDoc::str_capacity(t) + 1 : (t == "u8" || t == "i8") ? 1 : (t == "u16" || t == "i16") ? 2 : 4;
        }
        g.text(pb.x, pb.bottom() - 9, fmt("~%zu B/record, %zu B baked", bytes, bytes * doc.records.size()), g.th.faint, kSubText, pb.w);
    }

    static std::vector<std::string> wrap_list(const std::string& s, size_t cols) {
        std::vector<std::string> out;
        std::string cur;
        size_t p = 0;
        while (p < s.size()) {
            size_t e = s.find(", ", p);
            const std::string w = s.substr(p, e == std::string::npos ? std::string::npos : e - p + 1);
            if (!cur.empty() && cur.size() + w.size() + 1 > cols) { out.push_back(cur); cur.clear(); }
            cur += (cur.empty() ? "" : " ") + w;
            if (e == std::string::npos) break;
            p = e + 2;
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    }

    // ---- dialogs ----
    void add_field_dialog(Host& h) {
        struct St { std::string name; int type = 0; std::string err; bool first = true; };
        auto st = std::make_shared<St>();
        st->name = doc.fresh_field_name("field");
        h.modal("Add field", 280, 100, [this, st](Gui& g, Rect body) {
            const uint32_t nid = g.id("af-name");
            if (st->first) { g.set_focus(nid, true); g.edit().set(st->name, true); st->first = false; }
            g.text(body.x, body.y + 3, "name", g.th.dim);
            bool enter = false;
            if (g.focus() == nid && g.key(PHX_KEY_ENTER)) { st->name = g.edit().buf; enter = true; }   // Enter = Add
            g.text_field(nid, Rect{ body.x + 40, body.y, body.w - 40, 13 }, st->name, "C identifier");
            g.text(body.x, body.y + 20, "type", g.th.dim);
            const std::vector<std::string>& types = BinDoc::all_types();
            g.dropdown(g.id("af-type"), Rect{ body.x + 40, body.y + 17, 80, 13 }, types, st->type);
            g.text(body.x + 126, body.y + 20, type_help(types[size_t(st->type)]), g.th.faint, kSubText, body.w - 126);
            if (!st->err.empty()) g.text(body.x, body.y + 36, st->err, g.th.bad, kSubText, body.w);
            const int y = body.bottom() - 14;
            if (g.button(Rect{ body.x, y, 70, 14 }, "Add", Btn{ true }) || enter || (g.focus() == 0 && g.key(PHX_KEY_ENTER))) {
                if (!BinDoc::valid_ident(st->name)) { st->err = "the name must be a C identifier"; return true; }
                doc.push_undo();
                if (!doc.add_field(st->name, types[size_t(st->type)])) { doc.drop_undo(); st->err = "a field with that name exists"; return true; }
                col_ = int(doc.fields.size()) - 1;
                follow_ = true;
                return false;
            }
            if (g.button(Rect{ body.x + 76, y, 70, 14 }, "Cancel") || g.key(PHX_KEY_ESCAPE)) return false;
            return true;
        });
    }
    void rename_dialog(Host& h, int f) {
        if (f < 0 || f >= int(doc.fields.size())) return;
        struct St { std::string name; std::string err; bool first = true; };
        auto st = std::make_shared<St>();
        st->name = doc.fields[size_t(f)].name;
        h.modal("Rename field", 260, 76, [this, st, f](Gui& g, Rect body) {
            const uint32_t nid = g.id("rn");
            if (st->first) { g.set_focus(nid, true); g.edit().set(st->name, true); st->first = false; }
            bool go = false;
            if (g.focus() == nid && g.key(PHX_KEY_ENTER)) { st->name = g.edit().buf; go = true; }   // Enter = Rename
            go |= g.text_field(nid, Rect{ body.x, body.y, body.w, 13 }, st->name, "C identifier");
            if (!st->err.empty()) g.text(body.x, body.y + 17, st->err, g.th.bad, kSubText, body.w);
            const int y = body.bottom() - 14;
            go |= g.button(Rect{ body.x, y, 70, 14 }, "Rename", Btn{ true });
            if (go) {
                doc.push_undo();
                if (!doc.rename_field(size_t(f), st->name)) { doc.drop_undo(); st->err = "not a C identifier, or already used"; g.set_focus(g.id("rn"), true); return true; }
                return false;
            }
            if (g.button(Rect{ body.x + 76, y, 70, 14 }, "Cancel") || g.key(PHX_KEY_ESCAPE)) return false;
            return true;
        });
    }
    void type_dialog(Host& h, int f) {
        if (f < 0 || f >= int(doc.fields.size())) return;
        struct St { int type = 0; };
        auto st = std::make_shared<St>();
        const std::vector<std::string>& types = BinDoc::all_types();
        for (size_t i = 0; i < types.size(); ++i) if (types[i] == doc.fields[size_t(f)].type) st->type = int(i);
        h.modal("Change type of " + doc.fields[size_t(f)].name, 280, 90, [this, st, f](Gui& g, Rect body) {
            const std::vector<std::string>& ty = BinDoc::all_types();
            g.dropdown(g.id("ct"), Rect{ body.x, body.y, 90, 13 }, ty, st->type);
            g.text(body.x + 96, body.y + 3, type_help(ty[size_t(st->type)]), g.th.faint, kSubText, body.w - 96);
            g.text(body.x, body.y + 20, "values convert (numbers clamp, text parses)", g.th.dim, kSubText, body.w);
            const int y = body.bottom() - 14;
            if (g.button(Rect{ body.x, y, 70, 14 }, "Change", Btn{ true })) {
                doc.push_undo();
                doc.set_field_type(size_t(f), ty[size_t(st->type)]);
                return false;
            }
            if (g.button(Rect{ body.x + 76, y, 70, 14 }, "Cancel") || g.key(PHX_KEY_ESCAPE)) return false;
            return true;
        });
    }
};

} // namespace

std::unique_ptr<DocView> make_table_view(Host& h, const std::string& path, std::string* err) {
    (void)h;
    std::unique_ptr<TableView> v(new TableView);
    if (!v->load(path, err)) return nullptr;
    return v;
}

std::unique_ptr<DocView> make_table_view_new(Host& h, const std::string& path, const phxtool::BinDoc& doc) {
    (void)h;
    std::unique_ptr<TableView> v(new TableView);
    v->path = path;
    v->doc = doc;
    v->doc.dirty = true;
    return v;
}

} // namespace phxstudio
