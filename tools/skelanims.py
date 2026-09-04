# skelanims.py - read an Ogre .skeleton OFFLINE: bones, clip names, and KEYFRAME CURVES.  No game.
#
#   python tools\skelanims.py                       # male_skeleton: bone count + every clip name
#   python tools\skelanims.py --find "chop"         # ...only clips whose name contains a substring
#   python tools\skelanims.py --skel Crab           # another skeleton by filename substring
#   python tools\skelanims.py --list                # every .skeleton under data\, with clip counts
#   python tools\skelanims.py --bones               # handle / parent / name / bind quat
#   python tools\skelanims.py --sweep [chop]        # per clip: shoulder sweep + ELBOW bend, degrees
#   python tools\skelanims.py --track "chop down"   # one clip's curve for one bone (--bone <name>)
#
# Why the curve half exists: Kenshi ships every humanoid animation as tracks in ONE file, and an
# Ogre track's keyframe rotation is RELATIVE TO THE BONE'S BINDING POSE (Skeleton::reset() then
# Node::rotate(key, TS_LOCAL) => local = bind * key).  That is the same space RideSwingArmPose
# writes into, so a vanilla arm curve can be read here and replayed by OUR writer with OUR bone
# mask - which is NOT the family trip 22 killed (that one drove an engine AnimationState through
# the animation system).  Measured proof the rotations really are bind-relative: in `guard 1h` the
# track for `Bip01 L Toe0Nub` - whose bind quat is a distinctive (0,1,0,0) - is exactly identity on
# all 3 keys.  Absolute-local storage could not do that.
#
# Why it exists at all: `gamedata.py --tech` showed that 43 of 44 COMBAT TECHNIQUE(17) records name a
# clip (`chop left`, `attack1`, `blk right`, `dodgeback`) that has NO ANIMATION(24) record anywhere
# in the four data files.  A clip with no record has no `layer` string and no `wholeBodyAllLayer`
# bool - those two fields live on the RECORD, not on the skeleton track.  So the question "which
# layer is a swing on" cannot be answered from FCS for 43 of the 44; this script is the other half
# of the proof - it shows the clip really does exist, as a bare track in the skeleton file.
# See RidingPlugin_RE_NOTES.md 19.
#
# Format (Ogre 1.x .skeleton, little endian):
#   [u16 0x1000][str version]                       header; str is '\n'-terminated, NO length field
#   then chunks   [u16 id][u32 recordedLength] <payload>
#   0x1010 BLENDMODE       [u16]
#   0x2000 BONE            [str name][u16 handle][3f pos][4f rot]  (+[3f scale] iff non-unit)
#   0x3000 BONE_PARENT     [u16 handle][u16 parent]
#   0x4000 ANIMATION       [str name][f32 seconds]  then nested 0x4010 / 0x4100 / 0x4110
#   0x4100 TRACK           [u16 boneHandle]         then nested 0x4110 keyframes
#   0x4110 KEYFRAME        [f32 t][4f rot][3f xlat] (+[3f scale])
#   0x5000 ANIMATION_LINK  [str skeletonName][f32 scale]
# WARNING a chunk's recorded length CANNOT be walked blindly: Ogre's own `calcBoneSize()` omits the
#   name string, so every BONE chunk under-reports by exactly nameLen+1 (male_skeleton bone 0 says
#   36, really occupies 42).  A flat length walk therefore lands mid-name and stops after one bone -
#   measured, that is exactly what happened here first.  So this parser walks by the layouts above
#   and uses the recorded length only where it is trustworthy: BONE 36-vs-48 tells us whether the
#   optional scale triple is present, and keyframes (whose calc has no string) are skipped by it.
#
# WARNING these are asset-side facts, same caveat as gamedata.py: a track name proves the clip
# exists and gives its duration.  It says NOTHING about layer, `whole`, blending or runtime weight
# - none of those are stored here.

import math
import os
import struct
import sys

KENSHI = r'D:\steam\steamapps\common\Kenshi'
DATA = os.path.join(KENSHI, 'data')
HUMAN = os.path.join(DATA, r'character\meshes\male_skeleton\male_skeleton.skeleton')

C_BLEND, C_BONE, C_PARENT = 0x1010, 0x2000, 0x3000
C_ANIM, C_BASEINFO, C_TRACK, C_KEYFRAME, C_LINK = 0x4000, 0x4010, 0x4100, 0x4110, 0x5000


def _u16(b, o):
    return struct.unpack_from('<H', b, o)[0]


def _u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def _f32(b, o):
    return struct.unpack_from('<f', b, o)[0]


