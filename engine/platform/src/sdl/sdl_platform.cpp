// engine/platform/src/sdl/sdl_platform.cpp — the SDL2 desktop backend (Linux/Windows/macOS).
// It implements the SAME C seam as the null backend, so the entire engine + example run
// UNCHANGED on a real window: it owns the software framebuffer (render tier = software),
// uploads it to a streaming SDL texture each present, maps the keyboard to the canonical
// phx button bits, and uses a real monotonic clock. The GL backend will later replace the
// texture upload with GPU draws; until then this makes `make platformer` a game you can see.
//
// Compiled ONLY when PHX_HAVE_SDL is defined (and linked INSTEAD OF null_platform.cpp); the
// guard makes the translation unit empty otherwise, so it is harmless to list in a build that
// has no SDL2. See docs/02-platform-layer.md.
#if defined(PHX_HAVE_SDL)

#include "phx/platform/platform.h"
#include "phx/platform/gfx_soft.h"

#include <SDL.h>
#if defined(PHX_HAVE_GL)
#include <SDL_opengl.h>      // glReadPixels for the verification readback (GL render tier)
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// Canonical phx button bit order — MUST match phx::Button in engine/input (Up=0 .. Select=11).
enum Btn : uint32_t {
    B_UP = 0, B_DOWN, B_LEFT, B_RIGHT, B_A, B_B, B_X, B_Y, B_L, B_R, B_START, B_SELECT
};

constexpr int kScale = 3;   // window is kScale× the logical framebuffer for visibility

struct SdlState {
    SDL_Window*   win = nullptr;
    SDL_Renderer* ren = nullptr;             // software-present path (no GL)
    SDL_Texture*  tex = nullptr;
#if defined(PHX_HAVE_GL)
    SDL_GLContext glctx = nullptr;           // GL-render-tier path
#endif
    phx_soft_fb   fb  { nullptr, 0, 0 };     // soft path: CPU framebuffer; GL path: logical size only (pixels=null)
    uint64_t      freq = 1;                   // performance-counter frequency
    uint64_t      base = 0;                   // counter at init (clock origin)
    int           quit = 0;
};
SdlState g;

int sdl_init(const phx_platform_desc* desc) {
    const int w = desc->width  > 0 ? desc->width  : 240;
    const int h = desc->height > 0 ? desc->height : 160;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "[phx.sdl] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    const char* title = desc->title ? desc->title : "Phoenix";

#if defined(PHX_HAVE_GL)
    // GL render tier: an OpenGL context + double buffering. The GL backend draws; we swap.
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    g.win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             w * kScale, h * kScale, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
    if (!g.win) { std::fprintf(stderr, "[phx.sdl] CreateWindow(GL): %s\n", SDL_GetError()); return 1; }
    g.glctx = SDL_GL_CreateContext(g.win);
    if (!g.glctx) { std::fprintf(stderr, "[phx.sdl] GL_CreateContext: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetSwapInterval(desc->vsync ? 1 : 0);
    g.fb.w = w; g.fb.h = h; g.fb.pixels = nullptr;   // logical size only; GL owns the pixels
    std::printf("[phx.sdl] init '%s' %dx%d GL (window %dx%d, vsync=%d)\n",
                title, w, h, w * kScale, h * kScale, desc->vsync);
#else
    // Software render tier: a streaming texture we upload the CPU framebuffer into each frame.
    g.win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             w * kScale, h * kScale, SDL_WINDOW_SHOWN);
    if (!g.win) { std::fprintf(stderr, "[phx.sdl] CreateWindow: %s\n", SDL_GetError()); return 1; }

    Uint32 rflags = SDL_RENDERER_ACCELERATED | (desc->vsync ? Uint32(SDL_RENDERER_PRESENTVSYNC) : 0u);
    g.ren = SDL_CreateRenderer(g.win, -1, rflags);
    if (!g.ren) { std::fprintf(stderr, "[phx.sdl] CreateRenderer: %s\n", SDL_GetError()); return 1; }
    SDL_RenderSetLogicalSize(g.ren, w, h);    // crisp integer upscale of the framebuffer

    // phx Rgba is R|G<<8|B<<16|A<<24 -> bytes (MSB..LSB) A,B,G,R == SDL_PIXELFORMAT_ABGR8888.
    g.tex = SDL_CreateTexture(g.ren, SDL_PIXELFORMAT_ABGR8888,
                              SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!g.tex) { std::fprintf(stderr, "[phx.sdl] CreateTexture: %s\n", SDL_GetError()); return 1; }

    g.fb.w = w; g.fb.h = h;
    g.fb.pixels = static_cast<uint32_t*>(std::calloc(size_t(w) * size_t(h), sizeof(uint32_t)));
    if (!g.fb.pixels) return 1;
    std::printf("[phx.sdl] init '%s' %dx%d SW (window %dx%d, vsync=%d)\n",
                title, w, h, w * kScale, h * kScale, desc->vsync);
#endif

    g.freq = SDL_GetPerformanceFrequency();
    g.base = SDL_GetPerformanceCounter();
    g.quit = 0;
    return 0;
}

void sdl_shutdown(void) {
#if defined(PHX_HAVE_GL)
    if (g.glctx) SDL_GL_DeleteContext(g.glctx);
    g.glctx = nullptr;
#else
    std::free(g.fb.pixels); g.fb.pixels = nullptr;
    if (g.tex) SDL_DestroyTexture(g.tex);
    if (g.ren) SDL_DestroyRenderer(g.ren);
    g.tex = nullptr; g.ren = nullptr;
#endif
    g.fb.w = g.fb.h = 0;
    if (g.win) SDL_DestroyWindow(g.win);
    g.win = nullptr;
    SDL_Quit();
}

uint64_t sdl_clock_ns(void) {
    const uint64_t now = SDL_GetPerformanceCounter() - g.base;
    // ns = now * 1e9 / freq, computed to avoid overflow on large counters
    return (now / g.freq) * 1000000000ull + ((now % g.freq) * 1000000000ull) / g.freq;
}
void sdl_sleep_ns(uint64_t ns) { SDL_Delay(Uint32(ns / 1000000ull)); }

int sdl_pump_events(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) g.quit = 1;
        else if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) g.quit = 1;
    }
    return g.quit ? 0 : 1;
}

