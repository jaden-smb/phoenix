// examples/tinyllm/src/llm.cpp — the fixed-point decoder-only transformer.
//
// Portable code: no platform headers, no `#ifdef PLATFORM`, no float, no STL, no heap, no
// exceptions. The ONE target-specific thing it does is PHX_HOT_CODE (phx/core/hot.h) on the four
// measured hot spots, which is a no-op everywhere except a real GBA build — that macro is the
// single isolated place PHX_GBA_HW is honoured, exactly as the engine's own hot paths do it.
//
// Numeric contract (mirrored bit-for-bit by tools/export_model.py, which generates the golden
// tokens the suite asserts against):
//   * activations       Q16.16 int32
//   * weights           int8, per-group (group_size) Q30 scales, rows padded to a whole group
//   * matvec            int8 x int8 -> int32 group sums -> int64 Q16.16 accumulator -> Q30 rescale
//   * KV cache          int16 at Q(kv_shift), EWRAM
//   * exp / SiLU        ROM lookup tables, power-of-two step, linear interpolation
//   * reciprocals       Newton-Raphson (fixedmath.h) — ARM7TDMI has no divide instruction
//
// Weight streaming: every matvec walks `w` and `s` with a single monotonically increasing pointer
// each, so the GBA cartridge bus stays in its fast sequential-access mode. Nothing is ever copied
// out of ROM.
#include "llm.h"
#include "fixedmath.h"

#include "phx/core/hot.h"

namespace tinyllm {
namespace {

constexpr uint32_t kAlign = 16;   // ArenaAllocator's default alignment

inline uint32_t align_up_u32(uint32_t v, uint32_t a) { return (v + (a - 1)) & ~(a - 1); }

// Counts bytes when `arena` is null, otherwise carves them. One code path sizes the arena and
// fills it, so llm_required_arena_bytes() can never drift from llm_init().
struct Carver {
    phx::ArenaAllocator* arena = nullptr;
    uint32_t total = kAlign;      // slack for the arena's own starting misalignment
    bool     ok    = true;

