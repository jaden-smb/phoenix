// examples/tinyllm/src/llm.h — the public API of the fixed-point transformer inference core.
//
// The whole point of this file: ONE portable, float-free, allocation-free implementation that
// produces byte-identical tokens on a desktop and on a Game Boy Advance. It knows nothing about
// platforms, rendering, or the frame loop — its only engine dependency is `phx::ArenaAllocator`
// (all working memory is carved from a caller-supplied arena at init; nothing allocates after).
//
// Incrementality: a token takes tens of milliseconds on a 16 MHz ARM7TDMI, which would stall the
// frame loop. So generation is a RESUMABLE STATE MACHINE — llm_advance() performs one bounded
// STAGE (one transformer sub-block) and returns; the caller pumps it from on_fixed_update() and
// the UI stays live at 60 fps while a token is in flight. llm_run_token() is the blocking
// convenience wrapper the headless runner and the test suites use.
//
//   llm_init(ctx, blob, size, arena, kv_budget)   // validate + resolve + carve, once
//   llm_prefill(ctx, ids, n)                      // queue the prompt
//   while (llm_advance(ctx) != LlmStep::Token) {} // bounded work per call
//   llm_token(ctx)                                // the sampled id; it is fed back automatically
#ifndef TINYLLM_LLM_H
#define TINYLLM_LLM_H

#include "llm_format.h"
#include "fixedmath.h"

#include "phx/memory/allocators.h"

namespace tinyllm {

// What llm_advance() did.
enum class LlmStep : uint8_t {
    Idle,     // nothing queued — call llm_prefill() first
    Working,  // a stage of the current token completed; call again
    Token,    // a token was produced; read it with llm_token()
    Full,     // the context window is exhausted; call llm_reset()
};

// Sampling is entirely fixed-point and driven by a seeded xorshift, so a (prompt, seed) pair
// reproduces exactly — on either scalar tier, on any target.
struct LlmSampler {
    bool     greedy          = true;      // argmax; ignores temperature/top_p
    int32_t  temperature_q16 = kQ16One;   // 1.0
    int32_t  topp_q16        = 58982;     // 0.9
    uint32_t seed            = 0x2545F491u;
};

// A quantized weight matrix resolved to pointers into the blob (i.e. into cartridge ROM).
// Rows are `stride` int8 wide (padded up to a multiple of group_size so the kernel never has a
// ragged tail) and strictly contiguous, which is what keeps the GBA cart bus in its fast
// sequential-access mode; `cols` is the logical width the caller actually feeds.
struct LlmMat {
    const int8_t*  w      = nullptr;
    const int32_t* s      = nullptr;   // rows*groups Q30 scales, row-major, read sequentially too
    int32_t        rows   = 0;
    int32_t        cols   = 0;
    int32_t        stride = 0;
    int32_t        groups = 0;         // stride / group_size, precomputed (no divide on device)
};

struct LlmLayer {
    const int32_t* rms_att = nullptr;  // Q16.16 gains
    const int32_t* rms_ffn = nullptr;
    LlmMat wq, wk, wv, wo, w1, w2, w3;
};

// Everything the core needs. Large but POD: it is created inside the arena (llm_init), never on
// a GBA stack. Public fields are readable by front ends (the status line wants pos/max_seq).
struct LlmContext {
    const LlmHeader* hdr = nullptr;

    // --- config, widened to int32 so hot loops index without repeated zero-extends ---
    int32_t dim = 0, hidden = 0, n_layers = 0, n_heads = 0, n_kv_heads = 0;
    int32_t head_size = 0, kv_dim = 0, vocab = 0, group = 0;
    int32_t kv_mul = 0;          // n_heads / n_kv_heads (grouped-query fan-out)
    int32_t max_seq = 0;         // derived from the KV budget at init, NOT hardcoded
    int32_t kv_shift = 0;
    int32_t rms_eps_q16 = 0;
    int32_t inv_dim_q16 = 0;     // 1/dim, precomputed once (RMSNorm's mean)
    int32_t attn_scale_q16 = 0;  // 1/sqrt(head_size), precomputed once

    // --- weights (all pointers into the immutable blob) ---
    LlmLayer*      layers    = nullptr;
    LlmMat         tok{};        // the embedding table, dequantized row-wise on lookup
    LlmMat         cls{};        // the output head (aliases `tok` when the checkpoint shares it)
    const int32_t* rms_final = nullptr;
    const int16_t* rope_cos  = nullptr;
    const int16_t* rope_sin  = nullptr;
    const int32_t* exp_lut   = nullptr;
    const int32_t* silu_lut  = nullptr;

    // --- tokenizer (also in the blob) ---
    const uint8_t*  tok_bytes  = nullptr;
    const uint32_t* tok_index  = nullptr;   // vocab+1 offsets
    const uint16_t* tok_sorted = nullptr;   // ids ordered by piece bytes -> binary search
    const int32_t*  tok_score  = nullptr;   // Q16.16 merge scores

