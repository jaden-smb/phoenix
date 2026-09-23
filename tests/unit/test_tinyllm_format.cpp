// tests/unit/test_tinyllm_format.cpp — the `.phxllm` blob format: layout, validation, and the
// rejection of malformed input.
//
// The model blob is read ZERO-COPY out of cartridge ROM, so llm_validate() is the only thing
// standing between a truncated/hand-edited image and an out-of-bounds read on a console with no
// MMU. Every field it checks gets a test that corrupts exactly that field and asserts a refusal.
#include "../phx_test.h"
#include "../../examples/tinyllm/src/llm.h"
#include "../../examples/tinyllm/src/llm_build.h"

#include <vector>

using namespace tinyllm;

namespace {

// The fixture blob is GENERATED (this repo tracks no binaries) and is deterministic, so building
// it once per case is both cheap and reproducible.
const std::vector<uint8_t>& fixture() {
    static const std::vector<uint8_t> b = build::build_fixture_blob();
    return b;
}

// A mutable, 4-byte-aligned copy to corrupt. std::vector's data is suitably aligned for uint8_t
// only, so route through a uint32_t buffer to guarantee the alignment llm_validate demands.
struct Copy {
    std::vector<uint32_t> words;
    uint32_t size = 0;
    explicit Copy(const std::vector<uint8_t>& src)
        : words((src.size() + 3) / 4, 0u), size(uint32_t(src.size())) {
        for (size_t i = 0; i < src.size(); ++i)
            reinterpret_cast<uint8_t*>(words.data())[i] = src[i];
    }
    uint8_t*   bytes()  { return reinterpret_cast<uint8_t*>(words.data()); }
    LlmHeader* header() { return reinterpret_cast<LlmHeader*>(words.data()); }
    LlmTensor* dir()    { return reinterpret_cast<LlmTensor*>(bytes() + header()->dir_offset); }
    const LlmHeader* validate() { return llm_validate(bytes(), size); }
};

} // namespace

PHX_TEST(tinyllm_format_struct_layout_is_pinned) {
    // The exporter writes these sizes literally (struct.calcsize in export_model.py). If either
    // drifts, every blob in the wild silently misparses — so pin them here as well as in the
    // header's own static_asserts.
    CHECK_EQ(int(sizeof(LlmHeader)), 64);
    CHECK_EQ(int(sizeof(LlmTensor)), 32);
    CHECK_EQ(int(kLlmMagic), 0x4C584850);
    CHECK_EQ(int(kLlmVersionMajor), 1);
    // LUT geometry: both steps must stay powers of two or the shift-based lookup is wrong.
    CHECK_EQ(int(kExpLutN), 512);
    CHECK_EQ(int(kSiluLutN), 512);
    CHECK_EQ(16 << 16, kExpLutN << kExpLutShift);
    CHECK_EQ(32 << 16, kSiluLutN << kSiluLutShift);
}

PHX_TEST(tinyllm_format_accepts_the_fixture) {
    Copy c(fixture());
    const LlmHeader* h = c.validate();
    CHECK(h != nullptr);
    if (!h) return;
    CHECK_EQ(int(h->dim), 16);
    CHECK_EQ(int(h->hidden_dim), 32);
    CHECK_EQ(int(h->n_layers), 2);
    CHECK_EQ(int(h->n_heads), 4);
    CHECK_EQ(int(h->n_kv_heads), 2);
    CHECK_EQ(int(h->head_size), 4);
    CHECK_EQ(int(h->kv_dim), 8);
    CHECK_EQ(int(h->group_size), 16);
    CHECK_EQ(int(h->blob_size), int(c.size));
    CHECK((h->flags & kLlmFlagSharedClassifier) != 0);
    // The recorded CRC32 must match the bytes it covers.
    CHECK_EQ(h->crc32, build::crc32(c.bytes() + 52, c.size - 52));
}

PHX_TEST(tinyllm_format_find_locates_layered_and_global_tensors) {
    Copy c(fixture());
    const LlmHeader* h = c.validate();
    CHECK(h != nullptr);
    if (!h) return;
    CHECK(llm_find(h, kTensorTokEmb) != nullptr);
    CHECK(llm_find(h, kTensorRmsFinal) != nullptr);
    CHECK(llm_find(h, kTensorExpLut) != nullptr);
    for (uint16_t l = 0; l < h->n_layers; ++l) {
        CHECK(llm_find(h, kTensorWq, l) != nullptr);
        CHECK(llm_find(h, kTensorW2, l) != nullptr);
        CHECK(llm_find(h, kTensorRmsAtt, l) != nullptr);
    }
    // Absent things really are absent: a layer past the end, and the separate classifier a
    // shared-embedding checkpoint does not carry.
    CHECK(llm_find(h, kTensorWq, h->n_layers) == nullptr);
    CHECK(llm_find(h, kTensorWcls) == nullptr);
    // A layered kind is not findable as a global one, and vice versa.
    CHECK(llm_find(h, kTensorWq) == nullptr);
    CHECK(llm_find(h, kTensorRmsFinal, 0) == nullptr);
}

