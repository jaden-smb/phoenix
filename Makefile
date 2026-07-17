# Phoenix Engine — host Makefile (foundation build + tests).
# The canonical cross-platform build is CMake (docs/07); this Makefile exists so the
# engine foundation can be built and tested on a host with just g++ + make (no cmake).
#
#   make test     build and run the foundation test suite
#   make clean    remove build artifacts
#
# Default tier is PC (float scalar). To exercise the GBA fixed-point path on the host:
#   make test TIER=gba_sim    (defines PHX_TARGET_GBA -> scalar = fixed16)
#
# Host builds default to DEBUG (PHX_ASSERT live, full Trace logging). To build+run any target
# against the release configuration instead (PHX_BUILD_RELEASE=1 — see phx/core/assert.h):
#   make check RELEASE=1      (or just `make release`, a dedicated gate — see below)
# GBA/PSP cross builds (GBA_FLAGS/PSP_FLAGS below) are ALWAYS release: those ship on real
# hardware, where a debug assert trap hangs the console instead of failing a CI job, and every
# debug-only log format string costs ROM bytes the GBA size gate is already tight on.

CXX      ?= g++
CXXFLAGS := -std=c++17 -O2 -g -Wall -Wextra -Wpedantic
INCLUDES := -Iengine/core/include \
            -Iengine/memory/include \
            -Iengine/ecs/include \
            -Iengine/input/include \
            -Iengine/audio/include \
            -Iengine/platform/include \
            -Iengine/render/include \
            -Iengine/render/src \
            -Iengine/render/src/gba \
            -Iengine/render/src/gu \
            -Iengine/resource/include \
            -Iengine/physics/include \
            -Iengine/anim/include \
            -Iengine/scene/include \
            -Iengine/ui/include \
            -Iengine/runtime/include \
            -Itools/phxpack \
            -Itools/phxtmap \
            -Itools/common \
            -Itools/phxviz \
            -Iexamples/platformer/src \
            -Iexamples/miracle-player/src \
            -Itests

TIER ?= pc
ifeq ($(TIER),gba_sim)
  CXXFLAGS += -DPHX_TARGET_GBA=1
endif
RELEASE ?= 0
ifeq ($(RELEASE),1)
  CXXFLAGS += -DPHX_BUILD_RELEASE=1 -DNDEBUG
endif
CXXFLAGS += $(EXTRA_CXXFLAGS)   # hook for wrapper targets (e.g. `sanitize` adds -fsanitize=…)

BUILD  := build

# Engine version — single-sourced from the version header (see RELEASING.md). Parsed, never
# hand-copied, so the Makefile, CMake, and the release workflow can't disagree with the code.
PHX_VERSION_H := engine/core/include/phx/core/version.h
PHX_VERSION   := $(shell sed -nE 's/^\#define PHX_VERSION_(MAJOR|MINOR|PATCH) +([0-9]+).*/\2/p' $(PHX_VERSION_H) | paste -sd. -)

# Host objects are compiled into a per-(tier,release) directory so the float (pc) / fixed-point
# (gba_sim) tiers, AND the debug/release configs, can never contaminate each other: without
# this, `make test TIER=gba_sim` or `make test RELEASE=1` followed by a plain `make check`
# would link stale objects (make tracks timestamps, not flags) into the wrong build. Binaries
# keep their documented build/ paths; the config stamp below relinks them exactly when
# TIER or RELEASE changes.
CFG       := $(TIER)-r$(RELEASE)
HOSTOBJ   := $(BUILD)/obj-$(CFG)
TIERSTAMP := $(BUILD)/.tier-$(CFG)
BIN    := $(BUILD)/phx_tests
SMOKE  := $(BUILD)/phx_smoke
RENDER := $(BUILD)/phx_render
PPU    := $(BUILD)/phx_ppu
GU     := $(BUILD)/phx_gu

TEST_SRC   := tests/unit/main.cpp \
              tests/unit/test_fixed.cpp \
              tests/unit/test_math.cpp \
              tests/unit/test_memory.cpp \
              tests/unit/test_ecs.cpp \
              tests/unit/test_time.cpp \
              tests/unit/test_input.cpp \
              tests/unit/test_physics.cpp \
              tests/unit/test_anim.cpp \
              tests/unit/test_scene.cpp \
              tests/unit/test_ui.cpp \
              tests/unit/test_audio.cpp \
              tests/unit/test_stream.cpp \
              tests/unit/test_lz.cpp \
              tests/unit/test_png.cpp \
              tests/unit/test_json.cpp \
              tests/unit/test_wav.cpp \
              tests/unit/test_cmdqueue.cpp \
              tests/unit/test_viz.cpp \
              tests/unit/test_particles.cpp \
              tests/unit/test_version.cpp

# The unit-test binary does not link the App/loop entry (no main collision).
UNIT_ENGINE := engine/core/src/assert.cpp \
               engine/core/src/fixed.cpp \
               engine/core/src/log.cpp \
               engine/memory/src/memory_root.cpp \
               engine/ecs/src/world.cpp \
               engine/physics/src/physics.cpp \
               engine/anim/src/anim.cpp \
               engine/scene/src/scene.cpp \
               engine/audio/src/mixer.cpp \
               engine/audio/src/stream.cpp

UNIT_OBJ  := $(patsubst %.cpp,$(HOSTOBJ)/%.o,$(UNIT_ENGINE) $(TEST_SRC))

# The App now owns a World + Renderer, so anything linking the loop links those too.
APP_SRC := engine/core/src/assert.cpp \
           engine/core/src/fixed.cpp \
           engine/core/src/log.cpp \
           engine/memory/src/memory_root.cpp \
           engine/ecs/src/world.cpp \
           engine/render/src/renderer.cpp \
           engine/render/src/soft/soft_renderer.cpp \
           engine/physics/src/physics.cpp \
           engine/anim/src/anim.cpp \
           engine/scene/src/scene.cpp \
           engine/ui/src/ui.cpp \
           engine/platform/src/null/null_platform.cpp \
           engine/runtime/src/app.cpp

# The smoke binary: the loop + its own main.
SMOKE_SRC := $(APP_SRC) tests/suites/smoke_app.cpp
SMOKE_OBJ := $(patsubst %.cpp,$(HOSTOBJ)/%.o,$(SMOKE_SRC))

# The playable binary: full stack (memory+ecs+input+render+loop) driven by scripted input.
PLAYABLE_SRC := $(APP_SRC) tests/suites/playable_test.cpp
PLAYABLE_OBJ := $(patsubst %.cpp,$(HOSTOBJ)/%.o,$(PLAYABLE_SRC))
PLAYABLE     := $(BUILD)/phx_playable

# The physics binary: full stack with PhysicsWorld driving a falling body onto a tile floor.
PHYSICS_SRC := $(APP_SRC) tests/suites/physics_test.cpp
PHYSICS_OBJ := $(patsubst %.cpp,$(HOSTOBJ)/%.o,$(PHYSICS_SRC))
PHYSICS     := $(BUILD)/phx_physics

# The anim binary: full stack with AnimationSystem driving a sprite-sheet frame on screen.
ANIM_SRC := $(APP_SRC) tests/suites/anim_test.cpp
ANIM_OBJ := $(patsubst %.cpp,$(HOSTOBJ)/%.o,$(ANIM_SRC))
ANIM     := $(BUILD)/phx_anim

# The scene binary: full stack with a SceneStack driving a gameplay scene + menu overlay.
SCENE_SRC := $(APP_SRC) tests/suites/scene_test.cpp
SCENE_OBJ := $(patsubst %.cpp,$(HOSTOBJ)/%.o,$(SCENE_SRC))
SCENE     := $(BUILD)/phx_scene

# The ui binary: full stack with the immediate-mode UI drawing text/bar/menu + focus nav.
UI_SRC := $(APP_SRC) tests/suites/ui_test.cpp
UI_OBJ := $(patsubst %.cpp,$(HOSTOBJ)/%.o,$(UI_SRC))
UI     := $(BUILD)/phx_ui

# The platformer binary (M1 capstone): the full example game + every engine system, driven
# headlessly by a scripted controller. Links all gameplay modules + the example sources.
# APP_SRC already bundles physics/anim/scene/ui; add the resource cache + the example.
PLATFORMER_SRC := $(APP_SRC) \
                  engine/resource/src/cache.cpp \
                  engine/audio/src/mixer.cpp \
                  examples/platformer/src/systems.cpp \
                  tests/suites/platformer_test.cpp
PLATFORMER_OBJ := $(patsubst %.cpp,$(HOSTOBJ)/%.o,$(PLATFORMER_SRC))
PLATFORMER     := $(BUILD)/phx_platformer

# The production example binary (real main; runs until quit). Built to prove the shipping
# entry point compiles + links; not run by `check` (it loops on the null backend).
PLATAPP_SRC := $(APP_SRC) \
               engine/resource/src/cache.cpp \
               engine/audio/src/mixer.cpp \
               examples/platformer/src/systems.cpp \
               examples/platformer/src/main.cpp
PLATAPP_OBJ := $(patsubst %.cpp,$(HOSTOBJ)/%.o,$(PLATAPP_SRC))
PLATAPP     := $(BUILD)/platformer

# The SDL build of the example: a REAL WINDOW you can play. Swaps the null backend for the
# SDL backend and defines PHX_HAVE_SDL. Requires SDL2 (`sdl2-config`); it is deliberately NOT
# part of `check`/`build` so headless CI never needs SDL2 or a display. Single-shot
# compile+link so it can't collide with the headless object tree.
PLATSDL_SRC := $(filter-out engine/platform/src/null/null_platform.cpp,$(PLATAPP_SRC)) \
               engine/platform/src/sdl/sdl_platform.cpp

# The GL build: same as the SDL build, but swaps the SOFTWARE render backend for the GL one
# (render tier 2) and the SDL platform creates a GL context instead of a streaming texture.
# Requires SDL2 + libGL. Also not part of `check`/`build`. The software backend stays the
# golden reference the GL output is validated against.
PLATGL_SRC := $(filter-out engine/platform/src/null/null_platform.cpp \
                           engine/render/src/soft/soft_renderer.cpp,$(PLATAPP_SRC)) \
              engine/platform/src/sdl/sdl_platform.cpp \
              engine/render/src/gl/gl_backend.cpp

# ---- miracle-player: the "A Small Miracle" GBA music visualizer ----------------------------
# The song is a real MP3 in the repo root; decode it to a mono WAV on the host (ffmpeg) — the
# bake reads that WAV (no MP3 decoder in-tree). If ffmpeg or the MP3 is missing, the bake falls
# back to a short synthetic tone so the target still builds. The WAV is a build artifact.
MIRACLE_WAV := $(BUILD)/miracle.wav
$(MIRACLE_WAV):
	@mkdir -p $(BUILD)
	@if command -v ffmpeg >/dev/null 2>&1 && [ -f "A Small Miracle.mp3" ]; then \
	   ffmpeg -v error -y -i "A Small Miracle.mp3" -ac 1 -ar 44100 $@ && echo "miracle: decoded song -> $@"; \
	 else echo "miracle: ffmpeg or 'A Small Miracle.mp3' missing — bake will use the synthetic tone"; fi

# Headless build (null platform) — proves the shipping entry compiles/links; loops on null so
# it is not part of `check`. Uses the headless audio stand-in.
MIRACLEAPP_SRC := $(APP_SRC) \
                  engine/resource/src/cache.cpp \
                  engine/audio/src/mixer.cpp \
                  engine/audio/src/stream.cpp \
                  examples/miracle-player/src/miracle.cpp \
                  examples/miracle-player/src/main.cpp
MIRACLEAPP_OBJ := $(patsubst %.cpp,$(HOSTOBJ)/%.o,$(MIRACLEAPP_SRC))
MIRACLEAPP     := $(BUILD)/miracle_headless

