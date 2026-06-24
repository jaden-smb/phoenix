// tools/phxpack/main.cpp — the `.phxp` bundle assembler CLI. Host-only (STL allowed).
// Builds a baked, target-encoded bundle the engine mmaps, from either:
//   (a) author-friendly SOURCE files, baked directly via the shared builders (builders.h):
//         *.png/*.ppm -> Texture   *.tmcsv/*.tmj -> Tilemap(+Spawns)
//         *.sprdef    -> Texture+Sprite          *.wav -> Sound
//   (b) pre-baked INTERMEDIATE files emitted by the per-format converters
//       (phxsprite/phxtile/phxsnd/phxbin) — each is itself a one-source bundle, MERGED in:
//         *.phxspr  *.phxtmap  *.phxsnd  *.phxbin  *.phxp
// This is the two-stage pipeline of docs/08: converters -> intermediates -> phxpack -> bundle.
// Both paths share one bake implementation (builders.h), so output is identical either way.
//
//   phxpack --out assets.phxp [--target 0|1|2] [--compress] <inputs...>
//   --compress (-z): LZSS-compress each blob (kept only where it shrinks); the runtime
//                    ResourceCache decompresses on first access (see phx/resource/lz.h).
#include "builders.h"
#include "bundle_reader.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

bool is_intermediate(const std::string& s) {
    return phxtool::ends_with(s, ".phxspr") || phxtool::ends_with(s, ".phxtmap") ||
           phxtool::ends_with(s, ".phxsnd") || phxtool::ends_with(s, ".phxbin")  ||
           phxtool::ends_with(s, ".phxp");
}

// Merge a pre-baked converter bundle into the assembler's output (names already hashed).
bool merge_intermediate(phxtool::BundleWriter& w, const std::string& in) {
    std::vector<phxtool::ReadAsset> assets;
    if (!phxtool::bundle_read(in, assets)) {
        std::fprintf(stderr, "phxpack: bad/unsupported bundle '%s'\n", in.c_str()); return false; }
    for (auto& a : assets) w.add_raw(a.hash, a.type, std::move(a.blob));
    std::printf("  + merged  %-12s %u asset(s)  (%s)\n",
                phxtool::stem(in).c_str(), unsigned(assets.size()), in.c_str());
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::string out;
    int target = 2;
    bool compress = false;
    std::vector<std::string> inputs;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--out" && i + 1 < argc)         out = argv[++i];
        else if (a == "--target" && i + 1 < argc) target = std::atoi(argv[++i]);
        else if (a == "--compress" || a == "-z")  compress = true;
        else if (a == "--help" || a == "-h") {
            std::printf("usage: phxpack --out FILE [--target 0|1|2] [--compress] <inputs...>\n"
                        "  sources:       .png .ppm .tmcsv .tmj .wav .sprdef\n"
                        "  intermediates: .phxspr .phxtmap .phxsnd .phxbin .phxp  (merged)\n");
            return 0;
        } else inputs.push_back(a);
    }
    if (out.empty()) { std::fprintf(stderr, "phxpack: --out is required (try --help)\n"); return 2; }

    phxtool::BundleWriter w{uint8_t(target)};
    w.set_compression(compress);
    for (const std::string& in : inputs) {
        const bool ok = is_intermediate(in) ? merge_intermediate(w, in)
                                            : phxtool::build_from_source(w, in);
        if (!ok) return 1;
    }

    if (!w.write(out)) { std::fprintf(stderr, "phxpack: cannot write '%s'\n", out.c_str()); return 1; }
    std::printf("phxpack: wrote %s  (%u assets, target tier %d%s)\n",
                out.c_str(), w.count(), target, compress ? ", LZ-compressed" : "");
    return 0;
}
