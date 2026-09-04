# armarc.py - where does the AUTHORED swing arc actually put the hand?  Offline, no game.
#
#   python tools\armarc.py                   # the shipped kRideSwingArc, sampled
#   python tools\armarc.py --ref 1.41 3.88 3.51   # ... off a different reference pose (out fore down)
#   python tools\armarc.py --bindref          # ... off the BIND pose instead of the measured one
#   python tools\armarc.py --bind             # bind-pose sanity numbers only
#   python tools\armarc.py --abd -70 --flx -30 --elb -50    # one retired-model pose, ad hoc
#   python tools\armarc.py --log <RE_Kenshi_log>            # what the GAME actually drew, vs this
#   python tools\armarc.py --mirror           # ONLY diff AXIS/ARC2/ArcMs/WinMs against the .cpp
#
# ⚠️ EVERY MODE ABOVE RUNS --mirror FIRST and refuses to report on a mismatch.  This file holds the
# DLL's numbers a second time; the compiler never sees this copy, so a half-finished edit used to be
# invisible until an in-game trip contradicted the offline report.  That is what the check closes.
#
# Why this exists: the DLL hand-writes two arm bones (RidingPlugin.cpp, RideSwingArmPose) and every
# version of that has been a guess about a frame we do not own.  The .skeleton file carries every bone's
# bind position, bind rotation and parent, so the exact composition the engine does (derivedRot =
# parentDerivedRot * localRot, derivedPos = parentDerivedPos + parentDerivedRot * localPos) can be run
# here, and --log re-derives the same quantities from the game's own samples.  A wrong sign costs a
# rebuild instead of a ride.
#
# 🆕 T28 - THE LIVE MODEL IS ONE ROTATION.  The stroke is now: capture the arm as the host holds it at
# window open, then rotate it rigidly by deg(t) about ONE fixed axis.  Two consequences make this file
# much stronger than it was under the direction model:
#   * |shoulder->hand| is INVARIANT under a rotation about the shoulder, so `r` flat across a window is
#     a model-free proof that the elbow did not move - it needs no skeleton file and no bind pose.
#   * the hand bone's own axes (bx=/bz=) must follow the SAME rotation, so their drift is a model-free
#     read on 正手/反手 - the grip complaint that killed trip 25.
# Both witnesses run on any log that carries out=/fore=/down= (T26 onwards), which is why --log can put
# a number on the previous trip as well as this one.
#
# ⚠️ WHAT THIS CANNOT SAY: it computes the SKELETON-space path of the hand bone, which is the same
# quantity the log's out=/fore=/down= samples print.  It says nothing about how the mesh skins to
# that path, nothing about the weapon's own attachment offset, and nothing about whether the result
# reads as a swing to a human.
#
# Frame (§16, measured): +X = rider's LEFT, +Y = up, +Z = forward.  Reported as the log reports:
#   out  = -relX  (away from the body, for the RIGHT arm)   fore = +relZ   down = -relY
# relative to the shoulder (= the UpperArm bone's own derived position).

import io
import math
import os
import re
import struct
import sys

KENSHI = r'D:\steam\steamapps\common\Kenshi'
HUMAN = os.path.join(KENSHI, r'data\character\meshes\male_skeleton\male_skeleton.skeleton')

C_BLEND, C_BONE, C_PARENT = 0x1010, 0x2000, 0x3000
C_ANIM, C_BASEINFO, C_TRACK, C_KEYFRAME, C_LINK = 0x4000, 0x4010, 0x4100, 0x4110, 0x5000

# The arc under test.  ⚠️ MIRRORS kRideSwingArc in RidingPlugin.cpp - if one changes, change both;
# there is no build step that could catch a drift here.
#
# ⛔ RETIRED MODEL (T25, trip 23).  ARC below is the joint-ANGLE table: abd/flx about the bone's own
# bind axes.  It was measured right and still drew the wrong thing on screen, because a bind-relative
# angle only fixes the bone relative to its PARENT, and the parent (clavicle, spine) is host-driven by
# 'mid blow' at speed 2.5.  `--log` on the trip-23 log proves it without any model: two frames in one
# window wrote the SAME three angles (-25/10/-55) and the hand landed 72.3 deg apart, while
# |measured|/|predicted| stayed at 1.04 (so the arm geometry was never the problem).  The screen
# therefore showed our arc TIMES the host's torso sweep - reported as 「往下戳」 tracing a 「\」.
# ARC is kept only because `--log` needs it to explain that log.  The live model is ARC2.
ARC = [
    (0.00, -25.0,  10.0, -55.0),
    (0.26, -90.0,  45.0, -50.0),
    (0.66,  10.0, -40.0,   0.0),
    (1.00, -25.0,  10.0, -55.0),
]

# ⛔ ALSO RETIRED (T26/T27, trips 24-25).  ARC_DIR below authored a DIRECTION per bone per key and the
# DLL aimed each bone's local +X along it.  It was measured right too, and it failed for a reason no key
# in it could name: `UNIT_X.getRotationTo(dir)` is the MINIMAL rotation onto dir, so it fixes the bone's
# AXIS and leaves the ROLL about that axis to fall out of wherever the arc happens to be.  'Bip01 R Hand'
# - host-driven, in neither mask table by design, and the bone actually holding the weapon - inherits
# every degree of that roll: trip 25 measured bx= wandering by 1.13/1.59/1.70 over three windows and the
# verdict was 「正手变反手」.  In its own numbers it also cut with the ELBOW (ready->through turned the
# upper arm 27.7 deg while the forearm swung ~124), the exact inverse of 「大臂旋转小臂不动」.
# Kept only so --log can still explain the trip-24/25 logs.  The live model is below.
ARC_DIR = [
    #  t     upper (out, fore, down)      forearm (out, fore, down)      what it is
    (0.00, (0.30,  0.15,  0.94), (0.35,  0.80,  0.49)),   # ready: hand forward of the right hip
    (0.24, (0.80, -0.30, -0.52), (0.30, -0.25, -0.92)),   # cock: blade high over the right shoulder
    (0.50, (0.62,  0.62,  0.48), (0.30,  0.86,  0.42)),   # mid-cut: arm straight, sweeping forward
    (0.72, (0.05,  0.55,  0.83), (-0.55, 0.62,  0.56)),   # cut through: low, forward, across
    (1.00, (0.30,  0.15,  0.94), (0.35,  0.80,  0.49)),   # settle back to ready
]

# ---- the RETIRED model (T28/T29): ONE rigid rotation of the pose the host is already holding ----
# ⚠️ MIRRORS NOTHING ANY MORE.  T30 deleted kRideSwingAxis*/kRideSwingArc/kRideSwingArcKeys from the
# .cpp, so check_mirror() no longer looks for them - it re-bakes the clip instead (see the bake section
# below).  These numbers stay because they are the ONLY way to decode a pre-T30 log: report_log's
# witness 2/3 replay them against trips 24..26, and those readings are the baselines T30 is judged
# against.  Editing them silently rewrites history; nothing in the DLL cares.
# That model captured the upper arm's DERIVED orientation and the forearm's LOCAL orientation at window
# open, then wrote derived_upper = S(t) * refUp and local_forearm = refFo verbatim every frame.  A
# constant forearm local makes the elbow angle constant by construction, so the whole arm-plus-weapon
# assembly is one rigid body turning about the shoulder ⇒ everything this file needs to know about the
# hand's path is rel(t) = S(t) * rel(0).  No skeleton file, no bind pose, no per-bone lengths.  T30 broke
# exactly that: the elbow moves now, so `r=` SPANS instead of staying flat (see report_log).
# The axis is the normal of the plane the RETIRED table's own upper arm swept between its cock key and
# its cut-through key - i.e. the cut plane of the shape whose DIRECTION the user accepted (「从右侧劈
# 出」 was never the complaint; only the flip was).  A POSITIVE angle carries the right hand down and
# across the mount's neck; a NEGATIVE angle lifts it out and up over the shoulder.
AXIS = (-0.05, 0.83, -0.55)     # (out, fore, down), normalised below
# 🆕 T29 slid the window up the SAME circle (axis untouched = one variable).  T28's -100 -> +45 straddled
# the circle's out-extreme (out 5.2) ⇒ 6.00 vertical vs 7.27 lateral travel = the geometry behind the
# verdict 「侧面张开大臂」; -130 -> +25 gives 8.11 / 3.74 = vert/lat 2.17, a chop.  And T28's -60 「hang」
# leaked 40 of its 145 deg early, so the wind-up (22.2 u/s) nearly matched the cut (27.2 u/s); the hold
# below fixes that without moving the instant the cut lands (1092 ms into the window, same as T28).
ARC2 = [
    #  t      deg      what it is
    (0.00,    0.0),   # open: identity, i.e. the host's own pose - no handover step
    (0.40, -130.0),   # cock: high, over the right shoulder (clear of the head box)
    (0.54, -130.0),   # hold - deg_at is pure linear, so two equal keys are a true hold
    (0.78,   25.0),   # through: the circle's lowest reachable point is +18..20, so +25 is past it
    (0.86,   25.0),   # impact beat
    (1.00,    0.0),   # settle back onto the captured pose
]
# The pose the guard clip was holding at window open, measured - trip 25's first arm sample, out/fore/
# down (r = 5.42).  Everything --bindref does instead is a sanity bound, not a prediction: the game will
# open from whatever 'guard 1h' happens to be holding, which is this, not the bind pose.
REF_MEASURED = (1.41, 3.88, 3.51)
# ⚠️ MIRRORS kRideSwingArcMs / kRideSwingWinMs.  Only used to turn t into ms for the tempo column - the
# shape does not depend on them - but a drift here would mislabel the tempo, so keep them in step.
ARC_MS, WIN_MS = 1400, 1650
UPPER, FORE, HAND = 'Bip01 R UpperArm', 'Bip01 R Forearm', 'Bip01 R Hand'


def _u16(b, o):
    return struct.unpack_from('<H', b, o)[0]


def _u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def _f32(b, o):
    return struct.unpack_from('<f', b, o)[0]


def _line(b, o):
    e = b.index(b'\n', o)
    return b[o:e].decode('latin-1'), e + 1