# The SDL build: the REAL WINDOW (`make miracle`). Swaps the null backend for SDL + real audio.
MIRACLESDL_SRC := $(filter-out engine/platform/src/null/null_platform.cpp \
                               examples/miracle-player/src/main.cpp,$(MIRACLEAPP_SRC)) \
                 engine/platform/src/sdl/sdl_platform.cpp \
                 examples/miracle-player/src/desktop_main.cpp

# The miracle suite binary (headless, deterministic): the visualizer under the real loop, with a
# synthetic tone. Part of `check` and `determinism` (both scalar tiers must match byte-for-byte).
MIRACLE_TEST_SRC := $(APP_SRC) \
                    engine/resource/src/cache.cpp \
                    engine/audio/src/mixer.cpp \
                    engine/audio/src/stream.cpp \
                    examples/miracle-player/src/miracle.cpp \
                    tests/suites/miracle_test.cpp
MIRACLE_TEST_OBJ := $(patsubst %.cpp,$(HOSTOBJ)/%.o,$(MIRACLE_TEST_SRC))
MIRACLE_TEST     := $(BUILD)/phx_miracle

# The emberwing binary: the SECOND full-game capstone (examples/emberwing — the Cinder
# Hollow vertical slice), driven headlessly by a scripted controller. Exercises everything
# the platformer does PLUS parallax layers, the audio command queue, three enemy behaviours,
# checkpoints and the goal/clear flow. Its test includes the example's headers by relative
# path (both examples define a `bake.h`, so the include-path order must not pick one).
EMBERWING_SRC := $(APP_SRC) \
                 engine/resource/src/cache.cpp \
                 engine/audio/src/mixer.cpp \
                 examples/emberwing/src/systems.cpp \
                 examples/emberwing/src/scenes.cpp \
                 tests/suites/emberwing_test.cpp
EMBERWING_OBJ := $(patsubst %.cpp,$(HOSTOBJ)/%.o,$(EMBERWING_SRC))
EMBERWING     := $(BUILD)/phx_emberwing

# The same full game over the GBA-native PPU backend (quantize -> 4bpp palette banks, four
# streamed text BGs, OAM) — the headless twin of the `gba-emberwing-ppu` ROM. Proves the
# whole vertical slice composes on the GBA hardware model, not just the soft reference.
EMBERWING_PPU_SRC := $(patsubst engine/render/src/soft/soft_renderer.cpp,engine/render/src/gba/gba_ppu.cpp,$(EMBERWING_SRC))
EMBERWING_PPU_OBJ := $(patsubst %.cpp,$(HOSTOBJ)/%.o,$(EMBERWING_PPU_SRC))
EMBERWING_PPU     := $(BUILD)/phx_emberwing_ppu

# The production emberwing binary (real main; loops on the null backend — link proof only).
EWAPP_SRC := $(APP_SRC) \
             engine/resource/src/cache.cpp \
             engine/audio/src/mixer.cpp \
             examples/emberwing/src/systems.cpp \
             examples/emberwing/src/scenes.cpp \
             examples/emberwing/src/main.cpp
EWAPP_OBJ := $(patsubst %.cpp,$(HOSTOBJ)/%.o,$(EWAPP_SRC))
EWAPP     := $(BUILD)/emberwing_app

# Windowed emberwing: SDL platform + the desktop entry that opens the REAL audio device
# (the SDL audio thread drains the game's command queue — docs/10 §2 discipline).
EWSDL_SRC := $(filter-out engine/platform/src/null/null_platform.cpp \
                          examples/emberwing/src/main.cpp,$(EWAPP_SRC)) \
             engine/platform/src/sdl/sdl_platform.cpp \
             examples/emberwing/src/desktop_main.cpp
EWGL_SRC  := $(filter-out engine/render/src/soft/soft_renderer.cpp,$(EWSDL_SRC)) \
             engine/render/src/gl/gl_backend.cpp

# The resource binary: bake a bundle -> mount through the seam -> render from it.
RESOURCE_SRC := engine/core/src/assert.cpp \
                engine/core/src/fixed.cpp \
                engine/core/src/log.cpp \
                engine/memory/src/memory_root.cpp \
                engine/platform/src/null/null_platform.cpp \
                engine/render/src/renderer.cpp \
                engine/render/src/soft/soft_renderer.cpp \
                engine/resource/src/cache.cpp \
                tests/suites/resource_test.cpp
RESOURCE_OBJ := $(patsubst %.cpp,$(HOSTOBJ)/%.o,$(RESOURCE_SRC))
RESOURCE     := $(BUILD)/phx_resource

# The texcache binary: drive a real Renderer through the budget-bounded LRU TextureCache.
TEXCACHE_SRC := engine/core/src/assert.cpp \
                engine/core/src/fixed.cpp \
                engine/core/src/log.cpp \
                engine/memory/src/memory_root.cpp \
                engine/platform/src/null/null_platform.cpp \
                engine/render/src/renderer.cpp \
                engine/render/src/soft/soft_renderer.cpp \
                engine/render/src/texture_cache.cpp \
                tests/suites/texcache_test.cpp
TEXCACHE_OBJ := $(patsubst %.cpp,$(HOSTOBJ)/%.o,$(TEXCACHE_SRC))
TEXCACHE     := $(BUILD)/phx_texcache

# The png binary: decode a real PNG (pipeline decoder) -> bake -> mount -> render from it.
PNG_SRC := engine/core/src/assert.cpp \
           engine/core/src/fixed.cpp \
           engine/core/src/log.cpp \
           engine/memory/src/memory_root.cpp \
           engine/platform/src/null/null_platform.cpp \
           engine/render/src/renderer.cpp \
           engine/render/src/soft/soft_renderer.cpp \
           engine/resource/src/cache.cpp \
           tests/suites/png_test.cpp
PNG_OBJ := $(patsubst %.cpp,$(HOSTOBJ)/%.o,$(PNG_SRC))
PNG     := $(BUILD)/phx_png

# The sprite binary: decode a sheet PNG -> bake Texture+Sprite -> mount -> build Animator -> render.
SPRITE_SRC := engine/core/src/assert.cpp \
              engine/core/src/fixed.cpp \
              engine/core/src/log.cpp \
              engine/memory/src/memory_root.cpp \
              engine/ecs/src/world.cpp \
              engine/anim/src/anim.cpp \
              engine/platform/src/null/null_platform.cpp \
              engine/render/src/renderer.cpp \
              engine/render/src/soft/soft_renderer.cpp \
              engine/resource/src/cache.cpp \
              tests/suites/sprite_test.cpp
SPRITE_OBJ := $(patsubst %.cpp,$(HOSTOBJ)/%.o,$(SPRITE_SRC))
SPRITE     := $(BUILD)/phx_sprite

# The tiled binary: parse a Tiled .tmj -> bake Tilemap+Spawns -> mount -> render + resolve spawns.
TILED_SRC := engine/core/src/assert.cpp \
             engine/core/src/fixed.cpp \
             engine/core/src/log.cpp \
             engine/memory/src/memory_root.cpp \
             engine/platform/src/null/null_platform.cpp \
             engine/render/src/renderer.cpp \
             engine/render/src/soft/soft_renderer.cpp \
             engine/resource/src/cache.cpp \
             tests/suites/tiled_test.cpp
TILED_OBJ := $(patsubst %.cpp,$(HOSTOBJ)/%.o,$(TILED_SRC))
TILED     := $(BUILD)/phx_tiled

# The audio binary: bake a PCM blob -> mount through the seam -> mix it.
AUDIO_SRC := engine/core/src/assert.cpp \
             engine/core/src/fixed.cpp \
             engine/core/src/log.cpp \
             engine/memory/src/memory_root.cpp \
             engine/platform/src/null/null_platform.cpp \
             engine/resource/src/cache.cpp \
             engine/audio/src/mixer.cpp \
             engine/audio/src/stream.cpp \
             tests/suites/audio_test.cpp
AUDIO_OBJ := $(patsubst %.cpp,$(HOSTOBJ)/%.o,$(AUDIO_SRC))
AUDIO     := $(BUILD)/phx_audio

# The phxpack CLI (host tool): inputs -> .phxp bundle.
PHXPACK := $(BUILD)/phxpack

# Per-format converter CLIs (docs/08): author source -> intermediate .phx*, merged by phxpack.
PHXSPRITE := $(BUILD)/phxsprite
PHXTILE   := $(BUILD)/phxtile
PHXSND    := $(BUILD)/phxsnd
PHXBIN    := $(BUILD)/phxbin
PHXVIZ    := $(BUILD)/phxviz     # WAV -> per-frame .phxviz visualization track (miracle-player)

# The two-stage pipeline integration test (converters -> intermediates -> merge -> mount).
PIPELINE_SRC := engine/core/src/assert.cpp \
                engine/core/src/fixed.cpp \
                engine/core/src/log.cpp \
                engine/memory/src/memory_root.cpp \
                engine/platform/src/null/null_platform.cpp \
                engine/resource/src/cache.cpp \
                tests/suites/pipeline_test.cpp
PIPELINE_OBJ := $(patsubst %.cpp,$(HOSTOBJ)/%.o,$(PIPELINE_SRC))
PIPELINE     := $(BUILD)/phx_pipeline

# The render smoke links the front end + software backend + null platform framebuffer.
RENDER_SRC := engine/core/src/assert.cpp \
              engine/core/src/fixed.cpp \
              engine/core/src/log.cpp \
              engine/memory/src/memory_root.cpp \
              engine/platform/src/null/null_platform.cpp \
              engine/render/src/renderer.cpp \
              engine/render/src/soft/soft_renderer.cpp \
              tests/suites/render_test.cpp
RENDER_OBJ := $(patsubst %.cpp,$(HOSTOBJ)/%.o,$(RENDER_SRC))

# The PPU smoke links the SAME front end + null platform, but swaps the software backend
# for the GBA-native PPU backend (quantize -> 4bpp tiles + OAM -> ppu_compose). It proves
# the GBA hardware render model headlessly (palette/alignment/sprite-ceiling limits + all).
PPU_SRC := $(patsubst engine/render/src/soft/soft_renderer.cpp,engine/render/src/gba/gba_ppu.cpp,\
             $(patsubst tests/suites/render_test.cpp,tests/suites/ppu_test.cpp,$(RENDER_SRC)))
PPU_OBJ := $(patsubst %.cpp,$(HOSTOBJ)/%.o,$(PPU_SRC))

# The GU smoke: the SAME front end + null platform, swapping in the PSP-native GU backend
# (record GU sprite quads -> gu_compose). The GU is full-colour, so its output is bit-
# identical to the software reference — this renders the render_test scene and matches it.
GU_SRC := $(patsubst engine/render/src/soft/soft_renderer.cpp,engine/render/src/gu/gu_backend.cpp,\
            $(patsubst tests/suites/render_test.cpp,tests/suites/gu_test.cpp,$(RENDER_SRC)))
GU_OBJ := $(patsubst %.cpp,$(HOSTOBJ)/%.o,$(GU_SRC))

.PHONY: test smoke render ppu gu playable physics anim scene ui platformer emberwing emberwing-ppu emberwing-sdl emberwing-gl miracle miracle-headless miracle-test gba-miracle-ppu size-gate-miracle sdl gl sdl-verify gl-verify audio-verify gba gba-ppu gba-platformer gba-platformer-ppu gba-emberwing gba-emberwing-ppu psp psp-platformer psp-emberwing psp-gu psp-audio gba-audio audio texcache png sprite tiled resource phxpack pipeline tools size-gate check build clean depcheck version docs dist dist-win dist-gba dist-psp

