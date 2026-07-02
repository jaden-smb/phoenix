// examples/psp_save/main.cpp — verifies the PSP save/load seam on a real emulator/PSP.
// The host file path of the save seam is already covered by the platformer's save→reload
// test; what this exercises is the PSP DEVICE path: psp_save()/psp_load() doing real
// sceIoOpen/Write/Read against the memory stick.
//
// Round-trip: save a magic-tagged blob through the seam, load it back, compare. On a SECOND
// run the blob already exists on the stick before we write, proving persistence across boots.
// Results are reported via thread NAMEs visible in PPSSPP's log (same trick as psp_audio):
//   sceKernelCreateThread("SAVE_DEVICE_PASS"/"SAVE_DEVICE_FAIL", ...)   round-trip verdict
//   sceKernelCreateThread("SAVE_PERSIST_PASS", ...)                      only on run 2+
// Built by `make psp-save`. Run in PPSSPP twice, grep the log.
#include "phx/platform/platform.h"

#include <pspkernel.h>
#include <string.h>

PSP_MODULE_INFO("PHX_SAVE_VERIFY", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(1024);

namespace {
int exit_cb(int, int, void*) { sceKernelExitGame(); return 0; }
int cb_thread(SceSize, void*) {
    int cb = sceKernelCreateCallback("exit", exit_cb, nullptr);
    sceKernelRegisterExitCallback(cb);
    sceKernelSleepThreadCB();
    return 0;
}
void setup_callbacks() {
    int th = sceKernelCreateThread("cb", cb_thread, 0x11, 0xFA0, 0, nullptr);
    if (th >= 0) sceKernelStartThread(th, 0, nullptr);
}
void report(const char* verdict) {
    int vt = sceKernelCreateThread(verdict, cb_thread, 0x18, 0xFA0, 0, nullptr);
    (void)vt;
}

struct SaveBlob {
    uint32_t magic;      // 'PHXS'
    uint32_t version;
    uint32_t counter;    // bumped every run so run 2 sees run 1's value
    uint32_t pad;
};
constexpr uint32_t kMagic   = 0x53584850u;   // "PHXS" little-endian
constexpr uint32_t kVersion = 1;
const char*        kKey     = "phx_save_verify.bin";
} // namespace

int main() {
    setup_callbacks();
    const phx_platform* plat = phx_platform_get();

    // 1. Does a save from a PREVIOUS run exist? (Proves persistence across boots.)
    SaveBlob prev{}; uint32_t got = 0;
    const bool persisted = plat->load(kKey, &prev, sizeof(prev), &got) == 0 &&
                           got == sizeof(prev) && prev.magic == kMagic &&
                           prev.version == kVersion;
    if (persisted) report("SAVE_PERSIST_PASS");

    // 2. Round-trip through the real sceIo path: save, load back, byte-compare.
    SaveBlob out{ kMagic, kVersion, persisted ? prev.counter + 1 : 1, 0 };
    SaveBlob in{};  got = 0;
    const bool ok = plat->save(kKey, &out, sizeof(out)) == 0 &&
                    plat->load(kKey, &in, sizeof(in), &got) == 0 &&
                    got == sizeof(in) && memcmp(&out, &in, sizeof(out)) == 0;
    report(ok ? "SAVE_DEVICE_PASS" : "SAVE_DEVICE_FAIL");

    for (;;) sceKernelDelayThread(1000000);   // keep running so the verdict is observable
    return 0;
}