def read_bones(path):
    """-> {name: {'h','pos','rot','parent'}} with bind LOCAL pos/rot straight out of the file.

    Same structural walk as skelanims.py (a BONE chunk's recorded length omits its name string, so
    a flat length walk lands mid-name); this one keeps the numbers that one throws away.
    """
    b = open(path, 'rb').read()
    if _u16(b, 0) != 0x1000:
        raise ValueError('not an Ogre .skeleton')
    _, o = _line(b, 2)
    bones, by_handle = {}, {}
    while o + 6 <= len(b):
        cid, ln = _u16(b, o), _u32(b, o + 2)
        p = o + 6
        if cid == C_BONE:
            nm, p = _line(b, p)
            h = _u16(b, p)
            pos = (_f32(b, p + 2), _f32(b, p + 6), _f32(b, p + 10))
            # ⚠️ FIELD ORDER IS x,y,z,w ON DISK, not w,x,y,z.  Settled EMPIRICALLY, not from
            # upstream Ogre source (§21.5 forbids that as a second truth source): reading it as
            # w,x,y,z put the bind hand 0.39 units from the shoulder, and the two arm bones are
            # 2.85 + 3.24 long, so that composition was impossible.  With x,y,z,w the bind pose
            # measures 6.09 = exactly the straight-arm sum.  main() prints that check every run.
            rot = (_f32(b, p + 26), _f32(b, p + 14), _f32(b, p + 18), _f32(b, p + 22))
            bones[nm] = {'h': h, 'pos': pos, 'rot': rot, 'parent': None}
            by_handle[h] = nm
            p += 2 + 12 + 16 + (12 if ln >= 48 else 0)
        elif cid == C_PARENT:
            child, parent = _u16(b, p), _u16(b, p + 2)
            bones[by_handle[child]]['parent'] = by_handle[parent]
            p += 4
        elif cid == C_BLEND:
            p += 2
        elif cid == C_ANIM:
            _, p = _line(b, p)
            p += 4
        elif cid in (C_BASEINFO, C_LINK):
            _, p = _line(b, p)
            p += 4
        elif cid == C_TRACK:
            p += 2
        elif cid == C_KEYFRAME:
            p = o + ln
        else:
            raise ValueError('unknown chunk 0x%04X at 0x%X' % (cid, o))
        o = p
    return bones


# ---- quaternions, Ogre's conventions: (w,x,y,z), q1*q2 applies q2 first -----------------------
def qmul(a, b):
    aw, ax, ay, az = a
    bw, bx, by, bz = b
    return (aw * bw - ax * bx - ay * by - az * bz,
            aw * bx + ax * bw + ay * bz - az * by,
            aw * by + ay * bw + az * bx - ax * bz,
            aw * bz + az * bw + ax * by - ay * bx)


def qaxis(deg, axis):
    h = math.radians(deg) * 0.5
    s = math.sin(h)
    return (math.cos(h), axis[0] * s, axis[1] * s, axis[2] * s)


def qrot(q, v):
    w, x, y, z = q
    # v' = q * v * q^-1, expanded
    tx, ty, tz = 2.0 * (y * v[2] - z * v[1]), 2.0 * (z * v[0] - x * v[2]), 2.0 * (x * v[1] - y * v[0])
    return (v[0] + w * tx + (y * tz - z * ty),
            v[1] + w * ty + (z * tx - x * tz),
            v[2] + w * tz + (x * ty - y * tx))


UNIT_Y, UNIT_Z = (0.0, 1.0, 0.0), (0.0, 0.0, 1.0)


def derived(bones, name, override):
    """(derivedPos, derivedRot) in skeleton space, with `override` supplying a LOCAL rotation for
    any bone that has one.  Exactly the engine's composition - the local position is untouched by a
    rotation, but every descendant's position moves because it is carried by parentDerivedRot."""
    chain = []
    n = name
    while n is not None:
        chain.append(n)
        n = bones[n]['parent']
    pos, rot = (0.0, 0.0, 0.0), (1.0, 0.0, 0.0, 0.0)
    for n in reversed(chain):
        b = bones[n]
        pos = tuple(pos[i] + qrot(rot, b['pos'])[i] for i in range(3))
        rot = qmul(rot, override.get(n, b['rot']))
    return pos, rot


def arc_at(t):
    if t <= ARC[0][0]:
        return ARC[0][1:]
    for i in range(1, len(ARC)):
        a, b = ARC[i - 1], ARC[i]
        if t <= b[0] or i == len(ARC) - 1:
            u = (t - a[0]) / (b[0] - a[0]) if b[0] > a[0] else 1.0
            u = min(1.0, max(0.0, u))
            return tuple(a[k] + (b[k] - a[k]) * u for k in (1, 2, 3))
    return ARC[-1][1:]


def pose(bones, abd, flx, elb):
    """-> (out, fore, down) of the HAND relative to the shoulder, log conventions."""
    ov = {
        UPPER: qmul(bones[UPPER]['rot'], qmul(qaxis(abd, UNIT_Z), qaxis(flx, UNIT_Y))),
        FORE:  qmul(bones[FORE]['rot'], qaxis(elb, UNIT_Y)),
    }
    sh, _ = derived(bones, UPPER, ov)
    hd, _ = derived(bones, HAND, ov)
    rel = tuple(hd[i] - sh[i] for i in range(3))
    return (-rel[0], rel[2], -rel[1])


def vlen(v):
    return math.sqrt(sum(c * c for c in v))


def vnorm(v):
    l = vlen(v)
    return v if l < 1e-9 else tuple(c / l for c in v)


def to_skel(ofd):
    """(out, fore, down) -> skeleton (x, y, z).  Mirrors RideSwingDirToSkel in the DLL, unnormalised."""
    return (-ofd[0], -ofd[2], ofd[1])


def to_log(v):
    """skeleton (x, y, z) -> (out, fore, down), the way every log line prints it."""
    return (-v[0], v[2], -v[1])


# ⚠️ THE HANDEDNESS TRAP, stated once: the log frame is LEFT-handed relative to skeleton space (out =
# -x, fore = z, down = -y has determinant -1), so building a rotation from the axis TRIPLE in log
# conventions would silently give the mirror-image rotation.  Every rotation below is therefore built
# and applied in SKELETON space - exactly where the DLL builds it - and only the printing is converted.
AXIS_SKEL = vnorm(to_skel(AXIS))


def deg_at(t):
    """The authored angle at t, degrees.  Linear over ARC2, no easing - the shape lives in the key
    TIMES.  This has to reproduce RideSwingArcAt in the DLL exactly, so it stays this dull."""
    t = min(1.0, max(0.0, t))
    for i in range(1, len(ARC2)):
        a, b = ARC2[i - 1], ARC2[i]
        if t > b[0] and i != len(ARC2) - 1:
            continue
        span = b[0] - a[0]
        u = (t - a[0]) / span if span > 1e-4 else 1.0
        u = min(1.0, max(0.0, u))
        return a[1] + (b[1] - a[1]) * u
    return 0.0


def rot_at(t):
    """S(t): the rigid rotation the DLL applies at t, in skeleton space."""
    return qaxis(deg_at(t), AXIS_SKEL)


def hand_at(t, ref):
    """The hand relative to the shoulder at t, in (out, fore, down), given the reference pose `ref`
    (also out/fore/down) captured at window open.  ONE line of model: rel(t) = S(t) * rel(0).  Note
    what is NOT here - no bone lengths, no elbow angle, nothing above the shoulder.  That is the whole
    point of T28: the elbow cannot enter because a rigid rotation has no elbow."""
    return to_log(qrot(rot_at(t), to_skel(ref)))


def plot(rows, xi, yi, xlab, ylab, w=54, h=15):
    """ASCII scatter of the hand path.  Deliberately not an image: the numbers are the evidence and
    this is only here so the SHAPE of the path is visible at a glance."""
    xs = [r[1][xi] for r in rows]
    ys = [r[1][yi] for r in rows]
    x0, x1 = min(xs), max(xs)
    y0, y1 = min(ys), max(ys)
    if x1 - x0 < 1e-6:
        x1 = x0 + 1e-6
    if y1 - y0 < 1e-6:
        y1 = y0 + 1e-6
    grid = [[' '] * w for _ in range(h)]
    for k, r in enumerate(rows):
        cx = int((r[1][xi] - x0) / (x1 - x0) * (w - 1))
        cy = int((1.0 - (r[1][yi] - y0) / (y1 - y0)) * (h - 1))
        grid[cy][cx] = '0123456789ABCDEFGHIJ'[k % 20]
    print('    %s (increases rightwards) vs %s (increases UPWARDS on the plot, so for `down`'
          ' the top of the plot is LOWER in space):' % (xlab, ylab))
    for row in grid:
        print('      |' + ''.join(row))
    print('      +' + '-' * w)
    print('      %s: %.2f .. %.2f      %s: %.2f .. %.2f' % (xlab, x0, x1, ylab, y0, y1))


def _kv(line, key):
    """last-key-wins is fine here: every field appears once on a SWING arm row."""
    i = line.find(key)
    if i < 0:
        return None
    j = i + len(key)
    k = j
    while k < len(line) and line[k] not in ' \t\r\n':
        k += 1
    try:
        return float(line[j:k])
    except ValueError:
        return None


def _triple(line, key):
    i = line.find(key)
    if i < 0:
        return None
    j = line.find(')', i)
    if j < 0:
        return None
    try:
        p = [float(c) for c in line[i + len(key):j].split(',')]
    except ValueError:
        return None
    return tuple(p) if len(p) == 3 else None


def read_log(path):
    """-> [ {t, meas, deg, cone, elbow, r, kept, dot, want, bx, bz, sh, f, legacy} ] from the game's
    'SWING arm' rows, in order.  Deliberately tolerant about WHICH build wrote the row: `t=` and the
    measured out=/fore=/down= have been on every version of that line since T25, so one parser reads a
    T26, T27, T28 or T30 log and the report below decides what each of them can be asked.  A row
    missing `t=` or the measurement is DROPPED, never defaulted - a truncated row is not evidence.
    🆕 T30 dropped deg= and cone= (there is no single rotation any more) and added elbow=.  ⚠️ ' elb='
    (T27's legacy triple) cannot match ' elbow=': the search key carries its own '='."""
    rows = []
    with open(path, 'r', errors='replace') as fh:
        for line in fh:
            if 'SWING arm ' not in line:
                continue
            t = _kv(line, ' t=')
            o, f, d = _kv(line, ' out='), _kv(line, ' fore='), _kv(line, ' down=')
            if None in (t, o, f, d):
                continue
            abd, flx, elb = _kv(line, ' abd='), _kv(line, ' flx='), _kv(line, ' elb=')
            rows.append({'t': t, 'meas': (o, f, d),
                         'deg': _kv(line, ' deg='), 'cone': _kv(line, ' cone='),
                         'elbow': _kv(line, ' elbow='),
                         'r': _kv(line, ' r='), 'kept': _kv(line, ' kept='),
                         'dot': _kv(line, ' dot='), 'want': _triple(line, ' want=('),
                         'bx': _triple(line, ' bx=('), 'bz': _triple(line, ' bz=('),
                         'sh': _triple(line, ' sh=('), 'f': _kv(line, ' f='),
                         'legacy': None if None in (abd, flx, elb) else (abd, flx, elb)})
    return rows


def _fmt(v, spec):
    """A field that a given build never printed must read as '-', not as 0.0 - the whole point of the
    tolerant parser is that a missing field and a zero one are different facts."""
    return '-' if v is None else (spec % v)


