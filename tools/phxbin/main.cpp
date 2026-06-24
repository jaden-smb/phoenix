// tools/phxbin/main.cpp — the data-table converter (docs/08 §6). Host-only.
//   phxbin --out items.phxbin [--name items] [--header items.gen.h] [--target 0|1|2] <items.json>
// Bakes authored game data (JSON) into a flat, fixed-stride binary table the engine reads as a
// BlobView, and (optionally) emits a generated POD accessor header guaranteeing the struct and
// the blob agree. Emits a `.phxbin` intermediate (a one-source bundle: the table Blob) that
// `phxpack` merges into assets.phxp. Input schema:
//   { "struct": "ItemRecord",
//     "fields":  [ {"name":"id","type":"u16"}, {"name":"price","type":"u32"} ],
//     "records": [ {"id":1,"price":100}, {"id":2,"price":250} ] }
// Field types: u8/i8/u16/i16/u32/i32/f32. No runtime JSON parser ships.
#include "builders.h"

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    std::string out, name, header, in;
    int target = 2;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--out"    && i + 1 < argc) out    = argv[++i];
        else if (a == "--name"   && i + 1 < argc) name   = argv[++i];
        else if (a == "--header" && i + 1 < argc) header = argv[++i];
        else if (a == "--target" && i + 1 < argc) target = std::atoi(argv[++i]);
        else if (a == "--help" || a == "-h") {
            std::printf("usage: phxbin --out FILE.phxbin [--name N] [--header FILE.gen.h] [--target 0|1|2] <data.json>\n");
            return 0;
        } else in = a;
    }
    if (out.empty() || in.empty()) { std::fprintf(stderr, "phxbin: need --out and a .json input (try --help)\n"); return 2; }

    phxtool::BundleWriter w{uint8_t(target)};
    if (!phxtool::build_bin(w, in, name.empty() ? phxtool::stem(out) : name, header)) return 1;
    if (!w.write(out)) { std::fprintf(stderr, "phxbin: cannot write '%s'\n", out.c_str()); return 1; }
    std::printf("phxbin: wrote %s\n", out.c_str());
    return 0;
}
