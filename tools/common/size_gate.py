#!/usr/bin/env python3
"""size_gate.py — enforce the GBA ROM/RAM budget (docs/09 MVP gate: "Fits GBA budget — proven
by the CI size gate"). Classifies every ELF section by its load address into IWRAM (32 KB) or
EWRAM (256 KB), checks the ROM file size, and fails (exit 1) if any budget is exceeded.

Only STATIC footprint is measured here (code in ROM, .data/.bss in RAM). The runtime engine
arena (cfg.total_ram, ~160 KB carved from the EWRAM heap) and the framebuffer are allocated at
boot, not in the binary, so they are reported as a reminder but checked by the game's config.

Usage:
  size_gate.py --rom build/gba/x.gba --elf build/gba/x.elf [--size-tool arm-none-eabi-size]
"""
import argparse
import subprocess
import sys

# GBA memory map + budgets.
IWRAM_BASE, IWRAM_SIZE = 0x03000000, 32 * 1024
EWRAM_BASE, EWRAM_SIZE = 0x02000000, 256 * 1024
IWRAM_BUDGET = 28 * 1024            # leave ~4 KB headroom for the IWRAM stack
EWRAM_STATIC_BUDGET = 64 * 1024     # static EWRAM only; the ~160 KB arena is a runtime malloc
ROM_BUDGET = 1024 * 1024            # generous vs the ~120 KB demo; catches gross code/asset bloat


def iter_sections(size_tool, elf):
    out = subprocess.check_output([size_tool, "-A", elf], text=True)
    for line in out.splitlines():
        p = line.split()
        if len(p) < 3:
            continue
        try:
            size, addr = int(p[1]), int(p[2])
        except ValueError:
            continue
        if size > 0:
            yield size, addr


def region(addr):
    if IWRAM_BASE <= addr < IWRAM_BASE + IWRAM_SIZE:
        return "IWRAM"
    if EWRAM_BASE <= addr < EWRAM_BASE + EWRAM_SIZE:
        return "EWRAM"
    return "ROM"


def kb(n):
    return f"{n/1024:.1f} KB"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", required=True)
    ap.add_argument("--elf", required=True)
    ap.add_argument("--size-tool", default="arm-none-eabi-size")
    args = ap.parse_args()

    try:
        with open(args.rom, "rb") as f:
            rom_bytes = len(f.read())
    except OSError as e:
        print(f"size-gate: cannot read ROM: {e}", file=sys.stderr)
        return 1

    iwram = ewram = 0
    try:
        for size, addr in iter_sections(args.size_tool, args.elf):
            r = region(addr)
            if r == "IWRAM":
                iwram += size
            elif r == "EWRAM":
                ewram += size
    except (OSError, subprocess.CalledProcessError) as e:
        print(f"size-gate: cannot size ELF ({args.size_tool}): {e}", file=sys.stderr)
        return 1

    checks = [
        ("ROM (cartridge)", rom_bytes, ROM_BUDGET),
        ("IWRAM static (.data/.bss)", iwram, IWRAM_BUDGET),
        ("EWRAM static", ewram, EWRAM_STATIC_BUDGET),
    ]
    print(f"size-gate: {args.rom}")
    ok = True
    for name, used, budget in checks:
        status = "OK " if used <= budget else "OVER"
        if used > budget:
            ok = False
        print(f"  [{status}] {name:<26} {kb(used):>10} / {kb(budget):<9} ({used} / {budget} B)")
    print(f"  note: the ~160 KB engine arena + framebuffer are runtime EWRAM (cfg.total_ram),"
          f" not counted above")

    if not ok:
        print("size-gate: FAIL — a budget was exceeded", file=sys.stderr)
        return 1
    print("size-gate: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
