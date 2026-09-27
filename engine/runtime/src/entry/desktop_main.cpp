// engine/runtime/src/entry/desktop_main.cpp — main() for a game project on PC (Linux/Windows,
// the SDL platform) and for its headless host runs (the null platform). The bundle is a file
// the game mounts itself. Linked by `make game` when the project's sources have no main().
//
// PHX_DUMP_COMPONENTS=file: write the game's reflected components as JSON (for Phoenix Studio;
// `make game` does it after linking) and exit without booting a window.
#include "phx/runtime/main.h"

#include <cstdio>
#include <cstdlib>

int main() {
    if (const char* path = std::getenv("PHX_DUMP_COMPONENTS"); path && *path) {
        if (phx::write_component_schema(path)) return 0;
        std::fprintf(stderr, "cannot write the component schema to %s\n", path);
        return 1;
    }
    return phx::run_game(phx::game_instance(), phx::kTargetDesktop);
}
