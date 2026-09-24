// tools/phxstudio/ed_code.cpp — the CODE editor panel: a syntax-highlighted text editor for the
// engine's C++, the Makefile, docs, JSON tables and sprite defs, drawn glyph by glyph through the
// engine's own renderer. The document logic (TextDoc: edits, undo, find) and the lexers (syntax.h)
// are headless and unit-tested; this file lays them out and maps keys to commands.
//
//   typing / Enter / Backspace / Delete     auto-indent; '}' on a blank line dedents
//   arrows, Home/End, PgUp/PgDn (+Shift)    move / extend the selection (Ctrl = by word / doc)
//   Ctrl+A C X V Z Y (Ctrl+Shift+Z)          select all, clipboard, undo, redo
//   Tab / Shift+Tab                          indent / outdent (the selection's lines)
//   Ctrl+/  Ctrl+D  Ctrl+Shift+K  Alt+Up/Dn  comment, duplicate, delete line, move lines
//   Ctrl+F  Ctrl+H  F3 / Shift+F3  Ctrl+G    find, replace, next/previous match, go to line
//   mouse: click, drag, Shift+click, double-click (word), triple-click (line), gutter (line)
#include "host.h"
#include "textdoc.h"
#include "syntax.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace phxstudio {
namespace {

using namespace twk;

Rgba tok_colour(const Theme& th, Tok t) {
    switch (t) {
    case Tok::Keyword:  return rgba(236, 132, 104);
    case Tok::Type:     return rgba(104, 190, 236);
    case Tok::Number:   return rgba(214, 160, 240);
    case Tok::String:   return rgba(150, 214, 120);
    case Tok::Char:     return rgba(150, 214, 120);
    case Tok::Comment:  return rgba(112, 118, 150);
    case Tok::Doc:      return rgba(132, 150, 176);
    case Tok::Preproc:  return rgba(232, 188, 96);
    case Tok::Punct:    return rgba(170, 172, 196);
    case Tok::Func:     return rgba(248, 214, 132);
    case Tok::Constant: return rgba(240, 150, 190);
    case Tok::Key:      return rgba(104, 190, 236);
    case Tok::Heading:  return th.accent;
    case Tok::Emphasis: return rgba(248, 214, 132);
    case Tok::Variable: return rgba(240, 150, 190);
    case Tok::Error:    return th.bad;
    default:            return th.text;
    }
}

// An ASCII stand-in for the code point starting at s[i] (the font is ASCII): the punctuation the
// repo's comments use maps to its look-alike; anything else shows as the "unknown" box.
char utf8_glyph(const std::string& s, int i) {
    const unsigned char c0 = static_cast<unsigned char>(s[size_t(i)]);
    uint32_t cp = 0;
    int n = 0;
    if ((c0 & 0xE0) == 0xC0) { cp = c0 & 0x1F; n = 1; }
    else if ((c0 & 0xF0) == 0xE0) { cp = c0 & 0x0F; n = 2; }
    else if ((c0 & 0xF8) == 0xF0) { cp = c0 & 0x07; n = 3; }
    for (int k = 1; k <= n && i + k < int(s.size()); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[size_t(i + k)]) & 0x3F);
    switch (cp) {
    case 0x2014: case 0x2013: case 0x2212: case 0x2500: return '-';     // dashes, minus, box line
    case 0x2018: case 0x2019: case 0x2032: return '\'';
    case 0x201C: case 0x201D: return '"';
    case 0x2192: case 0x21D2: case 0x25B8: case 0x25B6: return '>';     // arrows, triangles
    case 0x2190: return '<';
    case 0x2191: case 0x25B2: return '^';
    case 0x2193: case 0x25BC: return 'v';
    case 0x00D7: return 'x';                                              // times
    case 0x2026: case 0x00B7: case 0x2022: return '.';                  // ellipsis, middots, bullet
    case 0x2264: return '<';
    case 0x2265: return '>';
    case 0x2248: return '~';
    case 0x00B0: return 'o';
    case 0x2502: case 0x2503: return '|';
    case 0x250C: case 0x2510: case 0x2514: case 0x2518: case 0x251C: case 0x2524: case 0x252C: case 0x2534: case 0x253C: return '+';
    case 0x00B5: return 'u';
    default: return char(127);                                            // the hollow box
    }
}

