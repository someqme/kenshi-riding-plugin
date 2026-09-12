# -*- coding: utf-8 -*-
"""List ANIMATION records usable as mounted-attack hosts.

Filter: reachable, has male_skeleton track (via `anim name`), NOT a stumble reaction.
"""
import os, sys, re, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gamedata

SKEL_TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "skelanims.py")

def skel_clips():
    out = subprocess.check_output(
        [sys.executable, SKEL_TOOL, "--skel", "male_skeleton.skeleton"],
        stderr=subprocess.STDOUT)
    text = out.decode("utf-8", "replace")
    names = {}
    for line in text.splitlines():
        m = re.match(r"\s+(\S.*?)\s+([0-9.]+)s\s*$", line)
        if m:
            names[m.group(1).strip()] = float(m.group(2))
    return names

def main():
    skel = skel_clips()
    print("skeleton clips: %d" % len(skel))
    rows, dis = gamedata._anim_rows(None)
    stumb, cands, miss = [], [], []
    for r, lay, whole in rows:
        refs = r.refs or {}
        has_st = "stumbles" in refs
        clip = r.f.get("anim name") or r.name
        dur = skel.get(clip)
        fl = gamedata.flags(r)
        cmode = r.f.get("is combat mode")
        dismv = r.f.get("disables movement")
        chance = r.f.get("chance")
        rec = dict(name=r.name, clip=clip, lay=lay, whole=whole, flags=fl,
                   dur=dur, cmode=cmode, dismv=dismv, chance=chance,
                   stumbles=refs.get("stumbles"), events=refs.get("events"),
                   file=r.file)
        if has_st:
            stumb.append(rec)
        elif dur is None:
            miss.append(rec)
        else:
            cands.append(rec)

    print("\n=== STUMBLE hit-reactions (%d) — forbidden as attack host ===" % len(stumb))
    for x in stumb:
        print("  %-28s clip=%-22s dur=%s  cmode=%s dismv=%s" % (
            x["name"], x["clip"], x["dur"], x["cmode"], x["dismv"]))

    print("\n=== no skeleton track for anim name (%d) ===" % len(miss))
    # only print action/combat-ish misses
    for x in miss:
        if "is action" in x["flags"] or (x["cmode"] not in (None, 0)):
            print("  %-28s clip=%-22s %s cmode=%s" % (x["name"], x["clip"], x["flags"], x["cmode"]))

    print("\n=== CANDIDATES no-stumbles + has track (%d) ===" % len(cands))
    def key(x):
        return (0 if (x["cmode"] not in (None, 0)) else 1,
                0 if "is action" in x["flags"] else 1,
                0 if "loop" not in x["flags"] else 1,
                x["name"].lower())
    cands.sort(key=key)
    print("%-28s %-8s %-6s %-5s %-6s %-6s %-6s %s" % (
        "record", "clip", "dur", "lay", "whole", "cmode", "dismv", "flags"))
    for x in cands:
        print("%-28s %-8s %-6s %-5s %-6s %-6s %-6s %s" % (
            x["name"][:28], x["clip"][:28],
            ("%.3f" % x["dur"]) if x["dur"] is not None else "-",
            x["lay"], "Y" if x["whole"] else "-",
            x["cmode"], x["dismv"], x["flags"]))

if __name__ == "__main__":
    main()
