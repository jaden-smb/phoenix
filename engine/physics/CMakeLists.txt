# engine/physics/CMakeLists.txt — L3 swept AABB-vs-tilemap + overlap pass + point query.
phx_add_module(physics
  SRC
    src/physics.cpp
  DEPS
    phx_core phx_ecs
)
