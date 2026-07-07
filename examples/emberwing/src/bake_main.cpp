// examples/emberwing/bake_main.cpp — host-only tool: bakes the game's .phxp bundle to a file
// so it can be embedded in the GBA ROM / PSP EBOOT (no filesystem load on console). Runs the
// SAME importers as the host game (bake.h). Not part of `make check`.
#include "bake.h"
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    const char* out = argc > 1 ? argv[1] : "emberwing.phxp";
    // Optional capability tier (0 = GBA, 1 = PSP, 2 = PC, the default): drives per-target
    // asset encoding — e.g. tier 0 resamples baked sounds down to the GBA device rate.
    const int tier = argc > 2 ? std::atoi(argv[2]) : 2;
    if (!game::bake_emberwing_assets(out, uint8_t(tier))) {
        std::fprintf(stderr, "bake_main: failed to write %s\n", out);
        return 1;
    }
    std::printf("baked %s (tier %d)\n", out, tier);
    return 0;
}
