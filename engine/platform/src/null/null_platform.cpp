// engine/platform/src/null/null_platform.cpp — headless backend implementing the C seam.
// No window, no audio. Its clock is VIRTUAL and deterministic (advances a fixed step per
// call), so the App loop runs reproducibly in tests/CI with no real-time waiting and no
// external dependency. It DOES own a CPU framebuffer (the software-render target), which
// the render backend draws into and tests can read back as a headless "screenshot".
// This is also the reference backend the conformance suite targets.
#include "phx/platform/platform.h"
#include "phx/platform/gfx_soft.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// Software framebuffer owned by the platform (the gfx device for the software tier).
struct NullGfx { phx_soft_fb fb; };
NullGfx g_gfx = { { nullptr, 0, 0 } };

// Virtual clock: each clock_ns() call advances time by one sim step. With the App loop
// calling clock_ns() once per iteration, that yields exactly one fixed step per frame —
// fully deterministic. Configurable via phx_null_set_step_ns() for tests.
uint64_t g_step_ns = 1000000000ull / 60;   // default ~60 Hz
uint64_t g_vtime   = 0;

// Optional frame budget: pump_events returns 0 (quit) after N frames. 0 = run forever.
uint64_t g_max_frames = 0;
uint64_t g_frames     = 0;

// Scripted input for headless tests: poll_input reports g_buttons every frame, OR — if a
// per-frame script is set — masks[frame] (holding the last entry once exhausted). The script
// lets a test drive button EDGES across frames (e.g. tap Down, then tap A) deterministically.
uint32_t        g_buttons      = 0;
const uint32_t* g_btn_script   = nullptr;
uint32_t        g_btn_script_n = 0;

int null_init(const phx_platform_desc* desc) {
    g_vtime = 0; g_frames = 0;
    int w = desc->width  > 0 ? desc->width  : 240;
    int h = desc->height > 0 ? desc->height : 160;
    g_gfx.fb.w = w; g_gfx.fb.h = h;
    g_gfx.fb.pixels = static_cast<uint32_t*>(std::calloc(size_t(w) * size_t(h), sizeof(uint32_t)));
    if (!g_gfx.fb.pixels) return 1;
    std::printf("[phx.null] init '%s' %dx%d (headless, software framebuffer)\n",
                desc->title ? desc->title : "?", w, h);
    return 0;
}
void     null_shutdown(void) {
    std::free(g_gfx.fb.pixels);
    g_gfx.fb.pixels = nullptr; g_gfx.fb.w = g_gfx.fb.h = 0;
}
uint64_t null_clock_ns(void) { uint64_t t = g_vtime; g_vtime += g_step_ns; return t; }
void     null_sleep_ns(uint64_t) {}

int  null_pump_events(void) {
    if (g_max_frames && g_frames >= g_max_frames) return 0;   // request quit
    ++g_frames;
    return 1;
}
void null_present(void) {}

phx_gfx*   null_gfx(void)   { return reinterpret_cast<phx_gfx*>(&g_gfx); }
phx_audio* null_audio(void) { return nullptr; }

void null_poll_input(phx_input_raw* out) {
    std::memset(out, 0, sizeof(*out));
    uint32_t btns = g_buttons;
    if (g_btn_script && g_btn_script_n) {           // pump_events already advanced g_frames
        uint32_t i = g_frames > 0 ? g_frames - 1 : 0;
        if (i >= g_btn_script_n) i = g_btn_script_n - 1;   // hold last entry when exhausted
        btns = g_btn_script[i];
    }
    out->buttons   = btns;
    out->pointer_x = -1; out->pointer_y = -1;
}

// Real host file I/O (load-once into a heap buffer, like the PSP backend). `map` then
// returns a stable pointer to that buffer — honoring the seam's "map returns a stable
// zero-copy view" contract even though there is no OS mmap involved.
struct NullFile { void* data; size_t size; };

phx_file* null_open(const char* path, size_t* out_size) {
    FILE* fp = std::fopen(path, "rb");
    if (!fp) { if (out_size) *out_size = 0; return nullptr; }
    std::fseek(fp, 0, SEEK_END);
    long n = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (n < 0) { std::fclose(fp); if (out_size) *out_size = 0; return nullptr; }

    NullFile* h = static_cast<NullFile*>(std::malloc(sizeof(NullFile)));
    h->size = size_t(n);
    h->data = std::malloc(h->size ? h->size : 1);
    size_t got = std::fread(h->data, 1, h->size, fp);
    std::fclose(fp);
    if (got != h->size) { std::free(h->data); std::free(h); if (out_size) *out_size = 0; return nullptr; }

    if (out_size) *out_size = h->size;
    return reinterpret_cast<phx_file*>(h);
}
const void* null_map(phx_file* f) { return f ? reinterpret_cast<NullFile*>(f)->data : nullptr; }
void        null_close(phx_file* f) {
    if (!f) return;
    NullFile* h = reinterpret_cast<NullFile*>(f);
    std::free(h->data);
    std::free(h);
}

// Persistence: a plain file keyed by path (the headless/PC store).
int null_save(const char* key, const void* data, uint32_t size) {
    FILE* fp = std::fopen(key, "wb");
    if (!fp) return 1;
    size_t w = std::fwrite(data, 1, size, fp);
    std::fclose(fp);
    return (w == size) ? 0 : 1;
}
int null_load(const char* key, void* out, uint32_t cap, uint32_t* out_size) {
    FILE* fp = std::fopen(key, "rb");
    if (!fp) { if (out_size) *out_size = 0; return 1; }       // no save present
    size_t got = std::fread(out, 1, cap, fp);
    std::fclose(fp);
    if (out_size) *out_size = uint32_t(got);
    return 0;
}

void null_log(phx_log_level level, const char* msg) {
    static const char* tag[] = { "TRACE", "DEBUG", "INFO ", "WARN ", "ERROR" };
    int l = int(level); if (l < 0 || l > 4) l = 2;
    std::printf("[phx.null][%s] %s\n", tag[l], msg);
}

const phx_platform g_null_platform = {
    null_init, null_shutdown,
    null_clock_ns, null_sleep_ns,
    null_pump_events, null_present,
    null_gfx, null_audio,
    null_poll_input,
    null_open, null_map, null_close,
    null_save, null_load,
    null_log,
};

} // namespace

extern "C" const phx_platform* phx_platform_get(void) { return &g_null_platform; }

// Software-tier graphics contract: hand the render backend our framebuffer.
extern "C" phx_soft_fb phx_gfx_soft_lock(phx_gfx* gfx) {
    return reinterpret_cast<NullGfx*>(gfx)->fb;
}

// Test/host hooks (not part of the seam) to make the headless loop deterministic.
extern "C" void phx_null_set_step_ns(uint64_t ns)     { g_step_ns = ns ? ns : 1; }
extern "C" void phx_null_set_max_frames(uint64_t n)   { g_max_frames = n; }
extern "C" void phx_null_set_buttons(uint32_t mask)   { g_buttons = mask; }
extern "C" void phx_null_set_button_script(const uint32_t* masks, uint32_t n) {
    g_btn_script = masks; g_btn_script_n = n;
}