    // --- working memory (arena) ---
    int32_t* x       = nullptr;   // [dim]     residual stream, Q16.16
    int32_t* xb      = nullptr;   // [dim]
    int32_t* xb2     = nullptr;   // [dim]
    int32_t* hb      = nullptr;   // [hidden]
    int32_t* hb2     = nullptr;   // [hidden]
    int32_t* q       = nullptr;   // [dim]
    int32_t* kbuf    = nullptr;   // [kv_dim]  this token's K, pre-RoPE then pre-cache
    int32_t* vbuf    = nullptr;   // [kv_dim]
    int32_t* att     = nullptr;   // [max_seq]
    int32_t* logits  = nullptr;   // [vocab]
    int8_t*  xq      = nullptr;   // [max(dim,hidden) padded to group] quantized activation
    int16_t* kv      = nullptr;   // the KV cache, see llm.cpp for the layout
    uint16_t* order  = nullptr;   // [vocab] top-p candidate indices
    uint16_t* prompt = nullptr;   // [max_seq] queued prompt tokens
    char*     scratch = nullptr;  // [2*max_token_len+2] tokenizer merge buffer

    // --- state machine ---
    int32_t  pos        = 0;      // tokens already in the context (== next cache slot)
    int32_t  stage      = 0;      // 0..2*n_layers inclusive; see llm_stages_per_token()
    uint16_t cur_token  = 0;
    uint16_t out_token  = 0;
    uint16_t prev_token = 0;      // for decode()'s leading-space rule
    int32_t  prompt_len = 0;
    int32_t  prompt_i   = 0;
    bool     active     = false;  // a token is being processed
    bool     want_logits = false; // false while prefilling all but the last prompt token
    LlmSampler sampler{};
    uint32_t   rng = 1;

    // --- observability (the status line and the test suites read these) ---
    uint32_t tokens_out = 0;
    uint32_t arena_bytes = 0;
};

// How many bytes llm_init() will take from the arena for THIS header and KV budget. Front ends
// call it before sizing `Config::total_ram`; the README's budget table is generated from it.
uint32_t llm_required_arena_bytes(const LlmHeader*, uint32_t kv_budget_bytes);

// The usable context length for this header under `kv_budget_bytes` of KV cache — min(seq_len,
// budget / per-token bytes). Derived, never hardcoded, so a bigger checkpoint just gets a
// shorter window instead of overflowing EWRAM.
int32_t llm_max_seq(const LlmHeader*, uint32_t kv_budget_bytes);

// Validate the blob, resolve every tensor, and carve the working set. Returns false (and leaves
// ctx.hdr null) on a malformed blob, a missing required tensor, a config the core cannot run, or
// an arena too small. Never allocates outside the arena and never throws.
bool llm_init(LlmContext&, const void* blob, uint32_t size,
              phx::ArenaAllocator&, uint32_t kv_budget_bytes);

// Drop the conversation: rewind to position 0. The KV cache is not cleared (positions past `pos`
// are never read), so this is O(1) — it is the B button.
void llm_reset(LlmContext&);

// Queue `n` prompt tokens. Returns false if n is 0 or would not fit the window. The last prompt
// token is the one that produces the first generated token.
bool llm_prefill(LlmContext&, const uint16_t* ids, int32_t n);

// Perform ONE bounded stage. See LlmStep. This is the only entry point that does real work.
LlmStep llm_advance(LlmContext&);

// Blocking convenience: advance until a token pops out (or Idle/Full). Used by the headless
// runner and both test suites, where frame pacing is irrelevant.
LlmStep llm_run_token(LlmContext&);

inline uint16_t llm_token(const LlmContext& c) { return c.out_token; }
// Stages per token: two per layer (attention, feed-forward) plus one for the final norm, the
// output head, and sampling. Front ends use it to draw a progress indicator.
inline int32_t llm_stages_per_token(const LlmContext& c) { return c.n_layers * 2 + 1; }
inline bool    llm_at_eos(const LlmContext& c) { return c.hdr && c.out_token == c.hdr->eos_id; }

// --- tokenizer ------------------------------------------------------------------------------
// BPE encode. Allocation-free: it works in `out` and `ctx.scratch`, and every vocab lookup is a
// binary search over the blob's sorted-id table. Returns the token count, or -1 if `max_out` is
// too small. Mirrors llama2.c: an optional BOS, then a dummy " " prefix, then greedy
// highest-score pair merging.
int32_t llm_encode(const LlmContext&, const char* text, bool add_bos,
                   uint16_t* out, int32_t max_out);

// Decode one token to bytes (NUL-terminated), applying llama2.c's two display rules: strip the
// leading space of the first piece after BOS, and turn a `<0xXX>` byte token into that raw byte.
// Returns the number of bytes written (excluding the NUL).
int32_t llm_decode(const LlmContext&, uint16_t prev, uint16_t token, char* out, int32_t cap);

// The raw piece for a token id, or nullptr. Points into the blob; NOT NUL-terminated, so `len`
// is an out-parameter.
const char* llm_piece(const LlmContext&, uint16_t token, uint32_t* len);

} // namespace tinyllm
#endif // TINYLLM_LLM_H
