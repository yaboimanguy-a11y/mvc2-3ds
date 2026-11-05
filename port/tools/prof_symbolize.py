#!/usr/bin/env python3
"""prof_symbolize.py ELF LOG: the profiler build's report with function names.

mvc2_prof.3dsx (port/platform/3ds/prof_fn_3ds.c) writes its hottest functions
to mvc2_log.txt as addresses ("  12.3%  0x0012A4C0").  This prints the log with
each address followed by the function's name from the matching ELF
(build/port-3ds/mvc2_prof.elf of the same build), using llvm-nm or
arm-none-eabi-nm.
"""
import bisect
import os
import re
import shutil
import subprocess
import sys


def symbols(elf):
    nm = (shutil.which("llvm-nm") or "/opt/homebrew/opt/llvm/bin/llvm-nm")
    if not os.path.exists(nm):
        nm = os.path.join(os.environ.get("DEVKITARM", "/opt/devkitpro/devkitARM"), "bin/arm-none-eabi-nm")
    out = subprocess.run([nm, "-n", elf], check=True, capture_output=True, text=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in "tTwW":
            addrs.append(int(parts[0], 16) & ~1)
            names.append(parts[2])
    return addrs, names


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    addrs, names = symbols(sys.argv[1])
    pat = re.compile(r"^(\s+[\d.]+%\s+)0x([0-9A-Fa-f]{8})\s*$")
    for line in open(sys.argv[2], errors="replace"):
        m = pat.match(line.rstrip("\n"))
        if not m:
            sys.stdout.write(line)
            continue
        a = int(m.group(2), 16) & ~1
        i = bisect.bisect_right(addrs, a) - 1
        name = names[i] if i >= 0 else "?"
        off = a - addrs[i] if i >= 0 else 0
        print(f"{m.group(1)}0x{a:08X}  {name}{'+0x%X' % off if off else ''}")


if __name__ == "__main__":
    main()
