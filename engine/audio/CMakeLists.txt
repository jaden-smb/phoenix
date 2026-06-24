# engine/audio/CMakeLists.txt — L2 software mixer + SPSC stream + lock-free command queue.
phx_add_module(audio
  SRC
    src/mixer.cpp
    src/stream.cpp
  DEPS
    phx_core phx_memory
)
