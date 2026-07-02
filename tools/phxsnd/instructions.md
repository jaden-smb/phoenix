# phxsnd — the audio converter

## What it is for

Bakes a **WAV file** into a `.phxsnd` intermediate holding one Sound asset — mono 16-bit PCM
the runtime wraps as a `SoundView` and plays through the software mixer. `phxpack` merges the
`.phxsnd` into the final bundle.

## How it works

Decodes RIFF/PCM WAV (8- or 16-bit, mono or stereo) with the pipeline's own decoder; stereo is
downmixed to mono, 8-bit is converted to signed 16. Then the **per-target encode** step runs:

- `--target 0` (GBA): the PCM is **resampled down to 16384 Hz at bake time** — the GBA Direct
  Sound device rate. The cartridge carries ~2.7× less sample data (for 44.1 kHz sources) and
  the 16 MHz CPU mixes 1:1 instead of resampling every voice. Q16 linear interpolation,
  all-integer, deterministic.
- `--target 1|2` (PSP/PC, default 2): the source rate is kept — those mixers run at 44.1 kHz
  and resample cheaply at runtime.

## Build & run

```bash
make check              # builds build/phxsnd along the way (or: make tools)

./build/phxsnd --out FILE.phxsnd [--name N] [--target 0|1|2] <sound.wav>
```

The asset name defaults to the WAV's stem; the game looks it up by that name
(`res->sound("jump"_hash)`) and plays it via the mixer / command queue.

## Example

```bash
./build/phxsnd  --out jump.phxsnd --name jump --target 0 jump.wav   # GBA-rate encode
./build/phxpack --out assets.phxp jump.phxsnd
```

Try it on the fixture `make check` drops: `./build/phxsnd --out /tmp/t.phxsnd build/p_tone.wav`.
Verified by `make audio` (decode → bake → mount → mix) and `make pipeline` (incl. the tier-0
resample: rate 16384, frame count scaled, samples intact).
