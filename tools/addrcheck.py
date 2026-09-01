# -*- coding: utf-8 -*-
"""addrcheck.py - drift audit for the five docs: one address, one authority.

    python tools\\addrcheck.py            # address drift report
    python tools\\addrcheck.py --refs     # dangling "TASK.md <section>" citations
    python tools\\addrcheck.py --all      # both

Why this exists.  Every exe address used to be restated in whatever doc happened to
need it, so correcting one left stale copies behind.  That is not hypothetical:
0x5B15C0 grew THREE different claims across FOUR docs before anyone noticed.  The
rule this script enforces is:

    RidingPlugin_RE_NOTES.md is the sole authority for addresses.  Every hex an
    address appears at in a private doc must carry a section pointer (§nn) back to
    it, so a reader always knows where the authoritative claim lives.

⚠️ This tool judges POINTERS, not correctness.  It cannot tell you an address is
wrong - only that two docs are free to disagree about it without anyone noticing.

⚠️ The spread histogram is not the scoreboard.  Giving an orphan address a home in
RE_NOTES RAISES its doc count - that is the fix.  The two numbers that must stay at
zero are TODO (restated with no pointer) and no-authority; both are gated by the
exit code.  They were 19 and 8 on 2026-09-01 and were driven to 0 the same day.

⚠️ RE_NOTES itself is exempt from the pointer rule: it IS the authority, and its
own cross-references are what the pointers point at.

--refs is the safety net for trimming TASK.md.  TASK.md is .gitignore'd yet cited
by section name from RidingPlugin.cpp (13 places), RE_NOTES, HISTORY and
TEST_REQUIRED.  Deleting a section heading silently breaks all of them, so every
"TASK.md P4-1N"-style citation must resolve to a live anchor in TASK.md.
"""

import os
import re
import sys

# Report lines can carry ⚠️; a cp936 console cannot encode it and would abort the run.
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The five docs that carry addresses.  RE_NOTES is listed last on purpose: it is the
# authority, and the report leans on that ordering when it names where a fact lives.
DOCS = [
    "doc.md",
    "TASK.md",
    "RidingPlugin_HISTORY.md",
    "TEST_REQUIRED.md",
    "RidingPlugin_RE_NOTES.md",
]
AUTHORITY = "RidingPlugin_RE_NOTES.md"

# HISTORY is a record: its addresses are snapshots of what we believed at the time, and
# rewriting them would falsify the log.  It gets one blanket disclaimer at the top of the
# file instead of per-line pointers, so per-line findings there are informational.
RECORD_DOCS = {"RidingPlugin_HISTORY.md"}

# Files that cite TASK.md by section name.  Sources, not docs, are the ones that hurt:
# a comment in the shipped .cpp pointing at a section nobody can find is a dead end.
CITERS = [
    "RidingPlugin.cpp",
    "doc.md",
    "CLAUDE.md",
    "RidingPlugin_HISTORY.md",
    "RidingPlugin_RE_NOTES.md",
    "TEST_REQUIRED.md",
    os.path.join("tools", "ridelog.py"),
]

# 4+ hex digits: shorter than that and we would match every 0x30 bit flag in the docs.
ADDR = re.compile(r"0x[0-9A-Fa-f]{4,}")
# Offsets inside RidingPlugin.dll, NOT exe RVAs - RE_NOTES rightly never mentions them, so
# auditing them would report a permanent "no authority" that no edit can ever fix.  Both come
# from the 2026-08-23 crash-dump analysis (HISTORY「游戏中读档崩溃」): 0x133D3 is the faulting
# instruction in our own DLL, 0x19188 is our own getMovement import thunk.
NOT_EXE = {"0x133D3", "0x19188"}
# A section pointer on the same line.  "§12", "§17.2", "§18.9", or a bare "RE_NOTES".
POINTER = re.compile(r"§\s*\d|RE_NOTES")
# "TASK.md P4-1N", "TASK.md's P3", "TASK.md X-1".  The 's is real - RidingPlugin.cpp:500.
CITATION = re.compile(r"TASK\.md(?:'s)?\s+((?:P\d+[A-Za-z0-9]*(?:-[A-Za-z0-9]+)*)|(?:X-\d+)|X\b)")
# Anchors inside TASK.md: "## P4-1c ...", "### X-5 ...", "**X-1 ...".  X-1..X-4 are bold
# bullets rather than headings, so matching only "#" would report them as dangling.
ANCHOR_LINE = re.compile(r"^(?:#{2,4}\s|-?\s*\*\*)")
ANCHOR_TOKEN = re.compile(r"\b(P\d+[A-Za-z0-9]*(?:-[A-Za-z0-9]+)*|X-\d+|X)\b")


def read_lines(rel):
    path = os.path.join(ROOT, rel)
    if not os.path.exists(path):
        return None
    with open(path, "r", encoding="utf-8-sig") as f:
        return f.read().splitlines()


def norm(addr):
    """0x5b15c0 and 0x5B15C0 are the same address; without this the drift hides."""
    return "0x" + addr[2:].upper().lstrip("0").rjust(4, "0")


