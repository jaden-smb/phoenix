// tools/phxviz/analyze.h — HOST-ONLY offline analysis for the miracle-player visualization
// track (docs/08 "bake offline, read in place"). Given mono PCM, it emits the .phxviz blob
// (a VizHeader + one VizFrame per video frame) that the ROM reads zero-copy. STL + float are
// fine here (host tool); nothing in this file ships to a console.
//
// Pipeline per video frame: Hann-windowed real FFT -> log-spaced band magnitudes -> RMS
// loudness -> low/mid/high energies -> spectral-flux onset + peak-picked beat flag. Everything
// is quantised to uint8 with a two-pass global normalisation so the bars use the full range.
#ifndef PHXVIZ_ANALYZE_H
#define PHXVIZ_ANALYZE_H

#include "viz.h"          // examples/miracle-player/src — the shared POD format

#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <array>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace phxviz {

using miracle::VizFrame;
using miracle::VizHeader;
using miracle::kVizBands;

constexpr uint32_t kFftSize  = 1024;   // ~56 ms window at 18157 Hz; plenty for music bars
constexpr uint32_t kFrameHz  = 60;     // one record per video frame

// ---- linear resampler: int16 @ in_rate -> float [-1,1] @ out_rate --------------------------
inline std::vector<float> resample_mono(const int16_t* in, uint32_t n, uint32_t in_rate,
                                        uint32_t out_rate) {
    std::vector<float> out;
    if (!n || !in_rate || !out_rate) return out;
    if (in_rate == out_rate) {
        out.resize(n);
        for (uint32_t i = 0; i < n; ++i) out[i] = in[i] * (1.0f / 32768.0f);
        return out;
    }
    const uint64_t out_n = (uint64_t(n) * out_rate) / in_rate;
    out.resize(size_t(out_n));
    for (uint64_t i = 0; i < out_n; ++i) {
        const double pos = double(i) * in_rate / out_rate;
        const uint32_t i0 = uint32_t(pos);
        const uint32_t i1 = i0 + 1 < n ? i0 + 1 : i0;
        const float    fr = float(pos - i0);
        const float    s  = in[i0] * (1.0f - fr) + in[i1] * fr;
        out[size_t(i)] = s * (1.0f / 32768.0f);
    }
    return out;
}

