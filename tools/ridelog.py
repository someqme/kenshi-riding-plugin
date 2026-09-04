# -*- coding: utf-8 -*-
"""ridelog.py - offline verdict table for one RidingPlugin diagnostic session.

Streams RE_Kenshi_log.txt line by line (it has reached 6.7 MB in the past -
never load it whole, never paste it into a chat) and prints the acceptance
table for TEST_REQUIRED.md T1 / T3 / T4 / T14 plus the standing regression fields.

    python tools\\ridelog.py [path\\to\\RE_Kenshi_log.txt]

Default: D:\\steam\\steamapps\\common\\Kenshi\\RE_Kenshi_log.txt

WARNING - what this tool is NOT.  It reports only what the log says.  Every
PASS below is a log-side fact; the eyeball half of each T item (does the rider
take damage, does his attack land, does the upper body return to a plain sit,
does the straddle look right) can ONLY be judged in game.  A field that is
missing entirely almost always means continuous diagnostics were never turned
on (Ctrl+NUM.) - it does NOT mean the value was zero.
"""

import math
import os
import re
import sys

DEFAULT_LOG = r"D:\steam\steamapps\common\Kenshi\RE_Kenshi_log.txt"

# Every diagnostic line is "Riding: <what> k=v k=v ...", so one generic scanner
# covers mounted / tuned / STANCE / TWIST / LEGPOSE / DBG.  Values may be
# negative, fractional, a tuple, or a quoted clip name.
KV = re.compile(r"([A-Za-z][A-Za-z0-9_]*)=('[^']*'|\([^)]*\)|[^\s]+)")

# P43RD prints sh='<pre>'->'<post>'.  KV's quoted-value branch matches the FIRST
# '...' and hands back only the pre-name, so the pair needs its own pattern.
RD_SH = re.compile(r"sh='([^']*)'->'([^']*)'")


def kv(line):
    return dict(KV.findall(line))


def ts(line):
    """The RE_Kenshi timestamp, i.e. the first token ("561.454").

    Frame counters cannot be cross-compared between line kinds (TWIST/LEGPOSE
    print gLegPoseFrames, STANCE prints the global frame), so the timestamp is
    the only way to line two kinds of line up against each other.
    """
    head = line.split(None, 1)[0] if line.strip() else ""
    return head if re.match(r"^\d+\.\d+$", head) else "?"


def tsnum(t):
    """ts() hands back the raw token ("561.454" or "?"); arithmetic needs a float.

    Returns None when the line had no usable timestamp, so callers can tell
    "cannot correlate" apart from "correlates to 0.000".
    """
    try:
        return float(t)
    except (TypeError, ValueError):
        return None


# Two P3CMB rows closer together than this belong to one knockdown window.  The
# probe is throttled, so consecutive samples of ONE knockdown are seconds apart.
kDownGap = 3.0


def add_down(s, line):
    """Fold this down=1 row's timestamp into s.down_spans."""
    t = ts(line)
    if t == "?":
        return
    t = float(t)
    if s.down_spans and 0.0 <= t - s.down_spans[-1][1] <= kDownGap:
        s.down_spans[-1][1] = t
    else:
        s.down_spans.append([t, t])


def in_down(s, t, pad=1.5):
    """Does timestamp t fall inside a knockdown window?

    pad widens the window by the sampling gap on each side: the spans are built
    from throttled samples, so the rider was already down a little before the
    first one and still down a little after the last.
    """
    if t is None or t == "?":
        return False
    try:
        t = float(t)
    except (TypeError, ValueError):
        return False
    for a, b in s.down_spans:
        if a - pad <= t <= b + pad:
            return True
    return False


def fnum(d, k, default=None):
    """Parse d[k] as float; tolerate the int-scaled fields and junk."""
    v = d.get(k)
    if v is None:
        return default
    try:
        return float(v)
    except ValueError:
        return default


def triple(d, k):
    """Parse a "(a,b,c)" field into three floats, e.g. T26's want=/bx=/sh=.

    Returns None unless all three parsed - a truncated tuple is dropped rather
    than padded, the same rule the rest of this file follows (a missing field
    must never look like a measured zero).
    """
    v = d.get(k)
    if not v or not v.startswith("(") or not v.endswith(")"):
        return None
    parts = v[1:-1].split(",")
    if len(parts) != 3:
        return None
    try:
        return tuple(float(p) for p in parts)
    except ValueError:
        return None


def tri_angle(a, b):
    """Angle in degrees between two (x,y,z) tuples, or None if either is degenerate.

    Used to turn adjacent want= samples into "how far the intended direction moved
    between these two log rows", which is what separates a read-lag deficit from a
    real aim error in -- T26 -- / -- T27 --.
    """
    if not a or not b:
        return None
    na = math.sqrt(sum(c * c for c in a))
    nb = math.sqrt(sum(c * c for c in b))
    if na < 1e-6 or nb < 1e-6:
        return None
    d = sum(x * y for x, y in zip(a, b)) / (na * nb)
    return math.degrees(math.acos(max(-1.0, min(1.0, d))))


def hostkeep(d, default=None):
    """The window's pin-site frame counter, under whichever key this build used.

    ⚠️ SAME COUNTER, OPPOSITE MEANING.  T20..T26 printed `guardoff=` = frames the
    guard assertion was WITHHELD so a pinned one-shot could own the torso.  T27
    stopped swapping the host at all, so the identical counter now means frames the
    window kept the GUARD as host, and it prints as `hostkeep=`.  The number is read
    the same way by every check that only asks "did the window reach the pin sites",
    which is all of them - but the KEY had to change so that nobody reads
    「guard 让开了 5126 帧」 off a build where the guard never let go.  Use
    hostkeep_key() when the answer is being printed back at a human.
    """
    v = d.get("hostkeep")
    if v is None:
        v = d.get("guardoff")
    if v is None:
        return default
    try:
        return float(v)
    except ValueError:
        return default


def hostkeep_key(d):
    """'hostkeep' / 'guardoff' / None - which of the two this row actually carries."""
    if "hostkeep" in d:
        return "hostkeep"
    if "guardoff" in d:
        return "guardoff"
    return None


def is_t27(s):
    """True when this log came from a build whose window keeps its host clip (T27+).

    Dispatch on the FIELD NAME, never on a byte count or a trip number: the close
    line prints hostkeep= only in T27 and later, and guardoff= only in T20..T26, so
    one key answers "which family of criteria applies" with no lookup table to keep
    in sync.  Mixed (a log spanning a redeploy) counts as T27 if any close row does -
    the newer criteria are the stricter statement about the newer build.
    """
    for d in s.sw_close:
        if "hostkeep" in d:
            return True
    return False


def fnum_flagged(d, k, default=None):
    """Parse a "<float>/<flag>" field, e.g. TWIST's sh=%.1f/%d.

    Returns (value, flag).  value is None unless the flag half is 1 - the
    source prints sh=0.0/0 when it could not read both UpperArm bones, and a
    zero with the flag clear is NOT a measurement.  bare float() on the whole
    token raises, which used to silently drop EVERY sample and report the
    sign criterion as "never exercised" (a tool bug, not a code failure).
    """
    v = d.get(k)
    if v is None:
        return (default, False)
    head, _, flag = v.partition("/")
    ok = (flag == "1")
    try:
        return (float(head) if ok else default, ok)
    except ValueError:
        return (default, ok)


class Stat(object):
    """min/max/count without keeping every sample."""

    def __init__(self):
        self.n = 0
        self.lo = None
        self.hi = None
        self.sum = 0.0

    def add(self, v):
        if v is None:
            return
        self.n += 1
        self.sum += v
        if self.lo is None or v < self.lo:
            self.lo = v
        if self.hi is None or v > self.hi:
            self.hi = v

    def __str__(self):
        if not self.n:
            return "n=0"
        return "n=%d min=%.3f avg=%.3f max=%.3f" % (
            self.n, self.lo, self.sum / self.n, self.hi)


class Session(object):
    def __init__(self):
        self.rides = []          # one dict per "mounted" line
        self.dismounts = 0
        # P4-4 (DLL 305664 B and later): "Riding: force dismount (<why>)", ungated,
        # one line per event.  why is one of "mount down" / "rider down" / "stale",
        # so this list is the only record of WHICH half ended a ride - the plain
        # "Riding: dismounted" that follows looks identical to a manual 「放倒」.
        self.forced = []         # (why, ts)
        self.restored = 0        # LEGPOSE restored on dismount
        self.takeovers = []      # LEGPOSE takeover lines (dicts)
        self.released = []       # LEGPOSE released lines (grace=, f=, ts)
        self.handback = []       # LEGPOSE handback audit lines (DLL 307712 B+)
        # Timestamp spans (merged, kDownGap tolerance) during which a P3CMB row
        # reported down=1.  2026-08-31: EVERY sub-1.0 kept sample and BOTH
        # grace-exhausted releases of that trip fell inside such a span, so this
        # correlation is the whole diagnosis of the straddle defect - the tool
        # now computes it instead of leaving it to hand-greps.
        self.down_spans = []     # [start_ts, end_ts]
        self.kept = Stat()
        self.kept_bad = 0        # samples not 1.0000
        self.kept_bad_rows = []  # (f, bone, kept, abd, flx, ts) for the bad ones
        # First/last timestamp a kept= sample was taken at.  Needed to tell a
        # real "no bad kept inside the knockdown window" from a VACUOUS one: the
        # kept row is budgeted, so on the 2026-08-31 fourth trip the sampling
        # stopped at 100.756 s while the knockdown happened at 156 s - an empty
        # set, not evidence.
        self.kept_t0 = None
        self.kept_t1 = None
        self.stance = []         # (state, f, cm, d, hold) in order
        self.twist = []          # (want, sh, d, on, msk, host, shok, f)
        self.tuned = []          # (species, key or None)
        # Input-chain diagnostics (DLL 301056 B and later).  The 2026-08-31 trip
        # reported "pressed the tune keys, the seat did not move" with a log that
        # said NOTHING: no 'input ... fired', no 'tuned', not even the tune gate's
        # own rejection line.  These four fields turn the next trip's answer into
        # a table instead of a hand-grep.
        self.rawkeys = []        # (kc, mask, ce, cmd) one per fresh press
        self.fired = []          # command names from "input '<name>' fired"
        self.ce = []             # controlEnabled transitions, in order
        self.rejects = {}        # rejection reason -> count
        self.bindings = None     # last "bindings resolved [...]" payload
        # P3CMB combat probe.  WARNING: this line's rAtk=/mAtk= are ATTACKER
        # COUNTS (getAllAttackers), nothing to do with the mount line's mAtk=
        # (the per-race attacks lektor).  Same spelling, unrelated meanings.
        self.cmb = 0
        self.cmb_ratk = 0        # rows where enemies are attacking the RIDER
        self.cmb_matk = 0        # rows where enemies are attacking the MOUNT
        self.cmb_rtgt = 0        # rider holds an attack target
        self.cmb_mtgt = 0        # mount holds an attack target (= 坐骑护主 fired)
        self.cmb_down = 0        # rider knocked down while mounted
        self.cmb_up = 0          # P3CMB rows that explicitly said down=0
        self.up_ts = []          # their timestamps (to test "did he stand up?")
        self.cmb_dr3 = Stat()    # nearest attacker to the rider's logical pos
        self.pose_w = Stat()
        self.pose_1 = 0          # weight >= 0.995
        self.pose_dip = 0        # 0.94 <= weight < 0.995  (the v1.5 residue)
        self.pose_low = 0        # weight < 0.94  (fade-in ramp or collapse)
        self.oth = Stat()
        self.act = Stat()
        self.act_over = 0        # act > 1.02
        self.wn_nonzero = 0      # ragdoll drag still moving the node
        self.mvw_nonzero = 0
        self.dbg = 0
        self.av = []             # access violation / rejection lines
        self.notes = []          # other one-off lines worth surfacing
        self.posedump = 0
        # P4-3 step 1: the sheatheWeapon naming probe (DLL 305152 B and later).
        # Per-site accounting, keyed by the "<module>+0x<rva>" the probe prints.
        # The verdict is NOT "how many lines" - it is "which site has real>0 and
        # repeats on a ~14-frame cadence"; a wih=0 call sheathed nothing and can
        # never be the writer we are hunting.
        self.sh_sites = {}       # site -> {"real":n,"noop":n,"gaps":[..],"first_ts":ts}
        self.sh_lines = 0
        self.sh_dump = None      # last "sites=.. lines=.. over=.." payload
        self.sh_hookfail = False # "Could not hook CharacterHuman::sheatheWeapon!"
        # P4-3 step 2: the attachItem naming probe (DLL 312832 B and later).  The
        # question is narrower than step 1's: is attachItem(..., "hands") called
        # AT ALL while the rider sits in the carried state?  Two hooks (the two
        # overloads are separate functions), tagged ov=2/3, and the per-ride dump
        # prints even with zero sites - so unlike P43SH, silence here has exactly
        # one reading as long as the dump line is present.
        self.at_sites = {}       # site -> {"ov":n,"hands":n,"other":n,"slots":set()}
        self.at_lines = 0
        self.at_hands = 0        # logged lines whose slot was "hands"
        # The count report_attach() actually judged on (dump counter if present,
        # else the logged-line count).  Stashed so report_p41d() can cross-check
        # its own weapon gate against it - see the "genuinely unarmed" branch.
        self.at_hands_best = 0
        self.at_dump = None      # last "sites=.. hands=.. over=.. hk=.. app=.." payload
        self.at_hookfail = []    # "Could not hook AppearanceBase::attachItem (N-arg)!"
        # P4-3 step 3, naming half: the P41D combat lever.  Its ONE question here is
        # "does chooseAttack hand back a technique, and what is that clip called" -
        # everything else on those lines is the ladder's own bookkeeping.
        # WARNING for whoever extends this: the "P41D read" line carries DUPLICATE
        # keys - cma=/tech=/cst= appear in both the `raw` and the `e` (enemy
        # positive control) sections, and reach= in both `api` and `e`.  kv() builds
        # a dict, so a naive kv(line) silently returns the ENEMY's values for those
        # four.  Everything below splits the line at " | e " first.
        self.p41d_ai = 0         # "P41D ai" lines (the P41B-era AI-layer view)
        self.p41d_read = []      # dicts of the rider half of each "P41D read" line
        self.p41d_precond = []   # "P41D precond tgt= atk= icm=" lines
        self.p41d_rungs = {}     # rung -> count, from "P41D rung=" readbacks
        self.p41d_rung_rows = [] # dicts of each "P41D rung=" line (rider half)
        self.p41d_clip_absent = []    # names reported ABSENT in allAnims
        self.p41d_clip_unres = []     # names that could not even be looked up
        self.p41d_clip_rows = []      # LogAnimRow rows that SNAPSHOTTED (lay=/flags= readable)
        # LogAnimRow's OTHER shape: "key='..' ptr=%p UNREADABLE".  It is NOT a
        # resolved record and must never be filed with the rows above: the call
        # site only reaches LogAnimRow when allAnims.find() HIT, so ptr=0 means
        # the map holds a key with a NULL value = a poisoned entry (engine-side
        # getAnimationData() = operator[]).  That is a finding, not a pass.
        self.p41d_clip_null = []      # verbatim "... ptr=.. UNREADABLE" rows
        self.p41d_abandoned = False   # "all rungs disarmed - ladder abandoned"
        # P4-3-2, the sheathe SUPPRESSOR (DLL 302080 B and later).  Same hook
        # address as the step-1 probe, opposite job: it declines the engine body
        # instead of describing the caller.  There is no site= field on purpose
        # (RUNTIME_RVA_DELTA must never enter shipping code), so the accounting is
        # per RIDE, not per site: real= is the only field that proves the fix bore
        # load, exactly like late= for the by-name mask rescue (RE_NOTES 21).
        self.sup_rides = []      # dicts of "P43SUP ride real=.. noop=.. pass=.."
        self.sup_skips = []      # dicts of the budgeted "P43SUP skip" lines
        # T18's instrument (DLL 312832 B / md5 E83DB50D... and later, UNGATED):
        # one "P43FT ride ok=.. noElig=.. noFight=.. noThr=.. far=.. dmin=.." per
        # ride, saying WHICH of RideStanceRaw's three terms refused.  It exists
        # because STANCE is diagnostics-gated, so a diag-OFF trip that fails would
        # otherwise be undiagnosable.  Absent in every log before T18.
        self.ft_rides = []
        # P4-1e's forced-draw ladder, read here as the SECOND self-proving field
        # for the suppressor: with the re-sheathe gone the budget stops being
        # spent, so n= should stall at 1-2 instead of climbing to kDrawTryBudget
        # (12) one rung per kDrawTryGap (10) frames.  Diagnostics-gated.
        self.draw_n = []         # (n, left, wih_pre, wih_post, ts) per P41E draw
        # P4-3-3, the stance-edge RE-DRAW (DLL 301568 B and later, UNGATED - it
        # changes game state).  One line per 0 -> 1 stance edge that actually
        # issued a drawWeapon; post= is the self-proving field, exactly like
        # real= for the suppressor and late= for the mask rescue (RE_NOTES 21).
        self.rd = []             # dicts of "P43RD edge ..." lines
        # P4-3-4, THE SWING (DLL 308224 B and later, UNGATED - it changes game
        # state, same discipline as P43RD).  ⚠️ The prefix P43SW is REUSED: trip
        # 10's deleted experiment window logged under it too (kP43Sw*, archived in
        # HISTORY §U).  The two are told apart by shape, not by name - the old one
        # printed "P43SW <open|hold|close|after> n= blow play= ... | guard play= ..."
        # with the two clips repeating field names on one line, this one prints
        # "P43SW open n= tech='..' init= minS= lim= d= reach=" plus a "P43SW ride"
        # summary the old one never had.  A log that has "P43SW ride" is this one.
        self.sw_rides = []       # "P43SW ride swing= tech= skip= fail= guardoff="
        self.sw_open  = []       # "P43SW open n= tech='..' init= minS= lim= d= reach="
        self.sw_close = []       # "P43SW close n= guardoff= tech= skip= fail="
        self.sw_legacy = 0       # lines that look like the trip-10 window instead
        self.sw_hold_bones = []  # T23 "SWING hold bone '<name>' has= handle="
        self.sw_free_bones = []  # T23 "SWING free bone '<name>' has= handle="
        self.sw_arm     = []     # T25 "SWING arm ... abd= flx= elb= kept= out= fore= down="
                                 # T26 "SWING arm ... kept= out= fore= down= want= dot= len= bx="
        self.sw_armback = []     # T25/T26 "SWING armback man= minDot= seen= lastt="
        # "P41K resolve guard='guard 1h' found blow='mid blow' found" - printed once
        # per DLL load on the stance's first frame and UNGATED (verified present in
        # the trip-24 diagnostics-OFF log).  It used to be background information;
        # T27 makes the guard half load-bearing, because the guard is now the body's
        # host for the whole window, so guard=ABSENT means nothing in -- T27 -- can
        # be judged at all.  Two strings, not a kv() dict: both values are "found" or
        # "ABSENT" under repeated key-less positions.
        self.k_guard = None      # "found" / "ABSENT" / None (line never printed)
        self.k_blow  = None      # ditto for 'mid blow', which T27 only RESOLVES

        # P4-3 step 2's THIRD probe (DLL 309760 B and later): who takes the hand
        # slot away AFTER attachItem put the weapon there.  Trip 11 closed the
        # data-layer half (post=1 4/4, wih=0->1, wpn=1 on 52/56 reads) while the
        # screen stayed empty, so this probe is the only thing that can name the
        # remover - or prove there isn't one, which is why BOTH halves below print
        # unconditionally at dismount.  Naming only (TASK.md P4-3 step 2 gate).
        self.dt_hookfail = []    # "Could not hook AppearanceBase::detachItem(slot)!"
        self.dt_calls = []       # dicts of the budgeted per-call "P43DT first/rep" rows
        self.dt_rides = []       # dicts of "P43DT ride sites=.. hands=.. over=.. hk=.. app=.."
        self.dt_sites = {}       # site -> {"hands":n,"other":n,"slot0":str} from the ride rows
        # P43HD: the hand-slot poll.  Its whole job is to make P43DT's SILENCE
        # readable - an entity still sitting in "hands" with no detach line means
        # the removal never happened.  ⚠️ It does NOT mean "the miss is
        # render-side": that was the trip-12 reading and trip 13 killed it by
        # fixing the same symptom with no render-side change at all (the real
        # gate was drawWeapon's 2nd argument, RE_NOTES §18.12 / §18.12.1).
        # loss= is still the money edge and has its own DLL-side budget.
        self.hd_edges = []       # dicts of the per-transition lines (kind in "_kind")
        self.hd_rides = []       # dicts of "P43HD ride samples=.. loss=.. .."
        self.hd_appswap = []     # verbatim "P43HD appswap was=.. now=.." lines


MOUNTED = re.compile(r"Riding: mounted \[(?P<sp>[^\]]*)\]")
TUNED = re.compile(r"Riding: tuned (?P<sp>\S+)")


def tuple_nonzero(v, eps=0.005):
    """True if a "(a,b,c)" field holds anything but zeros."""
    if not v or not v.startswith("("):
        return False
    try:
        parts = [float(x) for x in v.strip("()").split(",")]
    except ValueError:
        return False
    return any(abs(p) > eps for p in parts)


def parse(path, s):
    lines = 0
    with open(path, "rb") as fh:
        for raw in fh:
            lines += 1
            # The log is UTF-8 but species names have been seen mangled; never
            # let one bad byte abort the whole read.
            line = raw.decode("utf-8", "replace").rstrip("\r\n")
            # This one is an ErrorLog, so it says "RidingPlugin:" and would be
            # dropped by the "Riding:" filter below.  It has to be caught: "no
            # P43SH line at all" means two completely different things depending
            # on whether the hook installed (see report_sheathe).
            if "Could not hook CharacterHuman::sheatheWeapon" in line:
                s.sh_hookfail = True
                continue
            # Same trap, same reason: these are ErrorLogs ("RidingPlugin:"), and
            # "hands=0" only means "never called" if the hooks actually went on.
            if "Could not hook AppearanceBase::attachItem" in line:
                s.at_hookfail.append(line.strip())
                continue
            # Third trap, same reason.  This probe's answer may legitimately BE
            # silence, so a failed install has to be distinguishable from it.
            if "Could not hook AppearanceBase::detachItem" in line:
                s.dt_hookfail.append(line.strip())
                continue
            if "Riding:" not in line:
                continue
            if "access violation" in line or "AV" == line[-2:]:
                s.av.append(line.strip())
                continue

            m = MOUNTED.search(line)
            if m:
                d = kv(line)
                d["species"] = m.group("sp")
                s.rides.append(d)
                continue
            if "Riding: dismounted" in line:
                s.dismounts += 1
                continue
            if "Riding: force dismount (" in line:
                why = line.split("force dismount (", 1)[1].split(")", 1)[0]
                s.forced.append((why, ts(line)))
                continue
            if "LEGPOSE handback" in line:
                d = kv(line)
                d["_ts"] = ts(line)
                s.handback.append(d)
                continue
            if "LEGPOSE restored on dismount" in line:
                s.restored += 1
                continue
            if "LEGPOSE takeover" in line:
                d = kv(line)
                d["_ts"] = ts(line)
                s.takeovers.append(d)
                continue
            if "LEGPOSE released" in line:
                d = kv(line)
                d["_ts"] = ts(line)
                # Two completely different events share this line.  "(rider down)"
                # is the isDown()/isDead() guard of DLL 301056 B collecting its
                # own bones on purpose - it is the FIX firing, not a fault.  A
                # grace= release is the host-lost branch giving up after 12
                # frames, which is the one worth chasing.  Never judge them as
                # one bucket.
                d["_why"] = "down" if "(rider down)" in line else "grace"
                s.released.append(d)
                continue
            # ⚠️ SHAPE-BASED, so it needs an exclusion list.  This branch owns the
            # STRADDLE's mask audit (LEGPOSE's kept=), and T25's "SWING arm" line
            # carries a kept= of its own with the same meaning but a different
            # subject - the arm, not the thighs.  Letting it in here counted a
            # first-frame kept=-1.0000 as a straddle mask failure and hid the arm
            # samples from -- T25 -- at the same time (the same class of trap as
            # HISTORY §U's two-clips-one-line).
            if "kept=" in line and "SWING arm" not in line:
                d = kv(line)
                k = fnum(d, "kept")
                s.kept.add(k)
                kt = ts(line)
                if kt != "?":
                    kt = float(kt)
                    if s.kept_t0 is None:
                        s.kept_t0 = kt
                    s.kept_t1 = kt
                if k is not None and abs(k - 1.0) > 0.0005:
                    s.kept_bad += 1
                    # Keep the identity of the bad ones.  Without f= and the bone
                    # name these samples cannot be lined up against the LEGPOSE
                    # released / STANCE lines, and that correlation is the whole
                    # diagnosis when the mask misses somebody.
                    m = re.search(r"'([^']*)'", line)
                    if len(s.kept_bad_rows) < 24:
                        s.kept_bad_rows.append(
                            (fnum(d, "f"), m.group(1) if m else "?", k,
                             d.get("abd"), d.get("flx"), ts(line)))
                continue

            if "Riding: STANCE" in line:
                d = kv(line)
                st = line.split("STANCE", 1)[1].strip().split()[0]
                s.stance.append((st, d.get("f", "?"), d.get("cm", "?"),
                                 fnum(d, "d"),
                                 # holdms= is wall-clock ms (DLL 301056 B and later);
                                 # hold= was frames up to and including 297472 B.  Echoed
                                 # either way - the tool never compares it to a threshold,
                                 # and the acceptance test is "1 -> 0 was observed", never
                                 # a number of seconds.
                                 d.get("holdms", d.get("hold", "?")),
                                 "ms" if "holdms" in d else "f",
                                 # [6] wall-clock timestamp.  STANCE f= counts the
                                 # global frame while other lines count
                                 # gLegPoseFrames, so correlating a stance edge with
                                 # anything else (T13-A's draw ladder) has to go
                                 # through the timestamp.
                                 ts(line)))
                continue
            if "Riding: TWIST" in line:
                d = kv(line)
                # sh is "<deg>/<haveSh>" - split it, and keep the flag so a
                # bone-read failure never masquerades as a 0-degree shoulder.
                sh, shok = fnum_flagged(d, "sh")
                s.twist.append((fnum(d, "want"), sh, fnum(d, "d"),
                                d.get("on"), d.get("msk"), d.get("host"), shok,
                                fnum(d, "f")))
                continue
            if "Riding: P3CMB" in line:
                if "access violation" in line:
                    s.av.append(line.strip())
                    continue
                d = kv(line)
                s.cmb += 1
                if fnum(d, "rAtk", 0) > 0:
                    s.cmb_ratk += 1
                if fnum(d, "mAtk", 0) > 0:
                    s.cmb_matk += 1
                if d.get("rTgt") == "1":
                    s.cmb_rtgt += 1
                if d.get("mTgt") == "1":
                    s.cmb_mtgt += 1
                if d.get("down") == "1":
                    s.cmb_down += 1
                    add_down(s, line)
                elif d.get("down") == "0":
                    # Counted so the merged-window printer can tell "the rider
                    # stood up" from "the probe simply did not log for a while".
                    s.cmb_up += 1
                    t = ts(line)
                    if t != "?":           # ts() returns "?", never None
                        s.up_ts.append(float(t))
                dr = fnum(d, "dR3")
                if dr is not None and dr >= 0.0:
                    s.cmb_dr3.add(dr)
                continue
            if "Riding: RAWKEY" in line:
                d = kv(line)
                s.rawkeys.append((d.get("kc", "?"), d.get("mask", "?"),
                                  d.get("ce", "?"), d.get("cmd", "-")))
                continue
            if "Riding: input '" in line:
                s.fired.append(line.split("input '", 1)[1].split("'", 1)[0])
                continue
            if "Riding: controlEnabled ->" in line:
                s.ce.append(line.rsplit(">", 1)[1].strip())
                continue
            if "Riding: seat-tuning key ignored" in line:
                why = line.split("ignored", 1)[1].strip(" -")
                s.rejects[why] = s.rejects.get(why, 0) + 1
                continue
            if "Riding: bindings resolved" in line:
                if "[" in line and "]" in line:
                    s.bindings = line.split("[", 1)[1].rsplit("]", 1)[0].strip()
                continue
            m = TUNED.match(line.strip()) or TUNED.search(line)
            if m:
                d = kv(line)
                # up=/fwd= are the STORED (reference-frame) values x100, live= is
                # what the live animal actually gets.  Keeping them lets the report
                # answer the question the player's X-4 report really asked: not
                # "did a key fire" but "did the seat MOVE".  A key that fires while
                # the value is pinned at SeatTuneLimitRef looks identical in the
                # log line count and completely different on screen.
                s.tuned.append((m.group("sp"), d.get("key"),
                                d.get("up"), d.get("fwd"), d.get("live")))
                continue
            if "Riding: DBG" in line:
                s.dbg += 1
                d = kv(line)
                pose = d.get("pose")
                if pose and "/" in pose:
                    try:
                        w = float(pose.split("/")[1])
                    except ValueError:
                        w = None
                    if w is not None:
                        s.pose_w.add(w)
                        if w >= 0.995:
                            s.pose_1 += 1
                        elif w >= 0.94:
                            s.pose_dip += 1
                        else:
                            s.pose_low += 1
                s.oth.add(fnum(d, "oth"))
                a = fnum(d, "act")
                s.act.add(a)
                if a is not None and a > 1.02:
                    s.act_over += 1
                if tuple_nonzero(d.get("wn")):
                    s.wn_nonzero += 1
                if tuple_nonzero(d.get("mvW")):
                    s.mvw_nonzero += 1
                continue
            if "P43SUP" in line:
                d = kv(line)
                d["_ts"] = ts(line)
                # "ride" is the ungated per-ride summary, "skip" a budgeted sample
                # of the individual suppressed calls.  Never add them together.
                if "P43SUP ride" in line:
                    s.sup_rides.append(d)
                else:
                    s.sup_skips.append(d)
                continue
            if "Riding: P43SW " in line:
                d = kv(line)
                d["_ts"] = ts(line)
                # ⚠️ SHAPE, not keyword: trip 10's deleted window also printed
                # "P43SW open" and "P43SW close".  Its lines carry two clips'
                # fields separated by " | guard " and have no tech= / guardoff=;
                # kv() would silently hand back the guard's numbers for the blow's
                # (HISTORY §U records exactly this pitfall).  So a row only counts
                # as P4-3-4 when it carries a field the old one never had.
                # ⚠️ T27 RENAMED that field: the close line now prints hostkeep=
                # (frames the window kept the GUARD as host) where T20..T26 printed
                # guardoff= (frames the guard was WITHHELD).  Same counter, opposite
                # meaning - which is exactly why the key changed, and why BOTH have to
                # be accepted here or a T27 log falls into sw_legacy and every swing
                # section goes blind at once.
                if "P43SW ride" in line:
                    s.sw_rides.append(d)
                elif "P43SW open" in line and "tech" in d:
                    s.sw_open.append(d)
                elif "P43SW close" in line and ("guardoff" in d or "hostkeep" in d):
                    s.sw_close.append(d)
                else:
                    s.sw_legacy += 1
                continue
            # T23: the two bone tables of the complementary split, printed once per
            # DLL load.  "SWING hold bone '<name>' has=N handle=N" protects the seat
            # on the TECHNIQUE's state; "SWING free bone ..." releases the host's
            # upper body while a window is open.  A has=0 here disables half the
            # split silently, which is why the line exists at all.
            if "Riding: SWING hold bone " in line or "Riding: SWING free bone " in line:
                d = kv(line)
                d["_ts"] = ts(line)
                m = re.search(r"bone '([^']*)'", line)
                d["_bone"] = m.group(1) if m else "?"
                if "hold bone" in line:
                    s.sw_hold_bones.append(d)
                else:
                    s.sw_free_bones.append(d)
                continue
            # T25/T26: the AUTHORED arm.  One sample of the arc (budgeted, one per
            # kRideSwingArmLogGap authored frames).  T25 printed the joint angles
            # (abd=/flx=/elb=); T26 prints what it MEANT instead - want= is the intended
            # shoulder->hand vector and dot= is it against the measured one, which is the
            # only field that can tell a landed write apart from a CORRECT one.
            # "SWING armback man= minDot= seen= lastt=" is the custody handback, the same
            # proof LEGPOSE's handback line carries.  ⚠️ Both are ungated - they are the only
            # evidence that a hand-written pose reached the skeleton at all.
            if "Riding: SWING arm " in line:
                d = kv(line)
                d["_ts"] = ts(line)
                s.sw_arm.append(d)
                continue
            if "Riding: SWING armback " in line:
                d = kv(line)
                d["_ts"] = ts(line)
                s.sw_armback.append(d)
                continue
            # The two clip names the stance resolves once per DLL load.  Pulled with a
            # regex rather than kv(): the "found"/"ABSENT" verdicts are positional, not
            # key=value.  On T27 the guard half is the host guarantee (-- T27 -- section
            # 1), and the blow half is only kept as standing evidence that the rider's
            # own table really does hold a swing record - nothing requests it any more.
            if "Riding: P41K resolve " in line:
                m = re.search(r"guard='[^']*' (found|ABSENT) blow='[^']*' (found|ABSENT)",
                              line)
                if m:
                    s.k_guard, s.k_blow = m.group(1), m.group(2)
                continue

            if "Riding: P43FT ride" in line:
                d = kv(line)
                d["_ts"] = ts(line)
                s.ft_rides.append(d)
                continue
            if "Riding: P43RD " in line:
                # sh='<pre>'->'<post>' holds two quoted names with spaces in
                # them, and KV stops at the first closing quote - so the pair has
                # to be pulled out with its own regex before kv() is trusted.
                d = kv(line)
                d["_ts"] = ts(line)
                m = RD_SH.search(line)
                d["_shpre"], d["_shpost"] = m.groups() if m else ("?", "?")
                s.rd.append(d)
                continue
            if "P41E draw n=" in line:
                d = kv(line)
                pre, _, post = (d.get("wih") or "").partition("->")
                s.draw_n.append((fnum(d, "n"), fnum(d, "left"), pre, post, ts(line)))
                continue
            if "P43SH" in line:
                if "sites=" in line:
                    s.sh_dump = line.strip()
                    continue
                d = kv(line)
                site = d.get("site")
                if not site:
                    continue
                s.sh_lines += 1
                e = s.sh_sites.setdefault(site, {"real": 0, "noop": 0,
                                                 "gaps": [], "first_ts": ts(line)})
                # wih= is the PRE-call state (the probe runs before the engine
                # body), so wih=1 is the only kind of call that actually took a
                # drawn weapon out of the rider's hands.
                if d.get("wih") == "1":
                    e["real"] += 1
                else:
                    e["noop"] += 1
                try:
                    g = int(d.get("gap", "0"))
                except ValueError:
                    g = 0
                if g > 0:
                    e["gaps"].append(g)
                continue
            if "P43AT" in line:
                if "sites=" in line:
                    s.at_dump = line.strip()
                    continue
                d = kv(line)
                site = d.get("site")
                if not site:
                    continue
                s.at_lines += 1
                try:
                    ov = int(d.get("ov", "0"))
                except ValueError:
                    ov = 0
                e = s.at_sites.setdefault(site, {"ov": ov, "hands": 0, "other": 0,
                                                "slots": set()})
                slot = (d.get("slot") or "").strip("'")
                if slot:
                    e["slots"].add(slot)
                # hands= is the probe's own comparison, so trust it over re-parsing
                # the quoted slot name here.
                if d.get("hands") == "1":
                    e["hands"] += 1
                    s.at_hands += 1
                else:
                    e["other"] += 1
                continue
            if "P43DT" in line:
                if "P43DT ride" in line:
                    d = kv(line)
                    d["_ts"] = ts(line)
                    s.dt_rides.append(d)
                    continue
                m = re.search(r"P43DT \| (\S+) hands=(\d+) other=(\d+)"
                              r" slot0='([^']*)'", line)
                if m:
                    # The per-ride table, which is the authority: the per-call rows
                    # above it are budgeted and under-count on purpose.
                    e = s.dt_sites.setdefault(m.group(1), {"hands": 0, "other": 0,
                                                           "slot0": m.group(4)})
                    e["hands"] += int(m.group(2))
                    e["other"] += int(m.group(3))
                    continue
                d = kv(line)
                if d.get("site"):
                    d["_ts"] = ts(line)
                    d["_first"] = " P43DT first " in line
                    s.dt_calls.append(d)
                continue
            if "P43HD" in line:
                if "P43HD ride" in line:
                    d = kv(line)
                    d["_ts"] = ts(line)
                    s.hd_rides.append(d)
                    continue
                if "P43HD appswap was=" in line:
                    s.hd_appswap.append(line.strip())
                    continue
                m = re.search(r"P43HD (\w+)", line)
                if m:
                    d = kv(line)
                    d["_ts"] = ts(line)
                    d["_kind"] = m.group(1)
                    s.hd_edges.append(d)
                continue
            if "P41D" in line:
                # Order matters: the three "P41D access violation .." lines never
                # arrive here - the generic "access violation" catch above already
                # put them in s.av - so report_p41d() scans s.av for a disarmed rung.
                if "all rungs disarmed" in line:
                    s.p41d_abandoned = True
                    continue
                if "P41D clip" in line:
                    m = re.search(r"P41D clip '([^']*)' (ABSENT|unresolved)", line)
                    if m:
                        if m.group(2) == "ABSENT":
                            s.p41d_clip_absent.append(m.group(1))
                        else:
                            s.p41d_clip_unres.append(m.group(1))
                    elif "UNREADABLE" in line:
                        # LogAnimRow's failure shape, "key='..' ptr=%p UNREADABLE".
                        # Keep it OUT of p41d_clip_rows: it is the opposite of a
                        # resolved record (see the field comment).
                        s.p41d_clip_null.append(line.strip())
                    else:
                        # LogAnimRow's own shape: key='..' data='..' clip='..' lay=..
                        s.p41d_clip_rows.append(line.strip())
                    continue
                if "P41D precond" in line:
                    s.p41d_precond.append(kv(line))
                    continue
                if "P41D ai" in line:
                    s.p41d_ai += 1
                    continue
                # Both remaining shapes duplicate keys across their sections, so cut
                # the enemy control half off before kv() can overwrite the rider's.
                head = line.split(" | e ")[0]
                if "P41D read" in line:
                    d = kv(head)
                    d["_ts"] = ts(line)
                    d["_anim"] = (d.get("anim") or "").strip("'")
                    s.p41d_read.append(d)
                    continue
                if "P41D rung=" in line:
                    d = kv(head)
                    d["_ts"] = ts(line)
                    s.p41d_rung_rows.append(d)
                    try:
                        r = int(d.get("rung", "-1"))
                    except ValueError:
                        r = -1
                    s.p41d_rungs[r] = s.p41d_rungs.get(r, 0) + 1
                continue
            if "POSEDUMP" in line:
                s.posedump += 1
                continue
            if ("rejected" in line or "ignored" in line
                    or "unavailable" in line or "mask table full" in line):
                s.notes.append(line.strip())
    return lines


