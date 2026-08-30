# gamedata.py - read Kenshi's FCS data files (gamedata.base / *.mod) offline.  No game, no FCS.
#
#   python tools\gamedata.py                        # itemType histogram, per file
#   python tools\gamedata.py --verify               # parser self-check: header count vs scanned
#   python tools\gamedata.py --type 24              # every record of one itemType
#   python tools\gamedata.py --anim                 # ANIMATION(24) table: layer / category / flags
#   python tools\gamedata.py --anim "mid blow"      # ...filtered by name substring
#   python tools\gamedata.py --tech                 # COMBAT TECHNIQUE(17) -> its `anim name`, resolved
#   python tools\gamedata.py --rec "sitting chair"  # every field of the matching record(s)
#   python tools\gamedata.py --refs Dog             # reference categories of the matching record(s)
#
# Why this exists: a clip's LAYER and its `whole` bit decide whether any *other* animation can
# reach the skeleton at all (CLAUDE.md「关键机制」: a `wholeBodyAllLayer` pose pins every other
# layer's clip at `w=0.000`, measured in P2-1b-1).  Both facts live in the FCS ANIMATION record,
# i.e. inside these files - so they are readable WITHOUT entering the game.
#
# ONE container format, all four files (decoded 2026-08-30/31; see RidingPlugin_RE_NOTES.md §19).
#   header  [u32 fileVer][...][str depFiles]  BE 67 4C 00  [u32 recordCount]  <record>*
#           BE 67 4C 00 is the anchor: recordCount sits right behind it, record 0 right behind that.
#   record  [u32 size][u32 itemType][u32 id][str name][str stringID][u32 flag] <body>
#           `size` INCLUDES its own 4 bytes.  It is FILLED IN only in rebirth.mod; gamedata.base /
#           Newwworld.mod / Dialogue.mod leave it 0 -> the walk cannot lean on it.  What it CAN lean
#           on is <body>, which self-delimits: `off = bodyEnd` lands on the next record's `size`
#           field for every record of every file (verified: all four files walk header-exact and
#           finish precisely on EOF, and where size != 0 it agrees with bodyEnd 8571/8571).
#           flag: the LOW NIBBLE carries add/modify (0 or 2 = this file introduces the stringID,
#           1 or 3 = it modifies one), the high bits are a per-file family: `.base`/`Newwworld` set
#           0x80000000, rebirth/Dialogue use 0x10/0x11/0x13 plus 0x40/0x60/0x70/0x80/0x90.  So a
#           whole-u32 comparison matches nothing - and even the nibble is NOT authoritative:
#           rebirth.mod has 1375 nibble-1/3 records whose stringID appears in no earlier file, and
#           Dialogue.mod has 1005 nibble-0 records that DO shadow an earlier one.  ⚠️ Drive the merge
#           off "have I seen this stringID before", never off the flag (that is what `union()` does).
#           A DELETED record is not a flag value either: it is an ordinary override whose body carries
#           the bool `REMOVED = 1` (104 rebirth / 26 Newwworld / 88 Dialogue).  `union()` honours it.
#   <body>  six value blocks
#             [u32 n](key,u8)* bools  [u32 n](key,f32)* floats  [u32 n](key,i32)* ints
#             [u32 n](key,3f)* vec3   [u32 n](key,4f)*  vec4    [u32 n](key,str)* strings
#           then [u32 n](key,str)* filenames,
#           then [u32 nRefCat] and per category
#             [str category][u32 n]([str targetStringID][i32 v0][i32 v1][i32 v2])*
#           then [u32 nInstances] and per instance
#             [str name][str targetStringID][3f pos][4f rot][u32 0]     (towns/zones/buildings)
#   `str` = [u32 len][bytes], utf-8.  stringID is usually `<num>-<file>.base|mod|quack` but a
#   hand-authored one (`PLAYER_WEAPONS`) is legal - do not regex it tighter than "printable".
#   A record's name may be EMPTY (3 across the four files) - never validate it as non-empty.
#
# ⚠️ These are AUTHORING-side fields, not runtime state.  P1 proved the two differ: `crawl idle
# down` reads `spd 0/0/0 play=0.00` in the table yet its `t01` advances every frame on a live
# character.  Use this to pick candidates and to read STATIC properties (layer, flags, which clip
# a technique names); never to predict what a clip does at runtime.
# ⚠️ Later files override earlier ones BY stringID (vanilla load order = gamedata.base,
# rebirth.mod, Newwworld.mod, Dialogue.mod; `data\mods.cfg` lists only the extra mods stacked on
# top).  `union()` applies that; a single-file scan does not.
# ⚠️ One file is a LOWER BOUND: gamedata.base holds only 75 of the 124 type-24 ANIMATION records
# (69 of the 117 reachable ones); rebirth.mod adds 51 more AND overrides 71 of base's 75.  Reading a
# single file therefore misses half the table and mis-reads most of the rest.  Same trap as race_id.py.