def _line(b, o):
    """Ogre writes strings as raw bytes terminated by '\\n' (no length prefix)."""
    e = b.find(b'\n', o)
    if e < 0:
        e = len(b)
    return b[o:e].decode('utf-8', 'replace'), e + 1


class Skel(object):
    """One file, fully read.  Quaternions are (x, y, z, w) - the DISK order.  (armarc.py's
    read_bones() reorders to (w,x,y,z) for its own math; do not move numbers between the two
    tools without converting.)

    bones   [(handle, name, pos, quat)] in file order
    parents {handle: parentHandle}
    anims   [(name, seconds, {boneHandle: [(t, quat, xlat)]})]
    """

    def __init__(self, version):
        self.version = version
        self.bones = []
        self.parents = {}
        self.anims = []

    def by_handle(self):
        return dict((h, n) for (h, n, _p, _q) in self.bones)

    def by_name(self):
        return dict((n, h) for (h, n, _p, _q) in self.bones)


def parse(path):
    """The one parser in this file.  Flat single pass: a TRACK belongs to the animation last
    seen and a KEYFRAME to the track last seen, so no container end offset is ever needed -
    which is what makes it immune to the length quirk described at the top."""
    b = open(path, 'rb').read()
    if len(b) < 8 or _u16(b, 0) != 0x1000:
        raise ValueError('%s: not an Ogre .skeleton (first chunk 0x%04X)' % (path, _u16(b, 0)))
    ver, o = _line(b, 2)
    sk = Skel(ver)
    track = None
    while o + 6 <= len(b):
        cid, ln = _u16(b, o), _u32(b, o + 2)
        p = o + 6
        if cid == C_BONE:
            nm, p = _line(b, p)
            pos = (_f32(b, p + 2), _f32(b, p + 6), _f32(b, p + 10))
            quat = (_f32(b, p + 14), _f32(b, p + 18), _f32(b, p + 22), _f32(b, p + 26))
            sk.bones.append((_u16(b, p), nm, pos, quat))
            p += 2 + 12 + 16 + (12 if ln >= 48 else 0)  # handle, pos, rot, optional scale
        elif cid == C_PARENT:
            sk.parents[_u16(b, p)] = _u16(b, p + 2)
            p += 4
        elif cid == C_BLEND:
            p += 2
        elif cid == C_ANIM:
            nm, p = _line(b, p)
            sk.anims.append((nm, _f32(b, p), {}))
            track = None
            p += 4                                      # nested chunks follow, keep walking flat
        elif cid in (C_BASEINFO, C_LINK):
            _, p = _line(b, p)
            p += 4
        elif cid == C_TRACK:
            if not sk.anims:
                raise ValueError('%s: TRACK at 0x%X with no ANIMATION open' % (path, o))
            track = sk.anims[-1][2].setdefault(_u16(b, p), [])
            p += 2
        elif cid == C_KEYFRAME:
            if track is None:
                raise ValueError('%s: KEYFRAME at 0x%X with no TRACK open' % (path, o))
            track.append((_f32(b, p),
                          (_f32(b, p + 4), _f32(b, p + 8), _f32(b, p + 12), _f32(b, p + 16)),
                          (_f32(b, p + 20), _f32(b, p + 24), _f32(b, p + 28))))
            p = o + ln                                  # no string inside -> length is honest
        else:
            raise ValueError('%s: unknown chunk 0x%04X at 0x%X' % (path, cid, o))
        if p <= o:
            raise ValueError('%s: no progress at 0x%X (chunk 0x%04X)' % (path, o, cid))
        o = p
    return sk


def read(path):
    """Names-only view, kept for --list / --tech / gamedata cross-checks:
    (version, [boneName], [(animName, seconds)])."""
    sk = parse(path)
    return sk.version, [n for (_h, n, _p, _q) in sk.bones], [(n, s) for (n, s, _t) in sk.anims]


def _pick(pat):
    """A .skeleton path from a filename substring; no pattern -> the human skeleton."""
    if not pat:
        return HUMAN
    hits = []
    for root, _, files in os.walk(DATA):
        for f in files:
            if f.lower().endswith('.skeleton') and pat.lower() in f.lower():
                hits.append(os.path.join(root, f))
    if not hits:
        print('no .skeleton whose filename contains %r' % pat)
        return None
    hits.sort(key=lambda p: len(os.path.basename(p)))
    if len(hits) > 1:
        print('%d match(es), using the shortest name:' % len(hits))
        for h in hits[:8]:
            print('   %s' % h)
    return hits[0]


