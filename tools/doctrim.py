# -*- coding: utf-8 -*-
"""doctrim.py - move a retired TASK.md entry into a HISTORY appendix, verbatim.

    python tools\\doctrim.py --triage    # per-line live/retrospective judgement
    python tools\\doctrim.py --dry       # boundaries + what the pointer would say
    python tools\\doctrim.py --verify    # would the move lose a fact or a destination?
    python tools\\doctrim.py --apply     # back up, append to HISTORY, splice TASK.md

TASK.md:3 is its own charter: "阶段做完后 ... 本文件对应条目删掉.  不在这里留推理过程,
也不留已完成的细节."  This script executes that, and --triage is what keeps it honest.

⚠️ FINDING 2026-09-01, and the reason PLAN is only two lines long.  The 20 sections marked
✅ (TASK.md:22-165, 37 KB, 34% of the file) look like completed-phase residue, and trimming
them wholesale was the obvious move.  --triage says otherwise: of 65 non-blank body lines,
**40 are LIVE** - ⚠️ 别再重试 / ⏳ 仍开着 / hard results that constrain the still-open P4-3.
Those bodies were ALREADY reduced to an index once; what is left is mostly load-bearing,
sitting under a closed heading.  Worse, LIVE has false negatives: TASK.md:56-60 is a
conditional recipe for route P2c ("配方留档 ... 将来真的换姿势 clip 时才用得上") and carries
none of the marker words.  ⇒ **Do not batch-trim the ✅ sections.**  Only entries that are
purely retrospective belong here, and each one gets its caveats checked by hand first.

⚠️ Headings are never deleted, only entry bodies.  TASK.md is .gitignore'd yet cited BY
SECTION NAME from RidingPlugin.cpp (13 places), RE_NOTES, HISTORY and TEST_REQUIRED; the
heading is the anchor those citations resolve to.  Run `addrcheck.py --refs` afterwards.

⚠️ Line numbers are NEVER hard-coded.  An earlier trim used a throwaway script with literal
numbers - run it twice and it eats the wrong bytes.  Each entry names its ANCHOR text; a
missing or duplicated anchor aborts the whole run rather than guessing.
"""

import os
import re
import shutil
import sys

# The docs are Chinese and carry ✅/⚠️; a cp936 console cannot encode those and would abort
# the run mid-report (ridelog.py:2018 hit the same wall).
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TARGET = "TASK.md"
ARCHIVE = "RidingPlugin_HISTORY.md"
BACKUP_SUFFIX = ".bak_20260901_trim"

# New appendix in HISTORY.  Inserted BEFORE this heading so the pretrim snapshot stays last.
# ⚠️ §T has since grown a THIRD entry, added by hand on 2026-09-01 (the「地址单一真相源」item
# itself, once it was finished).  The title in HISTORY says 三条; the constant below is what the
# --apply run created and is kept as-is because --apply refuses to run again once §T exists.
ARCHIVE_BEFORE = "## 附录 PRETRIM-20260831"
APPENDIX_ID = "§T"
APPENDIX_TITLE = ("### §T. TASK.md P5 两条纯回顾条目的原文：RE_NOTES 各节落地清单 / "
                  "release 包玩家文档三轮改动（2026-08-30 → 08-31）")
APPENDIX_NOTE = ("> 2026-09-01 从 TASK.md P5 迁出（地址单一真相源那一轮）。TASK.md 原处只留压缩指针；"
                 "下面是原文，逐字未改。⚠️ 本节的地址与 ⚠️ 条都是**当时的复述**，权威一律看 "
                 "`RidingPlugin_RE_NOTES.md` 自己。")

# Docs that count as "this caveat still has a home": HISTORY is the verbatim record,
# RE_NOTES owns addresses and RE technique, doc.md owns the current implementation, and
# ridelog.py owns the acceptance criteria it enforces.
ARCHIVES = [ARCHIVE, "RidingPlugin_RE_NOTES.md", "doc.md", os.path.join("tools", "ridelog.py")]

