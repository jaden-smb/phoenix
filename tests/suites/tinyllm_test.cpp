// tests/suites/tinyllm_test.cpp — end-to-end verification of the tinyllm example: the
// fixed-point transformer generating tokens, and the whole thing running inside the real
// fixed-step App loop with a baked bundle mounted through the resource cache.
//
// THE LOAD-BEARING ASSERTION is the golden token file. tests/fixtures/tinyllm_golden.txt was
// produced by a completely independent implementation of the same integer arithmetic — the
// Python reference in examples/tinyllm/tools/export_model.py — reading the exact fixture blob
// this suite builds. Two implementations agreeing bit-for-bit is the only practical way to catch
// a fixed-point bug before it reaches a ROM, where it is nearly impossible to isolate.
//
// Everything printed here is deterministic integer state, so `make determinism` runs this on
// BOTH scalar tiers and diffs the output byte-for-byte: if float ever leaks into the core, the
// tiers diverge and this is where it shows.
#include "../../examples/tinyllm/src/tinyllm.h"
#include "../../examples/tinyllm/src/llm_build.h"
#include "../../examples/tinyllm/src/bake.h"
#include "phx/platform/platform.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" void phx_null_set_step_ns(uint64_t);
extern "C" void phx_null_set_max_frames(uint64_t);

using namespace phx;
using namespace tinyllm;

namespace {

// The golden lives in the source tree; `make` runs from the repo root, while CTest runs from the
// build tree, so CMake passes an absolute path in.
#ifndef TINYLLM_GOLDEN_PATH
#define TINYLLM_GOLDEN_PATH "tests/fixtures/tinyllm_golden.txt"
#endif

const char* kBundle = "build/tinyllm_test.phxp";
const char* kGolden = TINYLLM_GOLDEN_PATH;

int g_fail = 0;
void CK(bool c, const char* m) { if (!c) { ++g_fail; std::printf("    FAIL %s\n", m); } }

// --- the golden file ----------------------------------------------------------------------

struct Golden {
    bool                  loaded = false;
    uint32_t              blob_size = 0, blob_crc = 0, seed = 0;
    int32_t               temperature_q16 = 0, topp_q16 = 0;
    std::string           prompt;
    std::vector<uint16_t> prompt_tokens, fixed_tokens, float_tokens, sampled_tokens;
};

void parse_ids(const char* s, std::vector<uint16_t>& out) {
    while (*s) {
        while (*s == ' ') ++s;
        if (!*s) break;
        out.push_back(uint16_t(std::strtoul(s, nullptr, 10)));
        while (*s && *s != ' ') ++s;
    }
}

Golden load_golden(const char* path) {
    Golden g;
    FILE* f = std::fopen(path, "r");
    if (!f) return g;
    char line[8192];
    while (std::fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        if (char* nl = std::strchr(line, '\n')) *nl = '\0';
        auto is = [&](const char* k) { return std::strncmp(line, k, std::strlen(k)) == 0; };
        auto rest = [&](const char* k) { return line + std::strlen(k); };
        if      (is("blob_size "))      g.blob_size = uint32_t(std::strtoul(rest("blob_size "), nullptr, 10));
        else if (is("blob_crc "))       g.blob_crc  = uint32_t(std::strtoul(rest("blob_crc "), nullptr, 10));
        else if (is("seed "))           g.seed      = uint32_t(std::strtoul(rest("seed "), nullptr, 10));
        else if (is("temperature_q16 "))g.temperature_q16 = int32_t(std::strtol(rest("temperature_q16 "), nullptr, 10));
        else if (is("topp_q16 "))       g.topp_q16  = int32_t(std::strtol(rest("topp_q16 "), nullptr, 10));
        else if (is("prompt "))         g.prompt    = rest("prompt ");
        else if (is("prompt_tokens "))  parse_ids(rest("prompt_tokens "), g.prompt_tokens);
        else if (is("fixed_tokens "))   parse_ids(rest("fixed_tokens "), g.fixed_tokens);
        else if (is("float_tokens "))   parse_ids(rest("float_tokens "), g.float_tokens);
        else if (is("sampled_tokens ")) parse_ids(rest("sampled_tokens "), g.sampled_tokens);
    }
    std::fclose(f);
    g.loaded = !g.prompt_tokens.empty() && !g.fixed_tokens.empty();
    return g;
}

bool same_ids(const std::vector<uint16_t>& want, const uint16_t* got, int32_t n, const char* what) {
    if (int32_t(want.size()) != n) {
        std::printf("    FAIL %s: got %d tokens, golden has %zu\n", what, n, want.size());
        return false;
    }
    for (int32_t i = 0; i < n; ++i) {
        if (want[size_t(i)] != got[i]) {
            std::printf("    FAIL %s: token %d is %u, golden says %u\n",
                        what, i, unsigned(got[i]), unsigned(want[size_t(i)]));
            return false;
        }
    }
    return true;
}

// --- a standalone core instance over the generated fixture ----------------------------------

struct Core {
    std::vector<uint32_t> words;          // 4-byte-aligned storage for the blob
    uint32_t              size = 0;
    std::vector<uint8_t>  mem;
    ArenaAllocator        arena;
    LlmContext            ctx{};
    bool                  ok = false;

