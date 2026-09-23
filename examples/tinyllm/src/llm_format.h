// examples/tinyllm/src/llm_format.h — the `.phxllm` model blob format. Engine-free integer POD
// (like miracle-player's viz.h): plain <stdint.h>, no phx headers, no STL, so the SAME definition
// compiles into the device inference core and is mirrored byte-for-byte by the host exporter
// (`examples/tinyllm/tools/export_model.py` — keep the two in sync; the exporter re-states every
// constant here and `tests/unit/test_tinyllm_format.cpp` pins the struct sizes).
//
// The blob is read ZERO-COPY, in place, out of cartridge ROM. Nothing here is ever parsed or
// copied into RAM at load: llm_init() only validates bounds and caches pointers. Everything is
// little-endian and 4-byte aligned, which is also the GBA's natural alignment (a misaligned
// 32-bit load on ARM7TDMI silently rotates instead of faulting, so alignment is validated).
//
// Layout:
//   [LlmHeader (64 B)][LlmTensor dir (32 B each)][4-byte-aligned tensor payloads ...]
#ifndef TINYLLM_LLM_FORMAT_H
#define TINYLLM_LLM_FORMAT_H

#include <stdint.h>

namespace tinyllm {

// 'P','H','X','L' little-endian.
inline constexpr uint32_t kLlmMagic        = 0x4C584850u;
inline constexpr uint16_t kLlmVersionMajor = 1;
inline constexpr uint16_t kLlmVersionMinor = 0;

// --- LUT geometry (baked into the blob so the device never evaluates exp/silu) ----------------
// Both LUTs are Q16.16 and use a power-of-two step so the index/fraction split is a shift, never
// a divide (ARM7TDMI has no divide instruction).
inline constexpr int32_t kExpLutN      = 512;          // intervals; the table has N+1 entries
inline constexpr int32_t kExpLutMinQ16 = -(16 << 16);  // domain [-16, 0]; step = 1/32 = 2048 Q16
inline constexpr int32_t kExpLutShift  = 11;           // log2(2048)
inline constexpr int32_t kSiluLutN     = 512;
inline constexpr int32_t kSiluLutMinQ16 = -(16 << 16); // domain [-16, 16]; step = 1/16 = 4096 Q16
inline constexpr int32_t kSiluLutShift = 12;           // log2(4096)

// --- header flags ------------------------------------------------------------------------------
inline constexpr uint16_t kLlmFlagSharedClassifier = 1u << 0;   // the output head IS TokEmb

// Tensor kinds. `layer` selects which of n_layers a per-layer kind belongs to; non-layered kinds
// store 0xFFFF. Adding a kind is a minor-version bump (unknown kinds are ignored, required ones
// are checked by name at init), so a bigger checkpoint drops in without a format change.
enum LlmTensorKind : uint8_t {
    kTensorTokEmb    = 0,   // int8  (vocab, dim)      token embedding; also the classifier if shared
    kTensorRmsAtt    = 1,   // int32 (1, dim)          Q16.16 RMSNorm gain, pre-attention
    kTensorWq        = 2,   // int8  (dim, dim)
    kTensorWk        = 3,   // int8  (kv_dim, dim)
    kTensorWv        = 4,   // int8  (kv_dim, dim)
    kTensorWo        = 5,   // int8  (dim, dim)
    kTensorRmsFfn    = 6,   // int32 (1, dim)          Q16.16 RMSNorm gain, pre-FFN
    kTensorW1        = 7,   // int8  (hidden_dim, dim)
    kTensorW2        = 8,   // int8  (dim, hidden_dim)
    kTensorW3        = 9,   // int8  (hidden_dim, dim)
    kTensorRmsFinal  = 10,  // int32 (1, dim)
    kTensorWcls      = 11,  // int8  (vocab, dim)      only when !kLlmFlagSharedClassifier
    kTensorRopeCos   = 12,  // int16 (seq_len, head_size/2)  Q15
    kTensorRopeSin   = 13,  // int16 (seq_len, head_size/2)  Q15
    kTensorExpLut    = 14,  // int32 (1, kExpLutN+1)   Q16.16
    kTensorSiluLut   = 15,  // int32 (1, kSiluLutN+1)  Q16.16
    kTensorTokBytes  = 16,  // u8    (1, N)            concatenated vocab pieces, no separators
    kTensorTokIndex  = 17,  // u32   (1, vocab+1)      byte offsets into TokBytes
    kTensorTokSorted = 18,  // u16   (1, vocab)        token ids ordered by piece bytes (memcmp)
    kTensorTokScore  = 19,  // int32 (1, vocab)        Q16.16 merge scores
    kTensorKindCount = 20,
};

// Payload element types.
enum LlmDType : uint8_t {
    kDTypeQ8    = 0,   // int8 payload + a per-group int32 Q30 scale table at `scale_off`
    kDTypeI16   = 1,   // raw int16 (Q15 rope tables)
    kDTypeI32   = 2,   // raw int32 (Q16.16 gains / LUTs / scores)
    kDTypeU8    = 3,   // raw bytes
    kDTypeU16   = 4,   // raw uint16
    kDTypeU32   = 5,   // raw uint32
};

inline constexpr uint16_t kNoLayer = 0xFFFFu;

// One directory entry. 32 bytes, every field naturally aligned.
struct LlmTensor {
    uint32_t name;      // fnv1a-32 of the tensor's canonical name ("wq.3"), for diagnostics only
    uint8_t  kind;      // LlmTensorKind
    uint8_t  dtype;     // LlmDType
    uint16_t layer;     // layer index, or kNoLayer
    uint32_t rows;      // output rows (matrices) or 1
    uint32_t cols;      // LOGICAL input width
    uint32_t stride;    // PADDED row length: a multiple of group_size for kDTypeQ8, else == cols
    uint32_t data_off;  // byte offset of the payload from the blob start (4-byte aligned)
    uint32_t scale_off; // byte offset of the rows*(stride/group_size) int32 Q30 scales, else 0
    uint32_t nbytes;    // payload byte length
};

// 64 bytes. `blob_size` is authoritative: llm_validate() refuses anything that disagrees with the
// buffer it was handed, so a truncated ROM image can never drive an out-of-bounds read.
struct LlmHeader {
    uint32_t magic;
    uint16_t version_major;
    uint16_t version_minor;

