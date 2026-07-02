// engine/platform/src/psp/psp_platform.cpp — the PlayStation Portable backend of the C seam.
// The PSP display's 8888 format is byte order R,G,B,A — identical to the engine's soft framebuffer
// — so the software renderer's output blits to VRAM with NO per-pixel conversion (just a stride
// copy into the 512-wide framebuffer, integer-scaled + centered). Buttons map from sceCtrl;
// timing is VBlank-locked. No filesystem dependency here (assets would be loaded via sceIo in a
// full build); this smoke proves the portable engine runs on MIPS Allegrex.
//
// Compiled ONLY when PHX_TARGET_PSP is defined (linked INSTEAD OF other backends).
#if defined(PHX_TARGET_PSP)

#include "phx/platform/platform.h"
#include "phx/platform/gfx_soft.h"

#include <pspdisplay.h>
#include <pspctrl.h>
#include <pspge.h>
#include <pspaudio.h>
#include <pspthreadman.h>
#include <pspiofilemgr.h>
#include <stdlib.h>
#include <string.h>

// Freestanding mem/str primitives for the PSP build. The compiler emits calls to these (e.g. to
// zero-init objects in placement-new). pspsdk would otherwise resolve them to the KERNEL sysclib
// SYSCALL stubs in libpspkernel.a — wrong for a user module, and PPSSPP's HLE of sysclib_memset
// returns 0 instead of `dest`, so a constructor's zero-init wrote its result through NULL and the
// engine crashed on boot. Defining them here (our objects link before the archives) wins, and is
// correct on real hardware too. Built with -fno-tree-loop-distribute-patterns so these loops are
// not "optimized" back into calls to themselves.
extern "C" {
void* memset(void* dst, int c, size_t n) {
    unsigned char* p = static_cast<unsigned char*>(dst);
    for (size_t i = 0; i < n; ++i) p[i] = static_cast<unsigned char>(c);
    return dst;
}
void* memcpy(void* dst, const void* src, size_t n) {
    unsigned char* d = static_cast<unsigned char*>(dst);
    const unsigned char* s = static_cast<const unsigned char*>(src);
    for (size_t i = 0; i < n; ++i) d[i] = s[i];
    return dst;
}
void* memmove(void* dst, const void* src, size_t n) {
    unsigned char* d = static_cast<unsigned char*>(dst);
    const unsigned char* s = static_cast<const unsigned char*>(src);
    if (d < s) { for (size_t i = 0; i < n; ++i) d[i] = s[i]; }
    else       { for (size_t i = n; i-- > 0; ) d[i] = s[i]; }
    return dst;
}
size_t strlen(const char* s) { size_t n = 0; while (s[n]) ++n; return n; }
}

namespace {

constexpr int kScreenW = 480, kScreenH = 272, kStride = 512;

struct PspGfx { phx_soft_fb fb; };
PspGfx   g_gfx = { { nullptr, 0, 0 } };
uint64_t g_step_ns = 1000000000ull / 60;
uint64_t g_vtime   = 0;
uint32_t* g_vram   = nullptr;

const void* g_bundle = nullptr; size_t g_bundle_size = 0;

// When a hardware render backend (the GU backend) owns the display, it programs sceGu's draw/
// display buffers + does its own vblank/swap, so the platform must NOT set up sceDisplay or blit
// the software framebuffer. The GU entry calls phx_psp_set_direct(1) before boot.
bool g_direct = false;

// Canonical phx::Button bit order (phx/input/input.h).
enum PhxBtn { PB_UP=0, PB_DOWN=1, PB_LEFT=2, PB_RIGHT=3, PB_A=4, PB_B=5, PB_X=6, PB_Y=7, PB_L=8, PB_R=9, PB_START=10, PB_SELECT=11 };

int psp_init(const phx_platform_desc* desc) {
    int w = desc->width  > 0 ? desc->width  : kScreenW;
    int h = desc->height > 0 ? desc->height : kScreenH;
    if (w > kScreenW) w = kScreenW;
    if (h > kScreenH) h = kScreenH;
    if (g_direct) { w = 1; h = 1; }              // GU owns the display; the soft fb is unused
    g_gfx.fb.w = w; g_gfx.fb.h = h;
    g_gfx.fb.pixels = static_cast<uint32_t*>(malloc(size_t(w) * size_t(h) * sizeof(uint32_t)));
    if (!g_gfx.fb.pixels) return 1;

    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);
    g_vtime = 0;
    if (g_direct) return 0;                       // the GU backend sets up sceDisplay/sceGu itself

    // uncached VRAM pointer (EDRAM base | 0x40000000), used as the display framebuffer.
    g_vram = reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(sceGeEdramGetAddr()) | 0x40000000u);
    sceDisplaySetMode(0, kScreenW, kScreenH);
    sceDisplaySetFrameBuf(g_vram, kStride, PSP_DISPLAY_PIXEL_FORMAT_8888, PSP_DISPLAY_SETBUF_IMMEDIATE);
    return 0;
}
void psp_shutdown(void) { free(g_gfx.fb.pixels); g_gfx.fb.pixels = nullptr; }