def verdict(ok, text):
    return ("PASS  " if ok else "CHECK ") + text


def report_rides(s):
    print("")
    print("== T4 - size gate, one row per mount (all x10 as logged) ==")
    if not s.rides:
        print("  NO 'mounted' LINE AT ALL - the plugin never mounted anybody,")
        print("  or this log predates the diagnostic build.")
        return
    print("  %-16s %-26s %5s %5s %5s %4s %5s %5s %6s" % (
        "species", "key/race", "torso", "rad", "size", "eli", "mAtk", "k", "h"))
    for d in s.rides:
        key = d.get("key") or d.get("race", "?")
        print("  %-16s %-26s %5s %5s %5s %4s %5s %5s %6s" % (
            d.get("species", "?")[:16], key[:26],
            d.get("torso", "-"), d.get("rad", "-"), d.get("size", "-"),
            d.get("elig", "-"), d.get("mAtk", "-"), d.get("k", "-"),
            d.get("h", "-")))
    print("  mounts=%d dismounts=%d" % (len(s.rides), s.dismounts))
    # mAtk: absent means the log predates the P4-2 build (the field was added
    # there); -1 means unreadable (three guards).  0 means the per-race
    # AnimList::attacks lektor is EMPTY - which offline data says it is for
    # every species, so 0 says nothing about whether the animal can swing.
    # See the NOTE below before acting on it.
    gone = [d for d in s.rides if "mAtk" not in d]
    bad = [d for d in s.rides if d.get("mAtk") == "-1"]
    zero = [d for d in s.rides if d.get("mAtk") == "0"]
    if gone:
        print("  CHECK %d of %d mount line(s) carry NO mAtk= at all = this log"
              % (len(gone), len(s.rides)))
        print("        predates the P4-2 build that added the field."
              "  T4 cannot be")
        print("        judged from it - re-run on a log from the 297472 B DLL.")
    else:
        print("  " + verdict(not bad, "mAtk readable on every mount"
                             + (" - %d unreadable(-1)" % len(bad) if bad else "")))
    if zero:
        print("  NOTE  %d mount(s) report mAtk=0.  That does NOT mean the species"
              % len(zero))
        print("        cannot swing - AnimList::attacks is empty for EVERYONE."
              "  Evidence:")
        print("        gamedata.py --type 5 finds no attack among any ANIMAL ANIM"
              " record,")
        print("        P1 read attacks=0 off the HUMAN list (humans obviously"
              " swing), and")
        print("        animal attacks actually live in COMBAT TECHNIQUE records"
              " ('gar")
        print("        attack long', 'insect spider attack', ...) whose clips have"
              " no")
        print("        ANIMATION record at all (RE_NOTES 19).  So mAtk= reads a"
              " container")
        print("        nobody populates.  DO NOT write a per-species fallback off"
              " it -")
        print("        check P3CMB mTgt= instead: mTgt=1 means the mount did take"
              " the")
        print("        attack order.")

    elig = [(d.get("species", "?"), d.get("elig"), d.get("size")) for d in s.rides]
    print("  elig by mount: " + ", ".join("%s=%s(size %s)" % e for e in elig))
    print("  (predicted: small tier elig=1, big tier elig=0; the four big crabs"
          " must be")
    print("   held out by rad=, not torso= - a rad of 0 there means the read"
          " failed.)")


def report_forced(s):
    """P4-4: 「骑手被击倒就下马」 - did the rider-down branch actually end the ride?

    The plain "Riding: dismounted" that follows a forced dismount is
    indistinguishable from a manual 「放倒」, so the reason word on the force
    dismount line is the only record of WHICH half fired.  Judged here:
      - "rider down" events at all (this is the P4-4 branch receiving load),
      - each one should sit inside a P3CMB down=1 window (it is the same
        predicate, so a rider-down dismount OUTSIDE every window means the two
        readings of "down" disagree - go look at the probe throttle first),
      - the window it sits in must END there (~one sampling gap).  P3CMB only
        prints while mounted, so down=1 rows continuing well past the event
        mean the ride did NOT actually end,
      - a "LEGPOSE released (rider down)" at ~the same timestamp: both fire on
        the same frame off the same predicate.  Absent is only a finding if the
        legs were armed at all.
    ⚠️ The rider's own landing (does he end up on the ground, not stuck or
    floating?) is NOT in the log - getDropped() on an already-KO'd character is
    the unverified half, see TEST_REQUIRED.md T7.
    """
    print("")
    print("== P4-4 - forced dismount (rider down / mount down) ==")
    if not s.forced:
        print("  no 'force dismount' line.")
        print("  Two readings: this log predates DLL 305664 B (the P4-4 build),")
        print("  or nothing forced a dismount this trip (no KO'd rider, no KO'd")
        print("  mount).  down=1 rows below tell them apart: %d P3CMB row(s) said"
              % s.cmb_down)
        print("  the rider was down.")
        if s.cmb_down:
            print("  " + verdict(False, "rider WAS down (%d row(s)) yet nothing forced"
                                 " a dismount" % s.cmb_down))
            print("        => either an older DLL, or the new branch never ran.")
        return

    by_why = {}
    for why, t in s.forced:
        by_why[why] = by_why.get(why, 0) + 1
    print("  " + "  ".join("%s=%d" % kv2 for kv2 in sorted(by_why.items())))
    for why, t in s.forced:
        print("    %-10s @%s" % (why, t))
    if by_why.get("stale"):
        print("  " + verdict(False, "%d 'stale' dismount(s) = a NULL rider or mount"
                             " sat in riderToMount" % by_why["stale"]))
        print("        That is the dangling-pointer path, not P4-4.  Look for a"
              " load")
        print("        during the ride (CharacterLooksLive / the mainLoop"
              " sentinel).")

    rider = [t for why, t in s.forced if why == "rider down"]
    if not rider:
        print("  NOTE  no 'rider down' event: the P4-4 branch was not exercised"
              " this trip.")
        print("        (%d 'mount down' event(s) = the ORIGINAL rule, unchanged.)"
              % by_why.get("mount down", 0))
        return

    for t in rider:
        if t == "?":
            print("  CHECK  a 'rider down' line carries no timestamp - cannot be"
                  " correlated.")
            continue
        tv = float(t)
        span = None
        for a, b in s.down_spans:
            if a - kDownGap <= tv <= b + kDownGap:
                span = (a, b)
                break
        if span is None:
            print("  " + verdict(False, "rider down @%s falls in NO P3CMB down=1"
                                 " window" % t))
            print("        The pass and the probe read the same isDown(), so this"
                  " means the")
            print("        probe never sampled that knockdown (it is throttled) -"
                  " check its")
            print("        gap before doubting the branch.")
        else:
            tail = span[1] - tv
            print("  " + verdict(tail <= kDownGap,
                                 "rider down @%s ends its down window (%.3f-%.3f,"
                                 " last down=1 row %+.3f s)" % (t, span[0], span[1], tail)))
            if tail > kDownGap:
                print("        down=1 rows kept coming %.1f s AFTER the forced"
                      " dismount, and" % tail)
                print("        P3CMB only prints while mounted => the ride did not"
                      " end.  Look at")
                print("        Dismount()'s early returns and at whether"
                      " riderToMount lost the row.")
        near = [d for d in s.released
                if d.get("_why") == "down" and d.get("_ts") != "?"
                and abs(float(d["_ts"]) - tv) <= 1.0]
        if near:
            print("        + LEGPOSE released (rider down) @%s = the leg guard"
                  " collected its" % near[0]["_ts"])
            print("          own bones the same frame, as designed.")
        elif s.takeovers:
            print("        NOTE no 'LEGPOSE released (rider down)' within 1.0 s."
                  "  The legs")
            print("          WERE armed this trip (%d takeover(s)), so either the"
                  " guard did not" % len(s.takeovers))
            print("          fire or Dismount()'s unconditional restore beat it"
                  " to the bones -")
            print("          check for 'LEGPOSE restored on dismount' at the same"
                  " timestamp.")
        else:
            print("        (no LEGPOSE takeover this trip at all => nothing for"
                  " the guard to")
            print("         release; not a finding.)")
    print("  NOTE  The landing itself is not in the log: whether a KO'd rider ends"
          " up on")
    print("        the ground (and not stuck or floating) can only be seen in"
          " game -")
    print("        TEST_REQUIRED.md T7.")


def report_stance(s):
    print("")
    print("== T1 - combat stance enters AND leaves ==")
    if not s.stance:
        print("  no STANCE line - either no fight happened or diagnostics were off.")
        return
    trans = []
    prev = None
    for st, f, cm, d, hold, unit, _t in s.stance:
        if st != prev:
            trans.append((prev, st, f, cm, d, hold, unit))
            prev = st
    for a, b, f, cm, dd, hold, unit in trans:
        print("  %s -> %s  at f=%s  cm=%s d=%s %s=%s" % (
            a if a is not None else "start", b, f, cm,
            "-" if dd is None else "%.1f" % dd,
            "holdms" if unit == "ms" else "hold(frames)", hold))
    units = set(t[6] for t in trans)
    if units == set(["f"]):
        print("        (hold= is FRAMES here => this log predates DLL 301056 B;"
              " the tail")
        print("         was 150 frames = 150/fps seconds, so ~5 s at 30 fps.)")
    down = [t for t in trans if t[0] == "1" and t[1] == "0"]
    up = [t for t in trans if t[1] == "1"]
    print("  STANCE lines=%d  entries(->1)=%d  exits(1->0)=%d"
          % (len(s.stance), len(up), len(down)))
    print("  " + verdict(bool(down),
                         "the stance came back down at least once"
                         " (this is THE T1 criterion - never the seconds it took)"))
    if any(t[4] is not None and t[4] < 0.0 for t in down):
        print("  NOTE  an exit line shows cm=1 d=-1.0 = the engine's combat flag"
              " is still")
        print("        set while our distance term retired.  That is the"
              " EXPECTED shape,")
        print("        not a fault: isInCombatMode never drops while mounted.")
    if up and not down:

        print("  FIRST THING TO LOOK AT: the d= on the stuck line above.")
        print("    small d  -> RideNearestThreat is counting a body"
              " (isDown/isDead miss)")
        print("    large d  -> the hold budget is not being drained (only"
              " HaltAndForceSitPass")
        print("                passes advance=true; since 301056 B it drains by"
              " GetTickCount")
        print("                deltas, and a paused game deliberately drains"
              " nothing)")


def report_combat(s):
    print("")
    print("== T4 - who actually engages (P3CMB probe) ==")
    if not s.cmb:
        print("  no P3CMB line - the probe is budgeted and gated on Ctrl+NUM.,"
              " so this")
        print("  is unmeasured, not clean.")
        return
    print("  rows=%d   enemies attacking rider(rAtk>0)=%d   attacking mount"
          "(mAtk>0)=%d" % (s.cmb, s.cmb_ratk, s.cmb_matk))
    print("  rider holds a target(rTgt=1)=%d   mount holds a target(mTgt=1)=%d"
          % (s.cmb_rtgt, s.cmb_mtgt))
    print("  rider knocked down while mounted(down=1)=%d" % s.cmb_down)
    print("  nearest attacker to the rider's logical pos: dR3 %s" % s.cmb_dr3)
    print("  WARNING  this line's rAtk=/mAtk= are ATTACKER COUNTS."
          "  The mount line's")
    print("           mAtk= is a different quantity (the per-race attacks"
          " lektor).")
    print("  " + verdict(s.cmb_mtgt > 0,
                         "the mount took an attack order at least once"
                         " (坐骑护主 fired; mTgt=0 everywhere => look at"
                         " getAllAttackers on the mount)"))
    print("  " + verdict(s.cmb_ratk > 0,
                         "enemies registered the RIDER as a target"
                         " (log-side half of 「骑手会掉血」; the damage itself"
                         " is eyeball-only)"))
    print("  NOTE  a small dR3 is the point of leaving rMove on the ground -"
          " melee reach")
    print("        is a 3D distance, so lifting it to saddle height is what"
          " would make the")
    print("        rider untouchable.  Do not 'fix' dR3.")


def wrap180(a):
    while a > 180.0:
        a -= 360.0
    while a < -180.0:
        a += 360.0
    return a


