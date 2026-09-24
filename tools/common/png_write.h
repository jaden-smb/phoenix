// tools/common/png_write.h — a dependency-free PNG ENCODER for the editors (the sprite/pixel
// editor saves real .png sheets that phxsprite/phxpack then bake). The mirror image of
// tools/phxpack/png.h's decoder, and round-trip tested against it in the editors suite.
//
//   - Images with <= 256 distinct RGBA colours are written INDEXED (colour type 3, 8-bit, PLTE +
//     tRNS) — pixel art almost always is, and it is ~4x smaller than RGBA.
//   - Otherwise RGBA8 (colour type 6) with a per-row filter picked by the minimum-sum-of-absolute
//     heuristic (libpng's default strategy).
//   - Deflate: LZ77 over hash chains (32 KiB window, lazy matching) + the fixed Huffman code.
//     Not zlib -9, but real compression with no dependency, and fully deterministic (the same
//     pixels always produce the same bytes, so saved sheets diff cleanly in git).
// Host-only.
#ifndef PHX_TOOLS_PNG_WRITE_H
#define PHX_TOOLS_PNG_WRITE_H

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace phxtool {
namespace pngw {

inline uint32_t crc32(const uint8_t* p, size_t n, uint32_t c = 0xFFFFFFFFu) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t v = i;
            for (int k = 0; k < 8; ++k) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            table[i] = v;
        }
        init = true;
    }
    for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c;
}

inline uint32_t adler32(const uint8_t* p, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; ++i) { a = (a + p[i]) % 65521u; b = (b + a) % 65521u; }
    return (b << 16) | a;
}

struct BitWriter {
    std::vector<uint8_t>& out;
    uint32_t acc = 0;
    int nbits = 0;
    void put(uint32_t v, int n) {            // LSB-first
        acc |= v << nbits;
        nbits += n;
        while (nbits >= 8) { out.push_back(uint8_t(acc)); acc >>= 8; nbits -= 8; }
    }
    void put_rev(uint32_t code, int n) {     // Huffman codes are sent MSB-first
        uint32_t r = 0;
        for (int i = 0; i < n; ++i) r |= ((code >> i) & 1u) << (n - 1 - i);
        put(r, n);
    }
    void flush() { if (nbits) { out.push_back(uint8_t(acc)); acc = 0; nbits = 0; } }
};

// Fixed-Huffman literal/length symbol.
inline void put_litlen(BitWriter& bw, int sym) {
    if (sym < 144)      bw.put_rev(uint32_t(0x30 + sym), 8);
    else if (sym < 256) bw.put_rev(uint32_t(0x190 + sym - 144), 9);
    else if (sym < 280) bw.put_rev(uint32_t(sym - 256), 7);
    else                bw.put_rev(uint32_t(0xC0 + sym - 280), 8);
}

inline void put_match(BitWriter& bw, int len, int dist) {
    static const int lbase[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
    static const int lext[29]  = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
    static const int dbase[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
    static const int dext[30]  = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };
    int li = 28;
    while (li > 0 && lbase[li] > len) --li;
    put_litlen(bw, 257 + li);
    if (lext[li]) bw.put(uint32_t(len - lbase[li]), lext[li]);
    int di = 29;
    while (di > 0 && dbase[di] > dist) --di;
    bw.put_rev(uint32_t(di), 5);
    if (dext[di]) bw.put(uint32_t(dist - dbase[di]), dext[di]);
}

// Raw deflate (one final fixed-Huffman block) of `in`.
inline void deflate_fixed(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
    BitWriter bw{ out };
    bw.put(1, 1);          // BFINAL
    bw.put(1, 2);          // BTYPE = 01 (fixed Huffman)
    const int n = int(in.size());
    constexpr int kWin = 32768, kHash = 1 << 15, kChain = 48, kMax = 258;
    std::vector<int> head(kHash, -1), prev(size_t(n > 0 ? n : 1), -1);
    auto hash3 = [&](int i) { return ((in[size_t(i)] << 10) ^ (in[size_t(i) + 1] << 5) ^ in[size_t(i) + 2]) & (kHash - 1); };
    auto insert = [&](int i) { if (i + 2 < n) { const int h = hash3(i); prev[size_t(i)] = head[size_t(h)]; head[size_t(h)] = i; } };
    auto best = [&](int i, int& blen, int& bdist) {
        blen = 0; bdist = 0;
        if (i + 2 >= n) return;
        int cand = head[size_t(hash3(i))];
        const int maxl = std::min(kMax, n - i);
        for (int c = 0; c < kChain && cand >= 0 && i - cand <= kWin; ++c, cand = prev[size_t(cand)]) {
            int l = 0;
            while (l < maxl && in[size_t(cand + l)] == in[size_t(i + l)]) ++l;
            if (l > blen) { blen = l; bdist = i - cand; if (l == maxl) break; }
        }
    };
    int i = 0;
    while (i < n) {
        int len = 0, dist = 0;
        best(i, len, dist);
        if (len >= 3) {
            // lazy: a longer match one byte later wins (emit this byte as a literal)
            int l2 = 0, d2 = 0;
            insert(i);
            best(i + 1, l2, d2);
            if (l2 > len + 1) { put_litlen(bw, in[size_t(i)]); ++i; continue; }
            put_match(bw, len, dist);
            for (int k = 1; k < len; ++k) insert(i + k);
            i += len;
        } else {
            put_litlen(bw, in[size_t(i)]);
            insert(i);
            ++i;
        }
    }
    put_litlen(bw, 256);   // end of block
    bw.flush();
}

inline void put_be32(std::vector<uint8_t>& o, uint32_t v) {
    o.push_back(uint8_t(v >> 24)); o.push_back(uint8_t(v >> 16)); o.push_back(uint8_t(v >> 8)); o.push_back(uint8_t(v));
}
inline void chunk(std::vector<uint8_t>& png, const char* type, const std::vector<uint8_t>& data) {
    put_be32(png, uint32_t(data.size()));
    const size_t at = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), data.begin(), data.end());
    put_be32(png, crc32(&png[at], png.size() - at) ^ 0xFFFFFFFFu);
}