import collections
import os
import struct
import sys

KENSHI = r'D:\steam\steamapps\common\Kenshi'
DATA = os.path.join(KENSHI, 'data')
# vanilla load order; later stringID wins on a collision
FILES = ['gamedata.base', 'rebirth.mod', 'Newwworld.mod', 'Dialogue.mod']

MAGIC = b'\xbe\x67\x4c\x00'                            # sits right before the record count
FLAG_MOD, FLAG_ADD, FLAG_MOD3 = 1, 2, 3                # low byte of the record's flag u32

# itemType -> label, only the ones this project has needed
TYPES = {5: 'ANIMAL ANIM', 7: 'RACE', 16: 'BODY PART', 17: 'COMBAT TECHNIQUE', 19: 'DIALOGUE LINE',
         24: 'ANIMATION', 27: 'GLOBAL CONSTANTS', 31: 'DIALOG ACTION', 51: 'ITEM GROUP',
         62: 'JOB/STATION', 76: 'ANIMAL ANIM SET', 112: 'ANIMATION LIST'}
ANIM, TECH = 24, 17

# `layer` is a STRING in the record; the engine reports a LAYER plus a `whole` flag.
# 'all' == UPPER + wholeBodyAllLayer (cross-checked against P1's runtime dump, see main()).
LAYER = {'all': ('UPPER', True), 'upper': ('UPPER', False),
         'overlay': ('OVERLAY', False), 'lower': ('LOWER', False), 'tail': ('TAIL', False)}
CATEGORY = {0: 'NORMAL', 1: '?1', 2: 'RANGED?', 3: 'CARRIED', 4: 'SWIM', 5: 'GROUND?'}


class Rec(object):
    __slots__ = ('type', 'id', 'name', 'sid', 'off', 'file', 'ver', 'flag',
                 'f', 'files', 'refs', 'inst')

    def __init__(self, t, i, name, sid, off, file, ver):
        self.type, self.id, self.name, self.sid = t, i, name, sid
        self.off, self.file, self.ver = off, file, ver
        self.flag = 0                                  # low byte: 1/3 override, 2 added
        self.f = {}                                    # value blocks   {key: value}
        self.files = {}                                # filename block {key: str}
        self.inst = []                                 # instances [(name, targetStringID)]
        self.refs = {}                                 # {category: [(targetStringID, v0, v1, v2)]}

    def __repr__(self):
        return 'Rec(%d %r %r)' % (self.type, self.name, self.sid)


def _u(b, o):
    return struct.unpack_from('<I', b, o)[0]


def _str(b, o):
    n = _u(b, o)
    return b[o + 4:o + 4 + n].decode('utf-8', 'replace'), o + 4 + n


def _body(b, o, r):
    """Parse every block of <body> into `r`. -> offset of the next record's `size` field."""
    for size, fmt in ((1, '<B'), (4, '<f'), (4, '<i'), (12, '<3f'), (16, '<4f')):
        n = _u(b, o)
        o += 4
        for _ in range(n):
            k, o = _str(b, o)
            v = struct.unpack_from(fmt, b, o)
            r.f[k] = v[0] if len(v) == 1 else v
            o += size
    for tgt in (r.f, r.files):                         # strings, then the filename block
        n = _u(b, o)
        o += 4
        for _ in range(n):
            k, o = _str(b, o)
            tgt[k], o = _str(b, o)
    n = _u(b, o)                                       # reference categories
    o += 4
    for _ in range(n):
        cat, o = _str(b, o)
        m = _u(b, o)
        o += 4
        lst = []
        for _ in range(m):
            sid, o = _str(b, o)
            lst.append((sid,) + struct.unpack_from('<3i', b, o))
            o += 12
        r.refs[cat] = lst
    n = _u(b, o)                                       # instances (towns/zones/buildings)
    o += 4
    for _ in range(n):
        nm, o = _str(b, o)
        tgt, o = _str(b, o)
        r.inst.append((nm, tgt))
        o += 32                                        # [3f pos][4f rot][u32 0]
    return o


