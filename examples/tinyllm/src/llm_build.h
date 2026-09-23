// examples/tinyllm/src/llm_build.h — HOST-ONLY `.phxllm` writer (STL is fine here; this never
// ships to a console, exactly like tools/phxpack). Two jobs:
//
//   1. build_fixture_blob() — a deliberately tiny, fully deterministic model used as the test
//      fixture and as the offline fallback when no real checkpoint has been fetched. This repo
//      tracks ZERO binary files, so the fixture is GENERATED rather than committed; only the
//      small text golden lives in tests/fixtures/.
//   2. BlobBuilder — the writer those use, kept honest against the normative definition in
//      llm_format.h and against tools/export_model.py, which writes the same bytes for real
//      checkpoints.
//
// Determinism matters here: the golden token file is tied to the exact fixture bytes, so the
// weights come from an INTEGER prng (never float) and the blob's CRC32 is asserted by the suite
// before the tokens are, so a platform whose libm rounds the exp/SiLU/RoPE tables differently
// reports "fixture changed" instead of a mysterious token mismatch.
#ifndef TINYLLM_LLM_BUILD_H
#define TINYLLM_LLM_BUILD_H

#include "llm_format.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace tinyllm {
namespace build {

// ---------------------------------------------------------------------------------------------
// Blob assembly
// ---------------------------------------------------------------------------------------------

struct HeaderCfg {
    uint16_t dim = 0, hidden_dim = 0, n_layers = 0, n_heads = 0, n_kv_heads = 0;
    uint16_t vocab_size = 0, seq_len = 0, group_size = 32;
    int16_t  kv_shift = 9;
    uint16_t flags = kLlmFlagSharedClassifier;
    int32_t  rms_eps_q16 = 1;               // 1e-5 rounds to 1 in Q16.16
    uint16_t max_token_len = 0, bos_id = 1, eos_id = 2;
};

inline uint32_t fnv1a(const std::string& s) {
    uint32_t h = 0x811C9DC5u;
    for (unsigned char c : s) h = (h ^ c) * 0x01000193u;
    return h;
}

// CRC32 (the same polynomial phx/core/crc32.h uses) over an arbitrary span — the fixture's
// identity check.
inline uint32_t crc32(const uint8_t* p, size_t n, uint32_t crc = 0) {
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1u) + 1u));
    }
    return ~crc;
}

class BlobBuilder {
public:
    void add_q8(const std::string& name, uint8_t kind, uint16_t layer,
                const int8_t* codes, const int32_t* scales,
                uint32_t rows, uint32_t cols, uint32_t stride, uint32_t group) {
        Entry e;
        e.name = name; e.kind = kind; e.dtype = kDTypeQ8; e.layer = layer;
        e.rows = rows; e.cols = cols; e.stride = stride;
        e.payload.assign(reinterpret_cast<const uint8_t*>(codes),
                         reinterpret_cast<const uint8_t*>(codes) + size_t(rows) * stride);
        const size_t ns = size_t(rows) * (stride / group);
        e.scales.assign(reinterpret_cast<const uint8_t*>(scales),
                        reinterpret_cast<const uint8_t*>(scales) + ns * 4);
        entries_.push_back(std::move(e));
    }

