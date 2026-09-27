// phx/runtime/devtools.h — DEVELOPER TOOLS in a running game (desktop builds only; the engine's
// desktop entry installs them, a console build never links them):
//
//   F1          show / hide the overlay
//   F5          pause / resume the simulation (rendering goes on)
//   F6          advance exactly one fixed step (pauses first)
//   F7 / F8     select the previous / next entity; or click one
//
// The overlay is a live INSPECTOR for the selected entity: its prefab type and spawn, Transform,
// Body, collider and animation, plus every reflected component it has (phx/ecs/reflect.h: the
// stock behaviours and the game's own PHX_COMPONENTs) with its fields' current values. The
// selected entity's collider is outlined in the world. It is drawn straight into the software
// framebuffer after the game's frame, so it needs no font asset and never touches the game's
// sprites or budgets.
#ifndef PHX_RUNTIME_DEVTOOLS_H
#define PHX_RUNTIME_DEVTOOLS_H

#include "phx/runtime/app.h"

namespace phx {

// Install the tools on `app` (before app.run()). Host-only: engine/runtime/src/devtools.cpp.
void install_devtools(App& app);

} // namespace phx
#endif // PHX_RUNTIME_DEVTOOLS_H
