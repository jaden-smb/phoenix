// examples/platformer/src/bake_main.cpp — host-only tool: bakes the example's .phxp bundle to
// a file so it can be embedded in the GBA ROM (which has no filesystem). Runs the SAME importers
// as the host game (bake.h); see `make gba-platformer`. Not part of `make check`.
#include "bake.h"
#include <cstdio>

int main(int argc, char** argv) {
    const char* out = argc > 1 ? argv[1] : "platformer.phxp";
    if (!game::bake_platformer_assets(out)) {
        std::fprintf(stderr, "bake_main: failed to write %s\n", out);
        return 1;
    }
    std::printf("baked %s\n", out);
    return 0;
}
