// tools/phxviz/main.cpp — the visualization-track converter (docs/08 §5, sibling of phxsnd).
// Host-only.
//   phxviz --out song.phxviz [--name viz] [--rate 18157] <song.wav>
// Analyses a PCM WAV into a per-video-frame stream (VizHeader + VizFrame records, see
// examples/miracle-player/src/viz.h): log-spaced FFT bands, RMS loudness, spectral-flux onset.
// Emits a one-asset `.phxviz` bundle (a generic Blob) that `phxpack` merges into assets.phxp,
// read zero-copy by the ROM. `--rate` is the target audio device rate the stream aligns to
// (default = the GBA vblank-locked 18157 Hz). MP3 is pre-decoded to WAV on the host (ffmpeg);
// no MP3 decoder ships in-tree.
#include "builders.h"

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    std::string out, name, in;
    uint32_t rate = 18157;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--out"  && i + 1 < argc) out  = argv[++i];
        else if (a == "--name" && i + 1 < argc) name = argv[++i];
        else if (a == "--rate" && i + 1 < argc) rate = uint32_t(std::atoi(argv[++i]));
        else if (a == "--help" || a == "-h") {
            std::printf("usage: phxviz --out FILE.phxviz [--name N] [--rate HZ] <song.wav>\n");
            return 0;
        } else in = a;
    }
    if (out.empty() || in.empty() || rate == 0) {
        std::fprintf(stderr, "phxviz: need --out, a positive --rate, and a .wav input (try --help)\n");
        return 2;
    }

    phxtool::BundleWriter w{2};   // a Blob is not per-target encoded; tier is irrelevant here
    if (!phxtool::build_viz(w, in, name.empty() ? phxtool::stem(out) : name, rate)) return 1;
    if (!w.write(out)) { std::fprintf(stderr, "phxviz: cannot write '%s'\n", out.c_str()); return 1; }
    std::printf("phxviz: wrote %s\n", out.c_str());
    return 0;
}
