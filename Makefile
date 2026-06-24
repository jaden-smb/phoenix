# Phoenix Engine — host Makefile (foundation build + tests).
# The canonical cross-platform build is CMake (docs/07); this Makefile exists so the
# engine foundation can be built and tested on a host with just g++ + make (no cmake).
#
#   make test     build and run the foundation test suite
#   make clean    remove build artifacts
#
# Default tier is PC (float scalar). To exercise the GBA fixed-point path on the host:
#   make test TIER=gba_sim    (defines PHX_TARGET_GBA -> scalar = fixed16)

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
            -Iexamples/platformer/src \
            -Itests

ifeq ($(TIER),gba_sim)
  CXXFLAGS += -DPHX_TARGET_GBA=1
endif

BUILD  := build
BIN    := $(BUILD)/phx_tests
SMOKE  := $(BUILD)/phx_smoke
RENDER := $(BUILD)/phx_render
PPU    := $(BUILD)/phx_ppu
GU     := $(BUILD)/phx_gu

TEST_SRC   := tests/main.cpp \
              tests/test_fixed.cpp \
              tests/test_memory.cpp \
              tests/test_ecs.cpp \
              tests/test_time.cpp \
              tests/test_input.cpp \
              tests/test_physics.cpp \
              tests/test_anim.cpp \
              tests/test_scene.cpp \
              tests/test_ui.cpp \
              tests/test_audio.cpp \
              tests/test_stream.cpp \
              tests/test_lz.cpp \
              tests/test_png.cpp \
              tests/test_json.cpp \
              tests/test_wav.cpp \
              tests/test_cmdqueue.cpp

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

UNIT_OBJ  := $(patsubst %.cpp,$(BUILD)/%.o,$(UNIT_ENGINE) $(TEST_SRC))

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
SMOKE_SRC := $(APP_SRC) tests/smoke_app.cpp
SMOKE_OBJ := $(patsubst %.cpp,$(BUILD)/%.o,$(SMOKE_SRC))

# The playable binary: full stack (memory+ecs+input+render+loop) driven by scripted input.
PLAYABLE_SRC := $(APP_SRC) tests/playable_test.cpp
PLAYABLE_OBJ := $(patsubst %.cpp,$(BUILD)/%.o,$(PLAYABLE_SRC))
PLAYABLE     := $(BUILD)/phx_playable

# The physics binary: full stack with PhysicsWorld driving a falling body onto a tile floor.
PHYSICS_SRC := $(APP_SRC) tests/physics_test.cpp
PHYSICS_OBJ := $(patsubst %.cpp,$(BUILD)/%.o,$(PHYSICS_SRC))
PHYSICS     := $(BUILD)/phx_physics

# The anim binary: full stack with AnimationSystem driving a sprite-sheet frame on screen.
ANIM_SRC := $(APP_SRC) tests/anim_test.cpp
ANIM_OBJ := $(patsubst %.cpp,$(BUILD)/%.o,$(ANIM_SRC))
ANIM     := $(BUILD)/phx_anim

# The scene binary: full stack with a SceneStack driving a gameplay scene + menu overlay.
SCENE_SRC := $(APP_SRC) tests/scene_test.cpp
SCENE_OBJ := $(patsubst %.cpp,$(BUILD)/%.o,$(SCENE_SRC))
SCENE     := $(BUILD)/phx_scene

# The ui binary: full stack with the immediate-mode UI drawing text/bar/menu + focus nav.
UI_SRC := $(APP_SRC) tests/ui_test.cpp
UI_OBJ := $(patsubst %.cpp,$(BUILD)/%.o,$(UI_SRC))
UI     := $(BUILD)/phx_ui

# The platformer binary (M1 capstone): the full example game + every engine system, driven
# headlessly by a scripted controller. Links all gameplay modules + the example sources.
# APP_SRC already bundles physics/anim/scene/ui; add the resource cache + the example.
PLATFORMER_SRC := $(APP_SRC) \
                  engine/resource/src/cache.cpp \
                  engine/audio/src/mixer.cpp \
                  examples/platformer/src/systems.cpp \
                  tests/platformer_test.cpp
PLATFORMER_OBJ := $(patsubst %.cpp,$(BUILD)/%.o,$(PLATFORMER_SRC))
PLATFORMER     := $(BUILD)/phx_platformer

# The production example binary (real main; runs until quit). Built to prove the shipping
# entry point compiles + links; not run by `check` (it loops on the null backend).
PLATAPP_SRC := $(APP_SRC) \
               engine/resource/src/cache.cpp \
               engine/audio/src/mixer.cpp \
               examples/platformer/src/systems.cpp \
               examples/platformer/src/main.cpp
