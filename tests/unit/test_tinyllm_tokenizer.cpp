// tests/unit/test_tinyllm_tokenizer.cpp — the allocation-free BPE tokenizer that ships in the
// ROM: binary search over the blob's sorted piece table, greedy highest-score merging, and
// llama2.c's two decode rules (strip the dummy leading space after BOS, expand `<0xNN>` back to
// a raw byte).
//
// It runs on device with no heap and no STL, working only in the caller's buffer and the
// context's fixed scratch — so the interesting failures are boundary ones, and those are what
// this file leans on.
#include "../phx_test.h"
#include "../../examples/tinyllm/src/llm.h"
#include "../../examples/tinyllm/src/llm_build.h"

#include <string>
#include <vector>

using namespace tinyllm;

namespace {

struct Fx {
    std::vector<uint32_t> words;
    uint32_t size = 0;
    std::vector<uint8_t> mem;
    phx::ArenaAllocator arena;
    LlmContext ctx;
    bool ok = false;

    Fx() {
        const std::vector<uint8_t> src = build::build_fixture_blob();
        size = uint32_t(src.size());
        words.assign((src.size() + 3) / 4, 0u);
        for (size_t i = 0; i < src.size(); ++i)
            reinterpret_cast<uint8_t*>(words.data())[i] = src[i];
        const LlmHeader* h = llm_validate(bytes(), size);
        if (!h) return;
        const uint32_t need = llm_required_arena_bytes(h, 1u << 16);
        mem.assign(need + 64, 0);
        arena.init(mem.data(), mem.size());
        ok = llm_init(ctx, bytes(), size, arena, 1u << 16);
    }
    uint8_t* bytes() { return reinterpret_cast<uint8_t*>(words.data()); }

    // Decode a whole token run the way a front end does: feed each token the one before it, so
    // the post-BOS leading-space rule fires exactly once.
    std::string detokenize(const uint16_t* ids, int32_t n, uint16_t prev) {
        std::string out;
        char buf[64];
        for (int32_t i = 0; i < n; ++i) {
            const int32_t k = llm_decode(ctx, prev, ids[i], buf, sizeof buf);
            out.append(buf, size_t(k));
            prev = ids[i];
        }
        return out;
    }
};

Fx& fx() { static Fx f; return f; }

} // namespace

PHX_TEST(tinyllm_tokenizer_round_trips_ascii) {
    Fx& f = fx();
    CHECK(f.ok);
    if (!f.ok) return;

    static const char* kCases[] = {
        "the cat sat on the mat",
        "Once upon a time, there was a little girl.",
        "a",
        " leading and trailing ",
        "MiXeD CaSe 12345 !?*",
        "the the the the",
    };
    for (const char* s : kCases) {
        uint16_t ids[512];
        const int32_t n = llm_encode(f.ctx, s, /*add_bos=*/true, ids, 512);
        CHECK(n > 0);
        if (n <= 0) continue;
        CHECK_EQ(int(ids[0]), int(f.ctx.hdr->bos_id));
        // Decoding everything after BOS must reproduce the input byte for byte: the dummy space
        // the encoder prepends is exactly what the post-BOS rule strips back off.
        const std::string back = f.detokenize(ids + 1, n - 1, f.ctx.hdr->bos_id);
        CHECK(back == std::string(s));
        if (back != std::string(s))
            std::printf("      round-trip: %s -> %s\n", s, back.c_str());
    }
}

PHX_TEST(tinyllm_tokenizer_merges_fire) {
    Fx& f = fx();
    if (!f.ok) return;
    uint16_t single[512], merged[512];
    // "t"+"h"+"e" as bare characters vs. the text "the": the merge loop must collapse the latter
    // into strictly fewer tokens, or the BPE pass is not running at all.
    const int32_t a = llm_encode(f.ctx, "t h e", true, single, 512);
    const int32_t b = llm_encode(f.ctx, "the", true, merged, 512);
    CHECK(a > 0 && b > 0);
    CHECK(b < a);
    // " the" is a vocab piece, so with the dummy prefix the whole word is ONE token after BOS.
    CHECK_EQ(b, 2);
}

PHX_TEST(tinyllm_tokenizer_falls_back_to_byte_tokens) {
    Fx& f = fx();
    if (!f.ok) return;
    // The fixture vocab has no multi-byte pieces, so a non-ASCII codepoint must decompose into
    // `<0xNN>` byte tokens at id = byte + 3 (llama2.c's convention) and decode back exactly.
    const char* s = "\xC3\xA9";                       // U+00E9, 2 UTF-8 bytes
    uint16_t ids[16];
    const int32_t n = llm_encode(f.ctx, s, false, ids, 16);
    CHECK_EQ(n, 3);                                   // dummy space + two byte tokens
    if (n != 3) return;
    CHECK_EQ(int(ids[1]), 0xC3 + 3);
    CHECK_EQ(int(ids[2]), 0xA9 + 3);
    char buf[16];
    CHECK_EQ(llm_decode(f.ctx, 0, ids[1], buf, sizeof buf), 1);
    CHECK_EQ(int(uint8_t(buf[0])), 0xC3);
    CHECK_EQ(llm_decode(f.ctx, 0, ids[2], buf, sizeof buf), 1);
    CHECK_EQ(int(uint8_t(buf[0])), 0xA9);
}