# Run everything: unit + loop smoke + render(soft+ppu+gu) + gameplay slices + capstones + audio + resource + dep gate.
check: test smoke render ppu gu playable physics anim scene ui platformer emberwing emberwing-ppu miracle-test audio texcache png sprite tiled resource phxpack pipeline tools depcheck

# --- M7 release gates --------------------------------------------------------------------------
# Determinism gate: the SAME suites under scalar=float (pc) and scalar=fixed16 (gba_sim) must
# print identical outcomes AND render the byte-identical frame. Cheap to run: the per-tier
# object dirs mean the second tier is mostly relinks. This is a named release gate (docs/09 §5).
DET_SUITES := test render ppu gu physics anim scene ui platformer emberwing emberwing-ppu miracle-test
determinism:
	@echo "determinism gate: pc (scalar=float) vs gba_sim (scalar=fixed16)"
	@$(MAKE) -s $(DET_SUITES) TIER=pc      | grep -aE "PASS|FAIL" > $(BUILD)/det-pc.log
	@cp $(BUILD)/render_out.ppm $(BUILD)/det-pc.ppm
	@$(MAKE) -s $(DET_SUITES) TIER=gba_sim | grep -aE "PASS|FAIL" > $(BUILD)/det-gba.log
	@cp $(BUILD)/render_out.ppm $(BUILD)/det-gba.ppm
	@diff $(BUILD)/det-pc.log $(BUILD)/det-gba.log >/dev/null \
	  && cmp -s $(BUILD)/det-pc.ppm $(BUILD)/det-gba.ppm \
	  && echo "DETERMINISM PASS ($$(grep -c . $(BUILD)/det-pc.log) suite outcomes + the rendered frame identical across tiers)" \
	  || { echo "DETERMINISM FAIL — tier outcomes diverge:"; diff $(BUILD)/det-pc.log $(BUILD)/det-gba.log; exit 1; }

# Sanitizer gate: the full check suite under ASan + UBSan (no recover: any UB/heap error fails).
# Its own build root so instrumented objects never mix with the normal ones.
# detect_leaks=0: the engine allocates from arenas only (zero hot-path heap), so heap leaks can
# only come from test fixtures — which intentionally leak ("test lifetime") — and one-shot CLIs.
sanitize:
	@ASAN_OPTIONS=detect_leaks=0 $(MAKE) check BUILD=$(BUILD)/asan \
	  EXTRA_CXXFLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -O1"
	@echo "SANITIZE PASS (full check suite clean under ASan+UBSan)"

# Release gate: the full check suite must build and pass with PHX_BUILD_RELEASE=1 — the config
# every GBA/PSP cross build always uses (GBA_FLAGS/PSP_FLAGS above), and what a host `Release`
# CMake config picks up via the NDEBUG fallback in assert.h/log.h. Exists because stripping
# PHX_ASSERT to `((void)0)` can turn an assert-only-referenced local into an unused-variable
# warning under -Wall -Wextra (the zero-warnings bar applies here too) — this is where that
# would be caught, on the host, instead of only showing up in a devkitARM/pspsdk cross build.
# Own build root so release objects never mix with the default debug ones.
release:
	@$(MAKE) check BUILD=$(BUILD)/release RELEASE=1
	@echo "RELEASE PASS (full check suite clean with PHX_BUILD_RELEASE=1)"

test: $(BIN)
	@./$(BIN)

smoke: $(SMOKE)
	@./$(SMOKE)

render: $(RENDER)
	@./$(RENDER)

ppu: $(PPU)
	@./$(PPU)

gu: $(GU)
	@./$(GU)

playable: $(PLAYABLE)
	@./$(PLAYABLE)

physics: $(PHYSICS)
	@./$(PHYSICS)

anim: $(ANIM)
	@./$(ANIM)

scene: $(SCENE)
	@./$(SCENE)

ui: $(UI)
	@./$(UI)

platformer: $(PLATFORMER)
	@./$(PLATFORMER)

miracle-test: $(MIRACLE_TEST)
	@./$(MIRACLE_TEST)

emberwing: $(EMBERWING)
	@./$(EMBERWING)

emberwing-ppu: $(EMBERWING_PPU)
	@./$(EMBERWING_PPU)

# Build the windowed Emberwing (SDL software render + live audio device). Not part of `check`.
emberwing-sdl:
	@command -v sdl2-config >/dev/null 2>&1 || { echo "SDL build needs SDL2 (sdl2-config not found). Install libsdl2-dev."; exit 1; }
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -DPHX_HAVE_SDL $(INCLUDES) `sdl2-config --cflags` \
	  $(EWSDL_SRC) `sdl2-config --libs` -o $(BUILD)/emberwing_sdl
	@echo "built $(BUILD)/emberwing_sdl  —  run ./$(BUILD)/emberwing_sdl (arrows move, Z=jump, Enter=start)"

# The same windowed Emberwing through the OpenGL backend. Needs SDL2 + libGL.
emberwing-gl:
	@command -v sdl2-config >/dev/null 2>&1 || { echo "GL build needs SDL2 + libGL (sdl2-config not found)."; exit 1; }
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -DPHX_HAVE_SDL -DPHX_HAVE_GL $(INCLUDES) `sdl2-config --cflags` \
	  $(EWGL_SRC) `sdl2-config --libs` -lGL -o $(BUILD)/emberwing_gl
	@echo "built $(BUILD)/emberwing_gl  —  GPU-rendered window (software backend stays the golden ref)"

# Build the windowed SDL example (not run automatically — it opens a window).
sdl:
	@command -v sdl2-config >/dev/null 2>&1 || { echo "SDL build needs SDL2 (sdl2-config not found). Install libsdl2-dev."; exit 1; }
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -DPHX_HAVE_SDL $(INCLUDES) `sdl2-config --cflags` \
	  $(PLATSDL_SRC) `sdl2-config --libs` -o $(BUILD)/platformer_sdl
	@echo "built $(BUILD)/platformer_sdl  —  run ./$(BUILD)/platformer_sdl to play (arrows/WASD, Z=jump, Enter=start)"

# The "A Small Miracle" music visualizer in a real SDL window (software renderer + real audio).
# START=play/pause, A=style, B=chrome, SELECT=profiler. Decodes the song to WAV first (ffmpeg).
miracle: $(MIRACLE_WAV)
	@command -v sdl2-config >/dev/null 2>&1 || { echo "miracle needs SDL2 (sdl2-config not found). Install libsdl2-dev."; exit 1; }
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -DPHX_HAVE_SDL $(INCLUDES) `sdl2-config --cflags` \
	  $(MIRACLESDL_SRC) `sdl2-config --libs` -o $(BUILD)/miracle
	@echo "built $(BUILD)/miracle  —  run ./$(BUILD)/miracle (START=play/pause, A=style, B=chrome)"

# Headless build of the same app (null platform + audio stand-in): compiles the shipping entry
# and, with PHX_MAX_FRAMES=N, gives a bounded smoke run. Not part of `check` (loops on null).
miracle-headless: $(MIRACLE_WAV) $(MIRACLEAPP)
	@echo "built $(MIRACLEAPP)  —  PHX_MAX_FRAMES=N ./$(MIRACLEAPP) for a bounded run"

# Build the windowed GPU (OpenGL) example. Needs SDL2 + libGL.
gl:
	@command -v sdl2-config >/dev/null 2>&1 || { echo "GL build needs SDL2 + libGL (sdl2-config not found). Install libsdl2-dev + libgl-dev."; exit 1; }
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -DPHX_HAVE_SDL -DPHX_HAVE_GL $(INCLUDES) `sdl2-config --cflags` \
	  $(PLATGL_SRC) `sdl2-config --libs` -lGL -o $(BUILD)/platformer_gl
	@echo "built $(BUILD)/platformer_gl  —  GPU-rendered window (the software backend stays the golden ref)"

# Pixel-verify the desktop backends on a REAL window/GPU against the software golden reference
# (the on-hardware analogue of `make ppu`/`make gu`). Each renders the render_test scene through
# the actual SDL/OpenGL backend, reads the presented frame back, and diffs the golden pixels.
# Needs SDL2 (+ libGL for gl-verify) and a display ($DISPLAY).
SDLVER_SRC := engine/core/src/assert.cpp engine/core/src/fixed.cpp engine/core/src/log.cpp \
              engine/memory/src/memory_root.cpp engine/render/src/renderer.cpp \
              engine/render/src/soft/soft_renderer.cpp \
              engine/platform/src/sdl/sdl_platform.cpp tests/verify/window_verify.cpp
GLVER_SRC  := $(filter-out engine/render/src/soft/soft_renderer.cpp,$(SDLVER_SRC)) \
              engine/render/src/gl/gl_backend.cpp

sdl-verify:
	@command -v sdl2-config >/dev/null 2>&1 || { echo "needs SDL2 (sdl2-config not found)."; exit 1; }
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -DPHX_HAVE_SDL $(INCLUDES) `sdl2-config --cflags` \
	  $(SDLVER_SRC) `sdl2-config --libs` -o $(BUILD)/window_verify_sdl
	@./$(BUILD)/window_verify_sdl

gl-verify:
	@command -v sdl2-config >/dev/null 2>&1 || { echo "needs SDL2 + libGL (sdl2-config not found)."; exit 1; }
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -DPHX_HAVE_SDL -DPHX_HAVE_GL $(INCLUDES) `sdl2-config --cflags` \
	  $(GLVER_SRC) `sdl2-config --libs` -lGL -o $(BUILD)/window_verify_gl
	@./$(BUILD)/window_verify_gl

# Verify the SDL audio DEVICE glue live: open a real output device, drive the mixer + command
# queue from the audio thread, confirm the callback fires and produces non-silent SFX output.
AUDIOVER_SRC := engine/core/src/assert.cpp engine/core/src/fixed.cpp engine/core/src/log.cpp \
                engine/memory/src/memory_root.cpp engine/audio/src/mixer.cpp \
                engine/platform/src/sdl/sdl_platform.cpp tests/verify/audio_device_verify.cpp
audio-verify:
	@command -v sdl2-config >/dev/null 2>&1 || { echo "needs SDL2 (sdl2-config not found)."; exit 1; }
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -DPHX_HAVE_SDL $(INCLUDES) `sdl2-config --cflags` \
	  $(AUDIOVER_SRC) `sdl2-config --libs` -o $(BUILD)/audio_device_verify
	@./$(BUILD)/audio_device_verify

# --- GUI editors (SDL) ----------------------------------------------------------------------
# phxtmap: the mouse-driven tilemap editor over the ENGINE's own window/renderer/UI (docs/08).
# Its document model (tools/phxtmap/editor.h) is unit-tested in the pipeline suite; this builds
# the interactive shell. Needs SDL2 + a display; deliberately not part of `check`.
TMAP_SRC := $(filter-out engine/platform/src/null/null_platform.cpp,$(APP_SRC)) \
            engine/platform/src/sdl/sdl_platform.cpp tools/phxtmap/main.cpp

tmap:
	@command -v sdl2-config >/dev/null 2>&1 || { echo "needs SDL2 (sdl2-config not found)."; exit 1; }
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -DPHX_HAVE_SDL $(INCLUDES) `sdl2-config --cflags` \
	  $(TMAP_SRC) `sdl2-config --libs` -o $(BUILD)/phxtmap
	@echo "built $(BUILD)/phxtmap  —  GUI tilemap editor: ./$(BUILD)/phxtmap [--out f.tmj] [f.tmj]"

# phxentity: the keyboard-driven entity/prefab TABLE editor over the same engine shell
# (edits the phxbin author JSON; docs/08). Needs SDL2 + a display.
ENTITY_SRC := $(filter-out engine/platform/src/null/null_platform.cpp,$(APP_SRC)) \
              engine/platform/src/sdl/sdl_platform.cpp tools/phxentity/main.cpp