    uint16_t dim;
    uint16_t hidden_dim;
    uint16_t n_layers;
    uint16_t n_heads;
    uint16_t n_kv_heads;
    uint16_t vocab_size;
    uint16_t seq_len;        // the checkpoint's trained context; the runtime may cap below this
    uint16_t group_size;     // int8 quantization group (power of two, divides every `stride`)
    uint16_t head_size;      // dim / n_heads (stored so validation is a comparison, not a divide)
    uint16_t kv_dim;         // n_kv_heads * head_size
    int16_t  kv_shift;       // KV cache is int16 at Q(kv_shift): cached = value * 2^kv_shift
    uint16_t flags;          // kLlmFlag*

    int32_t  rms_eps_q16;    // RMSNorm epsilon, Q16.16
    uint32_t tensor_count;
    uint32_t dir_offset;     // byte offset of the LlmTensor array
    uint32_t blob_size;      // total bytes, header included
    uint32_t crc32;          // CRC32 of bytes [52, blob_size); 0 = not computed

    uint16_t max_token_len;  // longest tokenizer piece in bytes
    uint16_t bos_id;
    uint16_t eos_id;
    uint16_t reserved0;
    uint32_t reserved1;      // pads the header to 64 bytes; must be written as 0
};

static_assert(sizeof(LlmTensor) == 32, "LlmTensor must stay 32 bytes (the exporter writes 32)");
static_assert(sizeof(LlmHeader) == 64, "LlmHeader must stay 64 bytes (the exporter writes 64)");

// Validate a candidate blob and return its header, or nullptr. Checks (in order): size floor,
// magic, major version, header self-consistency (head_size*n_heads == dim, kv_dim, group_size a
// power of two, non-zero shapes), blob_size == size, directory in bounds, and for EVERY tensor
// that its payload AND its scale table lie inside [0, size) with 4-byte-aligned offsets and a
// byte length matching rows*stride*element_size. Pure reads, no allocation — safe to call on
// attacker-shaped bytes. Defined in llm.cpp.
const LlmHeader* llm_validate(const void* data, uint32_t size);

// Directory accessor (only valid after llm_validate succeeded).
inline const LlmTensor* llm_dir(const LlmHeader* h) {
    return reinterpret_cast<const LlmTensor*>(reinterpret_cast<const uint8_t*>(h) + h->dir_offset);
}

// Find a tensor by kind (+ layer for per-layer kinds). Linear over a directory of a few dozen
// entries, called only at init. Returns nullptr when absent.
const LlmTensor* llm_find(const LlmHeader*, uint8_t kind, uint16_t layer = kNoLayer);

// Byte size of one element of `dtype` (kDTypeQ8 counts the int8 payload only).
inline uint32_t llm_dtype_size(uint8_t dtype) {
    switch (dtype) {
        case kDTypeQ8:  case kDTypeU8:  return 1;
        case kDTypeI16: case kDTypeU16: return 2;
        default:                        return 4;
    }
}

} // namespace tinyllm
#endif // TINYLLM_LLM_FORMAT_H