def _rec_head(b, o, file, ver):
    """[u32 itemType][u32 id][str name][str stringID] -> (Rec, offset of the flag u32)."""
    t, i = _u(b, o), _u(b, o + 4)
    name, o2 = _str(b, o + 8)
    sid, o3 = _str(b, o2)
    return Rec(t, i, name, sid, o, file, ver), o3


def _scan(b, name):
    """Sequential walk, anchored on MAGIC, delimited by <body> itself (`size` is 0 in three of the
    four files).  Returns (records, headerCount, endOffset, sizeMismatches).  A single bad body
    desyncs everything after it, so the caller cross-checks the count and the end offset - both are
    exact on vanilla, which is what makes this walk trustworthy in the first place."""
    a = b.find(MAGIC)
    if a < 0:
        return [], 0, 0, 0
    want = _u(b, a + 4)
    ver = _u(b, 0)
    p, out, bad = a + 8, [], 0
    while len(out) < want and p + 16 < len(b):
        size = _u(b, p)
        try:
            r, o2 = _rec_head(b, p + 4, name, ver)
            r.flag = _u(b, o2)
            end = _body(b, o2 + 4, r)
        except Exception as e:                          # desync or truncation: stop, don't guess
            out.append(Rec(-1, 0, '', '', p, name, ver))
            out[-1].f['_parse_error'] = str(e)
            break
        if size and end != p + size:                   # rebirth.mod only; 0/8571 on vanilla
            bad += 1
            r.f['_size_mismatch'] = end - (p + size)
        out.append(r)
        p = end
    return out, want, p, bad


def scan(path):
    """Every record in one file, in file order."""
    b = open(path, 'rb').read()
    return _scan(b, os.path.basename(path))[0]


def verify(files=None):
    """Parser self-check: header count vs scanned, end offset vs EOF, parse errors, size mismatch.
    Everything downstream is only as good as this table - run it after touching the format code."""
    print('%-16s %-5s %-7s %-7s %-4s %-5s %-11s %s' % (
        'file', 'ver', 'header', 'scanned', 'err', 'size!', 'end/EOF', 'removed'))
    ok = True
    for fn in (files or FILES):
        p = os.path.join(DATA, fn)
        if not os.path.exists(p):
            print('%-16s MISSING' % fn)
            ok = False
            continue
        b = open(p, 'rb').read()
        rows, want, end, bad = _scan(b, fn)
        err = sum(1 for r in rows if '_parse_error' in r.f)
        rm = sum(1 for r in rows if r.f.get('REMOVED'))
        good = (len(rows) == want and err == 0 and bad == 0 and end == len(b))
        ok = ok and good
        print('%-16s %-5d %-7d %-7d %-4d %-5d %-11s %-7d %s' % (
            fn, _u(b, 0), want, len(rows), err, bad,
            '%d/%d' % (end, len(b)), rm, 'ok' if good else '** MISMATCH **'))
    print('\n%s' % ('all four files walk header-exact and land on EOF' if ok else
                    'walk is NOT clean - fix the format code before trusting anything below'))
    return 0 if ok else 1


def _merge(base, over):
    """Apply an override record onto the one it shadows. -> a new Rec (neither input is mutated).

    An override body carries ONLY the fields it changes (`walk upper` = 61 fields in
    gamedata.base, 4 in rebirth.mod), so a plain `out[sid] = r` loses everything the override did
    not restate - that is how 68 of 124 ANIMATION records came out with no `layer` at all.  Merge
    per key; reference categories merge per category (a restated category replaces its whole list)."""
    r = Rec(over.type, over.id, over.name or base.name, over.sid, over.off, over.file, over.ver)
    r.flag = over.flag
    r.f = dict(base.f)
    r.f.update(over.f)
    r.files = dict(base.files)
    r.files.update(over.files)
    r.refs = dict(base.refs)
    r.refs.update(over.refs)
    r.inst = over.inst or base.inst
    return r


