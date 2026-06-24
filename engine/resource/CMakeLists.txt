# engine/resource/CMakeLists.txt — L2 .phxp bundle mount + zero-copy views + LZSS decode.
phx_add_module(resource
  SRC
    src/cache.cpp
  DEPS
    phx_core phx_memory phx_platform
)
