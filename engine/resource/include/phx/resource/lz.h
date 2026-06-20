// phx/resource/lz.h — the bundle's compression codec. A tiny LZSS (LZ77 + control bits),
// chosen for the same reasons the GBA BIOS ships an LZ77 decompressor: the *decoder* is
// tiny, allocation-free, single-pass, and decodes straight into a caller-provided buffer —
// exactly what a console runtime needs. The *encoder* lives host-side (tools/phxpack);
// only the decoder below ships to the target, so this header stays STL-free.
//
// Stream format (byte-aligned, deterministic — identical bytes on every target):
//   The stream is a sequence of groups. Each group is one control byte followed by up to
//   8 items, one per control bit read MSB->LSB:
//     bit == 0 -> literal: copy 1 byte verbatim.
//     bit == 1 -> match:   read 2 bytes b0,b1.
//                          length      = (b0 >> 4) + kLzMinMatch     (3..18)
//                          displacement= (((b0 & 0xF) << 8) | b1) + 1 (1..4096)
//                          copy `length` bytes from `out - displacement`, byte by byte
//                          (overlapping copies are allowed -> gives run-length for free).
// The decoder knows the exact decompressed size up front (TocEntry.usize), so it simply
// produces bytes until the output buffer is full; no end marker is needed.
#ifndef PHX_RESOURCE_LZ_H
#define PHX_RESOURCE_LZ_H

#include "phx/core/types.h"

namespace phx {

constexpr uint32_t kLzMinMatch = 3;     // matches shorter than this are cheaper as literals
constexpr uint32_t kLzMaxMatch = 18;    // kLzMinMatch + 15 (4-bit length field)
constexpr uint32_t kLzMaxDisp  = 4096;  // 12-bit displacement window

// Decompress `src[0..src_size)` into `dst[0..dst_cap)`. Returns the number of bytes written
// (== dst_cap on success), or 0 on malformed input / insufficient capacity. Pure: touches
// only the two buffers, allocates nothing, never reads out of bounds. Safe on hostile data.
inline uint32_t lz_decode(const uint8_t* src, uint32_t src_size,
                          uint8_t* dst, uint32_t dst_cap) {
    uint32_t si = 0, di = 0;
    while (di < dst_cap) {
        if (si >= src_size) return 0;                       // input underrun
        const uint8_t control = src[si++];
        for (int b = 0; b < 8 && di < dst_cap; ++b) {
            if ((control & (0x80u >> b)) == 0) {            // literal
                if (si >= src_size) return 0;
                dst[di++] = src[si++];
            } else {                                        // match
                if (si + 1 >= src_size) return 0;
                const uint8_t b0 = src[si++], b1 = src[si++];
                const uint32_t len  = (uint32_t(b0 >> 4)) + kLzMinMatch;
                const uint32_t disp = ((uint32_t(b0 & 0x0Fu) << 8) | b1) + 1u;
                if (disp > di) return 0;                    // reference before output start
                if (di + len > dst_cap) return 0;           // would overflow output -> corrupt
                for (uint32_t k = 0; k < len; ++k, ++di)
                    dst[di] = dst[di - disp];               // forward copy supports overlap
            }
        }
    }
    return di;
}

} // namespace phx
#endif // PHX_RESOURCE_LZ_H