# The plan.  extent="line": the anchored line only (a bullet in P5, no nested children -
# checked, TASK.md:339 and :352 start fresh).  extent="section": anchored heading + body.
#
# Both entries below were caveat-checked by hand before being listed.  Every ⚠️/⛔ in them
# has a home outside TASK.md: ⛔ nextMove(0x1F4) -> RE_NOTES §17.3; ⛔「解释错了不等于禁令
# 错了」-> doc.md「禁止注册的危险 hook 地址」; ⚠️「没结第二个写手是谁」-> RE_NOTES §18.6/§18.7;
# ⚠️ T1 的判据 -> tools\ridelog.py:784 ("this is THE T1 criterion - never the seconds it
# took"); 「STANCE 1 → 0」-> doc.md + TEST_REQUIRED.md.  The live halves are re-stated in
# the pointers rather than moved.
PLAN = [
    {
        "anchor": "- `RE_NOTES.md`：",
        "extent": "line",
        "why": "整条是 RE_NOTES §15-§19 的摘要 ＝ 地址漂移的最大单一来源（4124 B）",
        "pointer": ("- `RE_NOTES.md`：**§15–§19 全部已落地**（逐节摘要已迁出，见 `HISTORY.md` "
                    "§T）。⚠️ **本条剩下的唯一义务：新的逆向结论进 `RE_NOTES.md`，别在本文件复述地址**"
                    " —— 地址的真相源是 RE_NOTES 自己，复述一次就多一个漂移面。"),
    },
    {
        "anchor": "- release 包：",
        "extent": "line",
        "why": "三轮玩家文档改动的逐轮字节数/md5/逐句核对 ＝ 纯经过（2838 B）",
        "pointer": ("- release 包 `README.txt` / `功能介绍.txt`：**✅ 三轮改动全部做完（2026-08-30 → "
                    "08-31）**，逐轮字节数 / md5 / 逐句对实测的原文见 `HISTORY.md` §T。⚠️ **验收判据没有"
                    "跟着改**：T1 只看「`STANCE` 出现过 `1 → 0`」，**永不许改成按秒数判失败**（写死秒数"
                    "只是因为它现在可预期；`tools\\ridelog.py` 的 T1 节自己也写着这句）。⚠️ **待测欠账"
                    "一律看 `TEST_REQUIRED.md` 本体，别在本文件复述条数** —— 原文那句「只剩 T2 一条」"
                    "已经过期。⏳ 条件项：走 P2c 则包结构变化，Nexus 说明要写清依赖。"),
    },
]

HEADING = re.compile(r"^#{2,4}\s")
# A line that still constrains future work, as opposed to one that only records what
# happened.  ⚠️/⏳/⛔ and 别再/不许/仍/见下 are how this project writes a live rule.
# ⚠️ Known false negatives - see the FINDING note above.  Treat as a prompt to read, not a
# verdict: a line without a marker word can still be a live conditional plan.
LIVE = re.compile(r"⚠️|⏳|⛔|别再|不许|不要|仍在|仍是|仍然|还活着|还开着|见下|等 P4-3|暂不删|要在|必须")
INDEX = re.compile(r"^\*\*(结论已|迁出去向|下游|本文件里还活着)")

FACTS = [
    ("addr", re.compile(r"0x[0-9A-Fa-f]{4,}")),
    ("md5", re.compile(r"\b[0-9A-Fa-f]{32}\b")),
    ("size", re.compile(r"\b(\d{6}) B\b")),
]

# What a body cites, so the replacement pointer can be checked for dropping a destination.
# ⚠️ The FACTS check alone can pass VACUOUSLY (a body whose md5s live in the heading has no
# hex of its own); destinations are the thing a reader actually loses.
DEST = re.compile(r"(HISTORY|RE_NOTES|CLAUDE|doc|TEST_REQUIRED)\.md\s*(§\s*[0-9.]+|§\s*[A-Z]\b|「[^」]{1,20}」)?")


def read(rel):
    with open(os.path.join(ROOT, rel), "r", encoding="utf-8-sig", newline="") as f:
        return f.read()


def destinations(text):
    """CLAUDE.md folds into doc.md: the old CLAUDE.md body was split out into doc.md and 53
    references still say "CLAUDE.md" meaning doc.md (left alone on purpose - HISTORY is a
    record).  Treating them as different would flag every rewritten pointer as a loss."""
    out = set()
    for f, sec in DEST.findall(text):
        if f == "CLAUDE":
            f = "doc"
        out.add("%s %s" % (f, re.sub(r"\s+", "", sec)) if sec else f)
    return out


def dropped(body_dests, ptr_dests):
    """A BARE file token ("doc") is satisfied by any citation of that file; a SECTIONED one
    ("RE_NOTES §16") must be cited as such - that is what tells the reader where to look."""
    files = set(d.split(" ")[0] for d in ptr_dests)
    return sorted(d for d in body_dests
                  if (d not in ptr_dests) and not (" " not in d and d in files))


def facts_in(text):
    out = set()
    for kind, rx in FACTS:
        for m in rx.findall(text):
            out.add((kind, m.upper() if kind != "size" else m))
    return out


def locate(lines):
    """-> [(entry, body_start, body_end)] 0-based, end exclusive.  Aborts on a missing or
    duplicated anchor: a silently skipped entry leaves the doc half-trimmed, a duplicated
    one means the anchor is not specific enough to be safe."""
    out = []
    for e in PLAN:
        hits = [i for i, ln in enumerate(lines) if ln.startswith(e["anchor"])]
        if len(hits) != 1:
            sys.exit("anchor %r matched %d lines in %s - fix PLAN before running"
                     % (e["anchor"], len(hits), TARGET))
        h = hits[0]
        if e["extent"] == "line":
            out.append((e, h, h + 1))
        else:
            end = h + 1
            while end < len(lines) and not HEADING.match(lines[end]):
                end += 1
            out.append((e, h + 1, end))
    return out


