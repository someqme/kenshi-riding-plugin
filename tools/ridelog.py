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
        # The swing window that used to be read here (P43SW, kP43Sw*) was taken
        # out of the DLL on the user's ruling after trip 10 answered it; its
        # findings live in HISTORY §U, and no build carries it any more.
        self.rd = []             # dicts of "P43RD edge ..." lines
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
            if "kept=" in line:
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








