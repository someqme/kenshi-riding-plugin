# -*- coding: utf-8 -*-
"""ridelog.py - offline verdict table for one RidingPlugin diagnostic session.

Streams RE_Kenshi_log.txt line by line (it has reached 6.7 MB in the past -
never load it whole, never paste it into a chat) and prints the acceptance
table for TEST_REQUIRED.md T1 / T3 / T4 plus the standing regression fields.

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
                                 "ms" if "holdms" in d else "f"))
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
    for st, f, cm, d, hold, unit in s.stance:
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
    report_attach(s)
    report_p41d(s)
    report_trailer(s, lines)
    print("")
    print("Every CHECK above is a log-side fact only.  The eyeball half of")
    print("T1/T3/T4 (damage taken, hits landing, plain sit, straddle) is not")
    print("in this file and cannot be judged from it.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))