def union(files=None, types=None):
    """All records across the vanilla load order, later stringID winning. -> {sid: Rec}
    An override MERGES onto the record it shadows (partial bodies, see `_merge`); a body carrying
    `REMOVED = 1` deletes that stringID (that is how FCS spells a deletion - there is no flag
    value for it).  A single-file `scan()` gives neither of those, so read the union unless you
    specifically want one file's own rows."""
    out = collections.OrderedDict()
    for fn in (files or FILES):
        p = os.path.join(DATA, fn)
        if not os.path.exists(p):
            continue
        for r in scan(p):
            if r.f.get('REMOVED'):
                out.pop(r.sid, None)
                continue
            if r.sid in out:
                out[r.sid] = _merge(out[r.sid], r)
            elif not types or r.type in types:
                out[r.sid] = r
    if types:
        for sid in [s for s, r in out.items() if r.type not in types]:
            del out[sid]
    return out


BOOL_ORDER = ['loop', 'is action', 'uses right arm', 'uses left arm', 'being carried',
              'relocates', 'prone', 'idle', 'delete weapons', 'delete tail', 'normal',
              'automatic', 'restricted', 'big stumble']


def flags(r):
    return ','.join(k for k in BOOL_ORDER if r.f.get(k))


def _anim_rows(pat, types=(ANIM,), withDisabled=False):
    """Rows for the ANIMATION table, `disabled` records excluded by default.

    That exclusion is what makes this table equal the engine's: the union holds 124 type-24
    records, 7 of them carry the bool `disabled = 1`, and dropping those 7 reproduces P1's live
    `allAnims` EXACTLY - 117 = UPPER 79 / OVERLAY 21 / LOWER 17 (see RE_NOTES.md 19).  So the
    engine skips a disabled record when it builds the list; do not count them as reachable."""
    rows, dis = [], 0
    for r in union(types=types).values():
        if r.f.get('disabled') and not withDisabled:
            dis += 1
            continue
        if pat and pat.lower() not in r.name.lower():
            continue
        lay, whole = LAYER.get(r.f.get('layer'), ('?', False))
        rows.append((r, lay, whole))
    rows.sort(key=lambda x: x[0].name.lower())
    return rows, dis


def cmd_anim(pat):
    rows, dis = _anim_rows(pat)
    print('%-30s %-8s %-7s %-8s %-16s %s' % ('record', 'layer', 'whole', 'cat', 'file', 'flags'))
    for r, lay, whole in rows:
        print('%-30s %-8s %-7s %-8s %-16s %s' % (
            r.name[:30], lay, 'whole' if whole else '-',
            CATEGORY.get(r.f.get('category'), r.f.get('category')), r.file, flags(r)))
    lc = collections.Counter(lay for _, lay, _ in rows)
    print('\n%d record(s)   layer: %s   whole: %d   (%d disabled record(s) excluded)' % (
        len(rows), dict(lc), sum(1 for _, _, w in rows if w), dis))
    print('NOTE layer/flags are FCS-side facts; runtime weight & timing do not follow from them.')
    return 0


def cmd_tech(pat):
    """COMBAT TECHNIQUE(17) -> the ANIMATION record its `anim name` names, if there is one.

    ⚠️ MEASURED 2026-08-31: for 43 of the 44 techniques there is NO record.  `anim name` is a raw
    CLIP name (`chop left`, `attack1`, `blk right`), and no type-24 ANIMATION nor type-5 ANIMAL ANIM
    record carries that name - not as a record name, not as any other record's `anim name` value.  A
    technique's only reference category is `events` (sfx).  `skelanims.py --tech` shows those same
    clips DO exist as bare tracks inside the .skeleton assets.
    => `layer` and `wholeBodyAllLayer` are RECORD fields, so for those 43 clips the two fields the
    engine would need in order to describe them are simply not authored anywhere in data\\.  That is
    the (negative) offline answer to P4-3 premise 2; the lone exception, `Downward cut static` ->
    `chop down static`, is UPPER and NOT whole.  See RidingPlugin_RE_NOTES.md 19."""
    anims = {}
    for r in union(types=(ANIM,)).values():
        anims[r.name] = r
    techs = [r for r in union(types=(TECH,)).values()
             if not pat or pat.lower() in r.name.lower()]
    techs.sort(key=lambda r: r.name.lower())
    print('%-28s %-24s %-8s %-7s %-4s %s' % (
        'technique', 'anim name', 'layer', 'whole', 'dis', 'flags'))
    missing, whole, lc = 0, 0, collections.Counter()
    for t in techs:
        an = t.f.get('anim name', '')
        a = anims.get(an)
        if a is None:
            missing += 1
            print('%-28s %-24s %s' % (t.name[:28], an, '(no ANIMATION record in data\\)'))
            continue
        lay, w = LAYER.get(a.f.get('layer'), ('?', False))
        lc[lay] += 1
        whole += 1 if w else 0
        print('%-28s %-24s %-8s %-7s %-4s %s' % (
            t.name[:28], an, lay, 'whole' if w else '-',
            'DIS' if a.f.get('disabled') else '-', flags(a)))
    print('\n%d technique(s), %d naming a clip with no ANIMATION record here' % (len(techs), missing))
    print('resolved clips: layer %s   whole: %d' % (dict(lc), whole))
    print('NOTE layer/flags are FCS-side facts; runtime weight & timing do not follow from them.')
    return 0