// ---- iterative radix-2 Cooley-Tukey FFT (in place) -----------------------------------------
inline void fft(std::vector<float>& re, std::vector<float>& im) {
    const uint32_t N = uint32_t(re.size());
    for (uint32_t i = 1, j = 0; i < N; ++i) {          // bit-reversal permutation
        uint32_t bit = N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (uint32_t len = 2; len <= N; len <<= 1) {
        const double ang = -2.0 * M_PI / len;
        const float wr = float(std::cos(ang)), wi = float(std::sin(ang));
        for (uint32_t i = 0; i < N; i += len) {
            float cr = 1.0f, ci = 0.0f;
            for (uint32_t k = 0; k < len / 2; ++k) {
                const float ur = re[i + k],            ui = im[i + k];
                const float vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
                const float vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
                re[i + k] = ur + vr;           im[i + k] = ui + vi;
                re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
                const float ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;        cr = ncr;
            }
        }
    }
}

// Log-spaced FFT-bin edge for band b in [0, kVizBands]: from ~40 Hz to Nyquist.
inline uint32_t band_edge(uint32_t b, uint32_t rate) {
    const double f_min = 40.0, f_max = rate * 0.5;
    const double f = f_min * std::pow(f_max / f_min, double(b) / kVizBands);
    uint32_t bin = uint32_t(f * kFftSize / rate + 0.5);
    if (bin < 1) bin = 1;
    if (bin > kFftSize / 2) bin = kFftSize / 2;
    return bin;
}

// Analyse mono float samples at `rate` -> the .phxviz blob bytes. hop_samples = rate/frame_hz.
inline std::vector<uint8_t> analyze(const std::vector<float>& mono, uint32_t rate) {
    const uint32_t hop = (rate + kFrameHz / 2) / kFrameHz;
    const uint32_t frames = mono.empty() ? 0u
                          : uint32_t((mono.size() + hop - 1) / hop);

    // Pass 1: raw float features per frame.
    std::vector<std::array<float, kVizBands>> raw(frames);
    std::vector<float> rms(frames, 0.0f), flux(frames, 0.0f);
    std::vector<float> low(frames, 0.0f), mid(frames, 0.0f), high(frames, 0.0f);

    // Precompute the Hann window.
    std::vector<float> win(kFftSize);
    for (uint32_t i = 0; i < kFftSize; ++i)
        win[i] = 0.5f - 0.5f * float(std::cos(2.0 * M_PI * i / (kFftSize - 1)));

    std::array<float, kVizBands> prev{}; prev.fill(0.0f);
    std::vector<float> re(kFftSize), im(kFftSize);

    for (uint32_t f = 0; f < frames; ++f) {
        const uint32_t start = f * hop;
        // RMS over the hop window (loudness envelope).
        double acc = 0.0; uint32_t cnt = 0;
        for (uint32_t i = 0; i < hop && start + i < mono.size(); ++i) {
            const float s = mono[start + i]; acc += double(s) * s; ++cnt;
        }
        rms[f] = cnt ? float(std::sqrt(acc / cnt)) : 0.0f;

        // FFT window (zero-padded past end of signal).
        for (uint32_t i = 0; i < kFftSize; ++i) {
            const uint32_t idx = start + i;
            re[i] = idx < mono.size() ? mono[idx] * win[i] : 0.0f;
            im[i] = 0.0f;
        }
        fft(re, im);

        std::array<float, kVizBands> band{}; band.fill(0.0f);
        for (uint32_t b = 0; b < kVizBands; ++b) {
            const uint32_t lo = band_edge(b, rate), hi = band_edge(b + 1, rate);
            double sum = 0.0; uint32_t nb = 0;
            for (uint32_t k = lo; k < hi && k <= kFftSize / 2; ++k) {
                sum += std::sqrt(double(re[k]) * re[k] + double(im[k]) * im[k]); ++nb;
            }
            band[b] = nb ? float(sum / nb) : 0.0f;
        }
        raw[f] = band;

        // Spectral flux (sum of positive band deltas) -> onset strength.
        float fl = 0.0f;
        for (uint32_t b = 0; b < kVizBands; ++b) { const float d = band[b] - prev[b]; if (d > 0) fl += d; }
        flux[f] = fl; prev = band;

        // low / mid / high groupings.
        for (uint32_t b = 0; b < 3;        ++b) low[f]  += band[b];
        for (uint32_t b = 3; b < 10;       ++b) mid[f]  += band[b];
        for (uint32_t b = 10; b < kVizBands; ++b) high[f] += band[b];
    }

    // Pass 2: global normalisation maxima (guard against divide-by-zero).
    auto vmax = [](const std::vector<float>& v) {
        float m = 0.0f; for (float x : v) m = std::max(m, x); return m > 0 ? m : 1.0f; };
    float bmax = 1e-9f;
    for (auto& fb : raw) for (float x : fb) bmax = std::max(bmax, x);
    const float lmax = vmax(low), mmax = vmax(mid), hmax = vmax(high);
    const float rmax = vmax(rms), fmax = vmax(flux);

    auto q = [](float v, float m) -> uint8_t {           // sqrt-compressed 0..255
        float n = m > 0 ? v / m : 0.0f; if (n < 0) n = 0; if (n > 1) n = 1;
        int o = int(std::sqrt(n) * 255.0f + 0.5f); return uint8_t(o < 0 ? 0 : o > 255 ? 255 : o);
    };

    // Peak-pick beats on the normalised flux: a local maximum that clears a moving average.
    std::vector<uint8_t> beat(frames, 0);
    const int W = 20;                                   // ~1/3 s moving-average half-window
    for (uint32_t f = 0; f < frames; ++f) {
        double sum = 0.0; int n = 0;
        for (int j = -W; j <= W; ++j) { int k = int(f) + j; if (k >= 0 && k < int(frames)) { sum += flux[k]; ++n; } }
        const float avg = n ? float(sum / n) : 0.0f;
        const bool localmax = (f == 0 || flux[f] >= flux[f - 1]) &&
                              (f + 1 >= frames || flux[f] > flux[f + 1]);
        if (localmax && flux[f] > avg * 1.5f && flux[f] > fmax * 0.1f) beat[f] = 1;
    }

    // Assemble the blob: header + records.
    VizHeader h{};
    h.magic = miracle::kVizMagic; h.version = miracle::kVizVersion;
    h.band_count = kVizBands; h.frame_count = frames;
    h.device_rate = rate; h.hop_samples = hop;

    std::vector<uint8_t> out(sizeof(VizHeader) + size_t(frames) * sizeof(VizFrame));
    std::memcpy(out.data(), &h, sizeof(h));
    VizFrame* rec = reinterpret_cast<VizFrame*>(out.data() + sizeof(VizHeader));
    for (uint32_t f = 0; f < frames; ++f) {
        VizFrame v{};
        for (uint32_t b = 0; b < kVizBands; ++b) v.bands[b] = q(raw[f][b], bmax);
        v.rms   = q(rms[f],  rmax);
        v.onset = q(flux[f], fmax);
        v.low   = q(low[f],  lmax);
        v.mid   = q(mid[f],  mmax);
        v.high  = q(high[f], hmax);
        v.beat  = beat[f];
        rec[f] = v;
    }
    return out;
}

// Convenience: resample int16 -> analyse in one call (matches how the app bakes per-target).
inline std::vector<uint8_t> analyze_pcm(const int16_t* pcm, uint32_t n, uint32_t in_rate,
                                        uint32_t out_rate) {
    return analyze(resample_mono(pcm, n, in_rate, out_rate), out_rate);
}

} // namespace phxviz
#endif // PHXVIZ_ANALYZE_H
