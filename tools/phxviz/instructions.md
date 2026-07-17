# phxviz — visualization-track converter

Host-only CLI (docs/08 §5, sibling of `phxsnd`). Analyses a song **offline** and emits a compact
per-video-frame "visualization track" that the GBA reads **zero-copy** at runtime — so no FFT
ever runs on the ARM7. Built for `examples/miracle-player/` (the "A Small Miracle" music ROM).

## Usage

```
phxviz --out FILE.phxviz [--name N] [--rate HZ] <song.wav>
```

- `--out`   output intermediate bundle (a one-asset `.phxviz`, merged by `phxpack`). Required.
- `--name`  asset name inside the bundle (default: the `--out` stem).
- `--rate`  target audio **device rate** the track aligns to, in Hz. Default `18157` (the GBA
            vblank-locked rate). Use the host mixer rate for a PC bundle.
- input     a PCM `.wav` (mono or stereo; stereo is downmixed). **MP3 is not read here** — decode
            it to WAV on the host first (e.g. `ffmpeg -i song.mp3 -ac 1 -ar 44100 song.wav`); no
            MP3 decoder ships in-tree.

Then merge it into the game bundle like any other intermediate:

```
phxpack --out assets.phxp <other intermediates...> song.phxviz
```

## What it produces

A `VizHeader` followed by one fixed-size `VizFrame` record per video frame (see
`examples/miracle-player/src/viz.h`, the shared engine-free POD format). Per frame:

| field        | meaning                                                         |
|--------------|----------------------------------------------------------------|
| `bands[16]`  | log-spaced FFT magnitude spectrum (the spectrogram / bars)      |
| `rms`        | overall loudness envelope                                       |
| `onset`      | spectral-flux onset strength (0 = none)                         |
| `low/mid/high`| grouped band energies (particle / shake routing)              |
| `beat`       | 1 on a peak-picked beat frame                                   |

All fields are `uint8` [0,255], globally normalised (sqrt-compressed) so the bars use the full
range. The record array is 4-byte aligned for zero-copy pointer-cast on device.

## A/V lock (the key invariant)

The runtime never counts video frames — it derives the record index straight from the audio
sample cursor by pure integer division:

```
index = samples_consumed / header.hop_samples        // hop_samples = round(rate / 60)
```

so audio and visuals can never drift, and the mapping is byte-identical on both scalar tiers.
`viz_validate()` bounds-checks the blob before the ROM trusts it.

## Format / analysis knobs

Bands, FFT size, frame rate, and normalisation live in `tools/phxviz/analyze.h`
(`kVizBands`, `kFftSize`, `kFrameHz`). Bump `kVizVersion` in `viz.h` on any layout change — the
`static_assert`s and `viz_validate()` reject a mismatched blob rather than misread it.