entity:
	@command -v sdl2-config >/dev/null 2>&1 || { echo "needs SDL2 (sdl2-config not found)."; exit 1; }
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -DPHX_HAVE_SDL $(INCLUDES) `sdl2-config --cflags` \
	  $(ENTITY_SRC) `sdl2-config --libs` -o $(BUILD)/phxentity
	@echo "built $(BUILD)/phxentity  —  GUI prefab-table editor: ./$(BUILD)/phxentity file.json"

# --- Windows cross build (MinGW-w64) --------------------------------------------------------
# Cross-compiles the FULL host build — engine + every test/tool binary — into Windows PE32+
# executables (the null platform backend is pure C++/stdio, so the whole suite is Windows-clean
# with no OS-specific code). Reuses the host rules recursively with the MinGW compiler and its
# own build root, so it can never mix objects with the native build. Works with GCC MinGW
# (apt install g++-mingw-w64-x86-64) or llvm-mingw; override WIN_CXX to point at either.
WIN_CXX ?= x86_64-w64-mingw32-g++

win:
	@command -v $(WIN_CXX) >/dev/null 2>&1 || { echo "needs a MinGW-w64 toolchain ($(WIN_CXX) not found); e.g. 'apt install g++-mingw-w64-x86-64' or llvm-mingw, or set WIN_CXX="; exit 1; }
	@$(MAKE) build BUILD=$(BUILD)/win CXX="$(WIN_CXX) -static"
	@echo "built $(BUILD)/win/*.exe (PE32+ Windows binaries, statically linked — no runtime DLLs)"

# Runs the Windows unit-test exe under Wine (native wine or the org.winehq.Wine flatpak).
win-verify: win
	@if command -v wine >/dev/null 2>&1; then \
	  wine $(BUILD)/win/phx_tests.exe; \
	elif command -v flatpak >/dev/null 2>&1 && flatpak info org.winehq.Wine >/dev/null 2>&1; then \
	  flatpak run --filesystem=host org.winehq.Wine $(CURDIR)/$(BUILD)/win/phx_tests.exe; \
	else \
	  echo "needs wine (or the org.winehq.Wine flatpak) to run the .exe suite"; exit 1; \
	fi

# --- GBA cross build (devkitARM) ------------------------------------------------------------
# Cross-compiles the portable engine + the GBA platform backend into a real .gba ROM. Needs
# devkitPro/devkitARM. Not part of `check` (it targets ARM7TDMI). Proves the portability thesis
# on metal: the SAME C++17 engine that runs the host suite boots on a Game Boy Advance.
DEVKITPRO  ?= /opt/devkitpro
DEVKITARM  ?= $(DEVKITPRO)/devkitARM
GBA_CXX    := $(DEVKITARM)/bin/arm-none-eabi-g++
GBA_OBJCOPY:= $(DEVKITARM)/bin/arm-none-eabi-objcopy
GBA_FIX    := $(DEVKITPRO)/tools/bin/gbafix
GBA_ARCH   := -mthumb -mthumb-interwork -mcpu=arm7tdmi
# -fno-threadsafe-statics: the GBA is single-threaded, and devkitARM's newlib lock/pthread stubs
# that GCC's __cxa_guard_acquire calls for a function-local `static` with a runtime initializer
# (e.g. phx::type_id<T>()'s id) DEADLOCK on bare metal. Dropping the guard is correct here and
# fixes a hard hang on the first World::add<>() (it spun forever before any LevelScene render).
# PHX_TARGET_GBA selects the fixed-point tier (also set by the host TIER=gba_sim build);
# PHX_GBA_HW additionally marks REAL hardware (MMIO/VRAM/OAM paths) — only the cross build sets it.
# PHX_BUILD_RELEASE=1 unconditionally: every GBA cross build (smoke ROMs included) ships on
# real hardware or an emulator standing in for it, never a dev host, so PHX_ASSERT's trap path
# (assert.h) is pure downside — a debug assert firing on a console hangs it, it doesn't drop
# into a debugger — and Trace/Debug log format strings just burn ROM the size gate is tight on.
GBA_FLAGS  := -std=c++17 -O2 -fno-rtti -fno-exceptions -fno-threadsafe-statics -Wall -Wextra $(GBA_ARCH) -DPHX_TARGET_GBA=1 -DPHX_GBA_HW=1 -DPHX_BUILD_RELEASE=1 -DNDEBUG
GBA_INC    := -Iengine/core/include -Iengine/memory/include -Iengine/ecs/include \
              -Iengine/input/include -Iengine/audio/include -Iengine/platform/include \
              -Iengine/render/include -Iengine/render/src -Iengine/resource/include \
              -Iengine/physics/include -Iengine/anim/include -Iengine/scene/include \
              -Iengine/ui/include -Iengine/runtime/include -Iexamples/platformer/src
GBA_SRC    := engine/core/src/assert.cpp engine/core/src/fixed.cpp engine/core/src/log.cpp \
              engine/render/src/renderer.cpp engine/render/src/soft/soft_renderer.cpp \
              engine/platform/src/gba/gba_platform.cpp examples/gba_smoke/main.cpp
GBA_OBJ    := $(patsubst %.cpp,$(BUILD)/gba/%.o,$(GBA_SRC))

# The PPU-hardware smoke: the SAME front end + GBA platform, but linking the GBA-native PPU
# backend (gba_ppu.cpp) INSTEAD OF the software rasterizer, so it programs the real PPU
# (Mode 0 tiles/map + OBJ) and the silicon scans it out.
GBA_PPU_SRC := engine/core/src/assert.cpp engine/core/src/fixed.cpp engine/core/src/log.cpp \
               engine/render/src/renderer.cpp engine/render/src/gba/gba_ppu.cpp \
               engine/platform/src/gba/gba_platform.cpp examples/gba_ppu/main.cpp
GBA_PPU_OBJ := $(patsubst %.cpp,$(BUILD)/gba/%.o,$(GBA_PPU_SRC))

# The GBA DirectSound device smoke: the portable AudioMixer + command queue driven through the
# platform's real DMA1/Timer0/FIFO-A path (no renderer). Verdict published in GDB-readable globals.
GBA_AUDIO_SRC := engine/core/src/assert.cpp engine/core/src/fixed.cpp engine/core/src/log.cpp \
                 engine/audio/src/mixer.cpp engine/platform/src/gba/gba_platform.cpp \
                 examples/gba_audio/main.cpp
GBA_AUDIO_OBJ := $(patsubst %.cpp,$(BUILD)/gba/%.o,$(GBA_AUDIO_SRC))

# The GBA battery-SRAM save smoke: round-trips a magic-tagged blob through the seam's real
# 0x0E000000 byte-wise SRAM path. Verdict in GDB-readable globals; mGBA also persists the SRAM
# to a .sav whose "PHXS" magic can be checked host-side (run twice for the persistence proof).
GBA_SAVE_SRC := engine/core/src/assert.cpp engine/core/src/fixed.cpp engine/core/src/log.cpp \
                engine/platform/src/gba/gba_platform.cpp examples/gba_save/main.cpp
GBA_SAVE_OBJ := $(patsubst %.cpp,$(BUILD)/gba/%.o,$(GBA_SAVE_SRC))

# The full example platformer as a GBA ROM: the portable engine + gameplay systems + the GBA
# platform backend, with the .phxp bundle baked offline (host) and linked into ROM via bin2s.
BIN2S        := $(DEVKITPRO)/tools/bin/bin2s
GBA_PLAT_SRC := engine/core/src/assert.cpp engine/core/src/fixed.cpp engine/core/src/log.cpp \
                engine/memory/src/memory_root.cpp engine/ecs/src/world.cpp \
                engine/render/src/renderer.cpp engine/render/src/soft/soft_renderer.cpp \
                engine/physics/src/physics.cpp engine/anim/src/anim.cpp \
                engine/scene/src/scene.cpp engine/ui/src/ui.cpp engine/runtime/src/app.cpp \
                engine/resource/src/cache.cpp engine/audio/src/mixer.cpp \
                engine/platform/src/gba/gba_platform.cpp \
                examples/platformer/src/systems.cpp examples/platformer/src/gba_main.cpp
GBA_PLAT_OBJ := $(patsubst %.cpp,$(BUILD)/gba/%.o,$(GBA_PLAT_SRC))
PLATBAKE     := $(BUILD)/platbake

# The same full platformer, but rendered by the GBA-native PPU backend (Mode-0 tiles + OBJ on
# real silicon) instead of the software rasterizer — just swap the backend TU at link time.
GBA_PLAT_PPU_SRC := $(filter-out engine/render/src/soft/soft_renderer.cpp \
                                 examples/platformer/src/gba_main.cpp,$(GBA_PLAT_SRC)) \
                    engine/render/src/gba/gba_ppu.cpp examples/platformer/src/gba_ppu_main.cpp
GBA_PLAT_PPU_OBJ := $(patsubst %.cpp,$(BUILD)/gba/%.o,$(GBA_PLAT_PPU_SRC))

# Emberwing (examples/emberwing) as a GBA ROM: same engine set as the platformer ROM, the
# game's own systems/scenes/entry swapped in. Software render tier — the PPU backend models
# one 32x32-cell BG, and Cinder Hollow is a 320x20 4-layer parallax map (see the example's
# README §1). Audio is real: gba_main starts the DirectSound pump at 18157 Hz (vblank-locked).
GBA_EW_SRC := $(filter-out examples/platformer/src/systems.cpp \
                           examples/platformer/src/gba_main.cpp,$(GBA_PLAT_SRC)) \
              examples/emberwing/src/systems.cpp examples/emberwing/src/scenes.cpp \
              examples/emberwing/src/gba_main.cpp
GBA_EW_OBJ := $(patsubst %.cpp,$(BUILD)/gba/%.o,$(GBA_EW_SRC))
EWBAKE     := $(BUILD)/ewbake

# Emberwing on the NATIVE PPU (the shipping GBA configuration): swap the soft rasterizer for
# the PPU backend + the native-resolution entry. The CPU stops rasterizing entirely — the
# silicon scans out four streamed text BGs + OAM — which restores the frame budget and, with
# it, an unstarved 18157 Hz DirectSound stream.
GBA_EW_PPU_SRC := $(filter-out engine/render/src/soft/soft_renderer.cpp \
                               examples/emberwing/src/gba_main.cpp,$(GBA_EW_SRC)) \
                  engine/render/src/gba/gba_ppu.cpp examples/emberwing/src/gba_ppu_main.cpp
GBA_EW_PPU_OBJ := $(patsubst %.cpp,$(BUILD)/gba/%.o,$(GBA_EW_PPU_SRC))

# The "A Small Miracle" music visualizer as the SHIPPING GBA ROM (native PPU). Engine set like the
# emberwing PPU ROM + the audio STREAM producer (stream.cpp), the miracle logic TU, and the GBA
# PPU entry. The spectrogram is a BG tilemap; particles are OBJ sparks; audio is the resident
# tier-0 song streamed at 18157 Hz. The ~10 MB song lives in cartridge ROM (read zero-copy).
GBA_MIRACLE_PPU_SRC := engine/core/src/assert.cpp engine/core/src/fixed.cpp engine/core/src/log.cpp \
                engine/memory/src/memory_root.cpp engine/ecs/src/world.cpp \
                engine/render/src/renderer.cpp engine/render/src/gba/gba_ppu.cpp \
                engine/physics/src/physics.cpp engine/anim/src/anim.cpp \
                engine/scene/src/scene.cpp engine/ui/src/ui.cpp engine/runtime/src/app.cpp \
                engine/resource/src/cache.cpp engine/audio/src/mixer.cpp engine/audio/src/stream.cpp \
                engine/platform/src/gba/gba_platform.cpp \
                examples/miracle-player/src/miracle.cpp examples/miracle-player/src/gba_ppu_main.cpp
