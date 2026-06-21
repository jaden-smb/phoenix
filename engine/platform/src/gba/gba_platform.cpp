// engine/platform/src/gba/gba_platform.cpp — the Game Boy Advance backend of the C seam.
// Bring-up uses BG Mode 3 (240x160, 16bpp BGR555 bitmap): the software renderer draws into an
// RGBA8 backbuffer (the soft-fb the engine already targets), and present() converts + blits it
// to VRAM, nearest-scaled to fill the screen. Keypad -> canonical buttons; the clock is frame-
// locked to VBlank. There is no filesystem, so assets are a ROM-embedded bundle handed in via
// phx_gba_set_bundle(). A real GBA build would add a PPU render backend (hardware sprites/tiles)
// for speed; this proves the portable engine runs on ARM7TDMI end to end.
//
// Compiled ONLY when PHX_TARGET_GBA is defined (linked INSTEAD OF the null/sdl backend); the TU
// is empty otherwise, so host builds are unaffected.
#if defined(PHX_TARGET_GBA)

#include "phx/platform/platform.h"
#include "phx/platform/gfx_soft.h"

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

namespace {

// ---- GBA memory-mapped I/O (raw, no libgba dependency) --------------------------------
#define REG_DISPCNT  (*(volatile uint16_t*)0x04000000)
#define REG_DISPSTAT (*(volatile uint16_t*)0x04000004)
#define REG_VCOUNT   (*(volatile uint16_t*)0x04000006)
#define REG_KEYINPUT (*(volatile uint16_t*)0x04000130)
static volatile uint16_t* const kVRAM = (volatile uint16_t*)0x06000000;

// DirectSound (Mode A): DMA1 streams 8-bit signed PCM into FIFO A, clocked by Timer0 overflow.
#define REG_SOUNDCNT_H (*(volatile uint16_t*)0x04000082)
#define REG_SOUNDCNT_X (*(volatile uint16_t*)0x04000084)
#define REG_TM0CNT_L   (*(volatile uint16_t*)0x04000100)
#define REG_TM0CNT_H   (*(volatile uint16_t*)0x04000102)
#define REG_DMA1SAD    (*(volatile uint32_t*)0x040000BC)
#define REG_DMA1DAD    (*(volatile uint32_t*)0x040000C0)
#define REG_DMA1CNT_H  (*(volatile uint16_t*)0x040000C6)
constexpr uint32_t kFIFO_A_ADDR = 0x040000A0;
// DMA1 control: enable | special(sound-FIFO) timing | 32-bit | repeat | dest fixed.
constexpr uint16_t kDmaSoundCnt = 0x8000 | 0x3000 | 0x0400 | 0x0200 | 0x0040; // 0xB640
// SOUNDCNT_H: DSA volume 100% | DSA -> Right | DSA -> Left | DSA timer = Timer0 | reset FIFO A.
constexpr uint16_t kSndCntH_DSA = 0x0004 | 0x0100 | 0x0200 | 0x0800;          // 0x0B04

constexpr uint16_t kMode3   = 0x0003;
constexpr uint16_t kBg2On   = 0x0400;
constexpr int      kScreenW = 240;
constexpr int      kScreenH = 160;

// GBA REG_KEYINPUT is active-low; bit set = NOT pressed. Bit order on the hardware:
enum GbaKey { GK_A=0, GK_B=1, GK_SELECT=2, GK_START=3, GK_RIGHT=4, GK_LEFT=5, GK_UP=6, GK_DOWN=7, GK_R=8, GK_L=9 };

// Canonical phx::Button bit order (must match phx/input/input.h):
enum PhxBtn { PB_UP=0, PB_DOWN=1, PB_LEFT=2, PB_RIGHT=3, PB_A=4, PB_B=5, PB_X=6, PB_Y=7, PB_L=8, PB_R=9, PB_START=10, PB_SELECT=11 };

// ---- backend state --------------------------------------------------------------------
struct GbaGfx { phx_soft_fb fb; };
GbaGfx   g_gfx = { { nullptr, 0, 0 } };
uint64_t g_step_ns = 1000000000ull / 60;
uint64_t g_vtime   = 0;

const void* g_bundle      = nullptr;     // ROM-embedded asset bundle (no FS on GBA)
size_t      g_bundle_size = 0;

// When a hardware render backend (the PPU backend) owns the display it programs DISPCNT/VRAM/
// OAM/PALRAM itself and the silicon scans it out — so present() must NOT do the Mode-3 RGBA→
// BGR555 blit (that would clobber the tile data living at 0x06000000 in Mode 0). The backend
// flips this on at init via phx_gba_set_direct(); the default (software tier) path is unchanged.
bool g_direct_present = false;

// ---- DirectSound device --------------------------------------------------------------------
// The GBA has no threads, so (unlike SDL/PSP) the audio "callback" is pumped synchronously once
// per frame at VBlank from present(): a classic double buffer — DMA1 plays one 8-bit buffer while
// the game fills the other. The game supplies the SAME fill contract as the other backends; the
// platform downmixes its stereo S16 to mono S8 for the FIFO. The platform owns the DMA/timer/FIFO
// but not the mixer (the game's fill does the mixing). Started via phx_gba_audio_start().
typedef void (*phx_audio_fill)(void* user, int16_t* out, int frames);

constexpr int kAudioBufCap = 320;          // samples per buffer (>= one frame at the chosen rate)

struct GbaAudio {
    phx_audio_fill fill = nullptr;
    void*          user = nullptr;
    int            spf  = 0;               // samples per frame = rate/60
    int            cur  = 0;               // buffer currently handed to DMA
    bool           running = false;
};
GbaAudio g_audio;
int8_t   g_audio_buf[2][kAudioBufCap];     // the two DMA source buffers (8-bit signed PCM)
int16_t  g_audio_scratch[kAudioBufCap * 2];// stereo S16 the mixer fills, before downmix

// Fill one S8 buffer from the game's mixer: produce spf stereo S16 frames, downmix to mono S8.
void gba_audio_render(int8_t* dst) {
    if (!g_audio.fill) { for (int i = 0; i < g_audio.spf; ++i) dst[i] = 0; return; }
    g_audio.fill(g_audio.user, g_audio_scratch, g_audio.spf);
    for (int i = 0; i < g_audio.spf; ++i) {
        int v = (int(g_audio_scratch[2 * i]) + int(g_audio_scratch[2 * i + 1])) >> 1; // L+R average
        v >>= 8;                                                                       // S16 -> S8
        if (v >  127) v =  127;
        if (v < -128) v = -128;
        dst[i] = int8_t(v);
    }
}

// Point DMA1 at a freshly filled buffer (restart resets its internal source pointer).
inline void gba_audio_kick(int8_t* buf) {
    REG_DMA1CNT_H = 0;                      // disable so re-enable re-latches SAD
    REG_DMA1SAD   = reinterpret_cast<uint32_t>(buf);
    REG_DMA1DAD   = kFIFO_A_ADDR;
    REG_DMA1CNT_H = kDmaSoundCnt;
}

// Called at VBlank from present(): play the buffer filled last frame, refill the other.
void gba_audio_pump(void) {
    if (!g_audio.running) return;
    g_audio.cur ^= 1;                       // the other buffer was filled on the previous pump
    gba_audio_kick(g_audio_buf[g_audio.cur]);
    gba_audio_render(g_audio_buf[g_audio.cur ^ 1]);   // refill the one just finished
}

inline void vblank_wait() {
    while (REG_VCOUNT >= kScreenH) {}    // wait until we're out of any current vblank
    while (REG_VCOUNT <  kScreenH) {}    // then wait for the start of the next one
}

inline uint16_t to_bgr555(uint32_t rgba) {       // soft fb is R | G<<8 | B<<16 | A<<24
    uint32_t r = (rgba       & 0xFF) >> 3;
    uint32_t g = ((rgba >> 8) & 0xFF) >> 3;
    uint32_t b = ((rgba >> 16)& 0xFF) >> 3;
    return uint16_t(r | (g << 5) | (b << 10));
}

int gba_init(const phx_platform_desc* desc) {
    int w = desc->width  > 0 ? desc->width  : kScreenW;
    int h = desc->height > 0 ? desc->height : kScreenH;
    if (w > kScreenW) w = kScreenW;
    if (h > kScreenH) h = kScreenH;
    // If a hardware render backend already took over the display (phx_gba_set_direct() called
    // before boot), the software framebuffer is never touched — allocate a 1x1 stub so a native
    // 240x160 logical resolution doesn't cost 150 KB of EWRAM the PPU path won't use.
    if (g_direct_present) { w = 1; h = 1; }
    g_gfx.fb.w = w; g_gfx.fb.h = h;
    g_gfx.fb.pixels = static_cast<uint32_t*>(malloc(size_t(w) * size_t(h) * sizeof(uint32_t)));
    if (!g_gfx.fb.pixels) return 1;
    REG_DISPCNT = kMode3 | kBg2On;
    g_vtime = 0;
    return 0;
}
void gba_shutdown(void) { free(g_gfx.fb.pixels); g_gfx.fb.pixels = nullptr; }

uint64_t gba_clock_ns(void) { uint64_t t = g_vtime; g_vtime += g_step_ns; return t; }
void     gba_sleep_ns(uint64_t) {}

int  gba_pump_events(void) { return 1; }          // a console runs until power-off

// Convert the RGBA8 backbuffer to BGR555 and blit to Mode 3 VRAM, integer-scaled + centered.
void gba_present(void) {
    vblank_wait();
    gba_audio_pump();                        // swap/refill DirectSound buffers (no-op if audio off)
    if (g_direct_present) return;            // PPU backend already scanned the frame from VRAM/OAM
    const int w = g_gfx.fb.w, h = g_gfx.fb.h;
    if (!g_gfx.fb.pixels || w <= 0 || h <= 0) return;
    int sx = kScreenW / w, sy = kScreenH / h;
    int s = sx < sy ? sx : sy; if (s < 1) s = 1;
    const int ox = (kScreenW - w * s) / 2, oy = (kScreenH - h * s) / 2;
    const uint32_t* src = g_gfx.fb.pixels;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint16_t c = to_bgr555(src[y * w + x]);
            const int bx = ox + x * s, by = oy + y * s;
            for (int dy = 0; dy < s; ++dy) {
                volatile uint16_t* row = kVRAM + size_t(by + dy) * kScreenW + bx;
                for (int dx = 0; dx < s; ++dx) row[dx] = c;
            }
        }
    }
}