    explicit Core(const std::vector<uint8_t>& blob, uint32_t kv_budget = 1u << 16) {
        size = uint32_t(blob.size());
        words.assign((blob.size() + 3) / 4, 0u);
        std::memcpy(words.data(), blob.data(), blob.size());
        const LlmHeader* h = llm_validate(bytes(), size);
        if (!h) return;
        const uint32_t need = llm_required_arena_bytes(h, kv_budget);
        mem.assign(need + 64, 0);
        arena.init(mem.data(), mem.size());
        ok = llm_init(ctx, bytes(), size, arena, kv_budget);
    }
    uint8_t* bytes() { return reinterpret_cast<uint8_t*>(words.data()); }
};

// --- 1. the core against the golden ----------------------------------------------------------

void test_core(const std::vector<uint8_t>& blob, const Golden& g) {
    Core c(blob);
    CK(c.ok, "core: llm_init on the fixture blob");
    if (!c.ok) return;

    // Encode must reproduce the golden's prompt tokens — that pins the BPE merge order and the
    // sorted-table binary search, not just the transformer.
    uint16_t ids[256];
    const int32_t n = llm_encode(c.ctx, g.prompt.c_str(), true, ids, 256);
    CK(n > 0, "core: prompt encodes");
    CK(same_ids(g.prompt_tokens, ids, n, "core/encode"), "core: prompt tokens match the golden");
    if (n <= 0) return;

    // Greedy generation.
    c.ctx.sampler.greedy = true;
    llm_reset(c.ctx);
    CK(llm_prefill(c.ctx, ids, n), "core: prefill accepted the prompt");
    std::vector<uint16_t> got;
    for (size_t i = 0; i < g.fixed_tokens.size(); ++i) {
        const LlmStep s = llm_run_token(c.ctx);
        if (s != LlmStep::Token) { std::printf("    FAIL core: step %zu returned %d\n", i, int(s)); break; }
        got.push_back(llm_token(c.ctx));
    }
    CK(same_ids(g.fixed_tokens, got.data(), int32_t(got.size()), "core/greedy"),
       "core: greedy tokens match the golden bit-for-bit");
    CK(c.ctx.tokens_out == uint32_t(got.size()), "core: token counter tracks output");

    // Temperature + top-p sampling, seeded. This pins the ENTIRE fixed-point sampler: the
    // reciprocal temperature, the ROM exp table, the nucleus cutoff, the partial selection sort,
    // and the xorshift draw.
    c.ctx.sampler.greedy = false;
    c.ctx.sampler.temperature_q16 = g.temperature_q16;
    c.ctx.sampler.topp_q16 = g.topp_q16;
    c.ctx.sampler.seed = g.seed;
    llm_reset(c.ctx);
    CK(llm_prefill(c.ctx, ids, n), "core: prefill accepted the prompt (sampled run)");
    std::vector<uint16_t> sampled;
    for (size_t i = 0; i < g.sampled_tokens.size(); ++i) {
        const LlmStep s = llm_run_token(c.ctx);
        if (s != LlmStep::Token) break;
        sampled.push_back(llm_token(c.ctx));
    }
    CK(same_ids(g.sampled_tokens, sampled.data(), int32_t(sampled.size()), "core/top-p"),
       "core: temperature+top-p tokens match the golden bit-for-bit");
}

// --- 2. the resumable state machine ------------------------------------------------------------

void test_state_machine(const std::vector<uint8_t>& blob, const Golden& g) {
    Core c(blob);
    if (!c.ok) return;

    CK(llm_stages_per_token(c.ctx) == c.ctx.n_layers * 2 + 1,
       "machine: stages per token = 2 per layer + the head");

    uint16_t ids[256];
    const int32_t n = llm_encode(c.ctx, g.prompt.c_str(), true, ids, 256);
    if (n <= 0) return;

    // Slicing a token across many advance() calls must produce EXACTLY what running it in one go
    // does — that equivalence is what lets the GBA front end interleave inference with the frame
    // loop without changing a single output token.
    c.ctx.sampler.greedy = true;
    llm_reset(c.ctx);
    llm_prefill(c.ctx, ids, n);
    std::vector<uint16_t> sliced;
    int32_t working = 0;
    int32_t run = 0, gap = -1;         // advance() calls between the 1st and 2nd generated token
    while (sliced.size() < g.fixed_tokens.size()) {
        const LlmStep s = llm_advance(c.ctx);
        if (s == LlmStep::Working) {
            ++working;
            ++run;
            continue;
        }
        if (s == LlmStep::Token) {
            sliced.push_back(llm_token(c.ctx));
            if (sliced.size() == 2 && gap < 0) gap = run + 1;   // + the call that returned Token
            run = 0;
            continue;
        }
        break;
    }
    CK(same_ids(g.fixed_tokens, sliced.data(), int32_t(sliced.size()), "machine/sliced"),
       "machine: one stage per call gives the same tokens as running the token whole");
    CK(working > 0, "machine: advance() actually returned Working (work IS sliced)");
    // A generated token is a fixed amount of sliced work: one call per transformer stage.
    CK(gap == llm_stages_per_token(c.ctx),
       "machine: a generated token takes exactly stages_per_token calls");

    // An un-prefilled context is Idle, not a crash or a garbage token.
    llm_reset(c.ctx);
    CK(llm_advance(c.ctx) == LlmStep::Idle, "machine: Idle before any prefill");
    CK(c.ctx.pos == 0 && c.ctx.tokens_out == 0, "machine: reset rewinds position and counters");

    // Running to the end of the window reports Full instead of writing past the KV cache.
    llm_reset(c.ctx);
    llm_prefill(c.ctx, ids, n);
    int32_t produced = 0;
    LlmStep last = LlmStep::Idle;
    for (int32_t guard = 0; guard < 100000; ++guard) {
        last = llm_run_token(c.ctx);
        if (last != LlmStep::Token) break;
        ++produced;
    }
    CK(last == LlmStep::Full, "machine: the context boundary reports Full");
    CK(c.ctx.pos == c.ctx.max_seq, "machine: stopped exactly at max_seq");
    // The LAST prompt token is the one that produces the first output, so n prompt tokens plus k
    // generated tokens occupy n - 1 + k slots.
    CK(produced == c.ctx.max_seq - n + 1, "machine: produced every remaining slot and no more");

    // A prompt that cannot fit is refused up front.
    llm_reset(c.ctx);
    std::vector<uint16_t> huge(size_t(c.ctx.max_seq) + 1, uint16_t(3));
    CK(!llm_prefill(c.ctx, huge.data(), int32_t(huge.size())), "machine: an oversized prompt is refused");
    CK(!llm_prefill(c.ctx, ids, 0), "machine: an empty prompt is refused");
}

// --- 3. a different KV budget must not change the tokens ---------------------------------------

void test_kv_budget_independence(const std::vector<uint8_t>& blob, const Golden& g) {
    // max_seq is derived from the budget, so a smaller cache means a shorter window — but for a
    // generation that fits in both, the arithmetic must be identical. This is the check that the
    // cache LAYOUT (indexed by max_seq) is not leaking into the results.
    // Slots needed: the last prompt token produces the first output, so n - 1 + k.
    const int32_t want = int32_t(g.prompt_tokens.size() + g.fixed_tokens.size()) - 1;
    Core big(blob, 1u << 16);
    Core tight(blob, uint32_t(2 * 2 * 8 * 2) * uint32_t(want));
    if (!big.ok || !tight.ok) { CK(false, "kv: both budgets initialise"); return; }
    CK(tight.ctx.max_seq < big.ctx.max_seq, "kv: a smaller budget derives a shorter window");
    CK(tight.ctx.max_seq >= want, "kv: the tight window still fits this generation");

    uint16_t ids[256];
    const int32_t n = llm_encode(big.ctx, g.prompt.c_str(), true, ids, 256);
    std::vector<uint16_t> a, b;
    for (Core* c : { &big, &tight }) {
        c->ctx.sampler.greedy = true;
        llm_reset(c->ctx);
        llm_prefill(c->ctx, ids, n);
        std::vector<uint16_t>& out = (c == &big) ? a : b;
        for (size_t i = 0; i < g.fixed_tokens.size(); ++i) {
            if (llm_run_token(c->ctx) != LlmStep::Token) break;
            out.push_back(llm_token(c->ctx));
        }
    }
    CK(a == b, "kv: the same tokens under two different cache sizes");
    CK(same_ids(g.fixed_tokens, b.data(), int32_t(b.size()), "kv/tight"),
       "kv: the tight-budget run still matches the golden");
}

// --- 4. the whole example under the real App loop ------------------------------------------------

Config make_cfg() {
    Config cfg = Config::from_defaults();
    cfg.sim_hz = 60;
    cfg.title = "tinyllm";
    cfg.width = 240; cfg.height = 160;
    cfg.total_ram = 8u << 20; cfg.frame_scratch = 64u << 10; cfg.max_entities = 16;
    return cfg;
}

struct LoopGame final : TinyLlmGame {
    uint32_t painted = 0;      // console cells that are not the empty tile, at shutdown
    void on_stop(App& a) override {
        for (int32_t i = 0; i < kCells; ++i) if (cells && cells[i]) ++painted;
        TinyLlmGame::on_stop(a);
    }
};

void test_app_loop(const std::vector<uint8_t>& blob, const Golden& g) {
    if (!bake_tinyllm_assets(kBundle, 2, nullptr, /*quiet=*/true)) {
        CK(false, "app: bundle bake");
        return;
    }

    // The app runs its own baked-in prompt (kPrompts[0]), not the golden's, so derive the
    // expected context occupancy from that prompt's real token count.
    int32_t app_prompt_tokens = 0;
    {
        Core c(blob, 1u << 16);
        uint16_t ids[256];
        app_prompt_tokens = c.ok ? llm_encode(c.ctx, kPrompts[0], true, ids, 256) : 0;
    }
    CK(app_prompt_tokens > 0, "app: the baked-in prompt encodes");

    phx_null_set_step_ns(1000000000ull / 60);
    phx_null_set_max_frames(400);

    LoopGame game;
    game.bundle = kBundle;
    game.kv_budget_bytes = 1u << 16;
    game.stages_per_frame = 3;                // deliberately mid-token, to exercise resumption
    game.sampler.greedy = true;
    game.max_tokens = uint32_t(g.fixed_tokens.size());
    game.quit_when_done = true;
    game.verify_checksum = true;              // the bundle is small here; check the CRC too

    App app(make_cfg());
    app.run(&game);

    std::printf("    app tokens=%u ctx=%d/%d painted=%u len=%u\n",
                game.tokens_generated(), game.context_used(), game.context_max(),
                game.painted, game.transcript_len());

    CK(game.model_ok, "app: the model loaded zero-copy from the mounted bundle");
    CK(game.tokens_generated() == uint32_t(g.fixed_tokens.size()),
       "app: generated the requested number of tokens across frames");
    CK(game.finished, "app: reached its stopping condition");
    CK(game.context_used() == app_prompt_tokens - 1 + int32_t(game.tokens_generated()),
       "app: context advanced by prompt + generated tokens");
    CK(game.painted > 40, "app: the console tilemap was actually painted");
    CK(game.transcript_len() > 0, "app: produced a transcript");

    // The prompt is echoed first, so the transcript must start with it.
    CK(std::strncmp(game.transcript(), kPrompts[0], std::strlen(kPrompts[0])) == 0,
       "app: the transcript opens with the echoed prompt");
}

int run() {
    const Golden g = load_golden(kGolden);
    if (!g.loaded) {
        std::printf("TINYLLM FAIL (could not read %s)\n", kGolden);
        return 1;
    }

    const std::vector<uint8_t> blob = build::build_fixture_blob();
    std::printf("    fixture %zu bytes crc %u (golden: %u bytes crc %u)\n",
                blob.size(), unsigned(build::crc32(blob.data() + 52, blob.size() - 52)),
                g.blob_size, g.blob_crc);
    // Identity first: if the generated fixture ever changes, say so plainly instead of letting
    // it surface as an inexplicable token mismatch.
    CK(blob.size() == g.blob_size, "fixture: size matches the golden's model");
    CK(build::crc32(blob.data() + 52, blob.size() - 52) == g.blob_crc,
       "fixture: CRC32 matches the golden's model");
    if (g_fail) {
        std::printf("    (regenerate with: make tinyllm-fixture)\n");
        return 1;
    }

    test_core(blob, g);
    test_state_machine(blob, g);
    test_kv_budget_independence(blob, g);
    test_app_loop(blob, g);
    return 0;
}

} // namespace

int main() {
    if (run() != 0) return 1;
    if (g_fail) { std::printf("TINYLLM FAIL (%d checks)\n", g_fail); return 1; }
    std::printf("TINYLLM PASS\n");
    return 0;
}