def angle_between(a, b):
    la, lb = vlen(a), vlen(b)
    if la < 1e-6 or lb < 1e-6:
        return None
    c = sum(a[i] * b[i] for i in range(3)) / (la * lb)
    return math.degrees(math.acos(max(-1.0, min(1.0, c))))


def report_log(bones, path):
    """The measured half.  🆕 T30 asks the log THREE questions that need no anchor and no model:

      * r  = |shoulder->hand|, elbow = the joint angle there, grip = angle(hand's own +X, that same
        vector).  All three are scalars built from vectors the torso carries TOGETHER, so the saddle's
        yaw cannot enter them - which is what makes them comparable to the offline bake DIRECTLY, key
        by key, with nothing fitted and no correction rotation at all.
      * T28's verdict inverts here: it needed r flat (a rigid rotation cannot change it) and read span
        0.000 over 13 windows.  T30 needs r and elbow to MOVE, because the baked forearm curve is the
        whole point; offline says r 2.90..5.42 and elbow 56..126 inside one window.
      * and the DIRECTION, which does need the one measured rotation: the offline path anchored on each
        window's own first sample (bx=/bz= give a full orthonormal R Hand frame), against out=/fore=/
        down= on every later row.  That is the trip-23 「往下戳」 guard - a correctly measured curve can
        still point the wrong way, and this is the number that would say so.

    Older logs still read: T28 rows carry deg= and get the rotation witnesses instead (which is how
    trips 24..26 were judged), and pre-T28 rows get the flatness witness and the raw bx= spans only -
    exactly how trip 25's 「正手变反手」 was measured."""
    rows = read_log(path)
    if not rows:
        print('no "SWING arm" row in %s - either the build predates T25 or nobody swung.'
              % os.path.basename(path))
        return 1
    windows, cur = [], []
    for r in rows:
        if cur and r['t'] < cur[-1]['t'] - 1e-9:
            windows.append(cur)
            cur = []
        cur.append(r)
    if cur:
        windows.append(cur)
    t28 = any(r['deg'] is not None for r in rows)
    t30 = any(r['elbow'] is not None for r in rows)
    bake = bake_curve(bones) if t30 else None
    print('')
    print('=== measured in game: %s ===' % os.path.basename(path))
    print('  %d sample(s) in %d window(s), model=%s'
          % (len(rows), len(windows), 'T30 baked curve (elbow=, no deg=)' if t30 else
             'T28 rotation' if t28 else 'pre-T28 (no deg=)'))
    if t30 and bake is None:
        print('  ⚠️ the offline bake is unavailable (asset missing), so the measured/predicted columns')
        print('     below are omitted; the span witnesses still work - they need no prediction.')
    if bake:
        print('  offline reference: %r %s form, %s - the SAME curve the .cpp ships (check_mirror'
              ' proves it)' % (BAKE_CLIP, BAKE_FORM, bake['lab']))
    rspread, angerr, bxerr, elspread, prederr, scalerr, tilts = [], [], [], [], [], [], []
    for w, win in enumerate(windows):
        f0, m0 = win[0], win[0]['meas']
        d0 = win[0]['deg']
        rs = [vlen(r['meas']) for r in win]
        rspread.append((w + 1, min(rs), max(rs)))
        els = [r['elbow'] for r in win if r['elbow'] is not None]
        if els:
            elspread.append((w + 1, min(els), max(els)))
        print('')
        print('  -- window %d: %d sample(s), f=%s..%s%s --'
              % (w + 1, len(win), _fmt(f0['f'], '%.0f'), _fmt(win[-1]['f'], '%.0f'),
                 '' if t30 else ', cone=%s' % _fmt(f0['cone'], '%.1f')))
        if t30:
            fr = (frame_of(to_skel(f0['bx']), to_skel(f0['bz']))
                  if bake and f0['bx'] and f0['bz'] else None)
            if fr:
                tl, tot = anchor_tilt(bake['off'], fr)
                tilts.append((tl, tot, w + 1))
                print('     anchor C at this window\'s t=%.2f: tilt %.1f deg, total %.1f deg'
                      % (f0['t'], tl, tot))
            print('     t |  meas: out  fore  down     r elbow  grip |  pred: out  fore  down     r'
                  ' elbow  grip |  err  ~ms   kept    dot')
            for r in win:
                m, pr = r['meas'], None
                if bake:
                    pr = curve_at(bake, r['t'])
                grip = angle_between(r['bx'], m) if r['bx'] else None
                pm = rebase(to_skel(pr[0]), bake['off'], fr) if (pr and fr) else None
                err = vlen(tuple(to_skel(m)[j] - pm[j] for j in range(3))) if pm else None
                rate = curve_rate(bake, r['t']) if bake else 0.0
                slip = (err / rate) if (err is not None and rate > 1e-4) else None
                if err is not None and r is not f0:
                    prederr.append((err, w + 1, r['t'], slip))
                if pr and r['elbow'] is not None and r['r'] is not None:
                    scalerr.append((abs(r['r'] - pr[1]), abs(r['elbow'] - pr[2]),
                                    (abs(grip - pr[3]) if grip is not None else 0.0), w + 1, r['t']))
                print('  %4.2f | %11.2f %5.2f %5.2f %5.2f %5s %5s | %11s %5s %5s %5s %5s %5s |'
                      ' %4s %4s %6s %6s'
                      % (r['t'], m[0], m[1], m[2], vlen(m), _fmt(r['elbow'], '%.1f'),
                         _fmt(grip, '%.1f'),
                         _fmt(to_log(pm)[0] if pm else None, '%.2f'),
                         _fmt(to_log(pm)[1] if pm else None, '%.2f'),
                         _fmt(to_log(pm)[2] if pm else None, '%.2f'),
                         _fmt(pr[1] if pr else None, '%.2f'), _fmt(pr[2] if pr else None, '%.1f'),
                         _fmt(pr[3] if pr else None, '%.1f'), _fmt(err, '%.2f'),
                         _fmt(slip, '%.0f'),
                         _fmt(r['kept'], '%.4f'), _fmt(r['dot'], '%.4f')))
            continue
        print('     t     deg  |  measured out/fore/down |    r    | rot(first) out/fore/down |'
              ' err   bxerr  kept    dot')
        for r in win:
            m = r['meas']
            pm, pa, pb = None, None, None
            if t28 and r['deg'] is not None and d0 is not None:
                q = qaxis(r['deg'] - d0, AXIS_SKEL)
                pm = to_log(qrot(q, to_skel(m0)))
                pa = angle_between(m, pm)
                if r['bx'] and f0['bx']:
                    pb = angle_between(r['bx'], to_log(qrot(q, to_skel(f0['bx']))))
            if pa is not None and r is not f0:
                angerr.append((pa, w + 1, r['t']))
            if pb is not None and r is not f0:
                bxerr.append((pb, w + 1, r['t']))
            print('  %5.2f %6s | %6.2f %6.2f %6.2f  | %7.3f | %s | %5s %5s  %6s %6s'
                  % (r['t'], _fmt(r['deg'], '%.0f'), m[0], m[1], m[2], vlen(m),
                     ('%6.2f %6.2f %6.2f    ' % pm) if pm else ' ' * 24,
                     _fmt(pa, '%.1f'), _fmt(pb, '%.1f'),
                     _fmt(r['kept'], '%.4f'), _fmt(r['dot'], '%.4f')))
    print('')
    print('  WITNESS 1 - the elbow (no model, no anchor): |shoulder->hand| per window')
    worst = best = 0.0
    for i, (n, lo, hi) in enumerate(rspread):
        worst = max(worst, hi - lo)
        best = (hi - lo) if i == 0 else min(best, hi - lo)
        el = [e for e in elspread if e[0] == n]
        print('    win%d: r %.3f .. %.3f   spread %.3f%s'
              % (n, lo, hi, hi - lo,
                 ('   elbow %.1f .. %.1f  span %.1f deg' % (el[0][1], el[0][2], el[0][2] - el[0][1]))
                 if el else ''))
    print('    no rotation of the whole arm can change either number, and neither can the one-frame')
    print('    read lag (a lagged sample is a rotated one), so both are MODEL-FREE and ANCHOR-FREE.')
    if t30:
        elw = min([e[2] - e[1] for e in elspread] or [0.0])
        print('    T30 INVERTS T28\'s verdict: the baked forearm curve is the whole point, so these MUST')
        print('    move.  Offline the shipped stroke says r 2.90..5.42 (2.52) and elbow 56..126 (69).')
        print('    smallest window: r spread %.3f => %s;  elbow span %.1f deg => %s'
              % (best, 'MOVES (forearm curve is playing)' if best >= 1.5 else
                 'TOO FLAT - the Fo table is not reaching the bone',
                 elw, 'MOVES' if elw >= 40.0 else 'TOO FLAT - check kRideSwingBakeFo is applied'))
        print('    ⚠️ a window cut short by the fuse legitimately spans less; read the t= column - the')
        print('    span is only comparable when the window actually reached armt=1.00.')
    else:
        print('    spread ~= 0 was 「大臂旋转小臂不动」 proven from the measurement alone (T28/T29).')
        print('    worst spread %.3f => %s' % (worst, 'FLAT (elbow frozen)' if worst <= 0.05
                                               else 'NOT flat - something else moves the hand bone'))
    print('    ⚠️ the one thing that could break it without a bug: the guard clip keying the HAND')
    print('    bone\'s local POSITION (we own rotations only).  That would show up here and nowhere else.')

    # Witness 2: offline-vs-measured.  Under T30 that is the baked curve; under T28 one rotation.
    print('')
    if t30:
        print('  WITNESS 2 - the baked curve, offline vs measured')
        if scalerr:
            for j, (lab, unit) in enumerate((('r    ', 'u'), ('elbow', 'deg'), ('grip ', 'deg'))):
                v = sorted(s[j] for s in scalerr)
                bad = max(scalerr, key=lambda s: s[j])
                print('    %s |offline-measured|: median %.2f %s, worst %.2f %s (win%d t=%.2f), %d'
                      ' sample(s)' % (lab, v[len(v) // 2], unit, bad[j], unit, bad[3], bad[4], len(v)))
            print('    ⚠️ ANCHOR-FREE, so a mismatch here cannot be blamed on the saddle\'s heading; it')
            print('    means the shape on the bone is not the shape in the table.  ⚠️ but they share the')
            print('    DIRECTION line\'s two time floors (t= printed to 2 decimals, plus one frame of read')
            print('    lag): the elbow moves up to 0.8 deg/ms mid-stroke, so ~40 ms of slip is worth ~30')
            print('    deg here and is NOT a defect.  A real failure is a residual that does not shrink')
            print('    where the curve is slow - the settle rows at the end are the place to look.')
        else:
            print('    (no sample carried r=/elbow= and a prediction at the same time)')
        if prederr:
            prederr.sort()
            sl = sorted(p[3] for p in prederr if p[3] is not None)
            print('    DIRECTION (anchored once per window at its own first sample):')
            print('      |offline - measured| best %.2f u, worst %.2f u (win%d t=%.2f) against r~5.4,'
                  ' %d sample(s)' % (prederr[0][0], prederr[-1][0], prederr[-1][1], prederr[-1][2],
                                     len(prederr)))
            if sl:
                print('      as TIME SLIP (residual / the curve\'s own local speed): median %.0f ms,'
                      ' 90th %.0f ms, worst %.0f ms' % (sl[len(sl) // 2], sl[int(len(sl) * 0.9)],
                                                        sl[-1]))
                print('      🔑 read THIS, not the units: `t=%.2f` quantises the x axis to +-7 ms and'
                      ' the sample is')
                print('      one frame (~33 ms) after the write, so ~40 ms is the FLOOR - a stroke that')
                print('      merely moves fast cannot fail it, while a wrong-shaped one cannot pass it.')
            print('      🔑 this is the trip-23 guard: 「往下戳」 was a correctly measured curve pointing')
            print('      the wrong way, and the scalars above cannot see that.  The anchor is taken ONLY')
            print('      at t~0 (where the arm is still the host\'s pose) - re-anchoring per sample would')
            print('      cancel the very rotation being measured, since bx= is downstream of our write.')
        if tilts:
            tl = sorted(t[0] for t in tilts)
            print('    THE ANCHOR ITSELF (what the two witnesses above cannot see):')
            print('      C tilt %.1f .. %.1f deg over %d window(s); trip 26 measured 14.9 .. 24.5'
                  % (tl[0], tl[-1], len(tilts)))
            print('      => %s' % (
                'inside the seated-torso band (5..40) - the arm is where the host put it, plus heading'
                if 5.0 <= tl[0] and tl[-1] <= 40.0 else
                'BELOW the band - the torso carrying the arm is the standing file\'s, not a seated one.'
                ' Not a swing defect by itself: check the rider really was mounted' if tl[-1] < 5.0 else
                'ABOVE the band - a constant rotation is being applied that the seated torso does not'
                ' explain; see anchor_tilt'))
            print('      ⚠️ this is the ONLY witness here that a whole-window constant rotation cannot')
            print('      hide in (a synthetic log pitching the entire arm 60 deg still read 5 ms median')
            print('      slip above), and it is an EMPIRICAL band, not a derivation.  A stroke that')
            print('      passes everything and still looks wrong is exactly what the eyeball step is for.')
        else:
            print('    DIRECTION unavailable: needs bx=/bz= on the window\'s first row plus the bake.')
    elif angerr:
        angerr.sort()
        print('  WITNESS 2 - one rotation, not two directions: angle(measured, rot(first sample))')
        print('    best %.1f deg, worst %.1f deg (win%d t=%.2f), %d sample(s) compared'
              % (angerr[0][0], angerr[-1][0], angerr[-1][1], angerr[-1][2], len(angerr)))
        print('    this is the DLL\'s deg= applied to the LOG\'s own first measurement, so it tests the')
        print('    model and the ARC2/kRideSwingArc mirror at once.  It absorbs the same one-frame read')
        print('    lag dot= does (worst case = one frame of the arc\'s own rate), so read it next to')
        print('    the frame gap, not against an absolute bar.')
    else:
        print('  WITNESS 2 unavailable: no deg= and no elbow= on these rows (pre-T28 build).')

    # Witness 3: the grip.  This is the trip-25 complaint, measured.
    print('')
    print('  WITNESS 3 - the grip (正手/反手), from the weapon hand\'s own axes:')
    if bxerr:
        bxerr.sort()
        print('    angle(bx measured, rot(first bx)): best %.1f, worst %.1f deg (win%d t=%.2f)'
              % (bxerr[0][0], bxerr[-1][0], bxerr[-1][1], bxerr[-1][2]))
        print('    Under T28 the hand rides the arm rigidly, so this residual is NOT our roll - it is')
        print('    whatever the host clip keys into the wrist (R Hand is deliberately in neither mask')
        print('    table, so the grip stays the host\'s).  Small = the blade holds its attitude relative')
        print('    to the arm for the whole stroke; large = the host\'s own wrist track is now the')
        print('    remaining suspect, and putting R Hand in BOTH tables is a decision, not a tuning.')
    elif t30:
        print('    T30 has no deg=, so there is no rotation to remove; the model-free form below is the')
        print('    whole witness, and it is now a MATCH test, not a flatness test.')
    for w, win in enumerate(windows):
        for k, lab in ((0, 'out'), (1, 'fore'), (2, 'down')):
            vals = [r['bx'][k] for r in win if r['bx']]
            if not vals:
                continue
            if k == 0:
                print('    win%d raw bx spans:' % (w + 1), end='')
            print('  %s %.2f' % (lab, max(vals) - min(vals)), end='')
        if any(r['bx'] for r in win):
            print('')
    print('    ⚠️ the RAW spans above are the pre-T28 measure (trip 25 read 1.13/1.59/1.70 and the')
    print('    verdict was 「正手变反手」).  They are EXPECTED to be large now: the hand is supposed to')
    print('    travel with the arm.  Only the residual after removing the rotation is a flip.')
    # The SAME criterion ridelog.py pre-registers, computed the same way: angle(bx, arm).  Both
    # vectors turn with the arm, so the angle between them survives ANY rotation of the whole limb -
    # no deg=, no axis constant, no quaternion.  That is what let T28 measure it on an old log, and
    # it is also why T30 can compare it to the offline bake with no anchor: same two vectors, same
    # invariance.  ⚠️ but the VERDICT flips.  T28 froze the elbow, so the arm vector was rigid with
    # the hand and the spread had to be ~0.  T30 bends the elbow, so this angle MUST move, and by
    # the amount the table says - that is the offline column printed underneath.
    print('    the model-free form (ridelog.py registers this one):')
    gp = [r[6] for r in bake['rows']] if bake else []
    for w, win in enumerate(windows):
        a = [v for v in (angle_between(r['bx'], r['meas'])
                         for r in win if r['bx']) if v is not None]
        if len(a) >= 2:
            print('      win%d angle(bx,arm) %5.1f .. %5.1f   spread %5.1f'
                  % (w + 1, min(a), max(a), max(a) - min(a)))
    if t30:
        if gp:
            print('      offline, the whole stroke: %5.1f .. %5.1f   spread %5.1f  <= what the spans'
                  ' above should look like' % (min(gp), max(gp), max(gp) - min(gp)))
            print('      (per-sample |offline-measured| is the `grip` line in WITNESS 2 - that is the')
            print('      sharp version; these spans only catch a stroke that moves by the wrong amount.)')
        print('      ⚠️ a T28-shaped ~0 spread here is now a FAILURE: it would mean the forearm table')
        print('      never reached the bone, i.e. the arm rotated rigidly after all.')
    else:
        print('      trip 25 (the aimed table): spread 27.1 / 30.7 / 46.6 / 52.5 / 54.0 deg.')
        print('      T28 predicts ~0 plus whatever the host keys into the wrist.')

    # And the shoulder itself: if it travels, the host torso is animating, full stop.
    sh = [r['sh'] for r in rows if r['sh']]
    if sh:
        print('')
        print('  shoulder (sh=) travel across all samples:')
        for k, lab in ((0, 'x'), (1, 'y'), (2, 'z')):
            vals = [s[k] for s in sh]
            print('    %s: %6.2f .. %6.2f  (span %.2f)' % (lab, min(vals), max(vals),
                                                           max(vals) - min(vals)))
        print('    the shoulder is host-driven (we own UpperArm and Forearm, not the clavicle or')
        print('    spine), so a big span here IS the host torso moving while our arc plays.')
        if t30:
            print('    ⚠️ T30 minds LESS than T28 did: it composes onto the captured LOCALS, so a heaving')
            print('    torso carries the whole arm along instead of shearing it, and the three anchor-free')
            print('    scalars cannot see this at all.  It only reaches the DIRECTION line of WITNESS 2,')
            print('    whose anchor is one frame old by construction.')
        else:
            print('    ⚠️ T28 does not mind: the arc is applied to a pose captured in SKELETON space, so')
            print('    the torso can heave without deforming the stroke.  It is still the first thing to')
            print('    look at if WITNESS 2 is poor - the upper arm\'s parent is a host-driven clavicle')
            print('    and our write cancels it from a cache that is one frame old.')

    # The retired joint-angle model, kept alive only for the trip-23 log (§ARC above says why).
    if any(r['legacy'] for r in rows):
        print('')
        print('  legacy rows carry abd=/flx=/elb= => this is a T25 log; the retired comparison:')
        rat, ang = [], []
        for r in rows:
            if not r['legacy']:
                continue
            p = pose(bones, *r['legacy'])
            a = angle_between(r['meas'], p)
            lm, lp = vlen(r['meas']), vlen(p)
            rat.append(lm / lp if lp > 1e-6 else 0.0)
            if a is not None:
                ang.append(a)
        if rat and ang:
            print('    |measured|/|bind prediction|: %.3f .. %.3f   angle: %.1f .. %.1f deg'
                  % (min(rat), max(rat), min(ang), max(ang)))
            print('    a big ANGLE spread with a flat ratio was the trip-23 verdict: the arm geometry')
            print('    was right and the host frame turned under it.  That is why the model moved to')
            print('    skeleton space (T26) and then to one rotation of a captured pose (T28).')
    return 0


# ---- P4-3-4l: bake a vanilla clip's TWO arm curves ---------------------------------------------
# The third knob, and the only one bigger than axis+arc.  T28's model is ONE rigid rotation, so the
# hand rides ONE circle and the elbow is frozen - that is exactly what witness 1 (`r=` span 0.000)
# measures.  Every `chop` Kenshi ships moves the elbow 62..80 deg (`skelanims.py --sweep chop`), so
# the shape an eye reads as a chop is not on any circle and NO arc table can reach it.  Baking means:
# take the two arm tracks out of the .skeleton and write them as LOCAL rotations - the same space
# Bone::setOrientation takes, because keys are bind-relative (local = bind * key, RE_NOTES 19.7).
#
# Two forms, and they differ in exactly ONE property, so both are computed here:
#   DELTA     local(t) = captured * conj(K0) * K(t).  X(0) = identity => the window still opens on
#             the pose already on screen (17.19's handover property, kept).  Price: vanilla's stroke
#             is RE-BASED onto the saddle guard pose, so its direction is whatever that re-basing
#             makes it - trip 23 (「往下戳」) is the standing precedent for a correctly measured curve
#             pointing the wrong way on screen.
#   ABSOLUTE  local(t) = bind * K(t).  Vanilla verbatim - the shape the user has already accepted on
#             foot.  Price: a pop at window open, whose size is the `opening gap` printed below and
#             which only a cross-fade can hide.
BAKE_CLIP, HOST_CLIP = 'chop down', 'guard 1h'
# ⚠️ MIRRORED, and this is the pair that says WHICH bake the .cpp is holding.  check_mirror() re-bakes
# with exactly these and diffs; if you ship a different form or tempo, change them here in the same
# commit or the checker will (correctly) call the .cpp drifted - and it will name the combination that
# does match, so the fix is never a guess.
BAKE_FORM, BAKE_MAP = 'delta', 'native'
ARM = (UPPER, FORE)
# ⚠️ MIRRORS NOTHING in the .cpp.  It is T28's/T29's measured landing instant, kept only as the target
# the 'lead' mapping aims at; the shape of the path does not depend on it.
LAND_MS = 1092


def qconj(q):
    return (q[0], -q[1], -q[2], -q[3])


def qang(a, b):
    """Degrees between two unit quaternions; |dot| folds q and -q together."""
    d = abs(sum(a[i] * b[i] for i in range(4)))
    return 2.0 * math.degrees(math.acos(min(1.0, d)))


def qalign(ref, q):
    """q or -q, whichever shares a hemisphere with ref.  The DISK IS NOT SIGN-ALIGNED - `chop down`'s
    forearm flips sign between key 1 and key 2 - and a dumb nlerp across a sign flip takes the long
    way round the sphere.  Aligning at BAKE time is what lets the DLL's interpolator stay dumb."""
    return q if sum(ref[i] * q[i] for i in range(4)) >= 0.0 else tuple(-c for c in q)

_SKEL = []


def _skel():
    """male_skeleton.skeleton, parsed ONCE per process.  It is 8 MB of chunks and a pure-python walk;
    --mirror + --bake would otherwise parse it four times for the same bytes."""
    if not _SKEL:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import skelanims
        _SKEL.append(skelanims.parse(HUMAN))
    return _SKEL[0]


def clip_tracks(clip):
    """-> (seconds, {boneName: [(t, quat)]}) with quats in ARMARC order (w,x,y,z) and sign-aligned.

    skelanims.py owns the parser - one per format is the rule this project already paid for once -
    and it hands back DISK order (x,y,z,w), so the reorder happens here and only here."""
    sk = _skel()
    bh = sk.by_handle()
    for (name, secs, tracks) in sk.anims:
        if name.lower() != clip.lower():
            continue
        out = {}
        for h, keys in tracks.items():
            prev, seq = (1.0, 0.0, 0.0, 0.0), []
            for (t, q, _x) in keys:
                w = qalign(prev, (q[3], q[0], q[1], q[2]))
                seq.append((t, w))
                prev = w
            out[bh[h]] = seq
        return secs, out
    return None, None


def host_pose(bones, clip):
    """The LOCAL rotation of every bone the HOST clip animates, at its key 0 = the pose our window
    opens on.  P4-3-4i pins the host to `guard 1h`; `spread` prints how quasi-static that clip really
    is over its own keys, so the claim is checked rather than assumed."""
    secs, tr = clip_tracks(clip)
    if tr is None:
        return None, None, None
    ov = dict((n, qmul(bones[n]['rot'], k[0][1])) for n, k in tr.items() if n in bones)
    spread = dict((n, max(qang(tr[n][0][1], k[1]) for k in tr[n])) for n in ARM if n in tr)
    return secs, ov, spread


def bake_table(bones, K, n, form, ts, tail):
    """-> [(t, w, x, y, z)] for ONE bone: exactly the rows the .cpp holds, synthetic tail included.

    🔑 ONE implementation, two callers: bake_report() prints it and check_mirror() diffs it.  That is
    the whole reason the AXIS/ARC2 hand-mirror could be deleted - the checker's side is DERIVED from
    the asset by the same code that generated the .cpp's side, so there is no second hand-copy to
    drift.  Sign-aligned against the PREVIOUS row (not against identity): the DLL's interpolator is a
    dumb nlerp between neighbours, so neighbours are what must share a hemisphere."""
    out, prev = [], (1.0, 0.0, 0.0, 0.0)
    for i in range(len(K[n])):
        k0, k = K[n][0][1], K[n][i][1]
        q = qalign(prev, qmul(qconj(k0), k) if form == 'delta' else qmul(bones[n]['rot'], k))
        out.append((ts[i],) + q)
        prev = q
    if tail:
        out.append((1.0, 1.0, 0.0, 0.0, 0.0))
    return out


def bake_tail(form, ts):
    """Does the .cpp table get the SYNTHETIC settle row?  Only the delta form can have one (identity
    means 'the captured pose' there, and nothing else), and only if the clip ends before the arc does."""
    return form == 'delta' and ts[-1] < 0.999


def bake_curve(bones, form=None, mapping=None, clip=None, host=None):
    """-> {'ts','rows','lab','off'} : the SHIPPED offline stroke, i.e. what the .cpp's two tables
    encode, sampled on its own keys and carrying the synthetic settle key.  None if an asset is
    missing, so report_log can fall back to the measurement-only witnesses instead of dying.

    'off' is the offline R Hand FRAME at window open - the src half of the one rotation that carries
    the whole prediction into the game's frame (log_anchor's docstring is the why)."""
    secs, K = clip_tracks(clip or BAKE_CLIP)
    if K is None or any(n not in K for n in ARM):
        return None
    _hs, base, _sp = host_pose(bones, host or HOST_CLIP)
    if base is None:
        return None
    form, mapping = form or BAKE_FORM, mapping or BAKE_MAP
    rows = bake_path(bones, base, K, form)
    ts, lab = bake_times(rows, secs, mapping)
    if bake_tail(form, ts):
        # The synthetic key's delta is IDENTITY, so the pose there IS the captured pose = row 0's.
        # (Only its tsec column is meaningless, and nothing reads that.)
        ts, rows = ts + [1.0], rows + [rows[0]]
    _p, rot0 = derived(bones, HAND, bake_local(bones, base, K, 0, form))
    return {'ts': ts, 'rows': rows, 'lab': lab,
            'off': frame_of(qrot(rot0, (1.0, 0.0, 0.0)), qrot(rot0, (0.0, 0.0, 1.0)))}


def curve_at(cur, t):
    """-> (relLog, r, elbowDeg, gripDeg) linearly sampled at WINDOW time t - the same t the DLL
    computes (elapsed / kRideSwingArcMs), so a log row and this share an x axis with nothing fitted.
    ⚠️ the DLL nlerps the QUATERNIONS and then does forward kinematics; this interpolates the
    resulting geometry.  Between keys 27.6 ms apart that difference is second-order - far under the
    one-frame read lag it sits next to - but it is not zero, so do not read the last digit."""
    ts, rows = cur['ts'], cur['rows']
    i = 1
    if t >= ts[-1]:
        i = len(ts) - 1
    else:
        while i < len(ts) - 1 and t > ts[i]:
            i += 1
    a, b = rows[i - 1], rows[i]
    span = ts[i] - ts[i - 1]
    u = 0.0 if span <= 1e-9 else max(0.0, min(1.0, (t - ts[i - 1]) / span))
    return (tuple(a[1][j] + (b[1][j] - a[1][j]) * u for j in range(3)),
            a[2] + (b[2] - a[2]) * u, a[3] + (b[3] - a[3]) * u, a[6] + (b[6] - a[6]) * u)


def bake_local(bones, base, K, i, form):
    """The full LOCAL-rotation override at clip key i, for one of the two forms."""
    ov = dict(base)
    for n in ARM:
        k0, k = K[n][0][1], K[n][i][1]
        ov[n] = qmul(base[n], qmul(qconj(k0), k)) if form == 'delta' else qmul(bones[n]['rot'], k)
    return ov


def curve_rate(cur, t):
    """-> how fast the offline hand moves at window time t, in units per MILLISECOND.

    🔑 This is what turns a residual into a verdict.  Two floors are baked into any comparison with a
    log row and NEITHER is a defect: the line prints `t=%.2f`, so the x axis is quantised to +-0.005 =
    +-7 ms of the 1400 ms arc, and the sample itself is read one frame (~33 ms at 30 fps) after the
    write.  Both are TIME errors, so dividing the position residual by this rate says how many ms of
    slip would explain it - and the fastest段 of this curve runs ~91 u/s, i.e. 7 ms of t rounding alone
    is worth 0.6 u.  Comparing raw units against a fixed bar would fail the stroke for being fast."""
    ts, rows = cur['ts'], cur['rows']
    i = 1
    if t >= ts[-1]:
        i = len(ts) - 1
    else:
        while i < len(ts) - 1 and t > ts[i]:
            i += 1
    span = (ts[i] - ts[i - 1]) * ARC_MS
    d = vlen(tuple(rows[i][1][j] - rows[i - 1][1][j] for j in range(3)))
    return 0.0 if span <= 1e-6 else d / span


def bake_path(bones, base, K, form, corr=None):
    """-> [(tsec, (out,fore,down), r, elbowJointDeg, upDeg, elbDeg, gripDeg)] over the clip's OWN keys.

    rel(t) = P * [Lu(t)*posFore + Lu(t)*Lf(t)*posHand]: only the two arm locals and the two bone
    offsets shape it, and P (everything above the shoulder) merely carries it - which is why one
    measured rotation (`corr`, from log_anchor) is enough to move the whole path into the game's
    frame, and why the elbow column below needs no correction at all (an angle is rotation-blind).
    🔑 THE LAST THREE COLUMNS NEED NO ANCHOR AT ALL, which is what makes them the T30 witnesses:
    r, the elbow joint angle and grip = angle(hand's own +X, shoulder->hand) are all scalars built
    from vectors that P carries TOGETHER, so the saddle can rotate the whole arm as much as it likes
    without moving any of them.  report_log compares the measured ones against these directly."""
    rows = []
    for i in range(len(K[UPPER])):
        ov = bake_local(bones, base, K, i, form)
        sh, _ = derived(bones, UPPER, ov)
        el, _ = derived(bones, FORE, ov)
        hd, hr = derived(bones, HAND, ov)
        rel = tuple(hd[j] - sh[j] for j in range(3))
        grip = angle_between(qrot(hr, (1.0, 0.0, 0.0)), rel)
        if corr:
            rel = rebase(rel, corr[0], corr[1])
        rows.append((K[UPPER][i][0], to_log(rel), vlen(rel),
                     180.0 - angle_between(tuple(el[j] - sh[j] for j in range(3)),
                                           tuple(hd[j] - el[j] for j in range(3))),
                     qang(K[UPPER][0][1], K[UPPER][i][1]),
                     qang(K[FORE][0][1], K[FORE][i][1]),
                     grip if grip is not None else -1.0))
    return rows


def bake_times(rows, secs, mapping):
    """Clip seconds -> window t in 0..1.  The PATH does not depend on this; the tempo and the instant
    the cut lands do.  native = vanilla speed, lead = vanilla speed slid so the cut lands where T28/
    T29 land theirs, stretch = the clip smeared over the whole arc (slower than vanilla by secs/arc)."""
    if mapping == 'stretch':
        return [r[0] / secs for r in rows], 'stretch (%.2fx vanilla)' % (ARC_MS / (secs * 1000.0))
    ms = [r[0] * 1000.0 for r in rows]
    lead = 0.0
    if mapping == 'lead':
        lead = LAND_MS - ms[max(range(len(rows)), key=lambda i: rows[i][1][2])]
    return ([min(1.0, (m + lead) / ARC_MS) for m in ms],
            'native tempo' + (' + %.0f ms lead-in' % lead if lead else ''))

def frame_of(bx, bz):
    """-> (x, y, z), the orthonormal COLUMNS of a rotation, from its own +X and +Z.  Gram-Schmidt, so
    a 2-decimal log is precise enough: bz only supplies the roll about x."""
    x = vnorm(bx)
    z = vnorm(tuple(bz[i] - sum(bz[j] * x[j] for j in range(3)) * x[i] for i in range(3)))
    return (x, (z[1] * x[2] - z[2] * x[1],       # y = z cross x
                z[2] * x[0] - z[0] * x[2],
                z[0] * x[1] - z[1] * x[0]), z)


def rebase(v, src, dst):
    """v, carried by the rotation dst * src^-1.  Express v in src's basis, rebuild it in dst's: three
    dot products, and it IS the correction C = R_measured * conj(R_offline) without ever building a
    quaternion for it."""
    c = tuple(sum(v[j] * src[k][j] for j in range(3)) for k in range(3))
    return tuple(sum(c[k] * dst[k][i] for k in range(3)) for i in range(3))


SKEL_UP = (0.0, 1.0, 0.0)          # the standing file's up axis; the log prints down = -y


def anchor_tilt(src, dst):
    """-> (tiltDeg, totalDeg) for the correction C = dst * src^-1: how far C moves the skeleton's UP
    axis, and C's whole rotation angle.

    🔑 Why the pair matters.  The anchored DIRECTION witness absorbs C by construction, so a constant
    rotation we apply BY MISTAKE for a whole window is invisible to it - proven with a synthetic log
    that pitched the entire arm 60 deg and still read a 5 ms median slip.  `total` cannot separate the
    two either, because the saddle's heading legitimately puts tens of degrees there.  `tilt` is the
    part heading CANNOT explain: a yaw about up leaves up alone.
    ⚠️ MEASURED, not derived: trip 26's six windows read tilt 14.9 / 19.3 / 19.7 / 19.7 / 19.9 / 24.5
    deg against totals of 19.5 .. 93.9, i.e. the seated torso really is ~20 deg off the standing file
    (the straddle writer pins Bip01/Pelvis and the spine is whatever the host holds) - so the bar is a
    BAND around 20, not zero.  Outside roughly 5..40 deg something other than the seated torso is
    carrying the arm, and that is the one class of error the scalars and the direction both miss."""
    up = rebase(SKEL_UP, src, dst)
    tr = sum(sum(dst[k][i] * src[k][i] for k in range(3)) for i in range(3))
    return (angle_between(up, SKEL_UP) or 0.0,
            math.degrees(math.acos(max(-1.0, min(1.0, (tr - 1.0) / 2.0)))))


def log_anchor(path, bones, base):
    """-> (offlineFrame, [(measuredFrame, measuredRelSkel, t)]) - the R Hand bone's OWN frame at every
    window's first arm sample, measured, beside the same frame computed offline from the host clip.

    🔑 Why this exists: the standing skeleton file cannot know the SADDLE.  `Bip01` and `Bip01 Pelvis`
    are pinned by the straddle writer and the spine is whatever the host holds while seated, so
    everything above the shoulder is rotated relative to the file - measured 42 deg of it.  The arm's
    own configuration is unaffected (r=5.42 both ways, the elbow matches), so ONE rotation carries an
    offline prediction into the game's frame, and the log itself supplies it: bx=/bz= are that bone's
    derived +X and +Z.  At a window's first sample S(0)=identity, so the arm there IS the host's pose -
    which is exactly the pose the offline side computes."""
    rows = [r for r in read_log(path) if r['bx'] and r['bz']]
    firsts, prev = [], 9.9
    for r in rows:
        if r['t'] <= prev and r['t'] < 0.05:
            firsts.append(r)
        prev = r['t']
    _, rot = derived(bones, HAND, base)
    off = frame_of(qrot(rot, (1.0, 0.0, 0.0)), qrot(rot, (0.0, 0.0, 1.0)))
    return off, [(frame_of(to_skel(r['bx']), to_skel(r['bz'])), to_skel(r['meas']), r['t'])
                 for r in firsts]


def bake_report(bones, clip, form, mapping, reflog=None):
    secs, K = clip_tracks(clip)
    if K is None:
        print('no clip %r in %s  (try `python tools\\skelanims.py --find %s`)'
              % (clip, os.path.basename(HUMAN), clip.split(' ')[0]))
        return 2
    missing = [n for n in ARM if n not in K]
    if missing:
        print('clip %r has no track for %s - nothing to bake' % (clip, ', '.join(missing)))
        return 2
    hsecs, base, spread = host_pose(bones, HOST_CLIP)
    if base is None:
        print('no host clip %r - the window-open pose cannot be modelled offline' % HOST_CLIP)
        return 2
    nk = len(K[UPPER])
    print('')
    print('BAKE  %r  %.3f s  %d keys (%.1f ms apart)   host %r  %.3f s over %d bones'
          % (clip, secs, nk, secs * 1000.0 / max(1, nk - 1), HOST_CLIP, hsecs, len(base)))
    print('  host quasi-static over its OWN keys: %s'
          % '  '.join('%s %.1f deg' % (n.replace('Bip01 ', ''), d) for n, d in sorted(spread.items())))
    print('    small = P4-3-4i really did park the arm, so ONE captured pose describes the window.')
    sh, _ = derived(bones, UPPER, base)
    hd, _ = derived(bones, HAND, base)
    rel0 = to_log(tuple(hd[i] - sh[i] for i in range(3)))
    res = vlen(tuple(rel0[i] - REF_MEASURED[i] for i in range(3)))
    print('  SELF-CHECK  offline rel(0)  out %6.2f fore %6.2f down %6.2f   r=%.2f'
          % (rel0 + (vlen(rel0),)))
    print('              measured       out %6.2f fore %6.2f down %6.2f   r=%.2f   <- REF_MEASURED, trip 25'
          % (REF_MEASURED + (vlen(REF_MEASURED),)))
    print('              residual %.2f u, %.0f deg apart at the SAME r => %s'
          % (res, angle_between(rel0, REF_MEASURED) or 0.0,
             'the standing chain already matches the saddle' if res < 0.5 else
             'the arm CONFIGURATION matches (r, elbow) but the SADDLE rotates everything above the'
             ' shoulder - anchor it'))
    corr = None
    if reflog:
        off, meas = log_anchor(reflog, bones, base)
        if not meas:
            print('  ANCHOR  %s has no first-of-window arm sample carrying bx=/bz= - the prediction'
                  ' stays UN-ANCHORED (standing frame)' % os.path.basename(reflog))
        else:
            corr = (off, meas[0][0])
            rel0s, p0 = to_skel(rel0), rebase(to_skel(rel0), off, meas[0][0])
            print('  ANCHOR  %s, %d window(s).  C = measured R Hand frame * conj(offline R Hand frame):'
                  % (os.path.basename(reflog), len(meas)))
            for i, (fr, mr, tt) in enumerate(meas):
                p = rebase(rel0s, off, fr)
                tl, tot = anchor_tilt(off, fr)
                print('    win %2d t=%.2f  C*rel(0) -> out %5.2f fore %5.2f down %5.2f   |err| %.2f u'
                      '   %4.1f deg from win 0   C: tilt %4.1f total %5.1f'
                      % ((i, tt) + to_log(p)
                         + (vlen(tuple(p[j] - mr[j] for j in range(3))), angle_between(p, p0) or 0.0,
                            tl, tot)))
            print('    |err| is the whole offline model against the game in ONE number, and nothing in')
            print('    it is fitted: the arm locals come from %r, the frame from bx=/bz=, the target'
                  % HOST_CLIP)
            print('    from out=/fore=/down= on that same row.  The 「deg from win 0」 column is how much')
            print('    the SADDLE POSE itself moved between windows - a big spread means no single C')
            print('    describes it.  C\'s own tilt/total is the anchor\'s honesty check: heading can put')
            print('    any amount in `total` but nothing in `tilt`, so tilt is the only part of a')
            print('    constant mistaken rotation the anchored witness cannot swallow (anchor_tilt says')
            print('    why, and why the bar is a band around 20 deg rather than zero).')
    sel = None
    for f in ('delta', 'absolute'):
        rows = bake_path(bones, base, K, f, corr)
        ts, tlab = bake_times(rows, secs, mapping)
        print('')
        print('  %s form%s   [%s]' % (f.upper(), '   <- selected' if f == form else '', tlab))
        if f == 'absolute':
            print('    opening gap: %s' % '  '.join(
                '%s %.1f deg' % (n.replace('Bip01 ', ''), qang(base[n], qmul(bones[n]['rot'], K[n][0][1])))
                for n in ARM))
            print('      = how far the arm JUMPS at window open; a cross-fade has to hide exactly this.')
        print('       t    ms |  up(deg) elb(deg) |    out   fore   down |     r  elbow  grip |'
              '  step   u/s')
        for i, r in enumerate(rows):
            ch = 0.0 if not i else vlen(tuple(r[1][j] - rows[i - 1][1][j] for j in range(3)))
            dms = 0.0 if not i else (ts[i] - ts[i - 1]) * ARC_MS
            print('   %5.3f %5.0f | %7.1f %7.1f | %6.2f %6.2f %6.2f | %5.2f %6.1f %5.1f | %5.2f %5.1f'
                  % (ts[i], ts[i] * ARC_MS, r[4], r[5], r[1][0], r[1][1], r[1][2], r[2], r[3], r[6],
                     ch, ch / (dms / 1000.0) if dms > 1e-6 else 0.0))
        ic = min(range(len(rows)), key=lambda i: rows[i][1][2])     # highest hand = the cock
        step = lambda i: vlen(tuple(rows[i][1][j] - rows[i - 1][1][j] for j in range(3)))
        peak = max([step(i) for i in range(ic + 1, len(rows))] or [0.0])
        it = ic
        for i in range(ic + 1, len(rows)):    # the cut ENDS where the descent's speed collapses; past
            if step(i) < 0.2 * peak:          # that the clip is only settling, and calling it travel
                break                         # would credit the stroke with the follow-through
            it = i
        hi, lo = rows[ic], rows[it]
        vert, lat = abs(lo[1][2] - hi[1][2]), abs(lo[1][0] - hi[1][0])
        # 🔑 lat and fore are each YAW-DEPENDENT (the saddle frame turned 0..39 deg between trip 26's
        # windows, nearly pure yaw), so vert/lat can only be compared with T28's 0.83 and T29's 2.17
        # because those were measured in the same convention.  horiz = |(out, fore)| is yaw-INVARIANT,
        # and so is vert, so vert/horiz is the ratio that means the same thing in any window.
        horiz = math.sqrt((lo[1][0] - hi[1][0]) ** 2 + (lo[1][1] - hi[1][1]) ** 2)
        chord = vlen(tuple(lo[1][j] - hi[1][j] for j in range(3)))
        dms = max(1e-6, (ts[it] - ts[ic]) * ARC_MS)
        print('    span: %s | r %.2f..%.2f (%.2f) | elbow %.0f..%.0f (%.0f deg) | grip %.0f..%.0f (%.0f)'
              % (' | '.join('%s %.2f..%.2f (%.2f)'
                            % (lab, min(x), max(x), max(x) - min(x))
                            for lab, x in (('out', [r[1][0] for r in rows]),
                                           ('fore', [r[1][1] for r in rows]),
                                           ('down', [r[1][2] for r in rows]))),
                 min(r[2] for r in rows), max(r[2] for r in rows),
                 max(r[2] for r in rows) - min(r[2] for r in rows),
                 min(r[3] for r in rows), max(r[3] for r in rows),
                 max(r[3] for r in rows) - min(r[3] for r in rows),
                 min(r[6] for r in rows), max(r[6] for r in rows),
                 max(r[6] for r in rows) - min(r[6] for r in rows)))
        print('    cock(clip %.3fs, t=%.3f) -> through(clip %.3fs, t=%.3f):'
              ' vertical %.2f lateral %.2f forward %.2f => vert/lat %.2f  vert/horiz %.2f (yaw-invariant)'
              % (hi[0], ts[ic], lo[0], ts[it], vert, lat, abs(lo[1][1] - hi[1][1]),
                 vert / lat if lat else float('inf'), vert / horiz if horiz else float('inf')))
        print('    chord %.2f over %.0f ms = %.1f u/s;  the cut lands %.0f ms into the window of %d ms'
              % (chord, dms, chord / (dms / 1000.0), ts[it] * ARC_MS, WIN_MS))
        if f == form:
            sel = (rows, ts)
    rows, ts = sel
    path = [(ts[i], rows[i][1]) for i in range(len(rows))]
    print('')
    plot(path, 1, 2, 'fore', 'down')
    print('')
    plot(path, 0, 2, 'out', 'down')
    print('')
    print('  C++ for RidingPlugin.cpp - the %s form of %r, sign-aligned so a dumb nlerp cannot take'
          % (form, clip))
    print('  the long way round.  ⚠️ paste BOTH tables or neither, and if this is not %s/%s then'
          % (BAKE_FORM, BAKE_MAP))
    print('  BAKE_FORM/BAKE_MAP at the top of this file move in the SAME commit - check_mirror()')
    print('  re-bakes with them and diffs every value, so a half-paste reads as drift.')
    tail = bake_tail(form, ts)
    for n, tag in ((UPPER, 'Up'), (FORE, 'Fo')):
        tab = bake_table(bones, K, n, form, ts, tail)
        print('')
        print('static const float kRideSwingBake%s[][5] = {   // t, w, x, y, z   (%s)'
              % (tag, n))
        for i, row in enumerate(tab):
            print('    { %6.4ff, %9.6ff, %9.6ff, %9.6ff, %9.6ff },%s'
                  % (row + (('   // synthetic settle onto the captured pose - NOT from the clip'
                             if tail and i == len(tab) - 1 else ''),)))
        print('};')
        print('static const int kRideSwingBake%sKeys = %d;' % (tag, len(tab)))
    return 0


# ---- the mirror, T30 form: DERIVE this side, do not hand-copy it -------------------------------
MIRROR_TOL = 1e-4          # the .cpp prints %.6f / %.4f, so anything above rounding is a real edit


def cpp_bake_table(src, tag):
    """-> ([(t,w,x,y,z)], KeysConstant, [complaint]) parsed out of the .cpp's kRideSwingBake<tag>.

    Parsed, not imported: the source is C++.  A regex that fails to match is reported as a FAILURE,
    never a silent pass - a checker that quietly finds nothing to check is worse than no checker."""
    bad, rows = [], []
    m = re.search(r'kRideSwingBake%s\s*\[\s*\]\s*\[\s*5\s*\]\s*=\s*\{(.*?)^\s*\}\s*;' % tag,
                  src, re.S | re.M)
    if m is None:
        bad.append('could not parse the kRideSwingBake%s table out of the .cpp' % tag)
    else:
        for raw in re.findall(r'\{([^{}]*)\}', m.group(1)):
            v = [float(x) for x in re.findall(r'-?[0-9]+\.[0-9]+', raw.split('//')[0])]
            if len(v) != 5:
                bad.append('kRideSwingBake%s row %d has %d number(s), expected 5: %r'
                           % (tag, len(rows), len(v), raw.strip()))
            else:
                rows.append(tuple(v))
    k = re.search(r'kRideSwingBake%sKeys\s*=\s*([0-9]+)\s*;' % tag, src)
    keys = None if k is None else int(k.group(1))
    if keys is None:
        bad.append('could not parse kRideSwingBake%sKeys' % tag)
    elif rows and keys != len(rows):
        bad.append('kRideSwingBake%sKeys = %d but the table has %d rows - the .cpp is'
                   ' self-inconsistent, which the compiler will NOT catch either'
                   % (tag, keys, len(rows)))
    return rows, keys, bad


def check_mirror(bones=None, cpp=None):
    """RE-BAKE the clip out of male_skeleton.skeleton and diff EVERY value against the .cpp's two
    baked tables (plus ArcMs / WinMs, which this file still holds twice).

    ⚠️ THIS IS THE ONE DRIFT NOTHING ELSE CAN CATCH.  The compiler never sees this script and this
    script never sees the DLL, so a half-finished edit leaves an offline report that describes a
    stroke the game is not drawing - and every conclusion drawn from it is then wrong about the
    right thing.  That is why every mode runs this first and refuses to report on a mismatch.
    🆕 T30 makes it STRICTLY STRONGER than the AXIS/ARC2 mirror it replaces: there is no second
    hand-copy of the numbers any more.  This side is derived from the ASSET by bake_table(), the
    same function that printed the .cpp's side, so the only way to fail is for the .cpp to have been
    hand-edited, generated from another clip/form/tempo, or for the asset itself to have changed.
    On a mismatch the other five (form, tempo) combinations are tried and any exact match is NAMED,
    so 'the .cpp is a --map lead bake' arrives as a fact instead of a guess."""
    if cpp is None:
        cpp = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'RidingPlugin.cpp')
    if not os.path.isfile(cpp):
        print('MIRROR: no RidingPlugin.cpp at %s' % cpp)
        return 2
    src = io.open(cpp, encoding='utf-8', errors='replace').read()
    bad = []

    def one(name):
        m = re.search(r'kRideSwing%s\s*=\s*(-?[0-9.]+)f?\s*;' % name, src)
        return None if m is None else float(m.group(1))

    # Still two hand-held copies, because this file turns t into ms with them.
    for nm, mine in (('ArcMs', ARC_MS), ('WinMs', WIN_MS)):
        v = one(nm)
        if v is None:
            bad.append('could not parse kRideSwing%s' % nm)
        elif int(v) != mine:
            bad.append('%s: this file %d vs .cpp %d' % (nm, mine, int(v)))

    theirs = {}
    for tag in ('Up', 'Fo'):
        rows, _keys, cb = cpp_bake_table(src, tag)
        theirs[tag] = rows
        bad.extend(cb)

    if not os.path.isfile(HUMAN):
        print('MIRROR: no skeleton at %s - the tables cannot be re-baked, so nothing below can be'
              ' trusted' % HUMAN)
        return 2
    if bones is None:
        bones = read_bones(HUMAN)
    for n in (UPPER, FORE, HAND):
        if n not in bones:
            print('MIRROR: no bone %r in the skeleton - the arm chain is not what this file assumes'
                  % n)
            return 2
    secs, K = clip_tracks(BAKE_CLIP)
    if K is None or any(n not in K for n in ARM):
        print('MIRROR: clip %r has no arm track in %s - the tables cannot be re-baked'
              % (BAKE_CLIP, os.path.basename(HUMAN)))
        return 2
    _hs, base, _sp = host_pose(bones, HOST_CLIP)
    if base is None:
        print('MIRROR: no host clip %r - the window-open pose cannot be modelled offline' % HOST_CLIP)
        return 2

    def rebake(form, mapping):
        rows = bake_path(bones, base, K, form)
        ts, _lab = bake_times(rows, secs, mapping)
        tail = bake_tail(form, ts)
        return dict((tag, bake_table(bones, K, n, form, ts, tail))
                    for n, tag in ((UPPER, 'Up'), (FORE, 'Fo')))

    def diff(mine):
        out = []
        for tag in ('Up', 'Fo'):
            a, b = mine[tag], theirs[tag]
            if len(a) != len(b):
                out.append('kRideSwingBake%s has %d rows, the re-bake has %d' % (tag, len(b), len(a)))
                continue
            for i in range(len(a)):
                w = max(abs(a[i][j] - b[i][j]) for j in range(5))
                if w > MIRROR_TOL:
                    out.append('kRideSwingBake%s row %d: .cpp %s vs re-bake %s   worst |d| %.6f'
                               % (tag, i, ' '.join('%9.6f' % v for v in b[i]),
                                  ' '.join('%9.6f' % v for v in a[i]), w))
        return out

    mine = rebake(BAKE_FORM, BAKE_MAP)
    rowdiff = diff(mine) if all(theirs.values()) else []
    bad.extend(rowdiff[:12])
    if len(rowdiff) > 12:
        bad.append('... and %d more row(s)' % (len(rowdiff) - 12))
    if rowdiff:
        for f in ('delta', 'absolute'):
            for mp in ('native', 'lead', 'stretch'):
                if (f, mp) != (BAKE_FORM, BAKE_MAP) and not diff(rebake(f, mp)):
                    bad.append('BUT the .cpp matches form=%s tempo=%s EXACTLY => the .cpp is that'
                               ' bake and BAKE_FORM/BAKE_MAP here are stale, not the tables' % (f, mp))

    if bad:
        # ASCII only in this function's own output: it must be printable on a cp936 console even
        # when nobody has called main()'s reconfigure() - a checker that dies while reporting a
        # failure is worse than no checker.
        print('MIRROR FAIL - the offline model and the DLL DISAGREE:')
        for b in bad:
            print('    %s' % b)
        print('  -> fix before trusting anything below, and before rebuilding: an offline report')
        print('     that describes a different stroke than the game draws is worse than none.')
        print('  -> the tables are GENERATED.  Do not hand-patch a row to silence this; re-run')
        print('     `python tools\\armarc.py --bake "%s"%s` and paste both tables again.'
              % (BAKE_CLIP, '' if BAKE_MAP == 'native' else ' --map ' + BAKE_MAP))
        return 1
    hinge = max(max(abs(r[2]), abs(r[4])) for r in theirs['Fo'])
    print('MIRROR OK - %r re-baked from %s and diffed value by value (tol %g):'
          % (BAKE_CLIP, os.path.basename(HUMAN), MIRROR_TOL))
    print('    kRideSwingBakeUp %d rows, kRideSwingBakeFo %d rows, form=%s tempo=%s,'
          ' ArcMs=%d WinMs=%d' % (len(theirs['Up']), len(theirs['Fo']), BAKE_FORM, BAKE_MAP,
                                  ARC_MS, WIN_MS))
    print('    forearm hinge check: max |x|,|z| over the Fo table = %.6f => %s'
          % (hinge, 'PURE HINGE, so the grip claim in the .cpp holds' if hinge <= MIRROR_TOL else
             'NOT a pure hinge - the .cpp comment says the grip survives because x=z=0; it does not'))
    print('    AXIS/ARC2 in this file are the RETIRED T28/T29 model - the .cpp no longer holds them,')
    print('    so they are NOT mirrored; they exist only to decode pre-T30 logs.')
    return 0


def main(argv):
    # A cp936 / cp1252 console must not abort the report over one glyph (same as ridelog.py).
    try:
        sys.stdout.reconfigure(errors='replace')
    except (AttributeError, ValueError):
        pass
    if '--mirror' in argv:
        return check_mirror()
    path = HUMAN
    if not os.path.isfile(path):
        print('no skeleton at %s' % path)
        return 2
    bones = read_bones(path)
    for n in (UPPER, FORE, HAND):
        if n not in bones:
            print('missing bone %r - the arm chain is not what T25 assumes' % n)
            return 2
    # Every other mode reports numbers that are supposed to describe the stroke the DLL draws, so the
    # mirror is checked FIRST and a mismatch is fatal: a report that silently describes a different
    # stroke is the one failure mode this file cannot survive.  ⚠️ `--mirror` alone is the standalone
    # form; here the parsed skeleton is handed over so the 8 MB walk happens once.
    rc = check_mirror(bones)
    if rc:
        return rc
    print('skeleton: %s  (%d bones)' % (os.path.basename(path), len(bones)))
    print('bind-pose sanity (lengths are |local position|, i.e. bone lengths):')
    for n in (UPPER, FORE, HAND, 'Bip01 R Calf', 'Bip01 R Foot'):
        if n in bones:
            b = bones[n]
            print('  %-18s parent=%-18s len=%.3f  handle=%d'
                  % (n.replace('Bip01 ', ''), (b['parent'] or '-').replace('Bip01 ', ''),
                     vlen(b['pos']), b['h']))
    print('  ⚠️ R Calf len is the FEMUR (hip->knee): §16 measured 4.17/4.18 in game, so a wildly')
    print('     different number here means this parser or the frame is wrong, not the arc.')
    sh, _ = derived(bones, UPPER, {})
    hd, _ = derived(bones, HAND, {})
    reach = vlen(tuple(hd[i] - sh[i] for i in range(3)))
    straight = vlen(bones[FORE]['pos']) + vlen(bones[HAND]['pos'])
    print('  bind: shoulder=(%.2f,%.2f,%.2f) hand=(%.2f,%.2f,%.2f)' % (sh + hd))
    print('  QUATERNION ORDER CHECK: bind shoulder->hand = %.2f, straight-arm sum = %.2f'
          % (reach, straight))
    print('    within a few percent = the on-disk order (x,y,z,w) is right and the hierarchy')
    print('    composes; wildly short = the order is wrong and every angle below is noise.')
    o, f, d = pose(bones, 0.0, 0.0, 0.0)
    print('  bind: out=%.2f fore=%.2f down=%.2f  (arms hang down => down should dominate)'
          % (o, f, d))
    print('  bone-local child offsets (does local +X really run along the bone?):')
    for n, lab in ((FORE, 'UpperArm->Forearm'), (HAND, 'Forearm->Hand')):
        p = bones[n]['pos']
        print('    %-18s (%7.3f,%7.3f,%7.3f)  |p|=%.3f' % (lab, p[0], p[1], p[2], vlen(p)))
    print('    one dominant component = that axis is the bone axis.  ⚠️ T28 does not need this any more')
    print('    (a rigid rotation carries the hand wherever it already was), but the log\'s want= term')
    print('    still uses both lengths, so a wrong parse would show up as a bad dot= in game.')
    if '--bind' in argv:
        return 0

    if '--log' in argv:
        return report_log(bones, argv[argv.index('--log') + 1])

    if '--bake' in argv:
        i = argv.index('--bake') + 1
        clip = argv[i] if i < len(argv) and not argv[i].startswith('--') else BAKE_CLIP
        return bake_report(bones, clip, 'absolute' if '--abs' in argv else BAKE_FORM,
                           argv[argv.index('--map') + 1] if '--map' in argv else BAKE_MAP,
                           argv[argv.index('--ref-log') + 1] if '--ref-log' in argv else None)

    if '--abd' in argv:
        g = lambda k, dv: float(argv[argv.index(k) + 1]) if k in argv else dv
        a, x, e = g('--abd', 0.0), g('--flx', 0.0), g('--elb', 0.0)
        o, f, d = pose(bones, a, x, e)
        print('one pose (RETIRED joint-angle model): abd=%.1f flx=%.1f elb=%.1f -> out=%.2f fore=%.2f'
              ' down=%.2f' % (a, x, e, o, f, d))
        return 0

    # ---- the live model, sampled -----------------------------------------------------------------
    ref = REF_MEASURED
    lab = 'measured trip-25 open pose'
    if '--bindref' in argv:
        ref, lab = pose(bones, 0.0, 0.0, 0.0), 'BIND pose (a bound, not a prediction)'
    if '--ref' in argv:
        i = argv.index('--ref')
        ref = tuple(float(c) for c in argv[i + 1:i + 4])
        lab = 'given on the command line'
    print('')
    print('the authored arc (kRideSwingArc, T28 rotation model), 17 samples')
    print('  axis  out=%.2f fore=%.2f down=%.2f   (skeleton %.4f,%.4f,%.4f, unit)'
          % (AXIS + AXIS_SKEL))
    print('  ref   out=%.2f fore=%.2f down=%.2f   r=%.3f   <- %s' % (ref + (vlen(ref), lab)))
    cone = angle_between(to_skel(ref), AXIS_SKEL)
    print('  hand-vector cone half-angle %.1f deg: the hand travels on a circle of radius'
          ' r*sin(cone) = %.2f,' % (cone, vlen(ref) * math.sin(math.radians(cone))))
    print('  so a cone near 0 or 180 would mean a big angle draws a small stroke.')
    print('  ⚠️ NOT the same number as the DLL\'s cone=, which is the angle between the axis and the')
    print('  UPPER ARM\'s own +X.  The log gives shoulder->hand (a sum of two segments), so the upper')
    print('  arm\'s direction is not recoverable from it - the two cones are cousins, not copies.')
    print('     t     deg  |   HAND out    fore    down |    r')
    rows = []
    for i in range(17):
        t = i / 16.0
        h = hand_at(t, ref)
        rows.append((t, h))
        print('  %5.2f %6.1f  | %7.2f %7.2f %7.2f | %7.3f' % ((t, deg_at(t)) + h + (vlen(h),)))
    print('  ⚠️ the r column is CONSTANT by construction - that is the point, and `--log` checks the')
    print('  game against it (WITNESS 1).  It is the one criterion no read lag can degrade.')
    spans = []
    for k, lab in ((0, 'out'), (1, 'fore'), (2, 'down')):
        vals = [r[1][k] for r in rows]
        spans.append((lab, min(vals), max(vals), max(vals) - min(vals)))
    print('  span: ' + ' | '.join('%s %.2f..%.2f (%.2f)' % s for s in spans))
    print('  key hand positions, and how far the hand moves between consecutive keys:')
    prev, prevt = None, None
    for k in ARC2:
        h = hand_at(k[0], ref)
        chord = None if prev is None else vlen(tuple(h[i] - prev[i] for i in range(3)))
        print('    t=%.2f  deg %6.1f  out %6.2f  fore %6.2f  down %6.2f%s'
              % ((k[0], k[1]) + h
                 + (('   chord %5.2f over %.2f of the window' % (chord, k[0] - prevt))
                    if chord is not None else '',)))
        prev, prevt = h, k[0]
    print('  the chord is a straight-line distance, not arc length: it is what makes a stroke read as')
    print('  one cut rather than a drift.  ⚠️ ONE number decides the AMPLITUDE and it is the total')
    print('  swept angle - %.0f deg between the extremes here.'
          % (max(k[1] for k in ARC2) - min(k[1] for k in ARC2)))
    print('  per-segment tempo (ARC_MS = %d):' % ARC_MS)
    for i in range(1, len(ARC2)):
        a, b = ARC2[i - 1], ARC2[i]
        dt, dd = (b[0] - a[0]) * ARC_MS, abs(b[1] - a[1])
        ch = vlen(tuple(hand_at(b[0], ref)[j] - hand_at(a[0], ref)[j] for j in range(3)))
        print('    %6.0f ms  %5.0f deg  %4.0f deg/s   chord %5.2f = %4.1f u/s%s'
              % (dt, dd, dd / (dt / 1000.0) if dt else 0.0, ch,
                 ch / (dt / 1000.0) if dt else 0.0, '   (hold)' if dd == 0 else ''))
    lo = min(ARC2, key=lambda k: k[1])
    hi = max(ARC2, key=lambda k: k[1])
    hc, ht = hand_at(lo[0], ref), hand_at(hi[0], ref)
    vert, lat = abs(ht[2] - hc[2]), abs(ht[0] - hc[0])
    print('  cock->through travel: vertical %.2f  lateral %.2f  forward %.2f => vert/lat %.2f'
          % (vert, lat, abs(ht[1] - hc[1]), vert / lat if lat else float('inf')))
    print('  ⚠️ vert/lat IS the difference between a chop and a sideways spread (T28 shipped 0.83 and the')
    print('  verdict named it: 「侧面张开大臂带动刀」).  It is set by WHERE on the circle the window sits,')
    print('  not by the amplitude - the circle itself only moves if the AXIS does.')
    print('  the cut lands %.0f ms into the window of %d ms (through reaches its extreme at t=%.2f)'
          % (hi[0] * ARC_MS, WIN_MS, hi[0]))
    print('')
    plot(rows, 1, 2, 'fore', 'down')
    print('')
    plot(rows, 0, 2, 'out', 'down')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))


