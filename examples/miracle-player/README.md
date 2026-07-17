# miracle-player — "A Small Miracle" music visualizer

A single-purpose app that plays **one baked-in song** and renders a music-reactive visualizer
around it: a spectrogram, a particle system, palette/camera reactivity, and a transport UI —
plus intro/outro dedication cards. It runs the **same gameplay code** on the host (software
renderer + real audio) and on a real **Game Boy Advance** (native PPU), using only engine
systems (no platform/OS headers, no `#ifdef PLATFORM`).

## The key idea: bake the DSP offline

The GBA (ARM7TDMI @ 16 MHz, no FPU) can neither decode MP3 nor run FFTs in real time, so nothing
signal-processing happens on device. At **bake time** the host tool `tools/phxviz` analyses the
song and emits a compact **visualization track** — one fixed-size `VizFrame` record per video
frame (16 log-spaced FFT bands + RMS + spectral-flux onset/beat), quantised to `uint8`. The ROM
reads that stream **zero-copy** and, each frame, maps the audio sample cursor to a record by pure
integer division:

```
viz_index = samples_consumed / hop_samples          // hop = round(rate / 60)
```

so audio and visuals can never drift, and the mapping is byte-identical on both scalar tiers.
See `src/viz.h` (the engine-free POD format) and `tools/phxviz/`.

## Audio

The whole track is baked to **resident PCM** (mono, tier-0-resampled to the GBA's 18157 Hz device
rate) and streamed through the mixer with `AudioStream` + `play_music_stream`, pumped off the
audio path each frame (the "streaming producer"). MP3 → WAV is a **host-only** ffmpeg step; no MP3
decoder ships in-tree. The ~9.5 MB song lives in cartridge ROM, read in place.

## Rendering (host vs GBA)

The GBA PPU can't scale or tint OBJ sprites, so:

- **Spectrogram** = a **BG tilemap** (Mode-0 tiles), repainted each frame from the band heights
  (`Renderer::refresh_tilemap` re-streams it on the PPU; the software backend reads it live).
- **Particles** = 8×8 OBJ sparks (integer Q16 pool from the arena; colour baked into the atlas).
- **Text** = OBJ glyphs. NOTE: the shared debug font is **UPPERCASE + digits + `:!.-` only**.

## Controls

| Button | Action                                             |
|--------|----------------------------------------------------|
| START  | play / pause (and dismiss the intro card)          |
| LEFT / RIGHT | seek ∓5 s (re-seeks the stream cursor + viz index together) |
| A      | cycle visualizer style (bars-up / mirrored)        |
| B      | toggle transport chrome                            |
| SELECT | toggle the frame-profiler overlay                  |

## Build & run

```bash
make miracle              # host SDL window (software renderer + real audio) — for visual dev
make gba-miracle-ppu      # devkitARM -> build/gba/phx-miracle-ppu.gba (native PPU ROM)
make size-gate-miracle    # GBA ROM/IWRAM/EWRAM budget gate (ROM budget raised for the song)
```

The host build bakes its bundle at startup; if `build/miracle.wav` (decoded from the repo-root
MP3 by the Makefile via ffmpeg) is absent, it falls back to a short synthetic tone so the target
still builds. The headless suite `tests/suites/miracle_test.cpp` (in `make check` /
`determinism`) proves the A/V-lock invariant, seeking, loop continuity, and non-silent streaming;
unit tests pin the particle-pool bound and the ring's no-underrun invariant.

## Notes / gotchas (see STATUS.md for detail)

- Mount the ROM bundle with `verify_checksum=false`: the default CRC32 runs over the whole ~10 MB
  bundle — ~27 s of black screen on the GBA. It's self-baked + immutable; structural checks still run.
- Keep the game object small on the GBA stack: the spectrogram cells + particle pool are
  arena-allocated (EWRAM), not inline, or the tiny IWRAM stack overflows under the audio IRQ.
