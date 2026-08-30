# callers.py - "who calls this?" straight out of Kenshi_x64.exe.  Offline; no game, no build.
#
#   python tools\callers.py 0x5CC820              # direct call/jmp sites -> that rva
#   python tools\callers.py 0x5CC820 0x641510     # several targets at once
#   python tools\callers.py --vcall 0x2D0         # indirect `call/jmp [reg+0x2D0]` sites
#   python tools\callers.py --ptr 0x5CC820 0x302B5  # 8-byte VA references (vftables live in .rdata)
#
# Why this exists: knowing a function's real entry (tools\hook_probe.py) does NOT tell you
# whether the engine reaches it *directly* or *through the vftable*.  That distinction decides
# which address a hook has to sit on:
#   * only indirect `[reg+slot]` sites  -> the call is virtual, so a CharacterHuman receiver
#     always lands on the human override; hook the override.
#   * direct `E8 rel32` sites to the BASE body -> some call site is non-virtual/qualified and
#     bypasses the override entirely; hooking the override would never fire on that path.
#
# `--ptr` is the third leg and the decisive one: it counts 8-byte occurrences of a target's
# VIRTUAL ADDRESS anywhere in the image.  "0 direct sites AND the only pointer references are
# the function's own ILT thunks, one apiece, both in .rdata" == the vftables are the sole way in,
# i.e. dispatch is purely virtual.  Without it, "0 direct sites" alone could just mean the call
# goes through a function pointer held in data.
#
# ⚠️ Static scan, so it sees only call sites encoded in the image: not calls made through a
# function pointer held in data, and not the RTTI-less thunks a /LTCG build may duplicate.
# "0 direct callers" therefore means "no direct caller in .text", not "nobody calls it".
# ⚠️ It scans every byte, so a byte pattern that merely LOOKS like `E8 rel32` inside a longer
# instruction (or inside data embedded in .text) can produce a false site.  Filter by
# `in <function>`: a real call site sits inside some .pdata function.

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ke_pe


def direct(pe, targets):
    """[(site_rva, kind, target)] for E8/E9 rel32 whose target is in `targets`."""
    out = []
    for name, va, vsz, ptr, rsz in pe.secs:
        if name != '.text':
            continue
        blob = pe.d[ptr:ptr + rsz]
        for i, b in enumerate(bytearray(blob)):
            if b != 0xE8 and b != 0xE9:
                continue
            if i + 5 > len(blob):
                break
            rel = struct.unpack_from('<i', blob, i + 1)[0]
            site = va + i
            tgt = site + 5 + rel
            if tgt in targets:
                out.append((site, 'call' if b == 0xE8 else 'jmp', tgt))
    return out


def indirect(pe, disp):
    """[(site_rva, kind, text)] for `call/jmp qword [reg+disp32]` with that disp32.

    Encoding: [REX] FF /2 (call) or /4 (jmp), mod=10 => disp32 follows the modrm
    (and a SIB byte when rm==100).
    """
    REG = ['rax', 'rcx', 'rdx', 'rbx', 'rsp', 'rbp', 'rsi', 'rdi']
    out = []
    for name, va, vsz, ptr, rsz in pe.secs:
        if name != '.text':
            continue
        blob = pe.d[ptr:ptr + rsz]
        bb = bytearray(blob)
        for i, b in enumerate(bb):
            if b != 0xFF or i + 6 > len(bb):
                continue
            modrm = bb[i + 1]
            if (modrm >> 6) != 0b10:                # need mod=10 (disp32)
                continue
            reg = (modrm >> 3) & 7
            if reg not in (2, 4):                   # /2 = call, /4 = jmp
                continue
            rm = modrm & 7
            k = i + 2 + (1 if rm == 4 else 0)       # skip SIB
            if k + 4 > len(bb):
                continue
            if struct.unpack_from('<i', blob, k)[0] != disp:
                continue
            rex = bb[i - 1] if i and 0x40 <= bb[i - 1] <= 0x4F else 0
            base = ('r%d' % (8 + rm)) if (rex & 1) else REG[rm]
            out.append((va + i - (1 if rex else 0),
                        'call' if reg == 2 else 'jmp',
                        '[%s+0x%X]' % ('sib' if rm == 4 else base, disp)))
    return out


def ptr(pe, rva):
    """[(hit_rva, section)] for every 8-byte occurrence of that rva's virtual address.

    findall() works in FILE OFFSETS, so each hit goes back through pe.rva(); a hit that
    lands outside every section maps to (None, None) and is reported as raw@<offset>.
    """
    pat = struct.pack('<Q', pe.base + rva)
    out = []
    for off in pe.findall(pat):
        r, sec = pe.rva(off)
        out.append((r, sec) if r is not None else ('raw@0x%X' % off, '(no section)'))
    return out


def where(pe, rva):
    f = pe.func_of(rva)
    return ('in 0x%X+0x%X' % (f[0], rva - f[0])) if f else 'NOT in any .pdata function'


def main(argv):
    if not argv:
        return __doc__ or 'usage: callers.py <true rva> [...] | --vcall <slot off> | --ptr <rva> [...]'
    pe = ke_pe.PE(ke_pe.KENSHI_EXE)
    print('exe   %s\nbase  0x%X   .pdata records %d\n' %
          (pe.path, pe.base, len(pe.funcs)))

    if argv[0] == '--vcall':
        disp = int(argv[1], 0)
        hits = indirect(pe, disp)
        print('indirect call/jmp with disp32 = 0x%X : %d site(s)' % (disp, len(hits)))
        for site, kind, text in hits:
            print('  0x%-8X %-4s %-16s %s' % (site, kind, text, where(pe, site)))
        return 0

    if argv[0] == '--ptr':
        for a in argv[1:]:
            t = int(a, 0)
            hits = ptr(pe, t)
            print('target 0x%X  -> %d pointer-sized reference(s)' % (t, len(hits)))
            for hit, sec in hits:
                print('  %-12s in %s' % (hit if isinstance(hit, str) else '0x%X' % hit, sec))
            print('')
        return 0

    targets = set(int(a, 0) for a in argv)
    hits = direct(pe, targets)
    for t in sorted(targets):
        mine = [h for h in hits if h[2] == t]
        print('target 0x%X  (%s, prolog=%s)  -> %d direct site(s)' %
              (t, pe.classify(t), pe.prolog_size(t), len(mine)))
        for site, kind, _ in mine:
            print('  0x%-8X %-4s %s' % (site, kind, where(pe, site)))
        print('')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
