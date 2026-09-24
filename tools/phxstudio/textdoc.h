// tools/phxstudio/textdoc.h — the code editor's DOCUMENT model: a line buffer with a caret +
// selection, every edit routed through one primitive (replace a range with text) so undo/redo is
// exact, typing coalesced into word-sized undo steps, auto-indent, block (un)indent, line
// comments, line duplicate/move/delete, and find / replace. Headless and unit-tested in the
// editors suite; the Studio's Code view only draws it and feeds it keys. Host-only (STL fine).
//
// Positions are (line, col) in BYTES. Display columns expand tabs to the tab width and count a
// UTF-8 sequence as ONE column (its continuation bytes are zero-width, and the caret never lands
// inside a sequence), so the repo's em dashes and arrows edit like any other character. Line endings are normalised to '\n' in memory
// and restored on save (a CRLF file stays CRLF), as is the presence of a final newline.
#ifndef PHX_TOOLS_PHXSTUDIO_TEXTDOC_H
#define PHX_TOOLS_PHXSTUDIO_TEXTDOC_H

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace phxstudio {

struct TextPos {
    int line = 0, col = 0;
    bool operator==(const TextPos& o) const { return line == o.line && col == o.col; }
    bool operator!=(const TextPos& o) const { return !(*this == o); }
    bool operator<(const TextPos& o) const { return line < o.line || (line == o.line && col < o.col); }
    bool operator<=(const TextPos& o) const { return !(o < *this); }
};

class TextDoc {
public:
    std::vector<std::string> lines{ std::string() };
    TextPos caret, anchor;
    int  want_col = -1;          // preferred DISPLAY column for vertical motion (-1 = caret's)
    int  tab_width = 4;
    bool dirty = false;
    bool crlf = false;           // file used \r\n
    bool final_newline = true;   // file ended with a newline
    uint64_t version = 0;        // bumped on every content change (views cache per version)
    int  first_changed_line = 0; // lowest line touched since the view last reset it

    // ---- load / save -----------------------------------------------------------------------
    static TextDoc from_text(const std::string& t) {
        TextDoc d;
        d.set_text(t);
        d.dirty = false;
        return d;
    }
    void set_text(const std::string& t) {
        lines.clear();
        crlf = t.find("\r\n") != std::string::npos;
        std::string cur;
        for (size_t i = 0; i < t.size(); ++i) {
            const char c = t[i];
            if (c == '\r') { if (i + 1 < t.size() && t[i + 1] == '\n') continue; lines.push_back(cur); cur.clear(); continue; }
            if (c == '\n') { lines.push_back(cur); cur.clear(); continue; }
            cur += c;
        }
        final_newline = !t.empty() && (t.back() == '\n' || t.back() == '\r');
        if (!final_newline || lines.empty()) lines.push_back(cur);
        else if (!cur.empty()) lines.push_back(cur);
        if (lines.empty()) lines.push_back("");
        caret = anchor = TextPos{};
        undo_.clear(); redo_.clear();
        saved_group_ = 0;
        changed(0);
    }
    std::string to_text() const {
        const char* nl = crlf ? "\r\n" : "\n";
        std::string o;
        for (size_t i = 0; i < lines.size(); ++i) {
            o += lines[i];
            if (i + 1 < lines.size() || final_newline) o += nl;
        }
        return o;
    }
    bool load_file(const std::string& path, std::string* err = nullptr) {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) { if (err) *err = "cannot open " + path; return false; }
        std::string t;
        char buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) t.append(buf, n);
        std::fclose(f);
        set_text(t);
        dirty = false;
        return true;
    }
    bool save_file(const std::string& path, std::string* err = nullptr) {
        const std::string t = to_text();
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) { if (err) *err = "cannot write " + path; return false; }
        const bool ok = std::fwrite(t.data(), 1, t.size(), f) == t.size();
        std::fclose(f);
        if (!ok) { if (err) *err = "short write to " + path; return false; }
        mark_saved();
        return true;
    }

    // ---- queries ------------------------------------------------------------------------------
    int line_count() const { return int(lines.size()); }
    const std::string& line(int i) const { return lines[size_t(std::max(0, std::min(i, line_count() - 1)))]; }
    TextPos clamp(TextPos p) const {
        p.line = std::max(0, std::min(p.line, line_count() - 1));
        p.col = std::max(0, std::min(p.col, int(lines[size_t(p.line)].size())));
        return p;
    }
    TextPos end_pos() const { return TextPos{ line_count() - 1, int(lines.back().size()) }; }
    bool has_sel() const { return caret != anchor; }
    TextPos sel_lo() const { return std::min(caret, anchor); }
    TextPos sel_hi() const { return std::max(caret, anchor); }
    std::string text_range(TextPos a, TextPos b) const {
        a = clamp(a); b = clamp(b);
        if (b < a) std::swap(a, b);
        if (a.line == b.line) return lines[size_t(a.line)].substr(size_t(a.col), size_t(b.col - a.col));
        std::string o = lines[size_t(a.line)].substr(size_t(a.col));
        for (int l = a.line + 1; l < b.line; ++l) { o += '\n'; o += lines[size_t(l)]; }
        o += '\n';
        o += lines[size_t(b.line)].substr(0, size_t(b.col));
        return o;
    }
    std::string selected_text() const { return text_range(sel_lo(), sel_hi()); }

    static bool is_cont(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }   // UTF-8 continuation
    // Display width of the byte at s[i] given the column d it starts at.
    int advance(const std::string& s, int i, int d) const {
        const char c = s[size_t(i)];
        if (c == '\t') return (d / tab_width + 1) * tab_width;
        return is_cont(c) ? d : d + 1;
    }
    // Tab-expanded, UTF-8-aware column <-> byte column.
    int display_col(int l, int col) const {
        const std::string& s = line(l);
        int d = 0;
        for (int i = 0; i < col && i < int(s.size()); ++i) d = advance(s, i, d);
        return d;
    }
    int col_from_display(int l, int dcol) const {
        const std::string& s = line(l);
        int d = 0;
        for (int i = 0; i < int(s.size()); ++i) {
            if (is_cont(s[size_t(i)])) continue;
            int j = i + 1;
            while (j < int(s.size()) && is_cont(s[size_t(j)])) ++j;   // the whole code point
            const int nd = advance(s, i, d);
            if (dcol < nd) return (dcol - d) * 2 < (nd - d) ? i : j;  // nearer edge of the char
            d = nd;
            i = j - 1;
        }
        return int(s.size());
    }
    int max_display_width() const {
        int m = 0;
        for (int l = 0; l < line_count(); ++l) m = std::max(m, display_col(l, int(lines[size_t(l)].size())));
        return m;
    }
    static bool word_char(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }
    std::string leading_ws(int l) const {
        const std::string& s = line(l);
        size_t i = 0;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        return s.substr(0, i);
    }
    std::string indent_unit() const {
        // Match the file: tabs if any line is tab-indented, else spaces.
        for (const std::string& s : lines) if (!s.empty() && s[0] == '\t') return "\t";
        return std::string(size_t(tab_width), ' ');
    }

    // ---- the one mutation primitive ------------------------------------------------------------
    // Replace [a, b) with `text` (which may contain '\n'); returns the end of the inserted text.
    // Records undo unless `record` is false.
    TextPos replace(TextPos a, TextPos b, const std::string& text, bool record = true) {
        a = clamp(a); b = clamp(b);
        if (b < a) std::swap(a, b);
        const std::string removed = text_range(a, b);
        if (removed.empty() && text.empty()) return a;
        const std::string head = lines[size_t(a.line)].substr(0, size_t(a.col));
        const std::string tail = lines[size_t(b.line)].substr(size_t(b.col));
        std::vector<std::string> ins;
        {
            std::string cur;
            for (char c : text) { if (c == '\n') { ins.push_back(cur); cur.clear(); } else if (c != '\r') cur += c; }
            ins.push_back(cur);
        }
        TextPos end;
        end.line = a.line + int(ins.size()) - 1;
        end.col = int((ins.size() == 1 ? head.size() : 0) + ins.back().size());
        ins.front() = head + ins.front();
        ins.back() += tail;
        lines.erase(lines.begin() + a.line, lines.begin() + b.line + 1);
        lines.insert(lines.begin() + a.line, ins.begin(), ins.end());
        if (record) {
            Edit e;
            e.at = a; e.removed = removed; e.inserted = text; e.group = group_;
            e.caret_before = caret; e.anchor_before = anchor;
            undo_.push_back(e);
            if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
            redo_.clear();
        }
        changed(a.line);
        return end;
    }

    // ---- editing commands (each is one undo group unless typing coalesces) ---------------------
    void insert(const std::string& text) {
        const bool typing = text.size() == 1 && text[0] != '\n';
        const bool coalesce = typing && !has_sel() && last_was_typing_ && caret == last_typed_end_ &&
                              !undo_.empty() && !(text[0] == ' ' && last_char_ != ' ');
        if (!coalesce) new_group();
        const TextPos lo = sel_lo();
        caret = anchor = replace(lo, sel_hi(), text);
        if (!undo_.empty()) { undo_.back().caret_after = caret; }
        want_col = -1;
        last_was_typing_ = typing;
        last_typed_end_ = caret;
        last_char_ = typing ? text[0] : 0;
    }
    // Enter: newline + the current line's indentation (+ one level after an opening brace/colon).
    void newline() {
        const int l = sel_lo().line;
        std::string ind = leading_ws(l);
        const std::string& s = line(l);
        const int upto = sel_lo().col;
        int k = upto - 1;
        while (k >= 0 && (s[size_t(k)] == ' ' || s[size_t(k)] == '\t')) --k;
        const char prev = k >= 0 ? s[size_t(k)] : 0;
        const bool open = prev == '{' || prev == '[' || prev == '(' || prev == ':';
        const char next = upto < int(s.size()) ? s[size_t(upto)] : 0;
        if (int(ind.size()) > upto) ind.resize(size_t(upto));
        new_group();
        std::string t = "\n" + ind;
        if (open) t += indent_unit();
        const TextPos lo = sel_lo();
        caret = anchor = replace(lo, sel_hi(), t);
        if (open && (next == '}' || next == ']' || next == ')')) {
            // "{|}" + Enter -> the closer goes on its own line at the outer indent
            const TextPos keep = caret;
            replace(caret, caret, "\n" + ind);
            caret = anchor = keep;
        }
        finish_cmd();
    }
    void backspace(bool word = false) {
        new_group();
        if (has_sel()) { erase_sel(); finish_cmd(); return; }
        if (caret.col == 0 && caret.line == 0) return;
        TextPos from = caret;
        if (word) from = word_left(caret);
        else if (caret.col > 0) {
            // In leading whitespace made of spaces, delete back to the previous indent stop.
            const std::string lw = leading_ws(caret.line);
            if (caret.col <= int(lw.size()) && lw.find('\t') == std::string::npos && caret.col > 0) {
                const int stop = ((caret.col - 1) / tab_width) * tab_width;
                from.col = stop;
            } else from = prev_pos(caret);
        } else from = TextPos{ caret.line - 1, int(line(caret.line - 1).size()) };
        caret = anchor = replace(from, caret, "");
        caret = anchor = from;
        finish_cmd();
    }
    void del(bool word = false) {
        new_group();
        if (has_sel()) { erase_sel(); finish_cmd(); return; }
        TextPos to = word ? word_right(caret) : next_pos(caret);
        if (to == caret) return;
        replace(caret, to, "");
        anchor = caret;
        finish_cmd();
    }
    void erase_sel() {
        const TextPos lo = sel_lo();
        replace(lo, sel_hi(), "");
        caret = anchor = lo;
    }
    void cut(std::string& out) { out = selected_text(); if (has_sel()) { new_group(); erase_sel(); finish_cmd(); } }
    void paste(const std::string& t) { new_group(); const TextPos lo = sel_lo(); caret = anchor = replace(lo, sel_hi(), t); finish_cmd(); }

    // Tab / Shift+Tab: with a multi-line selection (or shift) (un)indent the covered lines;
    // otherwise insert one indent unit at the caret.
    void tab(bool outdent) {
        const TextPos lo = sel_lo(), hi = sel_hi();
        if (!outdent && (!has_sel() || lo.line == hi.line)) { insert_indent(); return; }
        new_group();
        const int l1 = (hi.col == 0 && hi.line > lo.line) ? hi.line - 1 : hi.line;
        const std::string unit = indent_unit();
        TextPos nc = caret, na = anchor;
        for (int l = lo.line; l <= l1; ++l) {
            if (!outdent) {
                if (line(l).empty()) continue;
                replace(TextPos{ l, 0 }, TextPos{ l, 0 }, unit);
                if (nc.line == l && nc.col > 0) nc.col += int(unit.size());
                if (na.line == l && na.col > 0) na.col += int(unit.size());
            } else {
                const std::string& s = line(l);
                int n = 0;
                if (!s.empty() && s[0] == '\t') n = 1;
                else while (n < tab_width && n < int(s.size()) && s[size_t(n)] == ' ') ++n;
                if (!n) continue;
                replace(TextPos{ l, 0 }, TextPos{ l, n }, "");
                if (nc.line == l) nc.col = std::max(0, nc.col - n);
                if (na.line == l) na.col = std::max(0, na.col - n);
            }
        }
        caret = clamp(nc); anchor = clamp(na);
        finish_cmd();
    }
    // Toggle a line-comment prefix ("//", "#") on every line the selection touches.
    void toggle_comment(const std::string& prefix) {
        if (prefix.empty()) return;
        const TextPos lo = sel_lo(), hi = sel_hi();
        const int l1 = (hi.col == 0 && hi.line > lo.line) ? hi.line - 1 : hi.line;
        bool all = true;
        size_t min_ind = std::string::npos;
        for (int l = lo.line; l <= l1; ++l) {
            const std::string& s = line(l);
            const size_t i = s.find_first_not_of(" \t");
            if (i == std::string::npos) continue;
            min_ind = std::min(min_ind, i);
            if (s.compare(i, prefix.size(), prefix) != 0) all = false;
        }
        if (min_ind == std::string::npos) return;
        new_group();
        for (int l = lo.line; l <= l1; ++l) {
            const std::string s = line(l);
            const size_t i = s.find_first_not_of(" \t");
            if (i == std::string::npos) continue;
            if (all) {
                size_t n = prefix.size();
                if (i + n < s.size() && s[i + n] == ' ') ++n;
                replace(TextPos{ l, int(i) }, TextPos{ l, int(i + n) }, "");
                if (caret.line == l) caret.col = std::max(int(i), caret.col - int(n));
                if (anchor.line == l) anchor.col = std::max(int(i), anchor.col - int(n));
            } else {
                replace(TextPos{ l, int(min_ind) }, TextPos{ l, int(min_ind) }, prefix + " ");
                if (caret.line == l && caret.col >= int(min_ind)) caret.col += int(prefix.size()) + 1;
                if (anchor.line == l && anchor.col >= int(min_ind)) anchor.col += int(prefix.size()) + 1;
            }
        }
        caret = clamp(caret); anchor = clamp(anchor);
        finish_cmd();
    }
    void duplicate_lines() {
        const TextPos lo = sel_lo(), hi = sel_hi();
        const int l1 = (hi.col == 0 && hi.line > lo.line) ? hi.line - 1 : hi.line;
        std::string block;
        for (int l = lo.line; l <= l1; ++l) block += line(l) + "\n";
        new_group();
        replace(TextPos{ lo.line, 0 }, TextPos{ lo.line, 0 }, block);
        const int n = l1 - lo.line + 1;
        caret.line += n; anchor.line += n;
        finish_cmd();
    }
    void delete_lines() {
        const TextPos lo = sel_lo(), hi = sel_hi();
        const int l1 = (hi.col == 0 && hi.line > lo.line) ? hi.line - 1 : hi.line;
        new_group();
        if (l1 + 1 < line_count()) replace(TextPos{ lo.line, 0 }, TextPos{ l1 + 1, 0 }, "");
        else if (lo.line > 0) replace(TextPos{ lo.line - 1, int(line(lo.line - 1).size()) }, end_pos(), "");
        else replace(TextPos{ 0, 0 }, end_pos(), "");
        caret = anchor = clamp(TextPos{ std::min(lo.line, line_count() - 1), 0 });
        finish_cmd();
    }
    // Alt+Up / Alt+Down: move the covered lines one line up/down.
    void move_lines(int dir) {
        const TextPos lo = sel_lo(), hi = sel_hi();
        const int l1 = (hi.col == 0 && hi.line > lo.line) ? hi.line - 1 : hi.line;
        if (dir < 0 && lo.line == 0) return;
        if (dir > 0 && l1 + 1 >= line_count()) return;
        new_group();
        std::string block;
        for (int l = lo.line; l <= l1; ++l) { if (l > lo.line) block += '\n'; block += line(l); }
        if (dir < 0) {
            const std::string above = line(lo.line - 1);
            replace(TextPos{ lo.line - 1, 0 }, TextPos{ l1, int(line(l1).size()) }, block + "\n" + above);
        } else {
            const std::string below = line(l1 + 1);
            replace(TextPos{ lo.line, 0 }, TextPos{ l1 + 1, int(line(l1 + 1).size()) }, below + "\n" + block);
        }
        caret.line += dir; anchor.line += dir;
        caret = clamp(caret); anchor = clamp(anchor);
        finish_cmd();
    }

    // ---- motion --------------------------------------------------------------------------------
    TextPos next_pos(TextPos p) const {
        p = clamp(p);
        const std::string& s = line(p.line);
        if (p.col < int(s.size())) {
            int c = p.col + 1;
            while (c < int(s.size()) && is_cont(s[size_t(c)])) ++c;       // step over a whole code point
            return TextPos{ p.line, c };
        }
        if (p.line + 1 < line_count()) return TextPos{ p.line + 1, 0 };
        return p;
    }
    TextPos prev_pos(TextPos p) const {
        p = clamp(p);
        if (p.col > 0) {
            const std::string& s = line(p.line);
            int c = p.col - 1;
            while (c > 0 && is_cont(s[size_t(c)])) --c;
            return TextPos{ p.line, c };
        }
        if (p.line > 0) return TextPos{ p.line - 1, int(line(p.line - 1).size()) };
        return p;
    }
    TextPos word_left(TextPos p) const {
        p = clamp(p);
        if (p.col == 0) return prev_pos(p);
        const std::string& s = line(p.line);
        int c = p.col;
        while (c > 0 && (s[size_t(c - 1)] == ' ' || s[size_t(c - 1)] == '\t')) --c;
        if (c > 0 && word_char(s[size_t(c - 1)])) while (c > 0 && word_char(s[size_t(c - 1)])) --c;
        else if (c > 0) --c;
        return TextPos{ p.line, c };
    }
    TextPos word_right(TextPos p) const {
        p = clamp(p);
        const std::string& s = line(p.line);
        if (p.col >= int(s.size())) return next_pos(p);
        int c = p.col;
        if (word_char(s[size_t(c)])) while (c < int(s.size()) && word_char(s[size_t(c)])) ++c;
        else if (s[size_t(c)] != ' ' && s[size_t(c)] != '\t') ++c;
        while (c < int(s.size()) && (s[size_t(c)] == ' ' || s[size_t(c)] == '\t')) ++c;
        return TextPos{ p.line, c };
    }
    void move_to(TextPos p, bool extend, bool keep_want = false) {
        caret = clamp(p);
        if (!extend) anchor = caret;
        if (!keep_want) want_col = -1;
        break_typing();
    }
    void left(bool word, bool extend) {
        if (has_sel() && !extend && !word) { move_to(sel_lo(), false); return; }
        move_to(word ? word_left(caret) : prev_pos(caret), extend);
    }
    void right(bool word, bool extend) {
        if (has_sel() && !extend && !word) { move_to(sel_hi(), false); return; }
        move_to(word ? word_right(caret) : next_pos(caret), extend);
    }
    void vertical(int dl, bool extend) {
        if (want_col < 0) want_col = display_col(caret.line, caret.col);
        int l = caret.line + dl;
        TextPos p;
        if (l < 0) p = TextPos{ 0, 0 };
        else if (l >= line_count()) p = end_pos();
        else p = TextPos{ l, col_from_display(l, want_col) };
        const int keep = want_col;
        move_to(p, extend, true);
        want_col = keep;
    }
    // Home toggles between the first non-blank column and column 0.
    void home(bool extend) {
        const int ind = int(leading_ws(caret.line).size());
        move_to(TextPos{ caret.line, caret.col == ind ? 0 : ind }, extend);
    }
    void end(bool extend) { move_to(TextPos{ caret.line, int(line(caret.line).size()) }, extend); }
    void select_all() { anchor = TextPos{}; caret = end_pos(); break_typing(); }
    void select_word_at(TextPos p) {
        p = clamp(p);
        const std::string& s = line(p.line);
        int a = p.col, b = p.col;
        if (a < int(s.size()) && !word_char(s[size_t(a)]) && a > 0 && word_char(s[size_t(a - 1)])) --a, --b;
        while (a > 0 && word_char(s[size_t(a - 1)])) --a;
        while (b < int(s.size()) && word_char(s[size_t(b)])) ++b;
        if (a == b && b < int(s.size())) ++b;
        anchor = TextPos{ p.line, a }; caret = TextPos{ p.line, b };
        break_typing();
    }
    void select_line(int l) {
        l = std::max(0, std::min(l, line_count() - 1));
        anchor = TextPos{ l, 0 };
        caret = l + 1 < line_count() ? TextPos{ l + 1, 0 } : TextPos{ l, int(line(l).size()) };
        break_typing();
    }
    // The word under/left of the caret (for "find the word under the caret").
    std::string word_at_caret() const {
        if (has_sel() && sel_lo().line == sel_hi().line) return selected_text();
        const std::string& s = line(caret.line);
        int a = caret.col, b = caret.col;
        while (a > 0 && word_char(s[size_t(a - 1)])) --a;
        while (b < int(s.size()) && word_char(s[size_t(b)])) ++b;
        return s.substr(size_t(a), size_t(b - a));
    }

    // ---- find / replace ------------------------------------------------------------------------
    static char fold(char c, bool cs) { return (!cs && c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }
    // Match `q` at (l, c)? (single-line queries only; the find bar has no newlines)
    bool match_at(int l, int c, const std::string& q, bool cs, bool whole) const {
        const std::string& s = line(l);
        if (q.empty() || c < 0 || c + int(q.size()) > int(s.size())) return false;
        for (size_t i = 0; i < q.size(); ++i) if (fold(s[size_t(c) + i], cs) != fold(q[i], cs)) return false;
        if (whole) {
            if (c > 0 && word_char(s[size_t(c - 1)])) return false;
            const size_t e = size_t(c) + q.size();
            if (e < s.size() && word_char(s[e])) return false;
        }
        return true;
    }
    // Select the next (or previous) match after (before) the selection, wrapping. Returns false
    // when there is none.
    bool find(const std::string& q, bool cs, bool whole, bool backwards) {
        if (q.empty()) return false;
        const int n = line_count();
        TextPos from = backwards ? sel_lo() : sel_hi();
        for (int step = 0; step <= n; ++step) {
            const int l = backwards ? ((from.line - step) % n + n) % n : (from.line + step) % n;
            const std::string& s = line(l);
            if (!backwards) {
                const int c0 = step == 0 ? from.col : 0;
                for (int c = c0; c + int(q.size()) <= int(s.size()); ++c)
                    if (match_at(l, c, q, cs, whole)) { anchor = TextPos{ l, c }; caret = TextPos{ l, c + int(q.size()) }; break_typing(); return true; }
            } else {
                const int c1 = step == 0 ? from.col - int(q.size()) : int(s.size()) - int(q.size());
                for (int c = c1; c >= 0; --c)
                    if (match_at(l, c, q, cs, whole)) { anchor = TextPos{ l, c }; caret = TextPos{ l, c + int(q.size()) }; break_typing(); return true; }
            }
        }
        // wrapped fully: the partial first line (before `from`) when going forward
        if (!backwards) {
            const std::string& s = line(from.line);
            for (int c = 0; c < from.col && c + int(q.size()) <= int(s.size()); ++c)
                if (match_at(from.line, c, q, cs, whole)) { anchor = TextPos{ from.line, c }; caret = TextPos{ from.line, c + int(q.size()) }; return true; }
        }
        return false;
    }
    int count(const std::string& q, bool cs, bool whole) const {
        if (q.empty()) return 0;
        int k = 0;
        for (int l = 0; l < line_count(); ++l)
            for (int c = 0; c + int(q.size()) <= int(line(l).size()); ++c)
                if (match_at(l, c, q, cs, whole)) { ++k; c += int(q.size()) - 1; }
        return k;
    }
    // Replace the current selection if it matches, then find the next match.
    bool replace_one(const std::string& q, const std::string& r, bool cs, bool whole) {
        if (has_sel() && sel_lo().line == sel_hi().line && sel_hi().col - sel_lo().col == int(q.size()) &&
            match_at(sel_lo().line, sel_lo().col, q, cs, whole)) {
            new_group();
            const TextPos lo = sel_lo();
            caret = anchor = replace(lo, sel_hi(), r);
            finish_cmd();
        }
        return find(q, cs, whole, false);
    }
    int replace_all(const std::string& q, const std::string& r, bool cs, bool whole) {
        if (q.empty()) return 0;
        new_group();
        int k = 0;
        for (int l = 0; l < line_count(); ++l)
            for (int c = 0; c + int(q.size()) <= int(line(l).size()); ++c)
                if (match_at(l, c, q, cs, whole)) {
                    replace(TextPos{ l, c }, TextPos{ l, c + int(q.size()) }, r);
                    c += int(r.size()) - 1;
                    ++k;
                }
        caret = anchor = clamp(caret);
        finish_cmd();
        return k;
    }

    // ---- undo / redo ---------------------------------------------------------------------------
    bool can_undo() const { return !undo_.empty(); }
    bool can_redo() const { return !redo_.empty(); }
    bool undo() {
        if (undo_.empty()) return false;
        const int g = undo_.back().group;
        TextPos c{}, a{};
        while (!undo_.empty() && undo_.back().group == g) {
            Edit e = undo_.back();
            undo_.pop_back();
            const TextPos end = end_of(e.at, e.inserted);
            replace(e.at, end, e.removed, false);
            c = e.caret_before; a = e.anchor_before;
            redo_.push_back(e);
        }
        caret = clamp(c); anchor = clamp(a);
        break_typing();
        dirty = top_group() != saved_group_;
        return true;
    }
    bool redo() {
        if (redo_.empty()) return false;
        const int g = redo_.back().group;
        TextPos last{};
        while (!redo_.empty() && redo_.back().group == g) {
            Edit e = redo_.back();
            redo_.pop_back();
            last = replace(e.at, end_of(e.at, e.removed), e.inserted, false);
            undo_.push_back(e);
        }
        caret = anchor = clamp(last);
        break_typing();
        dirty = top_group() != saved_group_;
        return true;
    }
    void break_typing() { last_was_typing_ = false; }

private:
    struct Edit {
        TextPos at;
        std::string removed, inserted;
        int group = 0;
        TextPos caret_before, anchor_before, caret_after;
    };
    static constexpr size_t kMaxUndo = 4000;   // edits (groups are usually 1-40 edits)

    static TextPos end_of(TextPos at, const std::string& t) {
        TextPos e = at;
        for (char c : t) { if (c == '\n') { ++e.line; e.col = 0; } else ++e.col; }
        return e;
    }
    void new_group() { ++group_; last_was_typing_ = false; }
    void finish_cmd() { want_col = -1; last_was_typing_ = false; }
    void insert_indent() {
        new_group();
        const std::string unit = indent_unit();
        std::string t = unit;
        if (unit != "\t") {
            const int d = display_col(caret.line, sel_lo().col);
            t = std::string(size_t(tab_width - d % tab_width), ' ');
        }
        const TextPos lo = sel_lo();
        caret = anchor = replace(lo, sel_hi(), t);
        finish_cmd();
    }
    void changed(int from_line) {
        ++version;
        dirty = true;
        first_changed_line = std::min(first_changed_line, from_line);
    }

    std::vector<Edit> undo_, redo_;
    int group_ = 0;
    bool last_was_typing_ = false;
    TextPos last_typed_end_{};
    char last_char_ = 0;
    int saved_group_ = 0;
    int top_group() const { return undo_.empty() ? 0 : undo_.back().group; }

public:
    // The view resets this after re-highlighting (lines above it kept their cached state).
    void reset_changed_mark() { first_changed_line = line_count(); }
    void mark_saved() { saved_group_ = top_group(); dirty = false; }
};

} // namespace phxstudio
#endif // PHX_TOOLS_PHXSTUDIO_TEXTDOC_H