phx_gfx*   gba_gfx(void)   { return reinterpret_cast<phx_gfx*>(&g_gfx); }
phx_audio* gba_audio(void) { return nullptr; }   // DMA/timer sound backend is future work

void gba_poll_input(phx_input_raw* out) {
    for (size_t i = 0; i < sizeof(*out); ++i) reinterpret_cast<uint8_t*>(out)[i] = 0;
    const uint16_t keys = uint16_t(~REG_KEYINPUT & 0x03FF);     // active-high pressed mask
    uint32_t b = 0;
    if (keys & (1u<<GK_UP))     b |= 1u<<PB_UP;
    if (keys & (1u<<GK_DOWN))   b |= 1u<<PB_DOWN;
    if (keys & (1u<<GK_LEFT))   b |= 1u<<PB_LEFT;
    if (keys & (1u<<GK_RIGHT))  b |= 1u<<PB_RIGHT;
    if (keys & (1u<<GK_A))      b |= 1u<<PB_A;
    if (keys & (1u<<GK_B))      b |= 1u<<PB_B;
    if (keys & (1u<<GK_L))      b |= 1u<<PB_L;
    if (keys & (1u<<GK_R))      b |= 1u<<PB_R;
    if (keys & (1u<<GK_START))  b |= 1u<<PB_START;
    if (keys & (1u<<GK_SELECT)) b |= 1u<<PB_SELECT;
    out->buttons = b;
    out->pointer_x = -1; out->pointer_y = -1;
}

