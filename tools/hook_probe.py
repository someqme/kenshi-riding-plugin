# hook_probe.py - "is this header RVA a hookable entry in the installed exe?"
#
#   python tools\hook_probe.py                 # the recorded control set
#   python tools\hook_probe.py 0x5CC0A0 ...    # probe header RVAs from KenshiLib headers
#   python tools\hook_probe.py --true 0x5CC820 # probe true RVAs directly (no delta)
#   python tools\hook_probe.py --delta         # re-derive the header->true delta
#   python tools\hook_probe.py --vslot CharacterHuman 0x2D0
#
# Reading the output:
#   ENTRY  = the exe's own .pdata says this is a function entry -> address is right.
#   MID+N  = you are N bytes inside some other function -> WRONG address, do not hook.
#   leaf   = not in .pdata at all.  Check the bytes: "C2 00 00 CC CC ..." means the
#            delta is off by 0x10 for this symbol (try +0x790), not that it is a leaf.
#
# Hookability: KenshiLib::AddHook copies 5 bytes and has no trampoline replay, so the
# only real question is the prologue.  The PRODUCTION group is the positive control -
# hooks that work in the shipped DLL.  Only the two that come back ENTRY are usable as
# prologue controls; for the other five the delta is something else, and that never
# mattered because startPlugin() hooks by GetRealAddress(&Symbol), never by a literal.
# Both usable controls split mid-instruction at 5 bytes, so that alone is not
# disqualifying - compare shapes rather than hunting for a clean boundary.

import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ke_pe import PE, KENSHI_EXE, HEADER_RVA_DELTA, vftables, vslot

# header RVAs straight out of the KenshiLib headers (not exe RVAs - add the delta)
CONTROLS = [
    ('--- PRODUCTION hooks (positive controls) ---', None),
    ('AnimationClass::_NV_update', 0x5B6140),
    ('AnimationClassHuman::_NV_update', 0x5C4FD0),
    ('GameWorld::_NV_mainLoop_GPUSensitiveStuff', 0x7877A0),
    ('PlayerInterface::newPlayerTaskSelectedChars', 0x7F93F0),
    ('ContextMenuGUI::show', 0x7A6D80),
    ('InputHandler::loadConfig', 0x361F80),
    ('DatapanelGUI::addCustomLine', 0x6FE560),
    ('--- FORBIDDEN (hooking these crashes; see CLAUDE.md) ---', None),
    ('AnimationClass::beingCarriedUpdate', 0x5B5200),
    ('AnimationClass::updateAnimationTransforms', 0x5B0E30),
    ('--- direct calls (not hooked, known good) ---', None),
    ('AnimationClassHuman::_NV_ragdollModeUT', 0x5B98D0),
    ('AbstractMovementBase::getFacingDirection', 0x2ADB90),
    ('--- P4-3 weapon API ---', None),
    ('CharacterHuman::sheatheWeapon', 0x5CC0A0),
    ('CharacterHuman::dropWeaponInHands', 0x5CBFE0),
    ('CharacterHuman::leaveSheathEquipped', 0x5D1D30),
]


def probe(pe, label, rva, recurse=True):
    kind = pe.classify(rva)
    pro = pe.prolog_size(rva)
    b, sec = pe.read(rva, 16)
    print('%-44s 0x%06X %-8s %-9s %s' % (
        label, rva, kind.upper(),
        'prolog=%d' % pro if pro is not None else '',
        ' '.join('%02X' % c for c in bytearray(b))))
    # padding / "C2 00 00 CC CC..." means the delta is a little short for this
    # symbol (the +0x790 case).  Offer the next entry inside a narrow window.
    if recurse and kind != 'entry':
        for d in range(1, 0x41):
            if rva + d in pe.entries:
                probe(pe, '  ^ next entry at +0x%X' % d, rva + d, False)
                return
        # no .pdata entry nearby: small leaf functions need no unwind data, so
        # just point at where the CC padding stops.
        tail, _ = pe.read(rva, 0x40)
        for d, c in enumerate(bytearray(tail)):
            if c != 0xCC:
                if d:
                    probe(pe, '  ^ code at +0x%X (leaf, no .pdata)' % d,
                          rva + d, False)
                break


def run_controls(pe, delta):
    print('exe   %s' % pe.path)
    print('base  0x%X   .pdata records %d   delta +0x%X\n' % (
        pe.base, len(pe.funcs), delta))
    for label, hrva in CONTROLS:
        if hrva is None:
            print('\n%s' % label)
        else:
            probe(pe, label, hrva + delta)


def derive_delta(pe, window=0x4000):
    """Vote for the constant that maps header RVAs onto real .pdata entries."""
    import collections
    votes = collections.Counter()
    syms = [(l, r) for l, r in CONTROLS if r is not None]
    for _, rva in syms:
        for d in range(-window, window + 1):
            if rva + d in pe.entries:
                votes[d] += 1
    print('delta vote over %d symbols (window +/-0x%X):' % (len(syms), window))
    for d, n in votes.most_common(8):
        print('   %+#9x  %d/%d' % (d, n, len(syms)))
    print('\nthe winner is a HEURISTIC, not a law: some symbols sit at winner+0x10.')
    print('always confirm the candidate lands on an ENTRY before hooking it.')


def main(argv):
    pe = PE(KENSHI_EXE)
    if not argv:
        return run_controls(pe, HEADER_RVA_DELTA)
    if argv[0] == '--delta':
        return derive_delta(pe)
    if argv[0] == '--vslot':
        cls, off = argv[1], int(argv[2], 0)
        for vt, col, thisoff in vftables(pe, cls):
            print('%s vftable rva=0x%06X (COL 0x%06X, thisOffset=%d)'
                  % (cls, vt, col, thisoff))
        r = vslot(pe, cls, off)
        if r is None:
            print('no primary vftable found')
        else:
            probe(pe, '%s vtbl+0x%X' % (cls, off), r)
        return
    delta = 0 if argv[0] == '--true' else HEADER_RVA_DELTA
    for a in (argv[1:] if delta == 0 else argv):
        probe(pe, 'rva 0x%X' % int(a, 0), int(a, 0) + delta)


if __name__ == '__main__':
    main(sys.argv[1:])
