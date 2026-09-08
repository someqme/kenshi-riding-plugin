# legpose.py - the rider's LOWER BODY, offline.  No game, no DLL, no log.
#
#   python tools\legpose.py                  # the shipped cushion table, re-baked and described
#   python tools\legpose.py --list           # every sit-ish vanilla clip, leg geometry side by side
#   python tools\legpose.py --bake squat     # emit kRideCushionLeg for a different clip
#   python tools\legpose.py --bake bowing --time 1.5      # ... at a chosen instant
#   python tools\legpose.py --mirror         # ONLY diff the .cpp table against a fresh bake
#
# ⚠️ EVERY MODE RUNS --mirror FIRST and refuses to report on a mismatch - armarc.py's rule and the
# same reason: this file holds the DLL's numbers a second time, the compiler never sees this copy,
# so a half-finished edit would otherwise stay invisible until an in-game trip contradicted it.
#
# WHY THIS EXISTS.  P2-4 gave the lower body three styles (「我们现在应该有三种坐姿」):
#   * 跨骑 straddle  - thighs AUTHORED from bind, two angles (abduction mirrored, flexion not)
#   * 坐椅子 chair   - the 'sitting chair' pose's OWN hips, borrowed and replayed across fights
#   * 坐坐垫 cushion - a THIRD static pose, and there is no vanilla clip the engine will hand us
#                      for it, because a combat clip is UPPER and carries no leg track at all.
# So cushion is baked the way T30 bakes the swing: read a vanilla clip's own leg keys out of
# male_skeleton.skeleton, store them as bind-relative deltas, and let OUR writer replay them under
# OUR bone mask.  Nothing here drives an AnimationState and nothing plays an ANIMATION record.
#   want[i] = bone->getInitialOrientation() * kRideCushionLeg[i]      (i = kLegPoseBones order)
# which is algebraically the SAME shape the straddle already writes (bind * delta), so cushion is a
# third target for an existing writer rather than new machinery.
#
# ⚠️ Keys on disk are BIND-RELATIVE (local = bind * key, skelanims.py header) - that is exactly the
# space Bone::setOrientation takes, which is what makes the table a straight copy.
#
# ⚠️ WHAT THIS CANNOT SAY: whether the pose LOOKS like sitting on a cushion once a 6-unit-wide
# animal is under it.  It gives the leg SHAPE in the rider's own pelvis frame; the mount is not in
# this file at all.  Clipping into the animal's back is an eyeball question (TEST_REQUIRED.md T32).
#
# Frame, measured off the bind pose and stated so the columns below can be read: -Y = down,
# +X = forward, +Z = the rider's LEFT (both thighs share X in bind, which is what settles it).

import os
import sys
import io
import math

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import skelanims as S                                        # noqa: E402

CPP = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'RidingPlugin.cpp')

# ⚠️ MIRRORED: the pair that says WHICH bake the .cpp is holding.  check_mirror() re-bakes exactly
# this and diffs value by value, so these two lines and the table in the .cpp cannot drift apart.
BAKE_CLIP = 'sitting idle'
BAKE_TIME = None          # None = the medoid frame (the pose the clip mostly holds); see pick_time
                          # ⚠️ 'sitting idle' is an UPPER record: its four LEG tracks are constant
                          # across all 10.9 s, so ANY t gives this table.  The medoid is kept only
                          # because it is the tool's own definition of "the pose the clip holds".
MIRROR_TOL = 1e-4         # the .cpp prints %.6f, so anything above rounding is a real edit

# kLegPoseBones[0..3] order - load-bearing, the DLL indexes the table with the same i.
LEGS = ['Bip01 L Thigh', 'Bip01 R Thigh', 'Bip01 L Calf', 'Bip01 R Calf']
TABLE = 'kRideCushionLeg'

# The clips worth comparing for a seated lower body: everything vanilla holds with the knees bent.
# 'sitting chair' is in the list as the CONTROL - it is what chair mode borrows, so its row is the
# one the other rows have to look different from.
CANDIDATES = ['sitting chair', 'sitting idle', 'sitting dazed', 'squat', 'squat T', 'foetal',
              'kneeling hostage idle', 'sleeponfloor', 'sleepinbed', 'bowing', 'stealthKO']


def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz)


def qrot(q, v):
    x, y, z, w = q
    vx, vy, vz = v
    tx = 2.0 * (y * vz - z * vy)
    ty = 2.0 * (z * vx - x * vz)
    tz = 2.0 * (x * vy - y * vx)
    return (vx + w * tx + (y * tz - z * ty),
            vy + w * ty + (z * tx - x * tz),
            vz + w * tz + (x * ty - y * tx))


