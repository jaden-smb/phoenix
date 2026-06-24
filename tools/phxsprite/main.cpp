// tools/phxsprite/main.cpp — the sprite/atlas converter (docs/08 §3). Host-only.
//   phxsprite --out hero.phxspr [--name hero] [--target 0|1|2] <hero.sprdef | hero.json>
// Slices a sheet PNG into a frame grid + named animation clips and emits a `.phxspr`
// intermediate (a one-source bundle: the sheet Texture + the Sprite metadata) that `phxpack`
// merges into assets.phxp. Input is either a `.sprdef` text def or a JSON sidecar:
//   { "image": "hero.png", "tile": 16, "animations": { "run": {"frames":[2,3], "fps":12, "loop":true} } }
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
            std::printf("usage: phxsprite --out FILE.phxspr [--name N] [--target 0|1|2] <def.sprdef|def.json>\n");
            return 0;
        } else in = a;
    }
    if (out.empty() || in.empty()) { std::fprintf(stderr, "phxsprite: need --out and an input def (try --help)\n"); return 2; }

    phxtool::BundleWriter w{uint8_t(target)};
    if (!phxtool::build_sprite(w, in, name.empty() ? phxtool::stem(out) : name)) return 1;
    if (!w.write(out)) { std::fprintf(stderr, "phxsprite: cannot write '%s'\n", out.c_str()); return 1; }
    std::printf("phxsprite: wrote %s\n", out.c_str());
    return 0;
}