    // Quantize a float matrix exactly the way export_model.py does: symmetric int8, per-group
    // Q30 scale, rows zero-padded up to a whole group.
    void add_q8_float(const std::string& name, uint8_t kind, uint16_t layer,
                      const float* mat, uint32_t rows, uint32_t cols, uint32_t group) {
        const uint32_t stride = ((cols + group - 1) / group) * group;
        std::vector<int8_t>  codes(size_t(rows) * stride, 0);
        std::vector<int32_t> scales(size_t(rows) * (stride / group), 0);
        for (uint32_t r = 0; r < rows; ++r) {
            for (uint32_t g = 0; g < stride / group; ++g) {
                double amax = 0.0;
                for (uint32_t j = 0; j < group; ++j) {
                    const uint32_t col = g * group + j;
                    const double v = col < cols ? double(mat[size_t(r) * cols + col]) : 0.0;
                    if (std::fabs(v) > amax) amax = std::fabs(v);
                }
                const double sc = amax / 127.0;
                double q30 = std::floor(sc * double(1u << 30) + 0.5);
                if (q30 > 2147483647.0) q30 = 2147483647.0;
                scales[size_t(r) * (stride / group) + g] = int32_t(q30);
                for (uint32_t j = 0; j < group; ++j) {
                    const uint32_t col = g * group + j;
                    if (col >= cols || sc <= 0.0) continue;
                    double q = std::floor(double(mat[size_t(r) * cols + col]) / sc + 0.5);
                    if (q >  127.0) q =  127.0;
                    if (q < -127.0) q = -127.0;
                    codes[size_t(r) * stride + col] = int8_t(q);
                }
            }
        }
        add_q8(name, kind, layer, codes.data(), scales.data(), rows, cols, stride, group);
    }

    template <class T>
    void add_vec(const std::string& name, uint8_t kind, uint8_t dtype,
                 const T* data, uint32_t n, uint16_t layer = kNoLayer) {
        Entry e;
        e.name = name; e.kind = kind; e.dtype = dtype; e.layer = layer;
        e.rows = 1; e.cols = n; e.stride = n;
        e.payload.assign(reinterpret_cast<const uint8_t*>(data),
                         reinterpret_cast<const uint8_t*>(data) + size_t(n) * sizeof(T));
        entries_.push_back(std::move(e));
    }

    std::vector<uint8_t> finish(const HeaderCfg& cfg) const {
        const uint32_t count   = uint32_t(entries_.size());
        const uint32_t dir_off = uint32_t(sizeof(LlmHeader));
        uint32_t cursor = dir_off + count * uint32_t(sizeof(LlmTensor));

        std::vector<LlmTensor> dir(count);
        std::vector<uint8_t>   body;
        auto place = [&](const std::vector<uint8_t>& data) -> uint32_t {
            while (cursor & 3u) { body.push_back(0); ++cursor; }
            const uint32_t off = cursor;
            body.insert(body.end(), data.begin(), data.end());
            cursor += uint32_t(data.size());
            return off;
        };

        for (uint32_t i = 0; i < count; ++i) {
            const Entry& e = entries_[i];
            LlmTensor& t = dir[i];
            t.name = fnv1a(e.name);
            t.kind = e.kind; t.dtype = e.dtype; t.layer = e.layer;
            t.rows = e.rows; t.cols = e.cols; t.stride = e.stride;
            t.data_off  = place(e.payload);
            t.scale_off = e.scales.empty() ? 0u : place(e.scales);
            t.nbytes    = uint32_t(e.payload.size());
        }

        LlmHeader h{};
        h.magic = kLlmMagic;
        h.version_major = kLlmVersionMajor;
        h.version_minor = kLlmVersionMinor;
        h.dim = cfg.dim; h.hidden_dim = cfg.hidden_dim; h.n_layers = cfg.n_layers;
        h.n_heads = cfg.n_heads; h.n_kv_heads = cfg.n_kv_heads;
        h.vocab_size = cfg.vocab_size; h.seq_len = cfg.seq_len; h.group_size = cfg.group_size;
        h.head_size = uint16_t(cfg.dim / cfg.n_heads);
        h.kv_dim = uint16_t(h.head_size * cfg.n_kv_heads);
        h.kv_shift = cfg.kv_shift; h.flags = cfg.flags; h.rms_eps_q16 = cfg.rms_eps_q16;
        h.tensor_count = count; h.dir_offset = dir_off; h.blob_size = cursor; h.crc32 = 0;
        h.max_token_len = cfg.max_token_len; h.bos_id = cfg.bos_id; h.eos_id = cfg.eos_id;
        h.reserved0 = 0; h.reserved1 = 0;

        std::vector<uint8_t> out(cursor);
        std::memcpy(out.data(), &h, sizeof(h));
        std::memcpy(out.data() + dir_off, dir.data(), size_t(count) * sizeof(LlmTensor));
        std::memcpy(out.data() + dir_off + count * sizeof(LlmTensor), body.data(), body.size());
        const uint32_t crc = crc32(out.data() + 52, out.size() - 52);
        std::memcpy(out.data() + 48, &crc, 4);
        return out;
    }

private:
    struct Entry {
        std::string name;
        uint8_t kind = 0, dtype = 0;
        uint16_t layer = kNoLayer;
        uint32_t rows = 0, cols = 0, stride = 0;
        std::vector<uint8_t> payload, scales;
    };
    std::vector<Entry> entries_;
};

