// examples/miracle-player/src/player.h — the "A Small Miracle" music-visualizer game object.
// Engine-only (no platform/OS/SDK headers, no #ifdef PLATFORM): one App loop that streams the
// resident song PCM through the mixer and drives every visual from the precomputed viz track
// (viz.h) looked up by the audio sample cursor — so audio and video never drift. Compiles
// unchanged for host (software render) and GBA (PPU); only the linked backend + entry differ.
#ifndef MIRACLE_PLAYER_H
#define MIRACLE_PLAYER_H

#include "phx/runtime/app.h"
#include "phx/render/renderer.h"
#include "phx/input/input.h"
#include "phx/ui/ui.h"
#include "phx/audio/mixer.h"
#include "phx/audio/stream.h"
#include "phx/resource/cache.h"

#include "viz.h"
#include "particles.h"

#include <atomic>

namespace miracle {

using namespace phx;

// NOTE: the shared 5x7 debug font is UPPERCASE + digits + ":!.-" only (no lowercase, no "()/"),
// so every on-screen string here is uppercase and avoids unavailable glyphs.
inline constexpr char     kTitle[]       = "A SMALL MIRACLE";
inline constexpr uint32_t kMaxParticles  = 64;      // OAM budget: bars are BG, so <=64 sparks +
                                                    // ~43 text glyphs stays under the 128 OBJ ceiling
inline constexpr uint8_t  kNumStyles     = 2;       // A cycles: 0 = bars up, 1 = mirrored bars

// Where the piece is in its lifecycle. Intro/Outro are the required dedication cards.
enum class Phase : uint8_t { Intro, Playing, Paused, Outro };

class MiracleGame : public Game {
public:
    // --- set by the entry point before run() ---
    const char* bundle      = "miracle.phxp";
    uint32_t    mixer_rate  = 44100;   // 18157 on GBA; the rate the viz track was baked at
    bool        device_audio = false;  // desktop sets true (a real device drives samples_played);
                                       // headless keeps false and mixes a block per frame instead
    bool        loop        = false;   // restart at end instead of showing the outro

    // --- audio, shared with the audio thread through atomics ---
    AudioMixer*  mixer = nullptr;
    AudioStream  stream;
    int16_t*     ring = nullptr;
    SoundView    song{};
    // samples the DEVICE has consumed while playing (the single source of truth for position).
    // Written by the audio callback (desktop) or the headless stand-in; read on the game thread.
    // 32-bit: a full 4:22 track at 18157 Hz is ~4.8 M samples (<< 2^32), and the GBA (ARM7TDMI)
    // has no 64-bit atomic primitive — a 32-bit atomic is a plain word load/store there.
    std::atomic<uint32_t> samples_played{0};
    std::atomic<uint32_t> draining{0};   // 1 = audio callback should mix+advance; 0 = silent

    // Lock-free seek handshake (docs: reposition the stream cursor AND the viz index together).
    // The game thread publishes a target and stops pumping; the audio-owning side (the callback,
    // or the game itself when headless) performs the re-open. See apply_seek().
    std::atomic<uint32_t> seek_state{0};  // 0 = idle, 1 = a seek is requested
    uint32_t              seek_target = 0; // absolute sample to seek to (published before state)

    // --- visualization + resources ---
    const VizHeader* viz = nullptr;
    uint64_t         total_samples = 0;
    ResourceCache*   res  = nullptr;
    BitmapFont       font{};
    TextureId        heart_tex = kNoTexture;
    Camera2D         camera{};
    UI               ui;

    // The spectrogram is a BG tilemap so it renders natively on the GBA PPU (OBJ can't scale/tint,
    // so ui.rect bars would collapse to dots there). We own the cell buffer and rebuild it each
    // frame; refresh_tilemap() re-streams it on the PPU (the software backend reads it live).
    static constexpr int kSpecCols = 30, kSpecRows = 20;   // 240x160 / 8 = a screen of 8px tiles
    static constexpr int kSpecCells = kSpecCols * kSpecRows;
    static constexpr int kSpecBarRows = 15;                // bar area height in tiles (120 px)
    TilemapId spectrum_map = kNoTilemap;
    TextureId spark_tex    = kNoTexture;
    uint16_t* spec_idx = nullptr;                          // BG cells (0 = empty, b+1 = band b tile)
                                                           // — arena-allocated in on_start (EWRAM)

