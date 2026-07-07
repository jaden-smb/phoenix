// phx/core/hot.h — code-placement attribute for the few frame-path functions that must run
// from fast memory on cartridge consoles.
//
// On GBA, code normally executes as Thumb straight from cartridge ROM (16-bit bus, 3/1 wait
// states): fine for cold code, ~2-3× too slow for the per-sample / per-body inner loops.
// PHX_HOT_CODE moves a function into IWRAM (32-bit bus, zero wait states) as ARM code —
// devkitARM's crt0/linker script (`gba.specs`) copy the `.iwram` section at boot.
// `noinline` keeps the body from being inlined back into ROM callers (which would defeat the
// placement); `long_call` lets ROM (0x08...) reach IWRAM (0x03...) beyond BL's ±4 MB range.
//
// Everywhere else (PC, PSP, host test tiers) the macro is empty — zero effect on codegen —
// so annotated engine code stays fully portable and both scalar tiers stay byte-identical.
// Use it ONLY for measured hot spots: IWRAM is 32 KB shared with .data/.bss and the stack,
// and `make size-gate` enforces the budget.
#ifndef PHX_CORE_HOT_H
#define PHX_CORE_HOT_H

#if defined(PHX_GBA_HW)
#define PHX_HOT_CODE __attribute__((section(".iwram"), target("arm"), long_call, noinline))
#else
#define PHX_HOT_CODE
#endif

#endif // PHX_CORE_HOT_H
