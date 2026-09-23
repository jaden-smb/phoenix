// examples/tinyllm/src/bake_main.cpp — host-only tool. Two jobs, both build-time:
//
//   bake_main <out.phxp> [tier] [model.phxllm]   bake the bundle the GBA ROM embeds (the console
//                                                has no filesystem, so `make gba-tinyllm-ppu`
//                                                bakes here and links the result with bin2s)
//   bake_main --dump-fixture <out.phxllm>        write the generated tiny fixture model, which
//                                                tools/export_model.py then turns into the
//                                                committed golden token file
//
// Not part of `make check`.
#include "bake.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--dump-fixture") == 0) {
        const char* out = argc > 2 ? argv[2] : "tinyllm_fixture.phxllm";
        const std::vector<uint8_t> blob = tinyllm::build::build_fixture_blob();
        FILE* f = std::fopen(out, "wb");
        if (!f || std::fwrite(blob.data(), 1, blob.size(), f) != blob.size()) {
            if (f) std::fclose(f);
            std::fprintf(stderr, "bake_main: could not write %s\n", out);
            return 1;
        }
        std::fclose(f);
        std::printf("wrote %s (%zu bytes, crc %08x)\n", out, blob.size(),
                    unsigned(tinyllm::build::crc32(blob.data() + 52, blob.size() - 52)));
        return 0;
    }

    const char* out   = argc > 1 ? argv[1] : "tinyllm.phxp";
    const int   tier  = argc > 2 ? std::atoi(argv[2]) : 0;          // 0 = GBA
    const char* model = argc > 3 ? argv[3] : "build/tinyllm.phxllm";
    if (!tinyllm::bake_tinyllm_assets(out, uint8_t(tier), model)) {
        std::fprintf(stderr, "bake_main: failed to write %s\n", out);
        return 1;
    }
    std::printf("baked %s (tier %d)\n", out, tier);
    return 0;
}