class CodeView final : public DocView {
public:
    TextDoc doc;
    Lang lang = Lang::Plain;
    // Read-only: the engine's public API headers / docs while a project is open. Navigation,
    // selection, copy and find work; every edit, paste, undo and save is refused.
    bool read_only = false;

    CodeView() : id_(0) {}

    bool load(const std::string& p, std::string* err) {
        path = p;
        if (!doc.load_file(p, err)) return false;
        lang = lang_for_path(p);
        states_.clear();
        disk_stamp = file_stamp(p);
        return true;
    }

    FileKind kind() const override { return FileKind::Code; }
    int icon() const override { return read_only ? kIconLock : kIconFileCode; }
    bool dirty() const override { return !read_only && doc.dirty; }
    bool save(Host& h, std::string* err) override {
        if (read_only) { if (err) *err = base_name(path) + " is read-only"; return false; }
        if (!doc.save_file(path, err)) return false;
        disk_stamp = file_stamp(path);
        h.file_saved(path);
        return true;
    }
    bool reload(Host&, std::string* err) override {
        const TextPos c = doc.caret;
        const int top = top_;
        if (!doc.load_file(path, err)) return false;
        doc.caret = doc.anchor = doc.clamp(c);
        top_ = top;
        states_.clear();
        disk_stamp = file_stamp(path);
        return true;
    }
    bool undo() override { if (read_only) return false; const bool r = doc.undo(); follow_ = true; return r; }
    bool redo() override { if (read_only) return false; const bool r = doc.redo(); follow_ = true; return r; }
    void goto_line(int line, int col) override {
        doc.move_to(TextPos{ std::max(0, line - 1), std::max(0, col - 1) }, false);
        follow_ = true;
        center_ = true;
    }
    std::string status() const override {
        const int col = doc.display_col(doc.caret.line, doc.caret.col) + 1;
        std::string s = fmt("%sLn %d, Col %d", read_only ? "READ-ONLY  " : "", doc.caret.line + 1, col);
        if (doc.has_sel()) {
            const std::string t = doc.selected_text();
            int nl = 0;
            for (char c : t) nl += c == '\n';
            s += fmt(" (%zu sel%s)", t.size(), nl ? fmt(", %d lines", nl + 1).c_str() : "");
        }
        s += fmt("   %s   %s   %s", lang_name(lang), doc.crlf ? "CRLF" : "LF",
                 doc.indent_unit() == "\t" ? "Tabs" : "Spaces:4");
        return s;
    }
    std::vector<Action> actions() override {
        std::vector<Action> a;
        if (read_only) {
            a.push_back({ "Find", "Ctrl+F", [this] { open_find(false); } });
            a.push_back({ "Go to line", "Ctrl+G", [this] { want_goto_ = true; } });
            a.push_back({ "Select all", "Ctrl+A", [this] { doc.select_all(); } });
            return a;
        }
        a.push_back({ "Find", "Ctrl+F", [this] { open_find(false); } });
        a.push_back({ "Replace", "Ctrl+H", [this] { open_find(true); } });
        a.push_back({ "Go to line", "Ctrl+G", [this] { want_goto_ = true; } });
        a.push_back({ "Toggle comment", "Ctrl+/", [this] { doc.toggle_comment(comment_prefix(lang)); follow_ = true; }, comment_prefix(lang)[0] != 0 });
        a.push_back({ "Duplicate lines", "Ctrl+D", [this] { doc.duplicate_lines(); follow_ = true; } });
        a.push_back({ "Delete lines", "Ctrl+Shift+K", [this] { doc.delete_lines(); follow_ = true; } });
        a.push_back({ "Select all", "Ctrl+A", [this] { doc.select_all(); } });
        return a;
    }