inline uint8_t paeth(int a, int b, int c) {
    const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return uint8_t(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

} // namespace pngw

// Encode w*h phx RGBA pixels (R|G<<8|B<<16|A<<24) as a PNG. Pixels with alpha 0 are stored as
// transparent black (their RGB is meaningless and would only defeat palettes/compression).
inline std::vector<uint8_t> png_encode(const uint32_t* px, int w, int h) {
    using namespace pngw;
    std::vector<uint8_t> png = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    if (w <= 0 || h <= 0) return {};
    const size_t np = size_t(w) * size_t(h);
    std::vector<uint32_t> clean(px, px + np);
    for (uint32_t& c : clean) if ((c >> 24) == 0) c = 0;

    // Palette when it fits (first-appearance order, transparent first if present -> index 0).
    std::map<uint32_t, int> index;
    std::vector<uint32_t> palette;
    bool indexed = true;
    for (uint32_t c : clean) if (c == 0) { index[0] = 0; palette.push_back(0); break; }
    for (uint32_t c : clean) {
        if (index.count(c)) continue;
        if (palette.size() == 256) { indexed = false; break; }
        index[c] = int(palette.size());
        palette.push_back(c);
    }

    std::vector<uint8_t> ihdr;
    put_be32(ihdr, uint32_t(w)); put_be32(ihdr, uint32_t(h));
    ihdr.push_back(8);                    // bit depth
    ihdr.push_back(indexed ? 3 : 6);      // colour type
    ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    chunk(png, "IHDR", ihdr);

    std::vector<uint8_t> raw;
    if (indexed) {
        std::vector<uint8_t> plte, trns;
        bool any_alpha = false;
        for (uint32_t c : palette) {
            plte.push_back(uint8_t(c)); plte.push_back(uint8_t(c >> 8)); plte.push_back(uint8_t(c >> 16));
            trns.push_back(uint8_t(c >> 24));
            if ((c >> 24) != 255) any_alpha = true;
        }
        chunk(png, "PLTE", plte);
        if (any_alpha) {
            while (!trns.empty() && trns.back() == 255) trns.pop_back();   // trailing opaque entries implied
            chunk(png, "tRNS", trns);
        }
        raw.reserve(np + size_t(h));
        for (int y = 0; y < h; ++y) {
            raw.push_back(0);                                              // filter: None
            for (int x = 0; x < w; ++x) raw.push_back(uint8_t(index[clean[size_t(y) * size_t(w) + size_t(x)]]));
        }
    } else {
        const size_t stride = size_t(w) * 4;
        std::vector<uint8_t> cur(stride), prev(stride, 0), best(stride), tmp(stride);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const uint32_t c = clean[size_t(y) * size_t(w) + size_t(x)];
                cur[size_t(x) * 4 + 0] = uint8_t(c); cur[size_t(x) * 4 + 1] = uint8_t(c >> 8);
                cur[size_t(x) * 4 + 2] = uint8_t(c >> 16); cur[size_t(x) * 4 + 3] = uint8_t(c >> 24);
            }
            long best_sum = -1;
            uint8_t best_f = 0;
            for (uint8_t f = 0; f < 5; ++f) {
                long sum = 0;
                for (size_t i = 0; i < stride; ++i) {
                    const int a = i >= 4 ? cur[i - 4] : 0, b = prev[i], c = i >= 4 ? prev[i - 4] : 0;
                    int pred = 0;
                    if (f == 1) pred = a; else if (f == 2) pred = b; else if (f == 3) pred = (a + b) / 2; else if (f == 4) pred = paeth(a, b, c);
                    tmp[i] = uint8_t(cur[i] - pred);
                    sum += tmp[i] < 128 ? tmp[i] : 256 - tmp[i];
                }
                if (best_sum < 0 || sum < best_sum) { best_sum = sum; best_f = f; best = tmp; }
            }
            raw.push_back(best_f);
            raw.insert(raw.end(), best.begin(), best.end());
            prev = cur;
        }
    }

    std::vector<uint8_t> z = { 0x78, 0x01 };             // zlib header (32K window)
    deflate_fixed(raw, z);
    put_be32(z, adler32(raw.data(), raw.size()));
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    return png;
}

inline bool png_write_file(const std::string& path, const uint32_t* px, int w, int h, std::string* err = nullptr) {
    const std::vector<uint8_t> bytes = png_encode(px, w, h);
    if (bytes.empty()) { if (err) *err = "empty image"; return false; }
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { if (err) *err = "cannot write " + path; return false; }
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    std::fclose(f);
    if (!ok && err) *err = "short write to " + path;
    return ok;
}

} // namespace phxtool
#endif // PHX_TOOLS_PNG_WRITE_H
