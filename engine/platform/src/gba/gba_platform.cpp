// engine/platform/src/gba/gba_platform.cpp — the Game Boy Advance backend of the C seam.
// Bring-up uses BG Mode 3 (240x160, 16bpp BGR555 bitmap): the software renderer draws into an
// RGBA8 backbuffer (the soft-fb the engine already targets), and present() converts + blits it
// to VRAM, nearest-scaled to fill the screen. Keypad -> canonical buttons; the clock is virtual
// (one sim step per frame, null-backend convention). There is no filesystem, so assets are a
// ROM-embedded bundle handed in via
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
#define REG_WAITCNT  (*(volatile uint16_t*)0x04000204)
#define REG_IE       (*(volatile uint16_t*)0x04000200)
#define REG_IF       (*(volatile uint16_t*)0x04000202)
#define REG_IME      (*(volatile uint16_t*)0x04000208)
// The BIOS IRQ dispatch jumps (in ARM state) to the address stored here; 0x03007FF8 is the
// BIOS's own "IntrWait acknowledged" flag halfword our handler mirrors IF into.
#define MMIO_IRQ_VECTOR (*(volatile uint32_t*)0x03007FFC)
// Game Pak access timing + prefetch. The BIOS leaves WAITCNT at 0x0000 — the SLOWEST ROM
// timing (WS0 4/2 wait states, prefetch buffer OFF) — and all code/const-data executes from
// cartridge ROM, so every instruction fetch pays it. 0x4317 = prefetch ON + WS0 3/1, the
// standard all-carts-safe fast setting; a one-write, global speedup for every GBA build
// (software AND PPU paths). Set once at init before anything hot runs.
constexpr uint16_t kWaitCntFast = 0x4317;
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
// Virtual clock, same convention as the null backend: pump_events() (once per frame)
// advances one sim step; each clock_ns() read additionally ticks 1 µs so successive reads
// stay strictly ordered. NEVER advance a full step per read: App::run reads the clock five
// times per frame (accumulator + four profiler stamps), and per-read stepping made the
// accumulator see ~83 ms/frame — saturating the spiral-of-death clamp at 5 fixed updates
// EVERY frame, ~5× the sim cost on a 16.78 MHz ARM7 (the "Emberwing runs slow" bug).
uint64_t g_step_ns = 1000000000ull / 60;
uint64_t g_vtime   = 0;
constexpr uint64_t kCallTickNs = 1000;     // 1 µs per clock read (see above)

const void* g_bundle      = nullptr;     // ROM-embedded asset bundle (no FS on GBA)
size_t      g_bundle_size = 0;

// When a hardware render backend (the PPU backend) owns the display it programs DISPCNT/VRAM/
// OAM/PALRAM itself and the silicon scans it out — so present() must NOT do the Mode-3 RGBA→
// BGR555 blit (that would clobber the tile data living at 0x06000000 in Mode 0). The backend
// flips this on at init via phx_gba_set_direct(); the default (software tier) path is unchanged.
bool g_direct_present = false;

// ---- DirectSound device --------------------------------------------------------------------
// The audio "callback" is pumped from the VBLANK INTERRUPT — the GBA's stand-in for the audio
// thread the SDL/PSP backends get from their OS: a classic double buffer, DMA1 plays one 8-bit
// buffer while the pump fills the other, swapped at exactly 60 Hz by the ISR no matter what the
// main loop is doing. (It used to be pumped synchronously from present(); one slow game frame
// then let the DMA run off the end of its buffer into garbage — frame drops were audible as
// crackle. Frame-locked hardware wants a frame-locked pump, not a game-loop-locked one.)
// The game supplies the SAME fill contract as the other backends and the SPSC command-queue
// discipline carries over unchanged: game code produces intents, the ISR drains them into the
// mixer. The platform downmixes the mixer's stereo S16 to mono S8 for the FIFO; it owns the
// DMA/timer/FIFO but not the mixer (the game's fill does the mixing). phx_gba_audio_start()
// installs the handler and enables the VBlank IRQ.
typedef void (*phx_audio_fill)(void* user, int16_t* out, int frames);

