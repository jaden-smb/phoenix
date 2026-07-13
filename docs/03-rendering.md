# Phoenix Engine — Rendering

> `engine/render/` — one public API, four backends as implemented today: GBA PPU, PSP GU,
> PC GL, and a software rasterizer (the golden reference every other backend is diffed
> against; see `docs/graphics-engine.md` §5 for the as-built version of this document —
> in particular, §6 below describes a GL 3.3/Vulkan design that was never built; the
> implemented PC backend is GL 1.1 immediate mode, no Vulkan).
> The hard part: the GBA has **no framebuffer-blit model** in its fast path — it has a
> tile/sprite *PPU* with 128 hardware sprites and 4 background layers. The API is
> therefore shaped around **sprites and tilemaps**, which map *up* to GL/GU trivially
> but cannot be faked *down* onto the PPU. We design to the constraint, not around it.

## 1. The unified API (`phx/render/renderer.h`)

The renderer is **retained for tilemaps, immediate for sprites** — the combination
that every 2D console actually used.

```cpp
namespace phx {

struct DrawSprite {            // POD, batched per frame
    TextureId tex;
    int16_t   sx, sy, sw, sh;  // source rect in atlas (texels)
    vec2      pos;             // world position (top-left)
    uint16_t  flags;           // flip-x/y, priority/layer, blend
    uint8_t   palette;         // GBA/paletted; ignored on RGBA backends
    uint8_t   z;               // intra-layer sort key
};

struct Camera2D { vec2 pos; scalar zoom; int16_t shake; };

class Renderer {
public:
    static Result<Renderer*> create(phx_gfx*, ArenaAllocator&, const phx_caps&);

    void begin_frame(const Camera2D&);   // camera zoom + deterministic shake applied here
    // tilemaps: retained handle, uploaded once, scrolled cheaply
    TilemapId upload_tilemap(const TilemapDesc&);
    void      set_tilemap_scroll(TilemapId, vec2 px);
    // per-layer camera factor (1 = world, ½ = half-speed background, 0 = fixed sky);
    // implemented in the front end over the scroll seam with Q16 integer math, so every
    // backend inherits it tier-exactly — on GBA it is the free per-BG HOFS/VOFS trick (§4)
    void      set_tilemap_parallax(TilemapId, uint8_t layer, scalar fx, scalar fy);
    void      draw_tilemap(TilemapId, uint8_t layer);
    // sprites: immediate, batched & sorted by (layer, z, tex)
    void      draw_sprite(const DrawSprite&);
    // text & ui quads go through the same sprite batcher (font = atlas)
    void      end_frame();     // flush → backend submit → present

    TextureId load_texture(const TextureDesc&);   // from resource cache blob
    void      unload_texture(TextureId);          // slot recycling (drives the LRU cache)
    const RenderStats& stats() const;
};
} // namespace phx
```

Gameplay calls `draw_sprite`/`draw_tilemap`. It never sees GL/GU/PPU. The backend
decides how those become reality.

## 2. Backend mapping table