    void* take(uint32_t bytes) {
        total = align_up_u32(total, kAlign) + bytes;
        if (!arena) return nullptr;
        void* p = arena->alloc(bytes, kAlign);
        if (!p) ok = false;
        return p;
    }
    template <class T> T* array(uint32_t n) { return static_cast<T*>(take(uint32_t(sizeof(T)) * n)); }
};

// --- blob plumbing -------------------------------------------------------------------------------

inline const uint8_t* blob_at(const LlmHeader* h, uint32_t off) {
    return reinterpret_cast<const uint8_t*>(h) + off;
}

// Resolve a directory entry into an LlmMat (quantized matrix). Returns false when absent.
bool bind_mat(const LlmHeader* h, uint8_t kind, uint16_t layer, LlmMat& out) {
    const LlmTensor* t = llm_find(h, kind, layer);
    if (!t || t->dtype != kDTypeQ8 || t->scale_off == 0) return false;
    out.w      = reinterpret_cast<const int8_t*>(blob_at(h, t->data_off));
    out.s      = reinterpret_cast<const int32_t*>(blob_at(h, t->scale_off));
    out.rows   = int32_t(t->rows);
    out.cols   = int32_t(t->cols);
    out.stride = int32_t(t->stride);
    out.groups = int32_t(t->stride / h->group_size);
    return true;
}

template <class T>
const T* bind_raw(const LlmHeader* h, uint8_t kind, uint8_t dtype, uint16_t layer = kNoLayer) {
    const LlmTensor* t = llm_find(h, kind, layer);
    if (!t || t->dtype != dtype) return nullptr;
    return reinterpret_cast<const T*>(blob_at(h, t->data_off));
}

// --- KV cache layout ------------------------------------------------------------------------------
// [layer][K|V][kv_head][t][head_size] — chosen so BOTH attention inner loops (scores over t, and
// the value-weighted sum over t) walk memory strictly forward within a head. The per-token write
// is the only strided access, and it touches just kv_dim entries.
inline int32_t kv_head_offset(const LlmContext& c, int32_t layer, int32_t which, int32_t kv_head) {
    return (((layer * 2 + which) * c.n_kv_heads) + kv_head) * c.max_seq * c.head_size;
}

// --- kernels ----------------------------------------------------------------------------------

// RMSNorm: out = x * gain / sqrt(mean(x^2) + eps). The sum of squares is Q32.32 in an int64, so a
// dim of any realistic size cannot overflow it.
PHX_HOT_CODE
void rmsnorm(int32_t* out, const int32_t* x, const int32_t* gain,
             int32_t n, int32_t inv_n_q16, int32_t eps_q16) {
    int64_t ss = 0;
    for (int32_t i = 0; i < n; ++i) ss += int64_t(x[i]) * int64_t(x[i]);
    const int32_t mean_q16 = q_sat32(((ss >> 16) * int64_t(inv_n_q16)) >> 16);
    const int32_t r        = q_rsqrt(mean_q16 + eps_q16);
    for (int32_t i = 0; i < n; ++i) out[i] = q_mul(gain[i], q_mul(r, x[i]));
}

// Four int8 weights per aligned 32-bit load.
//
// This is the single most important line of code for on-device throughput. The GBA's cartridge
// bus is 16 bits wide and its prefetch buffer only serves INSTRUCTION fetches — a data read from
// ROM always pays the wait states. Loading one byte and loading one halfword therefore cost the
// same, so pulling four weights per 32-bit load (two sequential halfword accesses) halves the
// bus traffic of four byte loads. Measured A/B on mGBA with the 260K model, one whole token per
// frame: 413 ms/token -> 381 ms/token, i.e. 2.42 -> 2.62 tokens/sec (+8%). Less than the bus
// arithmetic alone suggests, because the unpack (ARMv4T has no SXTB, so each byte costs a
// shift pair) eats part of what the loads save.
//
// Alignment is guaranteed by the format, not assumed: llm_validate() requires 4-byte-aligned
// tensor payloads and a `stride` that is a whole multiple of `group_size` (itself a power of two
// >= 4), so every row and every group starts on a word boundary. `may_alias` is what lets the
// compiler emit a single LDR instead of the unaligned-safe byte sequence a plain memcpy would
// get on ARMv4T; `group % 4 == 0` is likewise a format invariant.
//
// Endianness does not matter here even though the unpack order does: `w` and `xq` are unpacked
// with the SAME expression, so a big-endian host pairs w[j+3] with xq[j+3] instead of w[j+0] with
// xq[j+0]. The dot product is the same set of products summed in a different order, and integer
// addition is associative — the result is identical, which is what the determinism gate needs.
#if defined(__GNUC__) || defined(__clang__)
typedef uint32_t LlmWord __attribute__((may_alias, aligned(4)));
#define TINYLLM_WORD_LOADS 1
#endif

inline int32_t sbyte(uint32_t v) { return int32_t(int8_t(uint8_t(v & 0xFFu))); }

// The dominant kernel: out[r] = sum_g (sum_j w[r][g][j] * xq[g][j]) * ws[r][g] * xs.
// `w` and `s` advance only forwards — that sequential walk is what makes the cartridge bus fast.
PHX_HOT_CODE
void matvec_q8(int32_t* out, const int8_t* w, const int32_t* s, const int8_t* xq,
               int32_t xs_q30, int32_t rows, int32_t groups, int32_t group) {
    for (int32_t r = 0; r < rows; ++r) {
        int64_t acc = 0;
        const int8_t* xp = xq;
        for (int32_t g = 0; g < groups; ++g) {
            int32_t ival = 0;
#if defined(TINYLLM_WORD_LOADS)
            const LlmWord* wp4 = reinterpret_cast<const LlmWord*>(w);
            const LlmWord* xp4 = reinterpret_cast<const LlmWord*>(xp);
            for (int32_t j = 0; j < group; j += 4) {
                const uint32_t wv = *wp4++;
                const uint32_t xv = *xp4++;
                ival += sbyte(wv)       * sbyte(xv);
                ival += sbyte(wv >>  8) * sbyte(xv >>  8);
                ival += sbyte(wv >> 16) * sbyte(xv >> 16);
                ival += sbyte(wv >> 24) * sbyte(xv >> 24);
            }
#else
            for (int32_t j = 0; j < group; j += 4) {
                ival += int32_t(w[j + 0]) * int32_t(xp[j + 0]);
                ival += int32_t(w[j + 1]) * int32_t(xp[j + 1]);
                ival += int32_t(w[j + 2]) * int32_t(xp[j + 2]);
                ival += int32_t(w[j + 3]) * int32_t(xp[j + 3]);
            }
#endif
            w  += group;
            xp += group;
            acc += (int64_t(ival) * int64_t(*s++)) >> 14;   // int8 product * Q30 scale -> Q16.16
        }
        out[r] = q_scale_q30(acc, xs_q30);
    }
}

// A PHX_HOT_CODE shell around the (inline, header-side) quantizer so it lands in IWRAM too.
PHX_HOT_CODE
int32_t quantize_row(const int32_t* x, int32_t n, int32_t stride, int8_t* out) {
    return q_quantize(x, n, stride, out).scale_q30;
}

// Scaled dot-product attention for every query head of one layer, reading the KV cache in place.
// `pos` is the current token's slot; scores run over t in [0, pos].
PHX_HOT_CODE
void attention(LlmContext& c, int32_t layer, int32_t pos) {
    const int32_t hs = c.head_size;
    const int32_t kv_sh = c.kv_shift;
    const int32_t nt = pos + 1;

    for (int32_t kvh = 0; kvh < c.n_kv_heads; ++kvh) {
        const int16_t* kbase = c.kv + kv_head_offset(c, layer, 0, kvh);
        const int16_t* vbase = c.kv + kv_head_offset(c, layer, 1, kvh);

        for (int32_t sub = 0; sub < c.kv_mul; ++sub) {
            const int32_t  h  = kvh * c.kv_mul + sub;
            const int32_t* qh = c.q + h * hs;
            int32_t*       xo = c.xb + h * hs;

            // 1. scores. int64 accumulate (one SMLAL per term on ARM), Q16.16 out.
            int32_t maxs = -2147483647 - 1;
            for (int32_t t = 0; t < nt; ++t) {
                const int16_t* kp = kbase + t * hs;
                int64_t dot = 0;
                for (int32_t i = 0; i < hs; ++i) dot += int64_t(qh[i]) * int64_t(kp[i]);
                const int32_t sc = q_mul(q_sat32(dot >> kv_sh), c.attn_scale_q16);
                c.att[t] = sc;
                if (sc > maxs) maxs = sc;
            }

            // 2. softmax through the ROM exp table, then renormalize into Q15 so step 3 can
            //    accumulate in int32 (sum of Q15 weights <= 2^15, times an int16 value, stays
            //    inside int31 with a bit to spare).
            int64_t sum = 0;
            for (int32_t t = 0; t < nt; ++t) {
                const int64_t d = int64_t(c.att[t]) - int64_t(maxs);
                const int32_t e = (d <= -(16 << 16)) ? 0 : q_exp_neg(c.exp_lut, int32_t(d));
                c.att[t] = e;
                sum += e;
            }
            const int32_t inv = q_div(kQ16One, q_sat32(sum));
            for (int32_t t = 0; t < nt; ++t) c.att[t] = q_mul(c.att[t], inv) >> 1;   // Q15

            // 3. value-weighted sum, walking V forward. int32 accumulators live in xo itself.
            for (int32_t i = 0; i < hs; ++i) xo[i] = 0;
            for (int32_t t = 0; t < nt; ++t) {
                const int32_t  a  = c.att[t];
                if (a == 0) continue;
                const int16_t* vp = vbase + t * hs;
                for (int32_t i = 0; i < hs; ++i) xo[i] += a * int32_t(vp[i]);
            }
            // Q(15+kv_shift) -> Q16.16
            for (int32_t i = 0; i < hs; ++i) xo[i] >>= (kv_sh - 1);
        }
    }
}

// --- forward-pass stages -----------------------------------------------------------------------

// x = dequantized embedding row for `token`.
void embed(LlmContext& c, uint16_t token) {
    const LlmMat& m = c.tok;
    const int8_t*  w = m.w + int32_t(token) * m.stride;
    const int32_t* s = m.s + int32_t(token) * m.groups;
    int32_t j = 0;
    for (int32_t g = 0; g < m.groups; ++g) {
        const int32_t sc = *s++;
        for (int32_t e = 0; e < c.group && j < c.dim; ++e, ++j)
            c.x[j] = q_sat32((int64_t(w[j]) * int64_t(sc)) >> 14);   // int8 * Q30 -> Q16.16
    }
}

// Rotary position embedding, read from the precomputed Q15 table (no sin/cos on device). Pairs
// are adjacent (2j, 2j+1) within a head and share frequency index j — llama2.c's convention.
void rope(LlmContext& c, int32_t pos) {
    const int32_t  half = c.head_size >> 1;
    const int16_t* cs   = c.rope_cos + pos * half;
    const int16_t* sn   = c.rope_sin + pos * half;

    for (int32_t h = 0; h < c.n_heads; ++h) {
        int32_t* p = c.q + h * c.head_size;
        for (int32_t j = 0; j < half; ++j) {
            const int64_t v0 = p[2 * j], v1 = p[2 * j + 1];
            const int64_t fc = cs[j], fs = sn[j];
            p[2 * j]     = q_sat32((v0 * fc - v1 * fs) >> 15);
            p[2 * j + 1] = q_sat32((v0 * fs + v1 * fc) >> 15);
        }
    }
    for (int32_t h = 0; h < c.n_kv_heads; ++h) {
        int32_t* p = c.kbuf + h * c.head_size;
        for (int32_t j = 0; j < half; ++j) {
            const int64_t v0 = p[2 * j], v1 = p[2 * j + 1];
            const int64_t fc = cs[j], fs = sn[j];
            p[2 * j]     = q_sat32((v0 * fc - v1 * fs) >> 15);
            p[2 * j + 1] = q_sat32((v0 * fs + v1 * fc) >> 15);
        }
    }
}

inline int16_t kv_pack(int32_t v_q16, int32_t kv_shift) {
    const int32_t v = v_q16 >> (16 - kv_shift);
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return int16_t(v);
}

void attention_block(LlmContext& c, int32_t l, int32_t pos) {
    const LlmLayer& L = c.layers[l];

    rmsnorm(c.xb, c.x, L.rms_att, c.dim, c.inv_dim_q16, c.rms_eps_q16);
    const int32_t xs = quantize_row(c.xb, c.dim, L.wq.stride, c.xq);

    matvec_q8(c.q,    L.wq.w, L.wq.s, c.xq, xs, L.wq.rows, L.wq.groups, c.group);
    matvec_q8(c.kbuf, L.wk.w, L.wk.s, c.xq, xs, L.wk.rows, L.wk.groups, c.group);
    matvec_q8(c.vbuf, L.wv.w, L.wv.s, c.xq, xs, L.wv.rows, L.wv.groups, c.group);

    rope(c, pos);

    // Publish this token's K/V into the cache (the only strided write in the whole forward pass).
    for (int32_t kvh = 0; kvh < c.n_kv_heads; ++kvh) {
        int16_t* kp = c.kv + kv_head_offset(c, l, 0, kvh) + pos * c.head_size;
        int16_t* vp = c.kv + kv_head_offset(c, l, 1, kvh) + pos * c.head_size;
        const int32_t base = kvh * c.head_size;
        for (int32_t i = 0; i < c.head_size; ++i) {
            kp[i] = kv_pack(c.kbuf[base + i], c.kv_shift);
            vp[i] = kv_pack(c.vbuf[base + i], c.kv_shift);
        }
    }

    attention(c, l, pos);

    const int32_t xs2 = quantize_row(c.xb, c.dim, L.wo.stride, c.xq);
    matvec_q8(c.xb2, L.wo.w, L.wo.s, c.xq, xs2, L.wo.rows, L.wo.groups, c.group);
    for (int32_t i = 0; i < c.dim; ++i) c.x[i] = q_sat32(int64_t(c.x[i]) + int64_t(c.xb2[i]));
}

void ffn_block(LlmContext& c, int32_t l) {
    const LlmLayer& L = c.layers[l];

    rmsnorm(c.xb, c.x, L.rms_ffn, c.dim, c.inv_dim_q16, c.rms_eps_q16);
    const int32_t xs = quantize_row(c.xb, c.dim, L.w1.stride, c.xq);

    matvec_q8(c.hb,  L.w1.w, L.w1.s, c.xq, xs, L.w1.rows, L.w1.groups, c.group);
    matvec_q8(c.hb2, L.w3.w, L.w3.s, c.xq, xs, L.w3.rows, L.w3.groups, c.group);

    for (int32_t i = 0; i < c.hidden; ++i)
        c.hb[i] = q_mul(q_silu(c.silu_lut, c.hb[i]), c.hb2[i]);   // SwiGLU

    const int32_t xs2 = quantize_row(c.hb, c.hidden, L.w2.stride, c.xq);
    matvec_q8(c.xb, L.w2.w, L.w2.s, c.xq, xs2, L.w2.rows, L.w2.groups, c.group);
    for (int32_t i = 0; i < c.dim; ++i) c.x[i] = q_sat32(int64_t(c.x[i]) + int64_t(c.xb[i]));
}

void classify(LlmContext& c) {
    rmsnorm(c.xb, c.x, c.rms_final, c.dim, c.inv_dim_q16, c.rms_eps_q16);
    const int32_t xs = quantize_row(c.xb, c.dim, c.cls.stride, c.xq);
    matvec_q8(c.logits, c.cls.w, c.cls.s, c.xq, xs, c.cls.rows, c.cls.groups, c.group);
}

// --- sampling ------------------------------------------------------------------------------------

inline uint32_t rng_next(LlmContext& c) {
    uint32_t x = c.rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    c.rng = x ? x : 1u;
    return c.rng;
}

uint16_t sample(LlmContext& c) {
    const int32_t n = c.vocab;
    int32_t* p = c.logits;

    if (c.sampler.greedy || c.sampler.temperature_q16 <= 0) {
        int32_t best = 0;
        for (int32_t i = 1; i < n; ++i) if (p[i] > p[best]) best = i;
        return uint16_t(best);
    }

    if (c.sampler.temperature_q16 != kQ16One) {
        const int32_t inv = q_div(kQ16One, c.sampler.temperature_q16);
        for (int32_t i = 0; i < n; ++i) p[i] = q_mul(p[i], inv);
    }

    int32_t maxl = p[0];
    for (int32_t i = 1; i < n; ++i) if (p[i] > maxl) maxl = p[i];
    int64_t sum = 0;
    for (int32_t i = 0; i < n; ++i) {
        const int64_t d = int64_t(p[i]) - int64_t(maxl);
        p[i] = (d <= -(16 << 16)) ? 0 : q_exp_neg(c.exp_lut, int32_t(d));
        sum += p[i];
    }
    const int32_t norm = q_div(kQ16One, q_sat32(sum));
    for (int32_t i = 0; i < n; ++i) p[i] = q_mul(p[i], norm);      // probabilities, Q16.16

    // top-p (nucleus): llama2.c's algorithm in fixed point. The 1/(n-1) pre-filter usually leaves
    // a handful of candidates, so the partial selection sort below stays cheap; it also stops the
    // instant the cumulative mass reaches topp, so the worst case is O(n * kept), not O(n^2).
    const int32_t topp = c.sampler.topp_q16;
    const int32_t cutoff = (n > 1) ? q_div(kQ16One - topp, q_sat32(int64_t(n - 1) * 65536)) : 0;
    int32_t ncand = 0;
    for (int32_t i = 0; i < n; ++i) if (p[i] >= cutoff) c.order[ncand++] = uint16_t(i);
    if (ncand == 0) { for (int32_t i = 0; i < n; ++i) c.order[i] = uint16_t(i); ncand = n; }

    int64_t cum = 0;
    int32_t kept = 0;
    while (kept < ncand) {
        int32_t bi = kept;
        for (int32_t j = kept + 1; j < ncand; ++j)
            if (p[c.order[j]] > p[c.order[bi]]) bi = j;           // strict '>' keeps ties stable
        const uint16_t tmp = c.order[kept]; c.order[kept] = c.order[bi]; c.order[bi] = tmp;
        cum += p[c.order[kept]];
        ++kept;
        if (cum >= topp) break;
    }

    const uint32_t rv = rng_next(c);
    const int64_t  r  = int64_t((uint64_t(rv) * uint64_t(cum)) >> 32);   // uniform in [0, cum)
    int64_t acc = 0;
    for (int32_t j = 0; j < kept; ++j) {
        acc += p[c.order[j]];
        if (r < acc) return c.order[j];
    }
    return c.order[kept > 0 ? kept - 1 : 0];
}

// --- tokenizer helpers -----------------------------------------------------------------------

int cmp_bytes(const char* a, uint32_t alen, const char* b, uint32_t blen) {
    const uint32_t n = alen < blen ? alen : blen;
    for (uint32_t i = 0; i < n; ++i) {
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[i]);
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    if (alen == blen) return 0;
    return alen < blen ? -1 : 1;
}

// Binary search (lower_bound) over the blob's sorted-id table. Returns the LOWEST matching id so
// a vocab with duplicate pieces still resolves deterministically. -1 when absent.
int32_t vocab_lookup(const LlmContext& c, const char* s, uint32_t len) {
    int32_t lo = 0, hi = c.vocab;
    while (lo < hi) {
        const int32_t mid = lo + ((hi - lo) >> 1);
        uint32_t plen = 0;
        const char* piece = llm_piece(c, c.tok_sorted[mid], &plen);
        if (cmp_bytes(piece, plen, s, len) < 0) lo = mid + 1;
        else                                    hi = mid;
    }
    if (lo >= c.vocab) return -1;
    uint32_t plen = 0;
    const char* piece = llm_piece(c, c.tok_sorted[lo], &plen);
    return cmp_bytes(piece, plen, s, len) == 0 ? int32_t(c.tok_sorted[lo]) : -1;
}

inline int hexval(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

} // namespace

// --- format validation ---------------------------------------------------------------------------

const LlmTensor* llm_find(const LlmHeader* h, uint8_t kind, uint16_t layer) {
    const LlmTensor* d = llm_dir(h);
    for (uint32_t i = 0; i < h->tensor_count; ++i)
        if (d[i].kind == kind && d[i].layer == layer) return &d[i];
    return nullptr;
}

const LlmHeader* llm_validate(const void* data, uint32_t size) {
    if (!data || size < sizeof(LlmHeader)) return nullptr;
    // Everything is read in place as int32/uint32, and a misaligned 32-bit load on ARM7TDMI
    // silently rotates instead of faulting — so alignment is a correctness check, not a nicety.
    if ((reinterpret_cast<uintptr_t>(data) & 3u) != 0) return nullptr;

    const LlmHeader* h = static_cast<const LlmHeader*>(data);
    if (h->magic != kLlmMagic) return nullptr;
    if (h->version_major != kLlmVersionMajor) return nullptr;
    if (h->blob_size != size) return nullptr;

    if (!h->dim || !h->hidden_dim || !h->n_layers || !h->n_heads || !h->n_kv_heads ||
        !h->vocab_size || !h->seq_len || !h->head_size) return nullptr;
    if (uint32_t(h->head_size) * uint32_t(h->n_heads)    != uint32_t(h->dim))    return nullptr;
    if (uint32_t(h->head_size) * uint32_t(h->n_kv_heads) != uint32_t(h->kv_dim)) return nullptr;
    if (h->head_size & 1) return nullptr;                       // RoPE rotates adjacent pairs
    if (h->n_kv_heads > h->n_heads) return nullptr;
    if (h->n_heads % h->n_kv_heads) return nullptr;             // integral GQA fan-out
    if (h->bos_id >= h->vocab_size || h->eos_id >= h->vocab_size) return nullptr;

    const uint32_t g = h->group_size;
    if (g < 4 || g > 256 || (g & (g - 1))) return nullptr;      // power of two, >= the unroll
    if (h->kv_shift < 1 || h->kv_shift > 16) return nullptr;
    if (!h->max_token_len || h->max_token_len > 1024) return nullptr;

    if (h->tensor_count == 0 || h->tensor_count > 4096) return nullptr;
    if (h->dir_offset < sizeof(LlmHeader) || (h->dir_offset & 3u)) return nullptr;
    if (h->dir_offset > size) return nullptr;
    if ((size - h->dir_offset) / sizeof(LlmTensor) < h->tensor_count) return nullptr;

    const LlmTensor* d = llm_dir(h);
    for (uint32_t i = 0; i < h->tensor_count; ++i) {
        const LlmTensor& t = d[i];
        if (t.dtype > kDTypeU32) return nullptr;
        if (!t.rows || !t.cols || t.stride < t.cols) return nullptr;
        if (t.data_off & 3u) return nullptr;

        const uint64_t esz   = llm_dtype_size(t.dtype);
        const uint64_t bytes = uint64_t(t.rows) * uint64_t(t.stride) * esz;
        if (bytes != uint64_t(t.nbytes)) return nullptr;
        if (uint64_t(t.data_off) + bytes > uint64_t(size)) return nullptr;

        if (t.dtype == kDTypeQ8) {
            if (t.stride % g) return nullptr;
            if (t.scale_off == 0 || (t.scale_off & 3u)) return nullptr;
            const uint64_t sbytes = uint64_t(t.rows) * uint64_t(t.stride / g) * 4u;
            if (uint64_t(t.scale_off) + sbytes > uint64_t(size)) return nullptr;
        } else if (t.scale_off != 0) {
            return nullptr;                                     // scales belong to Q8 only
        }
    }
    return h;
}

// --- sizing / init ---------------------------------------------------------------------------

int32_t llm_max_seq(const LlmHeader* h, uint32_t kv_budget_bytes) {
    if (!h) return 0;
    // Init-time arithmetic (once per boot), so an integer divide here is fine — the no-divide
    // rule is about the per-token frame path.
    const uint32_t per_token = uint32_t(h->n_layers) * 2u * uint32_t(h->kv_dim) * 2u;
    if (per_token == 0) return 0;
    uint32_t n = kv_budget_bytes / per_token;
    if (n > h->seq_len) n = h->seq_len;
    return int32_t(n);
}

namespace {
// The single description of the working set — run with a null arena to size it, with a real one
// to carve it. `ms` is the derived max sequence length.
void carve(Carver& cv, LlmContext& c, const LlmHeader* h, int32_t ms) {
    const int32_t dim = h->dim, hidden = h->hidden_dim, kvd = h->kv_dim, vocab = h->vocab_size;
    const uint32_t g  = h->group_size;
    const uint32_t wide = (dim > hidden ? uint32_t(dim) : uint32_t(hidden));
    const uint32_t xq_bytes = align_up_u32(wide, g);

    c.layers = static_cast<LlmLayer*>(cv.take(uint32_t(sizeof(LlmLayer)) * h->n_layers));
    c.x      = cv.array<int32_t>(uint32_t(dim));
    c.xb     = cv.array<int32_t>(uint32_t(dim));
    c.xb2    = cv.array<int32_t>(uint32_t(dim));
    c.hb     = cv.array<int32_t>(uint32_t(hidden));
    c.hb2    = cv.array<int32_t>(uint32_t(hidden));
    c.q      = cv.array<int32_t>(uint32_t(dim));
    c.kbuf   = cv.array<int32_t>(uint32_t(kvd));
    c.vbuf   = cv.array<int32_t>(uint32_t(kvd));
    c.att    = cv.array<int32_t>(uint32_t(ms));
    c.logits = cv.array<int32_t>(uint32_t(vocab));
    c.order  = cv.array<uint16_t>(uint32_t(vocab));
    c.prompt = cv.array<uint16_t>(uint32_t(ms));
    c.xq     = cv.array<int8_t>(xq_bytes);
    c.scratch = cv.array<char>(uint32_t(h->max_token_len) * 2u + 2u);
    c.kv     = cv.array<int16_t>(uint32_t(h->n_layers) * 2u * uint32_t(ms) * uint32_t(kvd));
}
} // namespace

uint32_t llm_required_arena_bytes(const LlmHeader* h, uint32_t kv_budget_bytes) {
    if (!h) return 0;
    const int32_t ms = llm_max_seq(h, kv_budget_bytes);
    if (ms <= 0) return 0;
    LlmContext probe{};
    Carver cv{};
    carve(cv, probe, h, ms);
    return align_up_u32(cv.total, kAlign);
}

bool llm_init(LlmContext& c, const void* blob, uint32_t size,
              phx::ArenaAllocator& arena, uint32_t kv_budget_bytes) {
    c = LlmContext{};
    const LlmHeader* h = llm_validate(blob, size);
    if (!h) return false;

    const int32_t ms = llm_max_seq(h, kv_budget_bytes);
    if (ms < 2) return false;                       // a one-token window cannot generate anything

    c.dim = h->dim; c.hidden = h->hidden_dim; c.n_layers = h->n_layers;
    c.n_heads = h->n_heads; c.n_kv_heads = h->n_kv_heads;
    c.head_size = h->head_size; c.kv_dim = h->kv_dim; c.vocab = h->vocab_size;
    c.group = h->group_size; c.kv_mul = h->n_heads / h->n_kv_heads;
    c.max_seq = ms; c.kv_shift = h->kv_shift; c.rms_eps_q16 = h->rms_eps_q16;

    Carver cv{ &arena, kAlign, true };
    carve(cv, c, h, ms);
    if (!cv.ok) { c = LlmContext{}; return false; }
    c.arena_bytes = align_up_u32(cv.total, kAlign);

    // --- resolve the weights (pointers into ROM; nothing is copied) ---
    if (!bind_mat(h, kTensorTokEmb, kNoLayer, c.tok)) return false;
    if (h->flags & kLlmFlagSharedClassifier) c.cls = c.tok;
    else if (!bind_mat(h, kTensorWcls, kNoLayer, c.cls)) return false;

    c.rms_final = bind_raw<int32_t>(h, kTensorRmsFinal, kDTypeI32);
    c.rope_cos  = bind_raw<int16_t>(h, kTensorRopeCos,  kDTypeI16);
    c.rope_sin  = bind_raw<int16_t>(h, kTensorRopeSin,  kDTypeI16);
    c.exp_lut   = bind_raw<int32_t>(h, kTensorExpLut,   kDTypeI32);
    c.silu_lut  = bind_raw<int32_t>(h, kTensorSiluLut,  kDTypeI32);
    if (!c.rms_final || !c.rope_cos || !c.rope_sin || !c.exp_lut || !c.silu_lut) return false;

    c.tok_bytes  = bind_raw<uint8_t>(h,  kTensorTokBytes,  kDTypeU8);
    c.tok_index  = bind_raw<uint32_t>(h, kTensorTokIndex,  kDTypeU32);
    c.tok_sorted = bind_raw<uint16_t>(h, kTensorTokSorted, kDTypeU16);
    c.tok_score  = bind_raw<int32_t>(h,  kTensorTokScore,  kDTypeI32);
    if (!c.tok_bytes || !c.tok_index || !c.tok_sorted || !c.tok_score) return false;

    for (int32_t l = 0; l < c.n_layers; ++l) {
        LlmLayer& L = c.layers[l];
        L = LlmLayer{};
        const uint16_t li = uint16_t(l);
        L.rms_att = bind_raw<int32_t>(h, kTensorRmsAtt, kDTypeI32, li);
        L.rms_ffn = bind_raw<int32_t>(h, kTensorRmsFfn, kDTypeI32, li);
        if (!L.rms_att || !L.rms_ffn) return false;
        if (!bind_mat(h, kTensorWq, li, L.wq) || !bind_mat(h, kTensorWk, li, L.wk) ||
            !bind_mat(h, kTensorWv, li, L.wv) || !bind_mat(h, kTensorWo, li, L.wo) ||
            !bind_mat(h, kTensorW1, li, L.w1) || !bind_mat(h, kTensorW2, li, L.w2) ||
            !bind_mat(h, kTensorW3, li, L.w3)) return false;
        // Shapes the kernels assume; a mismatched checkpoint fails here, never mid-generation.
        if (L.wq.rows != c.dim    || L.wq.cols != c.dim    ||
            L.wk.rows != c.kv_dim || L.wk.cols != c.dim    ||
            L.wv.rows != c.kv_dim || L.wv.cols != c.dim    ||
            L.wo.rows != c.dim    || L.wo.cols != c.dim    ||
            L.w1.rows != c.hidden || L.w1.cols != c.dim    ||
            L.w3.rows != c.hidden || L.w3.cols != c.dim    ||
            L.w2.rows != c.dim    || L.w2.cols != c.hidden) return false;
    }
    if (c.tok.rows != c.vocab || c.tok.cols != c.dim) return false;
    if (c.cls.rows != c.vocab || c.cls.cols != c.dim) return false;

    // Constants that would otherwise be a divide on the frame path.
    c.inv_dim_q16    = q_div(kQ16One, q_sat32(int64_t(c.dim) * 65536));
    c.attn_scale_q16 = q_rsqrt(q_sat32(int64_t(c.head_size) * 65536));

    c.hdr = h;
    c.rng = c.sampler.seed ? c.sampler.seed : 1u;
    llm_reset(c);
    return true;
}

void llm_reset(LlmContext& c) {
    c.pos = 0; c.stage = 0;
    c.cur_token = 0; c.out_token = 0; c.prev_token = 0;
    c.prompt_len = 0; c.prompt_i = 0;
    c.active = false; c.want_logits = false;
    c.tokens_out = 0;
    c.rng = c.sampler.seed ? c.sampler.seed : 1u;
}

bool llm_prefill(LlmContext& c, const uint16_t* ids, int32_t n) {
    if (!c.hdr || !ids || n <= 0) return false;
    if (n > c.max_seq - c.pos) return false;
    for (int32_t i = 0; i < n; ++i) c.prompt[i] = ids[i];
    c.prompt_len  = n;
    c.prompt_i    = 0;
    c.cur_token   = ids[0];
    c.want_logits = (n == 1);
    c.active      = true;
    c.stage       = 0;
    return true;
}

LlmStep llm_advance(LlmContext& c) {
    if (!c.hdr || !c.active) return LlmStep::Idle;
    if (c.pos >= c.max_seq) return LlmStep::Full;

    if (c.stage == 0) embed(c, c.cur_token);

    if (c.stage < c.n_layers * 2) {
        const int32_t l = c.stage >> 1;
        if ((c.stage & 1) == 0) attention_block(c, l, c.pos);
        else                    ffn_block(c, l);
        ++c.stage;
        return LlmStep::Working;
    }

    // Final stage: this token's contribution to the cache is complete.
    c.stage = 0;
    ++c.pos;

    if (c.want_logits) {
        classify(c);
        c.prev_token = c.cur_token;
        c.out_token  = sample(c);
        c.cur_token  = c.out_token;      // autoregressive: the next advance() continues from it
        ++c.tokens_out;
        return LlmStep::Token;
    }

    ++c.prompt_i;
    c.prev_token  = c.cur_token;
    c.cur_token   = c.prompt[c.prompt_i];
    c.want_logits = (c.prompt_i + 1 >= c.prompt_len);
    return LlmStep::Working;
}

LlmStep llm_run_token(LlmContext& c) {
    for (;;) {
        const LlmStep s = llm_advance(c);
        if (s != LlmStep::Working) return s;
    }
}

// --- tokenizer -----------------------------------------------------------------------------------

const char* llm_piece(const LlmContext& c, uint16_t token, uint32_t* len) {
    if (!c.hdr || token >= c.vocab) { if (len) *len = 0; return nullptr; }
    const uint32_t a = c.tok_index[token], b = c.tok_index[token + 1];
    if (b < a) { if (len) *len = 0; return nullptr; }
    if (len) *len = b - a;
    return reinterpret_cast<const char*>(c.tok_bytes + a);
}

int32_t llm_encode(const LlmContext& c, const char* text, bool add_bos,
                   uint16_t* out, int32_t max_out) {
    if (!c.hdr || !text || !out || max_out <= 0) return -1;
    const int32_t max_piece = int32_t(c.hdr->max_token_len);
    int32_t n = 0;
    bool overflow = false;

    auto push = [&](int32_t id) {
        if (n < max_out) out[n++] = uint16_t(id);
        else overflow = true;
    };

    if (add_bos) push(int32_t(c.hdr->bos_id));
    if (text[0]) {                                    // llama2.c's dummy prefix
        const int32_t sp = vocab_lookup(c, " ", 1);
        if (sp >= 0) push(sp);
    }

    const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
    while (*p && !overflow) {
        int32_t len = 1;
        if ((*p & 0xC0u) == 0xC0u)                    // a UTF-8 lead byte: absorb continuations
            while (len < 4 && (p[len] & 0xC0u) == 0x80u) ++len;
        const int32_t id = vocab_lookup(c, reinterpret_cast<const char*>(p), uint32_t(len));
        if (id >= 0) push(id);
        else for (int32_t i = 0; i < len; ++i) push(int32_t(p[i]) + 3);   // byte fallback
        p += len;
    }
    if (overflow) return -1;

    // Greedy highest-score pair merging, in place, no allocation.
    for (;;) {
        int32_t best_score = -2147483647 - 1, best_id = -1, best_idx = -1;
        for (int32_t i = 0; i + 1 < n; ++i) {
            uint32_t la = 0, lb = 0;
            const char* a = llm_piece(c, out[i],     &la);
            const char* b = llm_piece(c, out[i + 1], &lb);
            if (!a || !b) continue;
            if (int32_t(la + lb) > max_piece) continue;          // cannot be a vocab entry
            for (uint32_t k = 0; k < la; ++k) c.scratch[k]      = a[k];
            for (uint32_t k = 0; k < lb; ++k) c.scratch[la + k] = b[k];
            const int32_t id = vocab_lookup(c, c.scratch, la + lb);
            if (id >= 0 && c.tok_score[id] > best_score) {
                best_score = c.tok_score[id]; best_id = id; best_idx = i;
            }
        }
        if (best_idx < 0) break;
        out[best_idx] = uint16_t(best_id);
        for (int32_t i = best_idx + 1; i + 1 < n; ++i) out[i] = out[i + 1];
        --n;
    }
    return n;
}

int32_t llm_decode(const LlmContext& c, uint16_t prev, uint16_t token, char* out, int32_t cap) {
    if (!out || cap <= 0) return 0;
    out[0] = '\0';
    uint32_t len = 0;
    const char* p = llm_piece(c, token, &len);
    if (!p) return 0;

    if (c.hdr && prev == c.hdr->bos_id && len && p[0] == ' ') { ++p; --len; }

    if (len == 6 && p[0] == '<' && p[1] == '0' && p[2] == 'x' && p[5] == '>') {
        const int hi = hexval(p[3]), lo = hexval(p[4]);
        if (hi >= 0 && lo >= 0) {
            if (cap < 2) return 0;
            out[0] = char(static_cast<unsigned char>(hi * 16 + lo));
            out[1] = '\0';
            return 1;
        }
    }
    if (int32_t(len) > cap - 1) len = uint32_t(cap - 1);
    for (uint32_t i = 0; i < len; ++i) out[i] = p[i];
    out[len] = '\0';
    return int32_t(len);
}

} // namespace tinyllm