PLATAPP_OBJ := $(patsubst %.cpp,$(BUILD)/%.o,$(PLATAPP_SRC))
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

# The resource binary: bake a bundle -> mount through the seam -> render from it.
RESOURCE_SRC := engine/core/src/assert.cpp \
                engine/core/src/fixed.cpp \
                engine/core/src/log.cpp \
                engine/memory/src/memory_root.cpp \
                engine/platform/src/null/null_platform.cpp \
                engine/render/src/renderer.cpp \
                engine/render/src/soft/soft_renderer.cpp \
                engine/resource/src/cache.cpp \
                tests/resource_test.cpp
RESOURCE_OBJ := $(patsubst %.cpp,$(BUILD)/%.o,$(RESOURCE_SRC))
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
                tests/texcache_test.cpp
TEXCACHE_OBJ := $(patsubst %.cpp,$(BUILD)/%.o,$(TEXCACHE_SRC))
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
           tests/png_test.cpp
PNG_OBJ := $(patsubst %.cpp,$(BUILD)/%.o,$(PNG_SRC))
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
              tests/sprite_test.cpp
SPRITE_OBJ := $(patsubst %.cpp,$(BUILD)/%.o,$(SPRITE_SRC))
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
             tests/tiled_test.cpp
TILED_OBJ := $(patsubst %.cpp,$(BUILD)/%.o,$(TILED_SRC))
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
             tests/audio_test.cpp
AUDIO_OBJ := $(patsubst %.cpp,$(BUILD)/%.o,$(AUDIO_SRC))
AUDIO     := $(BUILD)/phx_audio

# The phxpack CLI (host tool): inputs -> .phxp bundle.
PHXPACK := $(BUILD)/phxpack

# Per-format converter CLIs (docs/08): author source -> intermediate .phx*, merged by phxpack.
PHXSPRITE := $(BUILD)/phxsprite
PHXTILE   := $(BUILD)/phxtile
PHXSND    := $(BUILD)/phxsnd
PHXBIN    := $(BUILD)/phxbin

# The two-stage pipeline integration test (converters -> intermediates -> merge -> mount).
PIPELINE_SRC := engine/core/src/assert.cpp \
                engine/core/src/fixed.cpp \
                engine/core/src/log.cpp \
                engine/memory/src/memory_root.cpp \
                engine/platform/src/null/null_platform.cpp \
                engine/resource/src/cache.cpp \
                tests/pipeline_test.cpp
PIPELINE_OBJ := $(patsubst %.cpp,$(BUILD)/%.o,$(PIPELINE_SRC))
PIPELINE     := $(BUILD)/phx_pipeline

# The render smoke links the front end + software backend + null platform framebuffer.
RENDER_SRC := engine/core/src/assert.cpp \
              engine/core/src/fixed.cpp \
              engine/core/src/log.cpp \
              engine/memory/src/memory_root.cpp \
              engine/platform/src/null/null_platform.cpp \
              engine/render/src/renderer.cpp \
              engine/render/src/soft/soft_renderer.cpp \
              tests/render_test.cpp
RENDER_OBJ := $(patsubst %.cpp,$(BUILD)/%.o,$(RENDER_SRC))

# The PPU smoke links the SAME front end + null platform, but swaps the software backend
# for the GBA-native PPU backend (quantize -> 4bpp tiles + OAM -> ppu_compose). It proves
# the GBA hardware render model headlessly (palette/alignment/sprite-ceiling limits + all).
PPU_SRC := $(patsubst engine/render/src/soft/soft_renderer.cpp,engine/render/src/gba/gba_ppu.cpp,\
             $(patsubst tests/render_test.cpp,tests/ppu_test.cpp,$(RENDER_SRC)))
PPU_OBJ := $(patsubst %.cpp,$(BUILD)/%.o,$(PPU_SRC))

# The GU smoke: the SAME front end + null platform, swapping in the PSP-native GU backend
# (record GU sprite quads -> gu_compose). The GU is full-colour, so its output is bit-
# identical to the software reference — this renders the render_test scene and matches it.
GU_SRC := $(patsubst engine/render/src/soft/soft_renderer.cpp,engine/render/src/gu/gu_backend.cpp,\
            $(patsubst tests/render_test.cpp,tests/gu_test.cpp,$(RENDER_SRC)))
GU_OBJ := $(patsubst %.cpp,$(BUILD)/%.o,$(GU_SRC))

.PHONY: test smoke render ppu gu playable physics anim scene ui platformer sdl gl sdl-verify gl-verify audio-verify gba gba-ppu gba-platformer gba-platformer-ppu psp psp-platformer psp-gu psp-audio gba-audio audio texcache png sprite tiled resource phxpack pipeline tools size-gate check build clean depcheck

