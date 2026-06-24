# engine/anim/CMakeLists.txt — L3 sprite-sheet frames + animation state machine.
phx_add_module(anim
  SRC
    src/anim.cpp
  DEPS
    phx_core phx_ecs
)