    void draw(Host& h, const Rect& area) override {
        Gui& g = h.gui();
        if (!id_) id_ = Gui::hash(path.c_str());
        g.push_id(id_);
        Rect r = area;
        g.rect(r, rgba(24, 24, 34), kSubBg);

        if (want_goto_) { want_goto_ = false; goto_dialog(h); }
        if (read_only) {
            const Rect banner = cut_top(r, 14);
            g.rect(banner, rgba(54, 44, 30), kSubFill);
            g.icon(banner.x + 4, banner.y + 2, kIconLock, g.th.warn);
            g.text(banner.x + 18, banner.y + 4, "Read-only engine reference - a project edits only its own files",
                   g.th.warn, kSubText, banner.w - 22);
        }
        if (find_open_) draw_find_bar(h, cut_top(r, replace_open_ ? 30 : 16));

        // geometry
        const int lines = doc.line_count();
        int digits = 1;
        for (int n = lines; n >= 10; n /= 10) ++digits;
        const int gutter = std::max(3, digits) * kAdv + 14;
        cut_right(r, 7);                               // room for the scrollbars
        cut_bottom(r, 7);
        const Rect text_r = Rect{ r.x + gutter, r.y, r.w - gutter, r.h };
        const Rect gut_r = Rect{ r.x, r.y, gutter, r.h };
        const int vis_lines = std::max(1, text_r.h / kLineH);
        const int vis_cols = std::max(1, (text_r.w - 4) / kAdv);

        // keyboard (only while this editor owns the keyboard)
        const uint32_t fid = g.id("text");
        if (g.focus() == 0 && !h.modal_open() && !g.any_popup()) g.set_focus(fid, true);
        const bool focused = g.focus() == fid;
        if (focused) { g.keep_focus(fid, true); handle_keys(h, g, vis_lines); }

        // mouse in the text + gutter
        handle_mouse(g, text_r, gut_r, lines);
        if (const int w = g.wheel(Rect{ r.x, r.y, r.w + 7, r.h })) top_ -= w * 3;
        if (const int w = g.wheel_x(r)) left_ = std::max(0, left_ - w * 4);

        // keep the caret visible after keyboard motion / edits
        if (follow_) {
            if (center_) { top_ = doc.caret.line - vis_lines / 2; center_ = false; }
            if (doc.caret.line < top_) top_ = doc.caret.line;
            if (doc.caret.line >= top_ + vis_lines) top_ = doc.caret.line - vis_lines + 1;
            const int dc = doc.display_col(doc.caret.line, doc.caret.col);
            if (dc < left_) left_ = std::max(0, dc - 4);
            if (dc >= left_ + vis_cols) left_ = dc - vis_cols + 4;
            follow_ = false;
        }
        top_ = clamp_scroll(top_, lines + vis_lines / 2, vis_lines);

        rehighlight(std::min(lines, top_ + vis_lines + 1));

        // diagnostics for this file (from the last build's output)
        if (g.frame % 30 == 1) diags_ = h.diagnostics_for(path);

        // matching bracket
        int ml = -1, mc = -1, bl = -1, bc = -1;
        {
            const std::string& cl = doc.line(doc.caret.line);
            int c = doc.caret.col;
            if (c < int(cl.size()) && std::strchr("()[]{}", cl[size_t(c)])) { bl = doc.caret.line; bc = c; }
            else if (c > 0 && std::strchr("()[]{}", cl[size_t(c - 1)])) { bl = doc.caret.line; bc = c - 1; }
            if (bl >= 0 && !match_bracket(doc.lines, bl, bc, ml, mc)) bl = -1;
        }

        // ---- draw ----
        g.rect(gut_r, rgba(28, 28, 40), kSubFill);
        g.push_clip(r);
        const TextPos lo = doc.sel_lo(), hi = doc.sel_hi();
        std::vector<Span> spans;
        for (int i = 0; i < vis_lines + 1; ++i) {
            const int l = top_ + i;
            if (l >= lines) break;
            const int y = r.y + i * kLineH;
            const std::string& s = doc.line(l);
            const bool cur = l == doc.caret.line;
            // line number + diagnostic marker
            int sev = -1;
            std::string dmsg;
            for (const Diag& d : diags_) if (d.line == l + 1 && d.severity > sev) { sev = d.severity; dmsg = d.msg; }
            const Rgba nc = cur ? g.th.text : g.th.faint;
            const std::string num = std::to_string(l + 1);
            g.text(gut_r.right() - 8 - Gui::text_w(num), y + 1, num, nc);
            if (sev >= 1) {
                g.rect(Rect{ gut_r.x + 2, y + 2, 4, 6 }, sev == 2 ? g.th.bad : g.th.warn, kSubImage);
                if (g.hover(Rect{ r.x, y, r.w, kLineH })) g.hint = (sev == 2 ? "error: " : "warning: ") + dmsg;
            }
            if (cur && !doc.has_sel()) g.rect(Rect{ text_r.x, y, text_r.w, kLineH }, rgba(34, 34, 50), kSubFill);
            // selection
            if (doc.has_sel() && l >= lo.line && l <= hi.line) {
                const int a = l == lo.line ? doc.display_col(l, lo.col) : 0;
                const int b = l == hi.line ? doc.display_col(l, hi.col) : doc.display_col(l, int(s.size())) + 1;
                const int x0 = text_r.x + 2 + (a - left_) * kAdv, x1 = text_r.x + 2 + (b - left_) * kAdv;
                g.push_clip(text_r);
                g.rect(Rect{ x0, y, std::max(0, x1 - x0), kLineH }, g.th.sel, kSubWidget);
                g.pop_clip();
            }
            // find matches
            g.push_clip(text_r);
            if (find_open_ && !find_q_.empty())
                for (int c = 0; c + int(find_q_.size()) <= int(s.size()); ++c)
                    if (doc.match_at(l, c, find_q_, find_case_, find_word_)) {
                        const int a = doc.display_col(l, c), b = doc.display_col(l, c + int(find_q_.size()));
                        g.frame_rect(Rect{ text_r.x + 2 + (a - left_) * kAdv - 1, y, (b - a) * kAdv + 1, kLineH }, g.th.warn, kSubImage);
                        c += int(find_q_.size()) - 1;
                    }
            // text
            highlight_line(lang, s, states_[size_t(l)], spans);
            int d = 0;
            size_t si = 0;
            for (int c = 0; c < int(s.size()); ++c) {
                while (si < spans.size() && c >= spans[si].start + spans[si].len) ++si;
                const Tok t = si < spans.size() && c >= spans[si].start ? spans[si].tok : Tok::Text;
                char ch = s[size_t(c)];
                if (TextDoc::is_cont(ch)) continue;                          // zero-width
                if (static_cast<unsigned char>(ch) >= 0x80) ch = utf8_glyph(s, c);   // one glyph per code point
                const int nd = doc.advance(s, c, d);
                if (d >= left_ && d < left_ + vis_cols + 1 && ch != '\t' && ch != ' ')
                    g.glyph(text_r.x + 2 + (d - left_) * kAdv, y + 1, ch, tok_colour(g.th, t));
                if (d >= left_ + vis_cols + 1) break;
                d = nd;
            }
            // bracket pair
            if (bl >= 0) {
                if (l == bl) g.frame_rect(Rect{ text_r.x + 2 + (doc.display_col(l, bc) - left_) * kAdv - 1, y, kAdv + 1, kLineH }, g.th.dim, kSubImage);
                if (l == ml) g.frame_rect(Rect{ text_r.x + 2 + (doc.display_col(l, mc) - left_) * kAdv - 1, y, kAdv + 1, kLineH }, g.th.dim, kSubImage);
            }
            // diagnostic squiggle under the reported column (or the whole line)
            if (sev >= 1) {
                const Rgba sc = sev == 2 ? g.th.bad : g.th.warn;
                for (const Diag& dg : diags_) {
                    if (dg.line != l + 1 || dg.severity < 1) continue;
                    const int c0 = dg.col > 0 ? doc.display_col(l, dg.col - 1) : 0;
                    const int c1 = dg.col > 0 ? c0 + 3 : doc.display_col(l, int(s.size()));
                    for (int x = text_r.x + 2 + (c0 - left_) * kAdv; x < text_r.x + 2 + (c1 - left_) * kAdv; x += 2)
                        g.rect(Rect{ x, y + kLineH - 1 - ((x / 2) & 1), 2, 1 }, sc, kSubOver);
                }
            }
            // caret
            if (cur && focused && ((g.frame / 32) % 2 == 0 || blink_hold_ > 0)) {
                const int cx = text_r.x + 2 + (doc.display_col(l, doc.caret.col) - left_) * kAdv - 1;
                g.rect(Rect{ cx, y, 2, kLineH }, g.th.caret, kSubTop);
            }
            g.pop_clip();
        }
        if (blink_hold_ > 0) --blink_hold_;
        if (blocked_ > 0) { --blocked_; g.hint = "This file is read-only: the engine's API can be read, not changed"; }
        g.pop_clip();

        // scrollbars
        if (cache_w_ver_ != doc.version) { cache_w_ = doc.max_display_width(); cache_w_ver_ = doc.version; }
        const int maxw = cache_w_;
        g.scrollbar_v(g.id("vs"), Rect{ r.right(), r.y, 7, r.h }, lines + vis_lines / 2, vis_lines, top_);
        g.scrollbar_h(g.id("hs"), Rect{ r.x + gutter, r.bottom(), r.w - gutter, 7 }, maxw + 8, vis_cols, left_);
        g.rect(Rect{ r.right(), r.bottom(), 7, 7 }, g.th.bar, kSubWidget);
        if (g.hover(text_r)) g.cursor = PHX_CURSOR_IBEAM;
        g.pop_id();
    }

private:
    uint32_t id_;
    int top_ = 0, left_ = 0;
    bool follow_ = false, center_ = false;
    std::vector<int> states_;          // lexer state at the START of each line
    int blink_hold_ = 0;
    int blocked_ = 0;                  // frames left of the "read-only" hint after a refused edit
    int cache_w_ = 0;
    uint64_t cache_w_ver_ = ~0ull;
    std::vector<Diag> diags_;
    bool select_drag_ = false;
    bool find_open_ = false, replace_open_ = false, find_case_ = false, find_word_ = false;
    bool want_goto_ = false;
    std::string find_q_, repl_q_;
    bool focus_find_ = false;
    int drag_mode_ = 0;                // 0 char, 1 word, 2 line (double/triple click drags)
    TextPos drag_origin_lo_, drag_origin_hi_;