# Run everything: unit + loop smoke + render(soft+ppu+gu) + gameplay slices + capstone + audio + resource + dep gate.
check: test smoke render ppu gu playable physics anim scene ui platformer audio texcache png sprite tiled resource phxpack pipeline tools depcheck

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

# Build the windowed SDL example (not run automatically — it opens a window).
sdl:
	@command -v sdl2-config >/dev/null 2>&1 || { echo "SDL build needs SDL2 (sdl2-config not found). Install libsdl2-dev."; exit 1; }
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -DPHX_HAVE_SDL $(INCLUDES) `sdl2-config --cflags` \
	  $(PLATSDL_SRC) `sdl2-config --libs` -o $(BUILD)/platformer_sdl
	@echo "built $(BUILD)/platformer_sdl  —  run ./$(BUILD)/platformer_sdl to play (arrows/WASD, Z=jump, Enter=start)"

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
              engine/platform/src/sdl/sdl_platform.cpp tests/window_verify.cpp
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
                engine/platform/src/sdl/sdl_platform.cpp tests/audio_device_verify.cpp
audio-verify:
	@command -v sdl2-config >/dev/null 2>&1 || { echo "needs SDL2 (sdl2-config not found)."; exit 1; }
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -DPHX_HAVE_SDL $(INCLUDES) `sdl2-config --cflags` \
	  $(AUDIOVER_SRC) `sdl2-config --libs` -o $(BUILD)/audio_device_verify
	@./$(BUILD)/audio_device_verify

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
GBA_FLAGS  := -std=c++17 -O2 -fno-rtti -fno-exceptions -fno-threadsafe-statics -Wall -Wextra $(GBA_ARCH) -DPHX_TARGET_GBA=1
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

# Enforce the GBA budget (docs/09 MVP gate): classify the ELF's static sections into IWRAM/EWRAM
# and check the ROM file size, failing if any budget is exceeded. Builds the shippable PPU ROM.
size-gate: $(BUILD)/gba/phx-platformer-ppu.gba
	@python3 tools/common/size_gate.py \
	  --rom $(BUILD)/gba/phx-platformer-ppu.gba \
	  --elf $(BUILD)/gba/platformer-ppu.elf \
	  --size-tool $(DEVKITARM)/bin/arm-none-eabi-size

# host tool that bakes the .phxp the ROM embeds (same importers as the host game)
$(PLATBAKE): $(BUILD)/examples/platformer/src/bake_main.o
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILD)/gba/platformer.phxp: $(PLATBAKE)
	@mkdir -p $(dir $@)
	./$(PLATBAKE) $@

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
PSP_FLAGS  := -std=c++17 -O2 -fno-rtti -fno-exceptions -fno-threadsafe-statics -fno-tree-loop-distribute-patterns -Wall -Wextra -G0 -DPHX_TARGET_PSP=1 \
              -I$(PSPSDK)/include
PSP_INC    := -Iengine/core/include -Iengine/memory/include -Iengine/ecs/include \
              -Iengine/input/include -Iengine/audio/include -Iengine/platform/include \
              -Iengine/render/include -Iengine/render/src -Iengine/resource/include \
              -Iengine/physics/include -Iengine/anim/include -Iengine/scene/include \
              -Iengine/ui/include -Iengine/runtime/include -Iexamples/platformer/src
# The PRX link flow: keep relocations (-Wl,-q) + the prx specs/linkfile so psp-prxgen works.
PSP_LDFLAGS:= -L$(PSPSDK)/lib -specs=$(PSPSDK)/lib/prxspecs -Wl,-q,-T$(PSPSDK)/lib/linkfile.prx
PSP_LIBS   := -lpspaudio -lpspdisplay -lpspge -lpspctrl -lpspgu -lpsputility -lpspuser -lpspkernel
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

psp: $(BUILD)/psp/EBOOT.PBP
	@echo "built $<  —  run in PPSSPP / on a PSP"

# Bake the bundle on the host, embed it in the EBOOT, and build the playable platformer.
psp-platformer: $(BUILD)/psp/platformer/EBOOT.PBP
	@echo "built $<  —  run in PPSSPP / on a PSP (arrows move, X/triangle jump, Start = menu)"

psp-gu: $(BUILD)/psp/gu/EBOOT.PBP
	@echo "built $<  —  PSP GU hardware backend (sceGu display list); run in PPSSPP"

psp-audio: $(BUILD)/psp/audio/EBOOT.PBP
	@echo "built $<  —  PSP sceAudio output device; run in PPSSPP, grep log for AUDIO_DEVICE_PASS"

