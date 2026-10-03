# engine/runtime/CMakeLists.txt — L4 composition root: the App fixed-step main loop.
# Owns World + Renderer + InputState + the game's audio output; the top of the dependency graph.
# (GameAudio is header-only: phx_audio/phx_resource are linked only where a game plays sound.)
phx_add_module(runtime
  SRC
    src/app.cpp
    src/game_main.cpp
    src/level.cpp
    src/behaviours.cpp
    src/flow.cpp
    src/dialogue.cpp
    src/component_schema.cpp
    src/budget.cpp
    src/devtools.cpp
  DEPS
    phx_core phx_memory phx_platform phx_render phx_ecs phx_input phx_audio phx_resource
    phx_physics phx_anim phx_ui
)