def twist_sign_test(s):
    """Decide kRideTwistSign without knowing the rider's world facing.

    sh= is an ABSOLUTE world angle - ATan2 of (L UpperArm - R UpperArm), i.e.
    the rider's lateral axis, whose baseline is whatever direction the mount is
    heading.  So sign(sh) alone says nothing: this log has sh at 170, -146,
    3.8 ... all in one fight.  (The older "112/112 opposite" reading came from
    a fight where the mount happened to hold one heading; that was luck.)

    Baseline removal instead: if the sign is right, sh = base - want, so
    (sh + want) is the slowly varying base; if the sign is flipped,
    (sh - want) is.  Compare the frame-to-frame jitter of both candidates on
    ADJACENT samples only, and use the median - the mean is dominated by the
    pairs where the mount really did turn between two samples.
    """
    rows = [(f, w, sh) for (w, sh, d, on, msk, host, ok, f) in s.twist
            if ok and w is not None and sh is not None and f is not None]
    rows.sort()
    if len(rows) < 8:
        return None
    gaps = [b[0] - a[0] for a, b in zip(rows, rows[1:]) if b[0] > a[0]]
    if not gaps:
        return None
    step = min(gaps)                      # = kRideTwistLogGap
    plus, minus = [], []
    for (f0, w0, s0), (f1, w1, s1) in zip(rows, rows[1:]):
        if f1 - f0 != step:
            continue
        plus.append(abs(wrap180((s1 + w1) - (s0 + w0))))
        minus.append(abs(wrap180((s1 - w1) - (s0 - w0))))
    if len(plus) < 8:
        return None
    med = lambda v: sorted(v)[len(v) // 2]
    return (len(plus), med(plus), med(minus),
            sum(1 for a, b in zip(plus, minus) if a < b), step)


def report_twist(s):
    print("")
    print("== T1 - twist sign and target distance ==")
    if not s.twist:
        print("  no TWIST line (no stance, or the 40-line budget never opened).")
        return
    dstat = Stat()
    same_sign = 0
    scored = 0
    far = 0
    notgt = 0
    noread = 0
    for want, sh, d, on, msk, host, shok, f in s.twist:
        # d = -1.0 is the "no target at all" sentinel, not a distance.
        if d is not None and d < 0.0:
            notgt += 1
        else:
            dstat.add(d)
            if d is not None and d > 60.0:
                far += 1
        if not shok:
            noread += 1
            continue
        if want is not None and sh is not None and abs(want) > 5.0:
            scored += 1
            if (want > 0) == (sh > 0):
                same_sign += 1
    print("  samples=%d   d= %s   d>60: %d   no-target(d=-1): %d"
          % (len(s.twist), dstat, far, notgt))
    print("  NOTE  want= is the APPLIED (low-passed) twist, raw= is the raw"
          " target.")
    print("        TWIST f= counts gLegPoseFrames; STANCE f= is the global frame."
          "  Two")
    print("        different counters - line them up by timestamp, never by f=.")
    if noread and noread == len(s.twist):
        print("  CHECK every sample has sh=<x>/0 = both UpperArm bones failed to"
              " read.")
        print("        The sign criterion is unmeasured, not passed.")
        return
    res = twist_sign_test(s)
    if res is None:
        print("  CHECK too few adjacent sh samples to test the sign"
              " (|want|>5 rows: %d)." % scored)
        print("        Unmeasured, not passed.")
    else:
        n, mp, mm, wins, step = res
        print("  sign test (baseline removed, %d adjacent pairs %d frames apart):"
              % (n, step))
        print("    median jitter of sh+want = %.1f deg   <- the rider's facing if"
              " the sign is RIGHT" % mp)
        print("    median jitter of sh-want = %.1f deg   <- the rider's facing if"
              " it is FLIPPED" % mm)
        print("    sh+want is the smoother one on %d of %d pairs" % (wins, n))
        print("  " + verdict(mp < mm,
                             "the applied twist opposes the shoulder line"
                             " (kRideTwistSign is correct; if sh-want were the"
                             " smooth one, flip that one constant)"))
    print("  raw sign tally, INFORMATIONAL ONLY: |want|>5 samples=%d, same sign"
          " as sh=%d" % (scored, same_sign))
    print("        sh is an ABSOLUTE world angle (the rider's lateral axis"
          " follows the")
    print("        mount's heading), so a same-sign row is not a fault by"
          " itself - it")
    print("        just means the baseline was past +-90 there.  Judge by the"
          " test above.")



def report_handback(s):
    # The balance above (takeovers == restored + released) counts EVENTS.  It was
    # perfectly balanced on the trip where the player reported "legs slightly
    # stuck apart after dismount" (6/6/0, kept 82/82 = 1.0000), which is exactly
    # why it cannot see that defect: it proves the handback RAN, not that it
    # WORKED.  DLL 307712 B reads the state back afterwards and prints it.
    #
    #   man=      bitmask of our bones STILL isManuallyControlled() after the
    #             clear.  Bit order = kLegPoseBones: 0/1 thighs, 2/3 calves,
    #             4/5 spine.  Anything but 0x00 IS the stuck leg.
    #   minDot=   worst |dot(orientation, initialOrientation)| after reset().
    #             1.0000 = every bone sits at its binding pose.  Lower = reset()
    #             did not take, i.e. the bone kept our 45 deg flex.
    #   residue=  our bone entries still held below 1.0 by a blend mask on a LIVE
    #             AnimationState.  Judge it WITH dropped=: residue>0 while
    #             dropped=0 is somebody else's mask (every tracked state was
    #             handled), so it is information, not our leak.
    #   dropped=  tracked states that were untraceable at release, i.e. the
    #             documented leak path (an unfindable pointer must never be
    #             dereferenced, so its mask stays behind).  A leaked mask leaves
    #             that clip permanently unable to drive the thigh, which renders
    #             as the binding-pose straddle - the reported symptom.
    #   late=     states the live lists had lost but the by-name re-fetch got
    #             back and restored anyway (RE_NOTES 21.4).  On every build
    #             before md5 E7613634... this field does not exist and those
    #             entries were counted in dropped= instead.  A plain ride loses
    #             2-4 clips this way, so late>0 / dropped=0 is the healthy shape
    #             and late=0 means the route simply was not exercised.
    #   states=   how many AnimationStates the ride ended up masking (cap 8).
    print("")
    print("  -- handback audit (LEGPOSE handback, DLL 307712 B+) --")
    if not s.handback:
        print("    no handback line: this log predates DLL 307712 B (or nothing"
              " was ever handed")
        print("    back).  The event balance above says the handback RAN; it"
              " cannot say the bones")
        print("    actually came home, so the reported stuck-leg defect is"
              " UNMEASURED here.")
        return
    bad_man = [d for d in s.handback if str(d.get("man", "0x00")).lower()
               not in ("0x00", "0")]
    dots = []
    for d in s.handback:
        v = fnum(d, "minDot")
        if v is not None:
            dots.append(v)
    bad_dot = [v for v in dots if v < 0.999]
    drop = 0
    resid = 0
    late = 0
    have_late = any("late" in d for d in s.handback)
    for d in s.handback:
        drop += int(fnum(d, "dropped", 0))
        resid += int(fnum(d, "residue", 0))
        late += int(fnum(d, "late", 0))
    print("    handbacks=%d  worst minDot=%s  man!=0: %d  residue total=%d "
          " dropped total=%d  late total=%s"
          % (len(s.handback),
             ("%.4f" % min(dots)) if dots else "?",
             len(bad_man), resid, drop, late if have_late else "n/a"))
    print("    " + verdict(not bad_man,
                           "every bone left manual control"
                           " (man=0x00 on all handbacks)"))
    if dots:
        print("    " + verdict(not bad_dot,
                               "every bone returned to its binding pose"
                               " (minDot=1.0000)"))
    else:
        print("    CHECK minDot missing - cannot say the bones returned to the"
              " binding pose")
    print("    " + verdict(drop == 0,
                           "no tracked mask was dropped untraceable (dropped=%d)"
                           % drop))
    # late= is the by-name re-fetch (RidingPlugin.cpp LegMaskRelease route 2,
    # RE_NOTES 21.4).  Without it a stopped clip is untraceable and its mask
    # leaks, so on a pre-fix build these very entries WERE the dropped= ones.
    # dropped=0 therefore stopped being self-sufficient evidence: it can also
    # mean "nothing stopped this ride", which proves nothing about the fix.
    if not have_late:
        print("        NOTE no late= field -> this log predates the by-name"
              " rescue build (md5 E7613634...).")
        print("             On that build dropped>0 was expected on any ride"
              " where a clip stopped.")
    elif late:
        print("        PASS  the by-name rescue fired: %d mask(s) the live lists"
              " had lost were" % late)
        print("              still restored (RE_NOTES 21.2: a stopped clip nulls"
              " mainState, which is")
        print("              why route 1 alone leaked).  late>0 with dropped=0 is"
              " the healthy shape.")
    else:
        print("        NOTE late=0: no clip stopped between mask install and"
              " handback this ride,")
        print("             so the rescue route was never exercised - dropped=0"
              " here is NOT evidence")
        print("             that it works.  A plain ride normally loses 2-4"
              " (compare the pre-fix trips).")
    if drop:
        if have_late:
            print("        => STILL LEAKING after the by-name rescue: %d mask(s)"
                  " outlived the ride." % drop)
        else:
            print("        => THIS is the leak: %d mask(s) outlived the ride."
                  % drop)
        print("           The clip they sit on can no longer drive the thigh,"
              " which renders as")
        print("           thighs clamped at the binding pose with only the"
              " calves animating.")
        if have_late:
            print("           Route 2 failing too means the state set was rebuilt"
                  " (RE_NOTES 21.3), the")
            print("           clip name did not fit 64 chars, or body/0xA8 was"
                  " NULL - check those three.")
        # ⚠️ residue can NEVER contradict this, by construction:
        # LegMaskResidueCount() re-scans only the LIVE addList/removeList, and a
        # leaked mask sits on a state that is no longer listed.  So
        # "residue=0 dropped>0" is the leak's normal shape, not a contradiction.
        print("           !! residue=%d does NOT contradict it: residue re-scans"
              " only the LIVE lists," % resid)
        print("              and a leaked mask sits on a state that is no longer"
              " listed at all.")
    if resid and not drop:
        print("        NOTE residue=%d with dropped=0 = somebody else's blend"
              " mask on our bones," % resid)
        print("             not our leak.  Information only.")
    for d in s.handback:
        print("      %s man=%s minDot=%s residue=%s dropped=%s late=%s states=%s"
              % (d.get("_ts", "?"), d.get("man", "?"), d.get("minDot", "?"),
                 d.get("residue", "?"), d.get("dropped", "?"),
                 d.get("late", "n/a"), d.get("states", "?")))
    print("    NOTE all-clean here does NOT clear the report: this measures the"
          " bones and the")
    print("    masks, and a straddle that survives BOTH is engine-side (whatever"
          " clip plays")
    print("    after dismount simply not driving the thighs).  That half is"
          " eyes-only.")


def report_legs(s):
    print("")
    print("== straddle regression (must not have moved) ==")
    if not s.takeovers and not s.restored and not s.released:
        print("  no LEGPOSE line at all - the straddle pass never armed"
              " (nobody mounted,")
        print("  or this log predates it).  NOT a pass: there is nothing here"
              " to judge.")
        return
    print("  takeovers=%d  restored-on-dismount=%d  released mid-ride=%d"
          % (len(s.takeovers), s.restored, len(s.released)))
    # A mid-ride release IS a handback (setManuallyControlled(false) + reset),
    # so the balance to check is takeovers == restored + released.  Comparing
    # takeovers against restored alone reports a balanced run as broken.
    print("  " + verdict(len(s.takeovers) == s.restored + len(s.released),
                         "every takeover was handed back"
                         " (restored + released must equal takeovers;"
                         " a shortfall = a leg left at 45 deg)"))
    report_handback(s)
    # Split the releases by reason before judging them.  Since DLL 301056 B a
    # "(rider down)" release is the isDown()/isDead() guard collecting its own
    # bones on purpose - the FIX firing, and the expected shape for any trip
    # where the rider gets knocked out.  Only a grace= release (host lost for
    # 12 straight frames) is a fault.  Judging them as one bucket reports a
    # correct guard as a defect.
    rel_down = [r for r in s.released if r.get("_why") == "down"]
    rel_grace = [r for r in s.released if r.get("_why") != "down"]
    print("  " + verdict(not rel_grace,
                         "no host-lost (grace=) release"
                         + ("" if not rel_grace else
                            " - grace/f: " + ", ".join(
                                "t=%s grace=%s f=%s%s"
                                % (r.get("_ts", "?"),
                                   r.get("grace", "?").rstrip(")"),
                                   r.get("f", "?").rstrip(")"),
                                   " DOWN" if in_down(s, r.get("_ts")) else "")
                                for r in rel_grace[:8]))))
    if rel_down:
        print("  INFO %d release(s) read '(rider down)' = the 301056 B"
              " isDown()/isDead() guard" % len(rel_down))
        print("       firing as designed, at t=%s."
              % ", ".join(str(r.get("_ts", "?")) for r in rel_down[:8]))
        print("       That is a PASS, not a fault: the alternative is unmasked"
              " knockdown clips")
        print("       rotating our manually controlled thighs.  Expect the"
              " matching re-arm on")
        print("       stand-up (or the next mount) and no bad kept in that"
              " window.")
    print("  kept: %s   not-1.0000 samples=%d" % (s.kept, s.kept_bad))
    if not s.kept.n:
        print("  CHECK no kept= sample - that row is budgeted AND gated on"
              " Ctrl+NUM.,")
        print("        so its absence means unmeasured, not clean.")
    else:
        print("  " + verdict(s.kept_bad == 0,
                             "kept is 1.0000 everywhere"
                             " (anything less = the blend mask missed a"
                             " contributor; compare msk=)"))

    # The bad samples themselves.  These used to be invisible, and the takeover
    # dump below sat right under the CHECK where it read as if IT were the list
    # of bad samples - it is not, it is one row per mount.
    if s.kept_bad_rows:
        print("  the not-1.0000 samples (t= lines these up against STANCE,"
              " 'released' and down=):")
        for f, bone, k, abd, flx, t in s.kept_bad_rows:
            print("    t=%s f=%s %-16s kept=%.4f abd=%s flx=%s%s"
                  % (t, "?" if f is None else "%d" % f, bone, k, abd, flx,
                     "   <- rider DOWN" if in_down(s, t) else ""))

    # 2026-08-31: the knockdown correlation.  Computed rather than asserted,
    # because the first (wrong) reading of the same data blamed the route-A
    # stance handover - the stance drops merely happened to be nearby.  Since
    # DLL 301056 B the EXPECTED shape inside a knockdown window changed: one
    # "released (rider down)" line and NO bad kept, because the guard hands the
    # thighs back before an unmasked knockdown clip can rotate them.
    if s.down_spans and (s.kept_bad_rows or s.released):
        print("  knockdown windows (P3CMB down=1, merged): "
              + ", ".join("%.0f-%.0f" % (a, b) for a, b in s.down_spans[:12]))
        # A gap between two merged spans is NOT a stand-up unless a down=0 row
        # actually sits in it.  The probe is throttled and can simply stop
        # logging for a while: on the 2026-08-31 fourth trip the 242-253 gap
        # held 3 rows and all 3 said down=1.
        for i in range(len(s.down_spans) - 1):
            g0, g1 = s.down_spans[i][1], s.down_spans[i + 1][0]
            ups = [t for t in s.up_ts if g0 < t < g1]
            print("        gap %.0f-%.0f: %s"
                  % (g0, g1,
                     ("%d down=0 row(s) in it -> he really stood up here"
                      % len(ups)) if ups else
                     "no down=0 row in it -> ROW GAP, not a stand-up"))
        # "Did he ever get back up?" is asked about the time AFTER the first
        # knockdown began - down=0 rows from before it only say he started the
        # ride on his feet.  On the fourth trip 121/121 rows from 160 s to the
        # 260.972 s dismount said down=1, so re-arm-after-stand-up went
        # unexercised (which is fine: re-arm is the ordinary takeover
        # fall-through, takeover = !gLegPoseArmed, not new code).
        if not [t for t in s.up_ts if t > s.down_spans[0][0]]:
            print("        no P3CMB down=0 row after the first knockdown:"
                  " nothing here shows him")
            print("        getting back up, so 're-arm after stand-up' is"
                  " UNEXERCISED (not failed -")
            print("        re-arm is the ordinary takeover fall-through, not new"
                  " code).")
        bad_in = sum(1 for r in s.kept_bad_rows if in_down(s, r[5]))
        gr_in = sum(1 for r in rel_grace if in_down(s, r.get("_ts")))
        dn_in = sum(1 for r in rel_down if in_down(s, r.get("_ts")))
        print("        inside a window: bad kept %d/%d   grace= releases %d/%d"
              "   (rider down) releases %d/%d"
              % (bad_in, len(s.kept_bad_rows), gr_in, len(rel_grace),
                 dn_in, len(rel_down)))
        # Vacuity check.  kept= is budgeted, so "0 bad kept in a window" can be
        # an empty set instead of a result - exactly what happened on the fourth
        # trip (sampling ended 100.756 s, knockdown at 156 s).
        kept_over = True
        if s.kept_t0 is not None:
            over = [1 for a, b in s.down_spans
                    if not (b < s.kept_t0 or a > s.kept_t1)]
            kept_over = bool(over)
            print("        kept sampling ran %.3f-%.3f s and %s a knockdown"
                  " window"
                  % (s.kept_t0, s.kept_t1,
                     "overlaps" if over else "NEVER overlaps"))
            if not over:
                print("        => the bad-kept count above is an EMPTY SET, not"
                      " evidence: the budget ran")
                print("           out before the knockdown.  Judge the guard by"
                      " the released line plus")
                print("           'nothing writes those bones after a release'.")
        if bad_in or gr_in:
            if bad_in == len(s.kept_bad_rows) and gr_in == len(rel_grace):
                print("        => KNOCKDOWN-CORRELATED, and with DLL 301056 B"
                      " that means the")
                print("           isDown()/isDead() guard did NOT hold.  Do NOT"
                      " widen the grace - that")
                print("           only buys more polluted frames.  Check that"
                      " the riderDown branch really")
                print("           sits BEFORE '---- gate + mask ----' and that"
                      " isDown() is read on the")
                print("           rider (an unmasked knockdown clip rotates our"
                      " manual thighs: a manual")
                print("           bone survives Skeleton::reset() but still"
                      " receives applyToNode).")
            else:
                print("        => PARTIAL correlation.  The ones outside a"
                      " window need their own")
                print("           explanation - do not fold them into the"
                      " knockdown story.")
        else:
            print("        => clean inside the windows.  With DLL 301056 B that"
                  " is the DESIGNED shape:")
            print("           the guard releases the thighs the moment the rider"
                  " goes down, so no")
            print("           unmasked knockdown clip ever gets to rotate them,"
                  " and the '(rider down)'")
            print("           release above is the fix firing - not a fault.")
            if not kept_over:
                print("           (Resting on the release line alone - see the"
                      " EMPTY SET note: kept was")
                print("            not being sampled during the knockdown.)")
            if s.kept_bad_rows or rel_grace:
                print("        NOTE every bad kept / grace= release in this log"
                      " sits OUTSIDE the")
                print("             knockdown windows, so they need their own"
                      " explanation: go read")
                print("             POSEDUMP and the host= field before"
                      " concluding.")
    elif s.kept_bad_rows or s.released:
        print("  no P3CMB down=1 row at all, so the knockdown correlation could"
              " not be tested")
        print("        (that probe is gated on Ctrl+NUM. too) - absence here is"
              " unmeasured, not clean.")

    print("  takeover rows - ONE PER MOUNT, not the kept samples above:")
    for t in s.takeovers:
        print("    abd=%s flx=%s rad=%s torso=%s host=%s hw=%s msk=%s calf=%s"
              " stance=%s twist=%s" % (
                  t.get("abd"), t.get("flx"), t.get("rad"), t.get("torso"),
                  t.get("host"), t.get("hw"), t.get("msk"), t.get("calf"),
                  t.get("stance"), t.get("twist")))
    print("        msk= is how many weighted clips got a mask this frame."
          "  It is NOT a")
    print("        defect marker: a plain ride has 2 (pose + whatever is fading)."
          "  Judge by kept.")


def report_pose(s):
    print("")
    print("== pose pin + ragdoll regression (v1.6 must still hold) ==")
    if not s.dbg:
        print("  no DBG line - continuous diagnostics (Ctrl+NUM.) was never on,")
        print("  so none of this section could be measured.")
        return
    print("  DBG frames=%d" % s.dbg)
    print("  pose weight: %s" % s.pose_w)
    print("    >=0.995: %d    0.94-0.995: %d    <0.94: %d"
          % (s.pose_1, s.pose_dip, s.pose_low))
    print("  " + verdict(s.pose_dip == 0,
                         "no 0.95-0.97 dips (a fixed-interval dip = PoseLayerPin"
                         " lost its grip -> read POSEDUMP)"))
    print("    (only the middle bucket is a defect signal.  <0.94 is EXPECTED"
          " on the")
    print("     mount fade-in ramp and for every combat-stance frame - route A"
          " clears")
    print("     the pose channel and pins 'guard 1h' instead, so kRidePose"
          " really is 0")
    print("     while a fight is on.  Cross-check with the STANCE section"
          " above.)")

    print("  oth (idle_stand_normal residue): %s" % s.oth)
    print("  act (total action weight): %s   frames over 1.02: %d"
          % (s.act, s.act_over))
    print("  " + verdict(s.act_over == 0,
                         "total action weight stayed under 1.02"))
    print("  wn nonzero frames=%d (ragdoll drag still moving the node)"
          % s.wn_nonzero)
    print("  " + verdict(s.wn_nonzero == 0,
                         "per-frame ragdoll kill still holding"))
    print("  mvW nonzero frames=%d (expected 0: the movement write lands,"
          " then gets dragged back next frame)" % s.mvw_nonzero)
    if s.posedump:
        print("  POSEDUMP lines=%d - remember the budget burns on the fade-in"
              " ramp unless you skip it" % s.posedump)
    else:
        print("  POSEDUMP lines=0 - EXPECTED from DLL 297984 B / md5"
              " DF86CD07... onward: the")
        print("    dump was removed with the probes (only the 'if (dump)' block"
              " went; PoseLayerPin's")
        print("    layer walk and both shipping gates stayed).  It is still the"
              " only way to name")
        print("    who steals the pose weight - recover it with 'git show"
              " 7838deb:RidingPlugin.cpp'")
        print("    if the pose line ever regresses.")


def report_input(s):
    """Why a hotkey did nothing.  Added after the 2026-08-31 trip, where the
    player pressed the seat keys and the log held no evidence of any press at
    all - the per-press trace is debugContinuous-gated AND is evaluated before
    the debug toggle, and the tune gate logged only one of its three rejections.
    The DLL now emits RAWKEY / controlEnabled / an explicit rejection reason."""
    print("")
    print("== input chain - only meaningful on DLL 301056 B or later ==")
    if s.bindings is not None:
        vals = s.bindings.split()
        print("  bindings resolved: [%s]" % s.bindings)
        if all(v == "0" for v in vals):
            print("  " + verdict(False, "every binding is 0 = the map scan found"
                                        " nothing (registration failed)"))
    else:
        print("  no 'bindings resolved' line - it only prints on CHANGE, so one"
              " per trip")
        print("  is normal and none at all means HotkeyPass never got that far.")
    if s.ce:
        print("  controlEnabled transitions: %s" % " ".join(s.ce))
        if s.ce[-1] == "0":
            print("  " + verdict(False, "it ended at 0 = a UI owned the keyboard;"
                                        " every hotkey was gated off"))
    if not s.rawkeys:
        print("  no RAWKEY line.")
        print("    Either this DLL predates the sniffer, or diagnostics were off"
              " (it is")
        print("    debugContinuous-gated), or the window received no key at all"
              " - and")
        print("    that last one is exactly the case the sniffer exists to prove.")
    else:
        seen = {}
        for kc, mask, ce, cmd in s.rawkeys:
            k = (kc, mask)
            e = seen.setdefault(k, [0, ce, cmd])
            e[0] += 1
            e[2] = cmd
        print("  distinct keys seen=%d, presses=%d (budget 400)"
              % (len(seen), len(s.rawkeys)))
        for (kc, mask), (n, ce, cmd) in sorted(seen.items(),
                                               key=lambda x: -x[1][0])[:24]:
            print("    kc=%-4s mask=%-5s x%-4d ce=%s cmd=%s"
                  % (kc, mask, n, ce, cmd))
        nomap = [k for k, v in seen.items() if v[2] == "-"]
        print("  " + verdict(not nomap,
                             "every key pressed resolved to one of our commands"
                             " (cmd=- means that physical key is bound to"
                             " nothing of ours - THE answer to 'I pressed it and"
                             " nothing happened')"))
    print("  commands fired=%d%s"
          % (len(s.fired),
             "" if not s.fired else ":  " + ", ".join(
                 "%s x%d" % (n, s.fired.count(n))
                 for n in sorted(set(s.fired)))))
    # ⚠️ fired=0 next to a pile of `tuned` lines is the EXPECTED shape of a trip
    # taken without diagnostics, NOT a contradiction.  Verified in the source:
    # both the RAWKEY sniffer and the `input '<name>' fired` trace sit behind
    # `if (debugContinuous)`, while the `tuned` DebugLog is unconditional.  So
    # `tuned` is the load-bearing evidence and the two gated families prove
    # nothing by their absence.
    if s.tuned and not s.fired:
        print("    (fired= is debugContinuous-gated, `tuned` is not - so this"
              " pairing is")
        print("     normal for a trip without diagnostics.  `tuned` carries the"
              " proof.)")
    if s.tuned:
        print("  " + verdict(True, "X-4 does not reproduce - %d press(es) reached"
                                   " TuneSeat, so the whole chain (OIS -> modifier"
                                   " mask -> KeyEdge -> HotkeyPass -> TuneSeat)"
                                   " ran" % len(s.tuned)))
    elif not s.rawkeys and not s.fired and not s.rejects:
        print("  " + verdict(False, "no evidence of ANY press this trip - and with"
                                    " diagnostics off that is not proof of"
                                    " absence either; re-run with Ctrl+Numpad."
                                    " ON"))
    if s.rejects:
        print("  tune gate rejections:")
        for why, n in sorted(s.rejects.items(), key=lambda x: -x[1]):
            print("    x%-4d %s" % (n, why[:96]))
        print("        (a rejection means the KEY WORKED and the selection or the"
              " seat row")
        print("         was the problem - a completely different fix from a"
              " missing RAWKEY.)")


def report_tuned(s, cfg):
    print("")
    print("== T3 - X-2: tuning must write to a race row, never a name row ==")
    if not s.tuned:
        print("  no 'tuned' line - nobody pressed a tune key this session,")
        print("  so T3's real acceptance point was not exercised.")
    else:
        nokey = [t for t in s.tuned if not t[1]]
        seen = {}
        for t in s.tuned:
            seen.setdefault(t[0], t[1])
        for sp, key in sorted(seen.items()):
            print("  %-18s -> %s" % (sp[:18], key or "NO key= (NAME ROW!)"))
        print("  tune presses=%d  without key=: %d" % (len(s.tuned), len(nokey)))
        print("  " + verdict(not nokey,
                             "every tune landed on a race row"
                             " (a missing key= means getRaceKey() came back"
                             " empty - check race= on the mount line)"))
        report_tune_travel(s)
    if cfg:
        print("  riding.cfg now: %d race row(s), %d name row(s)%s"
              % (cfg[0], cfg[1], "" if not cfg[2] else "  -> " + ", ".join(cfg[2])))
        print("  " + verdict(cfg[1] == 0,
                             "cfg holds race rows only"))


def report_tune_travel(s):
    """Did the seat MOVE, not just "did a key fire".  These are two different
    outcomes with the same log line count: a press that lands on
    SeatTuneLimitRef still logs a `tuned` line, with the value unchanged.
    That is the shape a player would report as "I pressed it, nothing moved",
    so the script has to separate it out rather than leave it to the eye.

    ⚠️ Values are the STORED reference-frame numbers x100 (source: the printf
    multiplies userOffset by 100).  A step is nominally kTuneStep(0.1)/k, so at
    k=1.0 consecutive presses should differ by ~10 in these units."""
    vals = []
    for t in s.tuned:
        if len(t) < 5:
            return                          # older parse, no values captured
        try:
            vals.append((int(t[2]), int(t[3])))
        except (TypeError, ValueError):
            return                          # pre-301056 B line shape
    if not vals:
        return
    moves = sum(1 for a, b in zip(vals, vals[1:]) if a != b)
    span_up = max(v[0] for v in vals) - min(v[0] for v in vals)
    span_fw = max(v[1] for v in vals) - min(v[1] for v in vals)
    print("  stored value travel: up %.2f..%.2f (span %.2f), fwd %.2f..%.2f"
          " (span %.2f)"
          % (min(v[0] for v in vals) / 100.0, max(v[0] for v in vals) / 100.0,
             span_up / 100.0,
             min(v[1] for v in vals) / 100.0, max(v[1] for v in vals) / 100.0,
             span_fw / 100.0))
    print("  presses that changed the value: %d of %d transition(s)"
          % (moves, max(0, len(vals) - 1)))
    print("  " + verdict(moves > 0,
                         "the seat value actually moved (a press that only"
                         " re-logs the same number = clamped at"
                         " SeatTuneLimitRef, which on screen IS"
                         " \"nothing happened\")"))


CFG_ROW = re.compile(r"^\d+-[A-Za-z_0-9]+\.(?:base|mod)$")


def read_cfg(path):
    """(race rows, name rows, name keys) or None.  Read-only, never written."""
    if not os.path.isfile(path):
        return None
    try:
        with open(path, "rb") as fh:
            text = fh.read().decode("utf-8-sig", "replace")
    except IOError:
        return None
    race, names = 0, []
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k = line.split("=", 1)[0]
        if k == "defaults":
            continue
        if CFG_ROW.match(k):
            race += 1
        else:
            names.append(k)
    return (race, len(names), names)


DEFAULT_CFG = (r"D:\steam\steamapps\common\Kenshi\mods"
               r"\RidingPlugin\riding.cfg")


def report_sheathe(s):
    """T6 / P4-3 step 1: name the second writer that re-sheathes the weapon."""
    print("")
    print("== P4-3 step 1 - who re-sheathes the rider's weapon (P43SH) ==")
    if s.sh_hookfail:
        print("  " + verdict(False, "the hook did NOT install ('Could not hook"
                                    " CharacterHuman::sheatheWeapon!')."))
        print("             Nothing below means anything - fix the hook first"
              " (prologue / GetRealAddress).")
        return
    if not s.sh_sites and not s.sh_dump:
        print("  no P43SH line at all.  THREE different readings, and the hook"
              " error line above")
        print("  did NOT appear, so if the probe is in this build the hook"
              " installed:")
        print("    - the probe was REMOVED in DLL 297984 B / md5 DF86CD07..."
              " (the probe-free")
        print("      build, 2026-09-01).  It had answered: the re-sheather is"
              " Character::")
        print("      _ragdollMode (real=16, median gap 22 frames), secondary"
              " _carryMode.")
        print("      Silence is EXPECTED here - check the DLL's md5 before"
              " reading anything")
        print("      into it.  Recover the probe with 'git show 61872dc'.")
        print("    - this log predates DLL 305152 B (the probe build), or")
        print("    - sheatheWeapon was never called on a tracked rider this"
              " session, which is")
        print("      itself a finding: the re-sheathe goes through some other"
              " API (look at")
        print("      dropWeaponInHands@0x5CC760's family).")
        print("  NOT MEASURED - not a pass.")
        return

    # The module name is printed by the probe on purpose: if AddHook ever hands
    # us something other than the caller's own frame, the return address lands in
    # KenshiLib/MinHook and every RVA below is void.  NOTE the test insists on
    # the .exe: "kenshi" alone would happily accept KenshiLib.dll, which is one
    # of the two shapes T6 criterion 1 is there to catch.
    foreign = [k for k in s.sh_sites
               if not re.match(r"(?i)^kenshi[^+]*\.exe\+0x", k)]
    print("  " + verdict(not foreign,
                         "all %d site(s) resolve inside the kenshi module"
                         % len(s.sh_sites) if not foreign else
                         "%d site(s) resolve OUTSIDE the kenshi module -> the"
                         " return address is not the" % len(foreign)))
    if foreign:
        for k in foreign[:6]:
            print("             " + k)
        print("             caller's.  Every RVA below is void until that is"
              " explained.")

    print("  %d P43SH line(s), %d distinct site(s)."
          % (s.sh_lines, len(s.sh_sites)))
    print("  %-34s %5s %5s %s" % ("site", "real", "noop", "gaps (frames)"))
    # real>0 first: a wih=0 call sheathed nothing and cannot be the writer.
    for site, e in sorted(s.sh_sites.items(),
                          key=lambda kv2: (-kv2[1]["real"], kv2[0])):
        g = e["gaps"]
        gs = ("n=%d min=%d med=%d max=%d"
              % (len(g), min(g), sorted(g)[len(g) // 2], max(g))) if g else "-"
        print("  %-34s %5d %5d %s" % (site[:34], e["real"], e["noop"], gs))

    real = {k: e for k, e in s.sh_sites.items() if e["real"] > 0}
    print("  " + verdict(bool(real),
                         "%d site(s) really took a drawn weapon away (wih=1)"
                         % len(real) if real else
                         "every call was a no-op (wih=0) - none of these sites"
                         " is the writer;"))
    if not real:
        print("             the rider's hands were empty every time, so force a"
              " real draw")
        print("             (get into a fight on an elig=1 mount) and ride"
              " again.")
    else:
        # The second writer is defined by a REPEAT, not by a single call: one
        # sheathe is explained by "combat ended -> auto-sheathe".
        rep = {k: e for k, e in real.items() if (e["real"] + e["noop"]) > 1}
        print("  " + verdict(bool(rep),
                             "%d of them repeat (>1 call) -> that is the"
                             " candidate set" % len(rep) if rep else
                             "none of them repeats - a single sheathe is"
                             " already explained by"))
        if not rep:
            print("             'combat ended -> auto-sheathe'; the SECOND"
                  " writer needs a repeat.")
            print("             Ride longer / fight twice before dismounting.")
        else:
            for k, e in sorted(rep.items(), key=lambda kv2: -kv2[1]["real"]):
                g = e["gaps"]
                print("             %s  real=%d  cadence=%s" % (
                    k, e["real"],
                    ("median %d frames" % sorted(g)[len(g) // 2]) if g else "?"))
            print("             Copy that <module>+0x<rva> back into TASK.md"
                  " P4-3 step 1.")
    if s.sh_dump:
        print("  dismount table: " + s.sh_dump[:220])
        mo = re.search(r"\bover=(\d+)", s.sh_dump)
        if mo and int(mo.group(1)) > 0:
            print("  " + verdict(False, "over=%s - the %d-slot site table filled"
                                        " up, so some caller never got its own"
                                        % (mo.group(1), len(s.sh_sites))))
            print("             row.  Raise kShSites and ride again before"
                  " trusting the table above.")
    else:
        print("  no dismount summary line (the ride never reached Dismount()).")
    print("  NOTE  naming only.  Never call sheatheWeapon as a fix (that is"
          " SHEATHING, not")
    print("        drawing) and never write 're-draw every frame' - HISTORY §B.")


def report_attach(s):
    """P4-3 step 2: is attachItem(..., "hands") called at all while carried?"""
    print("")
    print("== P4-3 step 2 - is the weapon re-attached to the HAND slot (P43AT) ==")
    if s.at_hookfail:
        print("  " + verdict(False, "at least one attachItem hook did NOT"
                                    " install:"))
        for line in s.at_hookfail[:4]:
            print("             " + line[:150])
        print("             Nothing below means anything.  attachItem is"
              " OVERLOADED and the two")
        print("             overloads are separate functions - with one hook"
              " missing, a real call")
        print("             through it is indistinguishable from 'never"
              " called'.")
        return
    if not s.at_dump and not s.at_sites:
        print("  no P43AT line at all, and no hook-failure ErrorLog.  TWO"
              " readings now:")
        print("    - the probe was REMOVED in DLL 297984 B / md5 DF86CD07..."
              " (the probe-free")
        print("      build, 2026-09-01).  It had answered: hands=12 / other=0 /"
              " slot0='hands'")
        print("      => the hand slot IS refreshed, yet the blade stays on the"
              " back, so whoever")
        print("      takes it back does NOT go through either hooked overload."
              "  Silence is")
        print("      EXPECTED here - check the DLL's md5 first.  Recover with"
              " 'git show 07f3588'.")
        print("    - the code never ran: the dump prints unconditionally once a"
              " ride has")
        print("      happened, so either this log predates DLL 312832 B or no"
              " Mount()")
        print("      completed this session.")
        print("  Note there is still NO 'installed but nobody called it'"
              " reading, unlike P43SH.")
        print("  NOT MEASURED - not a pass.")
        return

    # Same module test as P43SH, same reason, same insistence on the .exe:
    # "kenshi" alone would let KenshiLib.dll through, and a return address
    # inside the hook engine voids every RVA below it.
    foreign = [k for k in s.at_sites
               if not re.match(r"(?i)^kenshi[^+]*\.exe\+0x", k)]
    if s.at_sites:
        print("  " + verdict(not foreign,
                             "all %d site(s) resolve inside the kenshi module"
                             % len(s.at_sites) if not foreign else
                             "%d site(s) resolve OUTSIDE the kenshi module ->"
                             " the return address is not" % len(foreign)))
        for k in foreign[:6]:
            print("             " + k)
        if foreign:
            print("             the caller's.  Every RVA below is void until"
                  " that is explained.")

    print("  %d P43AT line(s), %d distinct site(s), %d of the logged lines had"
          " slot='hands'." % (s.at_lines, len(s.at_sites), s.at_hands))
    if s.at_sites:
        print("  %-34s %3s %6s %6s %s" % ("site", "ov", "hands", "other",
                                          "slots seen"))
        # hands first: everything else is bookkeeping for this question.
        for site, e in sorted(s.at_sites.items(),
                              key=lambda kv2: (-kv2[1]["hands"], kv2[0])):
            print("  %-34s %3d %6d %6d %s" % (
                site[:34], e["ov"], e["hands"], e["other"],
                ",".join(sorted(e["slots"]))[:40] or "-"))

    hk = app = None
    over = 0
    dump_hands = None
    bad_app = True          # stays True while we have no app= to judge
    if s.at_dump:
        print("  dismount table: " + s.at_dump[:220])
        m = re.search(r"\bhk=(\d+)", s.at_dump)
        hk = int(m.group(1)) if m else None
        m = re.search(r"\bapp=(\S+)", s.at_dump)
        app = m.group(1) if m else None
        m = re.search(r"\bover=(\d+)", s.at_dump)
        over = int(m.group(1)) if m else 0
        m = re.search(r"\bhands=(\d+)", s.at_dump)
        dump_hands = int(m.group(1)) if m else None
    else:
        print("  no dismount summary line (the ride never reached Dismount())"
              " - the counters")
        print("  below are only the logged lines, which have per-site budgets.")

    # hk= is what turns "hands=0" from a shrug into a verdict.  Bit 0 = 2-arg
    # hook, bit 1 = 3-arg; 3 = both, which is the only state that makes silence
    # meaningful.
    if hk is not None:
        print("  " + verdict(hk == 3,
                             "hk=3 - both overloads hooked, so silence is"
                             " informative" if hk == 3 else
                             "hk=%d - only %s hooked; a call through the other"
                             " overload would be" % (hk, {0: "neither", 1: "the"
                             " 2-arg form", 2: "the 3-arg form"}.get(hk, "?"))))
        if hk != 3:
            print("             invisible, so 'hands=0' below cannot be"
                  " trusted.")
    if app is not None:
        bad_app = app.strip("'") in ("0", "0000000000000000", "(null)",
                                     "00000000")
        print("  " + verdict(not bad_app,
                             "app=%s - the ride captured an AppearanceBase* to"
                             " filter on" % app if not bad_app else
                             "app=%s - getAppearance() returned NULL, so the"
                             " filter rejected" % app))
        if bad_app:
            print("             every call and hands=0 says nothing about the"
                  " engine.")
    if over:
        # NOT len(at_sites): that is how many rows we SAW, while over= counts
        # the callers that arrived after the fixed kAtSites table was already
        # full.  Printing the seen count here would read like the table is
        # tiny when it is simply saturated.
        print("  " + verdict(False, "over=%d - the site table (kAtSites) filled"
                                    " up, so that many calls never got" % over))
        print("             a row of their own.  Raise kAtSites and ride"
              " again; the sites listed")
        print("             above are still real, just incomplete.")

    # The verdict itself.  Prefer the dump's counter (unbudgeted) over the
    # logged-line count (per-site budgets can hide repeats).
    hands = dump_hands if dump_hands is not None else s.at_hands
    # Stash it for report_p41d()'s weapon gate: "wpn=0 pWpn=0" must not be read
    # as "unarmed rider" when this same ride attached a weapon to the hand slot.
    s.at_hands_best = hands
    trustworthy = (hk == 3) and (app is not None) and not bad_app
    if hands > 0:
        print("  ANSWER  attachItem(..., \"hands\") WAS called %d time(s) while"
              " the rider was" % hands)
        print("          carried => the attachment point really was refreshed,"
              " so an empty-handed")
        print("          rider on screen has ANOTHER cause (look at the mesh"
              " name and at whether")
        print("          something detaches it again afterwards).")
    else:
        print("  ANSWER  attachItem was never called with slot=\"hands\" this"
              " ride"
              + ("" if trustworthy else " (BUT see the checks above)"))
        print("          => drawWeapon cannot reach the attach step in the"
              " carried state.")
        if not trustworthy:
            print("          Not a clean verdict until hk=3 and app!=0.")
    print("  NOTE  naming only.  Neither answer may become 'so we call"
          " attachItem ourselves")
    print("        every frame' - that is compensating an absolute overwrite at"
          " the writing")
    print("        end, i.e. HISTORY §B's three rounds of servo.  Go kill the"
          " writer.")


def report_detach(s):
    """P4-3 step 2, the THIRD probe: who takes the hand slot away (P43DT+P43HD)."""
    print("")
    print("== P4-3 step 2 - who takes the HAND slot away (P43DT + P43HD) ==")
    if s.dt_hookfail:
        print("  " + verdict(False, "the detachItem hook did NOT install:"))
        for line in s.dt_hookfail[:3]:
            print("             " + line[:150])
        print("             Nothing below means anything: silence and 'never"
              " called' are the same")
        print("             shape with the hook off.")
        return
    if not s.dt_rides and not s.dt_sites and not s.hd_rides:
        print("  no P43DT/P43HD line at all, and no hook-failure ErrorLog.  THREE"
              " readings:")
        print("    - the probe was REMOVED in DLL 304128 B / md5 1792B72E... (the"
              " probe-free")
        print("      T19 build, 2026-09-02).  It had answered BOTH of its questions:"
              " exactly ONE")
        print("      site ever carried a slot='hands' detach"
              " (kenshi_x64.exe+0x5CBDF8, 903 calls")
        print("      on trip 15) and that site is attachItem's own"
              " clear-before-write positive")
        print("      control => no third-party writer takes the blade off the hand"
              " (T15); and the")
        print("      §18.12 sheath-slot fix reached the engine (back/back2/hip"
              " detaches appeared")
        print("      only once drawWeapon got a real 2nd arg) => T16.  Silence is"
              " EXPECTED here -")
        print("      check the DLL's md5 first.  The source is NOT in git: it is in"
              " the snapshot")
        print("      D:\\KenshiModDev\\RidingPlugin_src_E83DB50D.cpp.")
        print("    - this log predates DLL 309760 B / md5 4E223D95... (the third"
              " probe's first")
        print("      build, 2026-09-02), or")
        print("    - no ride reached Dismount() this session: BOTH ride lines"
              " print")
        print("      unconditionally once DtProbeArm has run.")
        print("  NOT MEASURED - not a pass.")
        return

    # Preconditions.  This probe's answer may legitimately BE silence, so each of
    # these has to be shown separately - a silent hook, a NULL filter pointer and
    # "nobody detached" are three different facts with one shape.
    hk = 0
    app = None
    over = 0
    dump_hands = None
    for d in s.dt_rides:
        try:
            hk |= int(d.get("hk", "0"))
            over += int(d.get("over", "0"))
        except ValueError:
            pass
    last = s.dt_rides[-1] if s.dt_rides else {}
    app = last.get("app")
    print("  %d ride line(s).  last: %s" % (
        len(s.dt_rides),
        " ".join("%s=%s" % (k, last[k]) for k in
                 ("sites", "hands", "over", "hk", "app") if k in last) or "-"))
    print("  " + verdict(hk & 1,
                         "hk=1 - detachItem(slot) is hooked, so silence is"
                         " informative" if hk & 1 else
                         "hk=0 - the hook is NOT on; every count below is"
                         " meaningless"))
    if hk & 1:
        print("        ONE hook is enough here, unlike attachItem: RE_NOTES"
              " §18.11 shows the")
        print("        Item* overload has zero reachable callers (its ILT thunk"
              " 0x44026 has no")
        print("        callers, and neither overload appears in any vtable) =>"
              " the slot overload")
        print("        is the only way in.")
    if app is not None:
        bad = app.strip("'") in ("0", "0000000000000000", "00000000", "(null)")
        print("  " + verdict(not bad,
                             "app=%s - the ride had an AppearanceBase* to filter"
                             " on" % app if not bad else
                             "app=%s - getAppearance() was NULL, so the filter"
                             " rejected everything" % app))
    if over:
        print("  " + verdict(False, "over=%d call(s) never got a site row"
                                    " (kDtSites full) - raise it and ride"
                                    " again" % over))
    foreign = [k for k in s.dt_sites
               if not re.match(r"(?i)^kenshi[^+]*\.exe\+0x", k)]
    if foreign:
        print("  " + verdict(False, "%d site(s) resolve OUTSIDE the kenshi"
                                    " module - the return address is not the"
                                    " caller's:" % len(foreign)))
        for k in foreign[:4]:
            print("             " + k)
        print("             Every RVA below is void until that is explained.")

    # The site table.  hands first: everything else is bookkeeping.
    hands_sites = [(k, e) for k, e in s.dt_sites.items() if e["hands"] > 0]
    total_hands = sum(e["hands"] for _, e in hands_sites)
    if s.dt_sites:
        print("  detach sites this session (per-ride tables, unbudgeted):")
        print("  %-34s %6s %6s %s" % ("site", "hands", "other", "slot0"))
        for k, e in sorted(s.dt_sites.items(),
                           key=lambda kv2: (-kv2[1]["hands"], kv2[0])):
            print("  %-34s %6d %6d %s" % (k[:34], e["hands"], e["other"],
                                          e["slot0"]))
    else:
        print("  site table EMPTY - nobody called detachItem on this rider's"
              " Appearance.")

    # The one address-free inference, and it is the load-bearing one: attachItem
    # clears the target slot before writing it, from exactly TWO static sites
    # (0x535E0E in the 2-arg form, 0x535ECF in the 3-arg one - RE_NOTES §18.11).
    # So every successful draw MUST produce one "hands" detach, and >2 distinct
    # hands sites PROVES a third writer without needing any RVA arithmetic.
    print("  %d distinct site(s) carried a slot='hands' detach (%d call(s)"
          " total)." % (len(hands_sites), total_hands))
    if len(hands_sites) > 2:
        print("  ANSWER  at least one NON-attachItem writer exists: attachItem"
              " can account for")
        print("          at most 2 distinct sites (§18.11), and %d were seen."
              "  Name them:" % len(hands_sites))
    elif hands_sites:
        print("  READING %d site(s) <= the 2 that attachItem itself can account"
              " for, so this is" % len(hands_sites))
        print("          CONSISTENT with the probe's built-in positive control"
              " and is NOT a")
        print("          failure: every successful draw has to produce one"
              " 'hands' detach from")
        print("          inside attachItem (§18.11).  But 2 sites can equally be"
              " one attachItem")
        print("          plus one OTHER writer - only --ret can tell those apart,"
              " so run it:")
    if hands_sites:
        for k, _e in sorted(hands_sites, key=lambda kv2: -kv2[1]["hands"])[:6]:
            m = re.search(r"\+0x([0-9A-Fa-f]+)$", k)
            if m:
                print("            python tools\\callers.py --ret 0x%s"
                      % m.group(1).upper())
            else:
                print("            (unparsable site: %s)" % k[:60])
        print("          ⚠️ Do NOT shift these by hand.  RUNTIME_RVA_DELTA"
              " (+0xA90) is anchored on")
        print("          three sites in 0x5CE000-0x5DC000 ONLY (§18.10), and"
              " attachItem lives at")
        print("          0x535xxx - outside that window it is unverified."
              "  --ret prints both.")

    # --- T16: the sheath-slot fix's self-evidence (RE_NOTES §18.12 / §18.12.1) ---
    # drawWeapon's 2nd argument IS the sheath location, and leaveSheathEquipped
    # whitelists exactly "hip" and "back" - the empty string we used to pass fell
    # straight through to the function tail, so the blade was never detached from
    # the back, the sheath field stayed '' and no scabbard appeared (§18.12).  Now
    # that a real slot name goes in, every successful draw must ALSO produce a
    # detach carrying that slot, from the call site inside leaveSheathEquipped.
    # That is this build's self-evidence and, like the hands inference above, it
    # needs no RVA arithmetic - the slot NAME does the work.
    SHEATH_SLOTS = ("back", "back2", "hip")
    sheath_sites = [(k, e) for k, e in s.dt_sites.items()
                    if (e.get("slot0") or "") in SHEATH_SLOTS]
    sheath_calls = sum(e["hands"] + e["other"] for _k, e in sheath_sites)
    rd_ok = [d for d in s.rd if fnum(d, "post") == 1]
    rd_sheathed = [d for d in rd_ok if (d.get("_shpost") or "") in SHEATH_SLOTS]
    print("")
    print("  -- T16, the sheath-slot fix: did drawWeapon's arg2 land? --")
    print("     %d site(s) / %d call(s) carried a sheath-family slot %s;"
          " P43RD post=1 edges with"
          % (len(sheath_sites), sheath_calls, "/".join(SHEATH_SLOTS)))
    print("     a non-empty POST sheath name: %d of %d."
          % (len(rd_sheathed), len(rd_ok)))
    if sheath_calls and rd_sheathed:
        print("  PASS   the fix took: the slot name reached the engine"
              " (sh='' -> '%s') AND the"
              % (rd_sheathed[-1].get("_shpost") or "?"))
        print("         previously-skipped detach inside leaveSheathEquipped"
              " now fires.  Name it with")
        print("         --ret (above) before quoting any RVA - the expected"
              " answer is the call at")
        print("         0x5D2632 in 0x5D24B0 (§18.11 / §18.12.1).")
    elif rd_ok and not rd_sheathed:
        print("  FAIL   every successful edge still logs an EMPTY post sheath"
              " name => arg2 never")
        print("         arrived.  Check RideSheathSlotFor()'s return, then the"
              " three ABI rules in")
        print("         §18.12 (never \"back2\", <=15 chars, arg3 is a CONSUMED"
              " in/out param - a")
        print("         temporary std::string is wrong on principle).")
    elif rd_sheathed and not sheath_calls:
        print("  CHECK  the field got written but NO sheath-family detach was"
              " seen => the whitelist")
        print("         still refused the value, or P43DT is absent from this"
              " build (check hk= above).")
    else:
        print("  NOT MEASURED  no successful P43RD edge in this log, so the"
              " fix was never exercised")
        print("         (empty hands + a stance edge is what arms it) - this"
              " is not a pass.")
    print("")

    # ---- the poll half.  This is what makes P43DT's silence readable. --------
    print("  -- P43HD, the hand-slot poll (what the ENGINE left, sampled before"
          " our re-draw) --")
    if not s.hd_rides and not s.hd_edges:
        print("     no P43HD line at all while P43DT spoke - the poll never ran."
              "  Half of this")
        print("     probe is missing: 'nobody detached' cannot be separated from"
              " 'it left another")
        print("     way'.  NOT MEASURED.")
        return
    tot = {"samples": 0, "nonnull": 0, "gain": 0, "loss": 0, "swap": 0,
           "appswap": 0, "nullapp": 0}
    for d in s.hd_rides:
        for k in tot:
            try:
                tot[k] += int(d.get(k, "0"))
            except ValueError:
                pass
    for d in s.hd_rides:
        print("     %s samples=%s nonnull=%s gain=%s loss=%s swap=%s appswap=%s"
              " nullapp=%s first=%s last=%s" % (
                  d.get("_ts", "?"), d.get("samples", "?"),
                  d.get("nonnull", "?"), d.get("gain", "?"), d.get("loss", "?"),
                  d.get("swap", "?"), d.get("appswap", "?"),
                  d.get("nullapp", "?"), d.get("first", "?"), d.get("last", "?")))
    if tot["samples"] == 0:
        print("     " + verdict(False, "samples=0 - HdPoll never reached the"
                                       " getAttachedEntity call"))
        return
    if tot["nullapp"]:
        print("     " + verdict(False, "nullapp=%d - getAppearance() returned"
                                       " NULL on a mounted rider, so those"
                                       " frames" % tot["nullapp"]))
        print("               are blind (and gDtApp's filter would have been"
              " blind with them).")
    if tot["appswap"]:
        print("     NOTE  appswap=%d - the rider's Appearance POINTER changed"
              " mid-ride." % tot["appswap"])
        print("           That is a finding on its own: an Appearance rebuild"
              " drops every")
        print("           attachment without any detachItem call, which is"
              " exactly the reading")
        print("           P43DT alone could not distinguish from 'nobody"
              " detached'.  The probe")
        print("           adopts the new pointer so the hook keeps hearing"
              " (HdPollImpl).")
        for line in s.hd_appswap[:4]:
            print("           " + line[:150])
    for d in s.hd_edges[:14]:
        print("     %s %-8s ent=%s prev=%s wih=%s st=%s cm=%s bc=%s" % (
            d.get("_ts", "?"), d.get("_kind", "?"), d.get("ent", "-"),
            d.get("prev", "-"), d.get("wih", "?"), d.get("st", "?"),
            d.get("cm", "?"), d.get("bc", "?")))
    if len(s.hd_edges) > 14:
        print("     ... %d more" % (len(s.hd_edges) - 14))

    last_hd = s.hd_rides[-1] if s.hd_rides else {}
    lastent = (last_hd.get("last") or "").strip("'")
    held = lastent not in ("", "0", "0000000000000000", "00000000", "(null)")
    if tot["loss"] == 0 and held:
        print("     ANSWER  the hand slot still HELD an entity on the last frame"
              " of the ride and")
        print("             never lost it (loss=0).  Paired with the P43DT"
              " reading above, that")
        print("             rules out 'somebody takes it back'.  ⚠️ It does NOT"
              " point at the render")
        print("             side: trip 13 (2026-09-02) fixed the same symptom"
              " with ZERO render-side")
        print("             change, so that arrow is retracted (RE_NOTES"
              " §18.11.1 / §18.12.1).")
        print("             Read the -- T16 -- block above first: an attachment"
              " that exists while")
        print("             the mesh is off screen is what an empty 2nd argument"
              " to drawWeapon")
        print("             looks like, because leaveSheathEquipped whitelists"
              " only 'hip'/'back'")
        print("             and the sheath pipeline never runs.")
    elif tot["loss"]:
        print("     ANSWER  loss=%d - the hand slot DID go empty %d time(s)"
              " while mounted." % (tot["loss"], tot["loss"]))
        print("             Line the loss timestamps up against the P43DT rows"
              " above: a loss with")
        print("             a detach in the same frame names the remover; a loss"
              " with NO detach")
        print("             line means it left some other way (see appswap)."
              "  ⚠️ kHdLossLines=8")
        print("             per ride, so a shortfall of lines is a budget, not"
              " an absence.")
    else:
        print("     ANSWER  loss=0 but the last sample was empty (last=%s) -"
              " the slot was never" % (lastent or "0"))
        print("             seen holding anything, so nothing was there to be"
              " taken away.  That")
        print("             contradicts trip 6's hands=12 attach and is the"
              " first thing to explain.")
    print("  NOTE  naming only (TASK.md P4-3 step 2).  Neither half may become"
          " 'so we call")
    print("        attachItem ourselves' - that is write-side compensation for"
          " an absolute")
    print("        overwrite, HISTORY §B's servo road.  Go kill the writer, or"
          " prove there")
    print("        isn't one - and if there isn't, check what YOU pass to"
          " drawWeapon before")
    print("        you blame the render side (§18.12: the 2nd argument is the"
          " sheath slot).")


def report_p41d(s):

    """P4-3 step 3, naming half: does chooseAttack yield a technique, and what is
    that clip called?  Everything the P41D lever prints is secondary to that one
    question, so the ladder bookkeeping is summarised, not expanded."""
    print("")
    print("== P4-3 step 3 (naming half) - does chooseAttack yield a clip name"
          " (P41D) ==")
    total = (s.p41d_ai + len(s.p41d_read) + len(s.p41d_rung_rows)
             + len(s.p41d_precond) + len(s.p41d_clip_absent)
             + len(s.p41d_clip_unres) + len(s.p41d_clip_rows)
             + len(s.p41d_clip_null))
    if not total:
        print("  no P41D line at all - NOT MEASURED, and that has five innocent"
              " readings:")
        print("    1. diagnostics were off.  The whole lever sits inside"
              " `else if (debugContinuous)`")
        print("       -> Ctrl+NUM. must be ON for the ride (same requirement as"
              " T9).")
        print("    2. the mount was big-tier.  The lever is the small-tier half"
              " of that if;")
        print("       the big half calls endCombatMode()+halt() instead"
              " (elig=1 in the ride table).")
        print("    3. no attacker came within kAtkTryRange=40.0u"
              " (both attacker lists empty, or")
        print("       the nearest was farther - that range gate is P4-1a's fix"
              " for an order at d=971).")
        print("    4. the per-ride budget was already spent"
              " (kAtkTryBudget=20 tries / kAtkReadBudget=60 reads).")
        print("    5. this log predates the P41D build.")
        print("  => none of the five is a probe failure.  Re-ride with"
              " diagnostics on, small mount,")
        print("    and stay in a fight: one full rung cycle needs >= 5 rungs x"
              " kAtkTryGap 75 frames")
        print("    = 375 frames with a target inside 40u.")
        return
    print("  lines: ai=%d read=%d rung=%d precond=%d | clip rows=%d absent=%d"
          " unresolved=%d null=%d"
          % (s.p41d_ai, len(s.p41d_read), len(s.p41d_rung_rows),
             len(s.p41d_precond), len(s.p41d_clip_rows),
             len(s.p41d_clip_absent), len(s.p41d_clip_unres),
             len(s.p41d_clip_null)))
    print("")
    print("  -- did chooseAttack yield? (the one question) --")
    yields = [d for d in s.p41d_read if d.get("ch") == "1"]
    names = {}
    for d in yields:
        nm = d.get("_anim") or "(empty)"
        names[nm] = names.get(nm, 0) + 1
    if s.p41d_read:
        print("     read lines=%d   ch=1 on %d of them"
              % (len(s.p41d_read), len(yields)))
        first = s.p41d_read[0]
        last = s.p41d_read[-1]
        print("     window: %s .. %s" % (first.get("_ts"), last.get("_ts")))
        print("     rider state on the last read: wpn=%s aCW=%s pWpn=%s pCat=%s"
              " cTech=%s"
              % (last.get("wpn"), last.get("aCW"), last.get("pWpn"),
                 last.get("pCat"), last.get("cTech")))
        print("     rider combat:  raw cma=%s tech=%s cst=%s | api in=%s gcs=%s"
              " reach=%s | sR=%s"
              % (last.get("cma"), last.get("tech"), last.get("cst"),
                 last.get("in"), last.get("gcs"), last.get("reach"),
                 last.get("sR")))
    if names:
        for nm in sorted(names, key=lambda k: -names[k]):
            print("     anim='%s'  x%d" % (nm, names[nm]))
        ex = yields[-1]
        print(verdict(True, "chooseAttack DOES yield a technique (ch=1)."
                            " init=%s minS=%s d=%s"
                            % (ex.get("init"), ex.get("minS"), ex.get("d"))))
        print("          => the naming half is answered: that string is the"
              " attack clip's name.")
    elif s.p41d_read:
        print(verdict(False, "every read has ch=0 - chooseAttack refused,"
                             " anim='' on all %d." % len(s.p41d_read)))
        print("          => NOT a probe failure.  RE_NOTES :480 already records"
              " ch=0 43/43 for an")
        print("            unarmed rider, i.e. a consequence, not a mystery:"
              " no weapon in hand => no")
        print("            weapon techniques to choose from.  The fix order"
              " stays 先把武器放回手里")
        print("            (P4-3 steps 1-2), not 'make chooseAttack talk'.")
        armed = [d for d in s.p41d_read if d.get("wpn") == "1"]
        print("          cross-check: wpn=1 on %d of %d reads, pWpn=1 on %d"
              % (len(armed), len(s.p41d_read),
                 len([d for d in s.p41d_read if d.get("pWpn") == "1"])))
        if armed:
            print("          !! ch=0 WITH wpn=1 is the interesting case:"
                  " the weapon is in hand and")
            print("            chooseAttack still refuses => the blocker is"
                  " past the weapon, look at")
            print("            cst=/reach= and at aCW= (SKILL_UNARMED=5 while"
                  " wpn=1 => the anim layer")
            print("            never learned about the weapon).")
    print("")
    print("  -- the weapon gate (whether ch could ever be 1) --")
    if not s.p41d_read:
        print("     no read lines - gate not measured.")
    else:
        n = len(s.p41d_read)
        n_wpn = len([d for d in s.p41d_read if d.get("wpn") == "1"])
        n_pw = len([d for d in s.p41d_read if d.get("pWpn") == "1"])
        n_unarmed = len([d for d in s.p41d_read if d.get("aCW") == "5"])
        cats = sorted(set(d.get("pCat") for d in s.p41d_read
                          if d.get("pWpn") == "1"))
        print("     wpn=1 (getCurrentWeapon) %d/%d | pWpn=1"
              " (getThePreferredWeapon) %d/%d" % (n_wpn, n, n_pw, n))
        print("     aCW=5 (SKILL_UNARMED, the anim layer's view) %d/%d"
              "  | pCat seen on pWpn=1: %s"
              % (n_unarmed, n, ",".join(str(c) for c in cats) or "-"))
        if n_pw and n_unarmed == n and not n_wpn:
            print(verdict(True, "pWpn=1 with a real pCat while aCW stays"
                                " SKILL_UNARMED(5) and wpn=0"))
            print("          => the rider OWNS a weapon that is not in his hands."
                  "  The whole reach=0")
            print("            chain is just a sheathed weapon => rung 0"
                  " (drawWeapon) is the whole fix,")
            print("            and that is the same answer P4-3 steps 1-2 are"
                  " chasing (who re-sheathes /")
            print("            does the HAND slot ever get refreshed).")
        elif n_wpn:
            print(verdict(False, "wpn=1 on %d reads - the weapon IS in hand at"
                                 " least sometimes" % n_wpn))
            print("          => so 'sheathed weapon' does not explain those"
                  " reads.  Compare ch= on exactly")
            print("            those lines: ch=0 while wpn=1 moves the blocker"
                  " past the weapon entirely.")
        else:
            print(verdict(False, "no weapon on either accessor (wpn=0 pWpn=0)"))
            # ⚠️ Cross-section, because this branch used to print "genuinely
            # unarmed" and trip 7 (2026-08-31) disproved it INSIDE THE SAME LOG:
            # P43SH counted real sheathes and P43AT counted hand attaches, so the
            # rider demonstrably owned a weapon and had it in the hand slot.
            # wpn=0 at every read only means it was already back on his back BY
            # THEN.  Each report_* judges one section, so say it here explicitly.
            sh_real = sum(e["real"] for e in s.sh_sites.values())
            if sh_real > 0 or s.at_hands_best > 0:
                print("          !! but NOT unarmed: this same ride logged"
                      " P43SH real=%d sheathe(s) (per-site," % sh_real)
                print("             budgeted - the dismount dump counts more)"
                      " and P43AT hands=%d hand-attach(es)" % s.at_hands_best)
                print("             => the weapon exists and reached the hand"
                      " slot.  wpn=0 on every read")
                print("             means it was ALREADY re-sheathed by the time"
                      " the lever read it => the")
                print("             blocker is the re-sheather (P4-3 step 1's"
                      " site), and ch= yielding only")
                print("             unarmed techniques is the DOWNSTREAM effect of"
                      " that, not an unarmed rider.")
            else:
                print("          => this rider is genuinely unarmed; ch=0 is then"
                      " arithmetic, not a finding.")
                print("            Give the rider a weapon and re-ride before"
                      " reading anything into ch=.")
    print("")
    print("  -- the clip line (premise 2: is that name a record?) --")
    if s.p41d_clip_absent:
        for nm in sorted(set(s.p41d_clip_absent)):
            print("     '%s' ABSENT in allAnims" % nm)
        print(verdict(True, "ABSENT confirms the offline finding in-engine"))
        print("          => tools\\gamedata.py found NO ANIMATION/ANIMAL ANIM"
              " record for 43 of the 44")
        print("            COMBAT TECHNIQUE clip names; allAnims.find() missing"
              " it is the runtime half")
        print("            of the same fact.  !! HARD CONSEQUENCE: such a name"
              " must NEVER be handed to")
        print("            getAnimationData() - that is operator[] semantics and"
              " a miss plants a")
        print("            permanent NULL in the engine's own allAnims."
              "  FindAnimData() only.")
        print("          => and 'layer'/'wholeBodyAllLayer' are RECORD fields, so"
              " for these clips those")
        print("            two bits do not exist anywhere in data\\ =>"
              " 判层只能钉上去实测.")
    if s.p41d_clip_unres:
        for nm in sorted(set(s.p41d_clip_unres)):
            print("     '%s' unresolved (no list)" % nm)
        print(verdict(False, "the human anim list itself was unreachable -"
                             " says nothing about the clip"))
    if s.p41d_clip_rows:
        for ln in s.p41d_clip_rows[:6]:
            print("     %s" % ln)
        print(verdict(True, "the name RESOLVED to a record - read lay= and"
                            " flags= off that row"))
        print("          => that answers the layer question directly:"
              " lay=UPPER + no 'whole' would be")
        print("            pinnable beside the hand-controlled legs;"
              " 'whole' means it presses the")
        print("            whole skeleton and P4-3 has to fight it"
              " (see CLAUDE.md 人形动画表).")
        print("          !! still 静态字段: it predicts nothing about runtime"
              " weight - pin it and look.")
    if s.p41d_clip_null:
        for ln in s.p41d_clip_null[:6]:
            print("     %s" % ln)
        if len(s.p41d_clip_null) > 6:
            print("     ... %d more" % (len(s.p41d_clip_null) - 6))
        print(verdict(False, "find() HIT but the record pointer is NULL - this is"
                             " NOT a resolved record"))
        print("          => the call site only reaches this row when"
              " allAnims.find() succeeded, so a")
        print("            NULL value means the engine's own map CARRIES THAT KEY"
              " WITH A NULL VALUE")
        print("            = a poisoned entry (getAnimationData() is operator[]"
              " semantics; see")
        print("            CLAUDE.md 关键机制).  The engine's combat code queried"
              " the technique clip")
        print("            name itself and planted it.  That is a finding, not a"
              " pass:")
        print("            'ABSENT' = never queried, this = queried already and"
              " left as a NULL.")
        print("          => our side is safe by construction (FindAnimData"
              " returns mi->second and every")
        print("            caller pointer-tests it), but the layer question stays"
              " UNANSWERED for these")
        print("            names - there is no record to read lay=/flags= off.")
    if not (s.p41d_clip_absent or s.p41d_clip_unres or s.p41d_clip_rows
            or s.p41d_clip_null):
        print("     no clip line at all.")
        if not yields:
            print("     => EXPECTED, not a failure: the clip line is guarded by"
                  " `if (chAnim[0] && ...)`,")
            print("       i.e. it only prints when chooseAttack actually"
                  " returned a technique (ch=1),")
            print("       and every read above has ch=0.")
        else:
            print("     !! ch=1 was seen but no clip line printed => its second"
                  " guard, `lastChAnim !=")
            print("       chAnim`, was already satisfied.  That static is"
                  " FUNCTION-LOCAL and neither")
            print("       per-ride reset block clears it => one line per distinct"
                  " name PER DLL LOAD.")
            print("       'Ride twice to be sure' will NOT reproduce it -"
                  " restart the game instead,")
            print("       and never read this silence as failure.")
    print("")
    print("  -- the ladder (bookkeeping only; rung attribution) --")
    RUNGS = {0: "drawWeapon",
             1: "focused _NV_initCombatMode(tgt,0,true)",
             2: "CcSetPtr(rcc,0x150,chTech)",
             3: "runCombatAnimation(chTech,1.0,'')",
             4: "changeState(CHOP_WEAPON,0.0)"}
    dead = {}
    for ln in s.av:
        m = re.search(r"access violation in rung (\d+)", ln)
        if m:
            r = int(m.group(1))
            dead[r] = dead.get(r, 0) + 1
    for r in sorted(RUNGS):
        n = s.p41d_rungs.get(r, 0)
        tag = ""
        if r in dead:
            tag = "  <- DISARMED by AV x%d (gRungDead[%d]) => untried after that" \
                  % (dead[r], r)
        elif not n:
            tag = "  <- never reached (budget/cycle, NOT a result)"
        print("     rung %d  x%-4d %-38s%s" % (r, n, RUNGS[r], tag))
    if s.p41d_abandoned:
        print(verdict(False, "'all rungs disarmed - ladder abandoned' was"
                             " printed"))
        print("          => every rung took an AV; nothing about P4-3 was tested"
              " after that point.")
    if s.p41d_precond:
        last = s.p41d_precond[-1]
        print("     precond x%d (last: tgt=%s atk=%s icm=%s)"
              % (len(s.p41d_precond), last.get("tgt"), last.get("atk"),
                 last.get("icm")))
    if s.p41d_ai:
        print("     ai x%d (the P41B-era AI-layer view, kept as a regression"
              " check)" % s.p41d_ai)
    if s.p41d_rung_rows:
        last = s.p41d_rung_rows[-1]
        print("     last rung readback: rung=%s %s"
              % (last.get("rung"),
                 " ".join("%s=%s" % (k, last[k]) for k in
                          ("tries_left", "d", "cma", "tech", "reach", "inZ",
                           "nearZ", "cst", "wpn")
                          if k in last)))
    for ln in s.av:
        if "P41D access violation outside a rung" in ln:
            print(verdict(False, "an AV landed OUTSIDE a rung (read block or"
                                 " readback)"))
            print("          => that zeroes gAtkTries/gAtkReads => the lever"
                  " stopped for the rest of the")
            print("            ride.  Unlike a per-rung AV this one is worth"
                  " stopping for (source note")
            print("            at the __except handler says so) - find the bad"
                  " offset before re-riding.")
            break
    print("")
    print("  NOTE  sR= is a SYNTHETIC reach handed to chooseAttack as an"
          " argument (its own reach")
    print("        when non-zero, else the enemy's, else 9.0), and the returned"
          " pointer is")
    print("        deliberately NOT installed here.  So ch=1 does NOT prove the"
          " engine would pick")
    print("        a technique unaided - installing it is rung 2's job, kept"
          " separate so the")
    print("        ladder stays attributable.")
    print("  NOTE  naming only.  A clip name is not a fix: 判层的字段对这 43 条"
          " 根本不存在, so the")
    print("        remaining half of step 3 is 钉上去实测 (does the skeleton"
          " move, how does it press")
    print("        against guard 1h and the hand-controlled thighs) - not"
          " 'drive every swing")
    print("        ourselves', which is HISTORY §B's servo road.")


def stance_windows(s):
    """[(enter_ts, exit_ts_or_None)] from the STANCE transitions, by timestamp.

    T14-A needs this: the suppressor only fires INSIDE a stance, while the
    P4-1e draw ladder only arms once the hands are EMPTY - which, with
    suppression on, is exactly outside the stance.  Where a ladder run started
    is therefore the whole difference between "n= is inconclusive by
    construction" and "a second writer is emptying the hands under our nose".
    STANCE f= counts the global frame, so this correlation must go by timestamp.
    """
    wins = []
    prev = None
    for row in s.stance:
        st, t = row[0], tsnum(row[6])
        if st == prev:
            continue
        prev = st
        if t is None:
            continue
        if st == "1":
            wins.append([t, None])
        elif wins and wins[-1][1] is None:
            wins[-1][1] = t
    return [(a, b) for a, b in wins]


def report_stance_terms(s):
    """T18: which of RideStanceRaw's three terms refused, per ride.

    Read this BEFORE T14-A / T14-B.  Those two only ever say "nothing
    happened"; this says why.  Trip 14 (2026-09-02, diag OFF) is the reason it
    exists: two fights with drawn=0 / real=0 and no way to tell from the log
    whether the stance was blocked at the size gate, the combat term or the
    threat search, because STANCE itself is diagnostics-gated.
    """
    print("")
    print("== T18 - why the stance did or did not arm (P43FT) ==")
    if not s.ft_rides:
        print("  no P43FT line.  It was UNGATED and printed one per ride, so its")
        print("  absence is NOT a diagnostics-off artefact.  TWO readings now, and")
        print("  the P43RD rows below tell them apart:")
        print("    - the instrument was REMOVED in the probe-free build 304128 B /")
        print("      md5 1792B72E... (T19, 2026-09-02).  It had answered: the stance")
        print("      DOES arm on the mount's own books (trip 15 ok=2182 / ok=2585,")
        print("      noElig=0, dmin=7.6/8.4).  On such a log judge the stance from")
        print("      T14-A/T14-B alone - real>0 and post=1 are the surviving fields.")
        print("      Silence here is EXPECTED; check the DLL's md5 before reading")
        print("      anything into it.")
        print("    - or this log predates the T18 build (312832 B / E83DB50D...),")
        print("      where the combat term was still rider->isInCombatMode(true,true)")
        print("      and a player build never reached it (RE_NOTES 17.7): expect")
        print("      drawn=0 / real=0 with no field to explain it.")
        print("  Discriminator: a T19 log still has P43RD edges with post=1; a pre-T18")
        print("  log has drawn=0 on every ride.")
        return
    print("  ok=armed  noElig=size gate  noFight=RideFightIsOn  noThr=no live")
    print("  threat  far=threat beyond kRideThreatDist(60)  dmin=closest seen")
    armed = 0
    for i, d in enumerate(s.ft_rides, 1):
        print("    ride %d: ok=%s noElig=%s noFight=%s noThr=%s far=%s dmin=%s"
              % (i, d.get("ok"), d.get("noElig"), d.get("noFight"),
                 d.get("noThr"), d.get("far"), d.get("dmin")))
        if fnum(d, "ok", 0) > 0:
            armed += 1
    if armed == len(s.ft_rides):
        print("  PASS   every ride armed the stance at least once.  The remaining")
        print("         half of T18 is T14-A/T14-B below (real>0, drawn>0) plus the")
        print("         eyeball: weapon out during the fight, combat pose not the")
        print("         seat pose.  ok>0 alone does NOT close T18.")
        return
    if armed:
        print("  CHECK  %d of %d rides armed it.  A ride spent entirely out of"
              % (armed, len(s.ft_rides)))
        print("         combat is normal - only judge rides that contained a fight.")
    else:
        print("  FAIL   no ride armed the stance.  The dominant zero-reason above")
        print("         is the next thing to fix:")
    # Point at the dominant refusal across all rides, which is the actionable bit.
    tot = {}
    for k in ("noElig", "noFight", "noThr", "far"):
        tot[k] = sum(fnum(d, k, 0) for d in s.ft_rides)
    worst = max(tot, key=lambda k: tot[k])
    if tot[worst] <= 0:
        return
    if worst == "noElig":
        print("         noElig dominates = MountCombatEligible refused.  Read the"
              " mount")
        print("         line's size=/rad=/elig= (T9): this is the size gate doing"
              " its")
        print("         job on a big mount, not a T18 regression.")
    elif worst == "noFight":
        print("         noFight dominates = RideFightIsOn is still false, i.e. the")
        print("         MOUNT's books are empty too.  That would contradict trip 13")
        print("         (mTgt=1 / mAtk=6 at 202.263, 3.3 s before any lever press),")
        print("         so check the mount line's mAtk= and P3CMB mTgt= first.")
    elif worst == "noThr":
        print("         noThr dominates = a fight is on but RideNearestThreat found")
        print("         nobody: every candidate was NULL, isDown() or isDead().  The")
        print("         mount tiers are the new ones - see RideNearestThreat.")
    else:
        print("         far dominates = threats WERE found but all beyond 60u."
              "  dmin")
        print("         above is the closest one; kRideThreatDist is the knob, and"
              " a")
        print("         dmin just over 60 means the fight was real and out of reach.")


def report_suppress(s):
    """T14-A (was T13-A): did the sheathe suppressor actually take a weapon away?"""
    print("")
    print("== T14-A - sheathe suppression (P43SUP) ==")
    if s.sh_hookfail:
        print("  CHECK the hook did not install"
              " ('Could not hook CharacterHuman::sheatheWeapon!').")
        print("  Nothing below can mean anything: the engine body ran every time.")
        print("  This ErrorLog is shared with the retired step-1 probe - on a"
              " 302080 B+")
        print("  log it is about the SUPPRESSOR.")
        return
    entries = 0
    prev = None
    for row in s.stance:
        if row[0] != prev:
            if row[0] == "1":
                entries += 1
            prev = row[0]
    if not s.sup_rides and not s.sup_skips:
        print("  no P43SUP line at all.  Neither shape is diagnostics-gated, so"
              " this is")
        print("  NOT the usual 'Ctrl+NUM. was off'.  Three readings:")
        print("    - this log predates DLL 302080 B (no suppressor in the build);")
        print("    - sheatheWeapon was never called on a tracked rider;")
        print("    - the state filter rejected every call (riderToMount ->"
              " mountSeat ->")
        print("      RideCombatStance).  The third is the one worth chasing, and"
              " the")
        print("      tell is STANCE: entries(->1)=%d." % entries)
        if entries:
            print("      FIRST THING TO LOOK AT: entries>0 with zero P43SUP means"
                  " the")
            print("      rider was in stance and nothing tried to sheathe -"
                  " re-read which")
            print("      writer P43SH named, because the suppressor only covers"
                  " the")
            print("      virtual, not a direct bone/appearance write.")
        return
    real = sum(int(fnum(d, "real", 0) or 0) for d in s.sup_rides)
    noop = sum(int(fnum(d, "noop", 0) or 0) for d in s.sup_rides)
    npass = sum(int(fnum(d, "pass", 0) or 0) for d in s.sup_rides)
    if s.sup_rides:
        print("  %d per-ride summary line(s) (printed at dismount):"
              % len(s.sup_rides))
        for d in s.sup_rides:
            print("    %s real=%s noop=%s pass=%s | P43RD drawn=%s fail=%s"
                  " nowpn=%s"
                  % (d.get("_ts"), d.get("real"), d.get("noop"), d.get("pass"),
                     d.get("drawn"), d.get("fail"), d.get("nowpn")))
    else:
        # The summary only prints on dismount; skip lines print during the ride.
        print("  %d skip line(s) but NO per-ride summary - the ride never ended"
              " with a" % len(s.sup_skips))
        print("  dismount in this log.  Counting the skip lines instead is wrong:"
              " they are")
        print("  budgeted at kShSupLines=10 per ride, so they saturate and"
              " understate.")
        for d in s.sup_skips:
            r = int(fnum(d, "real", 0) or 0)
            if r > real:
                real = r
            n = int(fnum(d, "noop", 0) or 0)
            if n > noop:
                noop = n
        print("  highest running counter seen in those lines:"
              " real=%d noop=%d" % (real, noop))
    # THE gate.  real= counts suppressed calls whose PRE-state had a weapon in
    # the hands, i.e. calls that would really have emptied them.  real=0 means
    # the fix never bore load, and then "the weapon stayed in hand" proves
    # nothing at all - identical discipline to late=0 invalidating the by-name
    # mask rescue (RE_NOTES 21).
    print("  " + verdict(real > 0,
                         "real=%d suppressed call(s) really had a drawn weapon"
                         " to lose" % real))
    if real == 0:
        print("        real=0 => THE FIX NEVER BORE LOAD.  Whatever the eyeball"
              " half saw")
        print("        this trip, it was not caused by this change.  Reference:"
              " the")
        print("        step-1 probe measured real=16 for _ragdollMode on one"
              " ride.")
        if noop:
            print("        noop=%d with real=0 = the calls we swallowed were"
                  " empty ones." % noop)
            print("        FIRST THING TO LOOK AT: the real writer is not this"
                  " virtual.")
    if npass:
        print("  NOTE pass=%d call(s) were deliberately let through (tracked"
              " rider, but" % npass)
        print("       not in stance).  Non-zero is the EXPECTED shape: the"
              " mount-time")
        print("       _carryMode sheathe must keep working, and the 1200 ms"
              " stance tail")
        print("       is what puts the weapon back afterwards.  pass=0 with"
              " real>0 would")
        print("       mean the gate is wider than designed.")
    if s.sup_skips:
        wih1 = sum(1 for d in s.sup_skips if d.get("wih") == "1")
        print("  skip samples: %d line(s), %d with wih=1 (budget kShSupLines=10"
              " per ride)" % (len(s.sup_skips), wih1))
        for d in s.sup_skips[:6]:
            print("    %s wih=%s sh=%s cm=%s bc=%s"
                  % (d.get("_ts"), d.get("wih"), d.get("sh"),
                     d.get("cm"), d.get("bc")))
        if s.sup_skips and wih1 == 0:
            print("        every sampled call had wih=0 - see the real=0 note"
                  " above; the")
            print("        sample is budgeted, so trust the ride summary's"
                  " real= over this.")
    # Second self-proving field: P4-1e's forced-draw ladder should stop being
    # spent once nothing re-sheathes.  Both this and the ladder are
    # diagnostics-gated, so absence here is silence, not a zero.
    if not s.draw_n:
        print("  no P41E draw line - the forced-draw ladder never ran"
              " (diagnostics off,")
        print("  or no equipped weapon in the slots).  The second self-proving"
              " field is")
        print("  simply unavailable this trip; judge on real= alone.")
        return
    nmax = max(n for n, _l, _a, _b, _t in s.draw_n if n is not None)
    # Group the rows into runs so the START of each ladder can be placed against
    # the stance windows.  A run breaks on n resetting to 1 or a >2 s gap.
    runs = []
    for row in s.draw_n:
        n, t = row[0], tsnum(row[4])
        newrun = (not runs or n == 1)
        if not newrun and t is not None:
            pt = tsnum(runs[-1][-1][4])
            if pt is not None:
                newrun = (t - pt) > 2.0
        if newrun:
            runs.append([])
        runs[-1].append(row)
    wins = stance_windows(s)

    def where(t):
        if t is None:
            return None
        for a, b in wins:
            # Half-open on purpose: a ladder run that starts in the SAME frame as
            # the 1->0 edge started after the gate closed, not inside it.
            if t >= a and (b is None or t < b):
                return True
        return False

    # Nearest stance transition of ANY kind, windows or not: a log can open with a
    # bare 'STANCE 0' (no matching entry), and that edge still explains a run.
    edges = []
    eprev = None
    for row in s.stance:
        if row[0] != eprev:
            eprev = row[0]
            et = tsnum(row[6])
            if et is not None:
                edges.append((et, row[0]))

    inside = [r for r in runs if where(tsnum(r[0][4])) is True]
    print("  P41E draw lines=%d in %d run(s)  highest n=%d  (budget"
          " kDrawTryBudget=12," % (len(s.draw_n), len(runs), int(nmax)))
    print("        one rung per kDrawTryGap=10 frames; since 301568 B a SUCCESSFUL"
          " P43RD")
    print("        edge also spends one unit, so left= can step without a rung"
          " here)")
    for r in runs[:6]:
        t = tsnum(r[0][4])
        near = None
        for et, est in edges:
            if t is None:
                continue
            dd = abs(t - et)
            if near is None or dd < near[0]:
                near = (dd, est)
        print("    run @%s  n=%s..%s  %s  nearest STANCE edge: %s"
              % (r[0][4],
                 "?" if r[0][0] is None else int(r[0][0]),
                 "?" if r[-1][0] is None else int(r[-1][0]),
                 "INSIDE stance" if where(t) is True else "outside stance",
                 "-" if near is None
                 else "-> %s, %.3f s away" % (near[1], near[0])))
    # THE reading.  n= climbing to the budget is only evidence of a second
    # writer if the climb happened while the gate was open; a ladder that runs
    # entirely outside the stance is disjoint from the suppressor by
    # construction and says nothing either way.  (Measured trip 10: all three
    # runs started within ~160 ms of a STANCE -> 0.)
    print("  " + verdict(not inside or nmax <= 2,
                         "no ladder run climbed inside a stance window"
                         " (inside=%d/%d)" % (len(inside), len(runs))))
    if not inside:
        print("        => n=%d is INCONCLUSIVE, not a failure: the ladder only"
              " arms on" % int(nmax))
        print("        empty hands, which under suppression happens only after"
              " the stance")
        print("        ends.  Judge half A on real= alone, and do not read this"
              " as a")
        print("        second writer.")
    elif nmax > 2:
        print("        a run climbed WHILE the gate was open => something else"
              " empties")
        print("        the hands inside the stance.  With real>0 that is a"
              " SECOND writer")
        print("        outside this virtual; with real=0 the suppressor never"
              " ran at all.")
    late = [row for row in s.draw_n if row[3] == "0"]
    if late:
        print("  NOTE %d draw line(s) ended wih=..->0: the draw itself did not"
              " leave a" % len(late))
        print("       weapon in the hands (post-state read in the same frame)."
              " That is a")
        print("       drawWeapon problem, not a sheathe problem - keep the two"
              " apart.")


def report_redraw(s):
    """T14-B: does the 0 -> 1 stance edge put a blade back into an empty hand?"""
    print("")
    print("== T14-B - stance-edge re-draw (P43RD) ==")
    if not s.rd:
        print("  no P43RD line.  This one is NOT diagnostics-gated (it changes"
              " game state,")
        print("  same discipline as 'force dismount' and P43SUP), so absence is a"
              " reading,")
        print("  not silence.  Four of them:")
        print("    - this log predates DLL 301568 B (no re-draw in the build);")
        print("    - no 0 -> 1 stance edge ever happened (never in combat mode, or"
              " no threat")
        print("      inside kRideThreatDist while mounted);")
        print("    - every edge found getCurrentWeapon() non-NULL, i.e. the rider"
              " came into")
        print("      the stance ALREADY armed and the suppressor kept it there -"
              " that is")
        print("      trip 10's real=41 / real=16 clusters, and it is the GOOD"
              " case, not a miss;")
        print("    - the rider had nothing in the weapon slots at all (nowpn).")
        print("  The P43SUP ride line separates them: drawn= / fail= / nowpn= are"
              " ungated")
        print("  and printed at every dismount.  All three zero WITH a stance in"
              " the log")
        print("  means no edge ever reached the draw.")
        return
    print("  %d line(s) (budget kStanceDrawLines=8 per ride).  Edges are rationed"
          " by the" % len(s.rd))
    print("        release tail, not by a budget: kRideStanceHoldMs=1200 ms has to"
          " drain")
    print("        before the stance can fall to 0, so ~1 edge per 1.2 s is the"
          " ceiling.")
    for d in s.rd[:8]:
        print("    %s post=%s wih=%s aCW=%s cm=%s ok=%s fail=%s left=%s f=%s"
              % (d.get("_ts"), d.get("post"), d.get("wih"), d.get("aCW"),
                 d.get("cm"), d.get("ok"), d.get("fail"), d.get("left"),
                 d.get("f")))
        print("          sheath '%s' -> '%s'"
              % (d.get("_shpre"), d.get("_shpost")))
    # THE gate.  post= is getCurrentWeapon() read straight after our drawWeapon
    # returned, so post=0 on every line means the engine refused the draw inside
    # the stance and the change bore no load - identical discipline to real=0 for
    # the suppressor and late=0 for the by-name mask rescue (RE_NOTES 21).
    okn = len([d for d in s.rd if d.get("post") == "1"])
    print("  " + verdict(okn > 0,
                         "post=1 on %d/%d edge(s) - the re-draw really put a"
                         " weapon in the hand" % (okn, len(s.rd))))
    if not okn:
        print("        post=0 everywhere => THE RE-DRAW BORE NO LOAD.  Whatever"
              " the eyeball")
        print("        half saw this trip, it was not caused by this change."
              "  Reference:")
        print("        P4-1h measured post=1 on 12/12 for the same call made"
              " OUTSIDE a")
        print("        stance, so a stance-only refusal IS the finding - write it"
              " down, do")
        print("        not answer it by calling more often (HISTORY §B).")
    # wih= is CharacterHuman::weaponInHands across the call, post= is
    # getCurrentWeapon().  They are different fields and can disagree: post=1 with
    # wih=..->0 is a weapon the engine considers current but has not put in the
    # hand yet, which is the same distinction P4-1h had to keep apart.
    split = []
    for d in s.rd:
        pre, _, wpost = (d.get("wih") or "").partition("->")
        split.append((d, pre, wpost))
    halfway = [t for t in split if t[0].get("post") == "1" and t[2] == "0"]
    if halfway:
        print("  NOTE %d line(s) ended post=1 wih=..->0: getCurrentWeapon() answers"
              " yes while" % len(halfway))
        print("       weaponInHands is still empty in that same frame.  Read the"
              " NEXT frame's")
        print("       P41K/DBG rows before calling it a failure - the attach may"
              " land one")
        print("       frame later - but if the blade stays on the back on screen,"
              " THIS is the")
        print("       field that said so first.")
    armed_pre = [t for t in split if t[1] == "1"]
    if armed_pre:
        print("  CHECK %d line(s) started wih=1: the hands were NOT empty, yet"
              " getCurrentWeapon()" % len(armed_pre))
        print("        was NULL (the function returns before this point"
              " otherwise).  Two")
        print("        readings of 'armed' disagreeing is worth a look, not a"
              " pass.")
    # aCW is animationRequirements.currentWeapon, the ANIMATION layer's view of
    # what the rider holds (5 = SKILL_UNARMED, the value P4-1d read on every
    # sheathed rider).  A draw that leaves it at 5 armed the hand and not the
    # animation, which is exactly what 「空手进架势仍空手」 looks like from here.
    unarmed = [d for d in s.rd if d.get("post") == "1" and d.get("aCW") == "5"]
    if okn:
        print("  " + verdict(not unarmed,
                             "aCW came off SKILL_UNARMED(5) on every successful"
                             " edge (%d/%d still at 5)" % (len(unarmed), okn)))
        if unarmed:
            print("        aCW=5 with post=1 => the blade is current but the anim"
                  " layer never")
            print("        learned about it.  Same field as the P4-1d weapon gate,"
                  " and the")
            print("        reason a technique can still refuse to fire.")
    # cm= is the RIDER's isInCombatMode(true,true).  Before the T18 fix
    # RideStanceRaw REQUIRED it, so cm=0 on an edge meant a latch fault.  After the
    # fix the combat term is RideFightIsOn(rider, mount) - an OR the MOUNT's own
    # target/attacker list can satisfy alone - and T17 proved the rider's flag is
    # permanently 0 in a player build (mounting drops aggro, the enemy never
    # retargets onto the rider).  So cm=0 is now the EXPECTED shape and cm=1 is the
    # notable one.  Telling the two builds apart takes TWO tests, because P43FT
    # shipped WITH the fix and was subtracted again for T19 (304128 B / md5
    # 1792B72E...): P43FT present => post-fix, and P43FT absent BUT edges with
    # post=1 => ALSO post-fix.  That second test is this file's own T19
    # discriminator (printed in the T18 section above): a pre-T18 player build
    # never re-drew at all, so it reaches this line with drawn=0 on every ride
    # and no edge to judge.  Byte size cannot do it here because 312832 collides
    # with two pre-fix backups (_prev_E7613634, _prev_544D8C86) - CLAUDE.md's
    # "identity is md5".
    nocm = [d for d in s.rd if d.get("cm") == "0"]
    if s.ft_rides or okn:
        why = ("P43FT is present" if s.ft_rides else
               "P43FT is gone yet %d edge(s) landed post=1 (a T19 build)" % okn)
        print("  NOTE  %d/%d edge(s) fired with cm=0, which is EXPECTED on this"
              " build:" % (len(nocm), len(s.rd)))
        print("        %s => the stance's combat term is"
              " RideFightIsOn(rider," % why)
        print("        mount), satisfied by the MOUNT's attack target or attacker"
              " list on its")
        print("        own (RE_NOTES §17.7).  The rider's own flag staying 0 all"
              " fight IS the")
        print("        T17 finding, not a fault.  cm=1 here would mean something"
              " wrote the")
        print("        rider into combat mode - RiderCombatLever, or a third"
              " party.")
    else:
        print("  " + verdict(not nocm,
                             "every edge fired with cm=1 (%d/%d)"
                             % (len(s.rd) - len(nocm), len(s.rd))))
        if nocm:
            print("        pre-T18 build (no P43FT line, and not one edge landed"
                  " post=1): cm=0")
            print("        on an edge is a LATCH fault, not a draw fault -"
                  " RideStanceRaw could")
            print("        not return 1 without combat mode.  Look at"
                  " gStanceDrawWho/Prev - a")
            print("        rider swap resets the latch and can re-fire on a stance"
                  " already up.")
    # The failure cap.  kStanceDrawFails=6 refusals disarm the re-draw for the
    # REST OF THAT RIDE, deliberately: re-issuing an equipment mutation forever on
    # a refusal is still a servo, just on a slower clock (HISTORY §B).
    fmax = 0
    for d in s.rd:
        f = int(fnum(d, "fail", 0) or 0)
        if f > fmax:
            fmax = f
    if fmax:
        print("  " + verdict(fmax < 6,
                             "refusals stayed under the cap (highest fail=%d of"
                             " kStanceDrawFails=6)" % fmax))
        if fmax >= 6:
            print("        cap reached => the re-draw is DISARMED for the rest of"
                  " that ride, so")
            print("        later empty-handed stances in this log say nothing about"
                  " the fix.")
    # The user's ruling 「补拔失败不消耗 kDrawTryBudget，失败退回梯子」 is
    # log-verifiable: left= is gDrawTries printed AFTER our own decrement, and the
    # only other spender is the P41E ladder, which prints one left= line per rung.
    # So between two P43RD lines left must fall by exactly (ladder lines in
    # between) + (1 if the later edge succeeded, else 0).
    # ONLY WITHIN ONE RIDE, though: Mount() re-arms gDrawTries = kDrawTryBudget
    # (RidingPlugin.cpp:5355 and :5637), so a transition that straddles a dismount
    # is comparing two different budgets.  lb > la catches that only when the
    # re-arm happens to show up as an increase - if the previous ride spent as much
    # as the next ride's first edge (trip 16: 11 -> 11) it looks identical to a
    # refused edge that stole a unit.  So cut the sequence at the P43SUP ride
    # lines, which print one per dismount and are ungated.
    ladder_ts = sorted(t for t in (tsnum(r[4]) for r in s.draw_n) if t is not None)
    ride_end_ts = sorted(t for t in (tsnum(d.get("_ts")) for d in s.sup_rides)
                         if t is not None)
    bad = []
    skipped = 0
    for i in range(1, len(s.rd)):
        a, b = s.rd[i - 1], s.rd[i]
        la, lb = fnum(a, "left"), fnum(b, "left")
        if la is None or lb is None:
            continue
        if lb > la:
            continue          # gDrawTries re-armed => a new ride (Mount / load)
        ta, tb = tsnum(a.get("_ts")), tsnum(b.get("_ts"))
        if (ta is not None and tb is not None
                and [t for t in ride_end_ts if ta < t <= tb]):
            skipped += 1
            continue          # a dismount sits between them => different budgets
        k = 0
        if ta is not None and tb is not None:
            k = len([t for t in ladder_ts if ta < t <= tb])
        want = la - k - (1 if b.get("post") == "1" else 0)
        if want < 0:
            want = 0
        if int(lb) != int(want):
            bad.append((b.get("_ts"), int(la), int(lb), int(want), k,
                        b.get("post")))
    if len(s.rd) >= 2:
        print("  " + verdict(not bad,
                             "kDrawTryBudget accounting matches the ruling (%d"
                             " transition(s) checked)"
                             % (len(s.rd) - 1 - skipped)))
        if skipped:
            print("        (%d transition(s) straddled a dismount and were skipped:"
                  " Mount()" % skipped)
            print("        re-arms the budget, so those two edges are not on the"
                  " same 12.)")
        for row in bad[:4]:
            print("        @%s left %d -> %d, expected %d (post=%s, %d ladder"
                  " line(s) between)"
                  % (row[0], row[1], row[2], row[3], row[5], row[4]))
        if bad:
            print("        A REFUSED edge that spent budget is the ruling being"
                  " broken.  An")
            print("        unexplained drop with diagnostics ON may still be the"
                  " ladder (its")
            print("        lines are counted above); with diagnostics OFF nothing"
                  " else spends it.")
        lefts = [x for x in (fnum(d, "left") for d in s.rd) if x is not None]
        refused = len([d for d in s.rd if d.get("post") == "0"])
        if not bad and lefts and max(lefts) == 0 and refused == 0:
            print("        INCONCLUSIVE though: every left= is 0 and no edge was"
                  " refused, so")
            print("        neither half of the ruling actually got tested."
                  "  gDrawTries was")
            print("        already spent before the first edge - with diagnostics ON"
                  " the P41E")
            print("        ladder burns all 12 rungs at mount (see the runs above);"
                  " with them")
            print("        OFF the first successful edge should print left=11."
                  "  And 'a refusal")
            print("        spends nothing' needs one post=0 edge before it can be"
                  " read at all.")
    # Frame correlation, by frame number rather than by timestamp.  The latch is
    # armed in HaltAndForceSitPass and consumed in CombatAndForceDismountPass, and
    # both lines print gP3Frames - but the counter ticks BETWEEN those two passes,
    # so the measured shape (trip 11, 4/4: 12319->12320, 12849->12850, 13756->13757,
    # 15666->15667, +1 ms each) is f_edge = f_stance + 1, not equality.  Accept both
    # frames.  STANCE is diagnostics-gated, so this is a bonus check, never the
    # verdict - post= is.
    st1 = set(row[1] for row in s.stance if row[0] == "1")
    if st1:
        okf = set(st1)
        for f in st1:
            try:
                okf.add(str(int(f) + 1))
            except (TypeError, ValueError):
                pass
        miss = [d for d in s.rd if d.get("f") not in okf]
        print("  " + verdict(not miss,
                             "every re-draw sits on a STANCE 1 frame or the one"
                             " after it (%d/%d)"
                             % (len(s.rd) - len(miss), len(s.rd))))
        if miss:
            print("        an f= that is neither a STANCE 1 frame nor f+1 means the"
                  " edge did not")
            print("        come from a 0 -> 1 transition this log recorded: look for"
                  " a second")
            print("        tracked rider (the latch is single-slot, keyed on"
                  " gStanceDrawWho).")
    else:
        print("  no STANCE row - the frame cross-check needs continuous"
              " diagnostics")
        print("  (Ctrl+NUM.).  post= above does not, so the verdict stands without"
              " it.")
    if s.act_over:
        print("  NOTE act>1.02 on %d frame(s) this trip.  Two known causes now"
              " that the" % s.act_over)
        print("       swing window is out of the build: the stance handover (trip"
              " 4/6/9) and")
        print("       the mount fade-in overlap (trip 8).  Anything else is new -"
              " tell them")
        print("       apart by timestamp.")


def report_swing(s):
    """T20 - P4-3-4: did the rider actually swing, and did the guard stand down?

    Three inherited pieces of judging discipline, all of them load-bearing:
      * swing=0 for a ride means NO swing was ever fired, so a "saw nothing"
        observation says nothing about this route - the same way real=0 voids the
        suppressor's reading and late=0 voids the mask rescue's (RE_NOTES 21).
      * guardoff=0 together with swing>0 means the window opened but the guard
        assertion never stood down, i.e. the swing played UNDERNEATH the stance.
        That is the exact shape trip 13 measured before this phase existed (guard
        play=1 w=1.000 with mainState 1.000 -> 0.909 right after rung 3), and it
        is the one failure this design is built to avoid.
        ⚠️ T27 KEEPS THE COUNTER AND DROPS THE MEANING: it prints hostkeep= for
        frames the window kept the GUARD as host (nothing stands down any more),
        so what >0 proves on a T27 log is only "the window reached the pin sites".
        The read goes through hostkeep(); the LABEL is whatever the row carries.
      * "did the stance come back" may ONLY be judged on the NEXT open row, never
        on the frames just after a close: the dying one-shot is still inside
        ClipPin's `others`, so for ~0.3 s the guard's ms structurally cannot climb
        back.  That was trip 10's judging-criterion error (HISTORY §U).
        (On T27 there is no dying one-shot at all - see -- T27 --.)
    """
    print("")
    print("== T20 - P4-3-4: the swing (P43SW) ==")
    if not (s.sw_rides or s.sw_open or s.sw_close):
        if s.sw_legacy:
            print("  %d line(s) of the TRIP-10 experiment window (P43SW open/hold/"
                  "close/after)." % s.sw_legacy)
            print("  That code was deleted on the user's ruling once trip 10"
                  " answered it, so this")
            print("  log predates P4-3-4.  Its findings are archived in HISTORY"
                  " §U - this tool")
            print("  deliberately does not re-judge them (the old line repeats"
                  " field names for")
            print("  two clips, which kv() cannot read without splitting on"
                  " ' | guard ' first).")
            return
        print("  no P43SW line at all: this log predates the swing (DLL 308224 B"
              " and later).")
        print("  NOT a failure - there is simply nothing to judge here.")
        return

    t27 = is_t27(s)
    hk_lbl = "hostkeep" if t27 else "guardoff"
    tot = dict(swing=0, tech=0, skip=0, noclip=0)
    tot_hk = 0
    print("  %d ride summary line(s) (ungated, one per dismount):" % len(s.sw_rides))
    for d in s.sw_rides:
        for k in tot:
            v = fnum(d, k, 0)
            tot[k] += int(v) if v is not None else 0
        hk = hostkeep(d, 0)
        tot_hk += int(hk) if hk is not None else 0
        print("    %8s  swing=%-3s tech=%-3s skip=%-3s noclip=%-3s %s=%-5s"
              " dmin=%-6s limlast=%s"
              % (d.get("_ts", "?"), d.get("swing", "-"), d.get("tech", "-"),
                 d.get("skip", "-"), d.get("noclip", "-"), hk_lbl,
                 d.get(hostkeep_key(d) or "hostkeep", "-"),
                 d.get("dmin", "-"), d.get("limlast", "-")))
    tot[hk_lbl] = tot_hk


    # ---- the three verdicts ------------------------------------------------
    swings = tot["swing"]
    if swings:
        print("  " + verdict(True, "%d swing(s) fired across %d ride(s) (tech=%d"
                                   " named, skip=%d out of range)"
                                   % (swings, len(s.sw_rides), tot["tech"],
                                      tot["skip"])))
    else:
        print("  " + verdict(False, "swing=0 on every ride - NO swing was ever"
                                    " fired"))
        if not tot["tech"]:
            print("        tech=0 too: chooseAttack never named a technique."
                  "  Check the weapon")
            print("        first (P43RD drawn=/post= above, and aCW= on those"
                  " rows) - trip 13's")
            print("        ch=1 run had wpn=1, and an UNARMED rider has no"
                  " weapon technique to")
            print("        choose, which is what ch=0 43/43 meant back in P4-1d.")
        elif tot["skip"]:
            dmins = [x for x in (fnum(d, "dmin") for d in s.sw_rides)
                     if x is not None and x >= 0.0]
            lims = [x for x in (fnum(d, "limlast") for d in s.sw_rides)
                    if x is not None and x >= 0.0]
            print("        tech=%d but every attempt was out of the technique's"
                  " own range." % tot["tech"])
            if dmins and lims:
                print("        closest approach dmin=%.2f vs limlast=%.2f =>"
                      " the enemy never came" % (min(dmins), max(lims)))
                print("        inside the range the ENGINE says this swing is"
                      " usable at.  That is a")
                print("        geometry finding, not a bug in the trigger -"
                      " read it before touching")
                print("        the range test, and do NOT add a slack term to"
                      " make it fire.")
        elif tot["noclip"]:
            print("        noclip=%d: the range test passed but 'mid blow' was"
                  " never resolved." % tot["noclip"])
            print("        Read the `P41K resolve` line - ABSENT there and this"
                  " route has no clip.")
    print("  " + verdict(tot["noclip"] == 0,
                         "'mid blow' was resolved every time a window opened"
                         " (noclip=%d)" % tot["noclip"]))
    if tot["noclip"]:
        if t27:
            print("        noclip>0 means the range test passed but the HOST clip"
                  " was still NULL - on")
            print("        T27 that host is gP41kGuard, resolved on the stance's"
                  " first frame, so this")
            print("        should be rare and bounded.  Check `P41K resolve"
                  " guard='guard 1h'` - ABSENT")
            print("        there means the stance itself has no host and nothing"
                  " below is judgeable.")
        else:
            print("        noclip>0 means the range test passed but gP41kBlow was"
                  " still NULL, so the")
            print("        window refused to open rather than drop the guard with"
                  " nothing to replace it.")
            print("        Check the `P41K resolve ... blow='mid blow'` line -"
                  " ABSENT there kills the route.")

    # ---- the regression trip 17 was killed by -------------------------------
    # Six swings, six `LEGPOSE released grace=12` lines 110-160 ms later, and the
    # player watched the rider stand up on the ox in 'idle_stand_normal'.  The
    # window withholds the guard, so if what replaces it is not a real host the
    # skeleton has none and LegPoseFindHost hands the straddle mask back.  This
    # correlation is therefore the acceptance test for the whole design, not a
    # side note - keep it even when it passes.
    rel_grace = [r for r in s.released if r.get("_why") != "down"]
    if s.sw_open:
        opens = [t for t in (tsnum(d.get("_ts")) for d in s.sw_open) if t is not None]
        graces = [t for t in (tsnum(r.get("_ts")) for r in rel_grace) if t is not None]
        hits = []
        for g in graces:
            near = [o for o in opens if 0.0 <= g - o <= 1.0]
            if near:
                hits.append((near[-1], g))
        print("  " + verdict(not hits,
                             "no host-lost leg release follows a swing (%d"
                             " grace release(s), %d window(s))"
                             % (len(graces), len(opens))))
        if hits:
            print("        %d grace release(s) land within 1 s of a window"
                  " opening:" % len(hits))
            for o, g in hits[:6]:
                print("          open %.3f -> released %.3f  (+%.0f ms)"
                      % (o, g, (g - o) * 1000.0))
            if t27:
                print("        ⚠️ ON T27 THIS SHOULD BE STRUCTURALLY IMPOSSIBLE: the"
                      " window keeps the guard")
                print("        as host for every frame of its life, so LegPoseFindHost"
                      " cannot come up empty")
                print("        because of the swing.  A hit here means the host was"
                      " lost for some OTHER")
                print("        reason (stance dropped mid-window, rider knocked down,"
                      " ClipPin refused the")
                print("        guard) - read the P41K weight rows around that"
                      " timestamp before anything else.")
            else:
                print("        THAT IS TRIP 17's FAILURE: the window drops the guard,"
                      " so whatever replaces")
                print("        it must be a clip that owns an AnimationState -"
                      " otherwise LegPoseFindHost")
                print("        finds no host, the straddle mask goes back, and the"
                      " rider STANDS UP on the")
                print("        mount ('idle_stand_normal').  Do not read anything"
                      " else in this section")
                print("        until this line passes.")

    if swings:
        bad_off = [d for d in s.sw_rides
                   if (fnum(d, "swing", 0) or 0) > 0
                   and (hostkeep(d, 0) or 0) <= 0]
        if t27:
            # ⚠️ SAME TEST, DIFFERENT CLAIM.  On T27 nothing swaps, so >0 no longer
            # proves the swing outranked the stance - it proves the window was LIVE
            # when the pin pass ran, which is the precondition for the mask and the
            # authored arm alike.  Stating the old claim here would be a lie about a
            # build that never withholds the guard.
            print("  " + verdict(not bad_off,
                                 "every window reached the pin sites (hostkeep=%d"
                                 " frame(s) total, guard kept as host)" % tot_hk))
            if bad_off:
                print("        %d ride(s) fired a swing with hostkeep=0 => RideSwing"
                      "InFlight was false at" % len(bad_off))
                print("        the pin site while RideSwingPass thought a window was"
                      " open.  The two read")
                print("        the SAME bound (kRideSwingWinMs) by construction, so"
                      " this can only be a")
                print("        rider mismatch (gRideSwingWho) or a pass-order change"
                      " - not a tuning issue.")
        else:
            print("  " + verdict(not bad_off,
                                 "the one-shot really got the torso (guardoff=%d"
                                 " frame(s) total)" % tot_hk))
            if bad_off:
                print("        %d ride(s) fired a swing with guardoff=0 => the pin"
                      " swap never happened, so" % len(bad_off))
                print("        'guard 1h' kept the body and ClipPin's global door"
                      " refused the swing its")
                print("        render weight (trip 13's shape).  Both swap sites have"
                      " to be reached -")
                print("        HaltAndForceSitPass AND the animUpdate pre-pass; only"
                      " the first counts frames.")

    # ---- the individual windows -------------------------------------------
    if s.sw_open:
        print("  %d open row(s), %d close row(s) (budget kRideSwingLines=24 per"
              " ride, shared):" % (len(s.sw_open), len(s.sw_close)))
        names = {}
        for d in s.sw_open[:24]:
            nm = (d.get("tech", "") or "").strip("'")
            names[nm] = names.get(nm, 0) + 1
            print("    %8s  n=%-3s tech='%s' init=%-6s minS=%-7s lim=%-6s"
                  " d=%-6s reach=%s"
                  % (d.get("_ts", "?"), d.get("n", "-"), nm,
                     d.get("init", "-"), d.get("minS", "-"), d.get("lim", "-"),
                     d.get("d", "-"), d.get("reach", "-")))
        print("    techniques seen: "
              + ", ".join("%s x%d" % (k or "?", v)
                          for k, v in sorted(names.items())))
        # The close row carries prog= (did the clip run out, or did the wall-clock
        # cap cut it in half - trip 10 measured a fixed 1000 ms playing only ~20%
        # of 'mid blow') and the read-only Ogre probe on the TECHNIQUE name, which
        # is NOT the clip we pin.  ogre=found on a record-less technique clip is
        # the one reading that would put runCombatAnimation back on the table;
        # ogre=absent closes it for good.  ⚠️ RE_NOTES 21 never claimed either way.
        if s.sw_close:
            print("    close rows:")
            for d in s.sw_close[:12]:
                print("      %8s  n=%-3s prog=%-6s noclip=%-3s tech='%s' %s"
                      % (d.get("_ts", "?"), d.get("n", "-"), d.get("prog", "-"),
                         d.get("noclip", "-"),
                         (d.get("tech", "") or "").strip("'"),
                         "ogre=" + (d.get("ogre", "?") or "?")))
            og = [(d.get("ogre", "") or "").strip("'") for d in s.sw_close]
            found = [x for x in og if x == "found"]
            absent = [x for x in og if x == "absent"]
            if found:
                print("    NOTE  ogre=found on %d/%d close row(s): a technique clip"
                      " with NO AnimationData" % (len(found), len(og)))
                print("          record DOES own an Ogre::AnimationState (§21.1"
                      " chain, reached by name).")
                print("          ⇒ driving that state's own weight is a live"
                      " option again - read en=/w=/t=")
                print("          to see whether anything ever started it.")
            elif absent:
                print("    NOTE  ogre=absent on %d/%d close row(s): the engine"
                      " never built a state for the" % (len(absent), len(og)))
                print("          technique clip, so nothing can play it by name"
                      " and runCombatAnimation had")
                print("          nothing to put on the body either.  'mid blow'"
                      " stays the only pinnable swing.")
        # By construction the open row can only exist when d <= lim; a violation
        # means the range test was edited, not that the engine did something odd.
        viol = []
        for d in s.sw_open:
            dd, ll = fnum(d, "d"), fnum(d, "lim")
            if dd is not None and ll is not None and dd > ll + 0.01:
                viol.append(d.get("_ts", "?"))
        print("  " + verdict(not viol,
                             "every open row satisfies d <= lim (%d row(s)"
                             " checked)" % len(s.sw_open)))
        if viol:
            print("        rows at %s have d > lim - the range test in"
                  " RideSwingPass no longer" % ", ".join(viol[:6]))
            print("        matches what the log prints; fix one of the two"
                  " before reading anything")
            print("        else in this section.")
        # An open without its close is the ride that ended mid-swing: Dismount()
        # calls endCombatAnimation() unconditionally (RidingPlugin.cpp:5489) but
        # prints no close line, so exactly one missing close per such ride is the
        # healthy shape - it is not a leak.
        gap = len(s.sw_open) - len(s.sw_close)
        if gap:
            print("  NOTE  %d open row(s) have no close row.  One per ride that"
                  " ended mid-swing is" % gap)
            print("        expected: Dismount() closes the swing"
                  " unconditionally but prints nothing.")
    print("  NOTE  \"did the stance come back\" is judged on the NEXT open row or"
          " the next P41K")
    print("        sample, NEVER on the frames right after a close - the dying"
          " one-shot is")
    print("        still inside ClipPin's `others` for ~0.3 s, so the guard's ms"
          " structurally")
    print("        cannot climb back there (trip 10's judging error, HISTORY §U).")


def report_swing_look(s):
    """T21 - P4-3-4b: is the swing ONE swing, from the start of the clip?

    T20 proved the mechanism (swing=17, guardoff=5126, zero grace releases) and
    then handed back a purely cosmetic defect the player described as
    "right hand slitting the left wrist".  Trip 18 measured why, twice over:
      * 'mid blow' advanced ~0.207 prog/s => a ~4.8 s clip, of which a 2500 ms
        window shows ~52%.
      * window 2 RESUMED at prog 0.518 where window 1 stopped at 0.517.  The
        pinned SingleAnimation outlives its window with currentFrameTime01
        intact, so every window after the first plays an arbitrary MIDDLE slice
        - and a window that opens on a leftover prog >= kRideSwingDoneProg
        closes on its very first frame having played nothing at all.
    So this section judges three writes, and one of them is optional:
      rst=   RideSwingRestart zeroed the clip - MUST equal swing= (load-bearing)
      ms=    the window ended because the CLIP ran out, not the wall clock
      sp=    the speed write survived the engine's update (nice-to-have: if the
             engine re-imposes 1.00 the swing is still start-aligned, just slow)
    ⛔ ALL THREE ARE RETIRED BY T27, and this section says so instead of judging
    them: every one is a statement about a one-shot clip pinned INSIDE the window,
    and T27 stopped swapping the host, so there is no such clip.  rst=0 / sp on a
    loop / prog cycling are the designed shapes, and reporting them as failures
    would be the tool contradicting the build it was written after.
    """
    print("")
    print("== T21 - P4-3-4b: one whole swing (restart + speed) ==")
    if not (s.sw_rides or s.sw_close):
        print("  no P4-3-4 P43SW row at all - nothing to judge (see T20 above).")
        return
    if is_t27(s):
        print("  RETIRED for this build (close rows carry hostkeep=): the window no"
              " longer swaps a")
        print("  one-shot onto the body, so there is no clip to restart, no speed to"
              " impose and no")
        print("  progress to close on.  rst=0 is the EXPECTED value here, not a"
              " miss - the window's")
        print("  length is kRideSwingWinMs and the stroke's own completion is armt="
              " in -- T25/T26 --.")
        print("  Judge this build in -- T27 -- below.")
        return
    has_t21 = any("rst" in d for d in s.sw_rides) or \
              any("pinst" in d for d in s.sw_close)
    if not has_t21:
        print("  P43SW rows carry no rst= / pinst= field: this log predates the")
        print("  restart+speed fix.  NOT a failure - T20 above is the whole of")
        print("  what this log can say about the swing.")
        return

    # ---- 1) one restart per window (the load-bearing check) -----------------
    # Checked per ride, not on the totals: two rides of 3+1 and 1+3 would sum
    # correctly while one of them played from wherever the other stopped.
    bad_rst, tot_sw, tot_rst = [], 0, 0
    for d in s.sw_rides:
        # ⚠️ No `or` default on either read: fnum returns 0.0 for a genuine rst=0,
        # and `0.0 or -1` is -1, which flagged every ride that had no fight at all
        # (trip 19's third ride: swing=0 rst=0 printed as "swing=0 but rst=-1").
        sw = fnum(d, "swing")
        rs = fnum(d, "rst")
        if sw is None:
            continue
        sw = int(sw)
        rs = -1 if rs is None else int(rs)
        tot_sw += sw
        tot_rst += max(rs, 0)
        if rs != sw:
            bad_rst.append((d.get("_ts", "?"), sw, rs))
    print("  " + verdict(not bad_rst,
                         "one restart per window on every ride (rst=%d,"
                         " swing=%d)" % (tot_rst, tot_sw)))
    if bad_rst:
        for t, sw, rs in bad_rst[:6]:
            print("        %8s  swing=%d but rst=%d" % (t, sw, rs))
        print("        rst < swing means RideSwingFindEntry could not find the"
              " pinned entry while")
        print("        the window was open, so that window played from wherever"
              " the previous one")
        print("        stopped - the trip-18 half-slice, unfixed.  Read pinst="
              " on the close rows:")
        print("        pinst=none there and the layer walk is looking in the"
              " wrong list; pinst=AV")
        print("        and the SEH shell caught a dangling layer.")

    # ---- 2) the clip ended the window, not the clock ------------------------
    # A close row is the only place both halves are visible.  prog >= 0.90 is
    # kRideSwingDoneProg, i.e. "the clip finished"; a ms= at the 3000 ms cap
    # means it was cut off instead - which is exactly what trip 18 looked like.
    closes = [d for d in s.sw_close if "pinst" in d]
    capped, instant, progs, mss = [], [], [], []
    for d in closes:
        p, m = fnum(d, "prog"), fnum(d, "ms")
        if p is not None:
            progs.append(p)
        if m is not None:
            mss.append(m)
            if m >= 2950.0:
                capped.append((d.get("_ts", "?"), p, m))
            if m < 200.0:
                instant.append((d.get("_ts", "?"), p, m))
    print("  " + verdict(not capped,
                         "every window closed on the CLIP, not the 3000 ms cap"
                         " (%d close row(s))" % len(closes)))
    if capped:
        for t, p, m in capped[:6]:
            print("        %8s  ms=%.0f prog=%s  <- clock cut it off"
                  % (t, m, p))
        print("        The swing is still a fraction of the clip.  Two knobs,"
              " in this order: raise")
        print("        kRideSwingSpeed (check sp= below actually took), then"
              " kRideSwingLenMs - and if")
        print("        you raise the cap you MUST raise kRideSwingMinGapMs with"
              " it (cap < gap, or a")
        print("        window re-opens on the frame after it closed).")
    print("  " + verdict(not instant,
                         "no window closed on its first frame (the trip-18"
                         " leftover-progress bug)"))
    if instant:
        for t, p, m in instant[:6]:
            print("        %8s  ms=%.0f prog=%s  <- opened already finished"
                  % (t, m, p))
        print("        A window that opens on a leftover prog >= 0.90 closes"
              " immediately: the restart")
        print("        did not land BEFORE RideSwingPass read prog.  It has to"
              " run in")
        print("        HaltAndForceSitPass, which the pass order (:7037-7040)"
              " puts first.")
    if progs and mss:
        print("    prog at close: min=%.3f max=%.3f | window ms: min=%.0f"
              " max=%.0f avg=%.0f"
              % (min(progs), max(progs), min(mss), max(mss),
                 sum(mss) / float(len(mss))))
    return _swing_look_tail(s, closes)


def _swing_look_tail(s, closes):
    """The read-back half of T21: what the two probes say about the pinned clip.

    Split out only to keep one screenful per idea - it is the same section.
    """
    # ---- 3) did the speed write survive? -----------------------------------
    # NOTE, not CHECK: nothing in this DLL had ever passed a speed other than
    # 1.0f before T21, so "the engine re-imposes 1.00" is an unknown being
    # measured here, not a failure.  Knob 1 (the restart) is what makes the
    # swing start at the start; speed only decides whether it FITS the cap.
    sps = [x for x in (fnum(d, "sp") for d in closes) if x is not None]
    if sps:
        took = [x for x in sps if x > 1.05]
        if took and len(took) == len(sps):
            print("  NOTE  sp=%.2f on %d/%d close row(s): the speed write"
                  " SURVIVED the engine's" % (took[0], len(took), len(sps)))
            print("        own update, so kRideSwingSpeed is a real knob.")
        elif took:
            print("  NOTE  sp>1.05 on only %d/%d close row(s) (min=%.2f"
                  " max=%.2f): the engine takes"
                  % (len(took), len(sps), min(sps), max(sps)))
            print("        the speed sometimes and drops it otherwise - read"
                  " ms= per row before")
            print("        trusting the knob.")
        else:
            print("  NOTE  sp=1.00 on all %d close row(s): the engine RE-IMPOSES"
                  " speed 1.0, so" % len(sps))
            print("        kRideSwingSpeed is dead as a knob (both the"
                  " runAnimation argument and the")
            print("        SingleAnimation field).  The swing is still"
                  " start-aligned - only the cap")
            print("        decides how much of a ~4.8 s clip is visible, so"
                  " kRideSwingLenMs is the")
            print("        only lever left (and cap < gap must hold).")

    # ---- 4) 'mid blow's true length, printed rather than derived -----------
    # Trip 18 could only infer ~4.8 s from two prog readings and a stopwatch.
    # olen= is Ogre's own number for the clip we pin, and every window-length
    # constant in the source is only as good as that estimate was.
    olens = [x for x in (fnum(d, "olen") for d in closes) if x is not None]
    if olens:
        ol = max(olens)
        print("    'mid blow' olen=%.3f s (Ogre, measured - trip 18 could only"
              " derive ~4.8 s)" % ol)
        sp = max([x for x in sps if x is not None] or [1.0])
        need = ol * 0.90 / max(sp, 0.01) * 1000.0
        print("      => 0->90%% at sp=%.2f needs %.0f ms; the cap is 3000 ms"
              % (sp, need))
        if need > 3000.0:
            print("      " + verdict(False, "the cap CANNOT fit a whole swing"
                                            " - raise speed, or cap and gap"
                                            " together"))

    # ---- 5) the restart's before/after pair --------------------------------
    # The open row samples the entry BEFORE the request site has run for that
    # window, so from n=2 on its t01= is the leftover the restart erases.  This
    # is the one place the defect and its fix sit on two adjacent lines.
    pres = [(d.get("_ts", "?"), d.get("n", "-"), d.get("pinst", "?"),
             fnum(d, "t01"))
            for d in s.sw_open if "pinst" in d]
    leftover = [(t, n, v) for t, n, st, v in pres
                if st == "live" and v is not None and v > 0.02]
    if pres:
        print("    open-row leftover (pre-restart t01, %d row(s) sampled):"
              % len(pres))
        for t, n, st, v in pres[:8]:
            print("      %8s  n=%-3s pinst=%-5s t01=%s"
                  % (t, n, st, "-" if v is None else "%.3f" % v))
        if leftover:
            print("    NOTE  %d open row(s) found a leftover t01>0.02 waiting"
                  " for them - that is the" % len(leftover))
            print("          trip-18 defect being caught in the act; rst="
                  " above says it was erased.")

    # ---- 6) the pin still held at close, and Ogre agreed -------------------
    held = [d for d in closes
            if (fnum(d, "pw", 0) or 0) >= 0.95 and d.get("psw") == "1"]
    if closes:
        print("  " + verdict(len(held) == len(closes),
                             "the pin still held at every close (pw>=0.95 and"
                             " psw=1 on %d/%d)"
                             % (len(held), len(closes))))
        oen = [d for d in closes if d.get("oen") == "1"]
        print("  " + verdict(len(oen) == len(closes),
                             "the pinned clip's Ogre state was enabled at close"
                             " (oen=1 on %d/%d)" % (len(oen), len(closes))))
        if len(oen) != len(closes):
            print("        oen=0 means the render side was not driving 'mid"
                  " blow' even though our")
            print("        fields said it was wanted - that is the same"
                  " ClipPin door (target+others")
            print("        <= 1.02f) the guard stand-down exists to get past."
                  "  Check guardoff= in T20.")
    print("  NOTE  the eyeball half of T21 is not in this file: \"it reads as"
          " one swing\" and the")
    print("        four v1.6 behaviours (draw / stance / keep held / sheathe)"
          " can only be judged")
    print("        in game.  This section only proves the clip started at 0"
          " and ran to the end.")


def swing_authored(s):
    """True when the build AUTHORS the swing (T25) instead of playing one of the
    engine's records.

    The ride line's arm= only exists from that build onward, and its arrival retires
    three fields at once: drv= (T22's drive of the technique's own Ogre state), hold=
    (T23's mask on that state) and fit= (T24's phase fitting) are all zero BY DESIGN
    afterwards, because both assertion sites were removed.  Every section that judges
    one of those three has to check this first or it reports a design decision as a
    regression.
    """
    return any("arm" in d for d in s.sw_rides) or bool(s.sw_arm)


def report_swing_drive(s):
    """T22 - two independent trip-19 answers.

    (A) The rider stopped facing the mount's REAR.  Trip 19: "人物在牛背上打架的
        时候会突然转向牛屁股的方向".  The rider's facing is the mount's per-frame
        position delta, refreshed above a 0.03 threshold - so a mount that backs
        off or is knocked back in a fight reverses it without turning its body.
        The fix vetoes a delta pointing the opposite way from the animal's own
        getFacingDirection() and HOLDS the last good heading instead.
    (B) Can the TECHNIQUE's own Ogre::AnimationState be driven?  Trips 18+19
        measured ogre=found 31/31 - the record-less clips DO own a state - and
        trip 19 handed back the one thing 'mid blow' can never fix: it is not an
        attack clip ("这个动作实在不像挥砍").  This is additive: the pin, its
        weight, its speed and its restart are untouched, so a refusal here costs
        nothing that trips 18-19 proved.
    """
    print("")
    print("== T22 - rear-facing veto (A) + driving the technique's state (B) ==")
    if not (s.sw_rides or s.sw_close):
        print("  no P43SW row at all - nothing to judge (see T20/T21 above).")
        return
    has_a = any("hdveto" in d for d in s.sw_rides)
    has_b = any("drv" in d for d in s.sw_rides) or any("drv" in d for d in s.sw_close)
    if not (has_a or has_b):
        print("  P43SW rows carry neither hdveto= nor drv=: this log predates the")
        print("  veto and the state drive.  NOT a failure.")
        return

    # ---- A) the heading veto ------------------------------------------------
    print("  A) heading veto (hdveto= = frames the travel delta was NOT taken):")
    if not has_a:
        print("     field absent - this log predates the veto.")
    else:
        tot_v = 0
        for d in s.sw_rides:
            v = fnum(d, "hdveto")
            if v is None:
                continue
            tot_v += int(v)
            print("     %8s  hdveto=%d" % (d.get("_ts", "?"), int(v)))
        print("  NOTE  there is no pass/fail here and hdveto=0 is not a failure:"
              " it means no")
        print("        reversal happened on those rides at all.  hdveto>0 proves"
              " the veto fired")
        print("        (%d frame(s) held the last good heading), and the eyeball"
              " half - \"did the" % tot_v)
        print("        rider still swing round to look at the mount's rear\" - is"
              " the verdict.")
        print("        ⚠️ A sideways shove reads dot ~= 0 and is NOT vetoed at"
              " kHeadingFaceMinDot=0;")
        print("        if the report becomes \"turns sideways\", that constant is"
              " the knob.")

    # ---- B) the technique-state drive ---------------------------------------
    print("  B) technique state drive (drv= frames written, en=/w=/t= at close):")
    if not has_b:
        print("     field absent - this log predates the drive.")
        return
    if swing_authored(s):
        print("     ⛔ RETIRED BY T25 (this log has arm=): the technique drive was removed"
              " from both")
        print("        assertion sites, so drv=0 / hold=0 / fit=0 are the EXPECTED shape"
              " here and none")
        print("        of the checks below apply.  Trip 22 ruled the whole"
              " play-one-of-the-engine's-records")
        print("        family out - 「刀砍不出去，只能在自己肚子那块拉，动作都挤成一团了」"
              " on a record the")
        print("        engine itself picked for the distance - and the user's ruling with it:")
        print("        「不一定非要和原版一样，只要像骑砍那样挥砍的动作就行」."
              "  Judge the swing in -- T25 --.")
        for d in s.sw_rides:
            sw, dv = fnum(d, "swing"), fnum(d, "drv")
            if sw is None or dv is None:
                continue
            print("     %8s  swing=%d drv=%d%s"
                  % (d.get("_ts", "?"), int(sw), int(dv),
                     "" if int(dv) == 0 else "   ⚠️ non-zero: a drive site came back!"))
        return
    # drv=0 on a ride that swung at all means not one write landed.
    dead = []
    for d in s.sw_rides:
        sw, dv = fnum(d, "swing"), fnum(d, "drv")
        if sw is None or dv is None:
            continue
        print("     %8s  swing=%d drv=%d" % (d.get("_ts", "?"), int(sw), int(dv)))
        if int(sw) > 0 and int(dv) == 0:
            dead.append(d.get("_ts", "?"))
    print("  " + verdict(not dead,
                         "the write reached the state on every ride that swung"))
    if dead:
        print("        drv=0 with swing>0 on: %s" % ", ".join(dead[:6]))
        print("        Not one write landed, so nothing below means anything."
              "  Two causes, and the")
        print("        close row separates them: ogre=absent means"
              " getAnimationState() returned NULL")
        print("        for that clip name (§21.1 chain), anything else means"
              " RideSwingDrive's SEH")
        print("        shell swallowed an access violation writing it.")
        return
    closes = [d for d in s.sw_close if "en" in d]
    gone = [d for d in s.sw_close if "en" not in d]
    if gone:
        print("     ⚠️ %d close row(s) print ogre=absent: getAnimationState()"
              " handed back NULL for" % len(gone))
        print("        that clip name at the close, so en=/w=/t= are missing"
              " there by construction.")
    if not closes:
        print("     no close row carries en= - budget exhausted or no window"
              " closed.")
        return
    en1 = [d for d in closes if d.get("en") == "1"]
    print("  " + verdict(len(en1) == len(closes),
                         "the state was still enabled at the close (en=1 on"
                         " %d/%d)" % (len(en1), len(closes))))
    if len(en1) != len(closes):
        print("        🔑 THE decisive reading of this trip: the write landed"
              " (drv>0) and the engine")
        print("        cleared it again.  Both assertion sites run BEFORE"
              " animUpdate_orig, so the")
        print("        next rung is asserting after it - not a new address, the"
              " same hook.")
    ws = [x for x in (fnum(d, "w") for d in closes) if x is not None]
    if ws:
        print("     weight at close: min=%.3f max=%.3f  (asked for"
              " kRideSwingTechW)" % (min(ws), max(ws)))
        print("        ⚠️ w= is NOT evidence on its own: trip 19 already read"
              " w=1.000 with en=0")
        print("        and t=0.000, i.e. that is the state's DEFAULT weight,"
              " untouched.  Only")
        print("        drv= and en= say whether our write happened and stuck.")
        if max(ws) < 0.001:
            print("        w=0.000 with en=1 means the weight write specifically"
                  " was reverted -")
            print("        the state is in the set and switched on but"
                  " contributes nothing.")
    arc = []
    for d in closes:
        t, ln = fnum(d, "t"), fnum(d, "len")
        if t is None or ln is None or ln <= 0.001:
            continue
        arc.append(t / ln)
    if arc:
        print("     arc reached at close: min=%.0f%% max=%.0f%% avg=%.0f%%"
              " over %d row(s)"
              % (100.0 * min(arc), 100.0 * max(arc),
                 100.0 * sum(arc) / len(arc), len(arc)))
        if max(arc) < 0.01:
            print("        0% across the board is the trip-19 BASELINE shape"
                  " (t=0.000): the state")
            print("        was never started, so this says nothing about window"
                  " length - read en= above.")
        elif min(arc) < 0.90:
            print("        under 90% means the window closed before the arc"
                  " finished: the window")
            print("        still ends on 'mid blow' (prog 0.90 at"
                  " kRideSwingSpeed), and kRideSwingTechMs")
            print("        is the restatement of that length - raise it and the"
                  " arc gets cut, lower")
            print("        it and the arc finishes early and holds its last"
                  " pose.")
    print("  NOTE  what this section CANNOT say: whether an enabled, weighted"
          " state actually")
    print("        reaches the skeleton.  🔑 TRIP 20 ANSWERED THAT, and the answer"
          " is YES: with")
    print("        drv>0 / en=1 11/11 / w=1.000 / arc=100% the eyeball read"
          " 「动作幅度很大」 -")
    print("        the sword left the hand for the chest, floated overhead, the"
          " body balled up and")
    print("        the rider slid in front of the mount.  So the pre-registered"
          " kill criterion")
    print("        (\"looks unchanged ⇒ this route is finished\") never fired: the"
          " route is alive")
    print("        and UNCONSTRAINED.  Everything after that is T23 below."
          "  The straddle")
    print("        regression that matters is in T20: grace releases must stay"
          " 0 (this change")
    print("        is additive, so a non-zero count there would be a surprise"
          " worth chasing).")


def report_swing_split(s):
    """T23 - the complementary split.

    Trip 20 answered T22(B) the other way round from the branch that was
    pre-registered as its kill criterion.  drv>0 on every ride that swung,
    en=1 11/11, w=1.000, arc=100% - and the eyeball read「把刀从右手往胸口收，
    然后刀飘到头顶，整个人缩成一团瞬移到坐骑前面。动作幅度很大，但是毫无意义」.
    So the driven state DOES reach the skeleton (§17.12's "nothing visible ⇒
    route finished" branch never happened); what it lacked was any constraint:

      * root/pelvis tracks of a GROUND clip stepped the whole skeleton forward
        (nobody applies the whole/reloc bits for a record-less clip, §19);
      * 'mid blow' (pinned host, weight 1.0) and the technique (weight 1.0, no
        blend mask at all) both drove every spine/arm bone.

    The split: the technique gets root/pelvis/legs/feet masked to 0 (it may have
    the torso, never the seat); the host gets its upper body masked to 0 for the
    length of the window (it keeps the legs, which is the only thing it is
    load-bearing for - §17.9).  Fingers are in neither list, so the grip stays.
    """
    print("")
    print("== T23 - the split: technique holds the seat, host frees the torso ==")
    has_tab = bool(s.sw_hold_bones or s.sw_free_bones)
    has_hold = any("hold" in d for d in s.sw_close)
    has_free = any("swfree" in d for d in s.sw_rides)
    if not (has_tab or has_hold or has_free):
        print("  no SWING bone row and no hold= / swfree= field: this log predates")
        print("  the split.  NOT a failure - T22 above is that build's whole verdict.")
        return

    # ---- 1) did the two bone tables resolve at all --------------------------
    for label, rows, why in (
            ("hold", s.sw_hold_bones,
             "the technique is masked OFF these (seat + straddle + the teleport)"),
            ("free", s.sw_free_bones,
             "the host is masked off these while a window is open (the swing)")):
        if not rows:
            print("  %s table: no row.  Either the build predates it or the leg-pose"
                  " pass never ran." % label)
            continue
        ok = [d for d in rows if d.get("has") == "1"]
        bad = [d for d in rows if d.get("has") != "1"]
        print("  %s table (%s):" % (label, why))
        if ok:
            print("     " + ", ".join("%s=%s" % (d["_bone"].replace("Bip01 ", ""),
                                                 d.get("handle", "?")) for d in ok))
        else:
            print("     none resolved")
        print("  " + verdict(not bad, "every %s bone resolved by name (%d/%d)"
                             % (label, len(ok), len(rows))))
        if bad:
            print("        MISSING: %s" % ", ".join(d["_bone"] for d in bad))
            if label == "hold":
                print("        ⚠️ 'Bip01' or 'Bip01 Pelvis' missing = the teleport guard"
                      " is not armed at all,")
                print("        so「瞬移到坐骑前面」would be expected to survive this"
                      " build.  Names come")
                print("        from the P2-1b inventory (DumpRiderSkeleton, Ctrl+NUM.)"
                      " - read it, do not")
                print("        guess a handle: this rider has 30 bones and 0/1 only"
                      " LOOK like root/pelvis.")
            else:
                print("        A missing free bone stays under the host, so the"
                      " technique fights it there")
                print("        for the whole window - that is the 「缩成一团」 half,"
                      " unfixed on that bone.")

    # ---- 2) hold= : did the technique's own mask get written, per window -----
    want_hold = len([d for d in s.sw_hold_bones if d.get("has") == "1"])
    if has_hold and swing_authored(s):
        print("  hold= is RETIRED BY T25 (this log has arm=): the technique's state is no longer"
              " driven,")
        print("        so nothing creates a mask on it and hold=0 is the expected shape."
              "  The free")
        print("        table above is NOT retired - it shrank to the two bones T25 authors,"
              " and that")
        print("        equality (freed set == authored set) is what -- T25 -- checks.")
    elif has_hold:
        rows = [d for d in s.sw_close if "hold" in d]
        vals = [int(fnum(d, "hold")) for d in rows if fnum(d, "hold") is not None]
        full = [v for v in vals if want_hold and v == want_hold]
        print("  hold= per window (entries zeroed on the TECHNIQUE's state):")
        print("     " + " ".join("%s:%d" % (d.get("_ts", "?"),
                                            int(fnum(d, "hold") or 0)) for d in rows[:14]))
        if want_hold:
            print("  " + verdict(len(full) == len(vals),
                                 "every window masked the whole hold table"
                                 " (%d/%d rows at %d entries)"
                                 % (len(full), len(vals), want_hold)))
        if vals and max(vals) == 0:
            print("        hold=0 on every window with the table resolved means"
                  " getNumBones() read 0,")
            print("        so createBlendMask was skipped - the technique ran"
                  " UNMASKED exactly as it")
            print("        did on trip 20, and 「瞬移」/「缩成一团」 are expected"
                  " to be unchanged.")
    else:
        print("  hold= absent from the close rows: no technique mask in this build.")

    # ---- 3) swfree= : did the host actually stand down, per ride -------------
    if has_free:
        print("  swfree= per ride (frames the host's upper body was released):")
        bad_free = []
        for d in s.sw_rides:
            sw, fr = fnum(d, "swing"), fnum(d, "swfree")
            if sw is None or fr is None:
                continue
            hk = hostkeep(d)
            hkk = hostkeep_key(d) or "hostkeep"
            print("     %8s  swing=%d swfree=%d%s"
                  % (d.get("_ts", "?"), int(sw), int(fr),
                     "" if hk is None else "  (%s=%d)" % (hkk, int(hk))))
            if int(sw) > 0 and int(fr) == 0:
                bad_free.append(d.get("_ts", "?"))
        print("  " + verdict(not bad_free,
                             "the host stood down on every ride that swung"))
        if bad_free:
            print("        swfree=0 with swing>0 on: %s" % ", ".join(bad_free[:6]))
            print("        The window opened and the leg-pose pass never saw it"
                  " (RideSwingInFlight is")
            print("        pointer-compared against gRideSwingWho), so the host"
                  " kept the torso for the")
            print("        whole swing - the 「缩成一团」 half is untested by this"
                  " trip, not disproved.")
        if is_t27(s):
            print("  NOTE  ON T27 THESE TWO ARE NOT COMPLEMENTARY ANY MORE.  swfree="
                  " is still the mask")
            print("        (the two arm bones taken off the host), hostkeep= is now"
                  " the frames the guard")
            print("        was KEPT as host - so both non-zero means \"the host holds"
                  " the body and has")
            print("        let go of exactly the arm\", which is the whole T27 design"
                  " on one row.")
        else:
            print("  NOTE  swfree= and guardoff= count different things and need not"
                  " match: guardoff is")
            print("        the two guard-assertion sites, swfree is the one leg-pose"
                  " pass.  Both being")
            print("        non-zero on the same ride is the shape to expect.")
    else:
        print("  swfree= absent from the ride rows: the host is never released"
              " in this build.")

    # ---- 4) the leak this change can cause, and where it is already measured -
    print("  NOTE  the regression to fear here is a LEAKED 0.0 entry, not a crash:"
          " 11 upper-body")
    print("        entries are now zeroed on every weighted clip mid-window, and a"
          " ride that ends")
    print("        INSIDE a window (dismount, knocked down, load) leaves them"
          " there.  Two places")
    print("        already judge it and BOTH must stay clean: the handback audit's"
          " residue= and")
    print("        dropped= (straddle section above - residue now counts the free"
          " bones too), and")
    print("        the eyeball on a character who has just dismounted (arms and"
          " head frozen at bind")
    print("        while walking = the leak, §21.2's disease on new bones).  The"
          " technique state's")
    print("        own mask leaks onto the PLAYER'S GROUND SWING instead (dead"
          " root ⇒ an attack that")
    print("        no longer steps into the blow), and nothing in this file can"
          " see that - only")
    print("        fighting on foot after a ride can.")
    print("  NOTE  what this section cannot say, again: whether the result LOOKS"
          " like a swing.")
    print("        Four eyeball outcomes, each with its own next rung:")
    print("          1. it reads as a chop            ⇒ P4-3-4 is done, close it.")
    print("          2. no teleport, but the torso is dead/stiff ⇒ the technique's"
          " tracks need the")
    print("             host underneath after all: put the arms back under the"
          " host (shrink the")
    print("             free table to the spine) and lower kRideSwingTechW instead"
          " of masking.")
    print("          3. still 「瞬移到坐骑前面」 ⇒ the root motion is not in"
          " 'Bip01'/'Bip01 Pelvis'.")
    print("             Read the hold table's handles above against the P2-1b bone"
          " inventory before")
    print("             adding names - a fourth guess at which bone carries it is"
          " not evidence.")
    print("          4. torso still 「缩成一团」 while swfree>0 ⇒ two clips is not"
          " the mechanism;")
    print("             the technique alone is that shape, and only lowering its"
          " weight can soften it.")


def report_swing_gate(s):
    """T24 - two chooseAttack questions, one geometry.

    Trip 21 closed T23's mechanism (no teleport, no balled-up torso, no leaked
    mask - the ground swings after dismounting were normal) and left exactly one
    gap: 9/9 windows played 'bigchopv2' and the eyeball read 「双手持刀然后反转刀身
    把刀朝下然后向下刺去」.  Two measured facts explain that shape:

      * init=25.00 is more than double the rider's own reach=10.50, and trip 20
        (same record, no mask) slid the rider bodily in front of the mount ⇒
        'bigchopv2' is a CLOSING attack whose footwork is half the animation.
        T23 masks the root off, so the screen gets the arrival half only.
      * the mount's own body holds the enemy off, so rider->threat is
        structurally 12..25 (trip 21 dmin= 12.56 / 13.89 / 21.40 / 15.47) and
        never inside reach ⇒ asked about that distance the engine correctly
        keeps answering "close the gap first".

    So the producer now asks twice: chooseAttack(d, reach) for the GATE (its
    init=/minS= still drive lim=, so the rhythm stays trip 21's) and
    chooseAttack(min(d, reach), reach) for the CLIP.  Both numbers are the
    engine's; no distance constant was invented.  gate= vs tech= on the open row
    is the whole verdict, and it can come back negative on its own terms.
    """
    print("")
    print("== T24 - two questions: gate the engagement, play the in-place attack ==")
    opens = [d for d in s.sw_open if "gate" in d]
    fits = [d for d in s.sw_close if "fit" in d]
    if not (opens or fits):
        print("  no gate= on any open row and no fit= on any close row: this log")
        print("  predates the two-question producer.  NOT a failure - T20..T23 above")
        print("  are that build's whole verdict.")
        return

    # ---- 1) did the second question return something else -------------------
    if opens:
        print("  open rows (gate= is what lim= is read off, tech= is what gets played):")
        pairs = {}
        for d in opens[:24]:
            g = (d.get("gate", "") or "").strip("'")
            t = (d.get("tech", "") or "").strip("'")
            pairs[(g, t)] = pairs.get((g, t), 0) + 1
            print("    %8s  n=%-3s d=%-6s dq=%-6s reach=%-6s gate='%s' -> tech='%s'"
                  % (d.get("_ts", "?"), d.get("n", "-"), d.get("d", "-"),
                     d.get("dq", "-"), d.get("reach", "-"), g, t))
        print("    pairs seen: "
              + ", ".join("%s -> %s x%d" % (g or "?", t or "?", n)
                          for (g, t), n in sorted(pairs.items())))
        moved = [d for d in opens
                 if (d.get("gate", "") or "").strip("'")
                 != (d.get("tech", "") or "").strip("'")]
        if moved:
            print("  PASS  the second question returned a DIFFERENT record on %d/%d"
                  " window(s)" % (len(moved), len(opens)))
            print("        ⇒ asking about min(d, reach) really does move the engine"
                  " off its closing")
            print("        attack.  Whether the new record LOOKS like a chop is the"
                  " eyeball half.")
        else:
            print("  CHECK the two questions returned the SAME record on all %d"
                  " window(s)." % len(opens))
            print("        This is a CLEAN NEGATIVE, not a code fault: for this"
                  " rider's technique set")
            print("        the engine's answer does not depend on the distance it is"
                  " asked about.")
            print("        ⇒ the remaining lever is naming a clip ourselves.  Take it"
                  " from the §17.10")
            print("        census ONLY ('downward combo' init 10.00/10.00 is the"
                  " rider-shaped one);")
            print("        ⚠️ a clip name has never once survived being guessed in"
                  " this project.")

        # ---- 2) dq= must literally be min(d, reach) -------------------------
        bad_dq = []
        for d in opens:
            dv, rv, qv = fnum(d, "d"), fnum(d, "reach"), fnum(d, "dq")
            if dv is None or rv is None or qv is None:
                continue
            if abs(qv - min(dv, rv)) > 0.011:
                bad_dq.append(d)
        print("  " + verdict(not bad_dq,
                             "dq= is min(d, reach) on every row (%d checked)"
                             % len(opens)))
        if bad_dq:
            print("        %d row(s) disagree, e.g. %s: d=%s reach=%s dq=%s."
                  % (len(bad_dq), bad_dq[0].get("_ts", "?"),
                     bad_dq[0].get("d", "-"), bad_dq[0].get("reach", "-"),
                     bad_dq[0].get("dq", "-")))
            print("        That is the clamp itself misreading, so the CLIP question"
                  " was asked about the")
            print("        wrong distance - everything below is void until it is fixed.")

        # ---- 3) the rhythm must not have moved ------------------------------
        # lim= is the ladder init -> minS -> reach -> kRideThreatDist, and after
        # T24 it MUST still be read off the GATE technique.  If it ever comes off
        # the clip's opinion instead, an in-place record ('chop left-3' 0.00/-10.00,
        # 'downward combo' 10.00/10.00) silently refuses every window against a
        # structural d of 12..25 - i.e. the swing disarms and the trip is wasted.
        bad_lim = []
        for d in opens:
            iv, mv, rv, lv = (fnum(d, "init"), fnum(d, "minS"),
                              fnum(d, "reach"), fnum(d, "lim"))
            if lv is None:
                continue
            if iv is not None and iv > 1.0:
                want = iv
            elif mv is not None and mv > 1.0:
                want = mv
            elif rv is not None and rv > 1.0:
                want = rv
            else:
                want = 60.0            # kRideThreatDist, the last rung
            if abs(lv - want) > 0.011:
                bad_lim.append((d, want))
        print("  " + verdict(not bad_lim,
                             "lim= still comes off the ladder's first real opinion"
                             " (%d row(s) checked)" % len(opens)))
        if bad_lim:
            d0, w0 = bad_lim[0]
            print("        %d row(s) disagree, e.g. %s: init=%s minS=%s reach=%s"
                  " lim=%s, expected %.2f."
                  % (len(bad_lim), d0.get("_ts", "?"), d0.get("init", "-"),
                     d0.get("minS", "-"), d0.get("reach", "-"),
                     d0.get("lim", "-"), w0))
            print("        ⚠️ THE failure mode of this rung: judge the real d against"
                  " the CLIP record's")
            print("        opinion and every window is refused (swing=0) with nothing"
                  " visible to explain it.")

    # ---- 4) the arc must never be STRETCHED ---------------------------------
    if fits and swing_authored(s):
        print("  fit= is RETIRED BY T25 (this log has arm=): no record is played, so there is no"
              " phase")
        print("        to fit and fit=0 is the expected shape.  The three checks above still"
              " stand - the")
        print("        producer keeps NAMING a technique every window, which is what gate=/dq=/lim="
              " read.")
    elif fits:
        print("  fit= per window (ms the arc was fitted into; kRideSwingTechMs=1700"
              " is a ceiling now):")
        rows = []
        for d in fits[:12]:
            fv = fnum(d, "fit")
            lv = fnum(d, "len")          # the technique state's own Ogre length
            rows.append("%s:%s%s" % (d.get("_ts", "?"), d.get("fit", "-"),
                                     "" if lv is None else "/len=%.3f" % lv))
        print("     " + " ".join(rows))
        over = [d for d in fits if (fnum(d, "fit") or 0.0) > 1700.5]
        print("  " + verdict(not over,
                             "no window fitted the arc into more than"
                             " kRideSwingTechMs (%d checked)" % len(fits)))
        if over:
            print("        %d row(s) over the ceiling - the clamp is inverted."
                  % len(over))
        # And when the clip is SHORTER than the window, fit must be its own length:
        # that is the whole point (1.067 s stretched to 1700 ms = 0.63x slow motion,
        # which reads as a defect and is what T24 must never introduce).
        checked, bad_fit, natural = 0, [], 0
        for d in fits:
            fv, lv = fnum(d, "fit"), fnum(d, "len")
            if fv is None or lv is None or lv <= 0.001:
                continue
            checked += 1
            want = min(1700.0, lv * 1000.0)
            if abs(fv - want) > 2.0:
                bad_fit.append((d, want))
            elif lv * 1000.0 < 1700.0:
                natural += 1      # only a row that AGREES may be counted as
                                  # "played at its own rate"
        if checked:
            print("  " + verdict(not bad_fit,
                                 "fit= is min(1700, clip length) on every window"
                                 " (%d checked)" % checked))
            if bad_fit:
                d0, w0 = bad_fit[0]
                print("        e.g. %s: len=%s fit=%s, expected %.0f."
                      % (d0.get("_ts", "?"), d0.get("len", "-"),
                         d0.get("fit", "-"), w0))
            if natural:
                print("  NOTE  %d/%d window(s) played the clip at its OWN rate"
                      " (fit<1700) - the record" % (natural, checked))
                print("        the new question selects is shorter than the window,"
                      " which is exactly the")
                print("        case the no-stretch clamp exists for.  It then holds"
                      " its last pose for the")
                print("        remainder (setLoop(false)) - a follow-through, not a"
                      " freeze bug.")
            else:
                print("  NOTE  no window played at its own rate ⇒ every record"
                      " selected this trip is")
                print("        still LONGER than the window (fit=kRideSwingTechMs"
                      " throughout), so the")
                print("        no-stretch clamp was inert - or the fit= check above"
                      " already failed.")
        else:
            print("  NOTE  no close row carried both fit= and len=, so the clamp is"
                  " unmeasured this trip.")

    # ---- 5) what this file cannot say --------------------------------------
    if swing_authored(s):
        print("  ⛔ TRIP 22 ANSWERED THIS SECTION, and the answer retires the whole family:"
              " the second")
        print("        question DID return a different record and it DID reach the skeleton,"
              " and the")
        print("        eyeball still read 「不是劈砍…平地上的动作在马上用有些放不开，刀砍不出去，"
              "只能在")
        print("        自己肚子那块拉，动作都挤成一团了」.  A ground record is authored around"
              " a STANDING")
        print("        pelvis; a seated one crushes it, whichever record is named."
              "  ⇒ the swing is")
        print("        authored now - judge it in -- T25 --.  gate=/dq=/lim= above are kept"
              " because the")
        print("        producer still names a technique per window (it is what lim= gates on).")
        return
    print("  NOTE  the verdict is the eyeball, as always.  Five outcomes:")
    print("          1. it reads as a chop ⇒ P4-3-4 is DONE.  Close T24, and the"
          " release decision")
    print("             (does this build replace v1.6 in release\\) becomes the"
          " next question.")
    print("          2. gate= == tech= on every row ⇒ see the CHECK above: the"
          " engine has one")
    print("             answer, so naming a clip from the §17.10 census is the"
          " only lever left.")
    print("          3. the blade is STILL held/pointed wrong ⇒ it is the WRIST,"
          " not the record:")
    print("             move 'Bip01 R Hand' (and if that is not enough 'Bip01 R"
          " Forearm') out of")
    print("             the T23 free table and into the hold table, so grip and"
          " blade orientation")
    print("             stay with 'mid blow' while the arm sweep stays with the"
          " technique.")
    print("          4. the new motion is legible but TOO SMALL ⇒ an in-place"
          " record has less")
    print("             amplitude by nature; prefer the longer of the two answers"
          " rather than")
    print("             raising a weight that is already 1.0.")
    print("          5. swing=0 for the whole trip ⇒ read the lim= check above"
          " FIRST; that is the")
    print("             one way this rung disarms the swing without anything"
          " visible saying so.")


def swing_aimed(s):
    """True when the build authors DIRECTIONS in skeleton space (T26) rather than
    joint angles about each bone's bind axes (T25).

    The two are the same machinery with a different parameterisation, so every
    mechanical criterion (freed==authored, arm=, armt=, kept=, armback) is shared and
    lives in -- T25/T26 --; only want=/dot=/bx= are T26's own, and they are what
    -- T26 -- judges.  Dispatch on want=, never on the build's byte count.
    """
    return any("want" in d for d in s.sw_arm)


def swing_dot_split(s):
    """Split the dot= samples into (settled, first_frame, unread).

    A sample whose kept= is negative is the FIRST authored frame of its window
    (RideSwingArmPose sets kept=-1.0 and only overwrites it when gRideSwingArmHeld was
    already true), and on that frame the cached derived transform we read still holds the
    HOST's pose ⇒ it measures the read, not the aim.  dot=-2.0000 means a zero-length
    vector on one side, i.e. never measured.  Both -- T26 -- and -- T27 -- judge only the
    settled population, so the partition lives here rather than in either of them.
    """
    settled, first, unread = [], [], []
    for d in s.sw_arm:
        v = fnum(d, "dot")
        if v is None:
            continue
        if v <= -1.5:
            unread.append((d, v))
        elif fnum(d, "kept") is not None and fnum(d, "kept") < 0.0:
            first.append((d, v))
        else:
            settled.append((d, v))
    return settled, first, unread


def report_swing_arm(s):
    """T25/T26 - the authored swing, the half both parameterisations share.

    Trip 22 closed the "play one of the engine's own records" family: the record was
    named, driven, masked and reached the skeleton, and it still read
    「不是劈砍…刀砍不出去，只能在自己肚子那块拉，动作都挤成一团了」, because a ground
    record is authored around a standing pelvis.  The user's ruling
    (「不一定非要和原版一样，只要像骑砍那样挥砍的动作就行」) drops the fidelity
    requirement, so the arm is hand-written now - the same machine as the straddle:
    manual control + a blend-mask 0 on every weighted clip, written at the pre-render
    point, handed back on the close edge and on dismount (§16, §21.2).

    Two self-proofs matter here and neither existed before: kept= (our write survived
    the clip's applyToNode) and out=/fore=/down= (where the hand actually went, in
    skeleton space).  ⚠️ They say the WRITE landed, not that the write was the right
    one: trip 23 passed every criterion in this section with an eyeball verdict of
    「往下戳」.  What that log was missing is want=/dot=, which is -- T26 --.
    """
    print("")
    print("== T25/T26 - the AUTHORED swing: we write the arm ourselves ==")
    if not (s.sw_arm or s.sw_armback or swing_authored(s)):
        print("  no SWING arm / armback line and no arm= on any ride line: this log predates")
        print("  the authored swing.  NOT a failure - T20..T24 above are that build's verdict.")
        return
    print("  parameterisation: %s"
          % ("DIRECTIONS in skeleton space (T26 - want=/dot= present)"
             if swing_aimed(s) else
             "joint angles about the bind axes (T25 - ⛔ retired by trip 23)"))

    # ---- 1) the freed set must be exactly the authored set ------------------
    rows = s.sw_free_bones
    if rows:
        names = [d.get("_bone", "?") for d in rows]
        bad = [d for d in rows if d.get("has") != "1"]
        print("  authored/freed bones: " + ", ".join(
            "%s=%s" % (n.replace("Bip01 ", ""), d.get("handle", "?"))
            for n, d in zip(names, rows)))
        print("  " + verdict(not bad and len(rows) == 2,
                             "the table is the two arm bones and both resolved by name"
                             " (%d row(s))" % len(rows)))
        if bad:
            print("        has=0 on: %s - that bone is freed from the host and never written,"
                  % ", ".join(d.get("_bone", "?") for d in bad))
            print("        so it renders at BIND for the whole window (§17.9's disease).")
        if len(rows) != 2:
            print("        ⚠️ %d entries, not 2.  The freed set and the authored set MUST be"
                  " identical:" % len(rows))
            print("        a freed-but-unwritten bone renders at bind, an authored-but-unfreed"
                  " one is")
            print("        overwritten by the clip.  An 11-row table is the T23/T24 shape.")
    else:
        print("  no SWING free bone row: the bone table never resolved (the leg-pose pass")
        print("  prints it once per DLL load), so nothing below can be trusted.")

    # ---- 2) did the arm get authored on the rides that swung ---------------
    bad_arm = []
    any_arm = False
    for d in s.sw_rides:
        sw, ar = fnum(d, "swing"), fnum(d, "arm")
        if sw is None or ar is None:
            continue
        any_arm = True
        print("     %8s  swing=%d arm=%d" % (d.get("_ts", "?"), int(sw), int(ar)))
        if int(sw) > 0 and int(ar) == 0:
            bad_arm.append(d.get("_ts", "?"))
    if any_arm:
        print("  " + verdict(not bad_arm,
                             "the arm was authored on every ride that swung"))
        if bad_arm:
            print("        arm=0 with swing>0 on: %s" % ", ".join(bad_arm[:6]))
            print("        The window opened and the leg-pose pass never authored a frame -"
                  " same cause")
            print("        as swfree=0 (RideSwingInFlight compares gRideSwingWho by pointer),"
                  " or")
            print("        gRideSwingOpenTick was 0 on every one of those frames.")

    # ---- 3) did the arc run to the end of every window ----------------------
    armts = [d for d in s.sw_close if "armt" in d]
    if armts:
        vals = [(d.get("_ts", "?"), fnum(d, "armt")) for d in armts]
        print("  armt= at close (arc progress, 1.00 = the last key was reached):")
        print("     " + " ".join("%s:%s" % (t, "-" if v is None else "%.2f" % v)
                                 for t, v in vals[:14]))
        short = [t for t, v in vals if v is not None and v < 0.95]
        never = [t for t, v in vals if v is not None and v < 0.0]
        print("  " + verdict(not short,
                             "every window ran the arc to its last key (%d checked)"
                             % len(vals)))
        if never:
            print("        armt=-1.00 means NOT ONE frame was authored in that window.")
        elif short:
            print("        cut short on: %s - the window closed before the arc did."
                  % ", ".join(short[:6]))
            print("        The window closes on 'mid blow' reaching kRideSwingDoneProg, so"
                  " kRideSwingArcMs")
            print("        has to stay under the SHORTEST window, not the average:"
                  " trip 23 measured")
            print("        1437..1781 ms and its 1437 ms window cut the 1700 ms arc off at"
                  " t=0.85, which")
            print("        is why the arc is 1400 now (it finishes, then HOLDS the last key ="
                  " ready).")
            print("        Lower the arc or lower the pinned clip's speed - do NOT raise"
                  " kRideSwingLenMs")
            print("        alone (it is a cap, and raising it needs kRideSwingMinGapMs raised"
                  " with it).")

    # ---- 4) THE self-proof: did our write survive the clip ------------------
    keeps = [v for v in (fnum(d, "kept") for d in s.sw_arm) if v is not None and v >= 0.0]
    if keeps:
        worst = min(keeps)
        print("  " + verdict(worst >= 0.99,
                             "our write held on every sample (worst kept=%.4f over %d"
                             " sample(s))" % (worst, len(keeps))))
        if worst < 0.99:
            print("        kept< 1 means the clip is writing these bones AFTER we do:"
                  " the blend-mask")
            print("        entry is not 0 for them on some weighted clip.  A manually"
                  " controlled bone")
            print("        survives Skeleton::reset() but STILL receives applyToNode -"
                  " only the mask")
            print("        protects it (§16).  Check that the freed table above and the"
                  " authored table")
            print("        are the same list.")
    elif s.sw_arm:
        print("  NOTE  every kept= sample is negative (-1.0000) = first authored frame of a"
              " window,")
        print("        which carries no previous write to compare against.  Unmeasured,"
              " not clean.")

    # ---- 5) where the hand actually went ------------------------------------
    if s.sw_arm:
        print("  arc samples (out = away from the body, fore = forward, down = downward,")
        print("               all in skeleton space, shoulder -> hand):")
        aimed = swing_aimed(s)
        for d in s.sw_arm[:18]:
            if aimed:
                print("     %8s  t=%-5s | out=%-7s fore=%-7s down=%-7s want=%-18s dot=%s"
                      % (d.get("_ts", "?"), d.get("t", "-"), d.get("out", "-"),
                         d.get("fore", "-"), d.get("down", "-"),
                         d.get("want", "-"), d.get("dot", "-")))
            else:
                print("     %8s  t=%-5s abd=%-7s flx=%-7s elb=%-7s | out=%-7s fore=%-7s down=%s"
                      % (d.get("_ts", "?"), d.get("t", "-"), d.get("abd", "-"),
                         d.get("flx", "-"), d.get("elb", "-"), d.get("out", "-"),
                         d.get("fore", "-"), d.get("down", "-")))
        spans = {}
        for k in ("out", "fore", "down"):
            v = [x for x in (fnum(d, k) for d in s.sw_arm) if x is not None]
            spans[k] = (min(v), max(v), max(v) - min(v)) if v else None
        line = []
        for k in ("out", "fore", "down"):
            if spans[k] is None:
                line.append("%s=?" % k)
            else:
                line.append("%s %.2f..%.2f (span %.2f)" % ((k,) + spans[k]))
        print("     travel: " + " | ".join(line))
        widest = max((spans[k][2] for k in spans if spans[k]), default=0.0)
        print("  " + verdict(widest >= 1.0,
                             "the hand really travelled (widest span %.2f units)" % widest))
        if widest < 1.0:
            print("        The writes hold (kept above) but the hand barely moves ⇒ the"
                  " rotation is")
            print("        going somewhere that does not carry the hand: wrong bone handles,"
                  " or the")
            print("        amplitude is being cancelled by a parent.  Read the handles in"
                  " section 1")
            print("        against the P2-1b bone inventory BEFORE changing any angle.")
        if not aimed:
            print("        ⚠️ A BIG SPAN IS NOT A GOOD SWING, and trip 23 is the proof: spans of")
            print("        7.55/9.12/7.64 on a 6.09 arm with kept=1.0000 read as 「往下戳」,"
                  " because")
            print("        bind-relative angles ride the host's torso.  Only want=/dot= (T26)"
                  " tells")
            print("        travel apart from the RIGHT travel, and this log has no want=.")

    # ---- 6) custody: the arm must be handed back ---------------------------
    if s.sw_armback:
        bad_man = [d for d in s.sw_armback if d.get("man") not in (None, "0x00")]
        dots = [v for v in (fnum(d, "minDot") for d in s.sw_armback) if v is not None]
        print("  armback: %d handback(s), worst minDot=%s, man!=0: %d"
              % (len(s.sw_armback),
                 "-" if not dots else "%.4f" % min(dots), len(bad_man)))
        print("  " + verdict(not bad_man and (not dots or min(dots) >= 0.999),
                             "every handback cleared the manual flag and returned to bind"))
        if bad_man or (dots and min(dots) < 0.999):
            print("        THIS is the leak that walks off the mount: an arm bone still under")
            print("        manual control keeps our last pose forever (Skeleton::reset() does"
                  " not")
            print("        touch it), which is a rider walking around with the sword up.")
    elif swing_authored(s):
        print("  CHECK no armback line at all.  If any window opened this trip, the arm was"
              " taken")
        print("        and never given back on the record - look for a rider with a raised"
              " arm.")

    # ---- 7) the shared part ends here; T26's own criteria are next ----------
    print("  NOTE  everything above says the WRITE landed, not that it was the right write.")
    print("        Trip 23 passed all of it and still read 「往下戳」.  The shape question is")
    print("        -- T26 -- (want=/dot=) plus the eyeball; the regressions that must hold")
    print("        alongside are: the four v1.6 behaviours, the straddle (takeovers ==")
    print("        restored + released, minDot=1.0000, residue=0), no standing upright, no")
    if is_t27(s):
        print("        sudden turn to the mount's rear, and the HOST still pinned - which on")
        print("        this build is 'guard 1h' for the whole window (hostkeep>0, pw>=0.95,")
        print("        judged in -- T27 --).  It owns everything the arm does not.")
    else:
        print("        sudden turn to the mount's rear, and 'mid blow' swapped in and pinned")
        print("        (guardoff>0, pw>=0.95) - it is the host for everything the arm does")
        print("        not own.")
    print("  NOTE  one eyeball check belongs to THIS section, not to T26: LOOK AT THE RIGHT ARM")
    print("        AFTER DISMOUNTING.  Section 6 is its log-side proof, but a manually")
    print("        controlled bone that is never released keeps our last pose for the rest of")
    print("        the session, which is a session-long disfigurement and outranks the shape.")


def report_swing_aim(s):
    """T26 - is the authored arc the arc that reached the screen?

    ⛔ WHY THIS SECTION EXISTS.  T25 authored JOINT ANGLES about each bone's bind axes,
    and trip 23 passed every mechanical criterion in -- T25/T26 -- with an eyeball
    verdict of 「更像是往下戳，位移像 \\ 這個符號」.  `tools\\armarc.py --log` found the
    reason with no model at all: two frames inside ONE window wrote the SAME three
    angles and the hand landed 72.3 deg apart, while |measured|/|predicted| never left
    1.04.  A bind-relative angle only fixes a bone against its PARENT, and the parent -
    clavicle, spine - is host-driven by 'mid blow' at speed 2.5 (the shoulder itself
    travelled 3.0/4.8/5.1 units inside a window).  The screen was showing our arc TIMES
    the host's torso sweep.

    ✅ T26 aims each bone's own +X along a direction in SKELETON space instead, by
    cancelling the parent: local = conj(parentDerived) * UNIT_X.getRotationTo(dir).  The
    hand then sits at exactly lenFore*dirUpper + lenHand*dirForearm - both bind child
    offsets are pure +X - so the DLL can compute the vector it MEANT (want=) and dot it
    against the vector it MEASURED.

    🔑 dot>=0.99 is the headline criterion, and it is a real falsifier, not a tautology:
    want= is arithmetic on the authored table, the measured half is two
    _getDerivedPosition() reads through the live skeleton.  A low dot says the parent
    cancellation did not work - which is also the one thing that would catch a wrong
    _getDerivedOrientation vtable slot.

    ⚠️ ONE carve-out, and it is stated in section 1 rather than applied quietly: samples
    with kept<0 are a window's FIRST authored frame, where the cached derived transform
    still holds the HOST's pose, so they are counted and printed separately instead of
    judged.  Trip 24's worst sample (0.8268) is exactly one of those.
    """
    print("")
    print("== T26 - the aimed swing: did the arc we authored reach the skeleton ==")
    if not s.sw_arm:
        print("  no SWING arm sample at all: nothing to judge here (see -- T25/T26 -- above).")
        return
    if not swing_aimed(s):
        print("  no want= on any SWING arm sample: this log predates the aimed swing (T26).")
        print("  NOT a failure - it is a T25 log, and -- T25/T26 -- above is its verdict.")
        print("  ⚠️ Passing that section is NOT passing this one: trip 23 did exactly that.")
        return

    # ---- 1) THE criterion: intended vs measured ----------------------------
    # ⚠️ ONE CARVE-OUT, pre-registered here BEFORE the trip it will be applied to, and
    # narrow on purpose: a sample whose kept= is negative is the FIRST authored frame of
    # its window (RideSwingArmPose sets kept=-1 when gRideSwingArmHeld was false), and on
    # that frame the hand position we read is still the one the HOST left there.
    # `_getDerivedPosition` is const in the linked header (§21.5) and returns the cached
    # transform, so every sample is really one frame behind - which is ~2 deg at arc rates
    # but a whole pose on the frame the host hands over.  Such a sample measures the READ,
    # not the aim, so it is reported with its own count instead of being folded into the
    # headline.  Trip 24 is exactly this shape: worst 0.8268 on a t=0.00 kept=-1.0000
    # sample, mean 0.9920 over 36.  ⚠️ It is a carve-out, not a pardon: if the FIRST-frame
    # numbers are bad AND the settled ones are only just passing, read both.
    settled, first, unread = swing_dot_split(s)
    dots = [v for _, v in settled]
    if dots:
        worst = min(dots)
        print("  dot= (intended vs measured shoulder->hand): worst %.4f, mean %.4f, %d sample(s)"
              % (worst, sum(dots) / len(dots), len(dots)))
        print("  " + verdict(worst >= 0.99,
                             "the hand went where the table said, on every settled sample"))
        if worst < 0.99:
            print("        worst dot=%.4f = %.1f deg off the intended direction."
                  % (worst, math.degrees(math.acos(max(-1.0, min(1.0, worst))))))
            print("        This falsifies the T26 paragraph in RidingPlugin.cpp, so read it in"
                  " this order:")
            print("          a. is kept= still ~1.0000 above?  If not, the mask is the bug,"
                  " not the aim.")
            print("          b. len= below: the two bind bone lengths must be ~2.85 / ~3.24."
                  "  A zero")
            print("             means the bone was not resolved and want= was computed from a"
                  " short arm.")
            print("          c. a CONSTANT offset on every sample ⇒ the parent cancellation is"
                  " inverted")
            print("             (conj vs the quaternion itself); an offset that GROWS with the"
                  " torso ⇒")
            print("             _getDerivedOrientation returned a stale or wrong value ="
                  " the vtable slot.")
            print("          d. only the LAST bone is off ⇒ the Forearm is reading its parent"
                  " back from")
            print("             the node instead of using the UpperArm's own aim (see the ⚠️ in"
                  " the")
            print("             RideSwingArmPose header) - that is a fresh-cache assumption,"
                  " and it fails.")
            # ---- lag or aim? -------------------------------------------------
            # Every sample is one frame behind by construction (const _getDerivedPosition,
            # §21.5), so SOME deficit is expected wherever the arc is moving fast.  The
            # separator is arithmetic, not another trip: the intended direction's own motion
            # between two adjacent log rows, divided by the rows' spacing in authored frames
            # (kRideSwingArmLogGap in the DLL - ⚠️ change that constant and change ARM_GAP),
            # is what ONE frame of lag on OUR write can cost.  A deficit near that estimate
            # is the read; a deficit far above it is not ours to explain by lag, and the next
            # suspect is the PARENT read (R Clavicle, host-driven - see (c) above).
            ARM_GAP = 12.0
            prev = None
            rows = []
            for d, v in settled:
                w = triple(d, "want")
                step = tri_angle(prev, w) if prev is not None else None
                if v < 0.99:
                    rows.append((d, v, step))
                if w is not None:
                    prev = w
            if rows:
                print("        which of the two it is, per failing sample"
                      " (deficit vs what ONE frame")
                print("        of lag could cost at that point in the arc):")
                for d, v, step in rows[:6]:
                    deficit = math.degrees(math.acos(max(-1.0, min(1.0, v))))
                    if step is None:
                        est = "  n/a (no previous want= row)"
                    else:
                        est = "  1-frame lag <= %.1f deg (row-to-row %.1f deg / %d frames)" % (
                            step / ARM_GAP, step, int(ARM_GAP))
                        est += "  ⇒ %s" % ("READ LAG" if deficit <= 2.5 * step / ARM_GAP
                                           else "NOT lag on our write")
                    print("          %8s t=%-5s dot=%.4f = %5.1f deg off;%s"
                          % (d.get("_ts", "?"), d.get("t", "-"), v, deficit, est))
                print("        ⚠️ 'NOT lag on our write' does not mean the table is wrong -"
                      " the parent")
                print("           read is stale by the same one frame, and the parent is"
                      " whatever clip")
                print("           holds R Clavicle.  A quieter host should shrink this"
                      " column; see -- T27 --.")
    elif first:
        print("  CHECK every dot= sample is a window's FIRST authored frame (kept<0), so the")
        print("        criterion was never measured on a settled frame.  Unjudged, not passed -")
        print("        raise kRideSwingArmLines or lower kRideSwingArmLogGap and re-run.")
    else:
        print("  CHECK every sample has dot=-2.0000 (or none at all) = shoulder->hand or want=")
        print("        was degenerate, so THE criterion of this rung was never measured."
              "  Unjudged,")
        print("        not passed.")
    if first:
        fw = min(v for _, v in first)
        print("        (%d first-frame sample(s), reported separately: worst dot=%.4f."
              "  kept=-1 means" % (len(first), fw))
        print("         the previous frame was the HOST's pose, so the cached derived"
              " transform this")
        print("         read comes from is the handover frame - it measures the read lag,"
              " not the aim.)")
    if unread:
        print("        (%d sample(s) reported dot=-2.0000 = a zero-length vector on one side.)"
              % len(unread))


    # ---- 2) the arm lengths want= was built from ---------------------------
    lens = [d.get("len") for d in s.sw_arm if d.get("len")]
    if lens:
        uniq = sorted(set(lens))
        print("  len= (bind UpperArm->Forearm / Forearm->Hand, read from the live skeleton): %s"
              % ", ".join(uniq[:4]))
        ok = True
        for v in uniq:
            a, _, b = v.partition("/")
            try:
                if not (2.0 <= float(a) <= 4.0 and 2.0 <= float(b) <= 4.5):
                    ok = False
            except ValueError:
                ok = False
        print("  " + verdict(ok, "both bone lengths are the measured bind arm"
                                 " (offline: 2.849 / 3.244)"))
        if not ok:
            print("        A 0.00 half means that bone did not resolve this frame, so want= was")
            print("        computed from a shorter arm than the game drew - dot= above is then")
            print("        measuring the tool, not the code.")

    # ---- 3) did the authored path actually get traversed -------------------
    wants = [triple(d, "want") for d in s.sw_arm]
    wants = [w for w in wants if w]
    if wants:
        cols = ("out", "fore", "down")
        got = []
        for i, k in enumerate(cols):
            v = [w[i] for w in wants]
            got.append((k, min(v), max(v), max(v) - min(v)))
        print("  want= spans: " + " | ".join("%s %.2f..%.2f (span %.2f)" % g for g in got))
        if is_t28(s):
            print("     offline table (tools\\armarc.py, T28's ARC2): out span 7.26 |"
                  " fore 3.83 | down 6.19")
        else:
            print("     offline table (tools\\armarc.py, the RETIRED ARC_DIR): out span 6.20 |"
                  " fore 6.09 | down 8.93")
        widest = max(g[3] for g in got)
        print("  " + verdict(widest >= 4.0,
                             "the windows sampled a real slice of the arc"
                             " (widest want= span %.2f)" % widest))
        if widest < 4.0:
            print("        The table is fine; the SAMPLING is thin.  kRideSwingArmLogGap (12"
                  " frames) x")
            print("        kRideSwingArmLines (30 since T28) has to cover kRideSwingArcMs -"
                  " with a short window")
            print("        or a spent line budget the log can miss the cut entirely."
                  "  Read armt= above:")
            print("        if armt reached 1.00, the ARC ran and only the LOG is short.")

    # ---- 4) the blade, as data instead of a guess --------------------------
    bxs = [triple(d, "bx") for d in s.sw_arm]
    bxs = [b for b in bxs if b]
    if bxs:
        cols = ("out", "fore", "down")
        got = []
        for i, k in enumerate(cols):
            v = [b[i] for b in bxs]
            got.append((k, min(v), max(v), max(v) - min(v)))
        print("  bx= (R Hand bone axis, the blade proxy): "
              + " | ".join("%s %.2f..%.2f (span %.2f)" % g for g in got))
        if is_t28(s):
            print("  ⚠️ T28: these RAW spans are expected to be LARGE - the hand travels with"
                  " the arm now.")
            print("        The grip criterion is the angle between bx= and the arm, in"
                  " -- T28 -- section 2;")
            print("        judging 正手/反手 off the raw spans is what these numbers"
                  " cannot do.")
        else:
            print("  NOTE  no criterion on purpose - nothing has ever measured which axis of the"
                  " hand")
            print("        the weapon follows.  The wrist is left to the host DELIBERATELY (it is"
                  " what")
            print("        kept the grip sane through T24), so this is here to answer 「刀身朝向对不对」")
            print("        with data on the NEXT round instead of a guess.  A bx= that barely moves")
            print("        while the hand travels = the blade keeps one attitude through the whole")
            print("        stroke, which is the 'stab, not slash' look; if that is the eyeball")
            print("        verdict, 'Bip01 R Hand' goes into BOTH tables (freed AND authored),"
                  " never one.")

    # ---- 5) what the eyeball has to answer this trip -----------------------
    if is_t28(s):
        print("  NOTE  the shape is judged in -- T28 -- section 7 for this build - the four")
        print("        outcomes there replace the ones that used to print here, because two of")
        print("        them (aim freely / edit the directions) are what T28 deleted.")
        return
    print("  NOTE  the shape is still not in this file.  Four outcomes:")
    print("          1. it reads as a sabre cut ⇒ P4-3-4 is DONE.  Close T26; what is left is")
    print("             the release decision (does this build replace v1.6 in release\\).")
    print("          2. dot>=0.99 and it STILL does not read as a swing ⇒ the machinery is")
    print("             finally out of the way and the TABLE is the only thing left."
          "  Edit the")
    print("             five rows of kRideSwingArc and armarc.py's ARC2 TOGETHER,"
          " look at the")
    print("             offline plot first, and change one key at a time.")
    print("          3. right shape, wrong TIMING (too slow to read as a cut, or over before")
    print("             it is seen) ⇒ kRideSwingArcMs, which must stay under the shortest")
    print("             window (trip 23: 1437 ms).  The arc holds its last key after t=1.")
    print("          4. the arm is right but the BLADE is wrong ⇒ section 4's bx=, and the")
    print("             wrist rule there.")


def report_swing_host(s):
    """T27 - the window KEEPS its host: does holding 'guard 1h' through the stroke work?

    ⛔ WHY THIS SECTION EXISTS.  Trip 24 (T26's aimed arc) got the eyeball to
    「有点劈砍的意思了」 and named what was left: 「角色的右手总是想找左手因为原版就是
    双手劈砍的，所以把动作带崩了」.  The mechanism is one step off that reading - the right
    HAND's position is already ours (dot= mean 0.9920 over 36 samples) so no clip is
    pulling that hand - but the conclusion holds.  What the swapped-in 'mid blow' still
    owned was everything the arc does NOT: the LEFT arm, the right WRIST and the SPINE.
    It is one of the six `blow` records, all 'whole,action,norm,reloc,restrict' =
    two-handed committed strike (doc.md :245), so its left-arm track kept reaching across
    for a grip our arc had already carried away.

    ✅ T27 therefore stops swapping the host at all: 'guard 1h' (UPPER, LOOP,
    weaponTypeFlags bit 0x04 = ONE-HANDED, no whole/reloc - doc.md :248) stays pinned
    straight through the window and the authored arc cuts on top of it.  Three things
    come free - no root motion inside the window, a wrist that keeps a one-handed
    attitude for the whole stroke, and a window length that is our own arc's rather than
    a restatement of 'mid blow's.

    WHAT THAT RETIRES, and this section states it so nothing reads a design decision as
    a regression: rst= (nothing to restart), sp= (a loop's speed is meaningless), prog=
    as a close test (a LOOP's progress cycles), and §U's ClipPin-door dodge (the door is
    `target + others <= 1.02f` and after this change only ONE clip ever asks for 1.0).

    WHAT IT PUTS AT RISK, which is what the checks below are: the guard has to survive
    being held for 1650 ms with our writes on two of its bones (pinst=/pw=/psw=/oen=),
    the window has to close on OUR clock (ms= ~ kRideSwingWinMs), and the retired
    assertion sites have to stay retired (rst=/drv=/hold=/fit= all 0).
    """
    print("")
    print("== T27 - the window keeps its host ('guard 1h' straight through) ==")
    if not (s.sw_rides or s.sw_close):
        print("  no P43SW row at all - nothing to judge (see T20 above).")
        return
    if not is_t27(s):
        print("  close rows carry guardoff=, not hostkeep=: this log is from a build whose")
        print("  window still SWAPPED 'mid blow' onto the body.  NOT a failure - T20/T21"
              " above")
        print("  are its verdicts.  ⚠️ Passing those is not passing this: they judge the swap")
        print("  this rung deletes.")
        return

    # ---- 1) the host guarantee, at its strongest ---------------------------
    # The guard is resolved once per DLL load on the stance's first frame.  On T20..T26
    # this line was background; here it is the precondition for the whole rung, because
    # the clip it names is the body's host for every frame of every window.
    if s.k_guard is None:
        print("  CHECK no `P41K resolve` line in this log at all.  It is UNGATED and prints"
              " on the")
        print("        stance's first frame, so its absence means the stance never armed"
              " - read")
        print("        -- T18 -- and stop here; nothing below is judgeable.")
    else:
        print("  " + verdict(s.k_guard == "found",
                             "'guard 1h' resolved in the rider's own table (guard=%s)"
                             % s.k_guard))
        if s.k_guard != "found":
            print("        gP41kGuard is NULL, so RideSwingPass's `if (!host)` gate refuses"
                  " EVERY window")
            print("        (noclip= in T20 counts them) and the stance has no host either."
                  "  This is not")
            print("        a swing bug - FindAnimData could not find the clip in this rider's"
                  " table.")
        if s.k_blow is not None:
            print("  NOTE  blow='mid blow' %s.  T27 only RESOLVES it - nothing requests it any"
                  % s.k_blow)
            print("        more.  The line is kept as standing evidence that the rider's table"
                  " really")
            print("        does hold a swing record, which is what makes the retreat in §17.18"
                  " possible.")

    # ---- 2) did the host survive being held through the stroke -------------
    # ⚠️ THE FIELDS ARE THE SAME ONES T21 READ AND THEY DESCRIBE A DIFFERENT CLIP.
    # RideSwingProbePin is handed `host`, which was gP41kBlow up to T26 and is gP41kGuard
    # now, so pinst=/pw=/psw=/oen= on a T27 close row are the GUARD's.  pw is the
    # SingleAnimation weight, oen the Ogre state's enabled flag: both at the close edge,
    # i.e. after the whole window has run with our two bones written every frame.
    closes = [d for d in s.sw_close if "pinst" in d]
    if closes:
        st = [(d.get("_ts", "?"), (d.get("pinst") or "?")) for d in closes]
        dead = [t for t, v in st if v != "live"]
        print("  " + verdict(not dead,
                             "the guard's entry was still live at every close (%d/%d)"
                             % (len(closes) - len(dead), len(closes))))
        if dead:
            print("        pinst=none/AV at %s.  none = the guard is no longer in the"
                  " playing list, which" % ", ".join(dead[:6]))
            print("        is trip 17's hostless skeleton with a new cause (T27 never"
                  " withholds it, so")
            print("        something else dropped it); AV = the layer walk hit a dangling"
                  " list.")
        pws = [v for v in (fnum(d, "pw") for d in closes) if v is not None]
        if pws:
            print("  " + verdict(min(pws) >= 0.95,
                                 "the guard kept its render weight through the window"
                                 " (worst pw=%.3f of %d)" % (min(pws), len(pws))))
            if min(pws) < 0.95:
                print("        The guard is pinned to 1.0 at BOTH sites every frame, so a"
                      " sagging pw means")
                print("        something is draining it while the window is open."
                      "  ClipPin's door")
                print("        (`target + others <= 1.02f`) is the first suspect, and on T27"
                      " it should be")
                print("        the LEAST likely it has ever been: only one clip asks for 1.0"
                      " now.  Read the")
                print("        P41K weight rows for that timestamp before touching the door"
                      " (⛔ TASK.md")
                print("        :326-334 says the door is not to be rewritten).")
        sw = [d.get("psw") for d in closes]
        oe = [d.get("oen") for d in closes]
        badsw = [x for x in sw if x is not None and x != "1"]
        badoe = [x for x in oe if x is not None and x != "1"]
        if any(x is not None for x in sw) or any(x is not None for x in oe):
            print("  " + verdict(not badsw and not badoe,
                                 "stillWanted and the Ogre state stayed on (psw!=1: %d,"
                                 " oen!=1: %d)" % (len(badsw), len(badoe))))
            if badsw or badoe:
                print("        psw=0 means our per-frame request stopped reaching the entry;"
                      " oen=0 means the")
                print("        entry is there but its Ogre state is disabled, so the pose"
                      " is not being")
                print("        applied at all.  Either one makes every 'kept=1.0000' above"
                      " a write onto a")
                print("        body nobody is drawing.")
        # The pair that answers "does holding the host cost the stance anything": pw on the
        # OPEN row is sampled at the open decision, pw on the close row after 1650 ms of
        # window.  ⚠️ pre pinst=live is the EXPECTED shape here and it was NOT on T21 -
        # there the probe looked at a one-shot whose entry did not exist yet (pinst=none
        # 14/14).  Here it looks at a clip that has been playing since the stance began.
        opens = [d for d in s.sw_open if "pinst" in d]
        if opens:
            pre_dead = [d.get("_ts", "?") for d in opens
                        if (d.get("pinst") or "?") != "live"]
            print("  " + verdict(not pre_dead,
                                 "the guard was ALREADY playing at every open decision"
                                 " (%d/%d live)"
                                 % (len(opens) - len(pre_dead), len(opens))))
            if pre_dead:
                print("        pre pinst=none at %s: the stance's own host was not in the"
                      " playing list when" % ", ".join(pre_dead[:6]))
                print("        the window opened.  On T21 that reading was ROUTINE (the"
                      " one-shot's entry was")
                print("        built a frame later); on T27 it is news, because the guard"
                      " is supposed to have")
                print("        been pinned since the stance's first frame.")
            po = [v for v in (fnum(d, "pw") for d in opens) if v is not None]
            if po and pws:
                print("  guard weight: open worst %.3f -> close worst %.3f  (held for the"
                      " whole window)" % (min(po), min(pws)))

    # ---- 3) the window closed on OUR clock ---------------------------------
    # kRideSwingArcMs 1400 < kRideSwingWinMs 1650 < kRideSwingLenMs 3000 (cap) <
    # kRideSwingMinGapMs 3200.  ms= below 1650 is structurally impossible (the close edge
    # only fires once `now - openTick >= kRideSwingWinMs`), and ms= at the 3000 cap means
    # the CAP closed the window - which after T27 can only happen if an open tick outlived
    # a per-ride reset, not because a clip ran long.
    WIN, CAP = 1650, 3000
    mss = [(d.get("_ts", "?"), v) for d in s.sw_close
           for v in (fnum(d, "ms"),) if v is not None]
    if mss:
        vals = [v for _, v in mss]
        early = [(t, v) for t, v in mss if v < WIN - 1]
        capped = [(t, v) for t, v in mss if v >= CAP - 60]
        print("  window lengths: %s ms  (expected ~%d = kRideSwingWinMs; cap %d)"
              % ("/".join("%d" % v for v in vals[:12]), WIN, CAP))
        print("  " + verdict(not early and not capped,
                             "every window closed on the arc's own clock (%d window(s))"
                             % len(vals)))
        if early:
            print("        ms < %d at %s.  The close edge cannot fire before that by"
                  " construction, so" % (WIN, ", ".join(t for t, _ in early[:6])))
            print("        either kRideSwingWinMs was changed without changing this section,"
                  " or the open")
            print("        tick was rewritten mid-window (gRideSwingOpenTick is cleared by"
                  " the per-ride")
            print("        reset and by the rider-mismatch branch - both should also clear"
                  " gRideSwingWasOpen).")
        if capped:
            print("        ms at the %d ms CAP at %s: kRideSwingLenMs closed the window,"
                  " not kRideSwingWinMs." % (CAP, ", ".join(t for t, _ in capped[:6])))
            print("        On T27 the cap is a pure safety net, so a hit means the window's"
                  " own bound was")
            print("        not applied - read the `open` computation, not the tuning.")
        print("  NOTE  the ARC's completion is armt= in -- T25/T26 --, not ms=."
              "  kRideSwingArcMs")
        print("        (1400) finishes ~250 ms before the window does, and holds its last"
              " key for the")
        print("        remainder - that tail IS the settle pose, and it is why the two"
              " numbers differ.")

    # ---- 4) the retired assertion sites must STAY retired ------------------
    # Four counters, four deleted call sites.  This is the check that catches the swap or
    # the drive being reintroduced by a later edit - each one is a design decision of this
    # rung, so a nonzero value is a code change, never a tuning artefact.
    zero_bad = []
    for d in s.sw_rides:
        for k in ("rst", "drv"):
            v = fnum(d, k)
            if v is not None and v != 0:
                zero_bad.append((d.get("_ts", "?"), k, v))
    for d in s.sw_close:
        for k in ("hold", "fit"):
            v = fnum(d, k)
            if v is not None and v != 0:
                zero_bad.append((d.get("_ts", "?"), k, v))
    print("  " + verdict(not zero_bad,
                         "the four retired sites are all silent (rst/drv/hold/fit = 0)"))
    if zero_bad:
        for t, k, v in zero_bad[:8]:
            print("        %8s  %s=%g - that call site was REMOVED in this rung." % (t, k, v))
        print("        rst= is RideSwingRestart (no one-shot to restart), drv=/hold=/fit="
              " are T22's")
        print("        drive of the technique's own Ogre state, its blend mask and T24's"
              " phase fit.")
        print("        ⚠️ Do not re-attach the drive to 'fix' a stiff swing: its mask frees"
              " only eight")
        print("        bones and would fight the authored arm.  The answer to 「太僵」 is the"
              " spine")
        print("        (P4-1M's twist is already ours, RidingPlugin.cpp:6295).")

    # ---- 5) the three per-frame counters have to agree ---------------------
    # hostkeep= is counted in HaltAndForceSitPass (render side), arm= and swfree= in
    # LegPosePass, all three on every frame a window is in flight.  Trip 24 measured
    # 636/635/635 - a one-frame offset from the open frame, before the leg pass runs.
    # A real gap means one of the two passes is not running while the other thinks a
    # window is open, which is a pass-order or rider-identity fault, not tuning.
    rows = [d for d in s.sw_rides if (fnum(d, "swing", 0) or 0) > 0]
    if rows:
        bad_agree = []
        for d in rows:
            hk = hostkeep(d)
            ar = fnum(d, "arm")
            fr = fnum(d, "swfree")
            if hk is None or ar is None:
                continue
            tol = max(8.0, 0.02 * hk)
            if abs(hk - ar) > tol or (fr is not None and abs(hk - fr) > tol):
                bad_agree.append((d.get("_ts", "?"), hk, ar, fr))
        print("  " + verdict(not bad_agree,
                             "hostkeep / arm / swfree agree on every ride that swung"
                             " (%d ride(s))" % len(rows)))
        if bad_agree:
            for t, hk, ar, fr in bad_agree[:6]:
                print("        %8s  hostkeep=%g arm=%g swfree=%s"
                      % (t, hk, ar, "-" if fr is None else "%g" % fr))
            print("        All three count frames INSIDE a live window, from two passes"
                  " that both run")
            print("        once per frame, so they can only diverge if one pass is not"
                  " reached (an early")
            print("        return above it) or the two disagree about WHICH rider is"
                  " swinging")
            print("        (gRideSwingWho).  arm << hostkeep in particular means the"
                  " window held the host")
            print("        for frames that authored nothing - which renders the freed bones"
                  " at BIND (§17.9).")

    # ---- 6) the aim, which is a LAG term and now has a bigger rate to lag behind ----
    # want= cancels the parent through `conj(parentDerived)`, and that read is one frame
    # stale (const _getDerivedOrientation, §21.5) ⇒ the deficit in dot= scales with how fast
    # the whole arm is turning: the host's contribution (R Clavicle, 'guard 1h', a LOOP at
    # 1.0) plus OUR OWN arc rate.  Trips 24→25→26 differenced the 9.6 deg residual by moving
    # one thing at a time: host 0.6 deg, model 1.7 deg, remainder 7.3 = the stale read.
    # 🆕 T29 RAISES THE ARC'S PEAK RATE ON PURPOSE, 341 → 461 deg/s (a real hold at the top
    # buys a faster descent).  A lag term must loosen with that, by construction:
    #   scaled expectation = 7.3 * 461/341 = 9.9 deg, and that is an UPPER bound because
    #   part of the 7.3 is the host, which did not change ⇒ anything at or under 9.9 is the
    #   same defect at a higher speed, NOT a regression.
    # ⛔ AND NOT A REASON TO TOUCH THE ARC.  This number is the price of the tempo the trip
    # is buying; retuning the table to flatter it would undo the change under test.  The only
    # honest fix is a fresh parent read (§21.5), which is a different rung entirely.
    # Baseline = trip 26 (BE962686, diagnostics OFF, same host, same model, the shape being
    # replaced), SETTLED samples only: worst 0.9918 (7.3 deg), mean 0.9984, 98 samples.
    if swing_aimed(s):
        settled, first24, _unread = swing_dot_split(s)
        vals = [v for _, v in settled]
        if vals:
            BASE_W, BASE_DEG = 0.9918, 7.3
            BAR_DEG = 11.0          # 9.9 scaled + 1.1 slack; see the derivation above
            worst = min(vals)
            wdeg = math.degrees(math.acos(max(-1.0, min(1.0, worst))))
            print("  aim vs a 1.35x faster arc: settled worst %.4f (%.1f deg), mean %.4f,"
                  " %d sample(s)" % (worst, wdeg, sum(vals) / len(vals), len(vals)))
            print("  " + verdict(wdeg <= BAR_DEG,
                                 "within the rate-scaled bar %.1f deg (trip 26 was %.4f /"
                                 " %.1f deg at 341 deg/s)" % (BAR_DEG, BASE_W, BASE_DEG)))
            if wdeg <= BASE_DEG:
                print("        ⇒ and it did not loosen AT ALL despite +35%% peak rate, which"
                      " says the")
                print("          residual is host/parent rather than ours by an even wider"
                      " margin than")
                print("          the trip 24→26 differencing showed.  Worth a line in §17.19.")
            elif wdeg > BAR_DEG:
                print("        Loosened by MORE than the rate scaling can explain ⇒ something"
                      " other than")
                print("          lag.  Check WITNESS 1 (r= flat) and kept= first: if either"
                      " moved, the")
                print("          rigid-body model is being fought, and the arc table is"
                      " downstream of that.")
                print("        ⛔ Do not answer this by softening the arc - that would retune"
                      " away the")
                print("          very tempo this trip is testing.")

    # ---- 7) what only the eyeball can answer this trip ---------------------
    print("  NOTE  the shape is STILL not in this file, and 🆕 T29 changed the ARC ITSELF (the")
    print("        window slid up the same circle: -100/+45 → -130/+25, plus a hold at each")
    print("        end).  Trip 26 already accepted the direction (「侧面张开大臂带动刀，简单美")
    print("        观」) - what this trip asks is whether it now reads as a CHOP.  Outcomes:")
    print("          1. it reads as a downward cut ⇒ P4-3-4 is DONE.  Close T29; what is left")
    print("             is the release decision (does this build replace v1.6 in release\\).")
    print("          2. STILL 「侧面」 ⇒ the endpoints are already vert/lat 2.17, so the")
    print("             remaining sideways component is the circle's own mid-stroke bulge (out")
    print("             reaches 5.21 between the two keys, +2.34 past either end) and NO arc")
    print("             table can remove it.  That is the AXIS dial, and the measured fallback")
    print("             is a sagittal axis (log ≈ out 1 / fore 0 / down 0), which holds out")
    print("             constant at 1.41 for the whole stroke.  ⚠️ Both mirrors together.")
    print("          3. 「太僵」 / the body does not join in ⇒ author the SPINE.  P4-1M's"
          " twist")
    print("             is already ours (RidingPlugin.cpp:6295) and 'Bip01 Spine' would"
          " re-enter")
    print("             BOTH tables together.  ⛔ NOT by bringing a ground record back:"
          " that is the")
    print("             wall trips 20/22/24 hit three different ways.")
    print("          4. the raise is now too big / the blade clips the head ⇒ offline says it")
    print("             does not (cock out 2.87 down -4.41 vs head base out -1.83 down -2.22),")
    print("             so trust the eyeball over that and pull the cock key back toward -115.")
    print("          5. the arm is right and the BLADE is wrong ⇒ bx=/bz= in -- T26 --"
          " section 4,")
    print("             and the wrist rule there ('Bip01 R Hand' into BOTH tables or"
          " neither).")
    print("  NOTE  the regressions that must hold alongside, each judged in its own section:")
    print("        the four v1.6 behaviours (P43RD/P43SUP), the straddle (takeovers ==")
    print("        restored + released, minDot=1.0000, residue=0, dropped=0), no standing")
    print("        upright on the mount, no sudden turn to its rear (hdveto=), and zero AV.")
    print("        ⚠️ AND THE ONE EYEBALL CHECK THAT OUTRANKS THE SHAPE: look at the right"
          " arm")
    print("        AFTER DISMOUNTING (-- T25/T26 -- section 6 is its log-side proof only).")


def swing_arm_windows(s):
    """s.sw_arm split into windows: t only ever increases inside one.

    The arm line carries no window number (it is throttled by kRideSwingArmLogGap and
    budgeted by kRideSwingArmLines, so it cannot be tied to a close row by counting), and
    every criterion in -- T28 -- is a WITHIN-window statement.  A t that goes backwards is
    the only boundary marker there is, and it is a sound one: t is elapsedMs/kRideSwingArcMs
    clamped to 1, so it is monotonic per window by construction.
    """
    wins, cur = [], []
    for d in s.sw_arm:
        t = fnum(d, "t")
        if t is None:
            continue
        if cur and t < fnum(cur[-1], "t", 0.0) - 1e-9:
            wins.append(cur)
            cur = []
        cur.append(d)
    if cur:
        wins.append(cur)
    return wins


def is_t28(s):
    """T28 rows carry deg= and r=; every earlier arm line carried neither."""
    return any("deg" in d and "r" in d for d in s.sw_arm)


def is_t30(s):
    """T30 rows carry elbow=; T28/T29 carried deg=/cone= and no elbow=.

    Dispatch on the FIELD NAME, never on a trip number or a byte count - the same rule
    is_t27() follows, and the reason is sharper here than anywhere else in this file: T28
    needed the elbow FROZEN and T30 needs it to MOVE, so the two sections' criteria are each
    other's negation.  Guessing the family from anything but the field is how a working build
    gets failed by the previous rung's bar.
    ⚠️ `elb=` is NOT this field.  T25's retired joint-angle line printed abd=/flx=/elb=, and
    kv() keys those separately, so a T25 log cannot be mistaken for a T30 one.
    """
    return any("elbow" in d for d in s.sw_arm)


def _len_pair(d):
    """An arm row's len=<UpperArm->Forearm>/<Forearm->Hand> as two floats, or None.

    Same rule as fnum_flagged(): one unparsable half drops the pair rather than reading as a
    zero, because a 0.00 there is the log SAYING a bone did not resolve that frame.
    """
    v = d.get("len")
    if not v:
        return None
    a, _, b = v.partition("/")
    try:
        return (float(a), float(b))
    except ValueError:
        return None


def arm_vec(d):
    """The shoulder->hand vector off an arm row's FLAT out=/fore=/down= fields.

    Not triple(): the arm line prints this one loose ("out=%.2f fore=%.2f down=%.2f") because
    it predates the (a,b,c) tuples beside it, while want=/bx=/bz=/sh= are tuples.  Same rule
    as everywhere else - one missing component drops the row rather than reading as a zero.
    """
    v = (fnum(d, "out"), fnum(d, "fore"), fnum(d, "down"))
    return None if None in v else v


def _t28_deg(d):
    """An arm row's own deg=, formatted, or '-' when the build never printed one."""
    v = fnum(d, "deg")
    return "-" if v is None else "%.1f" % v


def report_swing_arc(s):
    """T28 - ONE rigid rotation of a captured pose.  Did the elbow stay put and the grip hold?

    ⛔ WHAT TRIP 25 SAID, and it named a mechanism, not a taste:
    「本来是正手拿刀，动作是正手变反手然后从右侧劈出。只需要正手拿刀劈出就好。劈砍动作我试了
    一下，大臂旋转小臂不动，带动小臂，带动刀。其他和以前一样」  The DIRECTION of the cut was
    never the complaint - only the flip and the elbow.

    BOTH HALVES WERE MEASURABLE IN THAT SAME LOG, offline, and both came out against the
    retired model (`python tools\\armarc.py --log <trip-25 log>` reprints all of it):
      * |shoulder->hand| moved by 0.617 .. 0.868 units inside every window, on a 5.4-unit
        arm.  A rotation about the shoulder cannot change that length, so the elbow was
        demonstrably flexing - and in the retired table's own numbers the forearm swung
        ~124 deg while the upper arm turned 27.7, the exact inverse of 「大臂旋转小臂不动」.
      * the angle between the weapon hand's own +X and the arm wandered 27.1 .. 54.0 deg
        per window.  That IS 正手变反手, quantified: `UNIT_X.getRotationTo(dir)` is the
        MINIMAL rotation onto a direction, so it fixes the bone's axis and leaves the roll
        about it to fall out of the arc - and 'Bip01 R Hand' inherits every degree.

    ✅ T28 replaces the two authored DIRECTIONS with one authored ANGLE: capture the upper
    arm's derived orientation and the forearm's LOCAL orientation at window open, then write
    derived_upper = S(t) * refUp and local_forearm = refFo verbatim every frame.  A constant
    forearm local makes the elbow angle constant BY CONSTRUCTION, so the whole arm-and-blade
    assembly is one rigid body turning about the shoulder.  Both complaints therefore become
    arithmetic identities, and the two numbers above become this section's criteria.

    ⚠️ AND THE PRICE, so nobody reads it as a bug: a rigid rotation traces a CONE.  Reach
    and elbow angle are whatever 'guard 1h' happens to be holding - we do not choose where
    the hand ends up any more.  The only two dials left are the AXIS and the ANGLE PROFILE.
    """
    print("")
    print("== T28 - one rigid rotation: the elbow frozen, the grip carried ==")
    if not s.sw_arm:
        print("  no SWING arm sample at all - nothing to judge (see -- T25/T26 -- above).")
        return
    if not is_t28(s):
        if is_t30(s):
            print("  the arm rows carry elbow= and no deg=: this is a T30 log (vanilla's own two")
            print("  baked curves played as a delta), and it is judged in -- T30 -- below."
                  "  ⚠️ NOT")
            print("  failed here, and not judgeable here either: T28 needed the elbow FROZEN and"
                  " T30")
            print("  needs it to MOVE, so every bar in this section is the negation of the one"
                  " that")
            print("  applies.  Running them anyway would fail a working build.")
            return
        print("  the arm rows carry no deg=/r=: this log predates T28, whose whole model is"
              " ONE")
        print("  angle about one axis.  NOT a failure - -- T25/T26 -- and -- T27 -- are its")
        print("  verdicts.  ⚠️ Passing those is not passing this: they judge two independently")
        print("  aimed directions, which is the thing this rung deletes.")
        return
    wins = swing_arm_windows(s)
    print("  %d sample(s) in %d window(s) of samples (the arm line is throttled and budgeted,"
          % (len(s.sw_arm), len(wins)))
    print("  so this is not the number of windows - P43SW ride's swing= is).")

    # ---- 1) THE ELBOW.  The one criterion no read lag can degrade. ---------
    # r= is |shoulder->hand| straight off the two node positions.  Under our writes the whole
    # chain below the shoulder is a rigid rotation of the captured pose, so this length is
    # invariant - and a stale read cannot spoil it either, because a lagged sample is a
    # ROTATED sample and a rotation preserves length.  It needs no model, no bind pose and no
    # copy of the arc table, which makes it the sharpest instrument this file has ever had on
    # the swing.  Baseline to beat: trip 25's 0.617 .. 0.868 per window (armarc.py --log).
    FLAT = 0.05
    spreads = []
    for i, w in enumerate(wins):
        rs = [v for v in (fnum(d, "r") for d in w) if v is not None]
        if len(rs) >= 2:
            spreads.append((i + 1, min(rs), max(rs), max(rs) - min(rs), len(rs)))
    if not spreads:
        print("  CHECK no window has two r= samples - the arm line budget"
              " (kRideSwingArmLines) or the")
        print("        throttle (kRideSwingArmLogGap) is starving the one measurement this"
              " rung turns on.")
    else:
        worst = max(spreads, key=lambda x: x[3])
        for n, lo, hi, sp, cnt in spreads:
            print("    win%-2d n=%-3d r=%.3f .. %.3f   spread %.3f" % (n, cnt, lo, hi, sp))
        print("  " + verdict(worst[3] <= FLAT,
                             "|shoulder->hand| is FLAT in every window (worst spread %.3f <="
                             " %.2f) = 「大臂旋转小臂不动」" % (worst[3], FLAT)))
        if worst[3] > FLAT:
            print("        win%d spans %.3f .. %.3f.  Three things could do that and none of"
                  " them is tuning:" % (worst[0], worst[1], worst[2]))
            print("          a) the forearm write is not landing - then kept= in -- T25/T26 --"
                  " is also")
            print("             below 1.0, and the mask is the suspect (§17.9), not the arc.")
            print("          b) the capture went stale - refUp/refFo are cleared at the close"
                  " edge, at")
            print("             both per-ride resets and in RideSwingArmRelease; a survivor"
                  " would freeze")
            print("             an elbow angle that never existed, and r= would STEP once"
                  " rather than drift.")
            print("          c) 'guard 1h' keys the HAND bone's local POSITION (we own"
                  " rotations only).")
            print("             The only innocent explanation, and it would show up here and"
                  " nowhere else,")
            print("             as a drift correlated with t.")
            print("        ⛔ Do not answer this by editing kRideSwingArc: an angle profile"
                  " cannot change")
            print("        a length the model holds constant.")

    # ---- 2) THE GRIP.  Also model-free, also lag-proof. ---------------------
    # angle(bx, arm) = the weapon hand's own +X against the shoulder->hand vector.  Under one
    # rigid rotation BOTH turn by the same S(t), so the angle BETWEEN them is preserved - which
    # makes its spread a direct reading of 正手变反手 needing no axis constant, no quaternion
    # algebra and no second copy of the arc table to drift out of sync.
    # ⚠️ NOT the raw bx= spans in -- T26 -- section 4: those are EXPECTED to be large now,
    # because the hand is supposed to travel with the arm.  Only the part that survives after
    # the shared rotation is removed is a flip, and this is that part.
    # Baseline (trip 25, `python tools\\armarc.py --log`): spread 27.1 / 30.7 / 46.6 / 52.5 /
    # 54.0 deg per window, 1.7 .. 55.7 overall = the flip the user saw, quantified.
    GRIP = 15.0
    grips = []
    for i, w in enumerate(wins):
        a = [v for v in (tri_angle(triple(d, "bx"), arm_vec(d)) for d in w) if v is not None]
        if len(a) >= 2:
            grips.append((i + 1, min(a), max(a), max(a) - min(a), len(a)))
    if not grips:
        print("  CHECK no window has two bx= samples with a hand vector beside them - the grip")
        print("        criterion is UNJUDGED (not passed).")
    else:
        gworst = max(grips, key=lambda x: x[3])
        for n, lo, hi, sp, cnt in grips:
            print("    win%-2d n=%-3d angle(bx,arm)=%5.1f .. %5.1f deg   spread %5.1f"
                  % (n, cnt, lo, hi, sp))
        print("  " + verdict(gworst[3] <= GRIP,
                             "the blade keeps its attitude relative to the arm (worst spread"
                             " %.1f <= %.0f deg) = 正手 stays 正手" % (gworst[3], GRIP)))
        if gworst[3] > GRIP:
            print("        win%d wanders %.1f .. %.1f deg.  Read it in this order:"
                  % (gworst[0], gworst[1], gworst[2]))
            print("          a) WITNESS 1 also failed ⇒ one cause, not two: the forearm local"
                  " is not")
            print("             constant, so nothing about the assembly is rigid.  Fix that"
                  " first.")
            print("          b) WITNESS 1 passed ⇒ the rotation IS rigid and the wrist is"
                  " moving on")
            print("             its own.  'Bip01 R Hand' is deliberately in NEITHER table"
                  " (§17.14), so")
            print("             that motion is 'guard 1h' - a LOOP - keying the hand, and the"
                  " remedy is")
            print("             to put that bone in BOTH tables (freed AND authored), never"
                  " one.")
            print("        ⛔ Not a table question either: every key in kRideSwingArc rotates"
                  " hand and")
            print("        blade together, so no angle profile can widen or narrow this"
                  " number.")

    # ---- 3) DID THE LOG SEE THE CUT, and is the axis worth anything ---------
    # armt= in -- T27 -- says the ARC ran; this says the SAMPLING saw it.  The table's extremes
    # are -130 (cock) and +25 (through), 155 deg swept, and one sample per
    # kRideSwingArmLogGap=12 authored frames can miss either end - hence bars inside them.
    # 🆕 T29 re-registered both bars because the table moved.  How they were derived (offline,
    # armarc.py): sweep every phase offset of a 1-row-per-12-frames sampler over the 1400 ms arc.
    # The cock is HELD 196 ms (≈12 frames at 60 fps) so a sample lands in it at every phase ⇒
    # worst-phase min is the full -130, and -120 is slack.  The through key is held 112 ms and the
    # arc is above +15 for 212 ms total ⇒ worst-phase max seen is +17 at 60 fps.  ⚠️ At 30 fps
    # that bound drops to -2, so if max= comes in short, count the arm rows per window FIRST
    # (≈7-8 rows/window ⇒ ~60 fps, which is what trips 24-26 ran at); a low frame rate loosens
    # this criterion and nothing else.
    degs = [v for v in (fnum(d, "deg") for d in s.sw_arm) if v is not None]
    if degs:
        print("  deg= sampled %.1f .. %.1f   (kRideSwingArc: 0 / -130 cock+hold /"
              " +25 through+hold / 0)" % (min(degs), max(degs)))
        print("  " + verdict(min(degs) <= -120.0 and max(degs) >= 15.0,
                             "the samples cover both ends of the stroke"))
        if not (min(degs) <= -120.0 and max(degs) >= 15.0):
            print("        A stroke can be missing from the LOG and present on screen."
                  "  Split them:")
            print("          armt=1.00 in -- T27 -- ⇒ the arc ran, the line budget"
                  " (kRideSwingArmLines")
            print("            = 30/ride) or the gap (12 frames) is what is short."
                  "  Not a failure.")
            print("          armt<1.00 ⇒ the window closed before the arc finished, which is a"
                  " TIMING")
            print("            invariant, not a table one: kRideSwingArcMs 1400 <"
                  " kRideSwingWinMs 1650.")
            print("          deg= stuck at 0.0 ⇒ t is not advancing at all; read"
                  " RideSwingArcAt's caller,")
            print("            not the table.")
    cones = [v for v in (fnum(d, "cone") for d in s.sw_arm) if v is not None and v >= 0.0]
    if cones:
        print("  cone= %.1f .. %.1f deg  (per WINDOW, captured once: the axis against the UPPER"
              " ARM's" % (min(cones), max(cones)))
        print("        own +X, i.e. how wide a circle this rotation draws)")
        print("  " + verdict(25.0 <= min(cones) <= 155.0 and max(cones) <= 155.0,
                             "the axis is well off the arm's own length, so the rotation MOVES"
                             " the hand"))
        print("  ⚠️ NOT armarc.py's 77.0: that is the axis against the shoulder->hand VECTOR,"
              " which")
        print("     this log cannot become (a sample is two segments summed)."
              "  Cousins, not copies -")
        print("     never 'fix' one to match the other.")
        if not (25.0 <= min(cones) <= 155.0 and max(cones) <= 155.0):
            print("        A cone near 0 or 180 is the one failure that passes every other"
                  " criterion in")
            print("        this file: the arc rolls the arm about its own length, 155 deg get"
                  " swept, and")
            print("        the hand barely moves - r= flat, grip flat, kept=1.0, a dead stroke."
                  "  This is")
            print("        the AXIS dial (kRideSwingAxisOut/Fore/Down + armarc.py's AXIS,"
                  " edited together),")
            print("        and it is the only criterion here that points at it.")

    # ---- 4) THE CAPTURE.  A freed bone nobody wrote renders at BIND. --------
    # noref= counts frames that wanted to author but could not resolve BOTH arm bones.  The two
    # are freed by the same two flags they are authored by, so any nonzero reading is a frame
    # with a host-masked bone that nobody wrote - §17.9, the 「在牛背上站直」 family, except
    # localised to the arm.
    nrows = [d for d in s.sw_close if "noref" in d]
    if not nrows:
        if s.sw_close:
            print("  CHECK close rows carry no noref= - that field is T28's own, so either the"
                  " line")
            print("        budget (kRideSwingLines) was spent or this is not a T28 build after"
                  " all.")
    else:
        tot = sum(int(fnum(d, "noref", 0.0)) for d in nrows)
        print("  noref= over %d close row(s): %d" % (len(nrows), tot))
        print("  " + verdict(tot == 0,
                             "every authored frame resolved both bones (no freed-but-unwritten"
                             " bone)"))
        if tot:
            print("        Each counted frame left 'R UpperArm'/'R Forearm' masked off the host"
                  " AND")
            print("        unwritten by us ⇒ that frame drew them at the BIND pose."
                  "  The suspect is the")
            print("        skeleton instance being rebuilt under us (§16 - which is why both"
                  " bones are")
            print("        re-resolved BY NAME every frame), not the arc.")

    # ---- 5) THE HANDOVER, which this model makes free ----------------------
    # S(0) = identity, so the first authored frame writes the pose ALREADY on screen: aim[0] is
    # the captured host orientation itself and aim[1] = aim[0]*refFo is the host's own forearm.
    # want= and the read therefore come from the same cache on that frame, and dot= should be
    # ~1.  Trip 25's aimed table had no such property and paid for it: its three first frames
    # read dot=0.8449 (32.3 deg off) / 0.9711 / 0.9659.  This is the cheapest criterion in the
    # section and the one that says the model's zero point is where the code thinks it is.
    settled, first, unread = swing_dot_split(s)
    if first:
        vals = [v for _, v in first]
        print("  first authored frame (kept<0): "
              + " / ".join("dot=%.4f at deg=%s" % (v, _t28_deg(d)) for d, v in first))
        worstf = min(vals)
        print("  " + verdict(worstf >= 0.99,
                             "the window opens ON the host's own pose (worst first-frame"
                             " dot=%.4f) = no handover pop" % worstf))
        if worstf < 0.99:
            print("        %.1f deg off on a frame whose own deg= is within a few degrees of 0,"
                  % math.degrees(math.acos(max(-1.0, min(1.0, worstf)))))
            print("        i.e. where S(t) is still ~identity ⇒ this is NOT the table: the"
                  " capture and")
            print("        the read disagree inside one frame.  Two candidates, both cheap:"
                  " len= in")
            print("        -- T26 -- must be 2.85/3.24 (a zero = a bone that did not resolve, so"
                  " want= was")
            print("        built from a short arm), and _getDerivedOrientation vs"
                  " _getDerivedPosition must")
            print("        be reading the same point in the frame (§21.5).")
    else:
        print("  NOTE  no sample landed on a window's first authored frame (kept<0), so the")
        print("        handover is UNJUDGED this trip - it is throttle luck, not a failure.")

    # ---- 6) dot=, and why its bar moves with the tempo ----------------------
    # dot= compares THIS frame's intent against a transform read one frame late, so it scales
    # with how fast the intent is moving - and 🆕 T29 moves it faster again.  The arithmetic,
    # from the table alone: -130 -> +25 is 155 deg across 0.24 * kRideSwingArcMs = 336 ms =
    # 461 deg/s (T28's cut was 105 deg / 308 ms = 341), so one frame of lag costs 461/fps
    # degrees (~5.1 deg at the ~90 fps trip 25's own frame stamps imply).  A deficit near that
    # is the READ; a deficit far above it is the parent (R Clavicle, host-driven) exactly as in
    # trip 24.  -- T26 -- already prints that separator per failing sample; this only moves the
    # bar: trip 26 measured 7.3 deg at 341 deg/s, and 7.3 * 461/341 = 9.9 deg is what the same
    # defect costs at this speed.  Bar = 11.0 deg (0.9816) = that plus slack.
    if settled:
        sv = [v for _, v in settled]
        worst_s = min(sv)
        mean_s = sum(sv) / len(sv)
        print("  settled dot=: worst %.4f = %.1f deg, mean %.4f over %d sample(s)"
              % (worst_s, math.degrees(math.acos(max(-1.0, min(1.0, worst_s)))),
                 mean_s, len(sv)))
        print("     trip-26 baseline: worst 0.9918 = 7.3 deg, mean 0.9984 at 341 deg/s;"
              " rate-scaled")
        print("     expectation here 9.9 deg (0.9851), bar 11.0 deg (0.9816)")
        print("  " + verdict(worst_s >= 0.9816,
                             "the lag term grew no faster than the arc's peak rate did"))
        print("  ⛔ Whatever this reads, it is NOT answered by editing kRideSwingArc."
              "  dot= measures")
        print("     whether our write landed where we aimed it; the SHAPE is the eyeball's"
              " question and")
        print("     WITNESS 1/2 above are the model's.  Chasing dot= with the angle profile is"
              " how a")
        print("     round gets spent on the tool instead of the stroke - and this trip BUYS"
              " rate with")
        print("     the table, so flattering dot= would mean undoing the change under test.")

    # ---- 7) what only the eyeball can answer, and what each answer costs ----
    print("  NOTE  the SHAPE is still not in this file.  Four outcomes and their next rungs:")
    print("          1. it reads as a sabre cut ⇒ P4-3-4 is DONE; what is left is the release")
    print("             decision (does this build replace v1.6 in release\\).")
    print("          2. elbow and grip both pass and it STILL does not read as a cut ⇒ 🆕 T29")
    print("             has now SPENT the ANGLE PROFILE dial (the window slid up the same circle")
    print("             to -130/+25, endpoints vert/lat 0.83 → 2.17), so what is left is the")
    print("             AXIS: the circle's own mid-stroke bulge (out 5.21, +2.34 past either")
    print("             key) is unreachable from the table, and the measured fallback is a")
    print("             sagittal axis (log ≈ out 1 / fore 0 / down 0), which holds out constant")
    print("             at 1.41 the whole stroke.  ⚠️ Both dials are mirrored in tools\\armarc.py")
    print("             (AXIS / ARC2) and nothing in the build can catch the copies drifting -")
    print("             edit them together and look at the offline plot before rebuilding.")
    print("          3. the arm is in the right plane but too bent / too short / reaching wrong")
    print("             ⇒ that is the PRICE of a rigid rotation, not a bug: reach and elbow are")
    print("             whatever 'guard 1h' holds.  Authoring the elbow again is a NEW rung and")
    print("             it re-opens 正手变反手, which is what this one exists to close.")
    print("          4. the arm is right and the BLADE is wrong ⇒ section 2's remedy (the hand")
    print("             bone into BOTH tables), not the arc.")
    print("  ⚠️ WHAT THIS SECTION DOES NOT COVER, so nobody reads a pass here as a pass:")
    print("       -- T25/T26 --  kept= (our write survived applyToNode), the freed==authored"
          " table,")
    print("                      len=, armback (man=0x00 / minDot~1.0 = the arm was handed"
          " back).")
    print("       -- T27 --      pinst=live at every close, armt=1.00, ms= vs the 1650/3000"
          " pair,")
    print("                      the four retired counters (rst/drv/hold/fit = 0) and the")
    print("                      hostkeep/arm/swfree agreement.")
    print("       and the regressions that have nothing to do with the arm: zero AV, straddle")
    print("       takeovers == restored, dropped=0, and the eyeball's 「其他和以前一样」.")


def report_swing_bake(s):
    """T30 - vanilla's own 'chop down' replayed as a DELTA on the captured pose.  Did it land?

    THE MODEL IN ONE LINE:  local(t) = capturedLocal * X(t), one baked 27-row table per bone,
    X(0) = identity on both.  Three consequences, and each is a criterion below:
      * the window still opens on the pose already on screen (T28's one good property, kept).
      * NO PARENT IS READ TO BUILD THE POSE.  T28 wrote the upper arm in derived space and had
        to divide out a one-frame-stale parentDerived - the 7.3 deg residual that trips
        24->25->26 differenced down.  A local write has no such term, so dot= drops from a
        MECHANISM to a diagnostic (section 7).
      * THE ELBOW IS ANIMATED AGAIN, by vanilla's own hinge - the one thing a rigid rotation
        could not do at all.

    ⚠️⚠️ EVERY CRITERION HERE HAS THE OPPOSITE SIGN FROM -- T28 --.  T28 was ONE rigid
    rotation, so it needed |shoulder->hand| FLAT and measured span 0.000 over 13 windows; that
    same reading here means the forearm table never reached the bone.  The two sections dispatch
    on the field name (deg= vs elbow=) precisely so nobody carries a bar across.

    ⚠️ AND THE HONEST LIMIT, unchanged since trip 23: 「往下戳」 was a correctly measured curve
    pointing the wrong way.  Every number in this section can pass on a stroke that reads wrong
    on screen, because the DIRECTION half needs an anchor and no anchor exists in the log alone.
    That half is `python tools\\armarc.py --log <log>` (WITNESS 2's DIRECTION line, anchored once
    per window at its own first sample) - a separate implementation on a separate code path, so
    the two should agree number for number and disagreement is a tool bug in one of them.
    """
    print("")
    print("== T30 - vanilla's curve as a delta: did the ELBOW come back? ==")
    if not s.sw_arm:
        print("  no SWING arm sample at all - nothing to judge (see -- T25/T26 -- above).")
        return
    if not is_t30(s):
        print("  the arm rows carry no elbow=: this log predates T30, whose whole model is"
              " vanilla's")
        print("  two baked curves.  NOT a failure - -- T28 -- above is this log's verdict,"
              " and every")
        print("  criterion here is the negation of one there, so this section stays silent.")
        return
    wins = swing_arm_windows(s)
    print("  %d sample(s) in %d window(s) of samples (throttled one per"
          " kRideSwingArmLogGap=12" % (len(s.sw_arm), len(wins)))
    print("  authored frames, budgeted kRideSwingArmLines=30 per RIDE, so this is not the"
          " number of")
    print("  windows - P43SW ride's swing= is.  ~133 ms between samples at trip-26 frame"
          " rates.)")

    # Offline reference for every bar below.  Regenerate with `python tools\armarc.py --bake`,
    # which re-bakes the clip out of male_skeleton.skeleton, diffs it against the .cpp's two
    # tables value by value and REFUSES to report on a mismatch.
    # ⚠️ These numbers describe THE SHIPPED TABLE.  A different clip, --map lead/stretch or
    # --abs moves all of them, and then this block is stale rather than wrong - re-read it off
    # that report in the same commit, the way the AXIS/ARC2 mirror used to demand.
    OFF_R, OFF_EL, OFF_GRIP = (2.90, 5.42), (56.0, 126.0), 28.4
    DIP_LO, DIP_HI = 0.04, 0.18   # window t where offline r <= 3.62 and elbow <= 72.6
    MOVE_R, MOVE_EL = 1.5, 40.0   # the two pooled bars, against T28's FLAT = 0.05
    T28FLAT = 0.05

    # ---- 1) WITNESS 1: r= and elbow= must MOVE.  T28's criterion, inverted. -
    # Both come from three DERIVED POSITIONS and a law of cosines - no model, no bind pose, no
    # copy of either table - and a one-frame-stale read cannot spoil either, because a lagged
    # sample is a ROTATED sample and a rotation preserves both.  That is what makes them the
    # sharpest instruments in this file, and it is why they are also the pair whose sign flipped.
    # ⚠️ JUDGED POOLED, not per window, and the reason is sampling arithmetic rather than
    # laxity: offline r only dips below 3.62 for t in 0.055..0.166 = a 155 ms band of a 1400 ms
    # arc, while the throttle leaves ~133 ms between samples.  A window can therefore miss the
    # cocking phase entirely and span under 1.0 with nothing wrong with it.  Pooled over every
    # window the band is sampled many times over, so the pooled span is the build-level fact.
    # The per-window table is still printed, and a window that DID cover the dip and still reads
    # T28-flat is called out separately - that is a stale capture, not a sampling gap.
    per = []
    for i, w in enumerate(wins):
        tv = [v for v in (fnum(d, "t") for d in w) if v is not None]
        rs = [v for v in (fnum(d, "r") for d in w) if v is not None]
        es = [v for v in (fnum(d, "elbow") for d in w) if v is not None and v >= 0.0]
        dip = any(DIP_LO <= v <= DIP_HI for v in tv)
        per.append((i + 1, len(w), tv, rs, es, dip))
        print("    win%-2d n=%-3d t %.2f..%.2f   r %s   elbow %s   dip=%s"
              % (i + 1, len(w), min(tv) if tv else 0.0, max(tv) if tv else 0.0,
                 ("%4.2f..%4.2f span %4.2f" % (min(rs), max(rs), max(rs) - min(rs)))
                 if len(rs) >= 2 else "     (n<2)      ",
                 ("%3.0f..%3.0f span %3.0f" % (min(es), max(es), max(es) - min(es)))
                 if len(es) >= 2 else "    (n<2)    ",
                 "yes" if dip else "NO "))
    pr = [v for v in (fnum(d, "r") for d in s.sw_arm) if v is not None]
    pe = [v for v in (fnum(d, "elbow") for d in s.sw_arm) if v is not None and v >= 0.0]
    if len(pr) < 2 or len(pe) < 2:
        print("  CHECK fewer than two r=/elbow= samples in the whole log - the one measurement"
              " this")
        print("        rung turns on is starved (kRideSwingArmLines / kRideSwingArmLogGap).")
    else:
        rsp, esp = max(pr) - min(pr), max(pe) - min(pe)
        print("  pooled, every sample:  r %.2f..%.2f (span %.2f)   elbow %.0f..%.0f (span %.0f)"
              % (min(pr), max(pr), rsp, min(pe), max(pe), esp))
        print("  offline, the table:    r %.2f..%.2f (span %.2f)   elbow %.0f..%.0f (span %.0f)"
              % (OFF_R[0], OFF_R[1], OFF_R[1] - OFF_R[0],
                 OFF_EL[0], OFF_EL[1], OFF_EL[1] - OFF_EL[0]))
        print("  T28/T29 measured span 0.000 over 13 windows and that was its PASS."
              "  Here it is the")
        print("  failure: a frozen elbow means kRideSwingBakeFo never reached the bone.")
        print("  " + verdict(rsp >= MOVE_R and esp >= MOVE_EL,
                             "both MOVE (r span %.2f >= %.1f u, elbow span %.0f >= %.0f deg) ="
                             " vanilla's hinge is on the bone" % (rsp, MOVE_R, esp, MOVE_EL)))
        if rsp < MOVE_R or esp < MOVE_EL:
            print("        Read it in this order, and none of the four is a tuning question:")
            print("          a) elbow= is CONSTANT to the last digit ⇒ the forearm write is not")
            print("             landing at all.  kept= in -- T25/T26 -- is then also below 1.0 and"
                  " the")
            print("             mask is the suspect (§17.9), not the table.")
            print("          b) elbow= moves a little and r= barely ⇒ RideSwingBakeAt is being"
                  " asked")
            print("             for the wrong t, or kRideSwingBakeFoKeys disagrees with the table"
                  " length.")
            print("             `python tools\\armarc.py --mirror` decides that offline in one"
                  " second.")
            print("          c) every window's dip= reads NO ⇒ this is a SAMPLING gap, not a"
                  " failure:")
            print("             the throttle never landed a sample in the cocking phase.  Re-read"
                  " the")
            print("             t ranges above before touching anything.")
            print("          d) the window was cut short (armt= < 1.00 in -- T27 --) ⇒ the arc"
                  " never")
            print("             played, so there was no shape to see.")
        blind = [p for p in per if p[5] and len(p[3]) >= 2
                 and (max(p[3]) - min(p[3])) <= T28FLAT]
        if blind:
            print("  CHECK win%s covered the cocking phase (dip=yes) and still read T28-flat"
                  " (<= %.2f u)."
                  % (", ".join(str(p[0]) for p in blind), T28FLAT))
            print("        A sampling gap cannot do that.  The suspect is the CAPTURE: refUp/refFo"
                  " are")
            print("        cleared at the close edge, at every ride boundary and in"
                  " RideSwingArmRelease,")
            print("        and a survivor replays the delta off a pose from the previous window -"
                  " which")
            print("        shows up as one window flat while its neighbours move.")
    print("  ⚠️ the one innocent way to break this: 'guard 1h' keying the HAND bone's local"
          " POSITION")
    print("     (we own rotations only).  That would move r= without moving the elbow, and it"
          " would")
    print("     show up here and nowhere else in the file.")
    # ---- 2) THE GRIP, which is now a MATCH test rather than a flatness one --
    # angle(bx, arm) = the weapon hand's own +X against the shoulder->hand vector.  Still
    # anchor-free and still lag-proof (both vectors turn together), but the VERDICT flipped with
    # the model: T28 rotated the whole limb rigidly so this had to be ~0, while T30 bends the
    # elbow, which moves the hand's axis relative to the arm BY CONSTRUCTION.
    # 🔑 WHERE THE 正手 CLAIM NOW LIVES: it is proven OFFLINE, not here.  The forearm's baked
    # curve is a pure hinge about the bone's own -Y (x = z = 0.000000 on all 27 keys, which
    # `--mirror` re-derives from the asset every run), and a rotation perpendicular to the bone
    # axis induces no twist about it, so 'Bip01 R Hand' keeps the roll it was captured with.  The
    # upper arm's curve DOES twist - that is vanilla's shoulder, i.e. part of the chop.
    # ⚠️ SO THIS SPAN CAN NO LONGER DETECT A FLIP, and pretending otherwise would be the worst
    # kind of pass: trip 25's flip measured 27.1..54.0 deg per window and the offline T30
    # prediction is 28.4 - the two ranges OVERLAP.  The sharp instrument is the per-sample
    # residual against the offline curve (armarc.py WITNESS 2's `grip` line); this span only
    # catches a stroke that moves by the wrong AMOUNT.
    GRIP_LO, GRIP_HI = 8.0, 45.0
    grips = []
    for i, w in enumerate(wins):
        a = [v for v in (tri_angle(triple(d, "bx"), arm_vec(d)) for d in w) if v is not None]
        if len(a) >= 2:
            grips.append((i + 1, min(a), max(a), max(a) - min(a), len(a)))
    if not grips:
        print("  CHECK no window has two bx= samples with a hand vector beside them - the grip")
        print("        criterion is UNJUDGED (not passed).")
    else:
        for n, lo, hi, sp, cnt in grips:
            print("    win%-2d n=%-3d angle(bx,arm)=%5.1f ..%6.1f deg   spread %5.1f"
                  % (n, cnt, lo, hi, sp))
        gsp = max(g[3] for g in grips)
        print("    offline, the whole stroke: 1.8 .. 30.2 deg, spread %.1f  <- what the spreads"
              " above" % OFF_GRIP)
        print("    should look like; a window that missed the cocking phase legitimately"
              " shows less.")
        print("  " + verdict(GRIP_LO <= gsp <= GRIP_HI,
                             "the blade's attitude moves by about the amount the table says"
                             " (widest %.1f deg, band %.0f..%.0f)"
                             % (gsp, GRIP_LO, GRIP_HI)))
        if gsp < GRIP_LO:
            print("        ⚠️ a T28-shaped ~0 spread is now a FAILURE, and it is the SAME fault as")
            print("        WITNESS 1's: a rigid limb.  Fix that first; this number follows it.")
        elif gsp > GRIP_HI:
            print("        More motion than the table can explain.  WITNESS 1 decides which half:")
            print("          a) WITNESS 1 also out of range ⇒ one cause, and it is the tables.")
            print("          b) WITNESS 1 passed ⇒ the wrist is moving on its own."
                  "  'Bip01 R Hand' is")
            print("             deliberately in NEITHER table (§17.14), so that motion is"
                  " 'guard 1h'")
            print("             keying the hand, and the remedy is that bone into BOTH tables,"
                  " never one.")
        print("  ⛔ passing this is NOT a no-flip proof (see the comment above): 正手 is the"
              " eyeball's")
        print("     call plus the offline hinge check, and this span overlaps trip 25's flip.")
    # ---- 3) THE SETTLE CLOSES THE LOOP.  T28 had no equivalent. ------------
    # The last clip key sits at t=0.690 and bake_curve appends a SYNTHETIC key at t=1.000 whose
    # delta is identity, so from 967 ms to 1400 ms the arm interpolates back onto the pose it was
    # captured from.  That makes a prediction needing neither table nor anchor: a window's LAST
    # sample must read the same r= and elbow= as its FIRST one, because both are the captured
    # pose.  It is the cheapest test of the settle key AND of the handback - an arm that does not
    # come home here is an arm that pops when the window closes.
    # ⚠️ Only judgeable on windows the throttle happened to sample at both ends (the phase is
    # per-RIDE, so a window's first authored frame is not guaranteed a line).  Unjudged is
    # throttle luck, not a failure - the same rule -- T28 -- section 5 follows.
    # ⚠️⚠️ BOTH GATES ARE TIGHT ON PURPOSE, and a dry run against a synthetic PERFECT log is what
    # set them.  Loosening either one breaks the criterion rather than widening it:
    #   * t <= 0.005 - i.e. a printed t=0.00, the same frame kept<0 marks - because the cocking
    #     starts on the FIRST key and is steep: t=0.02 already reads r 4.86 (0.56 u gone in 28
    #     ms), so 0.02 was measured to admit a mid-stroke sample and self-fail a perfect log.
    #     The %.2f quantisation is worth at most ~0.14 u here, well inside the bar below.
    #   * t >= 0.99 because between the last clip key (t=0.690) and the synthetic settle key
    #     (t=1.000) the arm is still INTERPOLATING home: at t=0.94 it is ~80% of the way, worth
    #     0.7 u of r.  Only t=1.00 is the captured pose, and it is reachable: the arc clamps t to
    #     1 while the window runs on to kRideSwingWinMs, so ~250 ms (1-2 samples) sit at 1.00.
    OPEN_T, SETTLE_T, CLOSE_R, CLOSE_EL = 0.005, 0.99, 0.25, 8.0


    loops = []
    for (n, _cnt, _tv, _rs, _es, _dip), w in zip(per, wins):
        a = [d for d in w if fnum(d, "t") is not None and fnum(d, "t") <= OPEN_T]
        b = [d for d in w if fnum(d, "t") is not None and fnum(d, "t") >= SETTLE_T]
        if not a or not b:
            continue
        r0, r1 = fnum(a[0], "r"), fnum(b[-1], "r")
        e0, e1 = fnum(a[0], "elbow"), fnum(b[-1], "elbow")
        if None in (r0, r1, e0, e1) or e0 < 0.0 or e1 < 0.0:
            continue
        loops.append((n, r0, r1, abs(r1 - r0), e0, e1, abs(e1 - e0)))
    if not loops:
        print("  NOTE  no window carries a sample at BOTH t<=%.3f and t>=%.2f, so the settle is"
              % (OPEN_T, SETTLE_T))
        print("        UNJUDGED this trip - throttle luck, not a failure (trip 26 caught the"
              " first")
        print("        authored frame in 5 of 13 windows, so expect this to be judgeable"
              " sometimes).")

    else:
        for n, r0, r1, dr, e0, e1, de in loops:
            print("    win%-2d r %.2f -> %.2f (%+.2f)   elbow %.0f -> %.0f (%+.0f deg)"
                  % (n, r0, r1, r1 - r0, e0, e1, e1 - e0))
        wr = max(l[3] for l in loops)
        we = max(l[6] for l in loops)
        print("  " + verdict(wr <= CLOSE_R and we <= CLOSE_EL,
                             "every sampled window came home to its captured pose (worst %.2f u /"
                             " %.0f deg, bars %.2f / %.0f)" % (wr, we, CLOSE_R, CLOSE_EL)))
        if wr > CLOSE_R or we > CLOSE_EL:
            print("        The arm ends the window somewhere the capture never was, so the close"
                  " edge")
            print("        has to snap it back.  Two candidates and they are told apart by the"
                  " sign:")
            print("          a) it does not return at all ⇒ the synthetic settle key is missing"
                  " from")
            print("             the .cpp tables (both must end `{ 1.0000f, 1.0, 0, 0, 0 }`);"
                  " --mirror")
            print("             checks exactly that, since bake_tail() adds it on the offline"
                  " side.")
            print("          b) it returns to a DIFFERENT pose ⇒ the host clip advanced under us,"
                  " which")
            print("             is legitimate drift (offline 'guard 1h' moves the forearm 0.1 deg"
                  " over")
            print("             its own keys, so a large reading means the host is not the parked"
                  " guard).")
    # ---- 4) THE READ ITSELF, and the one thing that would move every bar ----
    # r=, elbow= and len= are three views of ONE triangle: r^2 = l1^2 + l2^2 - 2*l1*l2*cos(elbow)
    # whenever |shoulder->elbow| and |elbow->hand| really are the bind lengths len= prints.  So
    # the ratio r_measured / r_from_the_law is 1.000 by geometry, and it answers two questions no
    # other line here can:
    #   * a SCATTERED ratio = the three _getDerivedPosition() calls are not describing one frame
    #     (or a bone did not resolve), i.e. the measurement is unusable and every span above is
    #     measuring the tool.
    #   * a CONSISTENT ratio away from 1 = the live skeleton is SCALED - Kenshi sizes characters
    #     by race - and then this ratio IS the scale factor, so the offline r bars (2.90..5.42)
    #     scale with it while the two ANGLE bars (elbow, grip) do not.  That is the only way the
    #     offline reference can be right and the game still read differently, and it costs one
    #     division to rule out.
    # ⚠️ Informational: it cannot fail the stroke, only the reading of it.
    ratios = []
    for d in s.sw_arm:
        L, r, el = _len_pair(d), fnum(d, "r"), fnum(d, "elbow")
        if L is None or r is None or el is None or el < 0.0 or min(L) <= 0.0:
            continue
        c = math.cos(math.radians(el))
        pred = math.sqrt(max(0.0, L[0] * L[0] + L[1] * L[1] - 2.0 * L[0] * L[1] * c))
        if pred > 1.0e-3:
            ratios.append(r / pred)
    if not ratios:
        print("  NOTE  no sample carries r=, elbow= and len= together, so the read is UNCHECKED.")
    else:
        ratios.sort()
        med, lo, hi = ratios[len(ratios) // 2], ratios[0], ratios[-1]
        print("  law of cosines closure r/sqrt(l1^2+l2^2-2*l1*l2*cos elbow) over %d sample(s):"
              % len(ratios))
        print("     median %.4f   range %.4f .. %.4f   (1.0000 = one frame, unscaled bind arm)"
              % (med, lo, hi))
        print("  " + verdict(hi - lo <= 0.02,
                             "the three positions and the two lengths describe one triangle"
                             " (spread %.4f <= 0.0200)" % (hi - lo)))
        if abs(med - 1.0) > 0.02:
            print("        ⚠️ consistently %.1f%% off 1.0 ⇒ a SCALED rider."
                  "  Multiply the offline r" % ((med - 1.0) * 100.0))
            print("        bars (2.90..5.42, span 1.5) by %.3f before reading WITNESS 1;"
                  " the elbow and" % med)
            print("        grip bars are angles and do NOT scale.")
        if hi - lo > 0.02:
            print("        A spread this wide is not rounding (len= is 2 decimals ⇒ ~0.01,"
                  " elbow= 1")
            print("        decimal ⇒ ~0.002).  Suspect the three reads straddling a skeleton"
                  " rebuild")
            print("        (§16) - which is why both bones are re-resolved BY NAME every frame.")
    # ---- 5) THE CAPTURE.  A freed bone nobody wrote renders at BIND. --------
    # Unchanged from -- T28 --, and it stays a HARD criterion because it is about the mask, not
    # the model: kRideSwingFreeBones takes 'R UpperArm'/'R Forearm' away from the host clip, and
    # RideSwingArmPose bails as a unit (++gRideSwingNoRef) if either bone fails to resolve.  Every
    # counted frame is therefore a frame that drew those two bones at BIND - §17.9's 「在牛背上
    # 站直」 localised to one arm.
    nrows = [d for d in s.sw_close if "noref" in d]
    if not nrows:
        if s.sw_close:
            print("  CHECK close rows carry no noref= - the line budget (kRideSwingLines) was"
                  " spent, or")
            print("        this is not a build that authors the arm at all.")
    else:
        tot = sum(int(fnum(d, "noref", 0.0)) for d in nrows)
        print("  noref= over %d close row(s): %d" % (len(nrows), tot))
        print("  " + verdict(tot == 0,
                             "every authored frame resolved both bones (no freed-but-unwritten"
                             " bone)"))
        if tot:
            print("        Suspect the skeleton instance being rebuilt under us (§16 - which is"
                  " why both")
            print("        bones are re-resolved BY NAME every frame), not the tables.")

    # ---- 6) THE HANDOVER, which this model keeps for free -------------------
    # Both baked tables start at { 0.0000f, 1.0, 0, 0, 0 } - X(0) = conj(K(0))*K(0) = identity by
    # construction, not by tuning - so the first authored frame writes capturedLocal verbatim, i.e.
    # the pose already on screen.  🆕 And T30 makes that claim cheaper to trust than T28 could:
    # the write is a LOCAL orientation, so no parent transform is read to build it and there is no
    # stale-cache term in the write path at all (see section 7).  Trip 25's aimed table had no such
    # property and paid: 0.8449 (32.3 deg) / 0.9711 / 0.9659 on its three first frames.
    settled, first, unread = swing_dot_split(s)
    if first:
        vals = [v for _, v in first]
        print("  first authored frame (kept<0): "
              + " / ".join("dot=%.4f at t=%s" % (v, d.get("t", "?")) for d, v in first))
        worstf = min(vals)
        print("  " + verdict(worstf >= 0.99,
                             "the window opens ON the host's own pose (worst first-frame"
                             " dot=%.4f) = no handover pop" % worstf))
        if worstf < 0.99:
            wt = min(first, key=lambda p: p[1])[0]
            wtv = fnum(wt, "t")
            print("        %.1f deg off."
                  % math.degrees(math.acos(max(-1.0, min(1.0, worstf)))), end="")
            if wtv is not None and wtv > 0.05:
                print("  ⚠️ BUT its own t=%.2f is past 0.055, i.e. inside the" % wtv)
                print("        cocking band, where one frame of read lag legitimately costs up to"
                      " 13.8 deg")
                print("        at 90 fps (section 7's table).  A kept<0 sample can only be the"
                      " window's")
                print("        first authored frame, whose t is ~0.00, so a t this large means the"
                      " kept<0")
                print("        marker and t disagree - read it there, not here, and do not touch"
                      " the tables.")
            else:
                print("  X(t) is still ~identity on that frame, so this is NOT")
                print("        the tables.  Two candidates: the tables do not actually start at"
                      " identity (⇒")
                print("        `python tools\\armarc.py --mirror`, which compares key 0 of both),"
                      " or the")
                print("        capture read a different frame than the log did (§21.5).")

    else:
        print("  NOTE  no sample landed on a window's first authored frame (kept<0), so the")
        print("        handover is UNJUDGED this trip - throttle luck, not a failure.")
    # ---- 7) dot= IS NOW DIAGNOSTIC ONLY.  Read it, do not chase it. ---------
    # 🆕 The reclassification is the whole point of T30's write path.  T28 built a DERIVED
    # orientation and divided out the parent's derived transform, which is cached one frame stale
    # ⇒ the stale read entered the POSE and the 7.3 deg residual trips 24/25/26 differenced was a
    # real deformation.  T30 writes capturedLocal * X(t) - a LOCAL orientation, no parent read at
    # all - so want=/dot= are computed for the log only (RidingPlugin.cpp:5990+) and a deficit here
    # can no longer bend the arm.  What it still measures honestly: how far the arm travelled
    # between our write and the log's read one frame later.
    # The bar has to move with the tempo, and vanilla's own curve is FAST.  From the shipped table
    # (`python tools\armarc.py --bake`), arm-vector rate: PEAK 1244 deg/s measured over one key
    # (t 0.055->0.083, 48.1 deg in 38.7 ms), MEDIAN 214; T29's hand-drawn cut was 461, so vanilla
    # is 2.7x it.  ⚠️ 1244 is a per-key AVERAGE and the instantaneous peak is higher (~1400): the
    # cut passes through r=2.90, and the same linear hand speed sweeps more angle on a short arm.
    # 🔑 So the bars are computed from the frame rate THIS log ran at, not from a remembered one -
    # a dry run against a synthetic perfect log showed a fixed 0.99 mean bar false-alarming at
    # 30 fps while passing at 90, which would have spent a round on the tool.  The frame rate is
    # in the rows already: consecutive samples in one window are kRideSwingArmLogGap authored
    # frames apart in f= and (dt * kRideSwingArcMs) ms apart in t.
    if settled:
        steps = []
        for w in wins:
            for i in range(1, len(w)):
                df = (fnum(w[i], "f", 0.0) or 0.0) - (fnum(w[i - 1], "f", 0.0) or 0.0)
                dt = ((fnum(w[i], "t", 0.0) or 0.0) - (fnum(w[i - 1], "t", 0.0) or 0.0)) * 1.400
                if df > 0.0 and dt > 0.02:      # dt=0 is the t=1.00 clamp, not a frame time
                    steps.append(df / dt)
        steps.sort()
        fps = steps[len(steps) // 2] if steps else 90.0
        # one frame of read lag, in degrees, at that rate - times the slack a synthetic PERFECT
        # log needed with room to spare (1.25 on the peak, 3.0 on the mean; both were checked at
        # 90 / 60 / 30 fps, and a seeded 3x lag still CHECKs at all three).
        wdeg, mdeg = min(179.0, 1400.0 / fps * 1.25), min(179.0, 214.0 / fps * 3.0)
        wbar, mbar = math.cos(math.radians(wdeg)), math.cos(math.radians(mdeg))
        sv = [v for _, v in settled]
        worst_s, mean_s = min(sv), sum(sv) / len(sv)
        print("  settled dot=: worst %.4f = %.1f deg, mean %.4f over %d sample(s)"
              % (worst_s, math.degrees(math.acos(max(-1.0, min(1.0, worst_s)))),
                 mean_s, len(sv)))
        print("     %s%.0f fps from f=/t= over %d step(s) ⇒ one frame of lag is worth 1400/fps ="
              % ("" if steps else "ASSUMED ", fps, len(steps)))
        print("     %.1f deg at the cut and 214/fps = %.1f deg in the slow majority; bars"
              " mean >= %.4f" % (1400.0 / fps, 214.0 / fps, mbar))
        print("     (%.1f deg), worst >= %.4f (%.1f deg)" % (mdeg, wbar, wdeg))
        print("  " + verdict(mean_s >= mbar, "the read tracks the write away from the fast band"
                                             " (mean %.4f)" % mean_s))
        print("  " + verdict(worst_s >= wbar,
                             "even the worst sample is within one frame of the arc's peak rate"))
        if worst_s < wbar or mean_s < mbar:
            print("        Before treating this as a defect: at %.0f fps the cocking band alone"
                  % fps)
            print("        ALLOWS %.1f deg (dot %.4f), so check WHERE the bad samples landed -"
                  % (1400.0 / fps, math.cos(math.radians(min(179.0, 1400.0 / fps)))))
            print("        a deficit INSIDE t 0.055..0.083 is the table's own speed."
                  "  -- T26 -- prints")
            print("        the per-sample separator that tells that apart from a real one.")

        print("  ⛔ dot= is a DIAGNOSTIC here, not a gate on the shape: it cannot deform the arm"
              " under")
        print("     this model (local write, no parent read), and the only way to flatter it"
              " would be to")
        print("     slow vanilla's own cut - i.e. to undo the change under test.  The shape's"
              " judges are")
        print("     WITNESS 1/2 above and the eyeball below.")
    # ---- 8) what only the eyeball can answer, and what each answer costs ----
    print("  NOTE  the SHAPE is still not in this file.  Four outcomes and their next rungs:")
    print("          1. it reads as a sabre cut ⇒ P4-3-4 is DONE; what is left is the release")
    print("             decision (does this build replace v1.6 in release\\).")
    print("          2. the shape IS vanilla's and it still reads wrong ⇒ 🆕 the dials are no"
          " longer")
    print("             hand-tuned numbers.  Three remain and each is a REGENERATION:")
    print("               * the CLIP    - BAKE_CLIP in armarc.py ('chop down' today; 'chop left',")
    print("                               'attack1'... - `python tools\\skelanims.py --sweep chop`")
    print("                               ranks them by shoulder AND elbow travel).")
    print("               * the TEMPO   - --map native|lead|stretch (native fits the clip's own")
    print("                               967 ms into the 1650 ms window, cut at 541 ms).")
    print("               * the FORM    - --abs replays the clip's pose instead of a delta, which")
    print("                               trades the handover property for vanilla's exact arm.")
    print("             ⚠️ All three change BOTH tables at once, so the .cpp and armarc.py must be")
    print("             regenerated together - `python tools\\armarc.py --mirror` is the check and")
    print("             every armarc mode runs it first, refusing to report on a mismatch.")
    print("          3. the arm is right and the BLADE is wrong ⇒ section 2's remedy: 'Bip01 R"
          " Hand'")
    print("             into BOTH tables (§17.14), never one.  Note this is now a REAL option:")
    print("             the wrist is currently keeping a captured roll while the elbow moves"
          " under")
    print("             it, which is exactly the case where the hand needs its own curve.")
    print("          4. elbow= / r= came out FLAT ⇒ that is not a taste question at all, it is")
    print("             WITNESS 1 failing: the forearm table is not reaching the bone.  Fix that")
    print("             before spending a round on the shape.")
    print("  ⚠️ WHAT THIS SECTION DOES NOT COVER, so nobody reads a pass here as a pass:")
    print("       -- T25/T26 --  kept= (our write survived applyToNode), the freed==authored"
          " table,")
    print("                      len=, armback (man=0x00 / minDot~1.0 = the arm was handed"
          " back).")
    print("       -- T27 --      pinst=live at every close, armt=1.00, ms= vs the 1650/3000"
          " pair,")
    print("                      the four retired counters (rst/drv/hold/fit = 0) and the")
    print("                      hostkeep/arm/swfree agreement.")
    print("       and the regressions that have nothing to do with the arm: zero AV, straddle")
    print("       takeovers == restored, dropped=0, the eyeball's 「其他和以前一样」, and THE"
          " RIGHT")
    print("       ARM AFTER DISMOUNTING (the standing exit through LegPoseRestoreImpl's"
          " unconditional")
    print("       RideSwingArmRelease - the one regression this whole family keeps re-earning).")


def report_trailer(s, lines):
    print("")
    print("== leftovers ==")
    if s.av:
        print("  ACCESS VIOLATION / rejection lines (%d):" % len(s.av))
        for line in s.av[:10]:
            print("    " + line[:150])
    else:
        print("  no access-violation line.")
    if s.notes:
        print("  other one-off lines worth a look (%d):" % len(s.notes))
        for line in s.notes[:12]:
            print("    " + line[:150])
    print("  log lines scanned=%d" % lines)


def main(argv):
    # Species names are UTF-8 in the log; a cp936 / cp1252 console must not
    # abort the whole report over one glyph it cannot print.
    try:
        sys.stdout.reconfigure(errors="replace")
    except (AttributeError, ValueError):
        pass
    path = argv[1] if len(argv) > 1 else DEFAULT_LOG
    cfgpath = argv[2] if len(argv) > 2 else DEFAULT_CFG
    if not os.path.isfile(path):
        print("no such log: %s" % path)
        return 2
    print("log: %s  (%d bytes)" % (path, os.path.getsize(path)))
    cfg = read_cfg(cfgpath)
    print("cfg: %s%s" % (cfgpath, "" if cfg else "   (not found - T3's cfg"
                                                 " half not checked)"))
    s = Session()
    lines = parse(path, s)
    report_rides(s)
    report_combat(s)
    report_forced(s)
    report_stance(s)
    report_twist(s)
    report_input(s)
    report_tuned(s, cfg)
    report_legs(s)
    report_pose(s)
    report_sheathe(s)
    report_stance_terms(s)
    report_suppress(s)
    report_redraw(s)
    report_swing(s)
    report_swing_look(s)
    report_swing_drive(s)
    report_swing_split(s)
    report_swing_gate(s)
    report_swing_arm(s)
    report_swing_aim(s)
    report_swing_host(s)
    report_swing_arc(s)
    report_swing_bake(s)
    report_attach(s)
    report_detach(s)
    report_p41d(s)
    report_trailer(s, lines)
    print("")
    print("Every CHECK above is a log-side fact only.  The eyeball half of")
    print("T1/T3/T4 (damage taken, hits landing, plain sit, straddle) is not")
    print("in this file and cannot be judged from it.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))