GBA_MIRACLE_PPU_OBJ := $(patsubst %.cpp,$(BUILD)/gba/%.o,$(GBA_MIRACLE_PPU_SRC))
MIRACLEBAKE := $(BUILD)/miraclebake

gba: $(BUILD)/gba/phx-smoke.gba
	@echo "built $<  —  load in mGBA (d-pad moves the sprite)"

# The PPU-native render backend on real GBA hardware (Mode 0 tiles + OBJ, scanned by the silicon).
gba-ppu: $(BUILD)/gba/phx-ppu.gba
	@echo "built $<  —  PPU hardware backend (Mode 0 tiles + OBJ); load in mGBA (d-pad moves the sprite)"

# The GBA DirectSound output device (DMA1 + Timer0 -> FIFO A).
gba-audio: $(BUILD)/gba/phx-audio.gba
	@echo "built $<  —  GBA DirectSound device; verify with the mGBA GDB stub (phx_gba_audio_verdict==1)"

# Bake the bundle on the host, embed it in ROM, and build the playable platformer ROM.
gba-platformer: $(BUILD)/gba/phx-platformer.gba
	@echo "built $<  —  load in mGBA/an emulator (arrows move, A/Z jump, Start = menu)"

# The full platformer rendered by the native PPU hardware backend (Mode-0 tiles + OBJ).
gba-platformer-ppu: $(BUILD)/gba/phx-platformer-ppu.gba
	@echo "built $<  —  full game on the GBA PPU (Mode-0 tiles + OBJ); load in mGBA"

# Bake the Emberwing bundle (tier 0), embed it in ROM, and build the playable ROM.
gba-emberwing: $(BUILD)/gba/phx-emberwing.gba
	@echo "built $<  —  SOFTWARE render tier (slow; kept as the rasterizer reference on-device)"

# Emberwing on the PPU hardware backend — the GBA build to actually play.
gba-emberwing-ppu: $(BUILD)/gba/phx-emberwing-ppu.gba
	@echo "built $<  —  native PPU (Mode-0 BGs + OBJ); load in mGBA (arrows move, A/X jump, Start = pause)"

# The "A Small Miracle" music visualizer as a native-PPU GBA ROM (the shipping miracle build).
gba-miracle-ppu: $(BUILD)/gba/phx-miracle-ppu.gba
	@echo "built $<  —  music visualizer on the GBA PPU; load in mGBA (START play/pause, L/R seek, A style, B chrome)"

# Size gate for the miracle ROM: the song is legitimately ~10 MB of PCM in cartridge ROM, so the
# ROM budget is raised to 16 MB (still half the 32 MB cart cap); the scarce RAM budgets are unchanged.
size-gate-miracle: $(BUILD)/gba/phx-miracle-ppu.gba
	@python3 tools/common/size_gate.py \
	  --rom $(BUILD)/gba/phx-miracle-ppu.gba \
	  --elf $(BUILD)/gba/miracle-ppu.elf \
	  --rom-budget-mb 16 \
	  --size-tool $(DEVKITARM)/bin/arm-none-eabi-size

# Enforce the GBA budget (docs/09 MVP gate): classify the ELF's static sections into IWRAM/EWRAM
# and check the ROM file size, failing if any budget is exceeded. Builds the shippable PPU ROM.
size-gate: $(BUILD)/gba/phx-platformer-ppu.gba
	@python3 tools/common/size_gate.py \
	  --rom $(BUILD)/gba/phx-platformer-ppu.gba \
	  --elf $(BUILD)/gba/platformer-ppu.elf \
	  --size-tool $(DEVKITARM)/bin/arm-none-eabi-size

# Same gate for the OTHER shipping PPU ROM (README calls Emberwing the console proof-of-scale
# slice) — the platformer alone doesn't catch a budget regression that only Emberwing's bigger
# map/audio/enemy set would trip.
size-gate-emberwing: $(BUILD)/gba/phx-emberwing-ppu.gba
	@python3 tools/common/size_gate.py \
	  --rom $(BUILD)/gba/phx-emberwing-ppu.gba \
	  --elf $(BUILD)/gba/emberwing-ppu.elf \
	  --size-tool $(DEVKITARM)/bin/arm-none-eabi-size

# host tool that bakes the .phxp the ROM embeds (same importers as the host game)
$(PLATBAKE): $(HOSTOBJ)/examples/platformer/src/bake_main.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $< -o $@

$(BUILD)/gba/platformer.phxp: $(PLATBAKE)
	@mkdir -p $(dir $@)
	./$(PLATBAKE) $@ 0    # tier 0: per-target encode (sounds resampled to the GBA device rate)

# bin2s: bundle bytes -> a .rodata assembly object (lives in cartridge ROM, not EWRAM)
$(BUILD)/gba/bundle.o: $(BUILD)/gba/platformer.phxp
	@command -v $(BIN2S) >/dev/null 2>&1 || { echo "needs devkitPro bin2s ($(BIN2S))"; exit 1; }
	$(BIN2S) $< > $(BUILD)/gba/bundle.s
	$(GBA_CXX) $(GBA_ARCH) -x assembler-with-cpp -c $(BUILD)/gba/bundle.s -o $@

$(BUILD)/gba/phx-platformer.gba: $(GBA_PLAT_OBJ) $(BUILD)/gba/bundle.o
	$(GBA_CXX) $(GBA_FLAGS) -specs=gba.specs $^ -o $(BUILD)/gba/platformer.elf
	$(GBA_OBJCOPY) -O binary $(BUILD)/gba/platformer.elf $@
	$(GBA_FIX) $@ >/dev/null
	@echo "ROM: $@ ($$(stat -c%s $@) bytes, header fixed)"

$(BUILD)/gba/phx-platformer-ppu.gba: $(GBA_PLAT_PPU_OBJ) $(BUILD)/gba/bundle.o
	$(GBA_CXX) $(GBA_FLAGS) -specs=gba.specs $^ -o $(BUILD)/gba/platformer-ppu.elf
	$(GBA_OBJCOPY) -O binary $(BUILD)/gba/platformer-ppu.elf $@
	$(GBA_FIX) $@ >/dev/null
	@echo "ROM: $@ ($$(stat -c%s $@) bytes, header fixed)"

# host tool that bakes the Emberwing .phxp (same importers as the host game)
$(EWBAKE): $(HOSTOBJ)/examples/emberwing/src/bake_main.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $< -o $@

$(BUILD)/gba/emberwing.phxp: $(EWBAKE)
	@mkdir -p $(dir $@)
	./$(EWBAKE) $@ 0    # tier 0: sounds resampled to the 18157 Hz GBA device rate

$(BUILD)/gba/ew_bundle.o: $(BUILD)/gba/emberwing.phxp
	@command -v $(BIN2S) >/dev/null 2>&1 || { echo "needs devkitPro bin2s ($(BIN2S))"; exit 1; }
	$(BIN2S) $< > $(BUILD)/gba/ew_bundle.s
	$(GBA_CXX) $(GBA_ARCH) -x assembler-with-cpp -c $(BUILD)/gba/ew_bundle.s -o $@

$(BUILD)/gba/phx-emberwing.gba: $(GBA_EW_OBJ) $(BUILD)/gba/ew_bundle.o
	$(GBA_CXX) $(GBA_FLAGS) -specs=gba.specs $^ -o $(BUILD)/gba/emberwing.elf
	$(GBA_OBJCOPY) -O binary $(BUILD)/gba/emberwing.elf $@
	$(GBA_FIX) $@ >/dev/null
	@echo "ROM: $@ ($$(stat -c%s $@) bytes, header fixed)"

$(BUILD)/gba/phx-emberwing-ppu.gba: $(GBA_EW_PPU_OBJ) $(BUILD)/gba/ew_bundle.o
	$(GBA_CXX) $(GBA_FLAGS) -specs=gba.specs $^ -o $(BUILD)/gba/emberwing-ppu.elf
	$(GBA_OBJCOPY) -O binary $(BUILD)/gba/emberwing-ppu.elf $@
	$(GBA_FIX) $@ >/dev/null
	@echo "ROM: $@ ($$(stat -c%s $@) bytes, header fixed)"

# host tool that bakes the miracle .phxp (tier 0: 18157 Hz audio + viz baked at that rate)
$(MIRACLEBAKE): $(HOSTOBJ)/examples/miracle-player/src/bake_main.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $< -o $@

$(BUILD)/gba/miracle.phxp: $(MIRACLEBAKE) $(MIRACLE_WAV)
	@mkdir -p $(dir $@)
	./$(MIRACLEBAKE) $@ 0 $(MIRACLE_WAV)   # tier 0: resample the song to the GBA device rate

# bin2s: the bundle bytes -> a .rodata object (symbol miracle_phxp[]), lives in cartridge ROM
$(BUILD)/gba/miracle_bundle.o: $(BUILD)/gba/miracle.phxp
	@command -v $(BIN2S) >/dev/null 2>&1 || { echo "needs devkitPro bin2s ($(BIN2S))"; exit 1; }
	$(BIN2S) $< > $(BUILD)/gba/miracle_bundle.s
	$(GBA_CXX) $(GBA_ARCH) -x assembler-with-cpp -c $(BUILD)/gba/miracle_bundle.s -o $@

$(BUILD)/gba/phx-miracle-ppu.gba: $(GBA_MIRACLE_PPU_OBJ) $(BUILD)/gba/miracle_bundle.o
	$(GBA_CXX) $(GBA_FLAGS) -specs=gba.specs $^ -o $(BUILD)/gba/miracle-ppu.elf
	$(GBA_OBJCOPY) -O binary $(BUILD)/gba/miracle-ppu.elf $@
	$(GBA_FIX) $@ >/dev/null
	@echo "ROM: $@ ($$(stat -c%s $@) bytes, header fixed)"

$(BUILD)/gba/%.o: %.cpp
	@command -v $(GBA_CXX) >/dev/null 2>&1 || { echo "GBA build needs devkitARM ($(GBA_CXX) not found)."; exit 1; }
	@mkdir -p $(dir $@)
	$(GBA_CXX) $(GBA_FLAGS) $(GBA_INC) -MMD -MP -MF $(@:.o=.d) -c $< -o $@

$(BUILD)/gba/phx-smoke.gba: $(GBA_OBJ)
	$(GBA_CXX) $(GBA_FLAGS) -specs=gba.specs $(GBA_OBJ) -o $(BUILD)/gba/smoke.elf
	$(GBA_OBJCOPY) -O binary $(BUILD)/gba/smoke.elf $@
	$(GBA_FIX) $@ >/dev/null

$(BUILD)/gba/phx-ppu.gba: $(GBA_PPU_OBJ)
	$(GBA_CXX) $(GBA_FLAGS) -specs=gba.specs $(GBA_PPU_OBJ) -o $(BUILD)/gba/ppu.elf
	$(GBA_OBJCOPY) -O binary $(BUILD)/gba/ppu.elf $@
	$(GBA_FIX) $@ >/dev/null
	@echo "ROM: $@ ($$(stat -c%s $@) bytes, header fixed)"

$(BUILD)/gba/phx-audio.gba: $(GBA_AUDIO_OBJ)
	$(GBA_CXX) $(GBA_FLAGS) -specs=gba.specs $(GBA_AUDIO_OBJ) -o $(BUILD)/gba/audio.elf
	$(GBA_OBJCOPY) -O binary $(BUILD)/gba/audio.elf $@
	$(GBA_FIX) $@ >/dev/null
	@echo "ROM: $@ ($$(stat -c%s $@) bytes, header fixed)"

