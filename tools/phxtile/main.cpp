// tools/phxtile/main.cpp — the tilemap converter (docs/08 §4). Host-only.
//   phxtile --out level1.phxtmap [--name level1] [--target 0|1|2] <level1.tmj>
// Reads an open Tiled JSON map and emits a `.phxtmap` intermediate (a one-source bundle: the
// Tilemap layers + the object-group Spawns) that `phxpack` merges into assets.phxp. Solid tiles
// in the map double as the collision layer (the physics TileGrid reads them directly).
#include "builders.h"

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    std::string out, name, in;
    int target = 2;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--out"    && i + 1 < argc) out    = argv[++i];
        else if (a == "--name"   && i + 1 < argc) name   = argv[++i];
        else if (a == "--target" && i + 1 < argc) target = std::atoi(argv[++i]);
        else if (a == "--help" || a == "-h") {
            std::printf("usage: phxtile --out FILE.phxtmap [--name N] [--target 0|1|2] <map.tmj>\n");
            return 0;
        } else in = a;
    }
    if (out.empty() || in.empty()) { std::fprintf(stderr, "phxtile: need --out and a .tmj input (try --help)\n"); return 2; }

    phxtool::BundleWriter w{uint8_t(target)};
    if (!phxtool::build_tmj(w, in, name.empty() ? phxtool::stem(out) : name)) return 1;
    if (!w.write(out)) { std::fprintf(stderr, "phxtile: cannot write '%s'\n", out.c_str()); return 1; }
    std::printf("phxtile: wrote %s\n", out.c_str());
    return 0;
}