def collect():
    """-> (per_addr {addr: {doc: [line numbers]}}, missing docs)."""
    per_addr = {}
    missing = []
    for rel in DOCS:
        lines = read_lines(rel)
        if lines is None:
            missing.append(rel)
            continue
        for n, line in enumerate(lines, 1):
            for raw in ADDR.findall(line):
                a = norm(raw)
                if a in NOT_EXE:
                    continue
                per_addr.setdefault(a, {}).setdefault(rel, []).append(n)
    return per_addr, missing


def report_addresses(per_addr):
    docs_of = dict((a, set(d.keys())) for a, d in per_addr.items())

    print("== spread - how many docs each address appears in ==")
    hist = {}
    for a, ds in docs_of.items():
        hist[len(ds)] = hist.get(len(ds), 0) + 1
    for n in sorted(hist, reverse=True):
        print("   in %d docs: %3d addresses" % (n, hist[n]))
    print("   total     : %3d addresses" % len(docs_of))
    print("   2026-09-01 baselines (before / after the TASK.md trim + pointer pass):")
    print("     before: 241 total / 3 in four docs / 27 in three  =  30 drift surfaces")
    print("     after : 239 total / 3 in four docs / 25 in three  =  28 drift surfaces")
    print("   ⚠️ spread is NOT the scoreboard.  Adopting an orphan address INTO the authority")
    print("      raises its doc count by one on purpose - that is the fix, not a regression.")
    print("      The two numbers that must stay at zero are TODO and no-authority, below.")

    drift = sorted([a for a, ds in docs_of.items() if len(ds) >= 3],
                   key=lambda a: (-len(docs_of[a]), a))
    print()
    print("== drift surfaces - the same address stated in 3+ docs (%d) ==" % len(drift))
    for a in drift:
        short = [d.replace("RidingPlugin_", "").replace(".md", "") for d in sorted(docs_of[a])]
        print("   %-10s %s" % (a, " ".join(short)))

    # The actionable list: a restated address whose line does not say where the
    # authority is.  Those are the lines that can silently disagree.
    print()
    print("== TODO - restated address, no section pointer on that line ==")
    todo = 0
    noted = 0
    for rel in DOCS:
        if rel == AUTHORITY:
            continue
        lines = read_lines(rel)
        if lines is None:
            continue
        hits = []
        for n, line in enumerate(lines, 1):
            found = [norm(x) for x in ADDR.findall(line)]
            shared = sorted(set(a for a in found if len(docs_of.get(a, ())) >= 2))
            if shared and not POINTER.search(line):
                hits.append((n, shared))
        if not hits:
            continue
        tag = "  (record - blanket disclaimer, not per-line)" if rel in RECORD_DOCS else ""
        print("   %s%s" % (rel, tag))
        for n, shared in hits:
            print("      :%-4d %s" % (n, " ".join(shared)))
            if rel in RECORD_DOCS:
                noted += 1
            else:
                todo += 1
    if not todo and not noted:
        print("   (none)")
    print("   -> %d lines to fix, %d informational (record docs)" % (todo, noted))

    # A private doc naming an address RE_NOTES never mentions has no authority at all:
    # nothing to point at, and nowhere a correction would propagate from.
    print()
    print("== no authority - in a private doc, absent from %s ==" % AUTHORITY)
    orphans = sorted(a for a, ds in docs_of.items() if AUTHORITY not in ds)
    for a in orphans:
        short = [d.replace("RidingPlugin_", "").replace(".md", "") for d in sorted(docs_of[a])]
        print("   %-10s %s" % (a, " ".join(short)))
    if not orphans:
        print("   (none)")
    return len(orphans) + todo


def report_refs():
    """Every "TASK.md <section>" citation must resolve to an anchor in TASK.md."""
    task = read_lines("TASK.md")
    if task is None:
        print("== refs ==")
        print("   TASK.md not found - cannot check section citations")
        return 1

    anchors = set()
    for line in task:
        if ANCHOR_LINE.match(line):
            anchors.update(ANCHOR_TOKEN.findall(line))

    print("== refs - TASK.md section citations must resolve (%d anchors) ==" % len(anchors))
    dangling = []
    total = 0
    for rel in CITERS:
        lines = read_lines(rel)
        if lines is None:
            continue
        for n, line in enumerate(lines, 1):
            for tok in CITATION.findall(line):
                total += 1
                if tok not in anchors:
                    dangling.append((rel, n, tok))
    print("   %d citations checked across %d files" % (total, len(CITERS)))
    if dangling:
        for rel, n, tok in dangling:
            print("   DANGLING  %s:%d  -> TASK.md %s" % (rel, n, tok))
    else:
        print("   no dangling citations")
    return len(dangling)


def main(argv):
    want_refs = "--refs" in argv or "--all" in argv
    want_addr = "--refs" not in argv or "--all" in argv

    bad = 0
    if want_addr:
        per_addr, missing = collect()
        for rel in missing:
            print("   ⚠️ missing, not audited: %s" % rel)
        bad += report_addresses(per_addr)
        if want_refs:
            print()
    if want_refs:
        bad += report_refs()

    # Exit non-zero on the three failures: a restated address with no section pointer, an
    # address with no authority at all, or a citation pointing at a section that no longer
    # exists.  The first one was 19 lines when this script was written and was driven to 0 on
    # 2026-09-01 - so from here on any hit is a fresh regression, not a backlog, and gating on
    # it is what keeps it at zero.  Record docs (HISTORY) stay informational by design.
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