gba-save: $(BUILD)/gba/phx-save.gba
	@echo "built $<  —  battery-SRAM save smoke; verify via the mGBA GDB stub (phx_gba_save_verdict) or the .sav magic"

$(BUILD)/gba/phx-save.gba: $(GBA_SAVE_OBJ)
	$(GBA_CXX) $(GBA_FLAGS) -specs=gba.specs $(GBA_SAVE_OBJ) -o $(BUILD)/gba/save.elf
	$(GBA_OBJCOPY) -O binary $(BUILD)/gba/save.elf $@
	$(GBA_FIX) $@ >/dev/null
	@echo "ROM: $@ ($$(stat -c%s $@) bytes, header fixed)"

# --- PSP cross build (pspsdk) ---------------------------------------------------------------
# Cross-compiles the portable engine + the PSP platform backend into an EBOOT.PBP. Needs pspsdk
# (psp-gcc). Not part of `check`. The third real target for the one codebase.
PSPDEV     ?= /home/jaden/pspdev
PSPSDK     := $(PSPDEV)/psp/sdk
PSP_CXX    := $(PSPDEV)/bin/psp-g++
PSP_FIXUP  := $(PSPDEV)/bin/psp-fixup-imports
PSP_MKSFO  := $(PSPDEV)/bin/mksfo
PSP_PACK   := $(PSPDEV)/bin/pack-pbp
PSP_PRXGEN := $(PSPDEV)/bin/psp-prxgen
# -fno-tree-loop-distribute-patterns: we define our own memset/memcpy/memmove/strlen for PSP (see
# psp_platform.cpp — pspsdk would otherwise bind them to kernel sysclib SYSCALL stubs); this stops
# GCC from turning those functions' byte loops back into calls to themselves (infinite recursion).
# PHX_BUILD_RELEASE=1 unconditionally — same rationale as GBA_FLAGS above: every PSP cross
# build ships on real hardware/an emulator, so debug asserts and Trace/Debug logging are pure
# downside there, never a dev aid.
PSP_FLAGS  := -std=c++17 -O2 -fno-rtti -fno-exceptions -fno-threadsafe-statics -fno-tree-loop-distribute-patterns -Wall -Wextra -G0 -DPHX_TARGET_PSP=1 -DPHX_BUILD_RELEASE=1 -DNDEBUG \
              -I$(PSPSDK)/include
PSP_INC    := -Iengine/core/include -Iengine/memory/include -Iengine/ecs/include \
              -Iengine/input/include -Iengine/audio/include -Iengine/platform/include \
              -Iengine/render/include -Iengine/render/src -Iengine/resource/include \
              -Iengine/physics/include -Iengine/anim/include -Iengine/scene/include \
              -Iengine/ui/include -Iengine/runtime/include -Iexamples/platformer/src
# The PRX link flow: keep relocations (-Wl,-q) + the prx specs/linkfile so psp-prxgen works.
PSP_LDFLAGS:= -L$(PSPSDK)/lib -specs=$(PSPSDK)/lib/prxspecs -Wl,-q,-T$(PSPSDK)/lib/linkfile.prx
PSP_LIBS   := -lpspaudio -lpspdisplay -lpspge -lpspctrl -lpspgu -lpsppower -lpsputility -lpspuser -lpspkernel
PSP_SRC    := engine/core/src/assert.cpp engine/core/src/fixed.cpp engine/core/src/log.cpp \
              engine/render/src/renderer.cpp engine/render/src/soft/soft_renderer.cpp \
              engine/platform/src/psp/psp_platform.cpp examples/psp_smoke/main.cpp
PSP_OBJ    := $(patsubst %.cpp,$(BUILD)/psp/%.o,$(PSP_SRC))

# The PSP-native GU render backend on real hardware (sceGu GU_SPRITES display list) instead of the
# software rasterizer — swap the backend TU + a verifying entry that reads back eDRAM on-PSP.
PSP_GU_SRC := $(filter-out engine/render/src/soft/soft_renderer.cpp examples/psp_smoke/main.cpp,$(PSP_SRC)) \
              engine/render/src/gu/gu_backend.cpp examples/psp_gu/main.cpp
PSP_GU_OBJ := $(patsubst %.cpp,$(BUILD)/psp/%.o,$(PSP_GU_SRC))

# The PSP sceAudio output device: the portable AudioMixer + lock-free command queue driven through
# the platform's real sceAudio channel + audio thread (no renderer; verifies the device on-PSP).
PSP_AUDIO_SRC := engine/core/src/assert.cpp engine/core/src/fixed.cpp engine/core/src/log.cpp \
                 engine/audio/src/mixer.cpp engine/platform/src/psp/psp_platform.cpp \
                 examples/psp_audio/main.cpp
PSP_AUDIO_OBJ := $(patsubst %.cpp,$(BUILD)/psp/%.o,$(PSP_AUDIO_SRC))

# The PSP save smoke: round-trips a magic-tagged blob through the seam's real sceIo path
# (memory-stick file). Verdict via thread names in PPSSPP's log; run twice for persistence.
PSP_SAVE_SRC := engine/core/src/assert.cpp engine/core/src/fixed.cpp engine/core/src/log.cpp \
                engine/platform/src/psp/psp_platform.cpp examples/psp_save/main.cpp
PSP_SAVE_OBJ := $(patsubst %.cpp,$(BUILD)/psp/%.o,$(PSP_SAVE_SRC))

# The full example platformer as a PSP EBOOT: the portable engine + gameplay systems + the PSP
# platform backend (software renderer), with the .phxp bundle baked offline (host) and linked
# into the EBOOT via bin2s — exactly like the GBA ROM (the PSP backend's file seam serves the
# embedded bundle on mount, via phx_psp_set_bundle()). Entry point: psp_main.cpp (module info +
# HOME-exit callbacks + PSP-sized budgets), the PSP analogue of gba_main.cpp.
PSP_PLAT_SRC := engine/core/src/assert.cpp engine/core/src/fixed.cpp engine/core/src/log.cpp \
                engine/memory/src/memory_root.cpp engine/ecs/src/world.cpp \
                engine/render/src/renderer.cpp engine/render/src/soft/soft_renderer.cpp \
                engine/physics/src/physics.cpp engine/anim/src/anim.cpp \
                engine/scene/src/scene.cpp engine/ui/src/ui.cpp engine/runtime/src/app.cpp \
                engine/resource/src/cache.cpp engine/audio/src/mixer.cpp \
                engine/platform/src/psp/psp_platform.cpp \
                examples/platformer/src/systems.cpp examples/platformer/src/psp_main.cpp
PSP_PLAT_OBJ := $(patsubst %.cpp,$(BUILD)/psp/%.o,$(PSP_PLAT_SRC))

# Emberwing as a PSP EBOOT: the platformer's engine set with the game's own TUs + entry
# (real sceAudio device thread draining the command queue).
PSP_EW_SRC := $(filter-out examples/platformer/src/systems.cpp \
                           examples/platformer/src/psp_main.cpp,$(PSP_PLAT_SRC)) \
              examples/emberwing/src/systems.cpp examples/emberwing/src/scenes.cpp \
              examples/emberwing/src/psp_main.cpp
PSP_EW_OBJ := $(patsubst %.cpp,$(BUILD)/psp/%.o,$(PSP_EW_SRC))

psp: $(BUILD)/psp/EBOOT.PBP
	@echo "built $<  —  run in PPSSPP / on a PSP"

# Bake the bundle on the host, embed it in the EBOOT, and build the playable platformer.
psp-platformer: $(BUILD)/psp/platformer/EBOOT.PBP
	@echo "built $<  —  run in PPSSPP / on a PSP (arrows move, X/triangle jump, Start = menu)"

# Bake the Emberwing bundle (tier 1), embed it, and build the playable EBOOT.
psp-emberwing: $(BUILD)/psp/emberwing/EBOOT.PBP
	@echo "built $<  —  run in PPSSPP / on a PSP (arrows move, X jump — hold for height; Start = pause)"

psp-gu: $(BUILD)/psp/gu/EBOOT.PBP
	@echo "built $<  —  PSP GU hardware backend (sceGu display list); run in PPSSPP"

psp-audio: $(BUILD)/psp/audio/EBOOT.PBP
	@echo "built $<  —  PSP sceAudio output device; run in PPSSPP, grep log for AUDIO_DEVICE_PASS"

$(BUILD)/psp/%.o: %.cpp
	@command -v $(PSP_CXX) >/dev/null 2>&1 || { echo "PSP build needs pspsdk ($(PSP_CXX) not found)."; exit 1; }
	@mkdir -p $(dir $@)
	$(PSP_CXX) $(PSP_FLAGS) $(PSP_INC) -MMD -MP -MF $(@:.o=.d) -c $< -o $@

$(BUILD)/psp/EBOOT.PBP: $(PSP_OBJ)
	$(PSP_CXX) $(PSP_FLAGS) $(PSP_OBJ) $(PSP_LDFLAGS) $(PSP_LIBS) -o $(BUILD)/psp/smoke.elf
	$(PSP_FIXUP) $(BUILD)/psp/smoke.elf
	$(PSP_PRXGEN) $(BUILD)/psp/smoke.elf $(BUILD)/psp/smoke.prx
	$(PSP_MKSFO) "Phoenix PSP Smoke" $(BUILD)/psp/PARAM.SFO
	$(PSP_PACK) $@ $(BUILD)/psp/PARAM.SFO NULL NULL NULL NULL NULL $(BUILD)/psp/smoke.prx NULL

# The platformer EBOOT: host-bake the bundle, embed it via bin2s (same tool/symbols as the GBA
# ROM: `platformer_phxp` + `platformer_phxp_size`), link the game, then PRX -> pack into EBOOT.PBP.
$(BUILD)/psp/platformer.phxp: $(PLATBAKE)
	@mkdir -p $(dir $@)
	./$(PLATBAKE) $@ 1    # tier 1 (PSP)

# Uses the portable bin2s (tools/common/bin2s.py) instead of devkitPro's, so the PSP EBOOT build
# depends only on pspsdk + a host python3 (no second SDK) — important for CI.
$(BUILD)/psp/bundle.o: $(BUILD)/psp/platformer.phxp
	python3 tools/common/bin2s.py $< > $(BUILD)/psp/bundle.s
	$(PSP_CXX) -x assembler-with-cpp -c $(BUILD)/psp/bundle.s -o $@

$(BUILD)/psp/platformer/EBOOT.PBP: $(PSP_PLAT_OBJ) $(BUILD)/psp/bundle.o
	@mkdir -p $(dir $@)
	$(PSP_CXX) $(PSP_FLAGS) $(PSP_PLAT_OBJ) $(BUILD)/psp/bundle.o $(PSP_LDFLAGS) $(PSP_LIBS) -o $(BUILD)/psp/platformer/platformer.elf
	$(PSP_FIXUP) $(BUILD)/psp/platformer/platformer.elf
	$(PSP_PRXGEN) $(BUILD)/psp/platformer/platformer.elf $(BUILD)/psp/platformer/platformer.prx
	$(PSP_MKSFO) "Phoenix Platformer" $(BUILD)/psp/platformer/PARAM.SFO
	$(PSP_PACK) $@ $(BUILD)/psp/platformer/PARAM.SFO NULL NULL NULL NULL NULL $(BUILD)/psp/platformer/platformer.prx NULL
	@echo "EBOOT: $@ ($$(stat -c%s $@) bytes)"

$(BUILD)/psp/emberwing.phxp: $(EWBAKE)
	@mkdir -p $(dir $@)
	./$(EWBAKE) $@ 1    # tier 1 (PSP)

