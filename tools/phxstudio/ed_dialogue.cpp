// tools/phxstudio/ed_dialogue.cpp — the DIALOGUE editor panel over a `.dlg` (tools/phxpack/dialogue.h).
// Conversations on the left (and the speakers, with their portraits); the selected conversation's
// lines in the middle, each a card: its id, speaker, text, where it goes next, an `if` that skips it
// and a `do` that changes variables, and its choices (text, next, if, do). On the right, PLAY runs
// the conversation the way the game will (the compiled tables, the runtime's rules), with the
// variables it reads as editable numbers, and the problems the bake would report.
//
// The bake (phxpack / bake_project.py) turns the file into a Dialogue asset named after it; the
// game flow plays assets/dialogue.dlg (a "talk" screen, or a Talk component's conversation).
#include "host.h"
#include "dialogue.h"         // tools/phxpack

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace phxstudio {
namespace {

using namespace twk;
using phxtool::DlgChoice;
using phxtool::DlgConversation;
using phxtool::DlgDoc;
using phxtool::DlgNode;

class DialogueView final : public DocView {
public:
    DlgDoc doc;
    bool dirty_ = false;

    bool load(const std::string& path_, std::string* err) {
        path = path_;
        std::string text;
        if (FILE* f = std::fopen(path.c_str(), "rb")) {
            char buf[16384];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
            std::fclose(f);
        } else {
            if (err) *err = "cannot open " + path;
            return false;
        }
        if (!phxtool::dlg_from_json(text, doc, err)) return false;
        disk_stamp = file_stamp(path);
        dirty_ = false;
        changed();
        return true;
    }

    FileKind kind() const override { return FileKind::Dialogue; }
    int icon() const override { return kIconFileCode; }
    bool dirty() const override { return dirty_; }
    bool save(Host& h, std::string* err) override {
        const std::string t = phxtool::dlg_to_json(doc);
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
    bool reload(Host&, std::string* err) override {
        DialogueView fresh;
        if (!fresh.load(path, err)) return false;
        doc = fresh.doc; dirty_ = false; disk_stamp = fresh.disk_stamp;
        changed();
        return true;
    }
    bool undo() override {
        if (undo_.empty()) return false;
        redo_.push_back(doc); doc = undo_.back(); undo_.pop_back(); dirty_ = true; changed();
        return true;
    }
    bool redo() override {
        if (redo_.empty()) return false;
        undo_.push_back(doc); doc = redo_.back(); redo_.pop_back(); dirty_ = true; changed();
        return true;
    }
    std::string status() const override {
        size_t lines = 0;
        for (const DlgConversation& c : doc.convs) lines += c.nodes.size();
        int errs = 0, warns = 0;
        for (const auto& p : probs_) (p.error ? errs : warns)++;
        return fmt("%d conversations  %d lines  %d speakers   %d errors  %d warnings", int(doc.convs.size()), int(lines),
                   int(doc.speakers.size()), errs, warns);
    }

    void draw(Host& h, const Rect& area) override {
        Gui& g = h.gui();
        if (!id_) id_ = Gui::hash(path.c_str());
        g.push_id(id_);
        clamp();
        Rect r = area;
        g.rect(r, g.th.bg, kSubBg);
        Rect left = cut_left(r, std::min(150, std::max(112, area.w / 5)));
        Rect right = cut_right(r, std::min(240, std::max(160, area.w / 4)));
        draw_lists(h, left);
        draw_play(h, right);
        draw_lines(h, r);
        g.pop_id();
    }

private:
    uint32_t id_ = 0;
    std::vector<DlgDoc> undo_, redo_;
    int conv_ = 0, line_ = 0, spk_ = -1;
    int conv_scroll_ = 0, spk_scroll_ = 0, lines_scroll_ = 0;
    std::vector<phxtool::DlgProblem> probs_;
    // play-through
    phxtool::DlgCompiled compiled_;
    bool compiled_ok_ = false;
    phxtool::DlgSim sim_;
    std::map<std::string, int> vars_;      // the starting values of the variables
    std::vector<std::string> var_names_;
    std::vector<std::string> log_;         // what was said so far

    template <class F> void edit(F f) {
        DlgDoc before = doc;
        f();
        undo_.push_back(before);
        if (undo_.size() > 200) undo_.erase(undo_.begin());
        redo_.clear();
        dirty_ = true;
        changed();
    }
    void changed() {
        probs_ = phxtool::dlg_validate(doc);
        std::string err;
        compiled_ok_ = phxtool::dlg_compile(doc, compiled_, &err);
        var_names_ = phxtool::dlg_var_names(doc);
        sim_ = phxtool::DlgSim{};
        log_.clear();
        clamp();
    }
    void clamp() {
        conv_ = std::max(0, std::min(conv_, int(doc.convs.size()) - 1));
        const int n = conv_ >= 0 && conv_ < int(doc.convs.size()) ? int(doc.convs[size_t(conv_)].nodes.size()) : 0;
        line_ = std::max(0, std::min(line_, n - 1));
        spk_ = std::min(spk_, int(doc.speakers.size()) - 1);
    }
    DlgConversation* conv() { return conv_ >= 0 && conv_ < int(doc.convs.size()) ? &doc.convs[size_t(conv_)] : nullptr; }

    // ---- left: conversations + speakers ----
    void draw_lists(Host& h, Rect col) {
        Gui& g = h.gui();
        {
            Rect b = panel_section(g, col, std::max(120, col.h * 3 / 5), "CONVERSATIONS");
            const int n = int(doc.convs.size());
            const int rows = std::max(2, std::min(n, (b.h - 36) / 11));
            const Rect list{ b.x, b.y, b.w, rows * 11 + 2 };
            g.rect(list, g.th.field, kSubWidget);
            g.wheel_scroll(list, conv_scroll_, n, rows, 1);
            for (int i = 0; i < rows && conv_scroll_ + i < n; ++i) {
                const int k = conv_scroll_ + i;
                const Rect rr{ list.x + 1, list.y + 1 + i * 11, list.w - 2, 11 };
                if (k == conv_) g.rect(rr, g.th.sel, kSubImage);
                else if (g.hover(rr)) g.rect(rr, g.th.hover, kSubImage);
                g.text(rr.x + 3, rr.y + 2, doc.convs[size_t(k)].name, g.th.text, kSubText, rr.w - 26);
                g.text(rr.right() - 20, rr.y + 2, fmt("%d", int(doc.convs[size_t(k)].nodes.size())), g.th.faint);
                if (g.clicked(rr)) { conv_ = k; line_ = 0; lines_scroll_ = 0; sim_ = phxtool::DlgSim{}; log_.clear(); }
            }
            if (!n) g.text(list.x + 3, list.y + 3, "none: add one", g.th.warn);
            int y = list.bottom() + 3;
            Gui::Row row(Rect{ b.x, y, b.w, 12 }, 3);
            if (g.button(row.take(20), "+", Btn{ false, true, false, 0, "New conversation" }))
                edit([&] {
                    DlgConversation c;
                    c.name = doc.fresh_conv_name();
                    c.nodes.push_back(DlgNode{ "n1", doc.speakers.empty() ? "" : doc.speakers[0].name, "...", "", "", "", {} });
                    doc.convs.push_back(c);
                    conv_ = int(doc.convs.size()) - 1;
                });
            if (g.button(row.take(28), "dup", Btn{ false, n > 0, false, 0, "Duplicate this conversation" }) && n > 0)
                edit([&] { DlgConversation c = doc.convs[size_t(conv_)]; c.name = doc.fresh_conv_name(c.name); doc.convs.push_back(c); conv_ = int(doc.convs.size()) - 1; });
            if (g.button(row.take(28), "del", Btn{ false, n > 0, false, 0, "Delete this conversation" }) && n > 0)
                edit([&] { doc.convs.erase(doc.convs.begin() + conv_); });
            y += 16;
            if (DlgConversation* c = conv()) {
                std::string nm = c->name;
                if (g.text_field(g.id("cname"), Rect{ b.x, y, b.w, 12 }, nm, "name", 0,
                                 "Its name: a Talk component's conversation, a talk screen's `dialogue`") && nm != c->name) {
                    if (phxtool::dlg_ident(nm) && doc.find_conv(nm) < 0) edit([&] { doc.convs[size_t(conv_)].name = nm; });
                    else h.toast("conversation names: unique, letters/digits/_", Toast::Warn);
                }
            }
        }
        {
            Rect b = panel_section(g, col, std::max(80, col.h), "SPEAKERS");
            const int n = int(doc.speakers.size());
            const int rows = std::max(2, std::min(n, (b.h - 50) / 11));
            const Rect list{ b.x, b.y, b.w, rows * 11 + 2 };
            g.rect(list, g.th.field, kSubWidget);
            g.wheel_scroll(list, spk_scroll_, n, rows, 1);
            for (int i = 0; i < rows && spk_scroll_ + i < n; ++i) {
                const int k = spk_scroll_ + i;
                const Rect rr{ list.x + 1, list.y + 1 + i * 11, list.w - 2, 11 };
                if (k == spk_) g.rect(rr, g.th.sel, kSubImage);
                else if (g.hover(rr)) g.rect(rr, g.th.hover, kSubImage);
                g.text(rr.x + 3, rr.y + 2, doc.speakers[size_t(k)].name, g.th.text, kSubText, rr.w - 6);
                if (g.clicked(rr)) spk_ = k;
            }
            int y = list.bottom() + 3;
            Gui::Row row(Rect{ b.x, y, b.w, 12 }, 3);
            if (g.button(row.take(20), "+", Btn{ false, true, false, 0, "New speaker" }))
                edit([&] {
                    std::string nm = "Speaker";
                    for (int k = 2; doc.find_speaker(nm) >= 0; ++k) nm = "Speaker" + std::to_string(k);
                    doc.speakers.push_back(phxtool::DlgSpeaker{ nm, "" });
                    spk_ = int(doc.speakers.size()) - 1;
                });
            const bool have = spk_ >= 0 && spk_ < n;
            if (g.button(row.take(28), "del", Btn{ false, have, false, 0, "Delete this speaker (its lines keep the name)" }) && have)
                edit([&] { doc.speakers.erase(doc.speakers.begin() + spk_); });
            y += 16;
            if (have) {
                phxtool::DlgSpeaker sp = doc.speakers[size_t(spk_)];
                std::string nm = sp.name, pt = sp.portrait;
                if (g.text_field(g.id("sname"), Rect{ b.x, y, b.w, 12 }, nm, "name", 0, "Shown above the box") && nm != sp.name && !nm.empty())
                    edit([&] {
                        for (DlgConversation& c : doc.convs) for (DlgNode& ln : c.nodes) if (ln.speaker == sp.name) ln.speaker = nm;
                        doc.speakers[size_t(spk_)].name = nm;
                    });
                y += 15;
                if (g.text_field(g.id("sport"), Rect{ b.x, y, b.w, 12 }, pt, "portrait (a texture)", 0,
                                 "A texture asset shown left of the text: assets/sage.png -> sage") && pt != sp.portrait)
                    edit([&] { doc.speakers[size_t(spk_)].portrait = pt; });
            }
        }
    }

    // ---- middle: the lines of the selected conversation, as cards ----
    // A card is one row per field group when wide; narrow, next / if+do / each choice's details
    // get rows of their own.
    static bool wide(int w) { return w >= 420; }
    static int card_h(const DlgNode& n, int w) {
        return 6 + 15 + (wide(w) ? 0 : 30) + 15 + int(n.choices.size()) * (wide(w) ? 15 : 30) + 15;
    }

    void draw_lines(Host& h, Rect r) {
        Gui& g = h.gui();
        r = r.inset(4);
        DlgConversation* c = conv();
        if (!c) { g.text(r.x + 4, r.y + 4, "no conversation: add one on the left", g.th.faint); return; }
        int total = 0;
        for (const DlgNode& n : c->nodes) total += card_h(n, r.w) + 4;
        total += 20;
        if (const int wv = g.wheel(r)) lines_scroll_ -= wv * 24;
        lines_scroll_ = std::max(0, std::min(lines_scroll_, std::max(0, total - r.h)));
        g.push_clip(r);
        // the id choices every "next" dropdown offers
        std::vector<std::string> targets = { "(following)", "end" };
        for (const DlgNode& n : c->nodes) if (!n.id.empty()) targets.push_back(n.id);
        std::vector<std::string> speakers = { "(none)" };
        for (const auto& s : doc.speakers) speakers.push_back(s.name);
        int y = r.y - lines_scroll_;
        for (size_t i = 0; i < c->nodes.size(); ++i) {
            const int ch = card_h(c->nodes[i], r.w);
            if (y + ch >= r.y && y <= r.bottom()) draw_card(h, *c, i, Rect{ r.x, y, r.w, ch }, targets, speakers);
            y += ch + 4;
            if (!conv()) break;                                   // the card deleted the conversation's last line
            c = conv();
        }
        if (c && g.button(Rect{ r.x, y, 90, 14 }, "+ line", Btn{ false, true, false, 0, "Add a line at the end" }))
            edit([&] {
                DlgConversation& cc = doc.convs[size_t(conv_)];
                cc.nodes.push_back(DlgNode{ DlgDoc::fresh_node_id(cc), cc.nodes.empty() ? "" : cc.nodes.back().speaker, "...", "", "", "", {} });
                line_ = int(cc.nodes.size()) - 1;
            });
        g.pop_clip();
    }

    static int index_of(const std::vector<std::string>& v, const std::string& s, int def) {
        for (size_t i = 0; i < v.size(); ++i) if (v[i] == s) return int(i);
        return def;
    }

    void draw_card(Host& h, DlgConversation& c, size_t i, const Rect& cr, const std::vector<std::string>& targets,
                   const std::vector<std::string>& speakers) {
        Gui& g = h.gui();
        DlgNode n = c.nodes[i];
        const bool sel = int(i) == line_;
        const bool playing = sim_.active() && compiled_ok_ && conv_ < int(compiled_.convs.size()) &&
                             sim_.node == compiled_.convs[size_t(conv_)].first_node + i;
        g.rect(cr, sel ? g.th.panel2 : g.th.panel, kSubFill);
        if (playing) g.rect(Rect{ cr.x, cr.y, 3, cr.h }, g.th.accent, kSubOver);
        bool has_err = false;
        for (const auto& p : probs_) if (p.error && p.conv == conv_ && p.node == int(i)) has_err = true;
        if (has_err) g.frame_rect(cr, g.th.bad, kSubOver);
        if (g.hover(cr) && (g.in->pressed & kMouseL)) line_ = int(i);
        const int W = cr.w - 8;
        int x = cr.x + 4, y = cr.y + 3;
        const uint32_t base = uint32_t(i) * 64u;
        auto field = [&](const char* key, int w, std::string& v, const char* ph, const char* help) {
            const bool ch = g.text_field(g.id(key, int(base)), Rect{ x, y, w, 12 }, v, ph, 0, help);
            x += w + 3;
            return ch;
        };
        const bool wd = wide(cr.w);
        // row 1: #, id, speaker (+ next, if, do when wide)
        g.text(x, y + 2, fmt("%d", int(i) + 1), g.th.faint);
        x += 16;
        std::string id = n.id;
        if (field("id", 50, id, "id", "The line's id (what a next / a choice names)") && id != n.id) {
            if (id != "end" && std::none_of(c.nodes.begin(), c.nodes.end(), [&](const DlgNode& o) { return !id.empty() && o.id == id; }))
                edit([&] { DlgDoc::rename_node(doc.convs[size_t(conv_)], i, id); });
            else h.toast("line ids must be unique (and not 'end')", Toast::Warn);
        }
        int si = index_of(speakers, n.speaker, 0);
        const int spw = wd ? 70 : std::max(40, cr.x + 4 + W - x);
        if (g.dropdown(g.id("spk", int(base)), Rect{ x, y, spw, 12 }, speakers, si, "Who says it"))
            edit([&] { doc.convs[size_t(conv_)].nodes[i].speaker = si == 0 ? std::string() : speakers[size_t(si)]; });
        x += spw + 3;
        if (!wd) { x = cr.x + 4; y += 15; }
        g.text(x, y + 2, "->", g.th.dim);
        x += 14;
        int ni = index_of(targets, n.next.empty() ? "(following)" : n.next, 0);
        const int nw = wd ? 64 : std::max(40, cr.x + 4 + W - x);
        if (g.dropdown(g.id("nxt", int(base)), Rect{ x, y, nw, 12 }, targets, ni, "Where it goes after this line (no choices)"))
            edit([&] { doc.convs[size_t(conv_)].nodes[i].next = ni == 0 ? std::string() : targets[size_t(ni)]; });
        x += nw + 3;
        if (!wd) { x = cr.x + 4; y += 15; }
        const int rest = std::max(30, (cr.x + 4 + W - x - 3) / 2);
        std::string cond = n.cond, eff = n.effects;
        if (field("if", rest, cond, "if (always)", "Skip this line unless: coins >= 5, key, !key") && cond != n.cond)
            edit([&] { doc.convs[size_t(conv_)].nodes[i].cond = cond; });
        if (field("do", rest, eff, "do (nothing)", "When shown: key = 1, coins -= 5, met") && eff != n.effects)
            edit([&] { doc.convs[size_t(conv_)].nodes[i].effects = eff; });
        // the text
        x = cr.x + 4; y += 15;
        std::string text = n.text;
        if (field("txt", W, text, "what is said", "The line (the box wraps it)") && text != n.text)
            edit([&] { doc.convs[size_t(conv_)].nodes[i].text = text; });
        // choices (wide: one row each; narrow: the text, then next / if / do)
        for (size_t k = 0; k < n.choices.size(); ++k) {
            DlgChoice chc = n.choices[k];
            x = cr.x + 20; y += 15;
            g.text(cr.x + 8, y + 2, ">", g.th.accent);
            const uint32_t cb = base + 1 + uint32_t(k) * 4;
            const int tw = wd ? std::max(60, W / 3) : std::max(40, cr.x + 4 + W - x - 15);
            std::string t = chc.text;
            if (g.text_field(g.id("ct", int(cb)), Rect{ x, y, tw, 12 }, t, "choice", 0, "What the player picks") && t != chc.text)
                edit([&] { doc.convs[size_t(conv_)].nodes[i].choices[k].text = t; });
            x += tw + 3;
            if (!wd) {
                if (g.icon_button(Rect{ x, y, 12, 12 }, kIconClose, "Remove this choice"))
                    edit([&] { auto& v = doc.convs[size_t(conv_)].nodes[i].choices; v.erase(v.begin() + std::ptrdiff_t(k)); });
                x = cr.x + 20; y += 15;
            }
            int ci = index_of(targets, chc.next.empty() ? "(following)" : chc.next, 0);
            if (g.dropdown(g.id("cn", int(cb)), Rect{ x, y, 64, 12 }, targets, ci, "Where picking it leads"))
                edit([&] { doc.convs[size_t(conv_)].nodes[i].choices[k].next = ci == 0 ? std::string() : targets[size_t(ci)]; });
            x += 67;
            const int cw = std::max(24, (cr.x + 4 + W - x - (wd ? 18 : 3)) / 2);
            std::string cc = chc.cond, ce = chc.effects;
            if (g.text_field(g.id("ci", int(cb)), Rect{ x, y, cw, 12 }, cc, "if (always)", 0, "Hidden unless: coins >= 5") && cc != chc.cond)
                edit([&] { doc.convs[size_t(conv_)].nodes[i].choices[k].cond = cc; });
            x += cw + 3;
            if (g.text_field(g.id("cd", int(cb)), Rect{ x, y, cw, 12 }, ce, "do (nothing)", 0, "When picked: coins -= 5, key = 1") && ce != chc.effects)
                edit([&] { doc.convs[size_t(conv_)].nodes[i].choices[k].effects = ce; });
            x += cw + 3;
            if (wd && g.icon_button(Rect{ x, y, 12, 12 }, kIconClose, "Remove this choice"))
                edit([&] { auto& v = doc.convs[size_t(conv_)].nodes[i].choices; v.erase(v.begin() + std::ptrdiff_t(k)); });
        }
        // row: line actions
        y += 15;
        Gui::Row row(Rect{ cr.x + 4, y, W, 12 }, 3);
        if (g.button(row.take(52), "+ choice", Btn{ false, n.choices.size() < 8, false, 0, "Add a choice (up to 8 show)" }) && n.choices.size() < 8)
            edit([&] { doc.convs[size_t(conv_)].nodes[i].choices.push_back(DlgChoice{ "...", "end", "", "" }); });
        if (g.button(row.take(40), "+ line", Btn{ false, true, false, 0, "Insert a new line after this one" }))
            edit([&] {
                DlgConversation& cc = doc.convs[size_t(conv_)];
                cc.nodes.insert(cc.nodes.begin() + std::ptrdiff_t(i) + 1, DlgNode{ DlgDoc::fresh_node_id(cc), n.speaker, "...", "", "", "", {} });
                line_ = int(i) + 1;
            });
        if (g.icon_button(row.take(12), kIconUp, "Move up", false, i > 0) && i > 0)
            edit([&] { std::swap(doc.convs[size_t(conv_)].nodes[i], doc.convs[size_t(conv_)].nodes[i - 1]); line_ = int(i) - 1; });
        if (g.icon_button(row.take(12), kIconDown, "Move down", false, i + 1 < c.nodes.size()) && i + 1 < c.nodes.size())
            edit([&] { std::swap(doc.convs[size_t(conv_)].nodes[i], doc.convs[size_t(conv_)].nodes[i + 1]); line_ = int(i) + 1; });
        if (g.icon_button(row.take(12), kIconTrash, "Delete this line"))
            edit([&] { auto& v = doc.convs[size_t(conv_)].nodes; v.erase(v.begin() + std::ptrdiff_t(i)); });
    }

    // ---- right: play-through + variables + problems ----
    void draw_play(Host& h, Rect col) {
        Gui& g = h.gui();
        {
            Rect b = panel_section(g, col, std::max(130, col.h / 2), "PLAY");
            int y = b.y;
            DlgConversation* c = conv();
            Gui::Row row(Rect{ b.x, y, b.w, 12 }, 3);
            const bool can = compiled_ok_ && c && !c->nodes.empty();
            if (g.button(row.take(70), sim_.active() ? "restart" : "play", Btn{ false, can, false, 0,
                         can ? "Run this conversation from its first line, as the game will" : "fix the errors first" }) && can) {
                sim_ = phxtool::DlgSim{};
                for (const auto& kv : vars_) sim_.vars[phx::fnv1a(kv.first.c_str())] = kv.second;
                log_.clear();
                sim_.start(compiled_, c->name);
            }
            y += 16;
            if (!sim_.active()) {
                g.text(b.x, y, log_.empty() ? "press play" : "(the conversation ended)", g.th.faint, kSubText, b.w);
                y += 12;
            } else {
                const std::string who = sim_.speaker();
                if (!who.empty()) { g.text(b.x, y, who, g.th.accent, kSubText, b.w); y += 11; }
                for (const std::string& ln : wrap(sim_.text(), std::max(8, b.w / Gui::text_w("x")))) {
                    if (y + 10 > b.bottom()) break;
                    g.text(b.x, y, ln, g.th.text, kSubText, b.w);
                    y += 10;
                }
                y += 4;
                if (sim_.visible.empty()) {
                    if (g.button(Rect{ b.x, y, 70, 12 }, "next (A)", Btn{ false, true, false, 0, "Go on" })) step(-1);
                    y += 15;
                } else {
                    for (size_t k = 0; k < sim_.visible.size() && y + 12 <= b.bottom(); ++k) {
                        const char* t = compiled_.str(compiled_.choices[sim_.visible[k]].text);
                        if (g.button(Rect{ b.x, y, b.w, 12 }, std::string("> ") + t, Btn{ false, true, false, 0, "Pick it" })) { step(int(k)); break; }
                        y += 14;
                    }
                }
            }
            (void)y;
        }
        {
            Rect b = panel_section(g, col, std::max(60, col.h / 4), "VARIABLES");
            int y = b.y;
            if (var_names_.empty()) g.text(b.x, y, "none (use if / do)", g.th.faint, kSubText, b.w);
            for (const std::string& v : var_names_) {
                if (y + 12 > b.bottom()) break;
                g.text(b.x, y + 2, v, g.th.dim, kSubText, b.w - 90);
                int start = vars_.count(v) ? vars_[v] : 0;
                if (g.int_field(g.id(v.c_str()), Rect{ b.right() - 84, y, 40, 12 }, start, -32768, 32767, 1,
                                "Its value when play starts (in the game: the flow's totals, e.g. coins)"))
                    vars_[v] = start;
                const int now = sim_.d ? sim_.get(phx::fnv1a(v.c_str())) : start;
                g.text(b.right() - 40, y + 2, fmt("= %d", now), g.th.accent, kSubText, 40);
                y += 14;
            }
        }
        {
            Rect b = panel_section(g, col, std::max(40, col.h), "PROBLEMS");
            int y = b.y;
            if (probs_.empty()) g.text(b.x, y, "none - bakes clean", g.th.good, kSubText, b.w);
            for (const auto& p : probs_) {
                if (y + 11 > b.bottom()) break;
                const Rect rr{ b.x, y, b.w, 11 };
                g.icon(b.x, y, kIconWarning, p.error ? g.th.bad : g.th.warn);
                g.text(b.x + 13, y + 1, p.what, p.error ? g.th.bad : g.th.warn, kSubText, b.w - 14);
                g.tip(rr, p.what);
                if (g.clicked(rr) && p.conv >= 0) { conv_ = p.conv; if (p.node >= 0) line_ = p.node; }
                y += 11;
            }
        }
    }
    void step(int pick) {
        log_.push_back(sim_.text());
        sim_.advance(pick < 0 ? 0 : pick);
    }
    static std::vector<std::string> wrap(const std::string& s, int cols) {
        std::vector<std::string> out;
        std::string line, word;
        auto flush_word = [&]() {
            if (word.empty()) return;
            if (!line.empty() && int(line.size() + 1 + word.size()) > cols) { out.push_back(line); line.clear(); }
            line += (line.empty() ? "" : " ") + word;
            word.clear();
        };
        for (char ch : s) {
            if (ch == ' ') flush_word();
            else if (ch == '\n') { flush_word(); out.push_back(line); line.clear(); }
            else word += ch;
        }
        flush_word();
        if (!line.empty()) out.push_back(line);
        return out;
    }
};

} // namespace

std::unique_ptr<DocView> make_dialogue_view(Host& h, const std::string& path, std::string* err) {
    (void)h;
    std::unique_ptr<DialogueView> v(new DialogueView);
    if (!v->load(path, err)) return nullptr;
    return v;
}

} // namespace phxstudio