def cmd_rec(pat):
    n = 0
    for r in union().values():
        if pat.lower() not in r.name.lower():
            continue
        n += 1
        print('== [%d %s] %r  sid=%s  file=%s(v%d)  off=0x%X' % (
            r.type, TYPES.get(r.type, '?'), r.name, r.sid, r.file, r.ver, r.off))
        for k in sorted(r.f):
            print('   %-28s %r' % (k, r.f[k]))
        for k in sorted(r.files):
            print('   [file] %-21s %r' % (k, r.files[k]))
        for k in sorted(r.refs):
            print('   [refs] %-21s %d entry(s)' % (k, len(r.refs[k])))
    if not n:
        print('no record whose name contains %r' % pat)
    return 0


def cmd_refs(pat):
    names = {}
    for r in union().values():
        names[r.sid] = r
    for r in union().values():
        if pat.lower() not in r.name.lower() or not r.refs:
            continue
        print('== [%d %s] %r  sid=%s  file=%s' % (
            r.type, TYPES.get(r.type, '?'), r.name, r.sid, r.file))
        for cat in sorted(r.refs):
            print('   %s (%d)' % (cat, len(r.refs[cat])))
            for sid, v0, v1, v2 in r.refs[cat]:
                t = names.get(sid)
                lay = ''
                if t is not None and 'layer' in t.f:
                    l, w = LAYER.get(t.f.get('layer'), ('?', False))
                    lay = '  %s%s' % (l, '+whole' if w else '')
                print('      %-22s %-30s %d/%d/%d%s' % (
                    sid, (t.name if t else '(unresolved)')[:30], v0, v1, v2, lay))
    return 0


def cmd_type(t):
    for fn in FILES:
        p = os.path.join(DATA, fn)
        if not os.path.exists(p):
            continue
        rows = [r for r in scan(p) if r.type == t]
        print('-- %s : %d record(s) of type %d (%s)' % (fn, len(rows), t, TYPES.get(t, '?')))
        for r in rows:
            print('   %-40s %s' % (r.name[:40], r.sid))
    return 0


def cmd_hist():
    for fn in FILES:
        p = os.path.join(DATA, fn)
        if not os.path.exists(p):
            print('-- %s : MISSING' % fn)
            continue
        rows = scan(p)
        h = collections.Counter(r.type for r in rows)
        print('-- %s (v%d) : %d record(s)' % (fn, rows[0].ver if rows else 0, len(rows)))
        for t in sorted(h):
            sample = [r.name for r in rows if r.type == t][:2]
            print('   %3d %-18s %5d  %s' % (t, TYPES.get(t, ''), h[t], sample))
    return 0


USAGE = 'usage: gamedata.py [--verify] [--anim|--tech|--rec|--refs <pat>] [--type <N>]'


def main(argv):
    if not argv:
        return cmd_hist()
    a = argv[0]
    arg = argv[1] if len(argv) > 1 else ''
    if a == '--verify':
        return verify()
    if a == '--anim':
        return cmd_anim(arg)
    if a == '--tech':
        return cmd_tech(arg)
    if a == '--rec':
        return cmd_rec(arg)
    if a == '--refs':
        return cmd_refs(arg)
    if a == '--type':
        return cmd_type(int(arg, 0))
    print(USAGE)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