// No filesystem: the single bundle is linked into the ROM and registered before run().
struct GbaFile { const void* data; size_t size; };
GbaFile g_file;

phx_file* gba_open(const char* /*path*/, size_t* out_size) {
    if (!g_bundle) { if (out_size) *out_size = 0; return nullptr; }
    g_file.data = g_bundle; g_file.size = g_bundle_size;
    if (out_size) *out_size = g_bundle_size;
    return reinterpret_cast<phx_file*>(&g_file);
}
const void* gba_map(phx_file* f) { return f ? reinterpret_cast<GbaFile*>(f)->data : nullptr; }
void        gba_close(phx_file*) {}

void gba_log(phx_log_level, const char*) {}        // no console on hardware

const phx_platform g_gba_platform = {
    gba_init, gba_shutdown,
    gba_clock_ns, gba_sleep_ns,
    gba_pump_events, gba_present,
    gba_gfx, gba_audio,
    gba_poll_input,
    gba_open, gba_map, gba_close,
    gba_log,
};

} // namespace

extern "C" const phx_platform* phx_platform_get(void) { return &g_gba_platform; }
extern "C" phx_soft_fb phx_gfx_soft_lock(phx_gfx* gfx) { return reinterpret_cast<GbaGfx*>(gfx)->fb; }