PHX_TEST(tinyllm_tokenizer_piece_table_is_consistent) {
    Fx& f = fx();
    if (!f.ok) return;
    const LlmContext& c = f.ctx;

    // Every id resolves to a non-empty piece within max_token_len, and the sorted table is a
    // permutation of [0, vocab) in non-decreasing byte order — the precondition the runtime's
    // binary search silently assumes.
    std::vector<int> seen(size_t(c.vocab), 0);
    std::string prev;
    for (int32_t i = 0; i < c.vocab; ++i) {
        const uint16_t id = c.tok_sorted[i];
        CHECK(id < c.vocab);
        if (id >= c.vocab) continue;
        ++seen[id];
        uint32_t len = 0;
        const char* p = llm_piece(c, id, &len);
        CHECK(p != nullptr);
        CHECK(len > 0);
        CHECK(len <= c.hdr->max_token_len);
        const std::string cur(p, len);
        CHECK(prev <= cur);
        prev = cur;
    }
    for (int32_t i = 0; i < c.vocab; ++i) CHECK_EQ(seen[size_t(i)], 1);

    // Out-of-range ids are refused rather than read past the index table.
    uint32_t len = 123;
    CHECK(llm_piece(c, uint16_t(c.vocab), &len) == nullptr);
    CHECK_EQ(int(len), 0);
}

PHX_TEST(tinyllm_tokenizer_encode_reports_overflow) {
    Fx& f = fx();
    if (!f.ok) return;
    uint16_t ids[4];
    // A long prompt into a 4-slot buffer must return -1, never write past the end. (The merge
    // pass only shrinks, so the pre-merge run is what has to fit.)
    CHECK_EQ(llm_encode(f.ctx, "the quick brown fox jumps over the lazy dog", true, ids, 4), -1);
    CHECK_EQ(llm_encode(f.ctx, nullptr, true, ids, 4), -1);
    CHECK_EQ(llm_encode(f.ctx, "hi", true, ids, 0), -1);
    // An empty prompt with BOS is legal and yields exactly BOS.
    CHECK_EQ(llm_encode(f.ctx, "", true, ids, 4), 1);
    CHECK_EQ(int(ids[0]), int(f.ctx.hdr->bos_id));
}

PHX_TEST(tinyllm_tokenizer_decode_respects_the_output_cap) {
    Fx& f = fx();
    if (!f.ok) return;
    uint16_t ids[64];
    const int32_t n = llm_encode(f.ctx, "the that", true, ids, 64);
    CHECK(n > 2);
    if (n <= 2) return;

    // A cap of 1 leaves room only for the terminator; 2 gives one byte. Either way the buffer is
    // always NUL-terminated and never overrun.
    char tiny[8];
    for (int cap = 1; cap <= 4; ++cap) {
        for (int32_t i = 1; i < n; ++i) {
            std::string canary(8, '\xAB');
            for (int k = 0; k < 8; ++k) tiny[k] = canary[size_t(k)];
            const int32_t k = llm_decode(f.ctx, ids[i - 1], ids[i], tiny, cap);
            CHECK(k >= 0 && k <= cap - 1);
            CHECK_EQ(int(tiny[k]), 0);
            for (int j = cap; j < 8; ++j) CHECK_EQ(int(uint8_t(tiny[size_t(j)])), 0xAB);
        }
    }
    CHECK_EQ(llm_decode(f.ctx, 0, 0, nullptr, 8), 0);
    CHECK_EQ(llm_decode(f.ctx, 0, 0, tiny, 0), 0);
}

PHX_TEST(tinyllm_tokenizer_strips_the_dummy_space_only_after_bos) {
    Fx& f = fx();
    if (!f.ok) return;
    uint16_t ids[64];
    const int32_t n = llm_encode(f.ctx, "the cat", true, ids, 64);
    CHECK(n >= 3);
    if (n < 3) return;
    char buf[32];
    // After BOS the first piece loses its leading space...
    llm_decode(f.ctx, f.ctx.hdr->bos_id, ids[1], buf, sizeof buf);
    CHECK(buf[0] != ' ');
    // ...but the space before "cat" mid-sentence is preserved.
    const std::string whole = f.detokenize(ids + 1, n - 1, f.ctx.hdr->bos_id);
    CHECK(whole == std::string("the cat"));
}
