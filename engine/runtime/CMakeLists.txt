# engine/runtime/CMakeLists.txt — L4 composition root: the App fixed-step main loop.
# Owns World + Renderer + InputState; the top of the dependency graph.
phx_add_module(runtime
  SRC
    src/app.cpp
  DEPS
    phx_core phx_memory phx_platform phx_render phx_ecs phx_input
)