constexpr int kAudioBufCap = 320;          // samples per buffer (>= one frame at the chosen rate)

// The GBA scans one video frame in exactly 280896 CPU cycles, and Timer0 clocks one sample
// every (65536 - reload) cycles — so DMA1 consumes 280896/cycles_per_sample samples per frame.
// The pump swaps buffers once per frame, so that consumption MUST be an integer: pick a
// vblank-locked rate where cycles_per_sample divides 280896 (e.g. 18157 Hz -> 924 cycles ->
// exactly 304 samples/frame). A non-locked rate (16384 Hz -> 274.3125 samples/frame) makes
// the DMA run past the filled buffer and get its source yanked mid-sample every single frame:
// a guaranteed 60 Hz crackle even at full frame rate.
constexpr uint32_t kCyclesPerFrame = 280896;
constexpr uint32_t kCpuHz          = 16777216;

struct GbaAudio {
    phx_audio_fill fill = nullptr;
    void*          user = nullptr;
    int            spf  = 0;               // samples DMA1 consumes per video frame (see above)
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

// Reach the current-or-next vblank. Deliberately NOT "wait for the next vblank START": if
// the frame's work ran a few scanlines long and we arrive already inside vblank, that must
// cost those scanlines — the old two-spin version waited out the whole next frame, hard-
// quantizing a 5%-over-budget game to 30 fps. present() returns at the vblank END (draw
// start) instead, so one return per hardware frame still holds when the game is fast.
inline void vblank_wait() {
    while (REG_VCOUNT < kScreenH) {}     // no-op if we are already inside vblank
}

inline uint16_t to_bgr555(uint32_t rgba) {       // soft fb is R | G<<8 | B<<16 | A<<24
    uint32_t r = (rgba       & 0xFF) >> 3;
    uint32_t g = ((rgba >> 8) & 0xFF) >> 3;
    uint32_t b = ((rgba >> 16)& 0xFF) >> 3;
    return uint16_t(r | (g << 5) | (b << 10));
}

int gba_init(const phx_platform_desc* desc) {
    REG_WAITCNT = kWaitCntFast;              // fast ROM timing + prefetch (see note above)
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

uint64_t gba_clock_ns(void) { uint64_t t = g_vtime; g_vtime += kCallTickNs; return t; }
void     gba_sleep_ns(uint64_t) {}

// A console runs until power-off; one sim step per frame (frame pacing itself comes from
// present()'s vblank_wait — this just keeps the accumulator fed at exactly 60 Hz).
int  gba_pump_events(void) { g_vtime += g_step_ns; return 1; }

// Convert the RGBA8 backbuffer to BGR555 and blit to Mode 3 VRAM, integer-scaled + centered.
// The body lives in gba_present_hot (IWRAM/ARM, defined at the BOTTOM of this namespace):
// it carries the inlined audio pump (S16->S8 downmix of a whole frame's samples) and, on the
// software tier, the full-screen blit — both too hot for Thumb-from-ROM. gba_present itself
// stays a Thumb shim, and the attributed definition comes after g_gba_platform, because this
// GCC rejects a Thumb TU's static initializer once a target("arm") definition precedes it.
void gba_present_hot(void);
void gba_present(void) { gba_present_hot(); }

phx_gfx*   gba_gfx(void)   { return reinterpret_cast<phx_gfx*>(&g_gfx); }
// The seam's generic audio handle stays null: DirectSound is driven through the game-side
// phx_gba_audio_start() hook below (no threads on GBA, so the fill is pumped by present()).
phx_audio* gba_audio(void) { return nullptr; }

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

// Persistence: battery-backed cartridge SRAM at 0x0E000000 (single slot; key ignored). SRAM is
// an 8-bit bus — it MUST be accessed one byte at a time (no 16/32-bit loads/stores), so these
// copy byte-wise. There is no "absent" state to report: uninitialised SRAM reads back as garbage,
// so the caller validates a magic/version in the blob to tell a real save from a fresh cart.
static volatile uint8_t* const kSRAM = (volatile uint8_t*)0x0E000000;
constexpr uint32_t kSramSize = 32u * 1024u;
// Emulators/flash carts auto-detect the save type by scanning the ROM for this signature; keep
// it in the binary (the GBA link doesn't gc-sections, so `used` is enough) so SRAM saves are
// recognised (mGBA, VBA, hardware).
__attribute__((used)) static const char kSramSig[] = "SRAM_V113";

int gba_save(const char* /*key*/, const void* data, uint32_t size) {
    if (size > kSramSize) return 1;
    const uint8_t* s = static_cast<const uint8_t*>(data);
    for (uint32_t i = 0; i < size; ++i) kSRAM[i] = s[i];
    return 0;
}
int gba_load(const char* /*key*/, void* out, uint32_t cap, uint32_t* out_size) {
    if (cap > kSramSize) cap = kSramSize;
    uint8_t* d = static_cast<uint8_t*>(out);
    for (uint32_t i = 0; i < cap; ++i) d[i] = kSRAM[i];
    if (out_size) *out_size = cap;
    return 0;                                       // caller's magic check decides if it's valid
}

void gba_log(phx_log_level, const char*) {}        // no console on hardware

const phx_platform g_gba_platform = {
    gba_init, gba_shutdown,
    gba_clock_ns, gba_sleep_ns,
    gba_pump_events, gba_present,
    gba_gfx, gba_audio,
    gba_poll_input,
    gba_open, gba_map, gba_close,
    gba_save, gba_load,
    gba_log,
};

// The IWRAM/ARM present worker (see the shim above for why it is defined down here).
__attribute__((section(".iwram"), target("arm"), long_call, noinline))
void gba_present_hot(void) {
    vblank_wait();                           // reach (or already be inside) this frame's vblank
    if (!g_direct_present) {                 // software tier: blit during vblank
        const int w = g_gfx.fb.w, h = g_gfx.fb.h;
        if (g_gfx.fb.pixels && w > 0 && h > 0) {
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
    }
    while (REG_VCOUNT >= kScreenH) {}        // leave vblank: exactly one return per frame
}

// The VBlank pump: swap DMA buffers + refill, at a hard 60 Hz from the ISR.
__attribute__((section(".iwram"), target("arm"), noinline))
void do_audio_pump(void) {
    if (!g_audio.running) return;
    g_audio.cur ^= 1;                       // the other buffer was filled on the previous pump
    gba_audio_kick(g_audio_buf[g_audio.cur]);
    gba_audio_render(g_audio_buf[g_audio.cur ^ 1]);   // refill the one just finished
}

} // namespace

// C-named so the assembly dispatcher can `bl` it (see gba_irq_stub).
extern "C" __attribute__((section(".iwram"), target("arm"), noinline))
void gba_audio_pump_irq(void) { do_audio_pump(); }

// The raw IRQ entry the BIOS jumps to (ARM state, IRQ mode). The BIOS IRQ stack is only
// ~160 bytes — nowhere near enough for the mixer — so after acknowledging the interrupt
// this switches to SYSTEM mode, which shares the main (user) stack: legal because the
// interrupted main code cannot move SP while we run, and the user stack has kilobytes of
// headroom below it. IRQs stay masked throughout (no nesting). BIOS has already saved
// r0-r3, r12 and lr_irq; lr_sys belongs to the interrupted code, so it is saved here.
extern "C" __attribute__((section(".iwram"), target("arm"), naked))
void gba_irq_stub(void) {
    __asm__ volatile(
        "mov   r0, #0x04000000        \n"
        "add   r0, r0, #0x200         \n"
        "ldrh  r1, [r0, #2]           \n"   // r1 = REG_IF (pending)
        "strh  r1, [r0, #2]           \n"   // acknowledge all pending
        "mov   r2, #0x03000000        \n"   // mirror into the BIOS flags at 0x03007FF8
        "orr   r2, r2, #0x7F00        \n"
        "orr   r2, r2, #0x00F8        \n"
        "ldrh  r3, [r2]               \n"
        "orr   r3, r3, r1             \n"
        "strh  r3, [r2]               \n"
        "tst   r1, #1                 \n"   // bit 0 = VBlank
        "bxeq  lr                     \n"
        "mrs   r2, cpsr               \n"
        "bic   r3, r2, #0x1F          \n"
        "orr   r3, r3, #0x1F          \n"   // SYSTEM mode (I bit stays set: no nesting)
        "msr   cpsr_c, r3             \n"
        "push  {r2, lr}               \n"   // old cpsr + the interrupted code's lr
        "bl    gba_audio_pump_irq     \n"
        "pop   {r2, lr}               \n"
        "msr   cpsr_c, r2             \n"   // back to IRQ mode
        "bx    lr                     \n"
    );
}

extern "C" const phx_platform* phx_platform_get(void) { return &g_gba_platform; }
extern "C" phx_soft_fb phx_gfx_soft_lock(phx_gfx* gfx) { return reinterpret_cast<GbaGfx*>(gfx)->fb; }

// Host/game hook (not part of the seam): register the ROM-embedded asset bundle.
extern "C" void phx_gba_set_bundle(const void* data, size_t size) { g_bundle = data; g_bundle_size = size; }

// Hook (not part of the seam): a hardware render backend calls this to take over the display,
// so present() skips the Mode-3 software blit. See g_direct_present above.
extern "C" void phx_gba_set_direct(int on) { g_direct_present = on != 0; }

// Start DirectSound: configure SOUNDCNT, Timer0 at `rate`, and DMA1 -> FIFO A, prime both
// buffers from the game's fill, then install the VBlank-IRQ pump (see the DirectSound note
// above). Same contract as the other backends' audio_start. Returns 0 on success. Timer0's
// reload sets the sample rate: reload = 65536 - (16777216 / rate). Use a vblank-locked rate
// (see kCyclesPerFrame note); the buffer size is derived from actual per-frame DMA
// consumption, NOT rate/60 — the GBA frame is 59.73 Hz, so rate/60 under-fills by a
// fraction of a sample every frame.
extern "C" int phx_gba_audio_start(int rate, phx_audio_fill fill, void* user) {
    if (rate <= 0) rate = 18157;                              // vblank-locked default
    const uint32_t cps = kCpuHz / uint32_t(rate);             // Timer0 cycles per sample
    g_audio.spf = int((kCyclesPerFrame + cps / 2) / cps);     // samples consumed per frame
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

    // Frame-locked pump: VBlank IRQ -> gba_irq_stub -> gba_audio_pump_irq. Everything above
    // is primed before the first interrupt can fire.
    MMIO_IRQ_VECTOR = reinterpret_cast<uintptr_t>(&gba_irq_stub);
    REG_DISPSTAT   |= 0x0008;                 // VBlank IRQ request enable
    REG_IE         |= 0x0001;                 // VBlank
    REG_IME         = 1;
    return 0;
}
extern "C" void phx_gba_audio_stop(void) {
    REG_IME = 0;                              // no pump mid-teardown
    g_audio.running = false;
    REG_IE &= uint16_t(~0x0001);
    REG_DISPSTAT &= uint16_t(~0x0008);
    REG_TM0CNT_H  = 0;                        // stop the sample clock
    REG_DMA1CNT_H = 0;                        // stop the FIFO DMA
    REG_SOUNDCNT_X = 0;                       // master sound off
}

#endif // PHX_TARGET_GBA
