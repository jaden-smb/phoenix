# engine/memory/CMakeLists.txt — L1 allocators + MemoryRoot (one boot allocation).
phx_add_module(memory
  SRC
    src/memory_root.cpp
  DEPS
    phx_core
)
