// tools/phxpack/lz_encode.h — host-side encoder for the bundle's LZSS codec (decoder lives
// in phx/resource/lz.h and is the only half that ships to the target). STL is fine here:
// tools never run on a console. Greedy longest-match over a 4 KB window; assets are small
// (KB), so the O(n * window) search is more than fast enough and keeps the code obvious.
#ifndef PHX_TOOLS_LZ_ENCODE_H
#define PHX_TOOLS_LZ_ENCODE_H

#include "phx/resource/lz.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace phxtool {

// Append the LZSS encoding of src[0..n) to `out`. The result decodes back to exactly the
// input via phx::lz_decode (verified by the unit tests). Bit/field layout matches lz.h.
inline void lz_encode(const uint8_t* src, uint32_t n, std::vector<uint8_t>& out) {
    uint32_t i = 0;
    while (i < n) {
        const size_t ctrl_pos = out.size();
        out.push_back(0);                                   // control byte placeholder
        uint8_t control = 0;
        for (int b = 0; b < 8 && i < n; ++b) {
            uint32_t best_len = 0, best_disp = 0;
            const uint32_t win_start = (i > phx::kLzMaxDisp) ? (i - phx::kLzMaxDisp) : 0;
            const uint32_t max_len   = std::min<uint32_t>(phx::kLzMaxMatch, n - i);
            for (uint32_t j = win_start; j < i; ++j) {
                uint32_t l = 0;
                while (l < max_len && src[j + l] == src[i + l]) ++l;  // overlap-safe: src is truth
                if (l > best_len) { best_len = l; best_disp = i - j; }
            }
            if (best_len >= phx::kLzMinMatch) {
                control |= (0x80u >> b);
                const uint32_t lenc = best_len - phx::kLzMinMatch;    // 0..15
                const uint32_t dc   = best_disp - 1;                  // 0..4095
                out.push_back(uint8_t((lenc << 4) | ((dc >> 8) & 0x0Fu)));
                out.push_back(uint8_t(dc & 0xFFu));
                i += best_len;
            } else {
                out.push_back(src[i++]);                              // literal
            }
        }
        out[ctrl_pos] = control;
    }
}

} // namespace phxtool
#endif // PHX_TOOLS_LZ_ENCODE_H
