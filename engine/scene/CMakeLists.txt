# engine/scene/CMakeLists.txt — L3 LIFO scene stack + persistent Blackboard + arena rollback.
phx_add_module(scene
  SRC
    src/scene.cpp
  DEPS
    phx_core phx_memory
)
