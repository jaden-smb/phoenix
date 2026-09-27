// engine/runtime/src/devtools.cpp — see phx/runtime/devtools.h. Host-only (it reads the desktop
// event stream and draws into the software framebuffer): linked by desktop game builds.
#include "phx/runtime/devtools.h"
#include "phx/runtime/level.h"
#include "phx/ecs/reflect.h"
#include "phx/platform/desktop.h"
#include "phx/platform/gfx_soft.h"
#include "phx/platform/platform.h"

#include "dev_font.h"

#include <cstdio>
#include <cstring>

namespace phx {
namespace {

constexpr uint32_t kMaxList = 1024;

struct DevState {
    bool        shown  = false;
    bool        paused = false;
    int         step   = 0;                  // single steps requested while paused
    ecs::Entity sel    = ecs::kInvalid;
    uint64_t    frame  = 0;
};
DevState g_dev;

// The entities worth inspecting: every one with a Transform, in store order.
uint32_t list_entities(ecs::World& w, ecs::Entity* out, uint32_t cap) {
    uint32_t n = 0;
    w.each<Transform>([&](ecs::Entity e, Transform&) { if (n < cap) out[n++] = e; });
    return n;
}

void select_step(App& app, int dir) {
    static ecs::Entity list[kMaxList];
    const uint32_t n = list_entities(app.world(), list, kMaxList);
    if (!n) { g_dev.sel = ecs::kInvalid; return; }
    int at = -1;
    for (uint32_t i = 0; i < n; ++i) if (list[i] == g_dev.sel) at = int(i);
    at = at < 0 ? 0 : (at + dir + int(n)) % int(n);
    g_dev.sel = list[at];
}

// The entity under framebuffer pixel (x, y): its collider (or an 8 px box round its position).
void select_at(App& app, int x, int y) {
    const Camera2D& cam = app.render().camera();
    const scalar z = cam.zoom > s_from_int(0) ? cam.zoom : s_from_int(1);
    const scalar wx = cam.pos.x + s_from_int(x) / z, wy = cam.pos.y + s_from_int(y) / z;
    ecs::World& w = app.world();
    ecs::Entity best = ecs::kInvalid;
    w.each<Transform>([&](ecs::Entity e, Transform& t) {
        const AABBColl* c = w.get<AABBColl>(e);
        const scalar hx = c ? c->half.x : s_from_int(4), hy = c ? c->half.y : s_from_int(4);
        if (wx >= t.pos.x - hx && wx <= t.pos.x + hx && wy >= t.pos.y - hy && wy <= t.pos.y + hy) best = e;
    });
    g_dev.sel = best;
}

int dev_frame(void*, App& app, int steps) {
    ++g_dev.frame;
    phx_desktop_event ev;
    while (phx_desktop_poll(&ev)) {
        if (ev.kind == PHX_DEV_KEY_DOWN && !ev.repeat) {
            switch (ev.key) {
                case PHX_KEY_F1: g_dev.shown = !g_dev.shown; break;
                case PHX_KEY_F5: g_dev.paused = !g_dev.paused; g_dev.shown = true; break;
                case PHX_KEY_F6: g_dev.paused = true; g_dev.shown = true; ++g_dev.step; break;
                case PHX_KEY_F7: select_step(app, -1); g_dev.shown = true; break;
                case PHX_KEY_F8: select_step(app, +1); g_dev.shown = true; break;
                default: break;
            }
        } else if (ev.kind == PHX_DEV_MOUSE_DOWN && ev.button == PHX_MOUSE_LEFT && g_dev.shown) {
            select_at(app, ev.x, ev.y);
        }
    }
    if (!g_dev.paused) return steps;
    if (g_dev.step > 0) { --g_dev.step; return 1; }
    return 0;
}

// ---- drawing into the software framebuffer ----
struct Canvas {
    phx_soft_fb fb;
    void shade(int x0, int y0, int x1, int y1) {                  // darken a box (a backdrop)
        for (int y = y0 < 0 ? 0 : y0; y < y1 && y < fb.h; ++y)
            for (int x = x0 < 0 ? 0 : x0; x < x1 && x < fb.w; ++x) {
                uint32_t& p = fb.pixels[size_t(y) * size_t(fb.w) + size_t(x)];
                p = ((p >> 2) & 0x003F3F3Fu) | 0xFF000000u;
            }
    }
    void put(int x, int y, uint32_t c) {
        if (x >= 0 && y >= 0 && x < fb.w && y < fb.h) fb.pixels[size_t(y) * size_t(fb.w) + size_t(x)] = c;
    }
    void box(int x0, int y0, int x1, int y1, uint32_t c) {
        for (int x = x0; x <= x1; ++x) { put(x, y0, c); put(x, y1, c); }
        for (int y = y0; y <= y1; ++y) { put(x0, y, c); put(x1, y, c); }
    }
    int text(int x, int y, const char* s, uint32_t c) {
        for (; *s; ++s, x += 6) {
            const int g = (*s >= 32 && *s < 127) ? *s - 32 : 95;
            for (int r = 0; r < 7; ++r)
                for (int b = 0; b < 5; ++b)
                    if (devfont::kGlyph5x7[g][r] & (0x10 >> b)) put(x + b, y + r, c);
        }
        return x;
    }
};

constexpr uint32_t kWhite = 0xFFFFFFFFu, kDim = 0xFFB0B0B0u, kAccent = 0xFF3088FFu, kGood = 0xFF60D060u;

double sd(scalar v) { return s_to_double(v); }

// One reflected field's value as text.
void field_text(const FieldInfo& f, const uint8_t* d, char* out, size_t cap) {
    const uint8_t* p = d + f.offset;
    switch (f.type) {
        case FieldType::I8:   { int8_t v;   std::memcpy(&v, p, 1); std::snprintf(out, cap, "%d", int(v)); break; }
        case FieldType::U8:   { uint8_t v;  std::memcpy(&v, p, 1); std::snprintf(out, cap, "%u", unsigned(v)); break; }
        case FieldType::I16:  { int16_t v;  std::memcpy(&v, p, 2); std::snprintf(out, cap, "%d", int(v)); break; }
        case FieldType::U16:  { uint16_t v; std::memcpy(&v, p, 2); std::snprintf(out, cap, "%u", unsigned(v)); break; }
        case FieldType::I32:  { int32_t v;  std::memcpy(&v, p, 4); std::snprintf(out, cap, "%ld", long(v)); break; }
        case FieldType::U32:  { uint32_t v; std::memcpy(&v, p, 4); std::snprintf(out, cap, "%lu", (unsigned long)v); break; }
        case FieldType::Bool: { bool v;     std::memcpy(&v, p, sizeof(v)); std::snprintf(out, cap, "%s", v ? "true" : "false"); break; }
        case FieldType::Scalar: { scalar v; std::memcpy(&v, p, sizeof(v)); std::snprintf(out, cap, "%.2f", sd(v)); break; }
        case FieldType::Hash: { uint32_t v; std::memcpy(&v, p, 4); std::snprintf(out, cap, "#%08lx", (unsigned long)v); break; }
    }
}

void dev_overlay(void*, App& app) {
    if (!g_dev.shown || !app.platform() || !app.platform()->gfx) return;
    Canvas cv{ phx_gfx_soft_lock(app.platform()->gfx()) };
    if (!cv.fb.pixels || cv.fb.w < 60 || cv.fb.h < 40) return;
    ecs::World& w = app.world();
    if (g_dev.sel != ecs::kInvalid && !w.is_alive(g_dev.sel)) g_dev.sel = ecs::kInvalid;
    if (g_dev.sel == ecs::kInvalid) select_step(app, +1);

    // the selected entity's collider, outlined in the world
    const Camera2D& cam = app.render().camera();
    const scalar z = cam.zoom > s_from_int(0) ? cam.zoom : s_from_int(1);
    if (const Transform* t = g_dev.sel != ecs::kInvalid ? w.get<Transform>(g_dev.sel) : nullptr) {
        const AABBColl* c = w.get<AABBColl>(g_dev.sel);
        const scalar hx = c ? c->half.x : s_from_int(4), hy = c ? c->half.y : s_from_int(4);
        cv.box(s_to_int((t->pos.x - hx - cam.pos.x) * z), s_to_int((t->pos.y - hy - cam.pos.y) * z),
               s_to_int((t->pos.x + hx - cam.pos.x) * z) - 1, s_to_int((t->pos.y + hy - cam.pos.y) * z) - 1, kAccent);
    }

    // the panel: status, then the inspector
    char line[64];
    const int pw = cv.fb.w < 200 ? cv.fb.w : 200;
    int y = 2;
    const int lines_max = (cv.fb.h - 4) / 8;
    int shown_lines = 0;
    auto say = [&](const char* s, uint32_t c) {
        if (shown_lines >= lines_max) return;
        cv.shade(0, y - 1, pw, y + 7);
        cv.text(2, y, s, c);
        y += 8; ++shown_lines;
    };
    std::snprintf(line, sizeof(line), "%s  F5 %s F6 step F7/F8 pick", g_dev.paused ? "PAUSED" : "DEV", g_dev.paused ? "resume" : "pause");
    say(line, g_dev.paused ? kAccent : kGood);
    std::snprintf(line, sizeof(line), "frame %lu  entities %u", (unsigned long)app.frame(), unsigned(w.count()));
    say(line, kDim);
    const ecs::Entity e = g_dev.sel;
    if (e == ecs::kInvalid) { say("no entity selected", kDim); return; }

    const PrefabRef* pr = w.get<PrefabRef>(e);
    const Level* lv = Level::active();
    const char* type = pr && lv ? lv->prefabs().get_str(pr->row, "type"_hash) : nullptr;
    if (pr) std::snprintf(line, sizeof(line), "#%lu %s  spawn %u", (unsigned long)(e & 0xFFFFFFu), type ? type : "?", unsigned(pr->spawn));
    else    std::snprintf(line, sizeof(line), "#%lu", (unsigned long)(e & 0xFFFFFFu));
    say(line, kWhite);
    if (const Transform* t = w.get<Transform>(e)) { std::snprintf(line, sizeof(line), " pos %.1f, %.1f", sd(t->pos.x), sd(t->pos.y)); say(line, kDim); }
    if (const Body* b = w.get<Body>(e)) {
        std::snprintf(line, sizeof(line), " vel %.1f, %.1f%s", sd(b->vel.x), sd(b->vel.y), b->on_ground ? "  ground" : "");
        say(line, kDim);
    }
    if (const AABBColl* c = w.get<AABBColl>(e)) {
        std::snprintf(line, sizeof(line), " box %.0fx%.0f layer %u mask %u", sd(c->half.x) * 2, sd(c->half.y) * 2, unsigned(c->layer), unsigned(c->mask));
        say(line, kDim);
    }
    if (const Animator* an = w.get<Animator>(e)) {
        std::snprintf(line, sizeof(line), " clip %u frame %u", unsigned(an->clip), unsigned(an->frame));
        say(line, kDim);
    }
    for (uint32_t i = 0; i < reflected_count(); ++i) {
        const ComponentInfo& ci = *reflected_at(i);
        const uint8_t* d = static_cast<const uint8_t*>(ci.get(w, e));
        if (!d) continue;
        say(ci.name, kGood);
        for (uint8_t f = 0; f < ci.field_count; ++f) {
            char val[32];
            field_text(ci.fields[f], d, val, sizeof(val));
            std::snprintf(line, sizeof(line), "  %s %s", ci.fields[f].name, val);
            say(line, kDim);
        }
    }
}

} // namespace

void install_devtools(App& app) {
    g_dev = DevState{};
    DevHooks h;
    h.frame = &dev_frame;
    h.overlay = &dev_overlay;
    app.set_dev_hooks(h);
}

} // namespace phx
