// tools/phxpack/png.h — a minimal PNG decoder for the asset pipeline (host-only). Decodes the
// common 8-bit-per-channel, non-interlaced PNG color types — grayscale (0), truecolor RGB (2),
// palette (3, with optional tRNS), grayscale+alpha (4), and truecolor RGBA (6) — into a flat
// RGBA8 buffer in phx::Rgba byte order (R | G<<8 | B<<16 | A<<24), matching the PPM loader and
// what BundleWriter::add_texture expects. Uses inflate.h for the zlib'd IDAT. Returns false on
// anything it does not support (interlaced, 16-bit, bad CRC-free structure) so the bake fails
// loudly rather than producing garbage. Real authoring tools target these formats; exotic
// variants can be added behind the same entry point.
#ifndef PHX_TOOLS_PNG_H
#define PHX_TOOLS_PNG_H

#include "inflate.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace phxtool {

namespace detail {

inline uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
inline uint8_t paeth(int a, int b, int c) {
    int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return uint8_t(a);
    return pb <= pc ? uint8_t(b) : uint8_t(c);
}

} // namespace detail

// Decode `data[0..n)`; on success fills `rgba` (w*h entries) and the dimensions, returns true.
inline bool png_decode(const uint8_t* data, size_t n,
                       std::vector<uint32_t>& rgba, uint16_t& width, uint16_t& height) {
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    if (n < 8) return false;
    for (int i = 0; i < 8; ++i) if (data[i] != sig[i]) return false;

    uint32_t w = 0, h = 0; uint8_t depth = 0, color = 0, interlace = 0;
    std::vector<uint8_t> idat;
    std::vector<uint8_t> plte;                  // RGB triples
    std::vector<uint8_t> trns;                  // palette alpha
    bool have_ihdr = false;

    size_t off = 8;
    for (;;) {
        if (off + 8 > n) return false;
        const uint32_t len = detail::be32(data + off);
        const uint8_t* type = data + off + 4;
        const uint8_t* body = data + off + 8;
        if (off + 12 + size_t(len) > n) return false;   // length + type + data + crc
        if (type[0]=='I'&&type[1]=='H'&&type[2]=='D'&&type[3]=='R') {
            if (len < 13) return false;
            w = detail::be32(body); h = detail::be32(body + 4);
            depth = body[8]; color = body[9]; interlace = body[12];
            have_ihdr = true;
        } else if (type[0]=='P'&&type[1]=='L'&&type[2]=='T'&&type[3]=='E') {
            plte.assign(body, body + len);
        } else if (type[0]=='t'&&type[1]=='R'&&type[2]=='N'&&type[3]=='S') {
            trns.assign(body, body + len);
        } else if (type[0]=='I'&&type[1]=='D'&&type[2]=='A'&&type[3]=='T') {
            idat.insert(idat.end(), body, body + len);
        } else if (type[0]=='I'&&type[1]=='E'&&type[2]=='N'&&type[3]=='D') {
            break;
        }
        off += 12 + size_t(len);
    }
    if (!have_ihdr || w == 0 || h == 0 || w > 0xFFFF || h > 0xFFFF) return false;
    if (depth != 8 || interlace != 0) return false;     // only 8-bit, non-interlaced supported

    int channels;
    switch (color) {
        case 0: channels = 1; break;                    // grayscale
        case 2: channels = 3; break;                    // RGB
        case 3: channels = 1; break;                    // palette index
        case 4: channels = 2; break;                    // gray + alpha
        case 6: channels = 4; break;                    // RGBA
        default: return false;
    }
    if (color == 3 && plte.empty()) return false;

    std::vector<uint8_t> raw;
    if (!inflate_zlib(idat.data(), idat.size(), raw)) return false;

    const size_t bpp    = size_t(channels);             // bytes per pixel (8-bit)
    const size_t stride = size_t(w) * bpp;
    if (raw.size() < (stride + 1) * size_t(h)) return false;

    // Unfilter in place into a tight w*h*bpp buffer.
    std::vector<uint8_t> img(stride * size_t(h));
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t ft = raw[y * (stride + 1)];
        const uint8_t* in = &raw[y * (stride + 1) + 1];
        uint8_t* cur = &img[y * stride];
        const uint8_t* prev = y ? &img[(y - 1) * stride] : nullptr;
        for (size_t i = 0; i < stride; ++i) {
            const int a = i >= bpp ? cur[i - bpp] : 0;          // left
            const int b = prev ? prev[i] : 0;                   // up
            const int c = (prev && i >= bpp) ? prev[i - bpp] : 0;// up-left
            int v = in[i];
            switch (ft) {
                case 0: break;
                case 1: v += a; break;
                case 2: v += b; break;
                case 3: v += (a + b) / 2; break;
                case 4: v += detail::paeth(a, b, c); break;
                default: return false;
            }
            cur[i] = uint8_t(v);
        }
    }

    // Convert to RGBA8 (R|G<<8|B<<16|A<<24).
    rgba.assign(size_t(w) * h, 0);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t* p = &img[y * stride + size_t(x) * bpp];
            uint8_t r, g, bl, al = 255;
            switch (color) {
                case 0: r = g = bl = p[0]; break;
                case 2: r = p[0]; g = p[1]; bl = p[2]; break;
                case 3: {
                    const size_t idx = p[0];
                    if (idx * 3 + 2 >= plte.size()) return false;
                    r = plte[idx*3]; g = plte[idx*3+1]; bl = plte[idx*3+2];
                    al = idx < trns.size() ? trns[idx] : 255;
                    break;
                }
                case 4: r = g = bl = p[0]; al = p[1]; break;
                case 6: r = p[0]; g = p[1]; bl = p[2]; al = p[3]; break;
                default: return false;
            }
            rgba[y * w + x] = uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(bl) << 16) | (uint32_t(al) << 24);
        }
    }
    width  = uint16_t(w);
    height = uint16_t(h);
    return true;
}

} // namespace phxtool
#endif // PHX_TOOLS_PNG_H
