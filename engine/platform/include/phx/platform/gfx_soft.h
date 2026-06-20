/* phx/platform/gfx_soft.h — software-tier graphics contract.
 * A platform whose render tier is software (headless null, or SDL blitting a CPU surface)
 * exposes an RGBA8 framebuffer the software renderer draws into. The render backend locks
 * it each frame; the platform's present() displays whatever is in it.
 * Pixel order is [R,G,B,A] in memory (value = R | G<<8 | B<<16 | A<<24). */
#ifndef PHX_PLATFORM_GFX_SOFT_H
#define PHX_PLATFORM_GFX_SOFT_H

#include "phx/platform/platform.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct phx_soft_fb {
    uint32_t* pixels;   /* width*height, RGBA8 */
    int32_t   w;
    int32_t   h;
} phx_soft_fb;

/* Implemented by the software-capable backend that is linked (null / sdl). */
phx_soft_fb phx_gfx_soft_lock(phx_gfx* gfx);

#ifdef __cplusplus
}
#endif
#endif /* PHX_PLATFORM_GFX_SOFT_H */
