// engine/runtime/src/entry/gba_main.cpp — main() for a game project on Game Boy Advance, the
// native-PPU build (the shipping GBA configuration; see examples/emberwing/src/gba_ppu_main.cpp).
// Linked by `make game-gba`, which bakes the project's tier-0 bundle and links it into cartridge
// ROM as `phx_game_phxp` (tools/common/bin2s.py --name). The GBA has no filesystem: the game's
// ResourceCache::mount() gets this bundle whatever path it names.
#include "phx/runtime/main.h"

extern "C" const unsigned char phx_game_phxp[];
extern "C" const unsigned int  phx_game_phxp_size;

// Hooks exported by the GBA platform backend (not part of the C seam).
extern "C" void phx_gba_set_bundle(const void* data, unsigned long size);
extern "C" void phx_gba_set_direct(int on);

int main() {
    phx_gba_set_bundle(phx_game_phxp, phx_game_phxp_size);
    phx_gba_set_direct(1);           // the PPU owns the display: the platform skips the 150 KB soft fb
    return phx::run_game(phx::game_instance(), phx::kTargetGba);
}