// Host/game hook (not part of the seam): register the ROM-embedded asset bundle.
extern "C" void phx_gba_set_bundle(const void* data, size_t size) { g_bundle = data; g_bundle_size = size; }

// Hook (not part of the seam): a hardware render backend calls this to take over the display,
// so present() skips the Mode-3 software blit. See g_direct_present above.
extern "C" void phx_gba_set_direct(int on) { g_direct_present = on != 0; }

// Start DirectSound: configure SOUNDCNT, Timer0 at `rate`, and DMA1 -> FIFO A, then prime both
// buffers from the game's fill. Pumped each frame by present(). Same contract as the other
// backends' audio_start. Returns 0 on success. The GBA runs ~16.78 MHz, so Timer0's reload sets
// the sample rate: reload = 65536 - (16777216 / rate).
extern "C" int phx_gba_audio_start(int rate, phx_audio_fill fill, void* user) {
    if (rate <= 0) rate = 16384;
    g_audio.spf = rate / 60;
    if (g_audio.spf > kAudioBufCap) g_audio.spf = kAudioBufCap;
    g_audio.fill = fill; g_audio.user = user; g_audio.cur = 0; g_audio.running = true;

    gba_audio_render(g_audio_buf[0]);        // prime both buffers before the DMA starts
    gba_audio_render(g_audio_buf[1]);

    REG_SOUNDCNT_X = 0x0080;                  // master sound enable
    REG_SOUNDCNT_H = kSndCntH_DSA;           // DSA 100% to L+R, Timer0, FIFO A reset

    gba_audio_kick(g_audio_buf[0]);          // DMA1 streams buffer 0 into FIFO A

    const uint16_t reload = uint16_t(65536 - (16777216 / rate));
    REG_TM0CNT_L = reload;
    REG_TM0CNT_H = 0x0080;                    // Timer0 enable, 1:1 prescaler -> overflow at `rate`
    return 0;
}
extern "C" void phx_gba_audio_stop(void) {
    g_audio.running = false;
    REG_TM0CNT_H  = 0;                        // stop the sample clock
    REG_DMA1CNT_H = 0;                        // stop the FIFO DMA
    REG_SOUNDCNT_X = 0;                       // master sound off
}

#endif // PHX_TARGET_GBA
