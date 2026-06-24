# engine/core/CMakeLists.txt — L0 closed foundation (types/assert/fixed/math/caps/log/time).
# Depends on nothing; every other module depends on it.
phx_add_module(core
  SRC
    src/assert.cpp
    src/fixed.cpp
    src/log.cpp
)
