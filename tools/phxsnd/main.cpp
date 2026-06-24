// tools/phxsnd/main.cpp — the audio converter (docs/08 §5). Host-only.
//   phxsnd --out tone.phxsnd [--name tone] [--target 0|1|2] <tone.wav>
// Decodes a RIFF/WAVE file (PCM 8/16-bit, mono/stereo) to mono 16-bit at the file rate and emits
// a `.phxsnd` intermediate (a one-source bundle: the Sound asset) that `phxpack` merges into
// assets.phxp. (Per-target codecs — GBA 8-bit, PSP ADPCM — are a future encoding step; the
// runtime mixer currently consumes mono16, so that is what is baked.)
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
            std::printf("usage: phxsnd --out FILE.phxsnd [--name N] [--target 0|1|2] <sound.wav>\n");
            return 0;
        } else in = a;
    }
    if (out.empty() || in.empty()) { std::fprintf(stderr, "phxsnd: need --out and a .wav input (try --help)\n"); return 2; }

    phxtool::BundleWriter w{uint8_t(target)};
    if (!phxtool::build_wav(w, in, name.empty() ? phxtool::stem(out) : name)) return 1;
    if (!w.write(out)) { std::fprintf(stderr, "phxsnd: cannot write '%s'\n", out.c_str()); return 1; }
    std::printf("phxsnd: wrote %s\n", out.c_str());
    return 0;
}
