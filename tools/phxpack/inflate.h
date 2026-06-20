// tools/phxpack/inflate.h — a complete, dependency-free DEFLATE (RFC 1951) decompressor with
// a zlib (RFC 1950) wrapper, for decoding PNG IDAT streams in the asset pipeline. HOST-ONLY
// (STL allowed; tools never ship to a console). Decode logic follows the canonical "puff"
// approach: a LSB-first bit reader + canonical-Huffman tables built from code lengths.
//
// Handles all three block types — stored (00), fixed-Huffman (01), dynamic-Huffman (10) — and
// validates structure defensively (a malformed stream returns false, never reads out of bounds).
#ifndef PHX_TOOLS_INFLATE_H
#define PHX_TOOLS_INFLATE_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace phxtool {

namespace detail {

struct BitReader {
    const uint8_t* p; size_t n; size_t byte = 0; int bit = 0;
    // LSB-first read of `bits` bits (DEFLATE bit order). Returns false on underrun.
    bool get(uint32_t bits, uint32_t& out) {
        out = 0;
        for (uint32_t i = 0; i < bits; ++i) {
            if (byte >= n) return false;
            out |= uint32_t((p[byte] >> bit) & 1u) << i;
            if (++bit == 8) { bit = 0; ++byte; }
        }
        return true;
    }
    void align() { if (bit) { bit = 0; ++byte; } }
};

// Canonical Huffman table: counts of codes per length + symbols sorted by (length, symbol).
struct Huff {
    std::vector<int> count;     // [0..maxbits]
    std::vector<int> symbol;
    int maxbits = 0;
    bool build(const std::vector<int>& lengths) {
        maxbits = 0;
        for (int l : lengths) if (l > maxbits) maxbits = l;
        if (maxbits == 0) return true;                 // empty table (valid: e.g. no distances)
        count.assign(maxbits + 1, 0);
        for (int l : lengths) count[l]++;
        count[0] = 0;
        std::vector<int> offs(maxbits + 2, 0);
        for (int i = 1; i <= maxbits; ++i) offs[i + 1] = offs[i] + count[i];
        symbol.assign(lengths.size(), 0);
        for (size_t s = 0; s < lengths.size(); ++s)
            if (lengths[s]) symbol[offs[lengths[s]]++] = int(s);
        return true;
    }
};

inline bool decode_sym(BitReader& br, const Huff& h, int& sym) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= h.maxbits; ++len) {
        uint32_t b;
        if (!br.get(1, b)) return false;
        code |= int(b);
        const int cnt = h.count[len];
        if (code - first < cnt) { sym = h.symbol[index + (code - first)]; return true; }
        index += cnt; first += cnt; first <<= 1; code <<= 1;
    }
    return false;
}

inline bool inflate_raw(const uint8_t* src, size_t n, std::vector<uint8_t>& out) {
    static const int lbase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const int lext[29]  = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const int dbase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const int dext[30]  = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    static const int order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};

    BitReader br{src, n};
    for (;;) {
        uint32_t bfinal, btype;
        if (!br.get(1, bfinal)) return false;
        if (!br.get(2, btype))  return false;

        if (btype == 0) {                               // stored
            br.align();
            if (br.byte + 4 > n) return false;
            uint32_t len = uint32_t(src[br.byte]) | (uint32_t(src[br.byte + 1]) << 8);
            br.byte += 4;                               // skip LEN(2) + NLEN(2)
            if (br.byte + len > n) return false;
            out.insert(out.end(), src + br.byte, src + br.byte + len);
            br.byte += len;
        } else if (btype == 1 || btype == 2) {
            Huff lit, dist;
            if (btype == 1) {                           // fixed Huffman
                std::vector<int> ll(288), dl(30, 5);
                for (int i = 0;   i < 144; i++) ll[i] = 8;
                for (int i = 144; i < 256; i++) ll[i] = 9;
                for (int i = 256; i < 280; i++) ll[i] = 7;
                for (int i = 280; i < 288; i++) ll[i] = 8;
                lit.build(ll); dist.build(dl);
            } else {                                    // dynamic Huffman
                uint32_t hlit, hdist, hclen;
                if (!br.get(5, hlit) || !br.get(5, hdist) || !br.get(4, hclen)) return false;
                hlit += 257; hdist += 1; hclen += 4;
                std::vector<int> cl(19, 0);
                for (uint32_t i = 0; i < hclen; i++) { uint32_t v; if (!br.get(3, v)) return false; cl[order[i]] = int(v); }
                Huff clh; clh.build(cl);
                std::vector<int> lengths; lengths.reserve(hlit + hdist);
                while (lengths.size() < size_t(hlit + hdist)) {
                    int sym; if (!decode_sym(br, clh, sym)) return false;
                    if (sym < 16) { lengths.push_back(sym); }
                    else if (sym == 16) { uint32_t r; if (!br.get(2, r) || lengths.empty()) return false;
                        int prev = lengths.back(); for (uint32_t k = 0; k < r + 3; k++) lengths.push_back(prev); }
                    else if (sym == 17) { uint32_t r; if (!br.get(3, r)) return false;
                        for (uint32_t k = 0; k < r + 3;  k++) lengths.push_back(0); }
                    else if (sym == 18) { uint32_t r; if (!br.get(7, r)) return false;
                        for (uint32_t k = 0; k < r + 11; k++) lengths.push_back(0); }
                    else return false;
                }
                if (lengths.size() != size_t(hlit + hdist)) return false;
                std::vector<int> ll(lengths.begin(), lengths.begin() + hlit);
                std::vector<int> dl(lengths.begin() + hlit, lengths.end());
                lit.build(ll); dist.build(dl);
            }
            for (;;) {                                  // decode this block's symbols
                int sym; if (!decode_sym(br, lit, sym)) return false;
                if (sym == 256) break;                  // end of block
                if (sym < 256) { out.push_back(uint8_t(sym)); continue; }
                sym -= 257; if (sym >= 29) return false;
                uint32_t extra; if (!br.get(uint32_t(lext[sym]), extra)) return false;
                const int length = lbase[sym] + int(extra);
                int dsym; if (!decode_sym(br, dist, dsym)) return false;
                if (dsym >= 30) return false;
                uint32_t dextra; if (!br.get(uint32_t(dext[dsym]), dextra)) return false;
                const size_t distance = size_t(dbase[dsym] + int(dextra));
                if (distance > out.size()) return false;
                const size_t from = out.size() - distance;
                for (int k = 0; k < length; k++) out.push_back(out[from + size_t(k)]); // overlap ok
            }
        } else {
            return false;                               // btype 3 reserved
        }
        if (bfinal) break;
    }
    return true;
}

} // namespace detail

// Inflate a zlib-wrapped DEFLATE stream (PNG IDAT). The trailing adler32 is ignored.
inline bool inflate_zlib(const uint8_t* src, size_t n, std::vector<uint8_t>& out) {
    if (n < 2) return false;
    const uint8_t cmf = src[0], flg = src[1];
    if ((cmf & 0x0f) != 8) return false;                // CM != deflate
    if (((uint32_t(cmf) << 8) | flg) % 31 != 0) return false;   // header check bits
    size_t off = 2;
    if (flg & 0x20) off += 4;                           // FDICT -> skip preset dictionary id
    if (off > n) return false;
    return detail::inflate_raw(src + off, n - off, out);
}

} // namespace phxtool
#endif // PHX_TOOLS_INFLATE_H
