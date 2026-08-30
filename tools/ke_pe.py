# ke_pe.py - static PE inspection for Kenshi RE work (no game, no build needed).
#
# Why this exists: the KenshiLib headers' "RVA = 0x..." comments are NOT the RVAs of
# the installed Kenshi_x64.exe.  For the 1.0.65 exe the true entry is
#     true_rva = header_rva + 0x780
# (see RidingPlugin_RE_NOTES.md 18.1 for how that was established and re-derived,
#  and 18.2 for why a candidate must always be confirmed against .pdata).
# This module gives the two oracles needed to check any candidate hook target:
#   * pdata()  - the exe's own exception table: "is this rva REALLY a function entry?"
#   * rtti()   - class vftable via RTTI, for virtuals whose slot offset is known.
#
# Usage: see tools/hook_probe.py

import struct

KENSHI_EXE = r'D:\steam\steamapps\common\Kenshi\Kenshi_x64.exe'
HEADER_RVA_DELTA = 0x780          # 1.0.65; re-derive with hook_probe.py --delta


# --------------------------------------------------------------- PE basics ---

class PE(object):
    def __init__(self, path):
        self.path = path
        self.d = open(path, 'rb').read()
        d = self.d
        pe = struct.unpack_from('<I', d, 0x3C)[0]
        assert d[pe:pe + 4] == b'PE\0\0', 'not a PE file'
        nsec = struct.unpack_from('<H', d, pe + 6)[0]
        optsz = struct.unpack_from('<H', d, pe + 20)[0]
        self.base = struct.unpack_from('<Q', d, pe + 24 + 24)[0]
        self.pe = pe
        self.secs = []
        tbl = pe + 24 + optsz
        for i in range(nsec):
            o = tbl + 40 * i
            name = d[o:o + 8].rstrip(b'\0').decode('latin1')
            vsz, va, rsz, ptr = struct.unpack_from('<IIII', d, o + 8)
            self.secs.append((name, va, vsz, ptr, rsz))
        self._load_pdata()

    # rva <-> file offset ---------------------------------------------------
    def off(self, rva):
        for name, va, vsz, ptr, rsz in self.secs:
            if va <= rva < va + max(vsz, rsz):
                return ptr + (rva - va), name
        return None, None

    def rva(self, off):
        for name, va, vsz, ptr, rsz in self.secs:
            if ptr <= off < ptr + rsz:
                return va + (off - ptr), name
        return None, None

    def read(self, rva, n):
        o, sec = self.off(rva)
        return (self.d[o:o + n], sec) if o is not None else (b'', None)

    def u32(self, rva):
        return struct.unpack_from('<I', self.read(rva, 4)[0])[0]

    def u64(self, rva):
        return struct.unpack_from('<Q', self.read(rva, 8)[0])[0]

    def findall(self, needle, start=0):
        out, i = [], start
        while True:
            i = self.d.find(needle, i)
            if i < 0:
                return out
            out.append(i)
            i += 1

    # ------------------------------------------------- exception directory --
    def _load_pdata(self):
        d, pe = self.d, self.pe
        assert struct.unpack_from('<H', d, pe + 24)[0] == 0x20B, 'not PE32+'
        rva, size = struct.unpack_from('<II', d, pe + 24 + 112 + 3 * 8)
        o, _ = self.off(rva)
        self.funcs = sorted(struct.unpack_from('<III', d, o + 12 * i)
                            for i in range(size // 12))
        self.entries = set(f[0] for f in self.funcs)

    def func_of(self, rva):
        """RUNTIME_FUNCTION (begin, end, unwind) covering rva, or None."""
        lo, hi = 0, len(self.funcs) - 1
        while lo <= hi:
            m = (lo + hi) // 2
            b, e, u = self.funcs[m]
            if rva < b:
                hi = m - 1
            elif rva >= e:
                lo = m + 1
            else:
                return self.funcs[m]
        return None

    def prolog_size(self, rva):
        f = self.func_of(rva)
        if not f:
            return None
        o, _ = self.off(f[2] & ~1)
        return struct.unpack_from('<BBBB', self.d, o)[1]

    def classify(self, rva):
        """'entry' | 'mid+N' | 'padding' | 'leaf' | 'unmapped'"""
        b, sec = self.read(rva, 1)
        if not b:
            return 'unmapped'
        if rva in self.entries:
            return 'entry'
        f = self.func_of(rva)
        if f:
            return 'mid+%d' % (rva - f[0])
        return 'padding' if b[0] == 0xCC else 'leaf'


# ------------------------------------------------------------------- RTTI ---

def vftables(pe, cls):
    """RTTI walk: '.?AV<cls>@@' type descriptor -> COL -> vftable rva(s)."""
    out = []
    for soff in pe.findall(('.?AV%s@@' % cls).encode('ascii') + b'\0'):
        srva, _ = pe.rva(soff)
        if srva is None:
            continue
        td = srva - 16                              # TypeDescriptor header
        for coff in pe.findall(struct.pack('<I', td)):
            crva, csec = pe.rva(coff)
            if crva is None or csec not in ('.rdata', '.data'):
                continue
            col = crva - 12                         # pTypeDescriptor is at COL+12
            b, _ = pe.read(col, 24)
            if len(b) < 24:
                continue
            sig, thisoff, cd, ptd, pcd, pself = struct.unpack('<IIIIII', b)
            if sig != 1 or ptd != td or pself != col:
                continue
            for poff in pe.findall(struct.pack('<Q', pe.base + col)):
                prva, psec = pe.rva(poff)
                if prva is not None and psec == '.rdata':
                    out.append((prva + 8, col, thisoff))
    return out


def deref_thunk(pe, rva, depth=3):
    """Follow /INCREMENTAL ILT jumps (E9 rel32) to the real function."""
    for _ in range(depth):
        b, _ = pe.read(rva, 5)
        if len(b) == 5 and b[0] == 0xE9:
            rva = rva + 5 + struct.unpack_from('<i', b, 1)[0]
        else:
            break
    return rva


def vslot(pe, cls, slot_off):
    """Real rva behind <cls>'s vftable slot at byte offset slot_off, or None."""
    for vt, col, thisoff in vftables(pe, cls):
        if thisoff:
            continue
        return deref_thunk(pe, pe.u64(vt + slot_off) - pe.base)
    return None