// ---------------------------------------------------------------------------------------------
// Shared tables
// ---------------------------------------------------------------------------------------------

inline std::vector<int32_t> build_exp_lut() {
    std::vector<int32_t> t(size_t(kExpLutN) + 1);
    for (int i = 0; i <= kExpLutN; ++i)
        t[size_t(i)] = int32_t(std::floor(std::exp(-16.0 + i / 32.0) * 65536.0 + 0.5));
    return t;
}

inline std::vector<int32_t> build_silu_lut() {
    std::vector<int32_t> t(size_t(kSiluLutN) + 1);
    for (int i = 0; i <= kSiluLutN; ++i) {
        const double x = -16.0 + i / 16.0;
        t[size_t(i)] = int32_t(std::floor(x / (1.0 + std::exp(-x)) * 65536.0 + 0.5));
    }
    return t;
}

inline void build_rope_tables(int seq_len, int head_size,
                              std::vector<int16_t>& cos_t, std::vector<int16_t>& sin_t) {
    const int half = head_size / 2;
    cos_t.assign(size_t(seq_len) * half, 0);
    sin_t.assign(size_t(seq_len) * half, 0);
    for (int pos = 0; pos < seq_len; ++pos) {
        for (int j = 0; j < half; ++j) {
            const double freq = 1.0 / std::pow(10000.0, double(2 * j) / double(head_size));
            const double v = double(pos) * freq;
            auto q15 = [](double d) {
                double q = std::floor(d * 32768.0 + 0.5);
                if (q >  32767.0) q =  32767.0;
                if (q < -32768.0) q = -32768.0;
                return int16_t(q);
            };
            cos_t[size_t(pos) * half + j] = q15(std::cos(v));
            sin_t[size_t(pos) * half + j] = q15(std::sin(v));
        }
    }
}

// ---------------------------------------------------------------------------------------------
// The fixture model
// ---------------------------------------------------------------------------------------------

// The synthetic vocabulary, shaped exactly like llama2.c's tok512: 3 specials, then the 256
// `<0xNN>` byte tokens (so `byte + 3` is the encoder's fallback, as upstream), then single-byte
// printable-ASCII pieces and a handful of multi-byte merges so the BPE merge loop is exercised.
// Mirrors build_fixture_tokenizer() in tools/export_model.py.
inline void fixture_vocab(std::vector<std::string>& pieces, std::vector<double>& scores) {
    pieces.clear(); scores.clear();
    auto add = [&](const std::string& p, double s) { pieces.push_back(p); scores.push_back(s); };
    add("<unk>", 0.0);
    add("\n<s>\n", 0.0);
    add("\n</s>\n", 0.0);
    static const char* kHex = "0123456789ABCDEF";
    for (int b = 0; b < 256; ++b) {
        std::string s = "<0x";
        s += kHex[(b >> 4) & 15];
        s += kHex[b & 15];
        s += '>';
        add(s, 0.0);
    }
    int rank = 0;
    for (int c = 0x20; c < 0x7F; ++c) add(std::string(1, char(c)), -double(rank++));
    static const char* kMerges[] = {
        " t", "he", " a", "in", " the", "ed", " to", " and", "er", "on",
        " w", "nd", "ll", " s", "or", "an", " was", "it", "ay", " he",
        " that", "ing", " said", "ar", "om", " f", "ow", " b", "is", "en",
    };
    for (const char* m : kMerges) add(m, -double(rank++));
}

