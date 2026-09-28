// phx/runtime/dialogue.cpp — the conversation runner (phx/runtime/dialogue.h). Integer-only state
// (ticks, indices), so both scalar tiers step identically; the drawing goes through phx::UI.
#include "phx/runtime/dialogue.h"
#include "phx/core/log.h"

#include <cstring>

namespace phx {

bool DialogueRunner::load(Renderer& r, ResourceCache& res, NameHash name) {
    node_ = kDlgEnd;
    data_ = DialogueData{};
    if (!res.has(name, AssetType::Dialogue)) return false;
    auto d = res.dialogue(name);
    if (!d) { PHX_LOG_ERROR("dialogue: asset %08x is malformed", unsigned(name)); return false; }
    data_ = d.unwrap();
    for (uint32_t i = 0; i < kMaxSpeakers; ++i) { portraits_[i] = kNoTexture; portrait_w_[i] = portrait_h_[i] = 0; }
    for (uint32_t i = 0; i < data_.speaker_count && i < kMaxSpeakers; ++i) {
        const NameHash p = data_.speakers[i].portrait;
        if (!p || !res.has(p, AssetType::Texture)) continue;
        auto t = res.texture(p);
        if (!t) continue;
        const TextureView v = t.unwrap();
        TextureDesc desc{};
        desc.pixels = v.pixels; desc.width = v.width; desc.height = v.height; desc.format = v.format;
        portraits_[i] = r.load_texture(desc);
        portrait_w_[i] = uint8_t(v.width > 255 ? 255 : v.width);
        portrait_h_[i] = uint8_t(v.height > 255 ? 255 : v.height);
    }
    return true;
}

bool DialogueRunner::test(const DlgOp& op, const DialogueVars& vars) const {
    const int32_t v = vars.get(op.var), k = op.value;
    switch (op.op) {
    case kDlgEq: return v == k;
    case kDlgNe: return v != k;
    case kDlgLt: return v < k;
    case kDlgLe: return v <= k;
    case kDlgGt: return v > k;
    case kDlgGe: return v >= k;
    default:     return true;
    }
}

void DialogueRunner::apply(uint16_t first, uint8_t count, DialogueVars& vars) const {
    for (uint32_t i = 0; i < count; ++i) {
        const DlgOp& op = data_.ops[first + i];
        switch (op.op) {
        case kDlgSet: vars.set(op.var, op.value); break;
        case kDlgAdd: vars.set(op.var, vars.get(op.var) + op.value); break;
        case kDlgSub: vars.set(op.var, vars.get(op.var) - op.value); break;
        default: break;
        }
    }
}

void DialogueRunner::enter(uint16_t node, DialogueVars& vars) {
    uint32_t guard = uint32_t(data_.node_count) + 1;          // a cycle of skipped nodes ends it
    while (node != kDlgEnd && node < data_.node_count && guard--) {
        const DlgNodeDef& n = data_.nodes[node];
        if (n.cond.op != kDlgNone && !test(n.cond, vars)) { node = n.next; continue; }
        apply(n.first_op, n.op_count, vars);
        node_ = node; ticks_ = 0; ++shown_;
        nvis_ = 0; sel_ = 0;
        for (uint32_t i = 0; i < n.choice_count && nvis_ < kMaxChoices; ++i) {
            const DlgChoiceDef& c = data_.choices[n.first_choice + i];
            if (c.cond.op == kDlgNone || test(c.cond, vars)) vis_[nvis_++] = uint16_t(n.first_choice + i);
        }
        return;
    }
    node_ = kDlgEnd;
}

bool DialogueRunner::start(NameHash conversation, DialogueVars& vars) {
    const int32_t c = data_.find(conversation);
    node_ = kDlgEnd;
    shown_ = 0;
    if (c < 0) {
        if (data_.nodes) PHX_LOG_WARN("dialogue: no conversation %08x", unsigned(conversation));
        return false;
    }
    if (!data_.convs[c].node_count) return false;
    enter(data_.convs[c].first_node, vars);
    return active();
}

uint32_t DialogueRunner::text_len() const {
    const char* t = text();
    uint32_t n = 0;
    while (t[n]) ++n;
    return n;
}

bool DialogueRunner::revealed() const {
    if (!active() || !chars_per_sec) return true;
    return uint64_t(ticks_) * chars_per_sec >= uint64_t(text_len()) * 60u;
}

void DialogueRunner::update(const InputState& in, DialogueVars& vars) {
    if (!active()) return;
    if (ticks_ < 0x7FFFFFFFu) ++ticks_;
    if (!revealed()) {
        if (in.just(Button::A)) ticks_ = 0x7FFFFFFFu;          // show the rest of the line
        return;
    }
    if (nvis_) {
        if (in.just(Button::Down)) sel_ = (sel_ + 1) % nvis_;
        if (in.just(Button::Up))   sel_ = (sel_ + nvis_ - 1) % nvis_;
        if (in.just(Button::A)) {
            const DlgChoiceDef& c = data_.choices[vis_[sel_]];
            apply(c.first_op, c.op_count, vars);
            enter(c.next, vars);
        }
        return;
    }
    if (in.just(Button::A)) enter(data_.nodes[node_].next, vars);
}

const char* DialogueRunner::text() const { return active() ? data_.str(data_.nodes[node_].text) : ""; }

const char* DialogueRunner::speaker() const {
    if (!active()) return "";
    const uint16_t s = data_.nodes[node_].speaker;
    return s == kDlgNoSpeaker ? "" : data_.str(data_.speakers[s].name);
}

const char* DialogueRunner::choice(uint32_t i) const {
    return active() && i < nvis_ ? data_.str(data_.choices[vis_[i]].text) : "";
}

void DialogueRunner::render(UI& ui, const BitmapFont& font, int w, int h, vec2 origin) const {
    if (!active() || font.tex == kNoTexture) return;
    const int lh = font.line_h ? font.line_h : 8;
    const int bw = w - 8, bh = 3 * lh + 8;
    const int bx = 4, by = h - bh - 4;
    auto at = [&](int x, int y) { return vec2{ origin.x + s_from_int(x), origin.y + s_from_int(y) }; };

    // the speaker's name on a tab above the box
    const char* who = speaker();
    if (*who) {
        const int tw = UI::text_width(font, who) + 8;
        ui.rect(UIRect{ at(bx, by - lh - 3), vec2{ s_from_int(tw), s_from_int(lh + 3) } }, ui.panel_focus);
        ui.text(at(bx + 4, by - lh - 1), font, who, ui.label);
    }

    // the line, typed out (UI::dialogue wraps it and draws the portrait)
    DialogueView dv{};
    dv.lines = text(); dv.stride = 0; dv.count = 1;
    const uint16_t s = data_.nodes[node_].speaker;
    if (s != kDlgNoSpeaker && s < kMaxSpeakers && portraits_[s] != kNoTexture) {
        dv.portrait = portraits_[s]; dv.portrait_w = portrait_w_[s]; dv.portrait_h = portrait_h_[s];
    }
    int32_t q = 1 << 16;
    if (!revealed()) {
        const uint32_t len = text_len();
        q = len ? int32_t((uint64_t(ticks_) * chars_per_sec * 65536u) / (uint64_t(len) * 60u)) : (1 << 16);
        if (q > (1 << 16)) q = 1 << 16;
    }
    ui.dialogue(UIRect{ at(bx, by), vec2{ s_from_int(bw), s_from_int(bh) } }, font, dv, 0, s_from_q16(q));

    // the choices, once the line has shown: a panel above the box's right end
    if (revealed() && nvis_) {
        int cw = 0;
        for (uint32_t i = 0; i < nvis_; ++i) {
            const int tw = UI::text_width(font, choice(i));
            cw = tw > cw ? tw : cw;
        }
        cw += 14;
        if (cw > bw) cw = bw;
        const int ch = int(nvis_) * lh + 6;
        const int cx = bx + bw - cw, cy = by - ch - 2;
        ui.rect(UIRect{ at(cx, cy), vec2{ s_from_int(cw), s_from_int(ch) } }, ui.panel);
        for (uint32_t i = 0; i < nvis_; ++i) {
            const bool on = i == sel_;
            if (on) ui.text(at(cx + 3, cy + 3 + int(i) * lh), font, ">", rgba(255, 214, 110));
            ui.text(at(cx + 11, cy + 3 + int(i) * lh), font, choice(i), on ? rgba(255, 214, 110) : ui.label);
        }
    }
}

} // namespace phx
