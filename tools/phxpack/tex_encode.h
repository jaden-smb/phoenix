// tools/phxpack/tex_encode.h — per-target texture encoders (host-only, STL allowed).
// The bake-time half of the per-target texture story (docs/06 §4, docs/08 §2):
//
//   pal4_encode — tier 0 (GBA): RGBA8 -> PAL4_TILES, the exact offline mirror of the PPU
//     backend's upload quantizer (engine/render/src/gba/gba_ppu.cpp): unique opaque colours
//     gathered in scanline order, one 16-colour palette when the whole texture fits 15
//     colours (OBJ-safe), otherwise greedy-first-fit deduplicated per-tile palettes
//     (BG-only). The upload then only has to claim palette banks and remap nibbles — the
//     composed frame is pixel-identical to quantizing the RGBA8 at upload, but the bundle
//     carries ~8× less texel data and the 16 MHz CPU never scans texels for unique colours.
//
//   swz_encode — tier 1 (PSP): RGBA8 -> RGBA8_SWZ, the GU's swizzled block order. A pure
//     texel reorder (values untouched), so the GU backend stays bit-identical to the soft
//     golden reference while sampling the blob zero-copy with the swizzle bit set.
//
// Both return false when the source can't be expressed (caller keeps RGBA8 — the honest
// fallback: the runtime treats such textures exactly as it treats a v1 bundle's).
#ifndef PHX_TOOLS_TEX_ENCODE_H
#define PHX_TOOLS_TEX_ENCODE_H

#include "phx/core/pixel.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace phxtool {

namespace detail {
// One 16-colour palette being built: slot 0 = transparent, colours at 1..used.
struct Pal4 {
    uint16_t colors[16] = {};
    int      used = 0;

    int find(uint16_t c) const {
        for (int s = 1; s <= used; ++s) if (colors[s] == c) return s;
        return 0;
    }
    // Mirror of the PPU's claim_bank absorption: can this palette hold all `n` colours?
    bool can_absorb(const uint16_t* cols, int n) const {
        int missing = 0;
        for (int k = 0; k < n; ++k) if (!find(cols[k])) ++missing;
        return used + missing <= 15;
    }
    void absorb(const uint16_t* cols, int n) {
        for (int k = 0; k < n; ++k) if (!find(cols[k])) colors[++used] = cols[k];
    }
};

// Gather the unique opaque BGR555 colours of a rect, in scanline order (the same order the
// PPU upload quantizer interns them). Returns false if more than 15 are needed.
inline bool gather_colors(const uint32_t* px, uint32_t stride, uint32_t x0, uint32_t y0,
                          uint32_t w, uint32_t h, uint16_t* out, int& n) {
    n = 0;
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t texel = px[(y0 + y) * stride + (x0 + x)];
            if ((texel >> 24) == 0) continue;                    // alpha 0 -> transparent
            const uint16_t c = phx::rgba8_to_bgr555(texel);
            bool found = false;
            for (int k = 0; k < n; ++k) if (out[k] == c) { found = true; break; }
            if (found) continue;
            if (n >= 15) return false;
            out[n++] = c;
        }
    return true;
}
} // namespace detail

// RGBA8 -> PAL4_TILES payload (the bytes that follow TextureBlobHeader). False when the
// texture is not 8px-aligned, one 8×8 tile needs >15 opaque colours, or the tile palettes
// won't deduplicate into 255 (the tile_pal byte's range) — fall back to RGBA8 then.
inline bool pal4_encode(const uint32_t* px, uint16_t w, uint16_t h, std::vector<uint8_t>& out) {
    using namespace detail;
    if (w == 0 || h == 0 || (w % 8) || (h % 8)) return false;
    const uint32_t cols_t = w / 8u, rows_t = h / 8u;
    const uint32_t n_tiles = cols_t * rows_t;

    std::vector<Pal4>   pals;
    std::vector<uint8_t> tile_pal(n_tiles, 0);

    // Whole-texture palette first (required for OBJ use), per-tile spill otherwise —
    // the same decision, in the same colour order, as the PPU upload quantizer.
    uint16_t uniq[15]; int uniq_n = 0;
    if (gather_colors(px, w, 0, 0, w, h, uniq, uniq_n)) {
        Pal4 p; p.absorb(uniq, uniq_n);
        pals.push_back(p);
    } else {
        for (uint32_t t = 0; t < n_tiles; ++t) {
            const uint32_t tx = (t % cols_t) * 8u, ty = (t / cols_t) * 8u;
            uint16_t tc[15]; int tn = 0;
            if (!gather_colors(px, w, tx, ty, 8, 8, tc, tn)) return false;  // >15 in ONE tile
            int pi = -1;
            for (size_t p = 0; p < pals.size(); ++p)
                if (pals[p].can_absorb(tc, tn)) { pi = int(p); break; }     // greedy first fit
            if (pi < 0) {
                if (pals.size() >= 255) return false;                       // tile_pal is a byte
                pals.emplace_back();
                pi = int(pals.size()) - 1;
            }
            pals[size_t(pi)].absorb(tc, tn);
            tile_pal[t] = uint8_t(pi);
        }
    }

    const uint16_t pal_count = uint16_t(pals.size());
    out.assign(phx::pal4_payload_size(pal_count, n_tiles), 0);

    phx::TexturePal4Header ph{};
    ph.pal_count = pal_count;
    std::memcpy(out.data(), &ph, sizeof(ph));
    uint16_t* pal_out  = reinterpret_cast<uint16_t*>(out.data() + phx::pal4_palettes_off());
    uint8_t*  used_out = out.data() + phx::pal4_pal_used_off(pal_count);
    for (uint16_t p = 0; p < pal_count; ++p) {
        for (int s = 0; s < 16; ++s) pal_out[p * 16 + s] = pals[p].colors[s];   // slot 0 stays 0
        used_out[p] = uint8_t(pals[p].used);
    }
    std::memcpy(out.data() + phx::pal4_tile_pal_off(pal_count), tile_pal.data(), n_tiles);

    uint8_t* tiles = out.data() + phx::pal4_tile_data_off(pal_count, n_tiles);
    for (uint32_t t = 0; t < n_tiles; ++t) {
        const uint32_t tx = (t % cols_t) * 8u, ty = (t / cols_t) * 8u;
        const Pal4& p = pals[tile_pal[t]];
        for (uint32_t y = 0; y < 8; ++y)
            for (uint32_t x = 0; x < 8; ++x) {
                const uint32_t texel = px[(ty + y) * w + (tx + x)];
                if ((texel >> 24) == 0) continue;                // nibble stays 0 (transparent)
                const int slot = p.find(phx::rgba8_to_bgr555(texel));
                tiles[t * phx::kPal4TileBytes + y * 4u + (x >> 1)] |=
                    uint8_t((x & 1u) ? (slot << 4) : slot);
            }
    }
    return true;
}

// RGBA8 -> RGBA8_SWZ payload: the same w*h texels in the GU's swizzled block order.
// False when the size doesn't block-align (width % 4 || height % 8) — keep RGBA8 then.
inline bool swz_encode(const uint32_t* px, uint16_t w, uint16_t h, std::vector<uint8_t>& out) {
    if (!phx::swz_size_ok(w, h)) return false;
    out.resize(size_t(w) * h * 4u);
    uint32_t* dst = reinterpret_cast<uint32_t*>(out.data());
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
            dst[phx::swz_texel_index(x, y, w)] = px[y * w + x];
    return true;
}

} // namespace phxtool
#endif // PHX_TOOLS_TEX_ENCODE_H
