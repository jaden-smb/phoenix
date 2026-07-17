// examples/miracle-player/src/miracle.cpp — the visualizer game logic (engine-only). One App
// loop: stream the resident song through the mixer, derive the viz record from the audio sample
// cursor, and render bars + transport UI + the intro/outro dedication cards. No platform headers.
#include "player.h"

#include <cstdio>
#include <cstring>

namespace miracle {

namespace {
constexpr int      kScreenW    = 240;        // GBA-native resolution (host renders the same)
constexpr int      kScreenH    = 160;
constexpr uint32_t kRingCap    = 1u << 14;   // 16384 samples (power of two; SPSC ring)
constexpr uint32_t kIntroFrames = 300;       // ~5 s dedication card before playback (or START)
constexpr int32_t  kGravityQ16 = 1 << 14;    // ~0.25 px/frame^2 pulling particles down (Q16)
constexpr int32_t  kQ16One     = 1 << 16;

TextureId load_tex(ResourceCache* res, Renderer& r, NameHash h) {
    auto tr = res->texture(h);
    if (!tr) return kNoTexture;
    TextureView v = tr.unwrap();
    TextureDesc d{}; d.pixels = v.pixels; d.width = v.width; d.height = v.height; d.format = v.format;
    return r.load_texture(d);
}

inline vec2 v2(int x, int y) { return vec2{ s_from_int(x), s_from_int(y) }; }
inline vec2 v2sz(int w, int h) { return vec2{ s_from_int(w), s_from_int(h) }; }
} // namespace

void MiracleGame::on_start(App& app) {
    ArenaAllocator& A = app.mem().persistent();
    Renderer& r = app.render();

    res = ResourceCache::create(A).unwrap();
    // Skip the mount-time CRC32: it runs over the WHOLE bundle, and this one is ~10 MB (the song)
    // — on the 16 MHz GBA that CRC is several seconds of black screen before the first frame. The
    // bundle is self-baked and ROM-resident (immutable; can't corrupt), and mount still does all
    // the structural checks (magic/version/size/TOC bounds) regardless.
    if (res->mount(app.platform(), bundle, /*verify_checksum=*/false) != Status::Ok)
        PHX_LOG_ERROR("miracle: failed to mount '%s'", bundle);

    font.tex = load_tex(res, r, "font"_hash);
    font.glyph_w = 8; font.glyph_h = 8; font.cols = 16; font.first_char = 32;
    font.advance = 8; font.line_h = 8;
    heart_tex = load_tex(res, r, "heart"_hash);
    spark_tex = load_tex(res, r, "spark"_hash);

    // Big buffers live in the engine arena (EWRAM), NOT inline in the game object — the game
    // object stays small enough for the GBA's tiny IWRAM stack (which the audio IRQ preempts).
    particles = A.make<ParticlePool<kMaxParticles>>();
    spec_idx  = static_cast<uint16_t*>(A.alloc(kSpecCells * sizeof(uint16_t)));

    // The spectrogram BG map: uploaded once over our own cell buffer, repainted every frame.
    for (int i = 0; i < kSpecCells; ++i) spec_idx[i] = 0;
    if (TextureId ts = load_tex(res, r, "spectrum"_hash); ts != kNoTexture) {
        TilemapDesc md{}; md.indices = spec_idx;
        md.width = kSpecCols; md.height = kSpecRows; md.layers = 1;
        md.tile_w = 8; md.tile_h = 8; md.tileset = ts;
        spectrum_map = r.upload_tilemap(md);
    }

    if (auto sr = res->sound("song"_hash); sr.ok()) {
        SoundDataView v = sr.unwrap();
        song = SoundView{ v.samples, v.frames, v.rate };
        total_samples = v.frames;
    } else {
        PHX_LOG_ERROR("miracle: no 'song' asset in bundle");
    }
    if (auto br = res->blob("viz"_hash); br.ok()) {
        BlobView b = br.unwrap();
        viz = viz_validate(b.data, b.size);
        if (!viz) PHX_LOG_ERROR("miracle: 'viz' blob failed validation");
    } else {
        PHX_LOG_ERROR("miracle: no 'viz' asset in bundle");
    }

    mixer = AudioMixer::create(A, caps(), mixer_rate).unwrap();
    ring  = static_cast<int16_t*>(A.alloc(kRingCap * sizeof(int16_t)));
    stream.init(ring, kRingCap, mixer_rate);
    if (song.samples) {
        stream.open(song, false);          // cursor 0, producing; drained only once draining==1
        mixer->play_music_stream(&stream); // attach before any audio device starts (single-threaded)
    }
    particles->clear();

    phase = Phase::Intro; phase_frames = 0;
    samples_played.store(0, std::memory_order_relaxed);
    draining.store(0, std::memory_order_relaxed);
}

void MiracleGame::begin_playback() {
    phase = Phase::Playing; phase_frames = 0;
    draining.store(1, std::memory_order_release);   // unmute: the audio callback now mixes + advances
}

void MiracleGame::apply_seek() {
    if (song.frames == 0) { seek_state.store(0, std::memory_order_release); return; }
    const uint32_t t   = seek_target;
    uint32_t       off = t < song.frames ? t : song.frames - 1;
    // Re-open the stream on a zero-copy sub-view of the resident song at the seek point; drains
    // and refills the ring from there. Always non-looping — the game re-seeks to 0 to loop.
    stream.open(SoundView{ song.samples + off, uint32_t(song.frames - off), song.rate }, false);
    samples_played.store(t, std::memory_order_relaxed);
    seek_state.store(0, std::memory_order_release);
}

void MiracleGame::seek_by_seconds(int secs) {
    if (!total_samples) return;
    const int64_t delta = int64_t(secs) * int64_t(mixer_rate);
    int64_t t = int64_t(position_samples()) + delta;
    if (t < 0) t = 0;
    if (uint64_t(t) >= total_samples) t = int64_t(total_samples) - 1;
    seek_target = uint32_t(t);
    if (device_audio) seek_state.store(1, std::memory_order_release);  // callback performs it
    else              apply_seek();                                    // headless: do it now
}

void MiracleGame::on_fixed_update(App& app, scalar /*dt*/) {
    const InputState& in = app.input();
    ++phase_frames;

    // Keep the ring ahead of the read cursor (the "GBA streaming producer": pump off the audio
    // path, every frame) — but NOT while a seek is pending: the audio-owning side is about to
    // re-open the stream, so the producer must stay quiescent (lock-free seek handshake).
    if (seek_state.load(std::memory_order_acquire) == 0)
        stream.pump();

    switch (phase) {
        case Phase::Intro:
            if (in.just(Button::Start) || in.just(Button::A) || phase_frames >= kIntroFrames)
                begin_playback();
            break;

        case Phase::Playing: {
            if (in.just(Button::Start)) { phase = Phase::Paused; draining.store(0, std::memory_order_release); break; }
            if (in.just(Button::A))      style = uint8_t((style + 1) % kNumStyles);
            if (in.just(Button::B))      chrome = !chrome;
            if (in.just(Button::Select)) show_profiler = !show_profiler;
            if (in.just(Button::Left))   seek_by_seconds(-5);
            if (in.just(Button::Right))  seek_by_seconds(+5);

            if (const VizFrame* f = current_frame()) update_reactive(*f);

            if (total_samples && position_samples() >= total_samples) {
                if (loop) { seek_target = 0; if (device_audio) seek_state.store(1, std::memory_order_release); else apply_seek(); }
                else      { draining.store(0, std::memory_order_release); phase = Phase::Outro; phase_frames = 0; }
            }
            break;
        }

        case Phase::Paused:
            if (in.just(Button::Start)) { phase = Phase::Playing; draining.store(1, std::memory_order_release); }
            if (in.just(Button::Left))  seek_by_seconds(-5);
            if (in.just(Button::Right)) seek_by_seconds(+5);
            break;

        case Phase::Outro:
            particles->update(kGravityQ16);   // gentle idle drift of any lingering particles
            break;
    }
}

void MiracleGame::text_centered(int y, const char* s, Rgba color) {
    const int w = int(std::strlen(s)) * font.advance;
    ui.text(v2((kScreenW - w) / 2, y), font, s, color);
}

void MiracleGame::draw_intro() {
    text_centered(52, "FOR LIZ...", rgba(255, 220, 120));
    text_centered(74, "THIS IS MY ATTEMPT TO BRING", rgba(230, 230, 240));
    text_centered(86, "ONE OF YOUR FAVOURITE SONGS", rgba(230, 230, 240));
    text_centered(98, "TO THE GAME BOY ADVANCE.", rgba(230, 230, 240));
    if ((phase_frames / 30) & 1)
        text_centered(130, "PRESS START", rgba(150, 150, 170));
}

void MiracleGame::update_reactive(const VizFrame& f) {
    // Beat/onset -> spawn a burst from the base of the spectrum; count + speed scale with the
    // low/mid/high energies. All integer/Q16 so the sim is identical on both scalar tiers.
    if (f.beat || f.onset > 160) {
        const int burst = 3 + (int(f.low) + int(f.mid) + int(f.high)) / 96;   // ~3..10
        for (int i = 0; i < burst; ++i) {
            const int32_t px = int32_t(rng.next() % uint32_t(kScreenW)) * kQ16One;
            const int32_t py = int32_t(kScreenH - 12) * kQ16One;
            const int32_t vx = rng.range(kQ16One + (int32_t(f.high) << 9));    // spread by treble
            const int32_t vmag = (int32_t(f.mid) << 9) + int32_t(rng.next() % uint32_t(kQ16One)) + kQ16One;
            const int32_t vy = -vmag;                                          // upward burst
            const uint16_t life = uint16_t(24 + (rng.next() % 40u));
            const uint8_t  hue  = uint8_t(rng.next());
            const Rgba col = rgba(uint8_t(150 + (f.high >> 1)), uint8_t(80 + (hue >> 2)),
                                  uint8_t(200 - (f.low >> 2)));
            const uint8_t variant = uint8_t(rng.next() & 3);   // which baked spark colour (GBA)
            particles->spawn(px, py, vx, vy, life, col, variant);
        }
    }
    particles->update(kGravityQ16);

    // Beat -> a short screen-shake kick (integer, PPU-honoured) and a subtle zoom pulse
    // (Q16; PPU renders 1:1 and ignores it). Both decay each frame.
    if (f.beat) {
        const int16_t kick = int16_t(2 + (f.onset >> 6));
        if (kick > shake_q) shake_q = kick;
        const int32_t zp = kQ16One + (int32_t(f.onset) << 6);
        if (zp > zoom_q16) zoom_q16 = zp;
    }
    if (shake_q > 0) --shake_q;
    zoom_q16 -= (zoom_q16 - kQ16One) >> 3;     // ease back toward 1.0
}

void MiracleGame::draw_background(const VizFrame* f) {
    // A full-screen gradient-ish backdrop whose brightness tracks bass + loudness. Layer 0 so
    // the bars/particles/UI draw on top.
    const int lo = f ? f->low : 0, rms = f ? f->rms : 0;
    const uint8_t b = uint8_t(6 + (lo >> 3) + (rms >> 4));   // deep blue that pulses on bass
    ui.rect(UIRect{ v2(0, 0), v2sz(kScreenW, kScreenH) }, rgba(uint8_t(b / 3), uint8_t(b / 2), b), 0);
}

void MiracleGame::draw_particles(Renderer& r) {
    if (spark_tex == kNoTexture) return;
    const Particle* p = particles->data();
    for (uint32_t i = 0; i < particles->count(); ++i) {
        const int x = p[i].x >> 16, y = p[i].y >> 16;
        if (x < -8 || x > kScreenW || y < -8 || y > kScreenH) continue;
        DrawSprite d{};
        d.tex = spark_tex; d.sx = int16_t((p[i].size & 3) * 8); d.sy = 0; d.sw = 8; d.sh = 8;
        d.pos = v2(x, y); d.layer = 190; d.tint = p[i].color;   // tint honoured on soft, ignored on PPU
        r.draw_sprite(d);
    }
}

// Repaint the BG cell buffer: each of the 30 columns maps to a band; fill from the baseline up
// to the band's height with that band's colour tile (cell = b+1), rest empty (cell 0). Mirrored
// style grows from the centre. Pure integer -> identical on both scalar tiers.
void MiracleGame::rebuild_spectrogram(const VizFrame& f) {
    for (int i = 0; i < kSpecCells; ++i) spec_idx[i] = 0;
    const int rows = kSpecRows;                    // 20; bar area = the bottom kSpecBarRows
    for (int col = 0; col < kSpecCols; ++col) {
        const int b = col * int(kVizBands) / kSpecCols;
        const uint16_t cell = uint16_t(b + 1);
        int h = f.bands[b] * kSpecBarRows / 255;
        if (h < 1 && f.bands[b] > 0) h = 1;
        if (style == 1) {                          // mirrored around the vertical centre
            const int mid = rows / 2, hh = h / 2;
            for (int y = mid - hh; y < mid + hh + (h & 1); ++y)
                if (y >= 0 && y < rows) spec_idx[y * kSpecCols + col] = cell;
        } else {                                   // grow up from the baseline
            for (int y = rows - h; y < rows; ++y)
                if (y >= 0) spec_idx[y * kSpecCols + col] = cell;
        }
    }
}

void MiracleGame::draw_visualizer(const VizFrame& f) {
    // The bars themselves are the BG tilemap (drawn in on_render). This draws only the transport
    // chrome on top — all 1x1-quad UI (title bar aside) simply drops on the PPU, leaving the
    // tilemap + sprites, which is the intended GBA look.
    if (!chrome) return;

    ui.text(v2(6, 6), font, kTitle, rgba(245, 245, 255));

    const uint32_t pos_s = uint32_t(position_samples() / (mixer_rate ? mixer_rate : 1));
    const uint32_t tot_s = uint32_t(total_samples    / (mixer_rate ? mixer_rate : 1));
    char t[32];
    std::snprintf(t, sizeof(t), "%u:%02u - %u:%02u", unsigned(pos_s / 60), unsigned(pos_s % 60),
                  unsigned(tot_s / 60), unsigned(tot_s % 60));   // '-' (font has no '/')
    ui.text(v2(6, 18), font, t, rgba(200, 210, 230));

    const scalar prog = total_samples ? s_from_q16(int32_t((position_samples() << 16) / total_samples))
                                      : scalar{};
    ui.bar(UIRect{ v2(6, 154), v2sz(228, 4) }, prog, rgba(120, 200, 255), rgba(40, 40, 60));

    const scalar rms = s_from_q16(int32_t((int32_t(f.rms) << 16) / 255));
    ui.bar(UIRect{ v2(170, 18), v2sz(64, 5) }, rms, rgba(255, 180, 90), rgba(40, 40, 60));
}

void MiracleGame::draw_outro(const VizFrame* f) {
    (void)f;
    // "MADE WITH <heart> FOR LIZ", centered, heart as a baked glyph tile.
    const char* a = "MADE WITH";
    const char* b = "FOR LIZ";
    // Lay the row out: [a] [heart 8px] [b] with single-space gaps.
    const int aw = int(std::strlen(a)) * font.advance;
    const int bw = int(std::strlen(b)) * font.advance;
    const int gap = font.advance;
    const int total = aw + gap + 8 + gap + bw;
    int x = (kScreenW - total) / 2;
    const int y = 76;
    ui.text(v2(x, y), font, a, rgba(245, 245, 255));
    x += aw + gap;
    if (heart_tex != kNoTexture)
        ui.image(UIRect{ v2(x, y), v2sz(8, 8) }, heart_tex, 0, 0, 8, 8);
    x += 8 + gap;
    ui.text(v2(x, y), font, b, rgba(245, 245, 255));
}

void MiracleGame::headless_mix() {
    // Stand in for the platform audio callback on the null platform: mix exactly one video
    // frame's worth of samples and advance the position (deterministic, tier-identical).
    if (!draining.load(std::memory_order_acquire) || !mixer) return;
    const uint32_t frames = mixer_rate / 60;
    static int16_t buf[4096];
    uint32_t done = 0;
    while (done < frames) {
        uint32_t chunk = frames - done; if (chunk > 2048) chunk = 2048;
        mixer->mix(buf, chunk);
        for (uint32_t i = 0; i < chunk * 2; ++i) {
            int32_t a = buf[i] < 0 ? -buf[i] : buf[i];
            if (a > audio_peak) audio_peak = a;
        }
        done += chunk;
    }
    advance_played(frames);
    ++play_frames;
}

void MiracleGame::on_render(App& app, scalar /*alpha*/) {
    Renderer& r = app.render();

    // Reactive camera: beat-synced shake (integer, honoured everywhere) + zoom pulse (Q16;
    // ignored by the PPU which renders 1:1). Only during playback.
    camera.shake = (phase == Phase::Playing) ? shake_q : 0;
    camera.zoom  = (phase == Phase::Playing) ? s_from_q16(zoom_q16) : s_from_int(1);
    r.begin_frame(camera);
    ui.begin(r, app.input());

    const VizFrame* f = current_frame();
    if (phase == Phase::Playing && f) {
        for (uint32_t b = 0; b < kVizBands; ++b) if (f->bands[b]) { ++viz_active_frames; break; }
    }
    switch (phase) {
        case Phase::Intro:
            draw_background(nullptr);
            draw_intro();
            break;
        case Phase::Playing:
        case Phase::Paused:
            draw_background(f);
            if (f && spectrum_map != kNoTilemap) {
                rebuild_spectrogram(*f);
                r.refresh_tilemap(spectrum_map);      // re-stream the mutated cells (PPU); no-op on soft
                r.draw_tilemap(spectrum_map, 0);
            }
            draw_particles(r);
            if (f) draw_visualizer(*f); else draw_intro();
            break;
        case Phase::Outro:
            draw_background(f);
            draw_particles(r);
            draw_outro(f);
            break;
    }
    if (phase == Phase::Paused)
        text_centered(78, "PAUSED", rgba(255, 255, 255));
    if (show_profiler)
        ui.profile_overlay(v2(kScreenW - 96, 2), app.profile(), &font);

    ui.end();
    r.end_frame();

    if (!device_audio) headless_mix();
}

void MiracleGame::on_stop(App&) {}

} // namespace miracle