    // Re-lex line-start states from the first edited line up to `upto`. states_[0..valid_) are
    // known-good; an edit at line L invalidates everything after it.
    void rehighlight(int upto) {
        const int n = doc.line_count();
        valid_ = std::min(valid_, doc.first_changed_line + 1);
        doc.reset_changed_mark();
        states_.resize(size_t(n), 0);
        valid_ = std::max(1, std::min(valid_, n));
        states_[0] = 0;
        std::vector<Span> tmp;
        const int end = std::min(n, upto + 1);
        for (int l = valid_; l < end; ++l)
            states_[size_t(l)] = highlight_line(lang, doc.line(l - 1), states_[size_t(l - 1)], tmp);
        valid_ = std::max(valid_, end);
    }
    int valid_ = 1;

    TextPos pos_at(const Rect& text_r, int mx, int my) const {
        const int l = std::max(0, std::min(doc.line_count() - 1, top_ + (my - text_r.y) / kLineH));
        const int dcol = std::max(0, (mx - text_r.x - 2 + kAdv / 2) / kAdv + left_);
        return TextPos{ l, doc.col_from_display(l, dcol) };
    }

    void handle_mouse(Gui& g, const Rect& text_r, const Rect& gut_r, int lines) {
        const uint32_t tid = g.id("text");
        const uint32_t mid = g.id("mouse");
        if (g.drag(mid, text_r)) {
            const TextPos p = pos_at(text_r, g.mx(), g.my());
            if (g.just_pressed(mid)) {
                g.set_focus(tid, true);
                if (g.in->clicks >= 3) { doc.select_line(p.line); drag_mode_ = 2; }
                else if (g.in->clicks == 2) { doc.select_word_at(p); drag_mode_ = 1; }
                else { doc.move_to(p, (g.in->mods & kShift) != 0); drag_mode_ = 0; }
                drag_origin_lo_ = doc.sel_lo(); drag_origin_hi_ = doc.sel_hi();
                blink_hold_ = 30;
            } else {
                // extend by the unit the drag started with, and autoscroll at the edges
                if (g.my() < text_r.y) top_ = std::max(0, top_ - 1);
                if (g.my() >= text_r.bottom()) top_ = std::min(lines - 1, top_ + 1);
                if (drag_mode_ == 0) doc.move_to(p, true);
                else {
                    // extend by whole words / lines from the double/triple-clicked origin
                    TextPos a = p, b = p;
                    if (drag_mode_ == 2) {
                        a = TextPos{ p.line, 0 };
                        b = p.line + 1 < doc.line_count() ? TextPos{ p.line + 1, 0 } : TextPos{ p.line, int(doc.line(p.line).size()) };
                    } else {
                        const std::string& s = doc.line(p.line);
                        int x = p.col, y = p.col;
                        while (x > 0 && TextDoc::word_char(s[size_t(x - 1)])) --x;
                        while (y < int(s.size()) && TextDoc::word_char(s[size_t(y)])) ++y;
                        a = TextPos{ p.line, x }; b = TextPos{ p.line, y };
                    }
                    if (a < drag_origin_lo_) { doc.anchor = drag_origin_hi_; doc.caret = a; }
                    else { doc.anchor = drag_origin_lo_; doc.caret = b; }
                }
            }
        }
        const uint32_t gid = g.id("gutter");
        if (g.drag(gid, gut_r)) {
            const int l = std::max(0, std::min(lines - 1, top_ + (g.my() - gut_r.y) / kLineH));
            g.set_focus(tid, true);
            if (g.just_pressed(gid)) { doc.select_line(l); drag_origin_lo_ = doc.sel_lo(); }
            else {
                const TextPos a{ l, 0 };
                const TextPos b = l + 1 < doc.line_count() ? TextPos{ l + 1, 0 } : TextPos{ l, int(doc.line(l).size()) };
                if (a < drag_origin_lo_) { doc.anchor = TextPos{ drag_origin_lo_.line + 1 < doc.line_count() ? drag_origin_lo_.line + 1 : drag_origin_lo_.line, 0 }; doc.caret = a; }
                else { doc.anchor = drag_origin_lo_; doc.caret = b; }
            }
        }
    }

