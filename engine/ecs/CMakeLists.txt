# engine/ecs/CMakeLists.txt — L2 sparse-set World (spawn/despawn, components, each<>).
phx_add_module(ecs
  SRC
    src/world.cpp
  DEPS
    phx_core phx_memory
)