def qnorm(q):
    """Sign-normalised to w >= 0 so a diff against the .cpp is stable (q and -q are one rotation)."""
    x, y, z, w = q
    n = math.sqrt(x * x + y * y + z * z + w * w) or 1.0
    s = -1.0 / n if w < 0.0 else 1.0 / n
    return (x * s, y * s, z * s, w * s)


def qang(a, b):
    d = abs(sum(p * q for p, q in zip(a, b)))
    return math.degrees(2.0 * math.acos(max(-1.0, min(1.0, d))))


def vang(a, b):
    na = math.sqrt(sum(c * c for c in a))
    nb = math.sqrt(sum(c * c for c in b))
    d = sum(x * y for x, y in zip(a, b)) / ((na * nb) or 1.0)
    return math.degrees(math.acos(max(-1.0, min(1.0, d))))


_SK = [None]


def skel():
    if _SK[0] is None:
        if not os.path.isfile(S.HUMAN):
            return None
        _SK[0] = S.parse(S.HUMAN)
    return _SK[0]


def clip(sk, name):
    for a in sk.anims:
        if a[0] == name:
            return a
    return None


def key_at(tracks, handle, t):
    """Nearest key.  A pose is a HELD frame, so nearest-key is the honest read - no interpolation
    is invented here that the DLL would not do (the DLL stores one static quat per bone)."""
    ks = tracks.get(handle)
    if not ks:
        return (0.0, 0.0, 0.0, 1.0)
    return min(ks, key=lambda k: abs(k[0] - t))[1]


def key_times(tracks, handle):
    ks = tracks.get(handle)
    return [k[0] for k in ks] if ks else [0.0]


def pick_time(sk, name):
    """The MEDOID key: the frame whose four leg quats are closest to every other frame's.  = "the
    pose this clip mostly holds", which is the only defensible default for turning an animation
    into a static pose (t=0 is a transition on half these clips - `bowing` at 0 is still upright)."""
    a = clip(sk, name)
    if a is None:
        return None
    bn = sk.by_name()
    tracks = a[2]
    ts = key_times(tracks, bn[LEGS[0]])
    best, bestcost = ts[0], None
    for t in ts:
        qa = [key_at(tracks, bn[b], t) for b in LEGS]
        cost = 0.0
        for u in ts:
            qb = [key_at(tracks, bn[b], u) for b in LEGS]
            cost += sum(qang(p, q) for p, q in zip(qa, qb))
        if bestcost is None or cost < bestcost:
            best, bestcost = t, cost
    return best


def fk(sk, tracks, bone, t):
    """Derived position of `bone` in PELVIS space (pelvis at the origin, its own key applied so the
    numbers match what the clip actually draws).  Ogre's own composition, run by hand."""
    bn = sk.by_name()
    info = dict((h, (n, p, q)) for (h, n, p, q) in sk.bones)
    chain = []
    h = bn[bone]
    while h in sk.parents:
        chain.append(h)
        h = sk.parents[h]
    chain.append(h)
    chain.reverse()
    pel = bn['Bip01 Pelvis']
    chain = chain[chain.index(pel):]
    pos, rot = (0.0, 0.0, 0.0), (0.0, 0.0, 0.0, 1.0)
    for i, hb in enumerate(chain):
        _n, bpos, bq = info[hb]
        kq = key_at(tracks, hb, t)
        if i > 0:
            r = qrot(rot, bpos)
            pos = (pos[0] + r[0], pos[1] + r[1], pos[2] + r[2])
        rot = qmul(rot, qmul(bq, kq))
    return pos


def bake(name, t=None):
    """The four bind-relative quats, plus everything needed to describe the pose in words."""
    sk = skel()
    if sk is None:
        return None
    a = clip(sk, name)
    if a is None:
        return None
    if t is None:
        t = pick_time(sk, name)
    bn = sk.by_name()
    tracks = a[2]
    qs = [qnorm(key_at(tracks, bn[b], t)) for b in LEGS]
    geo = {}
    for side, tag in ((0, 'L'), (1, 'R')):
        hip = fk(sk, tracks, 'Bip01 %s Thigh' % tag, t)
        knee = fk(sk, tracks, 'Bip01 %s Calf' % tag, t)
        ankle = fk(sk, tracks, 'Bip01 %s Foot' % tag, t)
        thigh = [knee[i] - hip[i] for i in range(3)]
        shin = [ankle[i] - knee[i] for i in range(3)]
        geo[tag] = {'knee': knee, 'ankle': ankle, 'bend': 180.0 - vang(thigh, shin)}
    return {'clip': name, 't': t, 'secs': a[1], 'q': qs, 'geo': geo,
            'keys': len(key_times(tracks, bn[LEGS[0]]))}