    // Keys a read-only view still honours (motion, selection, copy, find, go to line).
    static bool read_only_key(int32_t k, bool ctl) {
        switch (k) {
        case PHX_KEY_LEFT: case PHX_KEY_RIGHT: case PHX_KEY_UP: case PHX_KEY_DOWN: case PHX_KEY_HOME:
        case PHX_KEY_END: case PHX_KEY_PAGE_UP: case PHX_KEY_PAGE_DOWN: case PHX_KEY_ESCAPE: case PHX_KEY_F3:
            return true;
        default:
            return ctl && (k == 'a' || k == 'c' || k == 'f' || k == 'g' || k == 'l');
        }
    }

    void handle_keys(Host& h, Gui& g, int vis_lines) {
        for (KeyEvent& e : g.in->keys) {
            if (e.used) continue;
            const uint16_t m = norm_mods(e.mods);
            const bool sh = (m & kShift) != 0, ctl = (m & kCtrl) != 0, alt = (m & kAlt) != 0;
            if (read_only && (!read_only_key(e.key, ctl) || alt)) {
                // swallow edits (and plain typing) so nothing else acts on them either
                if (e.key >= 32 && e.key < 127 && !ctl) e.used = true;
                else if (e.key == PHX_KEY_BACKSPACE || e.key == PHX_KEY_DELETE || (!ctl && (e.key == PHX_KEY_ENTER || e.key == PHX_KEY_TAB))) e.used = true;
                else if (ctl && (e.key == 'x' || e.key == 'v' || e.key == 'z' || e.key == 'y' || e.key == 'd' || e.key == 'k' || e.key == '/' || e.key == 'h' || e.key == '[' || e.key == ']')) e.used = true;
                if (e.used && e.key != PHX_KEY_TAB) blocked_ = 45;
                continue;
            }
            bool used = true, moved = true;
            switch (e.key) {
            case PHX_KEY_LEFT:  if (alt) { used = false; break; } doc.left(ctl, sh); break;
            case PHX_KEY_RIGHT: if (alt) { used = false; break; } doc.right(ctl, sh); break;
            case PHX_KEY_UP:
                if (alt) doc.move_lines(-1);
                else if (ctl) top_ = std::max(0, top_ - 1);
                else doc.vertical(-1, sh);
                break;
            case PHX_KEY_DOWN:
                if (alt) doc.move_lines(1);
                else if (ctl) ++top_;
                else doc.vertical(1, sh);
                break;
            case PHX_KEY_HOME: if (ctl) doc.move_to(TextPos{}, sh); else doc.home(sh); break;
            case PHX_KEY_END:  if (ctl) doc.move_to(doc.end_pos(), sh); else doc.end(sh); break;
            case PHX_KEY_PAGE_UP:   doc.vertical(-(vis_lines - 1), sh); top_ -= vis_lines - 1; break;
            case PHX_KEY_PAGE_DOWN: doc.vertical(vis_lines - 1, sh); top_ += vis_lines - 1; break;
            case PHX_KEY_BACKSPACE: doc.backspace(ctl); break;
            case PHX_KEY_DELETE:    if (sh && !doc.has_sel()) doc.delete_lines(); else doc.del(ctl); break;
            case PHX_KEY_ENTER:     if (ctl) { used = false; moved = false; break; } doc.newline(); break;
            case PHX_KEY_TAB:       if (ctl) { used = false; moved = false; break; } doc.tab(sh); break;
            case PHX_KEY_ESCAPE:
                if (find_open_) { find_open_ = false; replace_open_ = false; }
                else if (doc.has_sel()) doc.move_to(doc.caret, false);
                else { used = false; }
                moved = false;
                break;
            case PHX_KEY_F3: find_step(!sh); break;
            default:
                moved = false;
                if (!ctl) { used = e.key >= 32 && e.key < 127; break; }   // typed via TEXT events
                used = command_key(h, e.key, sh, alt);
                if (used) moved = true;
                break;
            }
            if (used) { e.used = true; if (moved) { follow_ = true; blink_hold_ = 30; } }
        }
        if (read_only && !g.in->text.empty()) { g.in->text.clear(); blocked_ = 45; }
        if (!g.in->text.empty()) {
            for (char c : g.in->text) {
                if (c == '}' || c == ']' || c == ')') {
                    // closing bracket typed on a blank line: dedent it one level first
                    const std::string& s = doc.line(doc.caret.line);
                    const bool blank = s.find_first_not_of(" \t") == std::string::npos && doc.caret.col == int(s.size()) && !s.empty();
                    if (blank && !doc.has_sel()) {
                        const std::string unit = doc.indent_unit();
                        const int n = int(std::min(s.size(), unit.size()));
                        doc.replace(TextPos{ doc.caret.line, int(s.size()) - n }, doc.caret, "");
                        doc.caret = doc.anchor = TextPos{ doc.caret.line, int(s.size()) - n };
                    }
                }
                doc.insert(std::string(1, c));
            }
            g.in->text.clear();
            follow_ = true;
            blink_hold_ = 30;
        }
    }