void sdl_present(void) {
#if defined(PHX_HAVE_GL)
    SDL_GL_SwapWindow(g.win);            // the GL backend already drew into the back buffer
#else
    SDL_UpdateTexture(g.tex, nullptr, g.fb.pixels, g.fb.w * int(sizeof(uint32_t)));
    SDL_RenderClear(g.ren);
    SDL_RenderCopy(g.ren, g.tex, nullptr, nullptr);
    SDL_RenderPresent(g.ren);
#endif
}

phx_gfx*   sdl_gfx(void)   { return reinterpret_cast<phx_gfx*>(&g); }   // gfx_soft_lock reads g.fb
phx_audio* sdl_audio(void) { return nullptr; }                          // device opened separately

// --- real audio device --------------------------------------------------------------------
// The platform owns the device but NOT the mixer (layering: platform must not depend on audio).
// The game registers a fill callback that drains its lock-free AudioCommandQueue and calls
// AudioMixer::mix(); SDL invokes it on the audio thread, so the mixer is touched single-threaded.
typedef void (*phx_audio_fill)(void* user, int16_t* out, int frames);
struct AudioState { SDL_AudioDeviceID dev; phx_audio_fill fill; void* user; };
AudioState g_audio{ 0, nullptr, nullptr };

void SDLCALL sdl_audio_trampoline(void* userdata, Uint8* stream, int len) {
    AudioState* a = static_cast<AudioState*>(userdata);
    const int frames = len / int(sizeof(int16_t) * 2);   // interleaved stereo S16
    if (a->fill) a->fill(a->user, reinterpret_cast<int16_t*>(stream), frames);
    else std::memset(stream, 0, size_t(len));
}