def emit(b):
    """The .cpp table, ready to paste - and the ONLY way a value in it is allowed to change."""
    out = []
    out.append('// Generated by `python tools\\legpose.py --bake %s`%s - DO NOT hand-edit a value.'
               % (b['clip'], '' if b['t'] is None else ' (t=%.4f)' % b['t']))
    out.append('static const float %s[4][4] = {   // w, x, y, z - bind-relative delta' % TABLE)
    for i, nm in enumerate(LEGS):
        x, y, z, w = b['q'][i]
        out.append('    { %10.6ff, %10.6ff, %10.6ff, %10.6ff }%s   // %s'
                   % (w, x, y, z, ',' if i < 3 else '', nm))
    out.append('};')
    return '\n'.join(out)


def read_cpp():
    """Pull the four rows back out of the .cpp.  Deliberately dumb parsing: the table is generated,
    so anything this cannot read is already a hand edit."""
    path = os.path.abspath(CPP)
    if not os.path.isfile(path):
        return None
    rows = []
    inside = False
    for line in io.open(path, encoding='utf-8', errors='replace'):
        if TABLE + '[4][4]' in line:
            inside = True
            continue
        if not inside:
            continue
        if line.strip().startswith('};'):
            break
        s = line.strip()
        if not s.startswith('{'):
            continue
        body = s[1:s.index('}')]
        try:
            vals = [float(v.strip().rstrip('f')) for v in body.split(',')]
        except ValueError:
            return 'BAD'
        if len(vals) != 4:
            return 'BAD'
        rows.append(tuple(vals))          # w, x, y, z as written
    return rows if len(rows) == 4 else None


def check_mirror(verbose=True):
    """Re-bake BAKE_CLIP out of the asset and diff every value against the .cpp.  Returns 'ok',
    'absent' (the .cpp has no table yet = bootstrapping a new bake) or 'fail'.  Every mode calls
    this first and refuses to report on 'fail'."""
    cpp = read_cpp()
    if cpp is None:
        print('MIRROR: no %s[4][4] in %s - the DLL does not have cushion mode yet (or the table'
              % (TABLE, os.path.basename(os.path.abspath(CPP))))
        print('        was renamed).  Nothing below is the DLL\'s numbers - it is a PROPOSAL.')
        return 'absent'
    if cpp == 'BAD':
        print('MIRROR: %s in the .cpp does not parse as four rows of four floats = a hand edit.'
              % TABLE)
        return 'fail'
    b = bake(BAKE_CLIP, BAKE_TIME)
    if b is None:
        print('MIRROR: cannot re-bake %r from %s - no skeleton or no such clip.'
              % (BAKE_CLIP, os.path.basename(S.HUMAN)))
        return 'fail'
    worst, where = 0.0, ''
    for i in range(4):
        x, y, z, w = b['q'][i]
        for val, got, nm in ((w, cpp[i][0], 'w'), (x, cpp[i][1], 'x'),
                             (y, cpp[i][2], 'y'), (z, cpp[i][3], 'z')):
            d = abs(val - got)
            if d > worst:
                worst, where = d, '%s %s (.cpp %.6f, asset %.6f)' % (LEGS[i], nm, got, val)
    if worst > MIRROR_TOL:
        print('MIRROR FAIL - the .cpp table and the asset DISAGREE (worst %.6f at %s)'
              % (worst, where))
        print('        Either somebody hand-edited a row, or BAKE_CLIP/BAKE_TIME here no longer')
        print('        names the bake the DLL is holding.  Fix with:')
        print('            python tools\\legpose.py --bake %s' % BAKE_CLIP)
        return 'fail'
    if verbose:
        print('MIRROR OK - %s re-baked from %r in %s and diffed value by value (tol %g, worst %.6f)'
              % (TABLE, BAKE_CLIP, os.path.basename(S.HUMAN), MIRROR_TOL, worst))
    return 'ok'


def describe(b):
    """The pose in words and in numbers, in the rider's own pelvis frame."""
    print('')
    print('== %r @ t=%.3f of %.3fs (%d keys) ==' % (b['clip'], b['t'], b['secs'], b['keys']))
    print('   %-6s %-24s %-24s %6s' % ('side', 'knee (fwd,down,lat)', 'ankle (fwd,down,lat)',
                                       'knee'))
    for tag in ('L', 'R'):
        g = b['geo'][tag]
        k, a = g['knee'], g['ankle']
        print('   %-6s (%6.2f,%6.2f,%6.2f)   (%6.2f,%6.2f,%6.2f)   %5.0f'
              % (tag, k[0], -k[1], k[2], a[0], -a[1], a[2], g['bend']))
    gl, gr = b['geo']['L'], b['geo']['R']
    # A cushion sit is SYMMETRIC (both legs folded the same) and its ankles come INBOARD of the
    # knees - that is the difference between sitting cross-legged and sitting on a bench.
    sym = abs(gl['bend'] - gr['bend'])
    lat = abs(gl['ankle'][2]) + abs(gr['ankle'][2])
    latk = abs(gl['knee'][2]) + abs(gr['knee'][2])
    print('   symmetry: knee bend differs %.0f deg between sides (a held pose should be < 10)'
          % sym)
    print('   ankles vs knees, lateral: %.2f vs %.2f  => %s' % (lat, latk,
          'INBOARD = folded/crossed (cushion)' if lat < latk else 'outboard = hanging (bench)'))
    print('   ankle vs knee, forward: %s' % ('BEHIND = shin folded back'
          if gl['ankle'][0] < gl['knee'][0] else 'ahead = shin hangs/extends forward'))
    hinge = max(abs(b['q'][2][0]) + abs(b['q'][2][2]), abs(b['q'][3][0]) + abs(b['q'][3][2]))
    print('   calf keys are a %s (|x|+|z| = %.6f)  <- a pure hinge cannot induce twist about the'
          % ('PURE HINGE' if hinge <= 1e-6 else 'GENERAL rotation', hinge))
    print('   %s   shin bone, so nothing below the knee gets rolled by this write.' % (' ' * 3))
    print('')
    print(emit(b))


