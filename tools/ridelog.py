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


def fnum(d, k, default=None):
    """Parse d[k] as float; tolerate the int-scaled fields and junk."""
    v = d.get(k)
    if v is None:
        return default
    try:
        return float(v)
    except ValueError:
        return default


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
        self.restored = 0        # LEGPOSE restored on dismount
        self.takeovers = []      # LEGPOSE takeover lines (dicts)
        self.released = []       # LEGPOSE released lines (grace=, f=)
        self.kept = Stat()
        self.kept_bad = 0        # samples not 1.0000
        self.stance = []         # (state, f, cm, d, hold) in order
        self.twist = []          # (want, sh, d, on, msk, host)
        self.tuned = []          # (species, key or None)
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
            if "LEGPOSE restored on dismount" in line:
                s.restored += 1
                continue
            if "LEGPOSE takeover" in line:
                s.takeovers.append(kv(line))
                continue
            if "LEGPOSE released" in line:
                s.released.append(kv(line))
                continue
            if "kept=" in line:
                k = fnum(kv(line), "kept")
                s.kept.add(k)
                if k is not None and abs(k - 1.0) > 0.0005:
                    s.kept_bad += 1
                continue

            if "Riding: STANCE" in line:
                d = kv(line)
                st = line.split("STANCE", 1)[1].strip().split()[0]
                s.stance.append((st, d.get("f", "?"), d.get("cm", "?"),
                                 fnum(d, "d"), d.get("hold", "?")))
                continue
            if "Riding: TWIST" in line:
                d = kv(line)
                s.twist.append((fnum(d, "want"), fnum(d, "sh"), fnum(d, "d"),
                                d.get("on"), d.get("msk"), d.get("host")))
                continue
            m = TUNED.match(line.strip()) or TUNED.search(line)
            if m:
                d = kv(line)
                s.tuned.append((m.group("sp"), d.get("key")))
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
    # there); -1 means unreadable (three guards); 0 means the species cannot
    # swing.  Absent is NOT the same as readable - never PASS on it.
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
        print("  NOTE  %d mount(s) report mAtk=0 = that species physically cannot"
              % len(zero))
        print("        swing; both-sides degrades to rider-only FOR THOSE."
              "  This is the")
        print("        only evidence that justifies a per-species fallback.")

    elig = [(d.get("species", "?"), d.get("elig"), d.get("size")) for d in s.rides]
    print("  elig by mount: " + ", ".join("%s=%s(size %s)" % e for e in elig))
    print("  (predicted: small tier elig=1, big tier elig=0; the four big crabs"
          " must be")
    print("   held out by rad=, not torso= - a rad of 0 there means the read"
          " failed.)")


def report_stance(s):
    print("")
    print("== T1 - combat stance enters AND leaves ==")
    if not s.stance:
        print("  no STANCE line - either no fight happened or diagnostics were off.")
        return
    trans = []
    prev = None
    for st, f, cm, d, hold in s.stance:
        if st != prev:
            trans.append((prev, st, f, cm, d, hold))
            prev = st
    for a, b, f, cm, dd, hold in trans:
        print("  %s -> %s  at f=%s  cm=%s d=%s hold=%s" % (
            a if a is not None else "start", b, f, cm,
            "-" if dd is None else "%.1f" % dd, hold))
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
        print("    large d  -> kRideStanceHoldFrames is not being decremented"
              " (only")
        print("                HaltAndForceSitPass passes advance=true)")


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
    for want, sh, d, on, msk, host in s.twist:
        # d = -1.0 is the "no target at all" sentinel, not a distance.
        if d is not None and d < 0.0:
            notgt += 1
        else:
            dstat.add(d)
            if d is not None and d > 60.0:
                far += 1
        if want is not None and sh is not None and abs(want) > 5.0:
            scored += 1
            if (want > 0) == (sh > 0):
                same_sign += 1
    print("  samples=%d   d= %s   d>60: %d   no-target(d=-1): %d"
          % (len(s.twist), dstat, far, notgt))
    print("  |want|>5 samples=%d, same-sign as sh=%d" % (scored, same_sign))
    if not scored:
        print("  CHECK no |want|>5 sample - the twist sign was never exercised")
        print("        (the fight stayed in front of the rider).  This is not a"
              " pass.")
    else:
        print("  " + verdict(same_sign == 0,
                             "want and sh stay opposite in sign"
                             " (same sign => flip kRideTwistSign, one constant)"))



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
    print("  " + verdict(len(s.takeovers) == s.restored,
                         "every takeover was handed back on dismount"
                         " (a mismatch = a leg left at 45 deg)"))
    print("  " + verdict(not s.released,
                         "no mid-ride release"
                         + ("" if not s.released else
                            " - grace values: " + ", ".join(
                                r.get("grace", "?") for r in s.released[:8]))))
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

    for t in s.takeovers:
        print("    abd=%s flx=%s rad=%s torso=%s host=%s hw=%s msk=%s calf=%s"
              " stance=%s twist=%s" % (
                  t.get("abd"), t.get("flx"), t.get("rad"), t.get("torso"),
                  t.get("host"), t.get("hw"), t.get("msk"), t.get("calf"),
                  t.get("stance"), t.get("twist")))


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


def report_tuned(s, cfg):
    print("")
    print("== T3 - X-2: tuning must write to a race row, never a name row ==")
    if not s.tuned:
        print("  no 'tuned' line - nobody pressed a tune key this session,")
        print("  so T3's real acceptance point was not exercised.")
    else:
        nokey = [t for t in s.tuned if not t[1]]
        seen = {}
        for sp, key in s.tuned:
            seen.setdefault(sp, key)
        for sp, key in sorted(seen.items()):
            print("  %-18s -> %s" % (sp[:18], key or "NO key= (NAME ROW!)"))
        print("  tune presses=%d  without key=: %d" % (len(s.tuned), len(nokey)))
        print("  " + verdict(not nokey,
                             "every tune landed on a race row"
                             " (a missing key= means getRaceKey() came back"
                             " empty - check race= on the mount line)"))
    if cfg:
        print("  riding.cfg now: %d race row(s), %d name row(s)%s"
              % (cfg[0], cfg[1], "" if not cfg[2] else "  -> " + ", ".join(cfg[2])))
        print("  " + verdict(cfg[1] == 0,
                             "cfg holds race rows only"))


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
    report_stance(s)
    report_twist(s)
    report_tuned(s, cfg)
    report_legs(s)
    report_pose(s)
    report_trailer(s, lines)
    print("")
    print("Every CHECK above is a log-side fact only.  The eyeball half of")
    print("T1/T3/T4 (damage taken, hits landing, plain sit, straddle) is not")
    print("in this file and cannot be judged from it.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))








