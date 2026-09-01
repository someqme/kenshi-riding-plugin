# callers.py - "who calls this?" straight out of Kenshi_x64.exe.  Offline; no game, no build.
#
#   python tools\callers.py 0x5CC820              # direct call/jmp sites -> that rva
#   python tools\callers.py 0x5CC820 0x641510     # several targets at once
#   python tools\callers.py --vcall 0x2D0         # indirect `call/jmp [reg+0x2D0]` sites
#   python tools\callers.py --ptr 0x5CC820 0x302B5  # 8-byte VA references (vftables live in .rdata)
#   python tools\callers.py --field 0x6D8            # `<op> [reg+0x6D8]` member-access sites
#   python tools\callers.py --calls 0x5B5980 2       # direct callees of that body, depth 2
#   python tools\callers.py --ret 0x5DB749           # a probe's `site=` -> the real call site
#   python tools\callers.py --strings 0x5D0DB0       # string constants a body touches (identity)
#   python tools\callers.py --import "AnimationState" # imported API by regex + its call sites
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
# ⚠️ The `logical entry` that `--ret` prints comes from logical_entry(), which is a HEURISTIC -
# read its docstring before quoting it.  A chunk with its own nonzero prolog stops the walk early,
# so the number can name a chunk that nothing ever calls (RE_NOTES 18.10).

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


def _ffcall_len(b, i):
    """Length of the `call r/m64` (FF /2) starting at b[i], or None if that is not one."""
    j = i
    while j < len(b) and 0x40 <= b[j] <= 0x4F:      # REX prefixes
        j += 1
    if j + 1 >= len(b) or b[j] != 0xFF or ((b[j + 1] >> 3) & 7) != 2:
        return None
    modrm = b[j + 1]
    mod, rm = modrm >> 6, modrm & 7
    ln = j + 2 - i
    if mod == 3:
        return ln
    if rm == 4:
        ln += 1                                     # SIB
    if mod == 0:
        ln += 4 if rm == 5 else 0                   # rip-relative
    elif mod == 1:
        ln += 1
    else:
        ln += 4
    return ln


def call_ending_at(pe, rva):
    """[(site, length, text, target_or_None)] for every call instruction whose NEXT address is rva.

    A return address is by definition the address right after a call, so this is the reverse of
    direct(): it turns a logged return address back into its call site.  Both encodings that can
    reach a hooked engine function are covered - `E8 rel32` (direct, the target is decoded and
    ILT-resolved) and `FF /2` (indirect: through the vftable, so the target is a slot, not a
    value visible here).
    """
    blob, sec = pe.read(rva - 16, 16)
    if not blob:
        return []
    b, out = bytearray(blob), []
    for ln in range(2, 9):
        i = 16 - ln
        if i < 0:
            continue
        if ln == 5 and b[i] == 0xE8:
            t = rva + struct.unpack_from('<i', blob, i + 1)[0]
            out.append((rva - ln, ln, 'call 0x%X' % t, ke_pe.deref_thunk(pe, t)))
        elif _ffcall_len(b, i) == ln:
            out.append((rva - ln, ln, 'call [reg%s]' % (
                '+0x%X' % struct.unpack_from('<i', blob, i + ln - 4)[0] if ln >= 6 else ''), None))
    return out


def logical_entry(pe, rva):
    """(chunk_begin, logical_begin): MSVC splits one function into several .pdata records, and this
    walks UP from rva's chunk while the chunk has prolog=0, on the theory that a record without a
    prologue must be a continuation (RE_NOTES 18.7).

    ⚠️ HEURISTIC, NOT A VERDICT (RE_NOTES 18.10): a separated/chained chunk can carry its OWN
    nonzero prolog size, so the walk stops early and reports a chunk as if it were the entry.
    Live counterexample: 0x5CEA40 is its own .pdata record with prolog=40, yet it has 0 direct
    sites and 0 pointer references, and the decompiler folds it into 0x5CE9C0 - the real entry,
    which this function does NOT return for an rva inside 0x5CEBA0.  `prolog != 0` therefore
    proves nothing about callability.  To settle an entry, all three must hold: it is a .pdata
    record, SOMETHING reaches it (a direct site from direct() or a pointer reference from ptr()),
    and the decompiler's getFunctionContaining agrees.
    """
    f = pe.func_of(rva)
    if not f:
        return None, None
    b = f[0]
    e = b
    idx = [x[0] for x in pe.funcs].index(b)
    while idx > 0 and not pe.prolog_size(e):
        idx -= 1
        e = pe.funcs[idx][0]
    return b, e


