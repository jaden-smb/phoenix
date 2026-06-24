/* phx/platform/platform.h — the C-ABI seam isolating every OS/hardware dependency.
 * Above this line: portable C++17. Below it: exactly one backend, chosen by the linker.
 * No phx module other than the platform backends may include an OS/SDK header.
 * See docs/02-platform-layer.md. */
#ifndef PHX_PLATFORM_PLATFORM_H
#define PHX_PLATFORM_PLATFORM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- opaque handles ---- */
typedef struct phx_window phx_window;
typedef struct phx_gfx    phx_gfx;     /* graphics device, consumed by render backend */
typedef struct phx_audio  phx_audio;   /* audio device, consumed by audio mixer       */
typedef struct phx_file   phx_file;    /* file / mmap handle                          */

/* ---- normalized input frame (filled by platform, decoded by input module) ----
 * Canonical button bit order is fixed so Button::Jump means the same everywhere. */
typedef struct phx_input_raw {
    uint32_t buttons;          /* bitmask in canonical phx_button order */
    int16_t  axis[4];          /* lx, ly, rx, ry  (-32768..32767), 0 where absent */
    int16_t  pointer_x;        /* mouse/touch; -1 if none */
    int16_t  pointer_y;
    uint8_t  pointer_down;
    uint8_t  connected_pads;
} phx_input_raw;

/* ---- creation descriptor ---- */
typedef struct phx_platform_desc {
    const char* title;
    int32_t     width;
    int32_t     height;
    int32_t     vsync;
    void*       root_arena;    /* backends allocate their state here, never via malloc */
    size_t      root_arena_size;
} phx_platform_desc;

typedef enum phx_log_level {
    PHX_LOG_TRACE = 0, PHX_LOG_DEBUG, PHX_LOG_INFO, PHX_LOG_WARN, PHX_LOG_ERROR
} phx_log_level;

/* ---- the seam: a flat struct of function pointers ---- */
typedef struct phx_platform {
    /* lifecycle */
    int   (*init)(const phx_platform_desc* desc);
    void  (*shutdown)(void);

    /* clock */
    uint64_t (*clock_ns)(void);                 /* monotonic */
    void  (*sleep_ns)(uint64_t ns);             /* may be a no-op on consoles */

    /* window / present */
    int   (*pump_events)(void);                 /* returns 0 to request quit */
    void  (*present)(void);                     /* swap buffers / wait vblank */

    /* device handles handed to higher layers */
    phx_gfx*   (*gfx)(void);
    phx_audio* (*audio)(void);

    /* input */
    void  (*poll_input)(phx_input_raw* out);

    /* file / asset access — map returns a stable zero-copy view (ROM ptr on GBA) */
    phx_file*   (*open)(const char* path, size_t* out_size);
    const void* (*map)(phx_file*);
    void        (*close)(phx_file*);

    /* persistent save storage — a tiny key->blob store: a file on PC/PSP, battery-backed SRAM
     * on GBA (single-slot; key ignored there). save() writes `size` bytes; load() reads up to
     * `cap` into `out` and sets *out_size to the bytes read. Both return 0 on success, non-zero
     * on failure (incl. "no save present"). The caller validates its own magic/version. */
    int   (*save)(const char* key, const void* data, uint32_t size);
    int   (*load)(const char* key, void* out, uint32_t cap, uint32_t* out_size);

    /* logging sink */
    void  (*log)(phx_log_level level, const char* msg);
} phx_platform;

/* Each backend (sdl/gba/psp/...) exports exactly this one symbol.
 * CMake links exactly one backend translation unit. No runtime platform branching. */
const phx_platform* phx_platform_get(void);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* PHX_PLATFORM_PLATFORM_H */
