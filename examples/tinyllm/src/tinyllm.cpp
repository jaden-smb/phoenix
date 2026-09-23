// examples/tinyllm/src/tinyllm.cpp — the tinyllm game logic (engine-only). One App loop that
// pumps the resumable inference core a bounded amount per frame and paints the result into a
// BG tilemap text console. No platform headers, no float on the model path.
#include "tinyllm.h"

namespace tinyllm {

// Verification window for a headless emulator run. Every console example in this tree publishes
// its verdict through C-linkage globals a GDB stub can read (see STATUS.md's mGBA recipe — the
// GBA has no stdout), and for this example the number that matters — tokens/sec on real
// silicon — is only measurable on the device. ~300 bytes of .bss, live in every build so the
// shipping ROM is the one being measured.
extern "C" {
volatile uint32_t phx_tinyllm_ready    = 0;   // 1 once the model validated and initialised
volatile uint32_t phx_tinyllm_tokens   = 0;
volatile uint32_t phx_tinyllm_tps_x10  = 0;   // tokens/sec x10, from the 60 Hz sim-step clock
volatile uint32_t phx_tinyllm_steps    = 0;   // elapsed fixed steps == elapsed vblanks on GBA
volatile int32_t  phx_tinyllm_ctx      = 0;
volatile int32_t  phx_tinyllm_ctx_max  = 0;
volatile uint32_t phx_tinyllm_arena    = 0;   // bytes the inference core took from the arena
char              phx_tinyllm_text[256] = {}; // the generated transcript, NUL-terminated
}

const char* const kPrompts[kPromptCount] = {
    "Once upon a time",
    "The little robot",
    "One day Tim found a",
    "Lily and Ben went to the",
};

namespace {

constexpr int kScreenW = 240, kScreenH = 160;

inline vec2 v2(int x, int y) { return vec2{ s_from_int(x), s_from_int(y) }; }

TextureId load_tex(ResourceCache* res, Renderer& r, NameHash h) {
    auto tr = res->texture(h);
    if (!tr) return kNoTexture;
    const TextureView v = tr.unwrap();
    TextureDesc d{};
    d.pixels = v.pixels; d.width = v.width; d.height = v.height; d.format = v.format;
    return r.load_texture(d);
}

// Append an unsigned decimal to `dst` at `n`, right-aligned into `width` (0 = natural width).
int32_t put_uint(char* dst, int32_t n, int32_t cap, uint32_t v, int32_t width = 0) {
    char tmp[12];
    int32_t k = 0;
    do { tmp[k++] = char('0' + (v % 10u)); v /= 10u; } while (v && k < 12);
    for (int32_t pad = k; pad < width && n < cap; ++pad) dst[n++] = ' ';
    while (k > 0 && n < cap) dst[n++] = tmp[--k];
    return n;
}

int32_t put_str(char* dst, int32_t n, int32_t cap, const char* s) {
    while (*s && n < cap) dst[n++] = *s++;
    return n;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Console
// ---------------------------------------------------------------------------------------------

void TinyLlmGame::put_cell(int32_t row, int32_t col, char c, bool amber) {
    if (!cells || row < 0 || row >= kRows || col < 0 || col >= kCols) return;
    cells[row * kCols + col] = font_cell(c, amber);
}

void TinyLlmGame::write_row(int32_t row, const char* s, bool amber) {
    for (int32_t c = 0; c < kCols; ++c)
        put_cell(row, c, s[0] ? *s++ : ' ', amber);
}

void TinyLlmGame::console_clear() {
    pend_n = 0;
    if (cells) for (int32_t i = 0; i < kCells; ++i) cells[i] = 0;
    cur_col = 0; cur_row = kTextTop; word_len = 0;
    text_len_ = 0; text_[0] = '\0';
}

void TinyLlmGame::console_newline() {
    cur_col = 0;
    if (++cur_row > kTextTop + kTextRows - 1) {
        // Scroll the text region up one row; the status and help rows are rewritten each frame
        // and must not move.
        for (int32_t r = kTextTop; r < kTextTop + kTextRows - 1; ++r)
            for (int32_t c = 0; c < kCols; ++c)
                cells[r * kCols + c] = cells[(r + 1) * kCols + c];
        cur_row = kTextTop + kTextRows - 1;
        for (int32_t c = 0; c < kCols; ++c) cells[cur_row * kCols + c] = 0;
    }
}

// Soft word wrap: printable characters accumulate into `word` and only land on the grid when the
// word is finished, so a token boundary never splits a word across two lines.
void TinyLlmGame::flush_word() {
    if (word_len == 0) return;
    if (cur_col + word_len > kCols) console_newline();
    for (int32_t i = 0; i < word_len; ++i) {
        if (cur_col >= kCols) console_newline();
        put_cell(cur_row, cur_col++, word[i], false);
    }
    word_len = 0;
}

void TinyLlmGame::console_putc(char c) {
    if (text_len_ + 1 < kTranscriptCap) { text_[text_len_++] = c; text_[text_len_] = '\0'; }

    if (c == '\n') { flush_word(); console_newline(); return; }
    if (c == ' ') {
        flush_word();
        if (cur_col > 0 && cur_col < kCols) put_cell(cur_row, cur_col++, ' ', false);
        return;
    }
    if (word_len >= kCols) flush_word();          // a word longer than the line: hard-break it
    word[word_len++] = c;
}

void TinyLlmGame::console_write(const char* s) {
    while (*s) console_putc(*s++);
}

// Paint the not-yet-committed word where it will land, without advancing the cursor.
void TinyLlmGame::draw_pending() {
    pend_n = 0;
    if (word_len <= 0 || !cells) return;
    int32_t row = cur_row, col = cur_col;
    if (col + word_len > kCols) { ++row; col = 0; }      // where flush_word() would wrap it
    if (row > kTextTop + kTextRows - 1) return;          // it will scroll into view on commit
    pend_row = row; pend_col = col;
    for (int32_t i = 0; i < word_len && col < kCols; ++i, ++col, ++pend_n)
        cells[row * kCols + col] = font_cell(word[i], false);
}

// Erase it again. MUST run before anything commits text, or the commit's own cells get wiped.
void TinyLlmGame::clear_pending() {
    for (int32_t i = 0; i < pend_n; ++i) {
        const int32_t col = pend_col + i;
        if (pend_row >= 0 && pend_row < kRows && col < kCols)
            cells[pend_row * kCols + col] = 0;
    }
    pend_n = 0;
}

// ---------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------

void TinyLlmGame::on_start(App& app) {
    ArenaAllocator& A = app.mem().persistent();
    Renderer& r = app.render();

    res = ResourceCache::create(A).unwrap();
    // The model is ~300 KB of the bundle and a mount-time CRC32 over all of it is seconds of
    // black screen on a 16 MHz CPU. The bundle is ROM-resident and immutable on console, and
    // mount still performs every structural check; the model has its own CRC in its header.
    if (res->mount(app.platform(), bundle, verify_checksum) != Status::Ok)
        PHX_LOG_ERROR("tinyllm: failed to mount '%s'", bundle);

    font_tex = load_tex(res, r, "font"_hash);
    font.tex = font_tex;
    font.glyph_w = 8; font.glyph_h = 8; font.cols = uint8_t(kFontCols); font.first_char = 32;
    font.advance = 6; font.line_h = 8;

    // The console cell buffer lives in the arena (EWRAM), never inline in the game object — the
    // GBA's IWRAM stack is tiny.
    cells = static_cast<uint16_t*>(A.alloc(kCells * sizeof(uint16_t)));
    console_clear();

    if (font_tex != kNoTexture) {
        TilemapDesc md{};
        md.indices = cells;
        md.width = kCols; md.height = kRows; md.layers = 1;
        md.tile_w = 8; md.tile_h = 8; md.tileset = font_tex;
        console_map = r.upload_tilemap(md);
    }

    // The model: a generic Blob in the bundle, read zero-copy in place. On GBA that pointer is
    // in cartridge ROM and nothing is ever copied out of it.
    auto br = res->blob("model"_hash);
    if (!br) {
        PHX_LOG_ERROR("tinyllm: no 'model' asset in the bundle");
        console_write("no model in bundle\n");
        return;
    }
    const BlobView b = br.unwrap();
    const LlmHeader* h = llm_validate(b.data, b.size);
    if (!h) {
        PHX_LOG_ERROR("tinyllm: the 'model' blob failed validation (%u bytes)", b.size);
        console_write("model blob invalid\n");
        return;
    }

    const uint32_t need = llm_required_arena_bytes(h, kv_budget_bytes);
    PHX_LOG_INFO("tinyllm: model %ux%u vocab %u, ctx %d, arena %u bytes",
                 h->dim, h->n_layers, h->vocab_size, llm_max_seq(h, kv_budget_bytes), need);

    sim_hz = app.config().sim_hz ? app.config().sim_hz : 60;
    llm.sampler = sampler;
    if (!llm_init(llm, b.data, b.size, A, kv_budget_bytes)) {
        PHX_LOG_ERROR("tinyllm: llm_init failed (arena needs %u bytes)", need);
        console_write("model init failed\n");
        return;
    }
    model_ok = true;
    restart(prompt_index);
    generating = auto_start;
}

void TinyLlmGame::restart(int32_t prompt_i) {
    if (!model_ok) return;
    prompt_index = ((prompt_i % kPromptCount) + kPromptCount) % kPromptCount;

    llm.sampler = sampler;
    llm_reset(llm);
    console_clear();
    tokens_out = 0;
    gen_steps = 0;
    finished = false;

    const char* p = kPrompts[prompt_index];
    // Encode straight into the context's own prompt staging buffer bound — no allocation.
    uint16_t ids[128];
    const int32_t n = llm_encode(llm, p, /*add_bos=*/true, ids,
                                 llm.max_seq < 128 ? llm.max_seq : 128);
    if (n <= 0 || !llm_prefill(llm, ids, n)) {
        console_write("prompt does not fit\n");
        finished = true;
        return;
    }
    console_write(p);      // echo the prompt so the screen reads as one continuous story
}

// ---------------------------------------------------------------------------------------------
// Update
// ---------------------------------------------------------------------------------------------

void TinyLlmGame::emit_token() {
    ++tokens_out;
    if (llm.hdr && llm.out_token == llm.hdr->eos_id) {
        console_write("\n[end]");
        generating = false;
        finished = true;
        return;
    }
    char buf[64];
    llm_decode(llm, llm.prev_token, llm.out_token, buf, sizeof buf);
    console_write(buf);
    if (max_tokens && tokens_out >= max_tokens) { generating = false; finished = true; }
}

// Run `stages_per_frame` bounded transformer stages. Called once per RENDERED frame (not once
// per sim step): the engine's fixed-step loop issues catch-up steps when a frame overruns, and
// doing inference in each of them would make the work grow the further behind it gets.
void TinyLlmGame::pump() {
    if (!generating || !model_ok || finished) return;
    clear_pending();

    for (int32_t done = 0; done < stages_per_frame; ++done) {
        const LlmStep s = llm_advance(llm);
        if (s == LlmStep::Token) { emit_token(); break; }
        if (s == LlmStep::Full) {
            console_write("\n[context full]");
            generating = false; finished = true;
            break;
        }
        if (s == LlmStep::Idle) { generating = false; break; }
    }
}

void TinyLlmGame::on_fixed_update(App& app, scalar /*dt*/) {
    const InputState& in = app.input();

    if (in.just(Button::Select)) show_profiler = !show_profiler;
    if (in.just(Button::B))      restart(prompt_index);
    if (in.just(Button::Start))  restart(prompt_index + 1);
    if (in.just(Button::A)) {
        if (finished) restart(prompt_index);
        else          generating = !generating;
    }

    if (generating) { ++spinner; ++gen_steps; }

    // One pump per rendered frame. app.frame() advances once per loop iteration, while this hook
    // can run several times in a catch-up frame; gen_steps counts those, and IS the 60 Hz clock.
    if (app.frame() != last_pump_frame) {
        last_pump_frame = app.frame();
        pump();
        draw_pending();
    }

    // Publish the live counters (see the block at the top of this file).
    phx_tinyllm_steps   = gen_steps;
    phx_tinyllm_ready   = model_ok ? 1u : 0u;
    phx_tinyllm_tokens  = tokens_out;
    phx_tinyllm_tps_x10 = tokens_per_sec_x10();
    phx_tinyllm_ctx     = llm.pos;
    phx_tinyllm_ctx_max = llm.max_seq;
    phx_tinyllm_arena   = llm.arena_bytes;
    {
        const uint32_t n = text_len_ < sizeof(phx_tinyllm_text) - 1
                         ? text_len_ : uint32_t(sizeof(phx_tinyllm_text) - 1);
        for (uint32_t i = 0; i < n; ++i) phx_tinyllm_text[i] = text_[i];
        phx_tinyllm_text[n] = '\0';
    }

    if (finished && quit_when_done) app.request_quit();
}

// ---------------------------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------------------------

uint32_t TinyLlmGame::tokens_per_sec_x10() const {
    if (!gen_steps || !tokens_out) return 0;
    // tokens * 10 * sim_hz / steps, all integers — identical on both scalar tiers.
    return uint32_t((uint64_t(tokens_out) * 10ull * uint64_t(sim_hz)) / uint64_t(gen_steps));
}

void TinyLlmGame::draw_status() {
    char line[kCols + 1];
    int32_t n = 0;
    const uint32_t tps = tokens_per_sec_x10();
    n = put_uint(line, n, kCols, tps / 10u);
    n = put_str (line, n, kCols, ".");
    n = put_uint(line, n, kCols, tps % 10u);
    n = put_str (line, n, kCols, " t/s ");
    n = put_uint(line, n, kCols, tokens_out, 3);
    n = put_str (line, n, kCols, "tok ");
    n = put_uint(line, n, kCols, uint32_t(llm.pos), 3);
    n = put_str (line, n, kCols, "/");
    n = put_uint(line, n, kCols, uint32_t(llm.max_seq));
    while (n < kCols) line[n++] = ' ';
    line[kCols] = '\0';

    // A busy marker in the last column, so a slow model reads as working rather than hung. It
    // also shows WHICH stage of the token is in flight, which is the honest signal.
    if (generating && !finished) {
        static const char kSpin[4] = { '|', '/', '-', '\\' };
        line[kCols - 1] = kSpin[(spinner >> 2) & 3];
    } else if (finished) {
        line[kCols - 1] = '.';
    } else {
        line[kCols - 1] = '=';                 // paused
    }
    write_row(kStatusRow, line, true);
    write_row(kHelpRow, model_ok ? "A run  B reset  START prompt"
                                 : "no model loaded", true);
}

void TinyLlmGame::on_render(App& app, scalar /*alpha*/) {
    Renderer& r = app.render();
    r.begin_frame(camera);
    ui.begin(r, app.input());

    draw_status();
    if (console_map != kNoTilemap) {
        r.refresh_tilemap(console_map);        // re-stream the mutated cells (PPU); live on soft
        r.draw_tilemap(console_map, 0);
    }
    if (show_profiler)
        ui.profile_overlay(v2(kScreenW - 96, 2), app.profile(), &font);

    ui.end();
    r.end_frame();
}

void TinyLlmGame::on_stop(App&) {}

} // namespace tinyllm