    // Ctrl+<key> commands. Returns true when handled.
    bool command_key(Host& h, int32_t k, bool sh, bool alt) {
        (void)alt;
        switch (k) {
        case 'a': doc.select_all(); return true;
        case 'c': case 'x': {
            std::string t;
            if (doc.has_sel()) t = doc.selected_text();
            else {                                  // no selection: the whole line (VS Code style)
                t = doc.line(doc.caret.line) + "\n";
                if (k == 'x') { phx_desktop_clipboard_set(t.c_str()); doc.delete_lines(); return true; }
            }
            phx_desktop_clipboard_set(t.c_str());
            if (k == 'x' && doc.has_sel()) { std::string tmp; doc.cut(tmp); }
            return true;
        }
        case 'v': {
            const std::string t = phx_desktop_clipboard_get();
            if (!t.empty()) doc.paste(t);
            return true;
        }
        case 'z': if (sh) doc.redo(); else doc.undo(); return true;
        case 'y': doc.redo(); return true;
        case 'd': doc.duplicate_lines(); return true;
        case 'k': if (sh) { doc.delete_lines(); return true; } return false;
        case '/': doc.toggle_comment(comment_prefix(lang)); return true;
        case 'f': open_find(false); return true;
        case 'h': open_find(true); return true;
        case 'g': goto_dialog(h); return true;
        case 'l': doc.select_line(doc.caret.line); return true;
        case ']': doc.tab(false); return true;
        case '[': doc.tab(true); return true;
        default: return false;
        }
    }