$(BUILD)/psp/%.o: %.cpp
	@command -v $(PSP_CXX) >/dev/null 2>&1 || { echo "PSP build needs pspsdk ($(PSP_CXX) not found)."; exit 1; }
	@mkdir -p $(dir $@)
	$(PSP_CXX) $(PSP_FLAGS) $(PSP_INC) -c $< -o $@

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
	./$(PLATBAKE) $@

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
	@test -f $(BUILD)/png_in.png && ./$(PHXPACK) --out $(BUILD)/cli_png.phxp --target 2 $(BUILD)/png_in.png \
	  && head -c4 $(BUILD)/cli_png.phxp | grep -q PHXP && echo "PHXPACK PASS (baked a real PNG)" || (echo "PHXPACK FAIL (png)"; exit 1)
	@test -f $(BUILD)/hero.sprdef && ./$(PHXPACK) --out $(BUILD)/cli_spr.phxp --target 2 $(BUILD)/hero.sprdef \
	  && head -c4 $(BUILD)/cli_spr.phxp | grep -q PHXP && echo "PHXPACK PASS (baked a sprite sheet + clips)" || (echo "PHXPACK FAIL (sprdef)"; exit 1)
	@test -f $(BUILD)/level.tmj && ./$(PHXPACK) --out $(BUILD)/cli_tmj.phxp --target 2 $(BUILD)/level.tmj \
	  && head -c4 $(BUILD)/cli_tmj.phxp | grep -q PHXP && echo "PHXPACK PASS (baked a Tiled map + spawns)" || (echo "PHXPACK FAIL (tmj)"; exit 1)
	@test -f $(BUILD)/tone.wav && ./$(PHXPACK) --out $(BUILD)/cli_wav.phxp --target 2 $(BUILD)/tone.wav \
	  && head -c4 $(BUILD)/cli_wav.phxp | grep -q PHXP && echo "PHXPACK PASS (baked a WAV sound)" || (echo "PHXPACK FAIL (wav)"; exit 1)

# The two-stage pipeline in-process: converters bake intermediates, phxpack merges, the cache
# mounts and reads back every asset type. Also drops the build/p_* source fixtures the CLI smoke
# (`tools`) re-runs the real binaries over.
pipeline: $(PIPELINE)
	@./$(PIPELINE)

# CLI smoke: run the REAL converter + assembler binaries over the fixtures `pipeline` dropped,
# proving the executables parse args, link, and produce a valid merged bundle.
tools: pipeline $(PHXSPRITE) $(PHXTILE) $(PHXSND) $(PHXBIN) $(PHXPACK)
	@./$(PHXSPRITE) --out $(BUILD)/c_hero.phxspr  --name hero  $(BUILD)/p_hero.sprdef
	@./$(PHXTILE)   --out $(BUILD)/c_level.phxtmap --name level $(BUILD)/p_level.tmj
	@./$(PHXSND)    --out $(BUILD)/c_tone.phxsnd   --name tone  $(BUILD)/p_tone.wav
	@./$(PHXBIN)    --out $(BUILD)/c_items.phxbin  --name items --header $(BUILD)/c_items.gen.h $(BUILD)/p_items.json
	@./$(PHXPACK)   --out $(BUILD)/c_assets.phxp $(BUILD)/c_hero.phxspr $(BUILD)/c_level.phxtmap $(BUILD)/c_tone.phxsnd $(BUILD)/c_items.phxbin
	@head -c4 $(BUILD)/c_assets.phxp | grep -q PHXP && echo "TOOLS PASS (4 converters -> merged bundle)" || (echo "TOOLS FAIL"; exit 1)

build: $(BIN) $(SMOKE) $(RENDER) $(PPU) $(GU) $(PLAYABLE) $(PHYSICS) $(ANIM) $(SCENE) $(UI) $(PLATFORMER) $(PLATAPP) $(AUDIO) $(TEXCACHE) $(PNG) $(SPRITE) $(TILED) $(RESOURCE) $(PHXPACK) $(PHXSPRITE) $(PHXTILE) $(PHXSND) $(PHXBIN) $(PIPELINE)

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
                tools/phxpack/png.h tools/phxpack/tiled.h tools/phxpack/wav.h tools/phxpack/json.h

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

$(PIPELINE): $(PIPELINE_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(PIPELINE_OBJ) -o $@

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -MMD -MP -MF $(@:.o=.d) -c $< -o $@

# Header-dependency tracking: each compile emits a .d listing the headers it included, so a
# changed header rebuilds exactly the objects that use it (this is what kept a `bake.h` edit
# from re-baking the embedded bundle before). `-include` ignores them on a clean tree.
-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)

depcheck:
	@python3 tools/common/depcheck.py engine

clean:
	@rm -rf $(BUILD)