    // --- presentation state ---
    Phase    phase = Phase::Intro;
    uint32_t phase_frames = 0;          // frames spent in the current phase
    uint8_t  style = 0;                 // A cycles visualizer style
    bool     chrome = true;             // B toggles the transport UI chrome
    bool     show_profiler = false;     // SELECT toggles the frame-profiler overlay

    // reactive camera state (integer/Q16 so both tiers match; PPU ignores zoom, honours shake)
    int16_t  shake_q = 0;               // current screen-shake magnitude (decays each frame)
    int32_t  zoom_q16 = 1 << 16;        // current zoom (1.0), pulses on strong onsets

    // particles: a fixed-capacity pool from the engine arena (no hot-path heap; the pool + its
    // storage are carved once in on_start), integer sim (tier-identical)
    ParticlePool<kMaxParticles>* particles = nullptr;
    Rng                          rng{0xC0FFEEu};

    // --- Game hooks ---
    void on_start(App&) override;
    void on_fixed_update(App&, scalar) override;
    void on_render(App&, scalar) override;
    void on_stop(App&) override;

    // Advance the consumed-sample count. samples_played has exactly ONE writer in any config (the
    // headless stand-in on the game thread, OR the audio callback), so a plain load+store is
    // correct — and it avoids an atomic read-modify-write, which the GBA (ARM7TDMI) has no
    // instruction for (it would need a libatomic __atomic_fetch_add helper that isn't linked).
    void advance_played(uint32_t n) {
        samples_played.store(samples_played.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
    }

    // Perform a pending/immediate seek: re-open the stream at seek_target (a zero-copy sub-view
    // of the resident song) and snap the sample cursor. MUST run on the audio-owning side (the
    // callback when device_audio, else the game thread) — never concurrently with pump().
    void apply_seek();

    // --- accessors (drive the desktop audio glue + the headless suite assertions) ---
    uint64_t position_samples() const {
        uint64_t s = samples_played.load(std::memory_order_relaxed);
        return (total_samples && s > total_samples) ? total_samples : s;   // loop re-seeks to 0
    }
    // total_samples is a plain 64-bit value (no atomic) — fine on GBA; it never changes after boot.
    uint32_t viz_index() const { return viz ? viz_index_for_sample(viz, position_samples()) : 0; }
    const VizFrame* current_frame() const {
        return (viz && viz->frame_count) ? viz_frames(viz) + viz_index() : nullptr;
    }
    Phase current_phase() const { return phase; }

    // Deterministic observability for the headless suite (same idea as PlatformerGame::audio_peak).
    int32_t  audio_peak = 0;         // peak |sample| the mixer produced (proves the song streamed)
    uint32_t viz_active_frames = 0;  // rendered frames whose record had a nonzero band (reactivity)
    uint32_t play_frames = 0;        // headless frames advanced while draining (A/V-lock cross-check)

private:
    void begin_playback();              // Intro -> Playing: unmute the audio callback
    void seek_by_seconds(int secs);     // LEFT/RIGHT: request a ±N s seek (stream + viz together)
    void update_reactive(const VizFrame&); // particles + camera shake/zoom from the record
    void rebuild_spectrogram(const VizFrame&); // repaint the BG cell buffer from the band heights
    void draw_background(const VizFrame*);
    void draw_particles(Renderer&);
    void draw_intro();
    void draw_visualizer(const VizFrame&);
    void draw_outro(const VizFrame*);
    void headless_mix();                // stand in for the platform audio callback when headless
    void text_centered(int y, const char* s, Rgba color);
};

} // namespace miracle
#endif // MIRACLE_PLAYER_H