// A tiny transformer with integer-generated weights. No float touches the weights, so the blob
// is byte-identical on every host — which is what lets a text golden pin it.
inline std::vector<uint8_t> build_fixture_blob() {
    const uint16_t dim = 16, hidden = 32, n_layers = 2, n_heads = 4, n_kv_heads = 2, seq_len = 32;
    const uint16_t group = 16;
    const uint16_t head_size = dim / n_heads;
    const uint16_t kv_dim = uint16_t(head_size * n_kv_heads);

    std::vector<std::string> pieces;
    std::vector<double>      scores;
    fixture_vocab(pieces, scores);
    const uint16_t vocab = uint16_t(pieces.size());

    uint32_t rng = 0x13579BDFu;
    auto next = [&]() { uint32_t x = rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return rng = x; };
    // int8 codes in [-96, 96] — never the +-127 rail, so nothing sits on a saturation edge.
    auto code = [&]() { return int8_t(int32_t(next() % 193u) - 96); };

    BlobBuilder b;
    std::vector<int8_t>  codes;
    std::vector<int32_t> scales;

    auto q8 = [&](const std::string& name, uint8_t kind, uint16_t layer,
                  uint32_t rows, uint32_t cols, int32_t scale_q30) {
        const uint32_t stride = ((cols + group - 1) / group) * group;
        codes.assign(size_t(rows) * stride, 0);
        scales.assign(size_t(rows) * (stride / group), 0);
        for (uint32_t r = 0; r < rows; ++r) {
            for (uint32_t c = 0; c < cols; ++c) codes[size_t(r) * stride + c] = code();
            for (uint32_t g = 0; g < stride / group; ++g)
                // Vary the scale per group by up to ~12% so the per-group path is genuinely
                // exercised (a single shared scale would hide a scale-indexing bug).
                scales[size_t(r) * (stride / group) + g] =
                    scale_q30 + int32_t(next() % uint32_t(scale_q30 / 8));
        }
        b.add_q8(name, kind, layer, codes.data(), scales.data(), rows, cols, stride, group);
    };

    const int32_t kScale = 1 << 22;   // ~0.0039 real: int8 * scale lands around +-0.5

    q8("tok_emb", kTensorTokEmb, kNoLayer, vocab, dim, kScale);
    for (uint16_t l = 0; l < n_layers; ++l) {
        q8("wq." + std::to_string(l), kTensorWq, l, dim,    dim,    kScale);
        q8("wk." + std::to_string(l), kTensorWk, l, kv_dim, dim,    kScale);
        q8("wv." + std::to_string(l), kTensorWv, l, kv_dim, dim,    kScale);
        q8("wo." + std::to_string(l), kTensorWo, l, dim,    dim,    kScale);
        q8("w1." + std::to_string(l), kTensorW1, l, hidden, dim,    kScale);
        q8("w2." + std::to_string(l), kTensorW2, l, dim,    hidden, kScale);
        q8("w3." + std::to_string(l), kTensorW3, l, hidden, dim,    kScale);
    }

    // RMSNorm gains around 1.0 (Q16.16), integer-generated like everything else.
    std::vector<int32_t> gain(dim);
    auto fill_gain = [&]() {
        for (uint16_t i = 0; i < dim; ++i) gain[i] = 45875 + int32_t(next() % 39322u);  // 0.7..1.3
    };
    for (uint16_t l = 0; l < n_layers; ++l) {
        fill_gain(); b.add_vec("rms_att." + std::to_string(l), kTensorRmsAtt, kDTypeI32, gain.data(), dim, l);
        fill_gain(); b.add_vec("rms_ffn." + std::to_string(l), kTensorRmsFfn, kDTypeI32, gain.data(), dim, l);
    }
    fill_gain();
    b.add_vec("rms_final", kTensorRmsFinal, kDTypeI32, gain.data(), dim);

    std::vector<int16_t> rc, rs;
    build_rope_tables(seq_len, head_size, rc, rs);
    b.add_vec("rope_cos", kTensorRopeCos, kDTypeI16, rc.data(), uint32_t(rc.size()));
    b.add_vec("rope_sin", kTensorRopeSin, kDTypeI16, rs.data(), uint32_t(rs.size()));

    const std::vector<int32_t> elut = build_exp_lut(), slut = build_silu_lut();
    b.add_vec("exp_lut",  kTensorExpLut,  kDTypeI32, elut.data(), uint32_t(elut.size()));
    b.add_vec("silu_lut", kTensorSiluLut, kDTypeI32, slut.data(), uint32_t(slut.size()));

    // Tokenizer: the concatenated pieces, an offset index, the memcmp-sorted id table the
    // runtime binary-searches, and Q16.16 merge scores.
    std::vector<uint8_t>  joined;
    std::vector<uint32_t> index(size_t(vocab) + 1, 0);
    uint16_t max_len = 0;
    for (uint16_t i = 0; i < vocab; ++i) {
        index[i] = uint32_t(joined.size());
        joined.insert(joined.end(), pieces[i].begin(), pieces[i].end());
        if (pieces[i].size() > max_len) max_len = uint16_t(pieces[i].size());
    }
    index[vocab] = uint32_t(joined.size());

    std::vector<uint16_t> sorted(vocab);
    for (uint16_t i = 0; i < vocab; ++i) sorted[i] = i;
    // (piece bytes, then id) — the same total order tools/export_model.py uses, so duplicate
    // pieces resolve to the same (lowest) id on both sides.
    for (uint16_t i = 1; i < vocab; ++i) {                      // insertion sort: stable, no <algorithm>
        const uint16_t v = sorted[i];
        int j = int(i) - 1;
        while (j >= 0 && (pieces[sorted[size_t(j)]] > pieces[v] ||
                          (pieces[sorted[size_t(j)]] == pieces[v] && sorted[size_t(j)] > v))) {
            sorted[size_t(j) + 1] = sorted[size_t(j)];
            --j;
        }
        sorted[size_t(j) + 1] = v;
    }

    std::vector<int32_t> score_q16(vocab);
    for (uint16_t i = 0; i < vocab; ++i)
        score_q16[i] = int32_t(std::floor(scores[i] * 65536.0 + 0.5));

    b.add_vec("tok_bytes",  kTensorTokBytes,  kDTypeU8,  joined.data(),    uint32_t(joined.size()));
    b.add_vec("tok_index",  kTensorTokIndex,  kDTypeU32, index.data(),     uint32_t(index.size()));
    b.add_vec("tok_sorted", kTensorTokSorted, kDTypeU16, sorted.data(),    uint32_t(sorted.size()));
    b.add_vec("tok_score",  kTensorTokScore,  kDTypeI32, score_q16.data(), uint32_t(score_q16.size()));

    HeaderCfg cfg;
    cfg.dim = dim; cfg.hidden_dim = hidden; cfg.n_layers = n_layers;
    cfg.n_heads = n_heads; cfg.n_kv_heads = n_kv_heads;
    cfg.vocab_size = vocab; cfg.seq_len = seq_len; cfg.group_size = group;
    cfg.kv_shift = 10;                       // random weights keep K/V small; leave real headroom
    cfg.flags = kLlmFlagSharedClassifier;
    cfg.rms_eps_q16 = 1;
    cfg.max_token_len = max_len;
    cfg.bos_id = 1; cfg.eos_id = 2;
    return b.finish(cfg);
}

} // namespace build
} // namespace tinyllm
#endif // TINYLLM_LLM_BUILD_H
