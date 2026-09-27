// tools/phxstudio/ed_sfx.cpp — the SOUND EFFECT editor panel: an sfxr-style generator over a `.sfx`
// document (tools/phxpack/synth.h). Pick a preset (pickup, laser, explosion, powerup, hurt, jump,
// blip), randomize or mutate it, and shape it with the parameters; every change re-renders and
// plays the exact PCM the bake will produce (phxsnd / bake_project.py turn a .sfx into a Sound
// asset named after the file, so assets/coin.sfx is res->sound("coin"_hash)).
//
//   Space   play              R   randomize             M   mutate
//   1..5    wave (square, saw, triangle, sine, noise)
#include "host.h"
#include "synth.h"            // tools/phxpack

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace phxstudio {
namespace {

using namespace twk;
using phxtool::SfxParams;
using phxtool::Wave;

class SfxView final : public DocView {
public:
    SfxParams p;
    bool dirty_ = false;

    bool load(const std::string& path_, std::string* err) {
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
        if (!phxtool::sfx_from_json(text, p, err)) return false;
        disk_stamp = file_stamp(path);
        dirty_ = false;
        return true;
    }

    FileKind kind() const override { return FileKind::Sfx; }
    int icon() const override { return kIconFileSound; }
    bool dirty() const override { return dirty_; }
    bool save(Host& h, std::string* err) override {
        const std::string t = phxtool::sfx_to_json(p);
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
        SfxView fresh;
        if (!fresh.load(path, err)) return false;
        p = fresh.p; dirty_ = false; disk_stamp = fresh.disk_stamp;
        return true;
    }
    bool undo() override {
        if (undo_.empty()) return false;
        redo_.push_back(p); p = undo_.back(); undo_.pop_back(); dirty_ = true; want_play_ = true;
        return true;
    }
    bool redo() override {
        if (redo_.empty()) return false;
        undo_.push_back(p); p = redo_.back(); redo_.pop_back(); dirty_ = true; want_play_ = true;
        return true;
    }
    std::string status() const override {
        return fmt("%s  %.2f s  %u frames @ %u Hz", phxtool::wave_name(p.wave), double(pcm_.size()) / phxtool::kSynthRate,
                   unsigned(pcm_.size()), unsigned(phxtool::kSynthRate));
    }
    void release(Host& h) override { if (playing_) h.stop_pcm(); }
    std::vector<Action> actions() override {
        return { { "Play", "Space", [this] { want_play_ = true; } },
                 { "Randomize", "R", [this] { want_random_ = true; } },
                 { "Mutate", "M", [this] { want_mutate_ = true; } } };
    }

    void draw(Host& h, const Rect& area) override {
        Gui& g = h.gui();
        if (!id_) id_ = Gui::hash(path.c_str());
        g.push_id(id_);
        rerender();
        Rect r = area;
        g.rect(r, g.th.bg, kSubBg);
        const Rect bar = cut_top(r, 18);
        Rect left = cut_left(r, 96);
        Rect right = cut_right(r, std::min(300, std::max(220, area.w * 2 / 5)));
        draw_bar(h, bar);
        draw_presets(h, left);
        draw_params(h, right);
        draw_wave(h, r);
        keys(h);
        if (want_random_) { want_random_ = false; edit(phxtool::sfx_random(next_seed(h))); }
        if (want_mutate_) { want_mutate_ = false; edit(phxtool::sfx_mutate(p, next_seed(h))); }
        rerender();
        if (want_play_ && g.active() == 0) { want_play_ = false; play(h); }
        if (playing_ && double(h.ticks() - play_t0_) / 60.0 > double(pcm_.size()) / phxtool::kSynthRate) playing_ = false;
        g.pop_id();
    }

private:
    uint32_t id_ = 0;
    std::vector<SfxParams> undo_, redo_;
    std::vector<int16_t> pcm_;
    SfxParams rendered_{};
    bool have_render_ = false;
    bool want_play_ = false, want_random_ = false, want_mutate_ = false;
    bool playing_ = false;
    uint64_t play_t0_ = 0;
    uint32_t seed_ = 1;
    uint32_t drag_field_ = 0;                // the slider being dragged (one undo step per drag)

    uint32_t next_seed(Host& h) { seed_ = seed_ * 1664525u + 1013904223u + uint32_t(h.ticks()); return seed_; }
    void push_undo() {
        undo_.push_back(p);
        if (undo_.size() > 200) undo_.erase(undo_.begin());
        redo_.clear();
        dirty_ = true;
    }
    void edit(const SfxParams& q) {
        if (q == p) return;
        push_undo();
        p = q;
        want_play_ = true;
    }
    void rerender() {
        if (have_render_ && rendered_ == p) return;
        pcm_ = phxtool::render_sfx(p);
        rendered_ = p;
        have_render_ = true;
    }
    void play(Host& h) {
        rerender();
        playing_ = h.play_pcm(pcm_, phxtool::kSynthRate);
        play_t0_ = h.ticks();
    }

    void keys(Host& h) {
        Gui& g = h.gui();
        if (g.hotkey(' ')) want_play_ = true;
        if (g.hotkey('r')) want_random_ = true;
        if (g.hotkey('m')) want_mutate_ = true;
        for (int k = 0; k < 5; ++k)
            if (g.hotkey('1' + k)) { SfxParams q = p; q.wave = Wave(k); edit(q); }
    }

    void draw_bar(Host& h, const Rect& bar) {
        Gui& g = h.gui();
        g.rect(bar, g.th.panel, kSubFill);
        Gui::Row row(Rect{ bar.x + 4, bar.y + 3, bar.w - 8, 12 }, 4);
        Btn pb; pb.icon = playing_ ? kIconStop : kIconPlay; pb.help = "Play the sound the bake will produce (Space)";
        if (g.button(row.take(52), playing_ ? "stop" : "play", pb)) {
            if (playing_) { h.stop_pcm(); playing_ = false; } else want_play_ = true;
        }
        if (g.button(row.take(66), "randomize", Btn{ false, true, false, 0, "A new random sound (R)" })) want_random_ = true;
        if (g.button(row.take(50), "mutate", Btn{ false, true, false, 0, "A small variation of this one (M)" })) want_mutate_ = true;
        g.text(row.x + 6, bar.y + 5, fmt("%.2f s  -> res->sound(\"%s\"_hash)", p.length(), stem_of(path).c_str()),
               g.th.faint, kSubText, row.rest().w);
    }

    void draw_presets(Host& h, Rect col) {
        Gui& g = h.gui();
        Rect b = panel_section(g, col, std::max(120, col.h), "PRESETS");
        int y = b.y;
        for (const std::string& n : phxtool::sfx_preset_names()) {
            if (g.button(Rect{ b.x, y, b.w, 13 }, n, Btn{ false, true, false, 0, "A new sound of this kind (each click varies it)" }))
                edit(phxtool::sfx_preset(n, next_seed(h)));
            y += 16;
        }
        y += 6;
        g.text(b.x, y, "WAVE", g.th.dim);
        y += 12;
        for (int k = 0; k < 5; ++k) {
            if (g.button(Rect{ b.x, y, b.w, 13 }, phxtool::wave_names()[size_t(k)], Btn{ p.wave == Wave(k), true, false, 0, "The oscillator (1..5)" })) {
                SfxParams q = p; q.wave = Wave(k); edit(q);
            }
            y += 15;
        }
    }

    void draw_params(Host& h, Rect col) {
        Gui& g = h.gui();
        Rect b = panel_section(g, col, std::max(120, col.h), "PARAMETERS");
        int y = b.y;
        const int label_w = 62, value_w = 50;
        const int slider_w = std::max(20, b.w - label_w - value_w - 6);
        bool dragging_any = false;
        for (size_t i = 0; i < phxtool::sfx_fields().size(); ++i) {
            const phxtool::SfxField& f = phxtool::sfx_fields()[i];
            if (y + 12 > b.bottom()) break;
            const bool off = (std::string(f.name) == "duty" || std::string(f.name) == "duty_sweep") && p.wave != Wave::Square;
            g.text(b.x, y + 2, f.name, off ? g.th.faint : g.th.dim, kSubText, label_w - 2);
            g.tip(Rect{ b.x, y, label_w, 12 }, f.help);
            double v = p.*(f.ptr);
            // slider over [lo, hi] in 1000 steps
            const uint32_t sid = g.id(f.name);
            int iv = int(std::lround((v - f.lo) / (f.hi - f.lo) * 1000.0));
            iv = std::max(0, std::min(1000, iv));
            const Rect sr{ b.x + label_w, y + 1, slider_w, 10 };
            if (g.slider(sid, sr, iv, 0, 1000, off ? g.th.panel2 : g.th.violet, f.help)) {
                if (drag_field_ != sid) { push_undo(); drag_field_ = sid; }
                p.*(f.ptr) = phxtool::synth_tidy(f.lo + (f.hi - f.lo) * iv / 1000.0);
                dirty_ = true;
            }
            if (g.dragging(sid)) dragging_any = true;
            double fv = v;
            if (g.float_field(g.id(f.name, 1), Rect{ b.right() - value_w, y, value_w, 12 }, fv, f.lo, f.hi,
                              (f.hi - f.lo) / 100.0, f.help) && fv != v) {
                push_undo();
                p.*(f.ptr) = fv;
                want_play_ = true;
            }
            y += 14;
        }
        if (!dragging_any && drag_field_) { drag_field_ = 0; want_play_ = true; }   // a drag ended: hear it
    }

    void draw_wave(Host& h, Rect r) {
        Gui& g = h.gui();
        r = r.inset(6);
        g.rect(r, g.th.bar, kSubWidget);
        g.rect(Rect{ r.x, r.y + r.h / 2, r.w, 1 }, g.th.line, kSubWidget);
        if (pcm_.empty() || r.w <= 0) { g.text(r.x + 6, r.y + 6, "silent (zero length)", g.th.warn); return; }
        const int half = r.h / 2 - 2;
        const double secs = double(pcm_.size()) / phxtool::kSynthRate;
        const int play_col = playing_ ? int(double(h.ticks() - play_t0_) / 60.0 / secs * r.w) : -1;
        for (int c = 0; c < r.w; ++c) {
            const size_t a = size_t(uint64_t(c) * pcm_.size() / uint64_t(r.w));
            const size_t e = std::max(a + 1, size_t(uint64_t(c + 1) * pcm_.size() / uint64_t(r.w)));
            int mn = 0, mx = 0;
            for (size_t i = a; i < e && i < pcm_.size(); ++i) { mn = std::min(mn, int(pcm_[i])); mx = std::max(mx, int(pcm_[i])); }
            const int top = r.y + r.h / 2 - mx * half / 32768;
            const int bot = r.y + r.h / 2 - mn * half / 32768;
            g.rect(Rect{ r.x + c, top, 1, std::max(1, bot - top + 1) }, c < play_col ? g.th.accent : g.th.violet, kSubImage);
        }
        if (play_col >= 0 && play_col < r.w) g.rect(Rect{ r.x + play_col, r.y, 1, r.h }, g.th.text, kSubOver);
        g.text(r.x + 4, r.bottom() - 11, fmt("%.3f s", secs), g.th.faint);
        if (g.hover(r)) g.hint = fmt("t = %.3f s", double(g.mx() - r.x) / r.w * secs);
        if (g.clicked(r)) want_play_ = true;
    }
};

} // namespace

std::unique_ptr<DocView> make_sfx_view(Host& h, const std::string& path, std::string* err) {
    (void)h;
    std::unique_ptr<SfxView> v(new SfxView);
    if (!v->load(path, err)) return nullptr;
    return v;
}

} // namespace phxstudio