PHX_TEST(tinyllm_format_rejects_malformed_blobs) {
    const std::vector<uint8_t>& src = fixture();

    CHECK(llm_validate(nullptr, 100) == nullptr);
    CHECK(llm_validate(src.data(), 0) == nullptr);
    CHECK(llm_validate(src.data(), 63) == nullptr);            // shorter than the header

    { Copy c(src); c.header()->magic ^= 1u;               CHECK(c.validate() == nullptr); }
    { Copy c(src); c.header()->version_major = 2;         CHECK(c.validate() == nullptr); }
    { Copy c(src); c.header()->blob_size += 4;            CHECK(c.validate() == nullptr); }
    { Copy c(src); c.size -= 4;                           CHECK(c.validate() == nullptr); }
    { Copy c(src); c.header()->dim = 0;                   CHECK(c.validate() == nullptr); }
    { Copy c(src); c.header()->n_layers = 0;              CHECK(c.validate() == nullptr); }
    { Copy c(src); c.header()->hidden_dim = 0;            CHECK(c.validate() == nullptr); }
    { Copy c(src); c.header()->head_size = 5;             CHECK(c.validate() == nullptr); }  // *n_heads != dim
    { Copy c(src); c.header()->kv_dim = 9;                CHECK(c.validate() == nullptr); }
    { Copy c(src); c.header()->group_size = 17;           CHECK(c.validate() == nullptr); }  // not a power of two
    { Copy c(src); c.header()->group_size = 2;            CHECK(c.validate() == nullptr); }  // below the unroll
    { Copy c(src); c.header()->kv_shift = 0;              CHECK(c.validate() == nullptr); }
    { Copy c(src); c.header()->kv_shift = 17;             CHECK(c.validate() == nullptr); }
    { Copy c(src); c.header()->n_kv_heads = 3;            CHECK(c.validate() == nullptr); }  // GQA not integral
    { Copy c(src); c.header()->bos_id = 60000;            CHECK(c.validate() == nullptr); }
    { Copy c(src); c.header()->eos_id = 60000;            CHECK(c.validate() == nullptr); }
    { Copy c(src); c.header()->max_token_len = 0;         CHECK(c.validate() == nullptr); }
    { Copy c(src); c.header()->tensor_count = 0;          CHECK(c.validate() == nullptr); }
    { Copy c(src); c.header()->tensor_count += 10000;     CHECK(c.validate() == nullptr); }
    { Copy c(src); c.header()->dir_offset = 3;            CHECK(c.validate() == nullptr); }  // misaligned
    { Copy c(src); c.header()->dir_offset = c.size + 4;   CHECK(c.validate() == nullptr); }

    // Per-tensor bounds — the checks that actually stop an out-of-bounds read.
    { Copy c(src); c.dir()[0].data_off = c.size - 4;      CHECK(c.validate() == nullptr); }
    { Copy c(src); c.dir()[0].data_off |= 1u;             CHECK(c.validate() == nullptr); }
    { Copy c(src); c.dir()[0].nbytes += 4;                CHECK(c.validate() == nullptr); }
    { Copy c(src); c.dir()[0].rows = 0;                   CHECK(c.validate() == nullptr); }
    { Copy c(src); c.dir()[0].stride = c.dir()[0].cols - 1; CHECK(c.validate() == nullptr); }
    { Copy c(src); c.dir()[0].scale_off = 0;              CHECK(c.validate() == nullptr); }  // Q8 needs scales
    { Copy c(src); c.dir()[0].scale_off = c.size - 4;     CHECK(c.validate() == nullptr); }
    { Copy c(src); c.dir()[0].dtype = 99;                 CHECK(c.validate() == nullptr); }
    // A non-Q8 tensor must NOT carry a scale table (it would be silently ignored otherwise).
    { Copy c(src);
      for (uint32_t i = 0; i < c.header()->tensor_count; ++i)
          if (c.dir()[i].dtype == kDTypeI32) { c.dir()[i].scale_off = 64; break; }
      CHECK(c.validate() == nullptr); }

    // A misaligned base pointer: a 32-bit load at an odd address silently rotates on ARM7TDMI
    // instead of faulting, so this has to be caught up front.
    { Copy c(src);
      std::vector<uint8_t> shifted(c.size + 1);
      for (uint32_t i = 0; i < c.size; ++i) shifted[i + 1] = c.bytes()[i];
      CHECK(llm_validate(shifted.data() + 1, c.size) == nullptr); }
}

