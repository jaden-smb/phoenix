// examples/emberwing/audio_gen.h — HOST-ONLY sound synthesis for the bake step. A tiny
// square/noise synthesizer produces every SFX, and a 3-voice pattern tracker (square lead,
// square bass, noise hats) renders the looping level theme. Everything is mono 16-bit PCM at
// 22050 Hz; the BundleWriter's tier-0 encode resamples it to the GBA Direct Sound rate at
// bake time. Never compiled into the game binary (STL allowed).
#ifndef EMBERWING_AUDIO_GEN_H
#define EMBERWING_AUDIO_GEN_H

#include <cmath>
#include <cstdint>
#include <vector>

namespace game {
namespace audio_gen {

constexpr uint32_t kRate = 22050;

using Buf = std::vector<int16_t>;

inline int16_t sat16(int32_t v) {
    return int16_t(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
}

// Gentle 3-tap lowpass (¼,½,¼), applied at bake time. Square waves are rich in harmonics
// that FOLD when the tier-0 encode resamples 22050 -> 18157 Hz for the GBA — unfiltered
// they alias into a metallic fizz on the DirectSound DAC. One pass tames the top octave
// while leaving the chip timbre intact (and costs nothing at runtime on any tier).
inline void soften(Buf& b) {
    if (b.size() < 3) return;
    int32_t prev = b[0];
    for (size_t i = 1; i + 1 < b.size(); ++i) {
        const int32_t cur = b[i];
        b[i] = sat16((prev + 2 * cur + b[i + 1]) / 4);
        prev = cur;
    }
}

// ---- oscillators ---------------------------------------------------------------------------
// Phase runs 0..1 per cycle. duty in [0,1] (0.5 = square, 0.25 = thin NES-ish lead).
inline double osc_square(double phase, double duty) {
    return (phase - std::floor(phase)) < duty ? 1.0 : -1.0;
}

// Deterministic 16-bit LFSR noise (the classic retro noise channel).
struct Noise {
    uint16_t lfsr = 0xACE1;
    double step() {
        const uint16_t bit = uint16_t(((lfsr >> 0) ^ (lfsr >> 2) ^ (lfsr >> 3) ^ (lfsr >> 5)) & 1u);
        lfsr = uint16_t((lfsr >> 1) | (bit << 15));
        return (lfsr & 1u) ? 1.0 : -1.0;
    }
};

// ---- SFX building blocks ---------------------------------------------------------------------
// A square tone sweeping f0 -> f1 over `secs`, with a linear decay envelope and `amp` peak.
inline void tone_sweep(Buf& out, double f0, double f1, double secs, double amp,
                       double duty = 0.5, bool attack = false) {
    const uint32_t n = uint32_t(secs * kRate);
    double phase = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
        const double t = double(i) / double(n);
        const double f = f0 + (f1 - f0) * t;
        phase += f / kRate;
        const double env = attack ? t : (1.0 - t);
        out.push_back(sat16(int32_t(osc_square(phase, duty) * env * amp * 32767.0)));
    }
}

// A short fixed-pitch note with decay (for arpeggio chimes).
inline void tone_note(Buf& out, double freq, double secs, double amp, double duty = 0.5) {
    tone_sweep(out, freq, freq, secs, amp, duty);
}

// A noise burst; `attack` swells instead of decaying (geyser). `lp` 1..8 = crude low-pass
// (moving average width) to soften thuds.
inline void noise_burst(Buf& out, double secs, double amp, int lp = 1, bool attack = false) {
    const uint32_t n = uint32_t(secs * kRate);
    Noise nz;
    double acc[8] = {};
    for (uint32_t i = 0; i < n; ++i) {
        for (int k = lp - 1; k > 0; --k) acc[k] = acc[k - 1];
        acc[0] = nz.step();
        double s = 0.0;
        for (int k = 0; k < lp; ++k) s += acc[k];
        s /= lp;
        const double t = double(i) / double(n);
        const double env = attack ? t : (1.0 - t) * (1.0 - t);
        out.push_back(sat16(int32_t(s * env * amp * 32767.0)));
    }
}

// ---- the SFX set ------------------------------------------------------------------------------
inline Buf sfx_jump()   { Buf b; tone_sweep(b, 280, 760, 0.13, 0.55, 0.25); soften(b); return b; }
inline Buf sfx_hurt()   { Buf b; tone_sweep(b, 480, 130, 0.24, 0.60, 0.5);  soften(b); return b; }
inline Buf sfx_stomp()  { Buf b; noise_burst(b, 0.10, 0.70, 6);
                          tone_sweep(b, 200, 90, 0.06, 0.4); return b; }
inline Buf sfx_geyser() { Buf b; noise_burst(b, 0.30, 0.45, 3, true);
                          noise_burst(b, 0.12, 0.40, 3); return b; }
inline Buf sfx_ember()  { Buf b; tone_note(b, 1046.5, 0.045, 0.42, 0.25);
                          tone_note(b, 1568.0, 0.075, 0.42, 0.25); return b; }
inline Buf sfx_heart()  { Buf b; tone_note(b, 659.3, 0.09, 0.40);
                          tone_note(b, 880.0, 0.14, 0.40); return b; }
inline Buf sfx_shard()  { Buf b;
    const double n[4] = { 1046.5, 1318.5, 1760.0, 2093.0 };
    for (double f : n) tone_note(b, f, 0.05, 0.42, 0.25);
    tone_note(b, 2093.0, 0.12, 0.35, 0.25);
    return b; }
inline Buf sfx_checkpoint() { Buf b;
    const double n[3] = { 880.0, 1108.7, 1318.5 };                  // A5 C#6 E6
    for (double f : n) tone_note(b, f, 0.07, 0.45, 0.5);
    tone_note(b, 1760.0, 0.18, 0.38, 0.5);
    return b; }
inline Buf sfx_goal() { Buf b;
    const double n[3] = { 784.0, 1046.5, 1318.5 };                  // G5 C6 E6
    for (double f : n) tone_note(b, f, 0.10, 0.5, 0.25);
    tone_note(b, 1568.0, 0.42, 0.5, 0.25);                          // G6 held
    return b; }

// ---- the level theme ---------------------------------------------------------------------------
// A 3-voice tracker over a 16th-note grid: 128 steps (two 8-bar sections) at ~136 BPM.
// Notes are MIDI numbers; 0 = rest. The melody sits in A-minor pentatonic with passing
// tones, bass walks the A–F–C–G roots, and the noise channel plays tick/backbeat hats.
inline double midi_hz(int m) { return 440.0 * std::pow(2.0, (m - 69) / 12.0); }

inline Buf music_theme() {
    constexpr int kSteps = 128;
    constexpr double kStepSecs = 0.115;
    // lead (square 25%). Section A: a rising call answered low; B: the answer up a third.
    static const uint8_t lead[kSteps] = {
        // A1
        69,  0, 72,  0, 76,  0, 74, 72,  69,  0,  0,  0, 67,  0, 69,  0,
        72,  0, 76,  0, 79,  0, 76, 74,  76,  0,  0,  0,  0,  0, 74, 72,
        69,  0, 72,  0, 76,  0, 74, 72,  69,  0,  0,  0, 67,  0, 64,  0,
        67,  0, 69,  0, 72,  0, 69, 67,  69,  0,  0,  0,  0,  0,  0,  0,
        // B
        76,  0, 79,  0, 81,  0, 79, 76,  74,  0,  0,  0, 72,  0, 74,  0,
        76,  0, 79,  0, 81,  0, 84,  0,  81,  0, 79,  0, 76,  0, 74,  0,
        72,  0, 76,  0, 74,  0, 72,  0,  69,  0,  0,  0, 67,  0, 69,  0,
        72,  0, 69,  0, 67,  0, 64,  0,  69,  0,  0,  0,  0,  0,  0,  0,
    };
    // bass (square 50%), one note per half-beat
    static const uint8_t bass[kSteps] = {
        45, 0, 0, 0, 45, 0, 0, 0, 45, 0, 0, 0, 45, 0, 52, 0,
        41, 0, 0, 0, 41, 0, 0, 0, 48, 0, 0, 0, 48, 0, 0, 0,
        45, 0, 0, 0, 45, 0, 0, 0, 45, 0, 0, 0, 45, 0, 52, 0,
        43, 0, 0, 0, 43, 0, 0, 0, 45, 0, 0, 0, 45, 0, 0, 0,
        41, 0, 0, 0, 41, 0, 0, 0, 45, 0, 0, 0, 45, 0, 0, 0,
        48, 0, 0, 0, 48, 0, 0, 0, 43, 0, 0, 0, 43, 0, 0, 0,
        41, 0, 0, 0, 41, 0, 0, 0, 45, 0, 0, 0, 45, 0, 52, 0,
        43, 0, 0, 0, 43, 0, 0, 0, 45, 0, 0, 0, 45, 0, 0, 0,
    };

    const uint32_t step_n = uint32_t(kStepSecs * kRate);
    Buf out(size_t(step_n) * kSteps, 0);

    // lead + bass: each note rings from its step until the next note (with per-step decay)
    auto render_voice = [&](const uint8_t* seq, double duty, double amp, double decay) {
        double phase = 0.0;
        int cur = 0;
        uint32_t age = 0;
        for (int s = 0; s < kSteps; ++s) {
            if (seq[s]) { cur = seq[s]; age = 0; }
            for (uint32_t i = 0; i < step_n; ++i, ++age) {
                if (!cur) continue;
                phase += midi_hz(cur) / kRate;
                const double env = std::max(0.0, 1.0 - decay * (double(age) / kRate));
                const size_t idx = size_t(s) * step_n + i;
                out[idx] = sat16(out[idx] + int32_t(osc_square(phase, duty) * env * amp * 32767.0));
            }
        }
    };
    render_voice(lead, 0.25, 0.28, 2.2);
    render_voice(bass, 0.50, 0.22, 1.2);

    // hats: a soft tick on every beat, a brighter one on the backbeat
    Noise nz;
    for (int s = 0; s < kSteps; s += 4) {
        const bool back = (s % 16) == 8;
        const uint32_t len = uint32_t((back ? 0.05 : 0.02) * kRate);
        for (uint32_t i = 0; i < len; ++i) {
            const double env = 1.0 - double(i) / len;
            const size_t idx = size_t(s) * step_n + i;
            out[idx] = sat16(out[idx] + int32_t(nz.step() * env * (back ? 0.08 : 0.05) * 32767.0));
        }
    }
    soften(out);           // twice: the melody carries the loop; the fizz must not
    soften(out);
    return out;
}

} // namespace audio_gen
} // namespace game
#endif // EMBERWING_AUDIO_GEN_H