    void open_find(bool replace) {
        find_open_ = true;
        replace_open_ = replace && !read_only;
        const std::string w = doc.word_at_caret();
        if (!w.empty() && w.find('\n') == std::string::npos) find_q_ = w;
        focus_find_ = true;
    }
    void find_step(bool forward) {
        if (find_q_.empty()) return;
        doc.find(find_q_, find_case_, find_word_, !forward);
        follow_ = true;
        center_ = true;
    }

    void draw_find_bar(Host& h, const Rect& bar) {
        Gui& g = h.gui();
        g.rect(bar, g.th.panel, kSubFill);
        g.rect(Rect{ bar.x, bar.bottom() - 1, bar.w, 1 }, g.th.line, kSubWidget);
        Gui::Row row(Rect{ bar.x + 4, bar.y + 2, bar.w - 8, 12 }, 3);
        const uint32_t fq = g.id("findq");
        if (focus_find_) {
            // put the keyboard into the find field with its text selected
            g.set_focus(fq, true);
            g.edit().set(find_q_, true);
            focus_find_ = false;
        }
        g.icon(row.take(10).x, bar.y + 3, kIconSearch, g.th.dim);
        const Rect qf = row.take(std::min(180, bar.w / 3));
        const std::string before = find_q_;
        // Enter/Shift+Enter step through matches and KEEP the field focused (taken before the field
        // would treat Enter as "commit and leave")
        if (g.focus() == fq) {
            if (g.key(PHX_KEY_ENTER)) find_step(true);
            if (g.key(PHX_KEY_ENTER, kShift)) find_step(false);
        }
        g.text_field(fq, qf, find_q_, "find", kFieldLive, "Find text (Enter = next match, Esc = back to the text)");
        if (find_q_ != before && !find_q_.empty()) {         // live: jump to the first match from the caret
            doc.move_to(doc.sel_lo(), false);
            find_step(true);
        }
        if (g.button(row.take(18), "Aa", Btn{ find_case_, true, false, 0, "Match case" })) find_case_ = !find_case_;
        if (g.button(row.take(18), "W", Btn{ find_word_, true, false, 0, "Whole word" })) find_word_ = !find_word_;
        if (g.icon_button(row.take(14), kIconUp, "Previous match (Shift+F3)")) find_step(false);
        if (g.icon_button(row.take(14), kIconDown, "Next match (F3)")) find_step(true);
        const int n = doc.count(find_q_, find_case_, find_word_);
        g.text(row.take(80).x + 2, bar.y + 4, find_q_.empty() ? "" : n ? fmt("%d match%s", n, n == 1 ? "" : "es") : "no matches",
               n || find_q_.empty() ? g.th.dim : g.th.bad);
        if (g.icon_button(Rect{ bar.right() - 16, bar.y + 2, 14, 12 }, kIconClose, "Close (Esc)")) { find_open_ = false; replace_open_ = false; }
        if (replace_open_) {
            Gui::Row r2(Rect{ bar.x + 4, bar.y + 16, bar.w - 8, 12 }, 3);
            r2.take(10);
            g.text_field(g.id("replq"), r2.take(std::min(180, bar.w / 3)), repl_q_, "replace with", kFieldLive);
            if (g.button(r2.take(52), "replace", Btn{ false, !find_q_.empty(), false, 0, "Replace this match and find the next" })) {
                doc.replace_one(find_q_, repl_q_, find_case_, find_word_);
                follow_ = true;
            }
            if (g.button(r2.take(30), "all", Btn{ false, !find_q_.empty(), false, 0, "Replace every match (one undo step)" })) {
                const int k = doc.replace_all(find_q_, repl_q_, find_case_, find_word_);
                h.toast(fmt("replaced %d occurrence%s", k, k == 1 ? "" : "s"), k ? Toast::Good : Toast::Warn);
            }
        }
    }