def report_list():
    """Every candidate side by side, so the cushion clip is CHOSEN rather than assumed."""
    sk = skel()
    if sk is None:
        print('no skeleton at %s' % S.HUMAN)
        return
    print('')
    print('== vanilla clips with the knees bent, leg geometry in PELVIS space ==')
    print('   ("sitting chair" is the control - it is what chair mode already borrows)')
    print('   %-22s %6s %-22s %-22s %5s %5s %s' % (
        'clip', 't', 'L knee (fwd,dn,lat)', 'L ankle (fwd,dn,lat)', 'bend', 'sym', 'shape'))
    for nm in CANDIDATES:
        b = bake(nm)
        if b is None:
            print('   %-22s  (absent)' % nm)
            continue
        gl, gr = b['geo']['L'], b['geo']['R']
        k, a = gl['knee'], gl['ankle']
        lat = abs(gl['ankle'][2]) + abs(gr['ankle'][2])
        latk = abs(gl['knee'][2]) + abs(gr['knee'][2])
        shape = []
        shape.append('folded-in' if lat < latk else 'hanging')
        shape.append('shin-back' if a[0] < k[0] else 'shin-fwd')
        if k[1] > 0.0:
            shape.append('knee-above-hip')
        print('   %-22s %6.3f (%6.2f,%6.2f,%6.2f)   (%6.2f,%6.2f,%6.2f)   %4.0f %5.0f %s'
              % (nm[:22], b['t'], k[0], -k[1], k[2], a[0], -a[1], a[2], gl['bend'],
                 abs(gl['bend'] - gr['bend']), ' '.join(shape)))
    print('')
    print('   READING IT: 坐坐垫 wants folded-in + symmetric (sym small).  shin-back = 跪坐-like,')
    print('   knee-above-hip = leaning-back ground sit (wrong on an animal\'s back).  t= is the')
    print('   medoid key, i.e. the pose the clip actually holds - not t=0, which is a transition')
    print('   on half of these.')


def main(argv):
    mode = 'show'
    name, tval = BAKE_CLIP, BAKE_TIME
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == '--mirror':
            mode = 'mirror'
        elif a == '--list':
            mode = 'list'
        elif a == '--bake':
            mode = 'bake'
            if i + 1 < len(argv) and not argv[i + 1].startswith('--'):
                i += 1
                name = argv[i]
        elif a == '--time':
            i += 1
            tval = float(argv[i])
        else:
            print('unknown argument %r' % a)
            return 2
        i += 1

    st = check_mirror(verbose=True)
    if mode == 'mirror':
        return 0 if st == 'ok' else 1
    if st == 'fail':
        print('')
        print('REFUSING to report: the offline table and the DLL disagree, so every number below')
        print('would describe a pose the game is not holding.  (armarc.py has the same rule.)')
        return 1
    # 'absent' is the ONE case that reports anyway: it is how the table gets created in the first
    # place.  Every print below is then labelled a PROPOSAL, never "what the DLL is holding".

    if mode == 'list':
        report_list()
        return 0

    b = bake(name, tval)
    if b is None:
        print('no clip %r in %s' % (name, os.path.basename(S.HUMAN)))
        return 1
    describe(b)
    stale = (st == 'absent') or (name != BAKE_CLIP or tval != BAKE_TIME)
    if mode == 'bake' and stale:
        print('')
        print('⚠️ THIS IS NOT WHAT THE DLL IS HOLDING.  To ship it: paste the table above over')
        print('   %s in RidingPlugin.cpp AND set BAKE_CLIP/BAKE_TIME at the top of this' % TABLE)
        print('   file to %r/%s, then re-run --mirror before building.'
              % (name, 'None' if tval is None else '%.4f' % tval))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
