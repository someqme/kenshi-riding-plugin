# callers.py - "who calls this?" straight out of Kenshi_x64.exe.  Offline; no game, no build.
#
#   python tools\callers.py 0x5CC820              # direct call/jmp sites -> that rva
#   python tools\callers.py 0x5CC820 0x641510     # several targets at once
#   python tools\callers.py --vcall 0x2D0         # indirect `call/jmp [reg+0x2D0]` sites
#   python tools\callers.py --ptr 0x5CC820 0x302B5  # 8-byte VA references (vftables live in .rdata)
#   python tools\callers.py --field 0x6D8            # `<op> [reg+0x6D8]` member-access sites
#   python tools\callers.py --calls 0x5B5980 2       # direct callees of that body, depth 2
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
# ⚠️ A .pdata record is a CHUNK, not necessarily a function ENTRY: MSVC splits one logical
# function into several records (separated/chained unwind info).  So `in 0x<X>+0x<N>` naming
# an address with 0 direct callers AND 0 pointer references usually means X is the tail chunk
# of the function just before it - check whether the previous record flows into it before
# treating X as a callable entry.  (Live example: 0x5CC77C is the tail of 0x5CC760.)

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


def callees(pe, rva, depth=1):
    """{callee_rva: set(caller_rva)} reachable by direct E8 from `rva`'s body, ILT-resolved.

    The other direction of direct(): "what does THIS function call", for walking a known
    entry (say the carried-state update) toward a function you already have a name for.
    ⚠️ Direct calls only - a virtual call inside the body is invisible here, so a short
    callee list does NOT mean the function is a leaf of the call graph.
    ⚠️ Most of what comes back on a first hop is CRT/std glue (std::string assign, the
    /GS cookie check, operator delete).  Identify those before reading anything into an
    "empty" intersection with some other set - the interesting edges leave via vftables.
    """
    seen, frontier, out = set(), [rva], {}
    for _ in range(max(1, depth)):
        nxt = []
        for f in frontier:
            if f in seen:
                continue
            seen.add(f)
            rec = pe.func_of(f)
            if not rec:
                continue
            blob, _ = pe.read(rec[0], rec[1] - rec[0])
            for i, b in enumerate(bytearray(blob)):
                if b != 0xE8 or i + 5 > len(blob):
                    continue
                site = rec[0] + i
                tgt = site + 5 + struct.unpack_from('<i', blob, i + 1)[0]
                if tgt <= 0 or pe.off(tgt)[1] != '.text':
                    continue                      # false E8 inside a longer instruction
                tgt = ke_pe.deref_thunk(pe, tgt)
                out.setdefault(tgt, set()).add(f)
                nxt.append(tgt)
        frontier = nxt
    return out


def where(pe, rva):
    f = pe.func_of(rva)
    return ('in 0x%X+0x%X' % (f[0], rva - f[0])) if f else 'NOT in any .pdata function'


# opcode -> (mnemonic, is_write).  Only the forms that actually show up in member access;
# anything not listed here is skipped rather than guessed at.
FIELD_OPS = {
    0x88: ('mov8  st', True),  0x89: ('mov   st', True),
    0x8A: ('mov8  ld', False), 0x8B: ('mov   ld', False),
    0x8D: ('lea     ', False),
    0xC7: ('mov   imm', True), 0xC6: ('mov8  imm', True),
    0x39: ('cmp   st', False), 0x3B: ('cmp   ld', False),
    0x83: ('grp1  imm', False),  # cmp/add/... [mem], imm8 - /7 is cmp, treated read-only
    0x85: ('test    ', False),
    0x01: ('add   st', True),  0x03: ('add   ld', False),
    0x29: ('sub   st', True),  0x2B: ('sub   ld', False),
    0xFF: ('grp5    ', False),  # call/push/inc [mem] - /2 call handled by indirect()
}


def field(pe, disp):
    """[(site_rva, mnemonic, is_write, reg, ctx)] for `<op> [reg+disp32]` with that disp32.

    Same mod=10 shape as indirect(), widened to the data-moving opcodes.  ⚠️ A member
    offset is NOT class-specific: any class with a field at that offset produces hits, so
    the containing function is the only thing that makes a hit meaningful - cross-reference
    it against RVAs you already know.
    ⚠️ `reg` == 'sib' means the modrm carried a SIB byte; when the ctx hex shows `24` right
    after the modrm that is `[rsp+disp32]` == a STACK FRAME, not a member.  Drop those.
    """
    REG = ['rax', 'rcx', 'rdx', 'rbx', 'rsp', 'rbp', 'rsi', 'rdi']
    out = []
    for name, va, vsz, ptr, rsz in pe.secs:
        if name != '.text':
            continue
        blob = pe.d[ptr:ptr + rsz]
        bb = bytearray(blob)
        pat = struct.pack('<i', disp)
        i = -1
        while True:
            i = blob.find(pat, i + 1)
            if i < 0:
                break
            # walk back: [disp32] <- modrm(mod=10) <- [SIB] <- opcode <- [0F] <- [REX/66/F2/F3]
            for sib in (0, 1):
                m = i - 1 - sib
                if m < 1:
                    continue
                modrm = bb[m]
                if (modrm >> 6) != 0b10:
                    continue
                rm = modrm & 7
                if (rm == 4) != bool(sib):       # rm==100 means a SIB byte is present
                    continue
                op = bb[m - 1]
                if op not in FIELD_OPS:
                    continue
                mnem, wr = FIELD_OPS[op]
                k = m - 2
                rex = 0
                while k >= 0 and (0x40 <= bb[k] <= 0x4F or bb[k] in (0x66, 0xF2, 0xF3)):
                    if 0x40 <= bb[k] <= 0x4F:
                        rex = bb[k]
                    k -= 1
                base = ('r%d' % (8 + rm)) if (rex & 1) else REG[rm]
                out.append((va + k + 1, mnem, wr,
                            'sib' if rm == 4 else base,
                            blob[m - 1:i + 4].hex()))
                break
    return out


def main(argv):
    if not argv:
        return (__doc__ or 'usage: callers.py <true rva> [...] | --vcall <slot off>'
                           ' | --ptr <rva> [...] | --field <member off> [...]')
    pe = ke_pe.PE(ke_pe.KENSHI_EXE)
    print('exe   %s\nbase  0x%X   .pdata records %d\n' %
          (pe.path, pe.base, len(pe.funcs)))

    if argv[0] == '--calls':
        rva = int(argv[1], 0)
        depth = int(argv[2], 0) if len(argv) > 2 else 1
        out = callees(pe, rva, depth)
        print('direct callees of 0x%X (depth %d) : %d' % (rva, depth, len(out)))
        for t in sorted(out):
            print('  0x%-8X %-26s from %s' % (
                t, pe.classify(t), ','.join('0x%X' % c for c in sorted(out[t]))))
        return 0

    if argv[0] == '--field':
        for a in argv[1:]:
            disp = int(a, 0)
            hits = field(pe, disp)
            print('member [reg+0x%X] : %d site(s)  (%d write)' %
                  (disp, len(hits), sum(1 for h in hits if h[2])))
            for site, mnem, wr, reg, ctx in hits:
                print('  0x%-8X %s %-4s %-3s %-22s %s' % (
                    site, mnem, 'W' if wr else 'r', reg, ctx, where(pe, site)))
            print('')
        return 0

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