uint64_t psp_clock_ns(void) { uint64_t t = g_vtime; g_vtime += g_step_ns; return t; }
void     psp_sleep_ns(uint64_t) {}
int      psp_pump_events(void) { return 1; }

void psp_present(void) {
    if (g_direct) return;                         // the GU backend did its own vblank + buffer swap
    sceDisplayWaitVblankStart();
    const int w = g_gfx.fb.w, h = g_gfx.fb.h;
    if (!g_gfx.fb.pixels || !g_vram || w <= 0 || h <= 0) return;
    int sx = kScreenW / w, sy = kScreenH / h;
    int s = sx < sy ? sx : sy; if (s < 1) s = 1;
    const int ox = (kScreenW - w * s) / 2, oy = (kScreenH - h * s) / 2;
    const uint32_t* src = g_gfx.fb.pixels;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint32_t c = src[y * w + x];          // 8888 == R,G,B,A: copy verbatim
            const int bx = ox + x * s, by = oy + y * s;
            for (int dy = 0; dy < s; ++dy) {
                uint32_t* row = g_vram + size_t(by + dy) * kStride + bx;
                for (int dx = 0; dx < s; ++dx) row[dx] = c;
            }
        }
}

phx_gfx*   psp_gfx(void)   { return reinterpret_cast<phx_gfx*>(&g_gfx); }
phx_audio* psp_audio(void) { return nullptr; }   // the device is driven via phx_psp_audio_start (below)

// --- sceAudio output device ----------------------------------------------------------------
// The platform owns the hardware channel + the audio thread but NOT the mixer (layering: platform
// must not depend on audio). The game registers a fill callback (same contract as the SDL backend)
// that drains its lock-free AudioCommandQueue then calls AudioMixer::mix(); a dedicated, high-
// priority PSP thread calls it and pushes the result to a reserved sceAudio channel with
// sceAudioOutputBlocking — so the mixer is touched single-threaded, exactly like on desktop.
// The PSP's standard channels run at 44.1 kHz stereo S16, matching the mixer's default output.
typedef void (*phx_audio_fill)(void* user, int16_t* out, int frames);

constexpr int kAudioSamples = 1024;              // frames per output block (multiple of 64)

struct PspAudio {
    int            ch      = -1;
    SceUID         thid    = -1;
    volatile int   running = 0;
    phx_audio_fill fill    = nullptr;
    void*          user    = nullptr;
};
PspAudio g_audio;
// Double-buffered output so the next block is filled while sceAudio consumes the current one.
int16_t g_audio_buf[2][kAudioSamples * 2] __attribute__((aligned(64)));

int psp_audio_thread(SceSize, void*) {
    int bi = 0;
    while (g_audio.running) {
        int16_t* buf = g_audio_buf[bi];
        if (g_audio.fill) g_audio.fill(g_audio.user, buf, kAudioSamples);
        else              memset(buf, 0, sizeof(g_audio_buf[0]));
        sceAudioOutputBlocking(g_audio.ch, PSP_AUDIO_VOLUME_MAX, buf);
        bi ^= 1;
    }
    return 0;
}

void psp_poll_input(phx_input_raw* out) {
    memset(out, 0, sizeof(*out));
    SceCtrlData pad; sceCtrlReadBufferPositive(&pad, 1);
    const unsigned k = pad.Buttons;
    uint32_t b = 0;
    if (k & PSP_CTRL_UP)       b |= 1u<<PB_UP;
    if (k & PSP_CTRL_DOWN)     b |= 1u<<PB_DOWN;
    if (k & PSP_CTRL_LEFT)     b |= 1u<<PB_LEFT;
    if (k & PSP_CTRL_RIGHT)    b |= 1u<<PB_RIGHT;
    if (k & PSP_CTRL_CROSS)    b |= 1u<<PB_A;
    if (k & PSP_CTRL_CIRCLE)   b |= 1u<<PB_B;
    if (k & PSP_CTRL_SQUARE)   b |= 1u<<PB_X;
    if (k & PSP_CTRL_TRIANGLE) b |= 1u<<PB_Y;
    if (k & PSP_CTRL_LTRIGGER) b |= 1u<<PB_L;
    if (k & PSP_CTRL_RTRIGGER) b |= 1u<<PB_R;
    if (k & PSP_CTRL_START)    b |= 1u<<PB_START;
    if (k & PSP_CTRL_SELECT)   b |= 1u<<PB_SELECT;
    out->buttons = b;
    out->pointer_x = -1; out->pointer_y = -1;
}

struct PspFile { const void* data; size_t size; };
PspFile g_file;
phx_file* psp_open(const char* /*path*/, size_t* out_size) {
    if (!g_bundle) { if (out_size) *out_size = 0; return nullptr; }
    g_file.data = g_bundle; g_file.size = g_bundle_size;
    if (out_size) *out_size = g_bundle_size;
    return reinterpret_cast<phx_file*>(&g_file);
}
const void* psp_map(phx_file* f) { return f ? reinterpret_cast<PspFile*>(f)->data : nullptr; }
void        psp_close(phx_file*) {}