PHX_TEST(tinyllm_format_arena_sizing_is_derived_not_hardcoded) {
    Copy c(fixture());
    const LlmHeader* h = c.validate();
    CHECK(h != nullptr);
    if (!h) return;

    // per-token KV bytes = n_layers * 2 (K and V) * kv_dim * sizeof(int16)
    const uint32_t per_token = uint32_t(h->n_layers) * 2u * h->kv_dim * 2u;
    CHECK_EQ(int(per_token), 2 * 2 * 8 * 2);

    // A generous budget caps at the checkpoint's trained context, not beyond it.
    CHECK_EQ(llm_max_seq(h, 1u << 20), int(h->seq_len));
    // A tight budget derives a shorter window.
    CHECK_EQ(llm_max_seq(h, per_token * 7), 7);
    CHECK_EQ(llm_max_seq(h, per_token * 7 + per_token / 2), 7);   // partial slots do not count
    CHECK_EQ(llm_max_seq(h, 0), 0);

    // Sizing must grow with the window and must be what init actually consumes.
    const uint32_t small = llm_required_arena_bytes(h, per_token * 8);
    const uint32_t big   = llm_required_arena_bytes(h, 1u << 20);
    CHECK(small > 0);
    CHECK(big > small);

    std::vector<uint8_t> mem(big + 64);
    phx::ArenaAllocator arena;
    arena.init(mem.data(), mem.size());
    LlmContext ctx;
    CHECK(llm_init(ctx, c.bytes(), c.size, arena, 1u << 20));
    CHECK_EQ(int(ctx.arena_bytes), int(big));
    CHECK(arena.used() <= big);
    CHECK_EQ(int(ctx.max_seq), int(h->seq_len));
    CHECK_EQ(int(ctx.kv_mul), 2);
}

PHX_TEST(tinyllm_format_init_refuses_an_undersized_arena) {
    Copy c(fixture());
    const LlmHeader* h = c.validate();
    CHECK(h != nullptr);
    if (!h) return;
    const uint32_t need = llm_required_arena_bytes(h, 1u << 20);

    std::vector<uint8_t> mem(need / 2);
    phx::ArenaAllocator arena;
    arena.init(mem.data(), mem.size());
    LlmContext ctx;
    CHECK(!llm_init(ctx, c.bytes(), c.size, arena, 1u << 20));
    CHECK(ctx.hdr == nullptr);       // and it leaves the context inert rather than half-built
}

PHX_TEST(tinyllm_format_init_refuses_a_useless_context_window) {
    Copy c(fixture());
    const LlmHeader* h = c.validate();
    CHECK(h != nullptr);
    if (!h) return;
    std::vector<uint8_t> mem(1u << 16);
    phx::ArenaAllocator arena;
    arena.init(mem.data(), mem.size());
    LlmContext ctx;
    // A window of 0 or 1 token cannot generate anything; better to fail at boot than to run.
    CHECK(!llm_init(ctx, c.bytes(), c.size, arena, 0));
    arena.reset();
    CHECK(!llm_init(ctx, c.bytes(), c.size, arena, 32));
}

PHX_TEST(tinyllm_format_init_refuses_a_shape_mismatched_directory) {
    // A structurally valid blob whose tensor shapes disagree with the header must fail at INIT,
    // not part-way through a generation on a console.
    const std::vector<uint8_t>& src = fixture();
    Copy c(src);
    for (uint32_t i = 0; i < c.header()->tensor_count; ++i) {
        if (c.dir()[i].kind == kTensorW2) { c.dir()[i].cols = 16; c.dir()[i].stride = 16;
                                            c.dir()[i].nbytes = c.dir()[i].rows * 16; break; }
    }
    CHECK(c.validate() != nullptr);        // still self-consistent as a blob...
    std::vector<uint8_t> mem(1u << 20);
    phx::ArenaAllocator arena;
    arena.init(mem.data(), mem.size());
    LlmContext ctx;
    CHECK(!llm_init(ctx, c.bytes(), c.size, arena, 1u << 16));   // ...but not a runnable model
}
