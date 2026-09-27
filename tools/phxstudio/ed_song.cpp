// tools/phxstudio/ed_song.cpp — the MUSIC editor panel: a small pattern tracker over a `.song`
// document (tools/phxpack/synth.h). Patterns are rows x channels of notes; the order list strings
// patterns into the song; instruments are synth voices (wave + envelope). The bake renders the
// song once to a looping Sound asset named after the file (phxsnd / bake_project.py), which a
// game plays with app.audio().play_music() or the flow table's `music` column.
//
//   piano keys   Z S X D C V G B H N J M , L . ; /  (octave)   Q 2 W 3 E R 5 T 6 Y 7 U I 9 O 0 P (octave + 1)
//   1            note off          Delete / Backspace   clear the cell
//   arrows / PgUp / PgDn / Home / End   move          Space   play / stop the song
//   F / Shift+F  octave up / down
#include "host.h"
#include "synth.h"            // tools/phxpack

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace phxstudio {
namespace {

using namespace twk;
using phxtool::Song;
using phxtool::SongCell;
using phxtool::SongInstrument;
using phxtool::Wave;

// Piano-key layout (a tracker's): the key's semitone above the current octave's C, or -1.
int piano_key(int k) {
    static const char* lower = "zsxdcvgbhnjm,l.;/";
    static const char* upper = "q2w3er5t6y7ui9o0p";
    for (int i = 0; lower[i]; ++i) if (k == lower[i]) return i;
    for (int i = 0; upper[i]; ++i) if (k == upper[i]) return 12 + i;
    return -1;
}

class SongView final : public DocView {
public:
    Song song;
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
        if (!phxtool::song_from_json(text, song, err)) return false;
        song.normalise();
        disk_stamp = file_stamp(path);
        dirty_ = false;
        clamp();
        return true;
    }

    FileKind kind() const override { return FileKind::Song; }
    int icon() const override { return kIconFileSound; }
    bool dirty() const override { return dirty_; }
    bool save(Host& h, std::string* err) override {
        const std::string t = phxtool::song_to_json(song);
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
        SongView fresh;
        if (!fresh.load(path, err)) return false;
        song = fresh.song; dirty_ = false; disk_stamp = fresh.disk_stamp;
        clamp();
        return true;
    }
    bool undo() override {
        if (undo_.empty()) return false;
        redo_.push_back(song); song = undo_.back(); undo_.pop_back(); dirty_ = true; clamp();
        return true;
    }
    bool redo() override {
        if (redo_.empty()) return false;
        undo_.push_back(song); song = redo_.back(); redo_.pop_back(); dirty_ = true; clamp();
        return true;
    }
    std::string status() const override {
        const int m = int(song.seconds()) / 60, s = int(song.seconds()) % 60;
        std::string st = fmt("%d bpm  %d channels  %d patterns  %d rows  %d:%02d", song.bpm, song.channels,
                             int(song.patterns.size()), song.total_rows(), m, s);
        if (const SongCell* c = cell()) st += "   cell: " + (c->note < 0 && c->note != phxtool::kCellOff ? std::string("empty") : phxtool::cell_text(song, *c));
        return st;
    }
    void release(Host& h) override { if (play_mode_) h.stop_pcm(); }
    std::vector<Action> actions() override {
        return { { "Play song", "Space", [this] { want_play_ = 1; } },
                 { "New pattern", "", [this] { edit([&] { pat_ = song.add_pattern(16); }); } },
                 { "Add pattern to the order", "", [this] { edit([&] { song.order.push_back(pat_); }); } } };
    }

    void draw(Host& h, const Rect& area) override {
        Gui& g = h.gui();
        if (!id_) id_ = Gui::hash(path.c_str());
        g.push_id(id_);
        clamp();
        update_playhead(h);
        Rect r = area;
        g.rect(r, g.th.bg, kSubBg);
        const Rect bar = cut_top(r, 18);
        Rect left = cut_left(r, 118);
        Rect right = cut_right(r, std::min(230, std::max(190, area.w / 4)));
        draw_bar(h, bar);
        draw_patterns(h, left);
        draw_instruments(h, right);
        draw_grid(h, r);
        keys(h);
        if (want_play_) { const int m = want_play_; want_play_ = 0; start(h, m); }
        g.pop_id();
    }

private:
    uint32_t id_ = 0;
    std::vector<Song> undo_, redo_;
    int pat_ = 0, row_ = 0, ch_ = 0, top_ = 0, left_ch_ = 0;
    int inst_ = 0, octave_ = 4, step_ = 1;
    int order_sel_ = -1;
    int want_play_ = 0;                       // 1 song, 2 pattern
    int play_mode_ = 0;                       // what is playing (0: nothing)
    bool loop_ = true, follow_ = true;
    Song play_song_;                          // the song as it was when play started
    uint64_t play_t0_ = 0;
    double play_len_ = 0;
    int play_order_ = -1, play_row_ = -1;     // the row playing now (in play_song_)
    int pat_scroll_ = 0, order_scroll_ = 0, inst_scroll_ = 0;

    template <class F> void edit(F f) {
        undo_.push_back(song);
        if (undo_.size() > 100) undo_.erase(undo_.begin());
        redo_.clear();
        f();
        song.normalise();
        dirty_ = true;
        clamp();
    }
    void clamp() {
        if (song.patterns.empty()) { song.add_pattern(16); }
        pat_ = std::max(0, std::min(pat_, int(song.patterns.size()) - 1));
        const int rows = int(song.patterns[size_t(pat_)].rows.size());
        row_ = std::max(0, std::min(row_, rows - 1));
        ch_ = std::max(0, std::min(ch_, song.channels - 1));
        inst_ = std::max(0, std::min(inst_, int(song.instruments.size()) - 1));
        octave_ = std::max(0, std::min(octave_, 7));
        order_sel_ = std::min(order_sel_, int(song.order.size()) - 1);
    }
    SongCell* cell() {
        if (pat_ < 0 || pat_ >= int(song.patterns.size())) return nullptr;
        auto& rows = song.patterns[size_t(pat_)].rows;
        if (row_ < 0 || row_ >= int(rows.size()) || ch_ < 0 || ch_ >= int(rows[size_t(row_)].size())) return nullptr;
        return &rows[size_t(row_)][size_t(ch_)];
    }
    const SongCell* cell() const { return const_cast<SongView*>(this)->cell(); }

    // ---- playback ----
    void start(Host& h, int mode) {
        if (play_mode_) { h.stop_pcm(); play_mode_ = 0; return; }        // the play buttons toggle
        play_song_ = song;
        if (mode == 2) play_song_.order = { pat_ };
        const std::vector<int16_t> pcm = phxtool::render_song(play_song_);
        if (pcm.empty()) { h.toast("nothing to play (the order list is empty)", Toast::Warn); return; }
        if (!h.play_pcm(pcm, phxtool::kSynthRate, loop_)) { h.toast("no audio device", Toast::Warn); return; }
        play_mode_ = mode;
        play_t0_ = h.ticks();
        play_len_ = double(pcm.size()) / phxtool::kSynthRate;
    }
    void update_playhead(Host& h) {
        play_order_ = play_row_ = -1;
        if (!play_mode_) return;
        double t = double(h.ticks() - play_t0_) / 60.0;
        if (t >= play_len_) {
            if (!loop_) { play_mode_ = 0; return; }
            t = std::fmod(t, play_len_);
        }
        if (!play_song_.position_at(t, play_order_, play_row_)) { play_order_ = play_row_ = -1; return; }
        const int playing_pat = play_song_.order[size_t(play_order_)];
        if (follow_ && play_mode_ == 1 && playing_pat != pat_ && playing_pat < int(song.patterns.size())) pat_ = playing_pat;
    }
    void preview_note(Host& h, int note) {
        if (inst_ < 0 || inst_ >= int(song.instruments.size())) return;
        if (play_mode_) play_mode_ = 0;
        h.play_pcm(phxtool::render_note(song.instruments[size_t(inst_)], note), phxtool::kSynthRate);
    }

    // ---- keyboard ----
    void keys(Host& h) {
        Gui& g = h.gui();
        if (g.text_focus()) return;
        const int rows = int(song.patterns[size_t(pat_)].rows.size());
        const int page = std::max(1, song.rows_per_beat * 4);
        if (g.key(PHX_KEY_UP)) row_ = std::max(0, row_ - 1);
        if (g.key(PHX_KEY_DOWN)) row_ = std::min(rows - 1, row_ + 1);
        if (g.key(PHX_KEY_LEFT)) ch_ = std::max(0, ch_ - 1);
        if (g.key(PHX_KEY_RIGHT)) ch_ = std::min(song.channels - 1, ch_ + 1);
        if (g.key(PHX_KEY_PAGE_UP)) row_ = std::max(0, row_ - page);
        if (g.key(PHX_KEY_PAGE_DOWN)) row_ = std::min(rows - 1, row_ + page);
        if (g.key(PHX_KEY_HOME)) row_ = 0;
        if (g.key(PHX_KEY_END)) row_ = rows - 1;
        if (g.hotkey(' ')) want_play_ = 1;
        if (g.hotkey('f')) octave_ = std::min(7, octave_ + 1);
        if (g.hotkey('f', kShift)) octave_ = std::max(0, octave_ - 1);
        if (g.hotkey(PHX_KEY_DELETE) || g.hotkey(PHX_KEY_BACKSPACE)) { edit([&] { *cell() = SongCell{}; }); advance(); }
        if (g.hotkey('1')) { edit([&] { *cell() = SongCell{ phxtool::kCellOff, -1 }; }); advance(); }
        for (const char* k = "zsxdcvgbhnjm,l.;/q2w3er5t6y7ui9o0p"; *k; ++k) {
            if (!g.hotkey(*k)) continue;
            const int note = std::min(phxtool::kMaxNote, octave_ * 12 + piano_key(*k));
            if (song.instruments.empty()) { h.toast("add an instrument first", Toast::Warn); continue; }
            edit([&] { *cell() = SongCell{ note, inst_ }; });
            preview_note(h, note);
            advance();
        }
    }
    void advance() {
        const int rows = int(song.patterns[size_t(pat_)].rows.size());
        row_ = std::min(rows - 1, row_ + step_);
    }

    // ---- panels ----
    void draw_bar(Host& h, const Rect& bar) {
        Gui& g = h.gui();
        g.rect(bar, g.th.panel, kSubFill);
        Gui::Row row(Rect{ bar.x + 4, bar.y + 3, bar.w - 8, 12 }, 4);
        Btn sb; sb.icon = play_mode_ == 1 ? kIconStop : kIconPlay; sb.help = "Play the song: the order list, as the bake renders it (Space)";
        if (g.button(row.take(48), "song", sb)) want_play_ = 1;
        Btn pb; pb.icon = play_mode_ == 2 ? kIconStop : kIconPlay; pb.help = "Play this pattern alone";
        if (g.button(row.take(62), "pattern", pb)) want_play_ = 2;
        g.checkbox(row.take(40), "loop", loop_, "Loop playback (the game loops music too)");
        g.checkbox(row.take(52), "follow", follow_, "Show the pattern that is playing");
        auto num = [&](const char* label, int& v, int lo, int hi, const char* help) {
            const Rect lr = row.take(Gui::text_w(label) + 2);
            g.text(lr.x, lr.y + 2, label, g.th.dim);
            int t = v;
            if (g.int_field(g.id(label), row.take(28), t, lo, hi, 1, help) && t != v) return t;
            return v;
        };
        const int bpm = num("bpm", song.bpm, 20, 400, "Tempo (beats per minute)");
        if (bpm != song.bpm) edit([&] { song.bpm = bpm; });
        const int rpb = num("rpb", song.rows_per_beat, 1, 16, "Rows per beat (4 = sixteenth notes)");
        if (rpb != song.rows_per_beat) edit([&] { song.rows_per_beat = rpb; });
        const int chn = num("ch", song.channels, 1, Song::kMaxChannels, "Channels (voices that sound at once)");
        if (chn != song.channels) edit([&] { song.set_channels(chn); });
        octave_ = num("oct", octave_, 0, 7, "Octave for the piano keys (F / Shift+F)");
        step_ = num("step", step_, 0, 16, "Rows the cursor moves after entering a note");
        g.text(row.x + 4, bar.y + 5, fmt("%.1f s -> music \"%s\"", song.seconds(), stem_of(path).c_str()),
               g.th.faint, kSubText, row.rest().w - 4);
    }

    void draw_patterns(Host& h, Rect col) {
        Gui& g = h.gui();
        {
            Rect b = panel_section(g, col, std::max(110, col.h / 2), "PATTERNS");
            const int n = int(song.patterns.size());
            const int rows = std::max(2, std::min(n, (b.h - 60) / 11));
            const Rect list{ b.x, b.y, b.w, rows * 11 + 2 };
            g.rect(list, g.th.field, kSubWidget);
            g.wheel_scroll(list, pat_scroll_, n, rows, 1);
            for (int i = 0; i < rows && pat_scroll_ + i < n; ++i) {
                const int k = pat_scroll_ + i;
                const Rect rr{ list.x + 1, list.y + 1 + i * 11, list.w - 2, 11 };
                if (k == pat_) g.rect(rr, g.th.sel, kSubImage);
                else if (g.hover(rr)) g.rect(rr, g.th.hover, kSubImage);
                g.text(rr.x + 3, rr.y + 2, song.patterns[size_t(k)].name, g.th.text, kSubText, rr.w - 30);
                g.text(rr.right() - 26, rr.y + 2, fmt("%d", int(song.patterns[size_t(k)].rows.size())), g.th.faint);
                if (g.clicked(rr)) { pat_ = k; row_ = 0; top_ = 0; }
            }
            int y = list.bottom() + 3;
            Gui::Row row(Rect{ b.x, y, b.w, 12 }, 3);
            if (g.button(row.take(24), "+", Btn{ false, true, false, 0, "New pattern" })) edit([&] { pat_ = song.add_pattern(16); });
            if (g.button(row.take(30), "dup", Btn{ false, true, false, 0, "Duplicate this pattern" })) edit([&] { pat_ = song.duplicate_pattern(pat_); });
            if (g.button(row.take(30), "del", Btn{ false, n > 1, false, 0, "Delete this pattern (and its places in the order)" }) && n > 1)
                edit([&] { song.remove_pattern(pat_); });
            y += 16;
            std::string nm = song.patterns[size_t(pat_)].name;
            if (g.text_field(g.id("pname"), Rect{ b.x, y, b.w - 40, 12 }, nm, "name", 0, "The pattern's name (unique)")) {
                if (nm != song.patterns[size_t(pat_)].name) {
                    Song t = song;
                    if (t.rename_pattern(pat_, nm)) edit([&] { song.rename_pattern(pat_, nm); });
                    else h.toast("pattern names must be unique and non-empty", Toast::Warn);
                }
            }
            int rows_n = int(song.patterns[size_t(pat_)].rows.size());
            if (g.int_field(g.id("prows"), Rect{ b.right() - 36, y, 36, 12 }, rows_n, 1, Song::kMaxRows, 1, "Rows in this pattern"))
                edit([&] { song.resize_pattern(pat_, rows_n); });
        }
        {
            Rect b = panel_section(g, col, std::max(90, col.h), "ORDER");
            const int n = int(song.order.size());
            const int rows = std::max(2, std::min(std::max(n, 2), (b.h - 20) / 11));
            const Rect list{ b.x, b.y, b.w, rows * 11 + 2 };
            g.rect(list, g.th.field, kSubWidget);
            g.wheel_scroll(list, order_scroll_, n, rows, 1);
            for (int i = 0; i < rows && order_scroll_ + i < n; ++i) {
                const int k = order_scroll_ + i;
                const int pi = song.order[size_t(k)];
                const Rect rr{ list.x + 1, list.y + 1 + i * 11, list.w - 2, 11 };
                if (k == order_sel_) g.rect(rr, g.th.sel, kSubImage);
                else if (g.hover(rr)) g.rect(rr, g.th.hover, kSubImage);
                if (k == play_order_ && play_mode_ == 1) g.rect(Rect{ rr.x, rr.y, 2, rr.h }, g.th.accent, kSubOver);
                g.text(rr.x + 5, rr.y + 2, fmt("%02d", k), g.th.faint);
                g.text(rr.x + 22, rr.y + 2, pi >= 0 && pi < int(song.patterns.size()) ? song.patterns[size_t(pi)].name : "?",
                       g.th.text, kSubText, rr.w - 24);
                if (g.clicked(rr)) { order_sel_ = k; if (pi >= 0 && pi < int(song.patterns.size())) { pat_ = pi; row_ = 0; top_ = 0; } }
            }
            if (!n) g.text(list.x + 3, list.y + 3, "empty: plays nothing", g.th.warn, kSubText, list.w - 6);
            Gui::Row row(Rect{ b.x, list.bottom() + 3, b.w, 12 }, 3);
            if (g.button(row.take(30), "+", Btn{ false, true, false, 0, "Append the selected pattern to the order" }))
                edit([&] { song.order.push_back(pat_); order_sel_ = int(song.order.size()) - 1; });
            const bool have = order_sel_ >= 0 && order_sel_ < n;
            if (g.button(row.take(30), "del", Btn{ false, have, false, 0, "Remove this entry from the order" }) && have)
                edit([&] { song.order.erase(song.order.begin() + order_sel_); });
            if (g.icon_button(row.take(12), kIconUp, "Move earlier", false, have && order_sel_ > 0) && have && order_sel_ > 0)
                edit([&] { std::swap(song.order[size_t(order_sel_)], song.order[size_t(order_sel_ - 1)]); --order_sel_; });
            if (g.icon_button(row.take(12), kIconDown, "Move later", false, have && order_sel_ + 1 < n) && have && order_sel_ + 1 < n)
                edit([&] { std::swap(song.order[size_t(order_sel_)], song.order[size_t(order_sel_ + 1)]); ++order_sel_; });
        }
    }

    void draw_instruments(Host& h, Rect col) {
        Gui& g = h.gui();
        Rect b = panel_section(g, col, std::max(160, col.h), "INSTRUMENTS");
        const int n = int(song.instruments.size());
        const int rows = std::max(2, std::min(n, 5));
        const Rect list{ b.x, b.y, b.w, rows * 11 + 2 };
        g.rect(list, g.th.field, kSubWidget);
        g.wheel_scroll(list, inst_scroll_, n, rows, 1);
        for (int i = 0; i < rows && inst_scroll_ + i < n; ++i) {
            const int k = inst_scroll_ + i;
            const SongInstrument& in = song.instruments[size_t(k)];
            const Rect rr{ list.x + 1, list.y + 1 + i * 11, list.w - 2, 11 };
            if (k == inst_) g.rect(rr, g.th.sel, kSubImage);
            else if (g.hover(rr)) g.rect(rr, g.th.hover, kSubImage);
            g.text(rr.x + 3, rr.y + 2, fmt("%X", unsigned(k)), g.th.faint);
            g.text(rr.x + 14, rr.y + 2, in.name, g.th.text, kSubText, rr.w - 70);
            g.text(rr.right() - 52, rr.y + 2, phxtool::wave_name(in.wave), g.th.violet, kSubText, 50);
            if (g.clicked(rr)) { inst_ = k; preview_note(h, octave_ * 12); }
        }
        if (!n) g.text(list.x + 3, list.y + 3, "none: add one", g.th.warn);
        int y = list.bottom() + 3;
        {
            Gui::Row row(Rect{ b.x, y, b.w, 12 }, 3);
            if (g.button(row.take(24), "+", Btn{ false, true, false, 0, "New instrument" }))
                edit([&] { SongInstrument in; in.name = "inst"; inst_ = song.add_instrument(in); });
            if (g.button(row.take(30), "del", Btn{ false, n > 0, false, 0, "Delete this instrument (its notes use the channel's previous one)" }) && n > 0)
                edit([&] { song.remove_instrument(inst_); });
            if (g.button(row.take(44), "hear", Btn{ false, n > 0, false, 0, "Play a note with it (at the octave)" }) && n > 0)
                preview_note(h, octave_ * 12);
        }
        y += 16;
        if (inst_ < 0 || inst_ >= n) return;
        SongInstrument in = song.instruments[size_t(inst_)];
        std::string nm = in.name;
        g.text(b.x, y + 2, "name", g.th.dim);
        if (g.text_field(g.id("iname"), Rect{ b.x + 52, y, b.w - 52, 12 }, nm, "name", 0, "Cells name it: \"C-4 lead\"")) {
            Song t = song;
            if (nm != in.name) {
                if (t.rename_instrument(inst_, nm)) edit([&] { song.rename_instrument(inst_, nm); });
                else h.toast("instrument names must be unique, without spaces", Toast::Warn);
            }
        }
        y += 15;
        g.text(b.x, y + 2, "wave", g.th.dim);
        int w = int(in.wave);
        if (g.dropdown(g.id("iwave"), Rect{ b.x + 52, y, b.w - 52, 12 }, phxtool::wave_names(), w, "The oscillator (noise: drums, hats)"))
            edit([&] { song.instruments[size_t(inst_)].wave = Wave(w); });
        y += 15;
        for (const phxtool::InstField& f : phxtool::inst_fields()) {
            if (y + 12 > b.bottom()) break;
            g.text(b.x, y + 2, f.name, g.th.dim, kSubText, 50);
            g.tip(Rect{ b.x, y, 50, 12 }, f.help);
            double v = in.*(f.ptr);
            if (g.float_field(g.id(f.name), Rect{ b.x + 52, y, b.w - 52, 12 }, v, f.lo, f.hi, (f.hi - f.lo) / 100.0, f.help) &&
                v != in.*(f.ptr))
                edit([&] { song.instruments[size_t(inst_)].*(f.ptr) = v; });
            y += 14;
        }
    }

    void draw_grid(Host& h, Rect r) {
        Gui& g = h.gui();
        r = r.inset(4);
        const auto& rows = song.patterns[size_t(pat_)].rows;
        const int nrows = int(rows.size());
        const int rh = 11, head = 13, num_w = 24;
        // channels share the width (56..90 px each); when they don't fit, the view scrolls by
        // channel to keep the cursor's in sight
        const int cw = std::max(56, std::min(90, (r.w - num_w) / std::max(1, song.channels)));
        const int vis_ch = std::max(1, std::min(song.channels, (r.w - num_w) / cw));
        if (ch_ < left_ch_) left_ch_ = ch_;
        if (ch_ >= left_ch_ + vis_ch) left_ch_ = ch_ - vis_ch + 1;
        left_ch_ = std::max(0, std::min(left_ch_, song.channels - vis_ch));
        g.rect(r, g.th.field, kSubWidget);
        g.push_clip(r);
        // header: the pattern and its channels
        g.text(r.x + 2, r.y + 3, song.patterns[size_t(pat_)].name, g.th.accent, kSubText, num_w - 2);
        for (int v = 0; v < vis_ch; ++v) {
            const int c = left_ch_ + v;
            g.text(r.x + num_w + v * cw + 3, r.y + 3, fmt("ch %d", c + 1), c == ch_ ? g.th.text : g.th.dim);
        }
        if (vis_ch < song.channels)
            g.text(r.right() - 30, r.y + 3, fmt("%d/%d", left_ch_ + vis_ch, song.channels), g.th.faint);
        g.rect(Rect{ r.x, r.y + head - 1, r.w, 1 }, g.th.line, kSubWidget);
        const Rect body{ r.x, r.y + head, r.w, r.h - head };
        const int vis = std::max(1, body.h / rh);
        // keep the cursor (or, while following, the playhead) in view
        const bool showing_play = play_row_ >= 0 && play_order_ >= 0 && play_order_ < int(play_song_.order.size()) &&
                                  play_song_.order[size_t(play_order_)] == pat_;
        const int focus_row = showing_play && follow_ ? play_row_ : row_;
        if (focus_row < top_) top_ = focus_row;
        if (focus_row >= top_ + vis) top_ = focus_row - vis + 1;
        if (const int wv = g.wheel(body)) top_ -= wv * 3;
        top_ = std::max(0, std::min(top_, std::max(0, nrows - vis)));
        for (int i = 0; i < vis && top_ + i < nrows; ++i) {
            const int rr = top_ + i;
            const int y = body.y + i * rh;
            const bool beat = song.rows_per_beat > 0 && rr % song.rows_per_beat == 0;
            if (beat) g.rect(Rect{ body.x, y, body.w, rh }, g.th.panel, kSubImage);
            if (showing_play && rr == play_row_) g.rect(Rect{ body.x, y, body.w, rh }, g.th.panel2, kSubImage);
            g.text(body.x + 3, y + 2, fmt("%02d", rr), beat ? g.th.dim : g.th.faint);
            for (int v = 0; v < vis_ch; ++v) {
                const int c = left_ch_ + v;
                if (c >= int(rows[size_t(rr)].size())) break;
                const Rect cr{ body.x + num_w + v * cw, y, cw - 2, rh };
                const bool cur = rr == row_ && c == ch_;
                if (cur) g.rect(cr, g.th.sel, kSubImage);
                const SongCell& cl = rows[size_t(rr)][size_t(c)];
                if (cl.note == phxtool::kCellOff) g.text(cr.x + 3, y + 2, "off", g.th.warn);
                else if (cl.note >= 0) {
                    g.text(cr.x + 3, y + 2, phxtool::note_text(cl.note), g.th.text);
                    if (cl.inst >= 0 && cl.inst < int(song.instruments.size()))
                        g.text(cr.x + 24, y + 2, song.instruments[size_t(cl.inst)].name, g.th.violet, kSubText, cr.w - 26);
                } else g.text(cr.x + 3, y + 2, "...", g.th.panel2);
                if (g.clicked(cr)) { row_ = rr; ch_ = c; g.blur(); }
            }
        }
        g.pop_clip();
        g.tip(body, "Piano keys enter notes (Z.. and Q.. rows), 1 = off, Delete clears; F / Shift+F octave");
    }
};

} // namespace

std::unique_ptr<DocView> make_song_view(Host& h, const std::string& path, std::string* err) {
    (void)h;
    std::unique_ptr<SongView> v(new SongView);
    if (!v->load(path, err)) return nullptr;
    return v;
}

} // namespace phxstudio