def cmd_dump(skel, find):
    p = _pick(skel)
    if not p:
        return 2
    ver, bones, anims = read(p)
    print('%s\n  version=%s  bones=%d  animations=%d' % (p, ver, len(bones), len(anims)))
    rows = [(n, s) for n, s in anims if not find or find.lower() in n.lower()]
    for n, s in sorted(rows):
        print('   %-34s %.3fs' % (n, s))
    if find:
        print('\n%d of %d clip(s) contain %r' % (len(rows), len(anims), find))
    print('NOTE asset-side only: a track carries NO layer and NO `whole` bit (those are on the'
          ' ANIMATION record, see gamedata.py).')
    return 0


def cmd_list():
    rows = []
    for root, _, files in os.walk(DATA):
        for f in files:
            if not f.lower().endswith('.skeleton'):
                continue
            p = os.path.join(root, f)
            try:
                _, bones, anims = read(p)
                rows.append((len(anims), len(bones), os.path.relpath(p, DATA)))
            except Exception as e:
                rows.append((-1, -1, '%s  (%s)' % (os.path.relpath(p, DATA), e)))
    rows.sort(key=lambda r: -r[0])
    print('%-6s %-6s %s' % ('anims', 'bones', 'file'))
    for a, bo, rel in rows:
        print('%-6s %-6s %s' % (a if a >= 0 else 'ERR', bo if bo >= 0 else '-', rel))
    print('\n%d .skeleton file(s)' % len(rows))
    return 0


def _index():
    """{clipName: [skeleton relpath]} over every .skeleton under data\\."""
    idx = {}
    for root, _, files in os.walk(DATA):
        for f in files:
            if not f.lower().endswith('.skeleton'):
                continue
            p = os.path.join(root, f)
            try:
                _, _, anims = read(p)
            except Exception:
                continue
            for n, _s in anims:
                idx.setdefault(n, []).append(os.path.relpath(p, DATA))
    return idx