void sdl_poll_input(phx_input_raw* out) {
    std::memset(out, 0, sizeof(*out));
    const Uint8* k = SDL_GetKeyboardState(nullptr);
    uint32_t b = 0;
    auto set = [&](Btn bit, bool on) { if (on) b |= (1u << uint32_t(bit)); };
    set(B_UP,    k[SDL_SCANCODE_UP]    || k[SDL_SCANCODE_W]);
    set(B_DOWN,  k[SDL_SCANCODE_DOWN]  || k[SDL_SCANCODE_S]);
    set(B_LEFT,  k[SDL_SCANCODE_LEFT]  || k[SDL_SCANCODE_A]);
    set(B_RIGHT, k[SDL_SCANCODE_RIGHT] || k[SDL_SCANCODE_D]);
    set(B_A,     k[SDL_SCANCODE_Z]     || k[SDL_SCANCODE_SPACE]);
    set(B_B,     k[SDL_SCANCODE_X]);
    set(B_X,     k[SDL_SCANCODE_C]);
    set(B_Y,     k[SDL_SCANCODE_V]);
    set(B_L,     k[SDL_SCANCODE_Q]);
    set(B_R,     k[SDL_SCANCODE_E]);
    set(B_START, k[SDL_SCANCODE_RETURN]);
    set(B_SELECT,k[SDL_SCANCODE_RSHIFT] || k[SDL_SCANCODE_TAB]);
    out->buttons = b;

    int mx = 0, my = 0;
    Uint32 ms = SDL_GetMouseState(&mx, &my);
    // Mouse arrives in WINDOW pixels; the seam promises framebuffer coordinates, so undo
    // the integer upscale (tools like phxtmap hit-test tiles against these).
    out->pointer_x = int16_t(mx / kScale); out->pointer_y = int16_t(my / kScale);
    out->pointer_down = (ms & SDL_BUTTON(SDL_BUTTON_LEFT)) ? 1 : 0;
}

// File I/O: load-once into a heap buffer; map() returns a stable pointer (seam contract).
struct SdlFile { void* data; size_t size; };

phx_file* sdl_open(const char* path, size_t* out_size) {
    FILE* fp = std::fopen(path, "rb");
    if (!fp) { if (out_size) *out_size = 0; return nullptr; }
    std::fseek(fp, 0, SEEK_END);
    long n = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (n < 0) { std::fclose(fp); if (out_size) *out_size = 0; return nullptr; }
    SdlFile* h = static_cast<SdlFile*>(std::malloc(sizeof(SdlFile)));
    h->size = size_t(n);
    h->data = std::malloc(h->size ? h->size : 1);
    size_t got = std::fread(h->data, 1, h->size, fp);
    std::fclose(fp);
    if (got != h->size) { std::free(h->data); std::free(h); if (out_size) *out_size = 0; return nullptr; }
    if (out_size) *out_size = h->size;
    return reinterpret_cast<phx_file*>(h);
}
const void* sdl_map(phx_file* f) { return f ? reinterpret_cast<SdlFile*>(f)->data : nullptr; }
void        sdl_close(phx_file* f) {
    if (!f) return;
    SdlFile* h = reinterpret_cast<SdlFile*>(f);
    std::free(h->data); std::free(h);
}

// Persistence: a plain save file keyed by path (the desktop store).
int sdl_save(const char* key, const void* data, uint32_t size) {
    FILE* fp = std::fopen(key, "wb");
    if (!fp) return 1;
    size_t w = std::fwrite(data, 1, size, fp);
    std::fclose(fp);
    return (w == size) ? 0 : 1;
}
int sdl_load(const char* key, void* out, uint32_t cap, uint32_t* out_size) {
    FILE* fp = std::fopen(key, "rb");
    if (!fp) { if (out_size) *out_size = 0; return 1; }
    size_t got = std::fread(out, 1, cap, fp);
    std::fclose(fp);
    if (out_size) *out_size = uint32_t(got);
    return 0;
}

void sdl_log(phx_log_level level, const char* msg) {
    static const char* tag[] = { "TRACE", "DEBUG", "INFO ", "WARN ", "ERROR" };
    int l = int(level); if (l < 0 || l > 4) l = 2;
    std::printf("[phx.sdl][%s] %s\n", tag[l], msg);
}

const phx_platform g_sdl_platform = {
    sdl_init, sdl_shutdown,
    sdl_clock_ns, sdl_sleep_ns,
    sdl_pump_events, sdl_present,
    sdl_gfx, sdl_audio,
    sdl_poll_input,
    sdl_open, sdl_map, sdl_close,
    sdl_save, sdl_load,
    sdl_log,
};

} // namespace

extern "C" const phx_platform* phx_platform_get(void) { return &g_sdl_platform; }

