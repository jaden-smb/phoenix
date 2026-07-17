// examples/miracle-player/src/bake_main.cpp — host-only tool: bakes the miracle-player .phxp so
// it can be embedded in the GBA ROM (which has no filesystem). Runs the SAME importers as the
// host game (bake.h): song WAV -> tier-0 resampled PCM, the offline .phxviz analysis, and the
// tileset/spark/font/heart textures. See `make gba-miracle-ppu`. Not part of `make check`.
#include "bake.h"

#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    const char* out  = argc > 1 ? argv[1] : "miracle.phxp";
    const int   tier = argc > 2 ? std::atoi(argv[2]) : 0;         // 0 = GBA (18157 Hz audio + viz)
    const char* wav  = argc > 3 ? argv[3] : "build/miracle.wav";  // pre-decoded song (ffmpeg)
    if (!miracle::bake_miracle_assets(out, uint8_t(tier), wav)) {
        std::fprintf(stderr, "bake_main: failed to write %s\n", out);
        return 1;
    }
    std::printf("baked %s (tier %d)\n", out, tier);
    return 0;
}