$(BUILD)/psp/ew_bundle.o: $(BUILD)/psp/emberwing.phxp
	python3 tools/common/bin2s.py $< > $(BUILD)/psp/ew_bundle.s
	$(PSP_CXX) -x assembler-with-cpp -c $(BUILD)/psp/ew_bundle.s -o $@

$(BUILD)/psp/emberwing/EBOOT.PBP: $(PSP_EW_OBJ) $(BUILD)/psp/ew_bundle.o
	@mkdir -p $(dir $@)
	$(PSP_CXX) $(PSP_FLAGS) $(PSP_EW_OBJ) $(BUILD)/psp/ew_bundle.o $(PSP_LDFLAGS) $(PSP_LIBS) -o $(BUILD)/psp/emberwing/emberwing.elf
	$(PSP_FIXUP) $(BUILD)/psp/emberwing/emberwing.elf
	$(PSP_PRXGEN) $(BUILD)/psp/emberwing/emberwing.elf $(BUILD)/psp/emberwing/emberwing.prx
	$(PSP_MKSFO) "Emberwing" $(BUILD)/psp/emberwing/PARAM.SFO
	$(PSP_PACK) $@ $(BUILD)/psp/emberwing/PARAM.SFO NULL NULL NULL NULL NULL $(BUILD)/psp/emberwing/emberwing.prx NULL
	@echo "EBOOT: $@ ($$(stat -c%s $@) bytes)"

$(BUILD)/psp/gu/EBOOT.PBP: $(PSP_GU_OBJ)
	@mkdir -p $(BUILD)/psp/gu
	$(PSP_CXX) $(PSP_FLAGS) $(PSP_GU_OBJ) $(PSP_LDFLAGS) $(PSP_LIBS) -o $(BUILD)/psp/gu/gu.elf
	$(PSP_FIXUP) $(BUILD)/psp/gu/gu.elf
	$(PSP_PRXGEN) $(BUILD)/psp/gu/gu.elf $(BUILD)/psp/gu/gu.prx
	$(PSP_MKSFO) "Phoenix PSP GU" $(BUILD)/psp/gu/PARAM.SFO
	$(PSP_PACK) $@ $(BUILD)/psp/gu/PARAM.SFO NULL NULL NULL NULL NULL $(BUILD)/psp/gu/gu.prx NULL
	@echo "EBOOT: $@ ($$(stat -c%s $@) bytes)"

$(BUILD)/psp/audio/EBOOT.PBP: $(PSP_AUDIO_OBJ)
	@mkdir -p $(BUILD)/psp/audio
	$(PSP_CXX) $(PSP_FLAGS) $(PSP_AUDIO_OBJ) $(PSP_LDFLAGS) $(PSP_LIBS) -o $(BUILD)/psp/audio/audio.elf
	$(PSP_FIXUP) $(BUILD)/psp/audio/audio.elf
	$(PSP_PRXGEN) $(BUILD)/psp/audio/audio.elf $(BUILD)/psp/audio/audio.prx
	$(PSP_MKSFO) "Phoenix PSP Audio" $(BUILD)/psp/audio/PARAM.SFO
	$(PSP_PACK) $@ $(BUILD)/psp/audio/PARAM.SFO NULL NULL NULL NULL NULL $(BUILD)/psp/audio/audio.prx NULL
	@echo "EBOOT: $@ ($$(stat -c%s $@) bytes)"

psp-save: $(BUILD)/psp/save/EBOOT.PBP
	@echo "built $<  —  sceIo save smoke; run in PPSSPP twice, grep log for SAVE_DEVICE_PASS / SAVE_PERSIST_PASS"

$(BUILD)/psp/save/EBOOT.PBP: $(PSP_SAVE_OBJ)
	@mkdir -p $(BUILD)/psp/save
	$(PSP_CXX) $(PSP_FLAGS) $(PSP_SAVE_OBJ) $(PSP_LDFLAGS) $(PSP_LIBS) -o $(BUILD)/psp/save/save.elf
	$(PSP_FIXUP) $(BUILD)/psp/save/save.elf
	$(PSP_PRXGEN) $(BUILD)/psp/save/save.elf $(BUILD)/psp/save/save.prx
	$(PSP_MKSFO) "Phoenix PSP Save" $(BUILD)/psp/save/PARAM.SFO
	$(PSP_PACK) $@ $(BUILD)/psp/save/PARAM.SFO NULL NULL NULL NULL NULL $(BUILD)/psp/save/save.prx NULL
	@echo "EBOOT: $@ ($$(stat -c%s $@) bytes)"

resource: $(RESOURCE)
	@./$(RESOURCE)

audio: $(AUDIO)
	@./$(AUDIO)

texcache: $(TEXCACHE)
	@./$(TEXCACHE)

png: $(PNG)
	@./$(PNG)

sprite: $(SPRITE)
	@./$(SPRITE)

tiled: $(TILED)
	@./$(TILED)

# Build the CLI, then prove it end-to-end: generate inputs, bake a bundle, check the magic.
phxpack: $(PHXPACK)
	@printf 'P6\n2 2\n255\n' > $(BUILD)/t.ppm
	@printf '\377\000\000\000\377\000\000\000\377\377\377\000' >> $(BUILD)/t.ppm
	@printf '# demo map\nt 8 8\n1,2\n2,1\n' > $(BUILD)/m.tmcsv
	@./$(PHXPACK) --out $(BUILD)/cli.phxp --target 2 $(BUILD)/t.ppm $(BUILD)/m.tmcsv
	@head -c4 $(BUILD)/cli.phxp | grep -q PHXP && echo "PHXPACK PASS (valid PHXP bundle)" || (echo "PHXPACK FAIL"; exit 1)
	@./$(PHXPACK) --out $(BUILD)/cli_z.phxp --target 2 --compress $(BUILD)/t.ppm $(BUILD)/m.tmcsv
	@head -c4 $(BUILD)/cli_z.phxp | grep -q PHXP && echo "PHXPACK PASS (valid compressed bundle)" || (echo "PHXPACK FAIL"; exit 1)
	@# lock-file pipeline (docs/08 §2): identical inputs skip the bake; a touched input is
	@# re-baked while the rest are reused from the previous bundle; the incremental output
	@# is BYTE-IDENTICAL to a from-scratch --full bake; --upgrade re-bakes from the lock.
	@cp $(BUILD)/t.ppm $(BUILD)/li_t.ppm; cp $(BUILD)/m.tmcsv $(BUILD)/li_m.tmcsv
	@rm -f $(BUILD)/cli_i.phxp.lock
	@./$(PHXPACK) --out $(BUILD)/cli_i.phxp --target 2 --manifest $(BUILD)/li_t.ppm $(BUILD)/li_m.tmcsv >/dev/null
	@test -f $(BUILD)/cli_i.phxp.lock && test -f $(BUILD)/cli_i.phxp.manifest.txt \
	  && echo "PHXPACK PASS (lock + manifest sidecars written)" || (echo "PHXPACK FAIL (sidecars)"; exit 1)
	@./$(PHXPACK) --out $(BUILD)/cli_i.phxp --target 2 $(BUILD)/li_t.ppm $(BUILD)/li_m.tmcsv | grep -q "up to date" \
	  && echo "PHXPACK PASS (unchanged inputs skip the bake)" || (echo "PHXPACK FAIL (lock skip)"; exit 1)
	@printf '# touched\n' >> $(BUILD)/li_m.tmcsv
	@./$(PHXPACK) --out $(BUILD)/cli_i.phxp --target 2 $(BUILD)/li_t.ppm $(BUILD)/li_m.tmcsv | grep -q "reused 1" \
	  && echo "PHXPACK PASS (incremental: unchanged input reused)" || (echo "PHXPACK FAIL (incremental)"; exit 1)
	@./$(PHXPACK) --out $(BUILD)/cli_if.phxp --target 2 --full $(BUILD)/li_t.ppm $(BUILD)/li_m.tmcsv >/dev/null
	@cmp -s $(BUILD)/cli_i.phxp $(BUILD)/cli_if.phxp \
	  && echo "PHXPACK PASS (incremental rebake byte-identical to full)" || (echo "PHXPACK FAIL (reproducibility)"; exit 1)
	@./$(PHXPACK) --upgrade $(BUILD)/cli_i.phxp >/dev/null && cmp -s $(BUILD)/cli_i.phxp $(BUILD)/cli_if.phxp \
	  && echo "PHXPACK PASS (--upgrade re-bakes reproducibly from the lock)" || (echo "PHXPACK FAIL (upgrade)"; exit 1)
	@# fixture INPUTS live at literal build/ — the suite binaries that drop them (png/sprite/
	@# tiled/audio) hardcode that path, so it holds even when a wrapper overrides $(BUILD)
	@test -f build/png_in.png && ./$(PHXPACK) --out $(BUILD)/cli_png.phxp --target 2 build/png_in.png \
	  && head -c4 $(BUILD)/cli_png.phxp | grep -q PHXP && echo "PHXPACK PASS (baked a real PNG)" || (echo "PHXPACK FAIL (png)"; exit 1)
	@test -f build/hero.sprdef && ./$(PHXPACK) --out $(BUILD)/cli_spr.phxp --target 2 build/hero.sprdef \
	  && head -c4 $(BUILD)/cli_spr.phxp | grep -q PHXP && echo "PHXPACK PASS (baked a sprite sheet + clips)" || (echo "PHXPACK FAIL (sprdef)"; exit 1)
	@test -f build/level.tmj && ./$(PHXPACK) --out $(BUILD)/cli_tmj.phxp --target 2 build/level.tmj \
	  && head -c4 $(BUILD)/cli_tmj.phxp | grep -q PHXP && echo "PHXPACK PASS (baked a Tiled map + spawns)" || (echo "PHXPACK FAIL (tmj)"; exit 1)
	@test -f build/tone.wav && ./$(PHXPACK) --out $(BUILD)/cli_wav.phxp --target 2 build/tone.wav \
	  && head -c4 $(BUILD)/cli_wav.phxp | grep -q PHXP && echo "PHXPACK PASS (baked a WAV sound)" || (echo "PHXPACK FAIL (wav)"; exit 1)

# The two-stage pipeline in-process: converters bake intermediates, phxpack merges, the cache
# mounts and reads back every asset type. Also drops the build/p_* source fixtures the CLI smoke
# (`tools`) re-runs the real binaries over.
pipeline: $(PIPELINE)
	@./$(PIPELINE)

# CLI smoke: run the REAL converter + assembler binaries over the fixtures `pipeline` dropped,
# proving the executables parse args, link, and produce a valid merged bundle.
# fixture INPUTS (build/p_*) live at literal build/ — pipeline_test hardcodes that path,
# so it holds even when a wrapper target overrides $(BUILD) (e.g. `sanitize`).
tools: pipeline $(PHXSPRITE) $(PHXTILE) $(PHXSND) $(PHXBIN) $(PHXVIZ) $(PHXPACK)
	@./$(PHXSPRITE) --out $(BUILD)/c_hero.phxspr  --name hero  build/p_hero.sprdef
	@./$(PHXTILE)   --out $(BUILD)/c_level.phxtmap --name level build/p_level.tmj
	@./$(PHXSND)    --out $(BUILD)/c_tone.phxsnd   --name tone  build/p_tone.wav
	@./$(PHXVIZ)    --out $(BUILD)/c_tone.phxviz   --name toneviz --rate 18157 build/p_tone.wav
	@./$(PHXBIN)    --out $(BUILD)/c_items.phxbin  --name items --header $(BUILD)/c_items.gen.h build/p_items.json
	@./$(PHXPACK)   --out $(BUILD)/c_assets.phxp $(BUILD)/c_hero.phxspr $(BUILD)/c_level.phxtmap $(BUILD)/c_tone.phxsnd $(BUILD)/c_tone.phxviz $(BUILD)/c_items.phxbin
	@head -c4 $(BUILD)/c_assets.phxp | grep -q PHXP && echo "TOOLS PASS (5 converters -> merged bundle)" || (echo "TOOLS FAIL"; exit 1)

