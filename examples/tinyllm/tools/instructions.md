# `export_model.py` — checkpoint → `.phxllm`

The host half of `examples/tinyllm`. Turns a llama2-family checkpoint into the quantized blob the
Game Boy Advance streams out of cartridge ROM, and generates the golden tokens the test suite
asserts against. Host-only (Python 3 + numpy; PyTorch only for `.pt` inputs) — nothing here ships
to a console.

Same philosophy as the rest of `tools/` (docs/08): **do all the expensive, fallible work
offline.** Quantization, the RoPE table, the exp/SiLU lookup tables and the tokenizer index are
all baked here, so the device only ever does integer arithmetic on int8.

## Usage

```
export_model.py export  --checkpoint CKPT --tokenizer TOK --out OUT.phxllm [options]
export_model.py fixture --out OUT.phxllm [options]
export_model.py eval    --checkpoint CKPT --tokenizer TOK [--limit N]
export_model.py golden  --blob IN.phxllm --golden OUT.txt [--prompt P] [--steps N]
```

| Subcommand | What it does |
|---|---|
| `export`  | Quantize a checkpoint and write the blob. Reports per-tensor error and a size breakdown. |
| `fixture` | Build a tiny random model end-to-end in Python (useful for format experiments; the *committed* test fixture is built by the C++ writer instead — see below). |
| `eval`    | Report the int8-vs-float32 perplexity delta on a held-out sample, so quantization damage is measured rather than guessed. |
| `golden`  | Read an EXISTING `.phxllm` and write a golden-token file for it. This is the path the committed test golden takes. |

| Option | Default | Meaning |
|---|---|---|
| `--checkpoint` | — | llama2.c legacy `.bin`, or a PyTorch `.pt`/`.pth` `state_dict` |
| `--tokenizer`  | — | llama2.c `tokenizer.bin` (`max_len`, then `float score, int len, bytes` per token) |
| `--out`        | — | output `.phxllm` |
| `--group-size` | 32 | int8 quantization group; a power of two in [4, 256] |
| `--kv-shift`   | calibrated | override the KV cache's int16 Q shift |
| `--golden`     | — | also write a golden-token file |
| `--prompt`     | `Once upon a time` | prompt for the golden |
| `--steps`      | 24 | tokens to generate for the golden |
| `--seed`       | 0x2545F491 | sampler seed recorded in (and used for) the golden |

### Typical invocations

```bash
# what `make tinyllm-model` runs
python3 export_model.py export --checkpoint build/stories260K.bin \
        --tokenizer build/tok512.bin --out build/tinyllm.phxllm

# how much did quantization cost?
python3 export_model.py eval --checkpoint build/stories260K.bin --tokenizer build/tok512.bin

# what `make tinyllm-fixture` runs, after the C++ builder dumps the fixture blob
python3 export_model.py golden --blob build/tinyllm_fixture.phxllm \
        --golden ../../../tests/fixtures/tinyllm_golden.txt \
        --prompt "the cat sat on the mat" --steps 16
```

## Quantization

- **int8, symmetric, group-wise** along each row's input axis, with a **Q30** per-group scale.
  (Not Q16.16: measured group scales on stories260K run 5.9e-4…1.5e-2, and Q16.16's 1.5e-5 step
  would round the smallest to 0.9% relative error.) Rows are zero-padded up to a whole group so
  the device kernel has no ragged tail.
- Activations are quantized per-vector at runtime, not per-group.
- RMSNorm gains (Q16.16), RoPE cos/sin (Q15) and the tokenizer stay in higher precision.
- `kv_shift` is **calibrated**, not guessed: the float reference runs a calibration prompt, the
  largest K/V magnitude observed sets the int16 Q shift with ~2× headroom.

## The `.phxllm` format

The normative definition is `examples/tinyllm/src/llm_format.h`; this script restates every
constant from it. Change one, change both — the C++ side `static_assert`s the struct sizes and
`tests/unit/test_tinyllm_format.cpp` pins the layout.

```
[LlmHeader 64 B][LlmTensor directory, 32 B per entry][4-byte-aligned payloads ...]
```

**`LlmHeader`** — magic `PHXL`, major/minor version, then the complete config the runtime needs:
`dim`, `hidden_dim`, `n_layers`, `n_heads`, `n_kv_heads`, `vocab_size`, `seq_len`, `group_size`,
`head_size`, `kv_dim`, `kv_shift`, flags (bit 0 = the classifier shares the embedding table),
`rms_eps_q16`, `tensor_count`, `dir_offset`, `blob_size`, `crc32`, and the tokenizer's
`max_token_len` / `bos_id` / `eos_id`. Because every shape lives here and is validated at load,
**a different checkpoint drops in with no code change** — the runtime derives its context window
and arena size from the header.

**`LlmTensor`** — `{name (fnv1a-32, diagnostics only), kind, dtype, layer, rows, cols, stride,
data_off, scale_off, nbytes}`. `cols` is the logical input width, `stride` the padded one.
`layer` is `0xFFFF` for non-layered tensors. `scale_off` points at `rows × (stride/group_size)`
int32 Q30 scales and must be 0 for anything that is not `kDTypeQ8`.

**Tensor kinds**: `tok_emb`, per-layer `rms_att`/`rms_ffn`, `wq`/`wk`/`wv`/`wo`,
`w1`/`w2`/`w3`, `rms_final`, optional `wcls`, `rope_cos`/`rope_sin` (Q15, precomputed so the
device never calls sin/cos), `exp_lut`/`silu_lut` (513 Q16.16 entries each, power-of-two stepped
so the device's index/fraction split is a shift), and the tokenizer as four tensors:
`tok_bytes` (concatenated pieces), `tok_index` (`vocab+1` offsets), `tok_sorted` (ids ordered by
piece bytes, for binary search) and `tok_score` (Q16.16 merge scores).

Everything is little-endian and 4-byte aligned — which is also ARM7TDMI's requirement, where a
misaligned 32-bit load silently rotates instead of faulting.

## The bit-exact reference

This file also contains a **Python mirror of the C++ inference core** (`FixedModel`,
`FixedSampler`, and the `q_*` primitives), matching it operation for operation: same shifts, same
saturation, same truncation direction. That is what makes the golden token file worth asserting —
two independent implementations of the same integer arithmetic agreeing bit-for-bit is the only
practical way to catch a fixed-point bug before it reaches a ROM.

A `FloatModel` (llama2.c's `run.c` forward pass, in float32) sits alongside it as the measuring
stick; the golden records both token streams and how far they agree.
