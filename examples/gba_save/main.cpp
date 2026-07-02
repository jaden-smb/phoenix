// examples/gba_save/main.cpp — verifies the GBA battery-SRAM save path on emulated hardware.
// The host file path of the save seam is already covered by the platformer's save→reload test;
// this exercises the GBA DEVICE path: gba_save()/gba_load() copying byte-wise over the 8-bit
// SRAM bus at 0x0E000000 (the platformer's actual persistence on a real cart).
//
// Round-trip: if SRAM already holds a magic-tagged blob (a PREVIOUS run — mGBA persists SRAM
// to a .sav next to the ROM), count it as persistence proof; then save a fresh blob through
// the seam, load it back, and compare. The GBA has no console, so verdicts are published in
// GDB-readable globals (mGBA GDB stub, same as the platformer/audio ROMs):
//   phx_gba_save_verdict    -1 running, 1 round-trip pass, 0 fail
//   phx_gba_save_persisted   1 iff a valid blob predated this run (set on run 2+)
//   phx_gba_save_counter     the blob's run counter
// The .sav file mGBA writes can ALSO be checked host-side for the "PHXS" magic. `make gba-save`.
#include "phx/platform/platform.h"

#include <string.h>

// Published for the GDB stub to read (volatile so they survive -O2 and land in memory).
extern "C" {
volatile int phx_gba_save_verdict   = -1;
volatile int phx_gba_save_persisted = 0;
volatile int phx_gba_save_counter   = 0;
}

namespace {
struct SaveBlob {
    uint32_t magic;      // 'PHXS'
    uint32_t version;
    uint32_t counter;    // bumped every run so run 2 sees run 1's value
    uint32_t pad;
};
constexpr uint32_t kMagic   = 0x53584850u;   // "PHXS" little-endian
constexpr uint32_t kVersion = 1;
} // namespace

int main() {
    const phx_platform* plat = phx_platform_get();
    phx_platform_desc desc{}; desc.title = "phx-gba-save"; desc.width = 240; desc.height = 160;
    if (plat->init(&desc) != 0) { for (;;) {} }

    // Wake the emulator's lazy savedata autodetect: mGBA only maps SRAM (and its .sav backing)
    // on the first WRITE, so a boot whose first SRAM access is a read would see open-bus 0xFF.
    // One byte-wise touch of the LAST SRAM byte (far outside any blob) — real HW doesn't care.
    {
        volatile uint8_t* last = (volatile uint8_t*)0x0E007FFF;
        *last = *last;
    }

    // 1. Does SRAM already hold a valid save (from a previous run / .sav file)? Uninitialised
    //    SRAM reads as garbage, so the magic+version check is the validity test (docs/02).
    SaveBlob prev{}; uint32_t got = 0;
    plat->load("slot", &prev, sizeof(prev), &got);
    const bool persisted = got == sizeof(prev) && prev.magic == kMagic && prev.version == kVersion;
    phx_gba_save_persisted = persisted ? 1 : 0;

    // 2. Round-trip through the real SRAM bus: save, load back, byte-compare.
    SaveBlob out{ kMagic, kVersion, persisted ? prev.counter + 1 : 1, 0 };
    SaveBlob in{};  got = 0;
    const bool ok = plat->save("slot", &out, sizeof(out)) == 0 &&
                    plat->load("slot", &in, sizeof(in), &got) == 0 &&
                    got == sizeof(in) && memcmp(const_cast<SaveBlob*>(&out), &in, sizeof(out)) == 0;
    phx_gba_save_counter = int(out.counter);
    phx_gba_save_verdict = ok ? 1 : 0;

    for (;;) { plat->present(); }               // spin so the GDB stub can read the verdict
    return 0;
}
