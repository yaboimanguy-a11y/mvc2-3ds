#!/usr/bin/env python3
"""luma_dump.py crash_dump_XXXX.dmp: decode a Luma3DS ARM11 exception dump (exception type, registers;
writes the dumped stack and code bytes next to it as .stack / .code).  Look up pc / lr in the
matching mvc2.elf, e.g.: llvm-addr2line -fe build/port-3ds/mvc2.elf 0x<pc>"""
import struct, sys
d = open(sys.argv[1], 'rb').read()
m1, m2, vmin, vmaj, proc, typ, total, regsz, codesz, stacksz, addsz = struct.unpack_from('<IIHHIIIIIII', d, 0)
print(f"magic {m1:08X} {m2:08X}  luma dump v{vmaj}.{vmin}  size {len(d)} (header says {total})")
print(f"processor ARM{proc & 0xffff} core {proc >> 16}   type {typ} = " + {0:'FIQ',1:'undefined instruction',2:'prefetch abort',3:'data abort'}.get(typ,'?'))
off = 40
regs = struct.unpack_from(f'<{regsz//4}I', d, off); off += regsz
names = [f'r{i}' for i in range(13)] + ['sp','lr','pc','cpsr','dfsr','ifsr','far','fpexc','fpinst','fpinst2']
for i, v in enumerate(regs):
    print(f"  {names[i] if i < len(names) else '?':7s} {v:08X}")
code = d[off:off+codesz]; off += codesz
stack = d[off:off+stacksz]; off += stacksz
add = d[off:off+addsz]
print("code bytes", codesz, " stack bytes", stacksz, " extra", add.replace(b'\0', b' ').strip())
open(sys.argv[1] + '.stack', 'wb').write(stack)
open(sys.argv[1] + '.code', 'wb').write(code)
