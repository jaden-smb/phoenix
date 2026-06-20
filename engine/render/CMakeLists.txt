# engine/render/CMakeLists.txt — illustrates the per-module pattern (docs/07).
# The renderer's public API is one header; exactly ONE backend is linked, chosen by tier.
# Each backend lives in src/<name>/ and defines phx_make_render_backend():
#   src/soft/soft_renderer.cpp — software reference (golden image, headless CI, GBA fallback)
#   src/gl/gl_backend.cpp       — desktop OpenGL (PHX_HAVE_GL; needs a GL context from the
#                                 SDL platform). The soft backend is the golden ref it's
#                                 diffed against, so on desktop pick gl OR soft, never both.
phx_add_module(render
  SRC
    src/renderer.cpp          # backend-agnostic front end (batch, sort, dispatch)
  DEPS
    phx_core phx_memory phx_platform
  BACKENDS
    gl:tier2                  # PC OpenGL 1.1 immediate-mode (no glad/glew; libGL only)
    gu:tier1                  # PSP Graphics Engine
    gba:tier0                 # GBA PPU (OAM/VRAM/BG regs)
    soft:tier2                # software reference (golden-image tests / headless CI)
)
