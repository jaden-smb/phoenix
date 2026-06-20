// phx/core/caps.h — compile-time capability tier for the active platform.
// The build system defines exactly one PHX_TARGET_* macro (see cmake/caps_select.cmake).
// Modules read these constants to size buffers and select code paths at COMPILE time,
// so GBA constraints are visible in source instead of discovered at runtime.
#ifndef PHX_CORE_CAPS_H
#define PHX_CORE_CAPS_H

#include <cstdint>

// ---- Tier selection --------------------------------------------------------
#if defined(PHX_TARGET_GBA)
    #define PHX_CAPS_HAS_FLOAT_HW   0
    #define PHX_CAPS_HAS_FILESYSTEM 0
    #define PHX_CAPS_RENDER_TIER    0          // tile/sprite PPU
    #define PHX_CAPS_TOTAL_RAM      (224u * 1024u)   // EWRAM available to engine
    #define PHX_CAPS_SCRATCH_RAM    (24u  * 1024u)   // IWRAM for hot data
    #define PHX_CAPS_MAX_ENTITIES   512
    #define PHX_CAPS_MAX_SPRITES    128                // hardware OAM ceiling
    #define PHX_CAPS_AUDIO_CHANNELS 2
#elif defined(PHX_TARGET_PSP)
    #define PHX_CAPS_HAS_FLOAT_HW   1
    #define PHX_CAPS_HAS_FILESYSTEM 1
    #define PHX_CAPS_RENDER_TIER    1          // fixed-function GU
    #define PHX_CAPS_TOTAL_RAM      (24u * 1024u * 1024u)
    #define PHX_CAPS_SCRATCH_RAM    0
    #define PHX_CAPS_MAX_ENTITIES   8192
    #define PHX_CAPS_MAX_SPRITES    1024
    #define PHX_CAPS_AUDIO_CHANNELS 8
#else // PHX_TARGET_PC (linux/windows)
    #define PHX_CAPS_HAS_FLOAT_HW   1
    #define PHX_CAPS_HAS_FILESYSTEM 1
    #define PHX_CAPS_RENDER_TIER    2          // programmable
    #define PHX_CAPS_TOTAL_RAM      (256u * 1024u * 1024u)
    #define PHX_CAPS_SCRATCH_RAM    0
    #define PHX_CAPS_MAX_ENTITIES   65536
    #define PHX_CAPS_MAX_SPRITES    16384
    #define PHX_CAPS_AUDIO_CHANNELS 32
#endif

namespace phx {

struct Caps {
    uint32_t total_main_ram;
    uint32_t scratch_ram;
    uint32_t max_entities;     // ECS index is 24-bit; ceiling is per-tier (GBA 512, PC 65536)
    uint32_t max_sprites;
    uint8_t  has_float_hw;
    uint8_t  has_filesystem;
    uint8_t  render_tier;
    uint8_t  audio_channels;
};

// The single source of truth, assembled from the macros above.
inline constexpr Caps caps() {
    return Caps{
        PHX_CAPS_TOTAL_RAM, PHX_CAPS_SCRATCH_RAM,
        PHX_CAPS_MAX_ENTITIES, PHX_CAPS_MAX_SPRITES,
        PHX_CAPS_HAS_FLOAT_HW, PHX_CAPS_HAS_FILESYSTEM,
        PHX_CAPS_RENDER_TIER, PHX_CAPS_AUDIO_CHANNELS,
    };
}

} // namespace phx
#endif // PHX_CORE_CAPS_H