// Persistence: a memory-stick file via sceIo. A bare key (no device prefix) is anchored at
// ms0:/PSP/SAVEDATA/PHX/<key> — a bare relative path has NO cwd when the EBOOT is launched
// directly (sceIoOpen fails with SCE_KERNEL_ERROR_NOCWD, seen on PPSSPP), and a UMD/flash
// game could never write next to itself anyway. Keys that already carry a device ("ms0:/…")
// pass through untouched.
void psp_save_path(const char* key, char* out, uint32_t cap) {
    static const char kPrefix[] = "ms0:/PSP/SAVEDATA/PHX/";
    if (strchr(key, ':')) {                       // already device-anchored: use as-is
        uint32_t i = 0;
        for (; key[i] && i + 1 < cap; ++i) out[i] = key[i];
        out[i] = '\0';
        return;
    }
    uint32_t i = 0;
    for (; kPrefix[i] && i + 1 < cap; ++i) out[i] = kPrefix[i];
    for (uint32_t k = 0; key[k] && i + 1 < cap; ++k, ++i) out[i] = key[k];
    out[i] = '\0';
}
int psp_save(const char* key, const void* data, uint32_t size) {
    char path[256]; psp_save_path(key, path, sizeof(path));
    sceIoMkdir("ms0:/PSP", 0777);                 // idempotent; EEXIST is fine
    sceIoMkdir("ms0:/PSP/SAVEDATA", 0777);
    sceIoMkdir("ms0:/PSP/SAVEDATA/PHX", 0777);
    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return 1;
    int w = sceIoWrite(fd, data, int(size));
    sceIoClose(fd);
    return (w == int(size)) ? 0 : 1;
}
int psp_load(const char* key, void* out, uint32_t cap, uint32_t* out_size) {
    char path[256]; psp_save_path(key, path, sizeof(path));
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
    if (fd < 0) { if (out_size) *out_size = 0; return 1; }
    int r = sceIoRead(fd, out, int(cap));
    sceIoClose(fd);
    if (r < 0) { if (out_size) *out_size = 0; return 1; }
    if (out_size) *out_size = uint32_t(r);
    return 0;
}

void        psp_log(phx_log_level, const char*) {}

const phx_platform g_psp_platform = {
    psp_init, psp_shutdown,
    psp_clock_ns, psp_sleep_ns,
    psp_pump_events, psp_present,
    psp_gfx, psp_audio,
    psp_poll_input,
    psp_open, psp_map, psp_close,
    psp_save, psp_load,
    psp_log,
};

} // namespace

extern "C" const phx_platform* phx_platform_get(void) { return &g_psp_platform; }
extern "C" phx_soft_fb phx_gfx_soft_lock(phx_gfx* gfx) { return reinterpret_cast<PspGfx*>(gfx)->fb; }
extern "C" void phx_psp_set_bundle(const void* data, size_t size) { g_bundle = data; g_bundle_size = size; }

// Hook (not part of the seam): a hardware render backend calls this before boot to take over the
// display, so psp_init skips sceDisplay setup and psp_present skips the software blit.
extern "C" void phx_psp_set_direct(int on) { g_direct = on != 0; }

// Open a 44.1 kHz stereo S16 sceAudio channel and run `fill` on a dedicated thread (interleaved
// L,R). Returns 0 on success. The game's fill drains its command queue then mixes — same contract
// as phx_sdl_audio_start. Stop with phx_psp_audio_stop().
extern "C" int phx_psp_audio_start(int /*rate*/, phx_audio_fill fill, void* user) {
    g_audio.ch = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL, kAudioSamples, PSP_AUDIO_FORMAT_STEREO);
    if (g_audio.ch < 0) return 1;
    g_audio.fill = fill; g_audio.user = user; g_audio.running = 1;
    g_audio.thid = sceKernelCreateThread("phx_audio", psp_audio_thread, 0x12, 0x10000, 0, nullptr);
    if (g_audio.thid < 0) { sceAudioChRelease(g_audio.ch); g_audio.ch = -1; g_audio.running = 0; return 1; }
    sceKernelStartThread(g_audio.thid, 0, nullptr);
    return 0;
}
extern "C" void phx_psp_audio_stop(void) {
    if (!g_audio.running) return;
    g_audio.running = 0;                          // the thread exits after its current block
    if (g_audio.thid >= 0) {
        sceKernelWaitThreadEnd(g_audio.thid, nullptr);
        sceKernelDeleteThread(g_audio.thid);
        g_audio.thid = -1;
    }
    if (g_audio.ch >= 0) { sceAudioChRelease(g_audio.ch); g_audio.ch = -1; }
}

#endif // PHX_TARGET_PSP