The PC column below is the *original design target* (§6 goes into more detail on why it
wasn't built that way). What's actually implemented is a GL 1.1 immediate-mode port of the
software rasterizer's geometry — `glBegin(GL_QUADS)` per sprite/tile, no instancing, no
shaders, no VAOs. It is pixel-verified against the software golden (`make gl-verify`) and
that's the bar that matters; revisit the instanced/shader design only if PC/Windows draw
call count becomes a real bottleneck (it hasn't at the scene sizes this engine targets).

| API concept       | GBA PPU backend                 | PSP GU backend            | PC GL backend (design target — see note above) |
|-------------------|----------------------------------|---------------------------|--------------------------|
| `Tilemap` layer   | hardware BG layer (REG_BGxCNT), map+tiles in VRAM | textured grid mesh, GE display list | instanced quads / tile shader |
| `set_scroll`      | `REG_BGxHOFS/VOFS` write (free!) | uniform/matrix offset     | uniform offset           |
| `draw_sprite`     | OAM entry (≤128) + VRAM tile     | `sceGuDrawArray` triangle pair | batched instanced quad |
| `Texture`         | RGBA8 in ROM; quantized to 4bpp paletted tiles in VRAM at *upload*, not bake time — see §5 | RGBA8 (no swizzle today — see §5) | RGBA8 |
| sort & batch      | priority bits + OAM order        | sort then one display list | sort, instance buffer    |
| blend             | PPU blend regs (limited modes)   | GE blend                  | full blend pipeline      |

As implemented today, the PC backend's actual row would read: `Tilemap`/`draw_sprite` →
one `glBegin(GL_QUADS)`/`glVertex2f` pair per draw; `Texture` → RGBA8 via `glTexImage2D`,
no sampler-side palette; sort & batch → `std::sort` then one draw call per sprite (no
instance buffer yet); blend → fixed-function `glBlendFunc`.

The **sprite ceiling is the API's honest limit**: `phx_caps::max_sprites` is 128 on
GBA. `draw_sprite` past the ceiling on GBA drops the lowest-priority sprite and
increments `RenderStats::dropped`. On PC the ceiling is effectively unbounded. The
*game* reads caps to budget; the *renderer* enforces gracefully.

## 3. The frame pipeline (backend-agnostic front end)

```
 begin_frame(cam)
   └─ stash camera, reset sprite batch on frame stack (StackAllocator)
 draw_tilemap(id, layer)         ──┐
 draw_sprite(...) × N             ─┤   recorded into a transient command list
 draw_sprite(...) (text/UI)       ──┘   (no GPU work yet)
 end_frame()
   ├─ sort sprites: key = (layer<<24 | z<<16 | tex)   radix on console, std::sort PC
   ├─ backend->submit(cmdlist, cam)   ← the ONLY virtual-ish call per frame
   └─ platform->present()             ← vblank/swap
```

The command list lives on the **double-buffered frame stack** (see memory doc), so
the PSP GE can chew on frame N-1's display list while we build frame N — no copy, no
malloc.

### Backend interface (internal, not public)

```cpp
struct IRenderBackend {                       // resolved at create(), not per-call
    void (*submit)(void* self, const FrameCmds&, const Camera2D&);
    TextureId (*upload_tex)(void* self, const TextureDesc&);
    TilemapId (*upload_map)(void* self, const TilemapDesc&);
    void (*destroy)(void* self);
    void* self;
};
```

One indirect call per frame, not per sprite. On GBA `submit` is the OAM/VRAM/DMA
update; on PSP it builds one GE display list; on PC it fills instance buffers and
issues a handful of draw calls.

## 4. GBA backend specifics (`src/gba/`)

The PPU is the engine here. Notes that shape the design:

- **Video modes:** Mode 0 (4 text BGs, tiled) is the workhorse for 2D; Mode 4
  (bitmap, paletted) available for effects/intros. Phoenix defaults to Mode 0.
- **Tiles:** 8×8, 4bpp (16 colors/palette) → 32 bytes/tile. Tilemaps reference tile
  indices; `upload_tilemap` writes the screen-base map and char-base tiles to VRAM.
- **Scroll is free:** `set_tilemap_scroll` is two register writes — parallax via 4 BG
  layers at different scroll rates costs nothing. This is the GBA's superpower and the
  API exposes it directly.
- **Sprites (OBJ):** 128 OAM entries; `draw_sprite` allocates an OAM slot, points it
  at VRAM OBJ tiles, sets attributes (pos/flip/priority/palette). VRAM OBJ tile budget
  (≈32 KB) is the real ceiling, tracked by the backend.
- **DMA** copies tiles/palette during VBlank to avoid tearing.

> 2.5D on GBA: Mode 7-style affine BG (one affine background, `REG_BG2PA..PD`) gives
> the "pseudo-3D floor" look (F-Zero/racers). `draw_tilemap` with an affine layer +
> per-scanline matrix (HBlank IRQ) is a supported, opt-in path, not the default.

## 5. PSP backend specifics (`src/gu/`)

- `sceGuInit`, double-buffered VRAM, depth/dither off for pure 2D (2.5D enables them).
- Textures are RGBA8, same as every other target — **not swizzled**. Swizzling at pack
  time for GE cache efficiency was the original design (`phxpack --target psp`) but isn't
  implemented; `engine/render/src/gu/gu_backend.cpp` rejects any format that isn't RGBA8
  rather than unswizzling one. Revisit if GE texture-fetch cost actually shows up in a
  profile — it hasn't needed to yet at this engine's texture sizes.
- One display list per frame built into the frame stack; `sceGuFinish`/`sceGuSync`.
- 2.5D: the same sprite quads but with real Z and a perspective/ortho `sceGumMatrix`,
  enabling billboards, layered parallax with depth, and simple textured 3D props.

## 6. PC backend specifics (`src/gl/` as implemented; `src/vk/` was the design, never built)

**As implemented:** `engine/render/src/gl/gl_backend.cpp` is a deliberate **GL 1.1
immediate-mode** port of the software rasterizer's geometry — `glBegin(GL_QUADS)` /
`glTexCoord2f` / `glVertex2f` per sprite or tile, nearest filtering, fixed-function alpha
blend. No GL loader (glad/glew) — it links straight against libGL, which is enough for a
1.1 context on every desktop GL driver, current or ancient. Chosen for maximum
compatibility over throughput (fits the "low-resource" ethos in spirit, if not in the GPU
API generation), since this engine's 2D scene sizes never came close to making draw-call
count the bottleneck. Pixel-verified against the software golden on a real GPU
(`make gl-verify`).

**The original design (below), not built — revisit only if PC/Windows batching becomes a
real bottleneck (docs/09 v0.4 tracks it):**

- **GL 3.3 core** baseline (maximum portability across old Linux/Win GPUs). One
  static-geometry tile shader + an instanced sprite shader.
- Sprites drawn via **instanced quads**: one VBO of unit quad, per-instance buffer of
  `{pos, src-rect, flags}`; a single `glDrawArraysInstanced` per (layer,texture) batch.
- **Vulkan optional** (`PHX_ENABLE_VULKAN`): same front end, a backend that pre-records
  command buffers. No `src/vk/` folder or Vulkan symbol exists in the repo today.

A **software fallback** (`src/soft/`) exists for headless CI / extreme low-end and
doubles as the reference image for backend conformance tests — this one IS built, and is
the golden reference every backend (including the GL 1.1 one above) is diffed against.

## 7. Text & UI share the sprite path

There is no separate text renderer. A bitmap font is a texture atlas; glyphs are
sprites; the UI module emits `DrawSprite`s. This keeps the backend count of code
paths minimal — crucial for four platforms — and means GBA text is just OBJ/BG tiles,
which is how GBA games actually do text.

## 8. Conformance & golden images

The render suite (`tests/render_test.cpp` + the ppu/gu suites; `make render ppu gu`,
and `make gl-verify` on a real GPU) renders a fixed scene (tilemap + sprites + text) through
the **software backend** and diffs against a checked-in golden PNG. GL/GU backends are
spot-checked on hardware/emulator (mGBA, PPSSPP) in CI with perceptual tolerance. This
catches "the GBA flips Y but GL doesn't" classes of bugs that otherwise ship.

## 9. Why not a generic command-buffer/RHI like bgfx?

A full RHI assumes a programmable pipeline. The GBA has none. Forcing the PPU into an
RHI abstraction would either (a) cripple the PC backend to PPU semantics, or (b) make
the GBA backend a lie. Phoenix instead abstracts at the **2D-intent layer** (sprites,
tilemaps, parallax, palettes) — the highest level that is *honestly* common to all
four machines. That is the single most important rendering decision in the engine.