// Software-tier graphics contract: hand the render backend our CPU framebuffer.
extern "C" phx_soft_fb phx_gfx_soft_lock(phx_gfx* gfx) {
    return reinterpret_cast<SdlState*>(gfx)->fb;
}

// --- verification readback ----------------------------------------------------------------
// Read the *actually presented* frame back as logical-resolution phx Rgba (R|G<<8|B<<16|A<<24),
// so a headless harness can pixel-diff the real window/GPU output against the software golden
// reference (the same way the PPU/GU backends are verified). Call right after the renderer's
// end_frame(), before present(). Returns 0 on success. The window is kScale× the logical size,
// so we read the drawable and sample each logical pixel's block centre.
extern "C" int phx_sdl_readback(uint32_t* out, int lw, int lh) {
    if (!out || lw <= 0 || lh <= 0) return 1;
#if defined(PHX_HAVE_GL)
    int ow = 0, oh = 0;
    SDL_GL_GetDrawableSize(g.win, &ow, &oh);
    if (ow <= 0 || oh <= 0) return 1;
    uint32_t* tmp = static_cast<uint32_t*>(std::malloc(size_t(ow) * size_t(oh) * 4));
    if (!tmp) return 1;
    glReadPixels(0, 0, ow, oh, GL_RGBA, GL_UNSIGNED_BYTE, tmp);  // bytes R,G,B,A == phx Rgba
    for (int y = 0; y < lh; ++y)
        for (int x = 0; x < lw; ++x) {
            int wx = x * ow / lw + ow / (2 * lw);
            int wy = y * oh / lh + oh / (2 * lh);
            int gy = oh - 1 - wy;                                // glReadPixels origin = bottom-left
            if (wx >= ow) wx = ow - 1;
            if (gy < 0) gy = 0;
            out[y * lw + x] = tmp[gy * ow + wx];
        }
    std::free(tmp);
    return 0;
#else
    int ow = 0, oh = 0;
    SDL_GetRendererOutputSize(g.ren, &ow, &oh);
    if (ow <= 0 || oh <= 0) return 1;
    // Compose the current soft framebuffer into the backbuffer exactly as present() does.
    SDL_UpdateTexture(g.tex, nullptr, g.fb.pixels, g.fb.w * int(sizeof(uint32_t)));
    SDL_RenderClear(g.ren);
    SDL_RenderCopy(g.ren, g.tex, nullptr, nullptr);
    uint32_t* tmp = static_cast<uint32_t*>(std::malloc(size_t(ow) * size_t(oh) * 4));
    if (!tmp) return 1;
    if (SDL_RenderReadPixels(g.ren, nullptr, SDL_PIXELFORMAT_ABGR8888, tmp, ow * 4) != 0) {
        std::free(tmp); return 1;
    }
    for (int y = 0; y < lh; ++y)
        for (int x = 0; x < lw; ++x) {
            int sx = x * ow / lw + ow / (2 * lw);
            int sy = y * oh / lh + oh / (2 * lh);                // SDL origin = top-left
            if (sx >= ow) sx = ow - 1;
            if (sy >= oh) sy = oh - 1;
            out[y * lw + x] = tmp[sy * ow + sx];
        }
    std::free(tmp);
    return 0;
#endif
}

// Open a stereo S16 device at `rate` and run `fill` on the audio thread (interleaved L,R).
// Returns 0 on success. The game's fill drains its command queue then mixes — see
// phx/audio/command_queue.h. Stop with phx_sdl_audio_stop().
extern "C" int phx_sdl_audio_start(int rate, phx_audio_fill fill, void* user) {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return 1;
    SDL_AudioSpec want; SDL_memset(&want, 0, sizeof(want));
    want.freq = rate > 0 ? rate : 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;                       // ~23ms latency at 44.1kHz
    want.callback = sdl_audio_trampoline;
    want.userdata = &g_audio;
    g_audio.fill = fill; g_audio.user = user;
    g_audio.dev = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
    if (!g_audio.dev) return 1;
    SDL_PauseAudioDevice(g_audio.dev, 0);      // unpause -> the callback starts firing
    return 0;
}
extern "C" void phx_sdl_audio_stop(void) {
    if (g_audio.dev) { SDL_CloseAudioDevice(g_audio.dev); g_audio.dev = 0; }
}

#endif // PHX_HAVE_SDL