    void goto_dialog(Host& h) {
        auto text = std::make_shared<std::string>();
        auto first = std::make_shared<bool>(true);
        h.modal("Go to line", 200, 60, [this, text, first](Gui& g, Rect body) {
            const uint32_t id = g.id("goto");
            if (*first) { g.set_focus(id, true); g.edit().set("", false); *first = false; }
            g.text(body.x, body.y + 2, fmt("line 1-%d[:col]", doc.line_count()), g.th.dim);
            bool done = false;
            if (g.text_field(id, Rect{ body.x, body.y + 14, body.w, 13 }, *text, "line")) done = true;
            if (g.focus() != id && !done) return false;           // Esc
            if (done) {
                int l = 0, c = 0;
                if (std::sscanf(text->c_str(), "%d:%d", &l, &c) >= 1) goto_line(l, c > 0 ? c : 1);
                return false;
            }
            return true;
        });
    }
};

} // namespace

std::unique_ptr<DocView> make_code_view(Host& h, const std::string& path, std::string* err, bool read_only) {
    (void)h;
    const std::string head = read_head(path, 8192);
    if (looks_binary(head)) { if (err) *err = base_name(path) + " looks binary - not opening it as text"; return nullptr; }
    std::unique_ptr<CodeView> v(new CodeView);
    if (!v->load(path, err)) return nullptr;
    v->read_only = read_only;
    return v;
}
} // namespace phxstudio