# --- release packaging (`make dist*`) --------------------------------------------------------
# Stages per-target release bundles under build/dist/ and archives them, named
# phoenix-<version>-<what>[-<os>-<arch>]. These are exactly the files the tag-driven release
# workflow (.github/workflows/release.yml) attaches to the GitHub release — build one locally
# to see what a release ships. Archives are tar.gz for the host, zip elsewhere (Windows users
# and emulator frontends expect zip); zipping uses python3's zipfile so no new tool dependency.
DIST      := $(BUILD)/dist
HOST_OS   := $(shell uname -s | tr '[:upper:]' '[:lower:]')
HOST_ARCH := $(shell uname -m)
PHX_ZIP    = python3 -m zipfile -c

version:
	@echo $(PHX_VERSION)

# --- API reference (`make docs`) --------------------------------------------------------------
# Doxygen over the PUBLIC headers only (engine/*/include — the boundary depcheck enforces; see
# Doxyfile). tools/common/doxyfilter.py promotes the house '//' comments to '///' so the
# existing header prose IS the reference. PROJECT_NUMBER is appended here from version.h so
# the version stays single-sourced. Needs doxygen on PATH (override with DOXYGEN=/path/to).
DOXYGEN ?= doxygen
docs:
	@command -v $(DOXYGEN) >/dev/null 2>&1 || \
	  { echo "docs: doxygen not found — install it (e.g. apt install doxygen) or set DOXYGEN=<path>"; exit 1; }
	@( cat Doxyfile; echo "PROJECT_NUMBER=$(PHX_VERSION)" ) | $(DOXYGEN) -
	@echo "docs: build/docs/html/index.html"

# Host tools SDK: the five asset-pipeline CLIs + license/readme. (The full dev SDK with headers
# + static libs installs via CMake: cmake --install / cpack — see RELEASING.md.)
dist: tools
	@rm -rf $(DIST)/phoenix-$(PHX_VERSION)-tools-$(HOST_OS)-$(HOST_ARCH)
	@mkdir -p $(DIST)/phoenix-$(PHX_VERSION)-tools-$(HOST_OS)-$(HOST_ARCH)/bin
	@cp $(PHXPACK) $(PHXSPRITE) $(PHXTILE) $(PHXSND) $(PHXBIN) \
	  $(DIST)/phoenix-$(PHX_VERSION)-tools-$(HOST_OS)-$(HOST_ARCH)/bin/
	@cp LICENSE README.md $(DIST)/phoenix-$(PHX_VERSION)-tools-$(HOST_OS)-$(HOST_ARCH)/
	@tar -C $(DIST) -czf $(DIST)/phoenix-$(PHX_VERSION)-tools-$(HOST_OS)-$(HOST_ARCH).tar.gz \
	  phoenix-$(PHX_VERSION)-tools-$(HOST_OS)-$(HOST_ARCH)
	@echo "dist: $(DIST)/phoenix-$(PHX_VERSION)-tools-$(HOST_OS)-$(HOST_ARCH).tar.gz"

# Windows tools (statically linked PE32+, no runtime DLLs). Depends on `make win`, which
# cross-builds everything; only the tools ship — the desktop games need the SDL platform
# backend, which the MinGW static build doesn't carry.
dist-win: win
	@rm -rf $(DIST)/phoenix-$(PHX_VERSION)-tools-windows-x86_64
	@mkdir -p $(DIST)/phoenix-$(PHX_VERSION)-tools-windows-x86_64/bin
	@cp $(BUILD)/win/phxpack.exe $(BUILD)/win/phxsprite.exe $(BUILD)/win/phxtile.exe \
	  $(BUILD)/win/phxsnd.exe $(BUILD)/win/phxbin.exe \
	  $(DIST)/phoenix-$(PHX_VERSION)-tools-windows-x86_64/bin/
	@cp LICENSE README.md $(DIST)/phoenix-$(PHX_VERSION)-tools-windows-x86_64/
	@cd $(DIST) && $(PHX_ZIP) phoenix-$(PHX_VERSION)-tools-windows-x86_64.zip \
	  phoenix-$(PHX_VERSION)-tools-windows-x86_64
	@echo "dist: $(DIST)/phoenix-$(PHX_VERSION)-tools-windows-x86_64.zip"

# GBA: the two shipping PPU ROMs (the software-render variants are dev references, not releases).
dist-gba: gba-platformer-ppu gba-emberwing-ppu
	@rm -rf $(DIST)/phoenix-$(PHX_VERSION)-gba
	@mkdir -p $(DIST)/phoenix-$(PHX_VERSION)-gba
	@cp $(BUILD)/gba/phx-platformer-ppu.gba $(BUILD)/gba/phx-emberwing-ppu.gba \
	  $(DIST)/phoenix-$(PHX_VERSION)-gba/
	@cp LICENSE $(DIST)/phoenix-$(PHX_VERSION)-gba/
	@cd $(DIST) && $(PHX_ZIP) phoenix-$(PHX_VERSION)-gba.zip phoenix-$(PHX_VERSION)-gba
	@echo "dist: $(DIST)/phoenix-$(PHX_VERSION)-gba.zip"

# PSP: EBOOT.PBP must keep its exact name, one folder per game (drop the folder into
# ms0:/PSP/GAME/ or point PPSSPP at it).
dist-psp: psp-platformer psp-emberwing
	@rm -rf $(DIST)/phoenix-$(PHX_VERSION)-psp
	@mkdir -p $(DIST)/phoenix-$(PHX_VERSION)-psp/platformer $(DIST)/phoenix-$(PHX_VERSION)-psp/emberwing
	@cp $(BUILD)/psp/platformer/EBOOT.PBP $(DIST)/phoenix-$(PHX_VERSION)-psp/platformer/
	@cp $(BUILD)/psp/emberwing/EBOOT.PBP  $(DIST)/phoenix-$(PHX_VERSION)-psp/emberwing/
	@cp LICENSE $(DIST)/phoenix-$(PHX_VERSION)-psp/
	@cd $(DIST) && $(PHX_ZIP) phoenix-$(PHX_VERSION)-psp.zip phoenix-$(PHX_VERSION)-psp
	@echo "dist: $(DIST)/phoenix-$(PHX_VERSION)-psp.zip"

build: $(BIN) $(SMOKE) $(RENDER) $(PPU) $(GU) $(PLAYABLE) $(PHYSICS) $(ANIM) $(SCENE) $(UI) $(PLATFORMER) $(PLATAPP) $(EMBERWING) $(EMBERWING_PPU) $(EWAPP) $(AUDIO) $(TEXCACHE) $(PNG) $(SPRITE) $(TILED) $(RESOURCE) $(PHXPACK) $(PHXSPRITE) $(PHXTILE) $(PHXSND) $(PHXBIN) $(PIPELINE)

$(BIN): $(UNIT_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(UNIT_OBJ) -o $@

$(SMOKE): $(SMOKE_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(SMOKE_OBJ) -o $@

$(RENDER): $(RENDER_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(RENDER_OBJ) -o $@

$(PPU): $(PPU_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(PPU_OBJ) -o $@

$(GU): $(GU_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(GU_OBJ) -o $@

$(PLAYABLE): $(PLAYABLE_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(PLAYABLE_OBJ) -o $@

$(PHYSICS): $(PHYSICS_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(PHYSICS_OBJ) -o $@

$(ANIM): $(ANIM_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(ANIM_OBJ) -o $@

$(SCENE): $(SCENE_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(SCENE_OBJ) -o $@

$(UI): $(UI_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(UI_OBJ) -o $@

$(PLATFORMER): $(PLATFORMER_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(PLATFORMER_OBJ) -o $@

$(PLATAPP): $(PLATAPP_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(PLATAPP_OBJ) -o $@

$(MIRACLEAPP): $(MIRACLEAPP_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(MIRACLEAPP_OBJ) -o $@

$(MIRACLE_TEST): $(MIRACLE_TEST_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(MIRACLE_TEST_OBJ) -o $@

$(EMBERWING): $(EMBERWING_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(EMBERWING_OBJ) -o $@

$(EMBERWING_PPU): $(EMBERWING_PPU_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(EMBERWING_PPU_OBJ) -o $@

$(EWAPP): $(EWAPP_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(EWAPP_OBJ) -o $@

$(RESOURCE): $(RESOURCE_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(RESOURCE_OBJ) -o $@

$(AUDIO): $(AUDIO_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(AUDIO_OBJ) -o $@

$(TEXCACHE): $(TEXCACHE_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(TEXCACHE_OBJ) -o $@

$(PNG): $(PNG_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(PNG_OBJ) -o $@

$(SPRITE): $(SPRITE_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(SPRITE_OBJ) -o $@

$(TILED): $(TILED_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(TILED_OBJ) -o $@

PHXPACK_HDRS := tools/phxpack/bundle_writer.h tools/phxpack/bundle_reader.h tools/phxpack/builders.h \
                tools/phxpack/png.h tools/phxpack/tiled.h tools/phxpack/wav.h tools/phxpack/json.h \
                tools/phxpack/tex_encode.h tools/phxpack/lock.h \
                tools/phxviz/analyze.h examples/miracle-player/src/viz.h

$(PHXPACK): tools/phxpack/main.cpp $(PHXPACK_HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) tools/phxpack/main.cpp -o $@

$(PHXSPRITE): tools/phxsprite/main.cpp $(PHXPACK_HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) tools/phxsprite/main.cpp -o $@

$(PHXTILE): tools/phxtile/main.cpp $(PHXPACK_HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) tools/phxtile/main.cpp -o $@

$(PHXSND): tools/phxsnd/main.cpp $(PHXPACK_HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) tools/phxsnd/main.cpp -o $@

$(PHXBIN): tools/phxbin/main.cpp $(PHXPACK_HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) tools/phxbin/main.cpp -o $@

$(PHXVIZ): tools/phxviz/main.cpp $(PHXPACK_HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) tools/phxviz/main.cpp -o $@

$(PIPELINE): $(PIPELINE_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(PIPELINE_OBJ) -o $@

$(HOSTOBJ)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -MF $(@:.o=.d) -c $< -o $@

# Config stamp: exactly one .tier-* exists at a time. Host binaries depend on it, so switching
# TIER or RELEASE forces a relink (their objects already live in per-config dirs — no recompile
# needed), and a binary linked under one config can never be mistaken for up-to-date under another.
$(TIERSTAMP):
	@mkdir -p $(BUILD)
	@rm -f $(BUILD)/.tier-*
	@touch $@

HOST_BINS := $(BIN) $(SMOKE) $(RENDER) $(PPU) $(GU) $(PLAYABLE) $(PHYSICS) $(ANIM) $(SCENE) \
             $(UI) $(PLATFORMER) $(PLATAPP) $(EMBERWING) $(EMBERWING_PPU) $(EWAPP) \
             $(AUDIO) $(TEXCACHE) $(PNG) $(SPRITE) $(TILED) \
             $(RESOURCE) $(PIPELINE) $(PHXPACK) $(PHXSPRITE) $(PHXTILE) $(PHXSND) $(PHXBIN) \
             $(PLATBAKE) $(EWBAKE)
$(HOST_BINS): $(TIERSTAMP)

# Header-dependency tracking: each compile emits a .d listing the headers it included, so a
# changed header rebuilds exactly the objects that use it (this is what kept a `bake.h` edit
# from re-baking the embedded bundle before). `-include` ignores them on a clean tree.
-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)

depcheck:
	@python3 tools/common/depcheck.py engine

clean:
	@rm -rf $(BUILD)
