// examples/tinyllm/src/tinyllm.h — the tinyllm game object: a language model generating text,
// live, inside the engine's fixed-step loop.
//
// Engine-only (no platform/OS/SDK headers, no `#ifdef PLATFORM`), so the SAME code runs on the
// host software renderer and on Game Boy Advance PPU hardware; only the entry point and the
// linked backend differ. The screen is one Mode-0 BG TILEMAP used as a 30x20 text console: cell
// index = glyph index into the baked font atlas, which is the native way to put 600 characters
// on a GBA (OBJ sprites cap at 128, so ui.text could not draw a screen of prose).
//
// Pacing: a token costs hundreds of milliseconds on a 16 MHz ARM7TDMI, so generation is pumped
// through the core's resumable state machine — llm_advance() does one bounded transformer stage
// per call, and the front end runs `stages_per_frame` of them per rendered frame. The console
// keeps redrawing (and the busy marker keeps turning) while a token is in flight, instead of the
// whole console freezing for half a second per word.
#ifndef TINYLLM_TINYLLM_H
#define TINYLLM_TINYLLM_H

#include "phx/runtime/app.h"
#include "phx/render/renderer.h"
#include "phx/input/input.h"
#include "phx/ui/ui.h"
#include "phx/resource/cache.h"

#include "llm.h"
#include "text_font.h"

namespace tinyllm {

using namespace phx;

// The console geometry: a screen of 8x8 tiles at the GBA's native 240x160.
inline constexpr int kCols       = 30;
inline constexpr int kRows       = 20;
inline constexpr int kStatusRow  = 0;                 // amber: tok/s, token count, context use
inline constexpr int kHelpRow    = kRows - 1;         // amber: the button legend
inline constexpr int kTextTop    = 1;
inline constexpr int kTextRows   = kHelpRow - kTextTop;
inline constexpr int kCells      = kCols * kRows;

// START cycles these. Kept short so the prompt plus a useful generation fits the context window.
inline constexpr int kPromptCount = 4;
extern const char* const kPrompts[kPromptCount];

class TinyLlmGame : public Game {
public:
    // --- set by the entry point before run() ---
    const char* bundle          = "tinyllm.phxp";
    uint32_t    kv_budget_bytes = 96u << 10;   // caps the KV cache; max_seq is derived from it
    // Work pacing, in bounded transformer STAGES per rendered frame.
    //
    // Deliberately NOT a wall-clock budget: the GBA platform's clock_ns() is a VIRTUAL clock
    // (it advances one microsecond per read and one sim step per vblank — see
    // engine/platform/src/gba/gba_platform.cpp), so a "run stages until 12 ms are gone" loop
    // silently never expires there and swallows a whole token in one frame. A fixed stage count
    // is honest on every target, keeps the display responsive, and is what makes the frame
    // counter usable as a real time base (see tokens_per_sec_x10).
    int32_t     stages_per_frame = 1;
    bool        auto_start      = true;        // begin generating without waiting for A
    bool        verify_checksum = false;       // see on_start(): the model blob is big and in ROM
    uint32_t    max_tokens      = 0;           // stop after N tokens (0 = run to EOS / full)
    bool        quit_when_done  = false;       // headless/test runs exit instead of idling

    // Sampling. The demo samples (a greedy 260K model loops badly); the suites force greedy.
    LlmSampler  sampler{};

    // --- engine resources ---
    ResourceCache* res = nullptr;
    LlmContext     llm{};
    BitmapFont     font{};                     // for the profiler overlay only (OBJ glyphs)
    TextureId      font_tex     = kNoTexture;
    TilemapId      console_map  = kNoTilemap;
    uint16_t*      cells        = nullptr;     // kCells BG tiles, arena-allocated (EWRAM)
    Camera2D       camera{};
    UI             ui;

    // --- console state ---
    int32_t  cur_col = 0, cur_row = kTextTop;
    char     word[kCols] = {};                 // pending word, for soft wrapping
    int32_t  word_len = 0;
    // The pending word is also drawn TENTATIVELY each frame, so the newest text is on screen the
    // instant a token lands instead of waiting for the space that ends the word (and so a final
    // partial word is never silently dropped). These record what to erase before the next commit.
    int32_t  pend_row = 0, pend_col = 0, pend_n = 0;

    // --- generation state ---
    bool     generating   = false;
    bool     finished     = false;             // hit EOS or filled the window
    bool     model_ok     = false;
    int32_t  prompt_index = 0;
    uint32_t tokens_out   = 0;
    uint32_t spinner      = 0;
    bool     show_profiler = false;
    // Elapsed 60 Hz sim steps while generating — the time base (see tokens_per_sec_x10).
    uint32_t gen_steps    = 0;
    uint32_t sim_hz       = 60;
    uint64_t last_pump_frame = ~0ull;          // pump once per RENDERED frame, not per sim step

    // --- Game hooks ---
    void on_start(App&) override;
    void on_fixed_update(App&, scalar) override;
    void on_render(App&, scalar) override;
    void on_stop(App&) override;

    // --- observability (the headless runner and the suite read these) ---
    uint32_t tokens_generated() const { return tokens_out; }
    // Tenths of a token per second, integer — no float anywhere, so the readout is identical on
    // both scalar tiers. 0 until the first token completes.
    //
    // The time base is ELAPSED SIM STEPS, not a wall clock: the engine's fixed-step loop runs one
    // step per elapsed vblank on GBA, so `gen_steps / sim_hz` is real seconds of console time,
    // whereas the platform clock there is virtual and would report nonsense. (The loop clamps
    // catch-up at 5 steps per frame, so this over-reports only if a single frame exceeds ~83 ms;
    // examples/tinyllm/README.md records the measured wall-clock cross-check.)
    uint32_t tokens_per_sec_x10() const;
    uint32_t elapsed_steps() const { return gen_steps; }
    int32_t  context_used() const { return llm.pos; }
    int32_t  context_max()  const { return llm.max_seq; }
    // The generated text so far, NUL-terminated (the console grid read back as characters).
    const char* transcript() const { return text_; }
    uint32_t    transcript_len() const { return text_len_; }

    // Restart from prompt `i` with a cleared context. Also the B button (same prompt) and
    // START (the next one).
    void restart(int32_t prompt_i);

private:
    void console_clear();
    void console_newline();
    void console_putc(char c);
    void console_write(const char* s);
    void flush_word();
    void draw_pending();     // show the partially-typed word before it is committed
    void clear_pending();
    void put_cell(int32_t row, int32_t col, char c, bool amber);
    void write_row(int32_t row, const char* s, bool amber);
    void draw_status();
    void pump();
    void emit_token();

    // A flat transcript so headless runs can print what was generated without scraping tiles.
    static constexpr uint32_t kTranscriptCap = 1024;
    char     text_[kTranscriptCap] = {};
    uint32_t text_len_ = 0;
};

} // namespace tinyllm
#endif // TINYLLM_TINYLLM_H
