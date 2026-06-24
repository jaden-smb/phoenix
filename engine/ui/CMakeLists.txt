# engine/ui/CMakeLists.txt — L3 immediate-mode menus/text/HUD (emits sprites via render).
phx_add_module(ui
  SRC
    src/ui.cpp
  DEPS
    phx_core phx_input phx_render
)