def cmd_tech():
    """Cross-check: every COMBAT TECHNIQUE(17) `anim name` against the actual skeleton tracks.

    This is the corroborating half of the premise-2 proof.  `gamedata.py --tech` shows 43 of the 44
    techniques name a clip with NO ANIMATION(24) record - i.e. no `layer` string and no
    `wholeBodyAllLayer` bool exist for them ANYWHERE in the four data files.  This table shows the
    same clips DO exist, as bare tracks inside .skeleton assets.  Together: the clip is real, and the
    two fields the engine would need in order to describe it are simply not authored."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import gamedata
    recs = set(r.name for r in gamedata.union(types=(gamedata.ANIM, 5)).values())
    techs = sorted(gamedata.union(types=(gamedata.TECH,)).values(), key=lambda r: r.name.lower())
    idx = _index()
    _, _, hum = read(HUMAN)
    human = set(n for n, _s in hum)
    print('%-30s %-24s %-5s %-6s %s' % ('technique', 'anim name', 'rec?', 'human', 'skeleton(s) with the track'))
    noRec = noTrack = 0
    for t in techs:
        an = t.f.get('anim name', '')
        hits = idx.get(an, [])
        noRec += 0 if an in recs else 1
        noTrack += 0 if hits else 1
        where = '%d: %s' % (len(hits), os.path.dirname(hits[0]).split(os.sep)[-1]) if hits else 'NONE'
        print('%-30s %-24s %-5s %-6s %s' % (
            t.name[:30], an[:24], 'yes' if an in recs else '-', 'yes' if an in human else '-', where))
    print('\n%d technique(s): %d name a clip with NO ANIMATION/ANIMAL-ANIM record, '
          '%d have no skeleton track either' % (len(techs), noRec, noTrack))
    print('=> for the %d record-less clips, `layer` and `wholeBodyAllLayer` DO NOT EXIST in data\\ -'
          ' those two fields live on the record, not on the track.' % noRec)
    return 0


UPPER, FORE = 'Bip01 R UpperArm', 'Bip01 R Forearm'


def _qangle(a, b):
    """Degrees between two unit quaternions; |dot| folds q and -q together."""
    d = abs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3])
    return 2.0 * math.degrees(math.acos(min(1.0, d)))


def cmd_bones(skel):
    p = _pick(skel)
    if not p:
        return 2
    sk = parse(p)
    bh = sk.by_handle()
    print('%s\n  version=%s  bones=%d  animations=%d' % (p, sk.version, len(sk.bones), len(sk.anims)))
    print('handle  name                          parent                    bind quat (x,y,z,w)')
    for (h, n, _pos, q) in sorted(sk.bones):
        par = sk.parents.get(h, -1)
        print('%6d  %-28s  %-22s  %7.4f %7.4f %7.4f %7.4f'
              % (h, n, 'ROOT' if par < 0 else bh.get(par, '?%d' % par),
                 q[0], q[1], q[2], q[3]))
    print('NOTE bind LOCAL orientation, i.e. the pose every track\'s keys are relative to.')
    return 0


def cmd_sweep(skel, find):
    """How much SHAPE does each clip carry for the two bones our writer owns?

    up    = the R UpperArm's largest departure from its OWN first key (shoulder sweep)
    elbow = the same for R Forearm - the thing the T28 one-rigid-rotation model cannot do at
            all, because it rewrites the captured forearm local verbatim every frame.
    Both are measured against key 0, which is the normalisation a replay would use anyway
    (the window opens on the pose already on screen => deltas from the first key)."""
    p = _pick(skel)
    if not p:
        return 2
    sk = parse(p)
    bn = sk.by_name()
    up, fo = bn.get(UPPER, -1), bn.get(FORE, -1)
    rows = []
    for (name, secs, tracks) in sk.anims:
        if find and find.lower() not in name.lower():
            continue
        vals = []
        for h in (up, fo):
            keys = tracks.get(h, [])
            vals.append(max(_qangle(keys[0][1], k[1]) for k in keys) if len(keys) > 1 else 0.0)
        rows.append((name, secs, len(tracks.get(up, [])), vals[0], vals[1]))
    print('%s\n  %s handle=%d   %s handle=%d' % (p, UPPER, up, FORE, fo))
    print('  len(s) keys   up(deg)  elbow(deg)  name')
    for (name, secs, nk, u, f) in sorted(rows, key=lambda r: -r[3]):
        print('  %6.3f %4d   %7.1f  %10.1f  %s' % (secs, nk, u, f, name))
    print('\n%d clip(s)%s; elbow(deg) > 0 is shape one rigid rotation cannot reach'
          % (len(rows), (' containing %r' % find) if find else ''))
    return 0


def cmd_track(skel, clip, bone):
    p = _pick(skel)
    if not p:
        return 2
    sk = parse(p)
    want = [a for a in sk.anims if a[0].lower() == clip.lower()]
    if not want:
        print('no clip named %r - run --find %r to see the near misses' % (clip, clip[:6]))
        return 2
    name, secs, tracks = want[0]
    h = sk.by_name().get(bone, -1)
    if h < 0:
        print('no bone named %r - run --bones' % bone)
        return 2
    keys = tracks.get(h, [])
    print('%s\n  clip %r  length=%.3fs   bone %r (handle %d)  keys=%d'
          % (p, name, secs, bone, h, len(keys)))
    print('    t(s)  t/len  ang(deg)       qx       qy       qz       qw     tx     ty     tz')
    k0 = keys[0][1] if keys else (0.0, 0.0, 0.0, 1.0)
    for (t, q, tr) in keys:
        print('  %6.3f  %5.3f  %8.1f %8.5f %8.5f %8.5f %8.5f  %5.2f %5.2f %5.2f'
              % (t, (t / secs) if secs > 0 else 0.0, _qangle(k0, q),
                 q[0], q[1], q[2], q[3], tr[0], tr[1], tr[2]))
    print('\nNOTE rotations are RELATIVE TO BIND (local = bind * key) - the space')
    print('     Bone::setOrientation takes.  ang(deg) = distance from THIS clip\'s key 0,')
    print('     i.e. the arc table the clip implies for that bone.')
    return 0




USAGE = ('usage: skelanims.py [--skel <nameSubstr>] [--find <clipSubstr>]\n'
         '                    | --list | --tech | --bones | --sweep [<clipSubstr>]\n'
         '                    | --track "<clip>" [--bone "<boneName>"]')


def main(argv):
    skel = find = ''
    clip = ''
    bone = UPPER
    mode = ''
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == '--list':
            return cmd_list()
        if a == '--tech':
            return cmd_tech()
        if a in ('--bones', '--sweep'):
            mode = a
            i += 1
            continue
        if a in ('--skel', '--find', '--track', '--bone') and i + 1 < len(argv):
            v = argv[i + 1]
            if a == '--skel':
                skel = v
            elif a == '--find':
                find = v
            elif a == '--track':
                clip, mode = v, '--track'
            else:
                bone = v
            i += 2
            continue
        if not a.startswith('--') and not find:        # bare arg = --find, the common case
            find = a
            i += 1
            continue
        print(USAGE)
        return 2
    if mode == '--bones':
        return cmd_bones(skel)
    if mode == '--sweep':
        return cmd_sweep(skel, find)
    if mode == '--track':
        return cmd_track(skel, clip, bone)
    return cmd_dump(skel, find)


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