def strings_in(pe, rva, limit=24):
    """[(site, target, text)] for `lea reg,[rip+disp32]` inside that .pdata record whose target is
    a printable string.  This is the cheap way to give a function an identity without a
    decompiler (RE_NOTES 18.6): the string constants a body touches usually name what it does.
    ⚠️ It reads ONE record, so a logical function split across chunks needs each chunk asked."""
    rec = pe.func_of(rva)
    if not rec:
        return []
    blob, _ = pe.read(rec[0], rec[1] - rec[0])
    b, out = bytearray(blob), []
    for i in range(len(b) - 7):
        if not (0x48 <= b[i] <= 0x4F and b[i + 1] == 0x8D and (b[i + 2] >> 6) == 0
                and (b[i + 2] & 7) == 5):
            continue
        site = rec[0] + i
        tgt = site + 7 + struct.unpack_from('<i', blob, i + 3)[0]
        raw, sec = pe.read(tgt, 48)
        if sec not in ('.rdata', '.data'):
            continue
        s = bytes(raw).split(b'\0')[0]
        if 3 <= len(s) <= 47 and all(32 <= c < 127 for c in bytearray(s)):
            out.append((site, tgt, s.decode('latin1')))
            if len(out) >= limit:
                break
    return out


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


def iat_sites(pe, iat_rvas):
    """{iat_rva: [(site, kind)]} for `call/jmp qword [rip+disp32]` landing on those IAT slots.

    The fourth way into a function, and the only one the other scanners miss: a cross-module
    call is `FF 15` (call) / `FF 25` (the ILT thunk's jmp) through the import table, so neither
    direct() (rel32) nor indirect() ([reg+disp]) sees it.  Pair it with ke_pe.imports(): the
    import list says WHETHER the exe can call an API, this says FROM WHERE.
    ⚠️ An import whose only site is its own `FF 25` ILT thunk is reached by `E8 -> thunk`
    instead; feed that thunk's rva back into direct() to find the real callers.
    """
    want, out = set(iat_rvas), {}
    for name, va, vsz, ptr, rsz in pe.secs:
        if name != '.text':
            continue
        blob = pe.d[ptr:ptr + rsz]
        bb = bytearray(blob)
        for i, b in enumerate(bb):
            if b != 0xFF or i + 6 > len(bb) or bb[i + 1] not in (0x15, 0x25):
                continue
            tgt = va + i + 6 + struct.unpack_from('<i', blob, i + 2)[0]
            if tgt in want:
                out.setdefault(tgt, []).append(
                    (va + i, 'call' if bb[i + 1] == 0x15 else 'jmp'))
    return out


def main(argv):
    if not argv:
        return (__doc__ or 'usage: callers.py <true rva> [...] | --vcall <slot off>'
                           ' | --ptr <rva> [...] | --field <member off> [...]')
    pe = ke_pe.PE(ke_pe.KENSHI_EXE)
    print('exe   %s\nbase  0x%X   .pdata records %d\n' %
          (pe.path, pe.base, len(pe.funcs)))

    if argv[0] == '--import':
        import re
        rx = re.compile(argv[1]) if len(argv) > 1 else None
        imps = ke_pe.imports(pe)
        sel = [t for t in imps if rx is None or rx.search(t[1])]
        print('imports %d total, %d matching %r\n' %
              (len(imps), len(sel), argv[1] if rx else '(all)'))
        sites = iat_sites(pe, [t[2] for t in sel])
        for dll, sym, iat in sel:
            hits = sites.get(iat, [])
            print('%s  [iat 0x%X]  %d site(s)   (%s)' % (sym, iat, len(hits), dll))
            for site, kind in hits:
                print('  0x%-8X %-4s %s' % (site, kind, where(pe, site)))
        return 0

    if argv[0] == '--calls':
        rva = int(argv[1], 0)
        depth = int(argv[2], 0) if len(argv) > 2 else 1
        out = callees(pe, rva, depth)
        print('direct callees of 0x%X (depth %d) : %d' % (rva, depth, len(out)))
        for t in sorted(out):
            print('  0x%-8X %-26s from %s' % (
                t, pe.classify(t), ','.join('0x%X' % c for c in sorted(out[t]))))
        return 0

    if argv[0] == '--ret':
        for a in argv[1:]:
            logged = int(a, 0)
            print('logged site=0x%X  (as printed by a naming probe)' % logged)
            for lbl, r in (('raw', logged),
                           ('+0x%X' % ke_pe.RUNTIME_RVA_DELTA,
                            logged + ke_pe.RUNTIME_RVA_DELTA)):
                hits = call_ending_at(pe, r)
                chunk, entry = logical_entry(pe, r)
                print('  %-8s 0x%-8X %-22s %s' % (
                    lbl, r, where(pe, r),
                    ('logical entry 0x%X' % entry) if entry and entry != chunk else ''))
                for site, ln, text, tgt in hits:
                    print('      call site 0x%-8X len %d  %-22s %s' % (
                        site, ln, text,
                        ('-> 0x%X %s' % (tgt, pe.classify(tgt))) if tgt else '(indirect/vftable)'))
                if not hits:
                    print('      no call instruction ends here => NOT a return address')
            print('')
        return 0

    if argv[0] == '--strings':
        for a in argv[1:]:
            rva = int(a, 0)
            chunk, entry = logical_entry(pe, rva)
            print('0x%X  %s  logical entry 0x%X' % (rva, where(pe, rva), entry or 0))
            for site, tgt, s in strings_in(pe, rva):
                print('  0x%-8X -> 0x%-8X %r' % (site, tgt, s))
            print('')
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