def main(argv):
    triage = "--triage" in argv
    dry = "--dry" in argv
    verify = "--verify" in argv
    apply_ = "--apply" in argv
    if not (triage or dry or verify or apply_):
        sys.exit(__doc__)

    raw = read(TARGET)
    lines = raw.splitlines(True)          # keepends: the splice must be byte-exact
    blocks = locate(lines)

    if triage:
        print("== triage - LIVE lines must survive ==")
        n_live = 0
        for e, b0, b1 in blocks:
            print("   %s" % e["anchor"])
            for i in range(b0, b1):
                ln = lines[i].rstrip()
                if not ln or ln == "---":
                    continue
                flag = ("LIVE " if LIVE.search(ln)
                        else "index" if INDEX.match(ln) else "     ")
                n_live += flag == "LIVE "
                print("      :%-4d %-5s %5dB  %s" % (i + 1, flag, len(ln.encode("utf-8")), ln[:76]))
        print("   -> %d LIVE lines; ⚠️ LIVE has false negatives, read before deleting" % n_live)

    if dry:
        print("== dry - %d entries in %s (%d lines) ==" % (len(blocks), TARGET, len(lines)))
        total = 0
        for e, b0, b1 in blocks:
            body = "".join(lines[b0:b1])
            total += len(body.encode("utf-8"))
            print("   %s" % e["anchor"])
            print("      why      %s" % e["why"])
            print("      body     :%d-%d (%d lines, %d B)"
                  % (b0 + 1, b1, b1 - b0, len(body.encode("utf-8"))))
            print("      first    %s" % lines[b0].rstrip()[:92])
            print("      last     %s" % lines[b1 - 1].rstrip()[:92])
            nxt = lines[b1].rstrip()[:92] if b1 < len(lines) else "(EOF)"
            print("      next     :%-4d %s" % (b1 + 1, nxt))
            print("      pointer  %s" % e["pointer"][:92])
        print("   -> %d B moves to %s %s" % (total, ARCHIVE, APPENDIX_ID))

    if verify:
        pool = "".join(read(a) for a in ARCHIVES)
        pool_facts = facts_in(pool)
        print("== verify - nothing lost by moving (facts + destinations) ==")
        bad = 0
        for e, b0, b1 in blocks:
            body = "".join(lines[b0:b1])
            # Facts are safe by construction here: the body is appended VERBATIM to HISTORY.
            # What can still be lost is a destination the pointer forgets to carry.
            lost = dropped(destinations(body), destinations(e["pointer"]))
            print("   %-22s %d facts / %d dests -> %s"
                  % (e["anchor"], len(facts_in(body)), len(destinations(body)),
                     "OK" if not lost else "DROPS %d" % len(lost)))
            for d in lost:
                print("      DROPPED dest  %s" % d)
                bad += 1
        print("   -> %s" % ("no destination dropped" if not bad
                            else "%d dropped - refusing --apply" % bad))
        if bad and apply_:
            sys.exit("verify failed; --apply aborted")

    if apply_:
        for rel in (TARGET, ARCHIVE):
            bak = os.path.join(ROOT, rel + BACKUP_SUFFIX)
            if os.path.exists(bak):
                print("backup kept (already there): %s" % os.path.basename(bak))
            else:
                shutil.copy2(os.path.join(ROOT, rel), bak)
                print("backup -> %s" % os.path.basename(bak))

        nl = "\r\n" if raw.count("\r\n") > raw.count("\n") / 2 else "\n"

        arch_raw = read(ARCHIVE)
        if APPENDIX_ID + "." in arch_raw:
            sys.exit("%s already has %s - nothing appended, TASK.md untouched"
                     % (ARCHIVE, APPENDIX_ID))
        chunks = [APPENDIX_TITLE + nl, nl, APPENDIX_NOTE + nl, nl]
        for e, b0, b1 in blocks:
            chunks.append("".join(lines[b0:b1]))
            if not chunks[-1].endswith(nl):
                chunks[-1] += nl
            chunks.append(nl)
        appendix = "".join(chunks)

        arch_lines = arch_raw.splitlines(True)
        at = next((i for i, ln in enumerate(arch_lines) if ln.startswith(ARCHIVE_BEFORE)), None)
        if at is None:
            sys.exit("could not find %r in %s - refusing to guess where %s goes"
                     % (ARCHIVE_BEFORE, ARCHIVE, APPENDIX_ID))
        arch_lines.insert(at, appendix)
        with open(os.path.join(ROOT, ARCHIVE), "w", encoding="utf-8", newline="") as f:
            f.write("".join(arch_lines))
        print("appended %s to %s before %r (%d B)"
              % (APPENDIX_ID, ARCHIVE, ARCHIVE_BEFORE, len(appendix.encode("utf-8"))))

        for e, b0, b1 in sorted(blocks, key=lambda b: -b[1]):   # descending: indices stay valid
            lines[b0:b1] = [e["pointer"] + nl]
        out = "".join(lines)
        with open(os.path.join(ROOT, TARGET), "w", encoding="utf-8", newline="") as f:
            f.write(out)
        print("== applied ==")
        print("   %s  %d B / %d lines  ->  %d B / %d lines"
              % (TARGET, len(raw.encode("utf-8")), len(raw.splitlines()),
                 len(out.encode("utf-8")), len(out.splitlines())))
        print("   next: python tools\\addrcheck.py --all   (no citation may dangle)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
