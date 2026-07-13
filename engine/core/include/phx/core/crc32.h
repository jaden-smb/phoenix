// phx/core/crc32.h — CRC32 (the IEEE 802.3 / zlib polynomial), used to guard `.phxp` bundle
// integrity (docs/06 §1: BundleHeader.blob_crc32). The 256-entry table is computed at COMPILE
// time (constexpr) so there is no runtime table-build cost and no heap — just a 1 KB rodata
// table, shared (inline) across every translation unit that includes this header. Table-driven
// lookup keeps the bake-time host pass over MB-sized blobs fast and the on-console mount-time
// verification cheap enough to run unconditionally.
#ifndef PHX_CORE_CRC32_H
#define PHX_CORE_CRC32_H

#include <cstddef>
#include <cstdint>

namespace phx {
namespace detail {

struct Crc32Table { uint32_t v[256]; };

constexpr Crc32Table make_crc32_table() {
    Crc32Table t{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        t.v[i] = c;
    }
    return t;
}

} // namespace detail

inline constexpr detail::Crc32Table kCrc32Table = detail::make_crc32_table();

// Streaming-capable: pass the previous return value as `seed` to continue a CRC across
// non-contiguous chunks. Call with the default seed for a fresh checksum.
constexpr uint32_t crc32(const void* data, size_t n, uint32_t seed = 0xFFFFFFFFu) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t c = seed;
    for (size_t i = 0; i < n; ++i)
        c = kCrc32Table.v[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c;
}

// Finalize a streamed (or one-shot) CRC — XOR out, matching the standard CRC-32 definition.
constexpr uint32_t crc32_finish(uint32_t c) { return c ^ 0xFFFFFFFFu; }

// One-shot convenience: crc32_of(data, n) == crc32_finish(crc32(data, n)).
constexpr uint32_t crc32_of(const void* data, size_t n) { return crc32_finish(crc32(data, n)); }

} // namespace phx
#endif // PHX_CORE_CRC32_H
