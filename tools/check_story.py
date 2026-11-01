# -*- coding: utf-8 -*-
"""
FS 剧本静态校验器（R-05 门禁）
=============================

一条命令跑完剧本的全部结构性 + 配额性检查：

    python3 tools/check_story.py                       # 默认校验 data/story_v2.json
    python3 tools/check_story.py data/other.json       # 指定剧本
    python3 tools/check_story.py --json                # 机器可读输出

检查分四组：

  A. 构成   12 个节点 = 7 scripted + 1 fallback + 3 ending + 1 terminal
  B. 配额   正文字数 900~1100；台词句数 50~70（口径见 ARCH_FS §6.6）
  C. 结构   跳转目标存在 / 兜底节点存在 / 可挽回节点必带 redeem_variant /
            intent 规范（PRD §7.1 硬规则）/ 禁止【自由输入】选项
  D. 结局   三个 ending 在 affinity 轴上互斥且穷尽（ARCH 偏差 4 新增 lint）

⚠ 口径裁定（ARCH §6.6 / AR-11）：
    - "对话节点数" 必须是 count(kind == "scripted") == 7，
      **不能**写成 count(kind != "ending") == 7（那样会把 n_idle 算进来变成 8）。
    - -1 不计入 3 个结局。
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from typing import Any, Dict, List, Tuple

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_STORY = os.path.join(ROOT, "data", "story_v2.json")

# ---- 配额目标（ARCH §6.6）------------------------------------------------
CHARS_MIN, CHARS_MAX = 900, 1100
SENT_MIN, SENT_MAX = 50, 70
SCRIPTED_TARGET = 7
ENDING_TARGET = 3
TOTAL_TARGET = 12

# ---- 句数口径 -------------------------------------------------------------
# 1) 把连续的省略号压成单个 '…'，避免 "……" 被切成两个空句子；
# 2) 以 。！？… 任一处切分；
# 3) 丢弃纯空白片段。
_ELLIPSIS_RUN = re.compile(r"…{2,}")
_SENT_SPLIT = re.compile(r"[。！？…]")
_WS = re.compile(r"\s+")

_RESULTS: List[Tuple[bool, str, str]] = []


def check(ok: bool, name: str, detail: str = "") -> bool:
    _RESULTS.append((bool(ok), name, detail))
    print(f"{'[OK]' if ok else '[!!]'} {name}" + (f"  :: {detail}" if detail else ""))
    return bool(ok)


# ---------------------------------------------------------------------------
#  配额统计
# ---------------------------------------------------------------------------
def body_text_of(node: Dict[str, Any]) -> str:
    """正文 = narrator + npc_text + redeem_variant.{narrator,npc_text}。

    `hint` / `npc_state` 不计入（前者是给玩家的元提示，后者是注入 LLM 的状态词）。
    """
    pres = node.get("presentation") or {}
    parts = [pres.get("narrator", "") or "", pres.get("npc_text", "") or ""]
    rv = pres.get("redeem_variant") or {}
    if isinstance(rv, dict):
        parts.append(rv.get("narrator", "") or "")
        parts.append(rv.get("npc_text", "") or "")
    return "\n".join(parts)


def count_chars(text: str) -> int:
    """去空白与转义后的字符数。"""
    return len(_WS.sub("", text))


def count_sentences(text: str) -> int:
    """按 。！？… 与换行切分后的句子数。"""
    text = _ELLIPSIS_RUN.sub("…", text)
    total = 0
    for line in text.splitlines():
        for seg in _SENT_SPLIT.split(line):
            if seg.strip():
                total += 1
    return total


# ---------------------------------------------------------------------------
#  affinity 轴：从 requires 里抠出阈值
# ---------------------------------------------------------------------------
def _walk_affinity(cond: Any, ge: List[int], le: List[int]) -> None:
    if not isinstance(cond, dict):
        return
    for op, arg in cond.items():
        if op == "affinity_at_least" or op == "affinity_gte":
            ge.append(int(arg))
        elif op == "affinity_at_most" or op == "affinity_lte":
            le.append(int(arg))
        elif op in ("all", "any"):
            for sub in arg or []:
                _walk_affinity(sub, ge, le)


def affinity_bounds(node: Dict[str, Any]) -> Tuple[int, int, bool]:
    """返回 (下界, 上界, 是否被 affinity 把守)。

    缺省用 -100 / 200 兜底（引擎的 affinity clamp 边界）。
    `has_affinity` 用来区分"靠 affinity 把守"与"靠 item / phase 把守"的结局 ——
    只有前者才需要参与互斥性/穷尽性判定。
    """
    ge: List[int] = []
    le: List[int] = []
    _walk_affinity(node.get("requires"), ge, le)
    return (max(ge) if ge else -100, min(le) if le else 200, bool(ge or le))


# ---------------------------------------------------------------------------
#  主校验
# ---------------------------------------------------------------------------
def verify(doc: Dict[str, Any]) -> Dict[str, Any]:
    nodes: Dict[str, Dict[str, Any]] = doc.get("nodes") or {}
    entry = doc.get("entry", "0")

    scripted = [n for n, v in nodes.items() if v.get("kind") == "scripted"]
    endings = [n for n, v in nodes.items()
               if v.get("kind") == "ending" and not v.get("terminal", False) and n != "-1"]
    fallbacks = [n for n, v in nodes.items() if v.get("kind") == "fallback"]
    terminals = [n for n, v in nodes.items() if v.get("terminal", False) or n == "-1"]

    total_chars = 0
    total_sents = 0
    per_node: List[Tuple[str, int, int]] = []
    for nid in sorted(nodes):
        c = count_chars(body_text_of(nodes[nid]))
        s = count_sentences(body_text_of(nodes[nid]))
        total_chars += c
        total_sents += s
        per_node.append((nid, c, s))

    print("=" * 72)
    print("A. 节点构成")
    print("=" * 72)
    check(len(nodes) == TOTAL_TARGET, f"A-1 节点总数 == {TOTAL_TARGET}",
          f"实际 {len(nodes)}")
    check(len(scripted) == SCRIPTED_TARGET,
          f"A-2 count(kind=='scripted') == {SCRIPTED_TARGET}",
          f"实际 {len(scripted)}: {sorted(scripted)}")
    check(len(endings) == ENDING_TARGET,
          f"A-3 count(kind=='ending' && id!='-1') == {ENDING_TARGET}",
          f"实际 {len(endings)}: {sorted(endings)}")
    check(len(fallbacks) >= 1, "A-4 存在 kind=='fallback' 兜底节点",
          f"{sorted(fallbacks)}")
    check(len(terminals) >= 1, "A-5 存在 terminal 节点（-1）", f"{sorted(terminals)}")
    check(entry in nodes, "A-6 entry 节点存在", f"entry={entry}")
    # 反面哨兵：证明 'kind != ending' 口径会把 n_idle 算进来
    non_ending = [n for n, v in nodes.items() if v.get("kind") != "ending"]
    check(len(non_ending) == 8 and len(scripted) == 7,
          "A-7 口径哨兵：kind!='ending' 会算成 8，故必须用 kind=='scripted'",
          f"count(kind!='ending')={len(non_ending)} vs count(kind=='scripted')={len(scripted)}")

    print()
    print("=" * 72)
    print("B. 配额（ARCH §6.6）")
    print("=" * 72)
    print(f"  {'节点':<14}{'字数':>6}{'句数':>6}")
    for nid, c, s in per_node:
        print(f"  {nid:<14}{c:>6}{s:>6}")
    check(CHARS_MIN <= total_chars <= CHARS_MAX,
          f"B-1 正文字数 {CHARS_MIN}~{CHARS_MAX}", f"实际 {total_chars}")
    check(SENT_MIN <= total_sents <= SENT_MAX,
          f"B-2 台词句数 {SENT_MIN}~{SENT_MAX}", f"实际 {total_sents}")

    print()
    print("=" * 72)
    print("C. 结构性 lint")
    print("=" * 72)
    errs: List[str] = []

    for nid, v in sorted(nodes.items()):
        for field in ("default_next", "fallback_next"):
            tgt = v.get(field)
            if tgt and tgt not in nodes:
                errs.append(f"{nid}.{field}='{tgt}' 目标不存在")
        for opt in v.get("options") or []:
            tgt = opt.get("next")
            if tgt and tgt not in nodes:
                errs.append(f"{nid}.options.next='{tgt}' 目标不存在")
            if "自由输入" in (opt.get("text") or ""):
                errs.append(f"{nid} 的 options 出现【自由输入】"
                            "（自由输入是引擎能力，不是剧本选项）")

        # 可挽回节点必须带挽回变体（R-10）
        if v.get("redeemable"):
            rv = (v.get("presentation") or {}).get("redeem_variant") or {}
            if not rv.get("narrator") and not rv.get("npc_text"):
                errs.append(f"{nid} redeemable=true 但缺少 presentation.redeem_variant"
                            "（挽回时会回放旧台词，违反 C2）")
            if int(v.get("redeem_expire", 0)) <= 0:
                errs.append(f"{nid} redeemable=true 但 redeem_expire<=0，永不可能挽回")

        # intent 规范（PRD §7.1）
        it = v.get("intent") or {}
        desc = it.get("description", "")
        is_fallback = v.get("kind") == "fallback"
        if not desc and not is_fallback and not v.get("terminal"):
            errs.append(f"{nid} 缺少 intent.description，AI 将无法判断这条路通向什么")
        if desc and re.search(r"\b(n\d|end_|item_|tag|affinity|Frontier|PendingPool)\b", desc):
            errs.append(f"{nid} 的 intent.description 含节点 ID/引擎术语：{desc[:40]}")
        kw = it.get("keywords") or []
        if is_fallback:
            if kw:
                errs.append(f"{nid} 是保底节点，keywords 必须为空数组")
            if not it.get("must_freeform"):
                errs.append(f"{nid} 是保底节点，必须标 must_freeform=true")
        elif kw and not v.get("terminal"):
            if not (3 <= len(kw) <= 5):
                errs.append(f"{nid} 的 keywords 有 {len(kw)} 个，规范要求 3~5 个（PRD §7.1-4）")
        if not it.get("examples") and not is_fallback and not v.get("terminal"):
            errs.append(f"{nid} 缺少 intent.examples（PRD §7.1-5）")

    # 对话节点 phase.allow 必须全开（偏差 1）
    phases = doc.get("phases") or []
    for nid in sorted(scripted):
        allow = ((nodes[nid].get("phase") or {}).get("allow")) or []
        if allow and sorted(allow) != sorted(phases):
            errs.append(f"{nid} 是对话节点，phase.allow 必须全开 {phases}"
                        f"（实际 {allow}）——否则 requires 里的 affinity 分支永不见天日")

    for e in errs:
        print(f"  - {e}")
    check(not errs, "C-1 结构性 lint 无错误", f"{len(errs)} 条")

    print()
    print("=" * 72)
    print("D. 结局互斥性与穷尽性（ARCH 偏差 4）")
    print("=" * 72)
    # 结局分两类：
    #   ① affinity 轴把守的（end_calm / end_regret）—— 必须互斥且穷尽；
    #   ② 非 affinity 把守的（end_hidden，靠 item）—— 允许与①重叠，
    #      但 priority 必须严格高于①的全部成员，这样重叠时由 priority 决胜、无歧义。
    # 这正是 ARCH 偏差 4 的原话："end_hidden(100) 与 end_regret(90) 条件同时满足
    # -> priority 高者胜出 -> 真相优先于遗憾，符合叙事直觉"。
    axis: List[Tuple[str, int, int, bool]] = []
    for nid in sorted(endings):
        lo, hi, guarded = affinity_bounds(nodes[nid])
        axis.append((nid, lo, hi, guarded))
        print(f"  {nid:<12} affinity ∈ [{lo}, {hi}]  "
              f"priority={nodes[nid].get('priority')}  "
              f"把守={'affinity' if guarded else '非 affinity（item/phase）'}")

    bounded = sorted([a for a in axis if a[3]], key=lambda x: x[1])
    unbounded = [a for a in axis if not a[3]]

    ok_disjoint = True
    for i in range(len(bounded)):
        for j in range(i + 1, len(bounded)):
            a, b = bounded[i], bounded[j]
            overlap_lo, overlap_hi = max(a[1], b[1]), min(a[2], b[2])
            if overlap_lo <= overlap_hi:
                ok_disjoint = False
                print(f"  ✗ {a[0]} 与 {b[0]} 在 affinity {overlap_lo}~{overlap_hi} 重叠")
    check(ok_disjoint, "D-1 affinity 轴把守的结局两两不重叠（互斥）",
          "，".join(f"{n}[{lo},{hi}]" for n, lo, hi, _ in bounded))

    ok_exhaustive = True
    detail = ""
    if len(bounded) >= 2:
        for i in range(len(bounded) - 1):
            if bounded[i][2] + 1 != bounded[i + 1][1]:
                ok_exhaustive = False
                detail = f"{bounded[i][0]} 上界 {bounded[i][2]} 与 " \
                         f"{bounded[i + 1][0]} 下界 {bounded[i + 1][1]} 之间有空隙"
        if ok_exhaustive:
            detail = " + ".join(f"{n}[{lo},{hi}]" for n, lo, hi, _ in bounded)
    else:
        ok_exhaustive = False
        detail = "affinity 轴上有界的结局不足 2 个，无法判定穷尽性"
    check(ok_exhaustive, "D-2 affinity 轴上的结局首尾相接（穷尽，无死锁空隙）", detail)

    # 非 affinity 把守的结局必须 priority 严格最高，否则重叠时会出现歧义
    ok_priority = True
    pdetail = "无非 affinity 把守的结局"
    if unbounded:
        max_bounded = max((nodes[n].get("priority", 0) for n, _, _, _ in bounded), default=0)
        bad = [(n, nodes[n].get("priority", 0)) for n, _, _, _ in unbounded
               if nodes[n].get("priority", 0) <= max_bounded]
        ok_priority = not bad
        pdetail = ", ".join(f"{n}(p={nodes[n].get('priority')})" for n, _, _, _ in unbounded) \
                  + f" vs affinity 组最高 priority={max_bounded}"
        for n, p in bad:
            print(f"  ✗ {n} 与 affinity 组结局重叠，但 priority={p} 不占优 -> 命中歧义")
    check(ok_priority, "D-2b 与 affinity 组重叠的结局 priority 必须严格最高（由 priority 决胜）",
          pdetail)

    # 结局可达性演算（用 effects 反推，PRD §5.6 的验算表）
    print()
    print("  结局演算（初始 affinity=90）：")
    expect = [
        ("坦白线 A0", "+5 -3 +2", 94, "end_calm"),
        ("拒绝线·不挽回 B0", "-10 -3 +2", 79, "end_regret"),
        ("拒绝线·挽回成功 B", "-10 -3 +5 +2", 84, "end_calm"),
    ]
    ok_table = True
    for name, expr, aff, expect_end in expect:
        hit = [n for n, lo, hi, _g in axis if lo <= aff <= hi]
        # end_hidden 靠 item 把守，演算里没有发卡 -> 淘汰
        hit = [n for n in hit if n != "end_hidden"]
        good = hit == [expect_end]
        ok_table = ok_table and good
        print(f"    {name:<20}{expr:<14}aff={aff:<4}-> {hit or '（无）'}"
              f"  期望 {expect_end}  {'OK' if good else 'MISMATCH'}")
    check(ok_table, "D-3 三条主线的结局演算与 PRD §5.6 一致（挽回线 84 -> end_calm）")

    return {
        "nodes": len(nodes),
        "scripted": len(scripted),
        "endings": len(endings),
        "chars": total_chars,
        "sentences": total_sents,
        "errors": errs,
        "per_node": per_node,
    }


def main(argv: List[str]) -> int:
    ap = argparse.ArgumentParser(description="FS 剧本静态校验器")
    ap.add_argument("story", nargs="?", default=DEFAULT_STORY, help="剧本 JSON 路径")
    ap.add_argument("--json", action="store_true", help="机器可读输出")
    args = ap.parse_args(argv)

    with open(args.story, "r", encoding="utf-8") as f:
        doc = json.load(f)

    summary = verify(doc)

    if args.json:
        print("\n===JSON===")
        print(json.dumps({k: v for k, v in summary.items() if k != "per_node"},
                         ensure_ascii=False, indent=2))
        return 0 if not summary["errors"] else 1

    print()
    print("=" * 72)
    ok_count = sum(1 for ok_, _, _ in _RESULTS if ok_)
    for ok_, name, detail in _RESULTS:
        if not ok_:
            print(f"  FAILED: {name} :: {detail}")
    print(f"结果: {ok_count}/{len(_RESULTS)} 项通过"
          f"（字数 {summary['chars']}，句数 {summary['sentences']}，"
          f"scripted {summary['scripted']}，ending {summary['endings']}）")
    print("=" * 72)
    return 0 if ok_count == len(_RESULTS) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
