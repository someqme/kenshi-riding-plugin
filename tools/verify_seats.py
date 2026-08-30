# verify_seats.py - byte-level check of kDefaultSeats[] in the BUILT DLL.
#
#   python tools\verify_seats.py                      # newest snapshots\riding_tuned_*.cfg
#   python tools\verify_seats.py snapshots\riding_tuned_20260828_v11.cfg
#   python tools\verify_seats.py <cfg> <dll>
#
# Why this exists: source-file Chinese literals compile to GBK (CLAUDE.md, 关键机制 first
# bullet), so a bare literal silently never matches getName().  This script proves, from the
# shipped bytes, that every table key is present as UTF-8 and absent as GBK.
#
# Rules it enforces:
#   * every key appears in the DLL as UTF-8 exactly `allow` times, where
#         allow = 1 (its own table row)
#               + (number of OTHER keys that contain it as a substring)
#               + EXTRA[key]   (places the source deliberately compares that literal)
#   * no key appears as GBK, EXCEPT the names that a bare Chinese literal in the source
#     legitimately puts there (mostly the dead kFlingSkeletons[]) - that set is read out of
#     RidingPlugin.cpp, so a GBK hit on any OTHER name means a new bare literal slipped in.
#
# ⚠️ Point it at the NEW snapshot after regenerating the table, or the rows you just added are
# not checked at all.  The cfg actually used is printed on the first line for that reason.
# ⚠️ ASCII keys (race stringIDs) necessarily report gbk == utf8: same bytes, not a problem.

import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DLL = r"D:\KenshiModDev\Build\RidingPlugin\RidingPlugin.dll"
SRC = os.path.join(ROOT, "RidingPlugin.cpp")

# Literals the source compares on purpose, outside the table itself.  Each entry buys one
# extra allowed UTF-8 occurrence.  Keep the source location in the comment.
EXTRA = {
    "65260-Newwworld.mod": 1,       # neckFollow race match, RidingPlugin.cpp
}


def newest_snapshot():
    """Highest _v<N> among snapshots\riding_tuned_*.cfg.

    ⚠️ Numeric, not lexicographic: plain sorted() puts "_v11" BEFORE "_v9" and would
    silently check the wrong (older) table.
    """
    cands = glob.glob(os.path.join(ROOT, "snapshots", "riding_tuned_*.cfg"))
    if not cands:
        sys.exit("no snapshots\\riding_tuned_*.cfg found - pass the cfg explicitly")

    def key(p):
        m = re.search(r"_v(\d+)\.cfg$", p)
        return (int(m.group(1)) if m else -1, os.path.getmtime(p))

    return max(cands, key=key)


def keys_of(cfg):
    out = []
    for raw in open(cfg, "rb").read().split(b"\n"):
        line = raw.strip()
        if line and not line.startswith(b"#") and b"=" in line:
            name = line.split(b"=")[0].decode("utf-8")
            if name != "defaults":
                out.append(name)
    return out


def bare_chinese_literals(src_path):
    """Names a BARE literal in the source puts into the DLL as GBK (expected, mostly dead code)."""
    src = open(src_path, "rb").read()
    ok = set()
    for hit in re.findall(rb'"[^"\n]*[\xe4-\xe9][\x80-\xbf][\x80-\xbf][^"\n]*"', src):
        try:
            s = hit.decode("utf-8")
        except UnicodeDecodeError:
            continue
        if chr(92) + "x" not in s:          # skip properly escaped \xNN strings
            ok.add(s.strip('"'))
    return ok


def main(argv):
    cfg = argv[0] if argv else newest_snapshot()
    dll = argv[1] if len(argv) > 1 else DLL
    names = keys_of(cfg)
    blob = open(dll, "rb").read()
    gbk_ok = bare_chinese_literals(SRC)

    print("cfg   %s" % cfg)
    print("dll   %s  (%d B)\n" % (dll, len(blob)))

    def count(b):
        return 0 if not b else len(re.findall(re.escape(b), blob))

    bad = 0
    for n in sorted(names):
        u = count(n.encode("utf-8"))
        try:
            g = count(n.encode("gbk"))
        except UnicodeEncodeError:
            g = 0
        subs = [m for m in names if m != n and n in m]
        allow = 1 + len(subs) + EXTRA.get(n, 0)
        gbad = g > 0 and not n.isascii() and n not in gbk_ok
        ok = (u == allow) and not gbad
        if not ok:
            bad += 1
        print("%-5s %-22s utf8=%d (allow %d) gbk=%d%s%s" % (
            "" if ok else "BAD", n.encode("unicode_escape").decode(), u, allow, g,
            " [gbk from dead literal - ok]" if (g and n in gbk_ok) else "",
            ("  subs:" + ",".join(s.encode("unicode_escape").decode() for s in subs))
            if subs else ""))

    print("\nrows=%d  bad=%d" % (len(names), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
