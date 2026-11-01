# -*- coding: utf-8 -*-
"""
NarrativeX - BFS 动态迷宫 (Dynamic BFS Labyrinth) 参考实现
===========================================================

本文件是 C++17 内核 `engine/include/nx/*.hpp` 的**逐行等价 Python 参考实现**，
用途有三：

1. **算法预言 (Test Oracle)**   —— 在没有 Qt/curl/sqlite 的环境下先跑通、
   再用同一组输入校验 C++ 版本的输出是否一致。
2. **离线仿真器**               —— 不需要联网、不需要大模型，用 StubLLM 即可
   完整验证 Frontier 重算 / Pending 事件挽回 / 意图匹配的闭环。
3. **设计文档的可执行形式**      —— docs/ 里的公式就是这里的 `FrontierScorer.score()`。

模块与 C++ 头文件的对应关系
----------------------------
    Condition.eval          <-> nx/Condition.hpp       cond_eval()
    NodeSpec / Catalog      <-> nx/NodeModel.hpp
    GameState / Rng         <-> nx/GameState.hpp
    Frontier / FrontierScorer <-> nx/Frontier.hpp     (空缺 1 的答案)
    PendingPool             <-> nx/PendingPool.hpp
    IntentMatcher           <-> nx/IntentMatch.hpp    (AI 匹配框架)
    PromptAssembler         <-> nx/PromptAssembler.hpp
    NarrativeEngine         <-> nx/NarrativeEngine.hpp
    EventStore              <-> persistence/include/nx/EventStore.hpp (空缺 2 的答案)

设计约束（三条不可违背的铁律）
------------------------------
  IRON-1 时间游标单调：seq 只能 +1，永不回退；任何分支都不得回到过去。
  IRON-2 历史只追加：所有状态变化以事件形式 append，禁止原地改写历史。
  IRON-3 结果可复现：随机性全部来自随 state 持久化的 rng_state，
         给定 (剧本, 事件序列) 必得唯一结果 —— 这是"读档不错乱"的根基。
"""

from __future__ import annotations

import hashlib
import json
import math
import time
from dataclasses import dataclass, field, asdict
from typing import Any, Dict, Iterable, List, Optional, Sequence, Set, Tuple

__all__ = [
    "ConditionError", "cond_eval", "NodeSpec", "Catalog", "GameState", "Rng",
    "FrontierScorer", "FrontierConfig", "Candidate", "PendingEntry", "PendingPool",
    "IntentMatcher", "IntentConfig", "IntentDecision", "PromptAssembler", "PromptBudget",
    "NarrativeEngine", "EventStore", "TurnResult", "StubLLM",
]

ENGINE_VERSION = "nx-bfs-0.2.0"


# =============================================================================
#  0.  基础工具
# =============================================================================

class ConditionError(RuntimeError):
    """剧本里的条件 DSL 写错时抛出，启动时立即失败（fail-fast）。"""


def _truthy(v: Any) -> bool:
    return bool(v)


# =============================================================================
#  1.  条件 DSL ( nx/Condition.hpp )
# =============================================================================
#
#  DSL 语法（JSON）：
#      { "all":  [c1, c2, ...] }              全部满足
#      { "any":  [c1, c2, ...] }              任一满足
#      { "not":  c }                          取反
#      { "tag":  "trust" }                    拥有标签
#      { "tags_all": ["a","b"] }              同时拥有
#      { "tags_any": ["a","b"] }              拥有其一
#      { "tag_none": ["a","b"] }              一个都没有
#      { "affinity_gte": 70 }                 好感度 >= 70
#      { "affinity_lte": 30 }                 好感度 <= 30
#      { "item": "item_hairclip" }            持有道具
#      { "items_all": ["x","y"] }
#      { "visited": "n_intro" }               去过某节点
#      { "not_visited": "n_secret" }          没去过
#      { "visit_count_gte": ["n_id", 2] }     去过至少 N 次
#      { "step_gte": 3 }                      time_cursor >= 3
#      { "step_lte": 20 }                     time_cursor <= 20
#      { "phase_in": ["evening","night"] }    处于某个物理时间阶段
#      { "phase_index_gte": 2 }               阶段序号 >= 2（阶段推进不可逆）
#      { "true": 1 } / { "false": 1 }         常量
#
#  设计要点：
#    * 采用"显式白名单算子"，未知算子直接抛错 —— 防止剧本作者拼错 key 导致
#      条件静默恒真/恒假（这是动态拓扑最常见的线上事故）。
#    * 条件求值必须是**纯函数**：只读 state，绝不写 state，这样才能被安全缓存。

_OPERATORS = {
    "all", "any", "not", "tag", "tags_all", "tags_any", "tag_none",
    "affinity_gte", "affinity_lte", "item", "items_all",
    "visited", "not_visited", "visit_count_gte",
    "step_gte", "step_lte", "phase_in", "phase_index_gte",
    "true", "false",
}


def cond_eval(cond: Optional[Dict[str, Any]], ctx: "EvalContext") -> bool:
    """求值条件 DSL。cond 为 None 视为恒真（无门槛）。

    ctx 提供只读状态视图：tags / items / affinity / visited / seq / phase ...
    """
    if cond is None:
        return True
    if not isinstance(cond, dict):
        raise ConditionError(f"条件必须是 JSON 对象，收到: {type(cond).__name__}")
    if not cond:
        return True
    if len(cond) != 1:
        raise ConditionError(f"每个条件对象只能有一个算子，收到: {list(cond)}")

    op, arg = next(iter(cond.items()))
    if op not in _OPERATORS:
        raise ConditionError(f"未知条件算子 '{op}'，合法算子: {sorted(_OPERATORS)}")

    if op == "true":
        return True
    if op == "false":
        return False
    if op == "all":
        return all(cond_eval(c, ctx) for c in _as_list(arg, op))
    if op == "any":
        return any(cond_eval(c, ctx) for c in _as_list(arg, op))
    if op == "not":
        return not cond_eval(arg, ctx)

    if op == "tag":
        return arg in ctx.tags
    if op == "tags_all":
        return all(t in ctx.tags for t in _as_list(arg, op))
    if op == "tags_any":
        return any(t in ctx.tags for t in _as_list(arg, op))
    if op == "tag_none":
        return all(t not in ctx.tags for t in _as_list(arg, op))

    if op == "affinity_gte":
        return ctx.affinity >= _as_int(arg, op)
    if op == "affinity_lte":
        return ctx.affinity <= _as_int(arg, op)

    if op == "item":
        return arg in ctx.items
    if op == "items_all":
        return all(i in ctx.items for i in _as_list(arg, op))

    if op == "visited":
        return arg in ctx.visited
    if op == "not_visited":
        return arg not in ctx.visited
    if op == "visit_count_gte":
        node_id, n = arg[0], int(arg[1])
        return ctx.visit_count.get(node_id, 0) >= n

    if op == "step_gte":
        return ctx.seq >= _as_int(arg, op)
    if op == "step_lte":
        return ctx.seq <= _as_int(arg, op)
    if op == "phase_in":
        return ctx.phase in _as_list(arg, op)
    if op == "phase_index_gte":
        return ctx.phase_index >= _as_int(arg, op)

    raise ConditionError(f"算子 '{op}' 未实现")  # pragma: no cover


def _as_list(arg: Any, op: str) -> List[Any]:
    if not isinstance(arg, list):
        raise ConditionError(f"算子 '{op}' 需要一个数组，收到: {arg!r}")
    return arg


def _as_int(arg: Any, op: str) -> int:
    if isinstance(arg, bool) or not isinstance(arg, (int, float)):
        raise ConditionError(f"算子 '{op}' 需要一个数字，收到: {arg!r}")
    return int(arg)


@dataclass(frozen=True)
class EvalContext:
    """条件的只读输入。刻意做成不可变，保证 cond_eval 无副作用。"""
    tags: frozenset = frozenset()
    items: frozenset = frozenset()
    affinity: int = 0
    visited: frozenset = frozenset()
    visit_count: Tuple[Tuple[str, int], ...] = ()
    seq: int = 0
    phase: str = ""
    phase_index: int = 0

    def count_of(self, node_id: str) -> int:
        return dict(self.visit_count).get(node_id, 0)


# =============================================================================
#  2.  动态节点模型 ( nx/NodeModel.hpp )
# =============================================================================

#: 节点种类 -> 分层桶。桶是**硬分层**，优先级高于打分函数，
#: 保证"主线收束"永远不会被"随机涌现"挤掉。
BUCKET_ORDER = {
    "ending": 40,        # 结局 / 主线收束（最高优先级，作者意图不可被覆盖）
    "scripted": 30,      # 主线脚本节点
    "redemption": 20,    # 被挽回的旧事件（后向指针唤醒）
    "emergent": 10,      # 由隐藏状态动态涌现的支线
    "fallback": 0,       # 兜底闲聊（防死锁的最后一道防线）
}


@dataclass
class Effects:
    """进入节点时产生的确定性副作用 —— 只有这里能改 state 的数值部分。"""
    add_tags: List[str] = field(default_factory=list)
    remove_tags: List[str] = field(default_factory=list)
    add_items: List[str] = field(default_factory=list)
    remove_items: List[str] = field(default_factory=list)
    affinity_delta: float = 0.0
    set_phase: Optional[str] = None
    summary_hint: str = ""


@dataclass
class IntentHint:
    """给 AI 匹配框架看的语义描述 —— 这是"AI 匹配框架"与"静态剧本"的接缝。"""
    description: str = ""          # 一句话语义描述，注入 {DYNAMIC_ROUTES}
    keywords: List[str] = field(default_factory=list)   # 强信号词（命中即高置信）
    examples: List[str] = field(default_factory=list)   # 玩家可能的原话（用于向量/字面相似）
    negatives: List[str] = field(default_factory=list)  # 反例（用于减分，抑制误命中）
    must_freeform: bool = False    # True 表示此节点只能由自然语言触发，不出现在选项里


@dataclass
class RedeemVariant:
    """挽回变体台词 —— 事件被后向指针拉回时呈现的文本。

    存在的唯一理由是实现硬约束 C2（动态重生成）：
    唤醒时**不回放旧台词**，而是在"过去的时间语境里、用现在的口径把这件事重讲一遍"。
    没有变体的话，玩家第二次走到同一个节点会看到一模一样的文本，
    "挽回"在观感上就退化成了"读档重来"。
    """
    has: bool = False
    narrator: str = ""
    npc_text: str = ""
    npc_state: str = ""

    @staticmethod
    def from_json(raw: Any) -> "RedeemVariant":
        if not isinstance(raw, dict) or not raw:
            return RedeemVariant()
        return RedeemVariant(
            has=bool(raw.get("narrator") or raw.get("npc_text") or raw.get("npc_state")),
            narrator=raw.get("narrator") or "",
            npc_text=raw.get("npc_text") or "",
            npc_state=raw.get("npc_state") or "",
        )


@dataclass
class NodeSpec:
    """动态节点：不再依赖写死的 next 连线，而是"条件 + 优先级 + 生命周期"。"""
    id: str
    kind: str = "scripted"                  # scripted|emergent|redemption|ending|fallback
    #: 终末占位节点（如旧版的 "-1"）。terminal 节点**永远不进入 Frontier**，
    #: 它只能作为跳转目标。少了这个标志，一个无任何 requires 的 ending 节点会
    #: 常驻最高 bucket，导致开局就被路由到"游戏结束"——这是仿真实测抓到的缺陷。
    terminal: bool = False
    presentation: Dict[str, str] = field(default_factory=dict)
    options: List[Dict[str, Any]] = field(default_factory=list)
    default_next: Optional[str] = None
    fallback_next: Optional[str] = None

    # ---- 生命周期（支柱一：时间游标） ----
    min_step: int = 0
    max_step: int = 1_000_000_000

    # ---- 物理时间阶段（防止"晚上吃午饭"） ----
    phases: Optional[List[str]] = None
    phase_index_min: int = 0
    phase_index_max: int = 1_000_000_000

    # ---- 准入（支柱二：动态拓扑） ----
    requires: Optional[Dict[str, Any]] = None

    # ---- 排序（空缺 1：多节点优先级） ----
    priority: int = 50
    weight: float = 1.0
    once: bool = True
    cooldown: int = 0
    exclusive_group: Optional[str] = None

    # ---- 唤醒条件（支柱三：事件挽回的后向指针） ----
    redeemable: bool = False
    redeem_gate: Optional[Dict[str, Any]] = None
    redeem_expire: int = 1_000_000_000      # 距 birth_seq 多少步之后彻底失效

    effects: Effects = field(default_factory=Effects)
    intent: IntentHint = field(default_factory=IntentHint)
    #: 挽回变体（C2 不回放旧台词）。redeemable 节点必须提供，lint 会拦。
    redeem_variant: RedeemVariant = field(default_factory=RedeemVariant)

    # ------------------------------------------------------------------
    @property
    def bucket(self) -> int:
        return BUCKET_ORDER.get(self.kind, 0)

    def window_ok(self, seq: int) -> bool:
        """生命周期窗口检查。max_step 已过 => 该节点永久失效。"""
        return self.min_step <= seq <= self.max_step

    def phase_ok(self, phase: str, phase_index: int) -> bool:
        if self.phases is not None and phase not in self.phases:
            return False
        return self.phase_index_min <= phase_index <= self.phase_index_max

    def cooldown_ok(self, seq: int, last_visit: Dict[str, int]) -> bool:
        if self.cooldown <= 0:
            return True
        last = last_visit.get(self.id)
        return last is None or (seq - last) >= self.cooldown

    # ------------------------------------------------------------------
    @staticmethod
    def from_json(node_id: str, raw: Dict[str, Any]) -> "NodeSpec":
        w = raw.get("window") or {}
        p = raw.get("phase") or {}
        e = raw.get("effects") or {}
        it = raw.get("intent") or {}
        fx = raw.get("effects") or {}
        pres = raw.get("presentation") or {}
        return NodeSpec(
            id=node_id,
            kind=raw.get("kind", "scripted"),
            terminal=bool(raw.get("terminal", False)),
            presentation=pres,
            options=raw.get("options") or [],
            default_next=raw.get("default_next"),
            fallback_next=raw.get("fallback_next"),
            min_step=int(w.get("min_step", 0)),
            max_step=int(w.get("max_step", 1_000_000_000)),
            phases=p.get("allow"),
            phase_index_min=int(p.get("min_index", 0)),
            phase_index_max=int(p.get("max_index", 1_000_000_000)),
            requires=raw.get("requires"),
            priority=int(raw.get("priority", 50)),
            weight=float(raw.get("weight", 1.0)),
            once=bool(raw.get("once", True)),
            cooldown=int(raw.get("cooldown", 0)),
            exclusive_group=raw.get("exclusive_group"),
            redeemable=bool(raw.get("redeemable", False)),
            redeem_gate=raw.get("redeem_gate"),
            redeem_expire=int(raw.get("redeem_expire", 1_000_000_000)),
            effects=Effects(
                add_tags=list(e.get("add_tags", [])),
                remove_tags=list(e.get("remove_tags", [])),
                add_items=list(e.get("add_items", [])),
                remove_items=list(e.get("remove_items", [])),
                affinity_delta=float(fx.get("affinity_delta", 0.0)),
                set_phase=e.get("set_phase"),
                summary_hint=e.get("summary_hint", ""),
            ),
            intent=IntentHint(
                description=it.get("description", raw.get("presentation", {}).get("hint", "")),
                keywords=list(it.get("keywords", [])),
                examples=list(it.get("examples", [])),
                negatives=list(it.get("negatives", [])),
                must_freeform=bool(it.get("must_freeform", False)),
            ),
            redeem_variant=RedeemVariant.from_json(pres.get("redeem_variant")),
        )


class Catalog:
    """剧本字典 + 倒排索引。

    索引的作用：把每次 Frontier 重算从 **O(全部节点)** 降到 **O(受影响的候选)**。
    索引键 = 节点 `requires` 中出现的 tag / item，以及 phase。
    不含任何 requires 的节点（如 fallback）进入 `_always` 集合，恒定参与。
    """

    def __init__(self, nodes: Dict[str, NodeSpec], phases: Sequence[str] = (),
                 entry: str = "0"):
        self.nodes: Dict[str, NodeSpec] = nodes
        self.phases: List[str] = list(phases)
        self.entry: str = entry
        self.by_tag: Dict[str, Set[str]] = {}
        self.by_item: Dict[str, Set[str]] = {}
        self.always: Set[str] = set()
        self.neighbors: Dict[str, Set[str]] = {}
        self._build_index()

    # ------------------------------------------------------------------
    @staticmethod
    def from_json(doc: Dict[str, Any]) -> "Catalog":
        raw_nodes = doc.get("nodes") or {}
        nodes: Dict[str, NodeSpec] = {}
        for nid, raw in raw_nodes.items():
            nodes[nid] = NodeSpec.from_json(nid, raw)
        entry = str(doc.get("entry", "0"))
        cat = Catalog(nodes, doc.get("phases") or [], entry)
        # entry 必须传给 validate：剧本的入口节点由 doc["entry"] 指定，
        # 写死 "0" 会让任何不以 "0" 为入口的剧本被误判成"全部节点不可达"。
        cat.validate(entry=entry)
        return cat

    # ------------------------------------------------------------------
    def new_state(self, session_id: str = "local", initial_affinity: int = 90,
                  seed: int = 0xC0FFEE) -> "GameState":
        """新建一个"开局"状态 —— 与 C++ `StoryLoader::new_state` 一一对应。

        为什么必须有这个工厂（而不是让调用方裸写 `GameState()`）：
        1. `GameState.current_node` 的默认值是 `"0"`（占位），**不是剧本入口**。
           裸写会让第一步的 `_match_option` 因为 `current=None` 而失效，
           玩家点选项①却只能走 L1 意图打分，被路由回入口节点本身 ——
           PRD §6.1 用例 A 第 1 步「点选项①→n2_kitchen」直接红。
        2. C++ 侧 `StoryLoader::new_state()` 会写 `st.current_node = entry`，
           Python 侧若不做同样的事，两侧从第 0 步起就分叉，交叉验证失去意义。
        """
        return GameState(session_id=session_id, seq=0, current_node=self.entry,
                         affinity=initial_affinity, rng_state=seed)

    def _index_tags(self, cond: Any, nid: str, hit: bool) -> None:
        """把条件里出现的所有 tag / item 登记到倒排表。"""
        if not isinstance(cond, dict):
            return
        for op, arg in cond.items():
            if op in ("all", "any"):
                for c in arg or []:
                    self._index_tags(c, nid, hit)
            elif op == "not":
                # 否定形式的条件无法用"存在性"剪枝，保守标记为 always-candidate
                self.always.add(nid)
            elif op in ("tag", "tags_all", "tags_any"):
                for t in ([arg] if isinstance(arg, str) else arg):
                    self.by_tag.setdefault(t, set()).add(nid)
            elif op == "item" or op == "items_all":
                for i in ([arg] if isinstance(arg, str) else arg):
                    self.by_item.setdefault(i, set()).add(nid)
            elif op in ("step_gte", "step_lte", "phase_in", "phase_index_gte",
                        "visited", "not_visited", "visit_count_gte",
                        "affinity_gte", "affinity_lte", "tag_none"):
                # 这些条件依赖"全局状态"或"否定"，不受单点 tag 增量影响
                self.always.add(nid)

    def _build_index(self) -> None:
        for nid, node in self.nodes.items():
            if node.requires is None:
                self.always.add(nid)
            else:
                self._index_tags(node.requires, nid, True)

            # 前向指针：当前节点能直达哪些节点（显式选项 + default + fallback）
            out: Set[str] = set()
            for opt in node.options:
                nxt = opt.get("next")
                if nxt:
                    out.add(nxt)
            for attr in ("default_next", "fallback_next"):
                v = getattr(node, attr)
                if v:
                    out.add(v)
            self.neighbors[nid] = out

    # ------------------------------------------------------------------
    def validate(self, entry: str = "0") -> None:
        """启动期校验 —— 把剧本错误挡在运行时之前。"""
        errs: List[str] = []
        if entry not in self.nodes:
            errs.append(f"入口节点 '{entry}' 不存在")
        for nid, node in self.nodes.items():
            for attr, nid2 in (("default_next", node.default_next),
                               ("fallback_next", node.fallback_next)):
                if nid2 and nid2 not in self.nodes:
                    errs.append(f"节点 {nid} 的 {attr}='{nid2}' 不存在")
            for opt in node.options:
                nxt = opt.get("next")
                if nxt and nxt not in self.nodes:
                    errs.append(f"节点 {nid} 的选项 next='{nxt}' 不存在")
            # 冷启动：条件必须可解
            try:
                cond_eval(node.requires, EvalContext())
            except ConditionError as e:
                errs.append(f"节点 {nid} 的 requires 非法: {e}")
            # R-10：可挽回节点必须带挽回变体，否则挽回时会回放旧台词（违反 C2）
            if node.redeemable and not node.redeem_variant.has:
                errs.append(f"节点 {nid} redeemable=true 但缺少 presentation.redeem_variant"
                            "（挽回时会回放旧台词，违反硬约束 C2）")
            if node.redeemable and node.redeem_expire <= 0:
                errs.append(f"节点 {nid} redeemable=true 但 redeem_expire<=0，永不可能挽回")
        if not any(n.kind == "fallback" for n in self.nodes.values()):
            errs.append("剧本缺少 kind=fallback 的兜底节点：一旦 Frontier 重算为空就会死锁")
        missing = set(self.nodes) - self._ceiling_reachable(entry)
        if missing:
            errs.append(
                "以下节点既不在任何节点的出边中，也不是 kind ∈ {emergent,redemption,fallback}："
                f"{sorted(missing)} —— 这类节点永远进不了 Frontier（死节点）")
        if errs:
            raise ValueError("剧本校验失败:\n  - " + "\n  - ".join(errs))

    #: 通过拓扑本身进入的节点种类：它们的入口是 **Frontier 倒排索引 + 后向指针**，
    #: 而不是写死的 next 连线，因此静态可达性分析必须豁免它们，否则会把整个
    #: 动态拓扑模型误判为"不可达"。这也是旧静态分支树模型与新模型最大的差别。
    _INDEX_REACHABLE_KINDS = {"emergent", "redemption", "fallback"}

    def _ceiling_reachable(self, entry: str = "0") -> Set[str]:
        reach = self._reachable_from_start(entry)
        reach |= {n.id for n in self.nodes.values()
                  if n.kind in self._INDEX_REACHABLE_KINDS}
        return reach

    def _reachable_from_start(self, start: str = "0") -> Set[str]:
        if start not in self.nodes:
            return set()
        seen, stack = {start}, [start]
        while stack:
            cur = stack.pop()
            for nxt in self.neighbors.get(cur, ()):
                if nxt not in seen:
                    seen.add(nxt)
                    stack.append(nxt)
        # union：所有 kind=fallback 的节点永远可达
        seen |= {n.id for n in self.nodes.values() if n.kind == "fallback"}
        return seen

    # ------------------------------------------------------------------
    def candidate_ids(self, state: "GameState") -> Set[str]:
        """倒排索引召回：只取"可能被这次状态变化影响"的节点。"""
        out: Set[str] = set(self.always)
        for t in state.tags:
            out |= self.by_tag.get(t, set())
        for i in state.items:
            out |= self.by_item.get(i, set())
        out |= self.neighbors.get(state.current_node, set())
        for p in state.pending:
            out.add(p.node_id)
        return {n for n in out if n in self.nodes}


# =============================================================================
#  3.  游戏状态与随机数 ( nx/GameState.hpp )
# =============================================================================

class Rng:
    """splitmix64。种子本身随存档持久化 —— 这样读档后 Frontier 的抽样结果一致。

    这是 IRON-3(可复现) 的关键：如果用了不可控熵源（std::random_device /
    Python 的 os.urandom），同一存档两次加载就会走出不同剧情，即"时空错乱"。
    """

    __slots__ = ("state",)

    def __init__(self, seed: int):
        self.state = seed & 0xFFFFFFFFFFFFFFFF

    def next_u64(self) -> int:
        self.state = (self.state + 0x9E3779B97F4A7C15) & 0xFFFFFFFFFFFFFFFF
        z = self.state
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & 0xFFFFFFFFFFFFFFFF
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & 0xFFFFFFFFFFFFFFFF
        return (z ^ (z >> 31)) & 0xFFFFFFFFFFFFFFFF

    def next_float(self) -> float:
        return (self.next_u64() >> 11) * (1.0 / (1 << 53))

    def snapshot(self) -> int:
        return self.state


@dataclass
class PendingEntry:
    """挂起事件：岔路口没被选中的那条路，被压入这里等待后向指针唤醒。"""
    node_id: str
    birth_seq: int
    reason: str = "unselected"
    source_node: str = ""
    attempts: int = 0
    awake_seq: Optional[int] = None

    @property
    def awake(self) -> bool:
        return self.awake_seq is not None


@dataclass
class GameState:
    """完整的、可序列化的游戏状态。所有字段都必须能进 SQLite。"""
    session_id: str = "local"
    seq: int = 0                       # time_cursor
    phase: str = "evening"
    phase_index: int = 0
    current_node: str = "0"
    affinity: int = 90
    tags: Set[str] = field(default_factory=set)
    items: Set[str] = field(default_factory=set)
    visited: Dict[str, int] = field(default_factory=dict)     # node -> first_seq
    last_visit: Dict[str, int] = field(default_factory=dict)  # node -> last_seq
    visit_count: Dict[str, int] = field(default_factory=dict)
    pending: List[PendingEntry] = field(default_factory=list)
    rng_state: int = 0x12345678
    summary: str = ""
    history_tail: List[Dict[str, str]] = field(default_factory=list)

    # ------------------------------------------------------------------
    def ctx(self) -> EvalContext:
        return EvalContext(
            tags=frozenset(self.tags),
            items=frozenset(self.items),
            affinity=self.affinity,
            visited=frozenset(self.visited.keys()),
            visit_count=tuple(self.visit_count.items()),
            seq=self.seq,
            phase=self.phase,
            phase_index=self.phase_index,
        )

    def to_json(self) -> str:
        d = {
            "session_id": self.session_id, "seq": self.seq, "phase": self.phase,
            "phase_index": self.phase_index, "current_node": self.current_node,
            "affinity": self.affinity,
            "tags": sorted(self.tags), "items": sorted(self.items),
            "visited": self.visited, "last_visit": self.last_visit,
            "visit_count": self.visit_count,
            "pending": [asdict(p) for p in self.pending],
            "rng_state": self.rng_state, "summary": self.summary,
            "history_tail": self.history_tail,
        }
        return json.dumps(d, ensure_ascii=False, sort_keys=True)

    def hash(self) -> str:
        return hashlib.sha256(self.to_json().encode("utf-8")).hexdigest()[:16]

    @staticmethod
    def from_json(s: str) -> "GameState":
        d = json.loads(s)
        st = GameState(
            session_id=d["session_id"], seq=d["seq"], phase=d["phase"],
            phase_index=d["phase_index"], current_node=d["current_node"],
            affinity=d["affinity"], tags=set(d["tags"]), items=set(d["items"]),
            visited={k: int(v) for k, v in d["visited"].items()},
            last_visit={k: int(v) for k, v in d["last_visit"].items()},
            visit_count={k: int(v) for k, v in d["visit_count"].items()},
            pending=[PendingEntry(**p) for p in d["pending"]],
            rng_state=int(d["rng_state"]), summary=d.get("summary", ""),
            history_tail=list(d.get("history_tail", [])),
        )
        return st


# =============================================================================
#  4.  Frontier 重算与多节点优先级排序 ——【空缺 1 的答案】( nx/Frontier.hpp )
# =============================================================================
#
#  算法分 4 步，**硬分层 + 软打分 + 稳定排序 + 防死锁兜底**：
#
#    Step 1  GATE   硬过滤：生命周期窗口 / 物理阶段 / requires / once / cooldown
#                   -> feasible 集合
#    Step 2  SCORE  多因子线性融合（见 FrontierScorer.score）
#    Step 3  RANK   按 (bucket desc, score desc, node_id asc) 稳定排序
#                   -> bucket 保证主线 > 挽回 > 涌现 > 兜底，永不被"意外高分"翻越
#                   -> node_id asc 作为最终 tie-break，保证结果确定性
#    Step 4  SAFETY feasible 为空 => 注入 idle fallback，绝不返回空（防死锁）
#
#  复杂度：GATE 只扫倒排索引召回集 O(|C|)，C << |V|；每步还要配上 O(|C| log|C|)
#  排序。相比"全局遍历"，500 节点量级的剧本每步可省 ~90% 的求值。

@dataclass
class FrontierConfig:
    """打分权重。全部可在剧本的 `frontier` 段覆盖，便于作者调参。"""
    w_base: float = 1.0        # 作者的 priority 意图
    w_urgency: float = 0.8     # 生命周期紧迫度：max_step 快到了就优先
    w_staleness: float = 1.2   # 反重复：刚访问过的降权（指数衰减）
    w_affinity: float = 0.4    # 好感度与节点设计意图的贴合度
    w_recency: float = 0.35    # 新鲜度：还没探索过的加分
    w_cooldown: float = 2.0    # 冷却剩余惩罚
    w_group: float = 3.0       # 互斥组已被选中后的连带惩罚
    w_pending_age: float = 0.5 # 挂起越久，挽回时优先级越高（情感补偿）
    staleness_tau: float = 6.0 # 指数衰减半衰（以 step 计）
    urgency_horizon: int = 12  # 认为"快要过期"的视野长度
    top_k: int = 5             # 送入 AI 意图层的候选数（受 token 预算二次约束）
    temperature: float = 0.0   # >0 时开启 ε-扰动多样性，0 = 完全确定性

    @staticmethod
    def from_json(d: Optional[Dict[str, Any]]) -> "FrontierConfig":
        if not d:
            return FrontierConfig()
        base = asdict(FrontierConfig())
        base.update({k: v for k, v in d.items() if k in base})
        return FrontierConfig(**base)


@dataclass
class Candidate:
    node_id: str
    bucket: int
    score: float
    parts: Dict[str, float] = field(default_factory=dict)

    def explain(self) -> str:
        detail = " ".join(f"{k}={v:+.2f}" for k, v in self.parts.items())
        return f"[{self.node_id}] bucket={self.bucket} score={self.score:+.3f} :: {detail}"


class FrontierScorer:
    def __init__(self, cfg: Optional[FrontierConfig] = None):
        self.cfg = cfg or FrontierConfig()

    # ------------------------------------------------------------------
    def gate(self, catalog: Catalog, state: GameState, nid: str) -> Tuple[bool, str]:
        """返回 (是否通过, 被拦截的原因)。原因会进 debug 日志，用于排剧情断裂。"""
        node = catalog.nodes[nid]
        if node.terminal:
            return False, "terminal"         # 终末节点只做跳转目标，不做 Frontier 成员
        if not node.window_ok(state.seq):
            return False, "window" if state.seq > node.max_step else "future"
        if not node.phase_ok(state.phase, state.phase_index):
            return False, "phase"
        if node.once and state.visit_count.get(nid, 0) >= 1:
            return False, "consumed"
        if not node.cooldown_ok(state.seq, state.last_visit):
            return False, "cooldown"
        try:
            if not cond_eval(node.requires, state.ctx()):
                return False, "requires"
        except ConditionError as e:
            return False, f"bad_cond:{e}"
        return True, "ok"

    # ------------------------------------------------------------------
    def score(self, catalog: Catalog, state: GameState, nid: str,
              awakened_from: Optional[PendingEntry] = None) -> Candidate:
        cfg = self.cfg
        node = catalog.nodes[nid]
        parts: Dict[str, float] = {}

        # f_base —— 作者意图（0~100 归一到 0~1 区间）
        parts["base"] = cfg.w_base * (node.priority / 100.0)

        # f_urgency —— 生命周期紧迫度：离 max_step 越近越紧急
        if node.max_step < 1_000_000_000:
            remain = node.max_step - state.seq
            urgency = max(0.0, 1.0 - max(0, remain) / float(cfg.urgency_horizon))
        else:
            urgency = 0.0
        parts["urgency"] = cfg.w_urgency * urgency

        # f_staleness —— 反重复：最近访问过的节点指数衰减降权
        lv = state.last_visit.get(nid)
        if lv is None:
            staleness = 0.0
        else:
            staleness = math.exp(-(state.seq - lv) / cfg.staleness_tau)
        parts["staleness"] = -cfg.w_staleness * staleness

        # f_recency —— 新鲜度：从没去过的节点加分
        parts["recency"] = cfg.w_recency * (0.0 if nid in state.visited else 1.0)

        # f_affinity —— 与节点设计意图的贴合（用 requires 里的 affinity_gte/lte 推断）
        target = self._affinity_target(node)
        if target is None:
            parts["affinity"] = 0.0
        else:
            dist = abs(state.affinity - target) / 100.0
            parts["affinity"] = -cfg.w_affinity * dist

        # f_cooldown —— 冷却剩余
        lv = state.last_visit.get(nid)
        if node.cooldown > 0 and lv is not None:
            left = max(0, node.cooldown - (state.seq - lv))
            parts["cooldown"] = -cfg.w_cooldown * (left / float(node.cooldown))
        else:
            parts["cooldown"] = 0.0

        # f_pending_age —— 挂起越久，挽回的分量越重（"弥补遗憾"的情感设计）
        if awakened_from is not None:
            age = state.seq - awakened_from.birth_seq
            parts["pending_age"] = cfg.w_pending_age * math.log1p(age) / 3.0

        total = sum(parts.values()) * node.weight
        return Candidate(node_id=nid, bucket=node.bucket, score=round(total, 6), parts=parts)

    @staticmethod
    def _affinity_target(node: NodeSpec) -> Optional[int]:
        """从 requires 里提取作者设定的好感度锚点，用于贴合度打分。"""
        found: List[int] = []

        def walk(c: Any) -> None:
            if not isinstance(c, dict):
                return
            for op, arg in c.items():
                if op in ("affinity_gte", "affinity_lte"):
                    found.append(int(arg))
                elif op in ("all", "any"):
                    for sub in arg or []:
                        walk(sub)
        walk(node.requires)
        return int(sum(found) / len(found)) if found else None

    # ------------------------------------------------------------------
    def refresh(self, catalog: Catalog, state: GameState,
                rng: Optional[Rng] = None,
                awakened: Optional[List[PendingEntry]] = None
                ) -> Tuple[List[Candidate], Dict[str, List[str]]]:
        """核心：重算 Current_Frontier。

        返回 (排序后的候选列表, 被拦截的原因字典) —— 后者直接服务于
        "剧情断裂时查看当前 time_cursor 的 Frontier 列表" 这一调试诉求。
        """
        awakened_map = {p.node_id: p for p in (awakened or [])}
        reasons: Dict[str, List[str]] = {}
        feasible: List[Candidate] = []

        for nid in catalog.candidate_ids(state):
            ok, why = self.gate(catalog, state, nid)
            if not ok:
                reasons.setdefault(why, []).append(nid)
                continue
            feasible.append(self.score(catalog, state, nid, awakened_map.get(nid)))

        # Step 3 —— 稳定排序：bucket 硬分层 -> score -> node_id 字典序兜底
        feasible.sort(key=lambda c: (-c.bucket, -c.score, c.node_id))

        # 多样性扰动（可选，默认关闭 = 完全确定性）
        if rng is not None and self.cfg.temperature > 0.0:
            feasible = self._epsilon_perturb(feasible, rng)

        # Step 4 —— 防死锁：任何时刻 Frontier 都不得为空
        if not feasible:
            eligible = self._fallbacks(catalog, state)
            if eligible:
                nid = eligible[0]
                feasible = [Candidate(nid, catalog.nodes[nid].bucket, -999.0,
                                      {"fallback": -999.0})]
            else:
                raise RuntimeError(
                    f"剧本死锁：seq={state.seq} node={state.current_node} 无任何可用节点，"
                    f"且缺少 kind=fallback 的兜底节点")

        return feasible, reasons

    def _epsilon_perturb(self, cands: List[Candidate], rng: Rng) -> List[Candidate]:
        """在同一 bucket 内做有界随机扰动，避免每次都走同一条路（但保持可复现）。"""
        if not cands:
            return cands
        eps = self.cfg.temperature
        out: List[Candidate] = []
        buckets: Dict[int, List[Candidate]] = {}
        for c in cands:
            buckets.setdefault(c.bucket, []).append(c)
        for b in sorted(buckets, reverse=True):
            group = buckets[b]
            top = group[0].score if group else 0.0
            for c in group:
                noise = rng.next_float() * eps * max(1.0, abs(top))
                group[group.index(c)] = Candidate(c.node_id, c.bucket, c.score + noise, c.parts)
            group.sort(key=lambda c: (-c.score, c.node_id))
            out.extend(group)
        return out

    @staticmethod
    def _fallbacks(catalog: Catalog, state: GameState) -> List[str]:
        out = []
        for nid, node in catalog.nodes.items():
            if node.kind != "fallback":
                continue
            if not node.window_ok(state.seq) or not node.phase_ok(state.phase, state.phase_index):
                continue
            try:
                if not cond_eval(node.requires, state.ctx()):
                    continue
            except ConditionError:
                continue
            out.append(nid)
        out.sort()
        return out


# =============================================================================
#  5.  PendingPool 后向指针 —— 事件挽回 ( nx/PendingPool.hpp )
# =============================================================================
#
#  "事件挽回"的本质是一个**受限的回溯**：
#      - 允许把某个"当时没走的路"重新拉回现在的 Frontier；
#      - 但绝不允许把 time_cursor 拉回过去（IRON-1）。
#
#  因此实现上有两个关键约束：
#    C1  单调唤醒：awake_seq > birth_seq 恒成立，且 awake_seq == 当前 seq。
#    C2  动态重生成：唤醒时**不回放旧文本**，而是把节点重新放进 Frontier，
#        由 AI 依据当前的 {phase} / {tags} / {history} 重新生成台词。
#        这就是文档里说的"在过去的时间语境里，用现在的口径重讲一次"。

class PendingPool:
    def __init__(self, catalog: Catalog):
        self.catalog = catalog

    # ------------------------------------------------------------------
    def push(self, state: GameState, node_id: str, reason: str = "unselected",
             source_node: str = "") -> bool:
        """把未选择的支线压入挂起池。

        幂等：同一节点同一来源不会重复入池（防止多轮对话把池子撑爆）。
        """
        if node_id not in self.catalog.nodes:
            return False
        node = self.catalog.nodes[node_id]
        if not node.redeemable:
            return False
        if any(p.node_id == node_id and not p.awake for p in state.pending):
            return False
        state.pending.append(PendingEntry(
            node_id=node_id, birth_seq=state.seq, reason=reason, source_node=source_node))
        return True

    # ------------------------------------------------------------------
    def scan(self, state: GameState) -> List[PendingEntry]:
        """后向指针扫描：找出"现在终于满足激活条件"的挂起事件。"""
        if not state.pending:
            return []
        ctx = state.ctx()
        awakened: List[PendingEntry] = []
        alive: List[PendingEntry] = []

        for entry in state.pending:
            node = self.catalog.nodes.get(entry.node_id)
            if node is None:                               # 剧本改版后节点消失 -> 销毁
                continue

            # ---- C1 单调性 + 过期（过期判定必须排在 awake 之前）----
            # 语义：redeem_expire 是"这件事在叙事上还能被提起多久"。
            # 一个被唤醒、但玩家始终没有走上去的事件，同样应该过期销毁 ——
            # 否则挂起池会无限增长（存档泄漏），且"错过就是错过"不成立
            # （对照组 B0 的 B-6 断言正是要证明这一点）。
            # 旧实现把 `if entry.awake: continue` 放在最前面，导致已唤醒条目
            # 要么立刻被丢弃（Python，留档丢失）要么永久滞留（C++，泄漏），
            # 两侧行为也不一致。现统一为"留档至过期为止"。
            if state.seq < entry.birth_seq:                # 理论不可能，防御性检查
                alive.append(entry)
                continue
            if state.seq - entry.birth_seq > node.redeem_expire:
                continue                                   # 彻底过期，销毁
            if entry.awake:
                alive.append(entry)                        # 已挽回，留档到过期为止
                continue
            if state.seq - entry.birth_seq < 1:
                alive.append(entry)                        # 同一步不允许自我唤醒
                continue

            gate = node.redeem_gate if node.redeem_gate is not None else node.requires
            try:
                ready = cond_eval(gate, ctx)
            except ConditionError:
                ready = False

            if ready:
                entry.awake_seq = state.seq                # C1：只写当下
                awakened.append(entry)
            alive.append(entry)

        state.pending = alive
        return awakened

    # ------------------------------------------------------------------
    @staticmethod
    def stats(state: GameState) -> Dict[str, int]:
        open_items = [p for p in state.pending if not p.awake]
        return {
            "total_events": len(state.pending),
            "open": len(open_items),
            "redeemed": len(state.pending) - len(open_items),
        }


# =============================================================================
#  6.  AI 意图匹配框架 ( nx/IntentMatch.hpp )
# =============================================================================
#
#  三级漏斗 —— 目标：**把 LLM 从"创作者"降级为"分类器"**。
#
#    L0 确定性快判 (0 token, 0 延迟)
#        选项序号、系统指令、正则强信号。命中且独占 -> 直接路由，不发请求。
#    L1 本地语义打分 (0 token, ~0.1ms)
#        关键词命中 + 字符二元组 Jaccard 相似度 + 反例扣分 + MMR 去冗余。
#        产出 Top-K 有序候选。
#    L2 LLM 受限选择 (少量 token)
#        只把 Top-K 的 {id, 一句话描述} 注入 {DYNAMIC_ROUTES}，让模型做**单选题**。
#        返回值必须落在本次注入的白名单内，否则判为幻觉并降级到 L1 第一名。
#
#  为什么不让 LLM 直接输出节点 ID 而不给候选？
#    -> 因为自由生成会产生不存在的节点ID（幻觉），现有 GameEngine 已用
#       `storyData["nodes"].contains(nextNode)` 打了补丁，但那是事后兜底，
#       事后兜底会造成"玩家说了半天，剧情原地不动"的体验断层。
#       白名单约束 + 事后校验双保险，才能把幻觉率压到接近 0。

@dataclass
class IntentConfig:
    kw_hit_score: float = 0.55        # 单个关键词命中的加分
    kw_full_bonus: float = 0.30       # 关键词全部命中的额外奖励
    sim_weight: float = 0.45          # 字面相似度权重
    neg_penalty: float = 0.5          # 命中反例的扣分
    l0_confident: float = 0.86        # L0 独占命中阈值，超过直接路由
    l1_margin: float = 0.12           # Top1 领先第二名多少才算"有把握"
    mmr_lambda: float = 0.25          # MMR 去冗余系数
    top_k: int = 5

    @staticmethod
    def from_json(d: Optional[Dict[str, Any]]) -> "IntentConfig":
        if not d:
            return IntentConfig()
        base = asdict(IntentConfig())
        base.update({k: v for k, v in d.items() if k in base})
        return IntentConfig(**base)


@dataclass
class IntentDecision:
    node_id: Optional[str]
    layer: str                        # L0 / L1 / L2 / fallback
    confidence: float
    reason: str = ""
    scores: Dict[str, float] = field(default_factory=dict)


def _bigrams(text: str) -> Set[str]:
    """中文友好的字符二元组。中英混排都能用，不需要分词器也不需要额外依赖。"""
    s = "".join(ch for ch in text.strip().lower() if not ch.isspace())
    if len(s) < 2:
        return {s} if s else set()
    return {s[i:i + 2] for i in range(len(s) - 1)}


def jaccard(a: Set[str], b: Set[str]) -> float:
    if not a or not b:
        return 0.0
    inter = len(a & b)
    if inter == 0:
        return 0.0
    return inter / float(len(a | b))


class IntentMatcher:
    def __init__(self, cfg: Optional[IntentConfig] = None):
        self.cfg = cfg or IntentConfig()

    # ------------------------------------------------------------------
    def l0(self, user_input: str, node: NodeSpec) -> float:
        """L0：强信号词的确定性判定。"""
        text = user_input.strip()
        if not text or not node.intent.keywords:
            return 0.0
        hits = sum(1 for k in node.intent.keywords if k in text)
        if hits == 0:
            return 0.0
        score = min(1.0, 0.5 + 0.25 * hits)
        if hits == len(node.intent.keywords):
            score = min(1.0, score + self.cfg.kw_full_bonus)
        return score

    # ------------------------------------------------------------------
    def l1(self, user_input: str, catalog: Catalog,
           cands: Sequence[Candidate]) -> Dict[str, float]:
        """L1：本地语义打分 -> {node_id: score}。"""
        q = _bigrams(user_input)
        out: Dict[str, float] = {}
        for c in cands:
            node = catalog.nodes[c.node_id]
            hint = node.intent
            corpus = " ".join([hint.description] + list(hint.examples))
            cb = _bigrams(corpus)
            sim = jaccard(q, cb)

            kw = hint.keywords
            kw_score = 0.0
            if kw:
                hits = sum(1 for k in kw if k in user_input)
                kw_score = min(1.0, hits / len(kw))
            neg = sum(1 for n in hint.negatives if n in user_input)

            s = (self.cfg.kw_hit_score * kw_score
                 + self.cfg.sim_weight * sim
                 + 0.15 * c.score                       # Frontier 优先级作为先验
                 - self.cfg.neg_penalty * neg)
            out[c.node_id] = round(s, 6)
        return out

    # ------------------------------------------------------------------
    def mmr(self, catalog: Catalog, scores: Dict[str, float], k: int) -> List[str]:
        """Maximal Marginal Relevance：在保留相关性的同时去掉语义重复的候选，
        避免 {DYNAMIC_ROUTES} 里塞三条长得一样的路径浪费 token。"""
        if not scores:
            return []
        ranked = sorted(scores, key=lambda n: (-scores[n], n))
        picked: List[str] = [ranked[0]]
        grams = {n: _bigrams(" ".join([catalog.nodes[n].intent.description]
                                      + catalog.nodes[n].intent.examples))
                 for n in ranked}
        lam = self.cfg.mmr_lambda
        while len(picked) < min(k, len(ranked)):
            best, best_v = None, -1e9
            for n in ranked:
                if n in picked:
                    continue
                redundancy = max(jaccard(grams[n], grams[p]) for p in picked) if picked else 0.0
                v = lam * scores[n] - (1 - lam) * redundancy
                if v > best_v or (abs(v - best_v) < 1e-9 and best and n < best):
                    best, best_v = n, v
            if best is None:
                break
            picked.append(best)
        return picked

    # ------------------------------------------------------------------
    def decide(self, user_input: str, catalog: Catalog, cands: Sequence[Candidate],
               default_next: Optional[str] = None,
               llm: Optional["LlmSelector"] = None) -> IntentDecision:
        """三级漏斗的总入口。"""
        if not cands:
            return IntentDecision(default_next, "fallback", 0.0, "frontier 为空，走 default_next")

        # ---- 先在第一梯队找 L0 独占命中 ----
        top_bucket = max(c.bucket for c in cands)
        tier = [c for c in cands if c.bucket == top_bucket]
        l0_scores = {c.node_id: self.l0(user_input, catalog.nodes[c.node_id]) for c in tier}
        best_node, best_v = max(l0_scores.items(), key=lambda kv: (kv[1], kv[0]))
        runners = [n for n, v in l0_scores.items() if n != best_node and v >= best_v]
        if best_v >= self.cfg.l0_confident and not runners:
            return IntentDecision(best_node, "L0", best_v,
                                  "关键词强命中且无歧义，跳过 LLM", {best_node: best_v})

        # ---- L1 本地语义打分 + MMR ----
        l1_scores = self.l1(user_input, catalog, cands)
        shortlist_ids = self.mmr(catalog, l1_scores, self.cfg.top_k)
        shortlist = [c for c in cands if c.node_id in shortlist_ids]
        shortlist.sort(key=lambda c: (-l1_scores.get(c.node_id, 0.0), c.node_id))
        ranked = sorted(l1_scores.items(), key=lambda kv: (-kv[1], kv[0]))
        margin = (ranked[0][1] - ranked[1][1]) if len(ranked) > 1 else 1.0

        # ---- 本地足够有把握 -> 不发请求（省钱、省延迟、消除不确定性） ----
        if llm is None or (ranked[0][1] >= 0.5 and margin >= self.cfg.l1_margin):
            return IntentDecision(ranked[0][0], "L1", ranked[0][1],
                                  "本地语义打分已足够判别", dict(ranked[:5]))

        # ---- L2 LLM 受限选择 ----
        try:
            picked = llm.select(user_input, shortlist)
        except Exception as e:                                     # 网络层异常必须吞掉
            return IntentDecision(ranked[0][0], "L1", ranked[0][1],
                                  f"LLM 异常降级: {e}", dict(ranked[:5]))
        if picked in l1_scores:
            return IntentDecision(picked, "L2", 1.0, "LLM 受限选择", dict(ranked[:5]))
        # 幻觉防线：LLM 返回了不在白名单里的节点 -> 降级，不信任，但记录留痕
        return IntentDecision(ranked[0][0], "L1", ranked[0][1],
                              f"LLM 返回了白名单外的节点 '{picked}'，已降级", dict(ranked[:5]))


class LlmSelector:
    """LLM 选择器接口（策略模式）。C++ 侧对应 `nx::ILlmSelector` 纯虚基类。"""

    def select(self, user_input: str, shortlist: Sequence[Candidate]) -> str:
        raise NotImplementedError


class StubLLM(LlmSelector):
    """离线仿真用：模拟一个"总是选语义最贴合项"的理想模型。

    用途：在没有网络/密钥时验证整条 pipeline；同时作为回归测试的稳定基准。
    """

    def __init__(self, matcher: Optional[IntentMatcher] = None):
        self.matcher = matcher or IntentMatcher()
        self.calls = 0
        self.last_routes: List[str] = []

    def select(self, user_input: str, shortlist: Sequence[Candidate]) -> str:
        self.calls += 1
        self.last_routes = [c.node_id for c in shortlist]
        ordered = sorted(shortlist, key=lambda c: (-c.score, c.node_id))
        return ordered[0].node_id


# =============================================================================
#  7.  Prompt 组装与 Token 预算 ( nx/PromptAssembler.hpp )
# =============================================================================
#
#  解决"长上下文 Lost-in-the-Middle"：不把全部历史塞进去，而是**分块 + 预算**。
#  预算不足时按优先级（低 -> 高）依次裁剪：
#       先砍摘要 -> 砍历史轮数 -> 砍候选条数
#  永远保留：[persona] [当前状态] [输出格式]，这三项被砍 = 人设崩塌。

#: 甲方硬约束：每轮实际发出的 prompt 不得超过 1200 token。
#: 写成模块级常量而不是散落的魔法数字，是为了让"上限"只有一处定义、可被断言。
#: 各块软预算（persona/world/summary/history/routes/output）之和允许超过它 ——
#: 那是"理想配比"，真正的兜底由 `PromptAssembler.build()` 末尾的硬上限循环负责。
K_MAX_PROMPT_TOKENS = 1200

#: 喂给 LLM 的候选池 / 白名单大小（必须与 C++ `kRoutePoolSize` / `kRouteShortlist` 一致）。
#: 先按 bucket+score 取前 12 个作为 MMR 的候选池，再去冗余到 5 条真正注入 prompt。
K_ROUTE_POOL_SIZE = 12
K_ROUTE_SHORTLIST = 5


@dataclass
class PromptBudget:
    # total_tokens 是**软**总预算，只能在 [1, K_MAX_PROMPT_TOKENS] 区间内收紧
    # 硬上限，不能放大。剧本里配成 2000 会被静默压回 1200。
    total_tokens: int = K_MAX_PROMPT_TOKENS
    persona: int = 260
    world: int = 140
    summary: int = 180
    history: int = 360
    routes: int = 320
    output: int = 140
    max_history_turns: int = 6

    @staticmethod
    def from_json(d: Optional[Dict[str, Any]]) -> "PromptBudget":
        if not d:
            return PromptBudget()
        base = asdict(PromptBudget())
        base.update({k: v for k, v in d.items() if k in base})
        b = PromptBudget(**base)
        # 甲方硬约束 1200：剧本里配的 total_tokens 只能**收紧**，不能放大。
        # 否则一个写着 2000 的老剧本会让"配置说够了、实际超发"。
        if not 0 < b.total_tokens <= K_MAX_PROMPT_TOKENS:
            b.total_tokens = K_MAX_PROMPT_TOKENS
        return b


def estimate_tokens(s: str) -> int:
    """粗估 token 数：CJK 按 1 字 ≈ 0.7 token，其余按 4 字符 ≈ 1 token。
    够用作预算裁剪，不需要精确 tokenizer（也不应该引入额外依赖）。
    """
    if not s:
        return 0
    cjk = sum(1 for ch in s if "\u4e00" <= ch <= "\u9fff")
    ascii_ = len(s) - cjk
    return int(cjk * 0.7 + ascii_ / 4.0) + 1


class PromptAssembler:
    def __init__(self, budget: Optional[PromptBudget] = None):
        self.budget = budget or PromptBudget()

    def build(self, persona: str, world_view: str, phase: str, npc_state: str,
              affinity: int, summary: str, history: Sequence[Dict[str, str]],
              routes: Sequence[Tuple[str, str]]) -> Dict[str, Any]:
        """返回 {'system':..., 'user':..., 'truncated':[...]}"""
        b = self.budget
        truncated: List[str] = []

        def clip(text: str, limit: int) -> str:
            nonlocal truncated
            if estimate_tokens(text) <= limit:
                return text
            # 按 token 预算做字符级截断（保守估计每 token ≈ 1.4 个中文字符）
            keep = int(limit * 1.4)
            truncated.append(f"裁剪: {text[:12]}...")
            return text[:keep] + "…"

        world_block = (
            f"【当前时间阶段】{phase}\n【NPC 状态】{npc_state}\n"
            f"【隐藏好感度】{affinity}\n【长期记忆摘要】{summary or '（暂无）'}")
        if estimate_tokens(world_block) > b.world + b.summary:
            world_block = (
                f"【当前时间阶段】{phase}\n【NPC 状态】{clip(npc_state, 60)}\n"
                f"【隐藏好感度】{affinity}\n【长期记忆摘要】{clip(summary, b.summary)}")
            truncated.append("world/summary")

        hist = list(history)[-b.max_history_turns:]
        hist_block = "\n".join(f"{h.get('role','')}: {h.get('content','')}" for h in hist)
        while hist and estimate_tokens(hist_block) > b.history:
            hist = hist[1:]
            hist_block = "\n".join(f"{h.get('role','')}: {h.get('content','')}" for h in hist)
            truncated.append("history")

        # ⚠ 裁剪只能从**尾部**丢：`routes` 是 Frontier 按 bucket+score 排好序的
        #   Top-K，头部是最该走的节点。从头部丢 = 把正确分支裁掉 = 选路失真。
        routes = list(routes)
        routes_block = self._render_routes(routes)
        while len(routes) > 1 and estimate_tokens(routes_block) > b.routes:
            routes = routes[:-1]
            routes_block = self._render_routes(routes)
            truncated.append("routes")

        # 硬上限：剧本里配的 total_tokens 只能**收紧**，绝不能放大到 1200 以上。
        cap = b.total_tokens if 0 < b.total_tokens < K_MAX_PROMPT_TOKENS \
            else K_MAX_PROMPT_TOKENS

        def assemble() -> str:
            s = clip(persona, b.persona) + "\n\n" + world_block
            if routes_block:
                s += "\n\n【可选路径（只能从这里选）】\n" + routes_block
            if hist_block:
                s += "\n\n【刚刚的对话】\n" + hist_block
            s += "\n\n" + self._render_output(routes)
            return s

        system = assemble()

        # ---- 硬上限兜底 ----
        # 放弃顺序严格按"信息价值从低到高"：候选条数 → 历史轮数 → 摘要。
        # guard 是收敛保护：某轮没有任何块变小（理论上不可能）就跳出，绝不死循环。
        guard = 0
        while estimate_tokens(system) > cap and guard < 4096:
            guard += 1
            prev = system
            if len(routes) > 1:                    # ① 先压候选条数（只丢尾部）
                routes = routes[:-1]
                routes_block = self._render_routes(routes)
                truncated.append("routes:cap")
            elif hist:                             # ② 再压历史轮数（丢最老的）
                hist = hist[1:]
                hist_block = "\n".join(
                    f"{h.get('role','')}: {h.get('content','')}" for h in hist)
                truncated.append("history:cap")
            elif summary:                          # ③ 再砍长期摘要
                summary = ""
                world_block = (
                    f"【当前时间阶段】{phase}\n【NPC 状态】{npc_state}\n"
                    f"【隐藏好感度】{affinity}\n【长期记忆摘要】（暂无）")
                truncated.append("summary:cap")
            else:                                  # ④ 最后才动人设（保命）
                persona = clip(persona, max(64, cap // 3))
                truncated.append("persona:cap")
            system = assemble()
            if system == prev:                     # 收敛保护
                break

        return {"system": system, "user": "", "truncated": truncated,
                "routes_injected": [r[0] for r in routes],
                "est_tokens": estimate_tokens(system)}

    @staticmethod
    def _render_routes(routes: Sequence[Tuple[str, str]]) -> str:
        if not routes:
            return "（无，按当前情境自然回应）"
        return "\n".join(f"- <{nid}> {desc}" for nid, desc in routes)

    @staticmethod
    def _render_output(routes: Sequence[Tuple[str, str]]) -> str:
        ids = "|".join(f"<{n}>" for n, _ in routes) or "<none>"
        return (
            "【输出格式】只能输出一个 JSON 对象，不要输出其它任何内容：\n"
            '{"affinity_change": int, "next_node": "节点ID", "reply": "台词"}\n'
            f"约束：next_node 必须是 {ids} 之一，或空字符串表示留在当前情境。")


# =============================================================================
#  8.  状态持久化：事件溯源 + 快照 ——【空缺 2 的答案】( EventStore )
# =============================================================================
#
#  为什么不能像现在那样只存 current_node？
#    因为 Frontier / pending / tags / rng_state 都是**引擎内部状态**，
#    一旦只持久化当前节点，读档后重算出的 Frontier 与存档时刻不一致，
#    表现为"读档后突然能去/不能去某个地方"，也就是"时空错乱"。
#
#  方案：Appendix-only Event Log + 周期快照
#    - 每一步写一条不可变事件 (seq 严格递增, UNIQUE(session_id, seq))
#    - 每 N 步写一个一致性快照 (ckpt_seq <= seq)
#    - 恢复 = load(latest snapshot) + replay(events where seq > ckpt_seq)
#    - 所有写入在一个事务内完成；乐观锁防止并发/回退写
#
#  Python 版用内存模拟 sqlite 行为（SQL 语义见 persistence/migrations/*.sql）

class ConcurrentModification(RuntimeError):
    """乐观锁失败：有人在别处推进了游标。"""


class EventStore:
    def __init__(self, snapshot_every: int = 10):
        self.snapshot_every = snapshot_every
        self.events: List[Dict[str, Any]] = []
        self.snapshots: List[Tuple[int, str, str]] = []   # (ckpt_seq, json, hash)
        self.meta: Dict[str, int] = {"time_cursor": 0, "version": 0}

    # ------------------------------------------------------------------
    def append(self, session_id: str, seq: int, event_type: str,
               payload: Dict[str, Any], expected_seq: int) -> None:
        """IRON-1 / IRON-2：seq 必须严格等于当前 time_cursor+1，且历史不可改写。"""
        if seq != expected_seq:
            raise ConcurrentModification(
                f"拒绝写入：期望 seq={expected_seq}，实际 seq={seq}（历史不可改写）")
        if any(e["seq"] == seq and e["session_id"] == session_id for e in self.events):
            raise ConcurrentModification(f"seq={seq} 的事件已存在，禁止覆盖")
        self.events.append({"session_id": session_id, "seq": seq,
                            "event_type": event_type, "payload": payload,
                            "ts": time.time()})
        self.meta["time_cursor"] = seq
        self.meta["version"] += 1

        if seq % self.snapshot_every == 0:
            self.put_snapshot(session_id, seq, payload.get("state_json", "{}"))

    def put_snapshot(self, session_id: str, ckpt_seq: int, state_json: str) -> None:
        if ckpt_seq > self.meta["time_cursor"]:
            raise ConcurrentModification("快照不得超前于 time_cursor")
        h = hashlib.sha256(state_json.encode("utf-8")).hexdigest()[:16]
        self.snapshots.append((ckpt_seq, state_json, h))
        self.snapshots.sort(key=lambda x: x[0])

    # ------------------------------------------------------------------
    def restore(self, session_id: str) -> Optional[Tuple[str, int]]:
        """返回 (state_json, ckpt_seq)。没有快照则返回 None -> 从头 replay。"""
        if not self.snapshots:
            return None
        ckpt_seq, state_json, _ = self.snapshots[-1]
        return state_json, ckpt_seq

    def events_after(self, session_id: str, ckpt_seq: int) -> List[Dict[str, Any]]:
        return sorted([e for e in self.events
                       if e["session_id"] == session_id and e["seq"] > ckpt_seq],
                      key=lambda e: e["seq"])


# =============================================================================
#  9.  引擎编排 ( nx/NarrativeEngine.hpp )
# =============================================================================

@dataclass
class Presentation:
    """本步应当呈现给玩家的文本（旁白 / 台词 / NPC 状态）。

    `is_redeem_variant=True` 表示用的是 `presentation.redeem_variant`，
    即"这个节点是被后向指针拉回来的，台词按现在的口径重讲了一遍"。
    """
    narrator: str = ""
    npc_text: str = ""
    npc_state: str = ""
    is_redeem_variant: bool = False


@dataclass
class TurnResult:
    reply: str
    next_node: str
    node: Optional[NodeSpec]
    affinity_change: float
    events: List[str] = field(default_factory=list)
    decision: Optional[IntentDecision] = None
    frontier: List[Candidate] = field(default_factory=list)
    blocked: Dict[str, List[str]] = field(default_factory=dict)
    redeemed: List[str] = field(default_factory=list)
    debug: Dict[str, Any] = field(default_factory=dict)
    #: 本步呈现的文本（含"是否用了挽回变体"标记）—— B-5 断言的数据来源
    presentation: Optional[Presentation] = None
    #: 本步 refresh 的 IN 列表（有序）—— A-1/A-3/B-6/B-7 的结构化断言来源
    frontier_in: List[str] = field(default_factory=list)
    #: 本轮 prompt 的**实测** token 数（`estimate_tokens()` 算），上限 K_MAX_PROMPT_TOKENS。
    #: 注意这是"如果要发 L2 会发多大"，不等于实际消耗 —— 见 `prompt_sent`。
    prompt_tokens: int = 0
    #: 本轮是否**真的**把 prompt 发给了 LLM。L0/L1 命中时不发请求，实际消耗 0 token；
    #: 只有 L2 才会真的发出去。甲方"每轮 ≤1200 token"约束要的是这个字段为 True 时的
    #: `prompt_tokens`，离线档（llm=None）全程为 False。
    prompt_sent: bool = False


class NarrativeEngine:
    """把所有零件串起来的编排层。C++ 侧同名 `nx::NarrativeEngine`。

    单次对话的顺序被严格固定（顺序本身就是设计的一部分）：

        step(t):
          0. 断言 seq 单调
          1. refresh Frontier（此刻还没有任何副作用）
          2. IntentMatcher 三级漏斗选路
          3. 应用选中节点的 effects（tags/items/affinity/phase）
          4. 把"本次没选中的可挽回兄弟节点"压入 PendingPool
          5. seq += 1                        <- 时间游标唯一推进点
          6. PendingPool.scan() 做后向指针唤醒
          7. 同一事务内落库（事件 + 可能的快照）
    """

    def __init__(self, catalog: Catalog, state: Optional[GameState] = None,
                 store: Optional[EventStore] = None,
                 matcher: Optional[IntentMatcher] = None,
                 scorer: Optional[FrontierScorer] = None,
                 prompter: Optional[PromptAssembler] = None,
                 llm: Optional[LlmSelector] = None):
        self.catalog = catalog
        self.state = state or GameState()
        self.store = store or EventStore()
        self.matcher = matcher or IntentMatcher()
        self.scorer = scorer or FrontierScorer()
        self.prompter = prompter or PromptAssembler()
        self.llm = llm
        self.rng = Rng(self.state.rng_state)
        self.pending_pool = PendingPool(catalog)
        self.history_layers: List[str] = []

    # ------------------------------------------------------------------
    @staticmethod
    def from_json(doc: Dict[str, Any], **kw) -> "NarrativeEngine":
        catalog = Catalog.from_json(doc)
        cfg = FrontierConfig.from_json(doc.get("frontier"))
        icfg = IntentConfig.from_json(doc.get("intent"))
        budget = PromptBudget.from_json(doc.get("budget"))
        kw.setdefault("scorer", FrontierScorer(cfg))
        kw.setdefault("matcher", IntentMatcher(icfg))
        kw.setdefault("prompter", PromptAssembler(budget))
        return NarrativeEngine(catalog, **kw)

    # ------------------------------------------------------------------
    def _default_reply(self, node: NodeSpec) -> str:
        return node.presentation.get("npc_text") or node.presentation.get("narrator") or "……"

    # ------------------------------------------------------------------
    def present(self, node_id: str, prev_last_visit: Optional[int] = None) -> Presentation:
        """决定本步呈现哪一套文本：首次访问的 `presentation`，还是挽回变体。

        判定规则（比 ARCH §4.2-(2) 原文的 `awake_seq == st.seq` 更通用）：

            存在 pending 条目 e 使 e.node_id == node_id 且 e.awake，并且
                e.awake_seq == st.seq                       （本步刚被唤醒）
            或  e.awake_seq > prev_last_visit               （唤醒发生在上一次访问之后）

        ARCH 原文只覆盖第一种情形；但实际流程里"唤醒"发生在第 N 步的 step 6，
        玩家真正走进这个节点是第 N+1 步，此刻 `awake_seq != st.seq`。
        只按原文实现的话，用例 B 的 `n3_tender` 会错误地回放首次台词，
        B-5（不回放旧文本）直接失败。故补上第二种情形。

        `prev_last_visit` 必须是**本次进入之前**的 last_visit 值
        （step() 里在推进游标前抓取），否则刚写入的 seq 会盖掉判定。
        """
        node = self.catalog.nodes.get(node_id)
        if node is None:
            return Presentation()
        base = node.presentation or {}
        rv = node.redeem_variant
        use_variant = False
        if rv.has:
            prev = prev_last_visit if prev_last_visit is not None \
                else self.state.last_visit.get(node_id, -1)
            for e in self.state.pending:
                if e.node_id != node_id or not e.awake:
                    continue
                if e.awake_seq == self.state.seq or prev < 0 or e.awake_seq > prev:
                    use_variant = True
                    break
        if use_variant:
            return Presentation(
                narrator=rv.narrator or base.get("narrator", ""),
                npc_text=rv.npc_text or base.get("npc_text", ""),
                npc_state=rv.npc_state or base.get("npc_state", ""),
                is_redeem_variant=True,
            )
        return Presentation(
            narrator=base.get("narrator", ""),
            npc_text=base.get("npc_text", ""),
            npc_state=base.get("npc_state", ""),
            is_redeem_variant=False,
        )

    # ------------------------------------------------------------------
    def frontier_in_ids(self) -> List[str]:
        """结构化 API：当前 time_cursor 下 Frontier 的 IN 列表（已排序）。

        为什么不能靠 grep `debug_frontier()` 的字符串：节点名互为前缀时
        （如 `n5_search` 与 `n5_search_x`）子串匹配会误判，且 C++ 与 Python
        两份日志格式一旦漂移就无从比对。给结构化 API，两侧断言同一份数据。
        """
        frontier, _ = self.scorer.refresh(self.catalog, self.state)
        return [c.node_id for c in frontier]

    def blocked_map(self) -> Dict[str, List[str]]:
        """结构化 API：{拦截原因: [节点 id...]}（每种原因内部按字典序排好）。"""
        _, blocked = self.scorer.refresh(self.catalog, self.state)
        return {why: sorted(ids) for why, ids in blocked.items()}

    def step(self, user_input: str) -> TurnResult:
        st = self.state
        events: List[str] = []

        # ---- 0. 单调性断言 ----
        if st.seq < 0:
            raise RuntimeError("time_cursor 异常：负值")

        # ---- 1. 先算出"此刻还没被任何副作用污染"的 Frontier ----
        frontier, blocked = self.scorer.refresh(self.catalog, st, self.rng)

        # ---- 2. 三级漏斗选路 ----
        current = self.catalog.nodes.get(st.current_node)
        default_next = current.default_next if current else None
        # 先看是不是点了选项按钮。**必须排在漏斗之前**：
        # 点按钮是最强的确定性意图，为它调用 LLM 既浪费钱与延迟，
        # 又平白引入一次不必要的模型不确定性。
        opt_pick = self._match_option(user_input, current)
        if opt_pick is not None:
            decision = IntentDecision(opt_pick, "L0", 1.0, "玩家点了选项按钮")
        else:
            decision = self.matcher.decide(user_input, self.catalog, frontier,
                                           default_next, self.llm)

        next_node = decision.node_id or default_next or st.current_node
        if next_node not in self.catalog.nodes:
            next_node, decision.layer = st.current_node, "fallback(guard)"
            events.append(f"拦截：目标节点不存在，留在原地")

        node = self.catalog.nodes[next_node]

        # ---- 2b. 本轮 prompt 的实测 token 数 ----
        # 必须放在副作用（第 3 步）**之前**采样：L2 若真的要发请求，用的就是
        # 此刻这份尚未被 effects 污染的 state。放在之后会算出"下一步"的 prompt。
        # `prompt_sent` 区分"会发多大"与"真的发了"：L0/L1 命中时不发请求，实耗 0。
        prompt_tokens = self.build_prompt(
            user_input,
            npc_state=(current.presentation.get("npc_state", "") if current else ""),
        )["est_tokens"]

        # 抓取"本次进入之前"的 last_visit，供 present() 判断是否该用挽回变体。
        # 必须早于第 5 步的 visit 更新，否则刚写入的 seq 会让判定失效。
        prev_last_visit = st.last_visit.get(next_node, -1)

        # ---- 3. 应用确定性副作用 ----
        fx = node.effects
        before_affinity = st.affinity
        for t in fx.add_tags:
            if t not in st.tags:
                st.tags.add(t)
                events.append(f"+tag {t}")
        for t in fx.remove_tags:
            if t in st.tags:
                st.tags.discard(t)
                events.append(f"-tag {t}")
        for i in fx.add_items:
            if i not in st.items:
                st.items.add(i)
                events.append(f"+item {i}")
        for i in fx.remove_items:
            if i in st.items:
                st.items.discard(i)
                events.append(f"-item {i}")
        st.affinity = int(max(-100, min(200, st.affinity + fx.affinity_delta)))
        if fx.set_phase and fx.set_phase in self.catalog.phases:
            idx = self.catalog.phases.index(fx.set_phase)
            if idx > st.phase_index:                     # 阶段只能前进
                st.phase, st.phase_index = fx.set_phase, idx
                events.append(f"phase -> {st.phase}")

        # ---- 4. 把岔路口没走的路压入挂起池 ----
        if current is not None:
            alts = []
            for opt in current.options:
                nxt = opt.get("next")
                if nxt and nxt != next_node and nxt in self.catalog.nodes:
                    alts.append(nxt)
            seen: Set[str] = set()
            for alt in alts:
                if alt in seen:
                    continue
                seen.add(alt)
                if self.pending_pool.push(st, alt, "unselected", current.id):
                    events.append(f"pending <- {alt}")

        # ---- 5. 时间游标推进（唯一入口）----
        st.seq += 1
        st.rng_state = self.rng.snapshot()
        st.visit_count[next_node] = st.visit_count.get(next_node, 0) + 1
        st.last_visit[next_node] = st.seq
        st.visited.setdefault(next_node, st.seq)
        st.current_node = next_node
        st.history_tail.append({"role": "你", "content": user_input})
        st.history_tail.append({"role": "她", "content": self._default_reply(node)})
        if len(st.history_tail) > 40:
            st.history_tail = st.history_tail[-40:]

        self.history_layers.append(decision.layer)

        # ---- 6. 后向指针：事件挽回扫描 ----
        awakened = self.pending_pool.scan(st)
        redeemed = []
        for p in awakened:
            redeemed.append(p.node_id)
            events.append(f"redeem {p.node_id} (born@{p.birth_seq} -> awake@{p.awake_seq})")

        # ---- 7. 原子落库 ----
        self.store.append(st.session_id, st.seq, "TURN", {
            "input": user_input, "from": getattr(current, "id", None),
            "to": next_node, "layer": decision.layer,
            "events": events, "state_json": st.to_json(),
            "state_hash": st.hash(),
        }, expected_seq=st.seq)

        return TurnResult(
            reply=self._default_reply(node),
            next_node=next_node, node=node,
            affinity_change=round(st.affinity - before_affinity, 3),
            events=events, decision=decision, frontier=frontier,
            blocked=blocked, redeemed=redeemed,
            presentation=self.present(next_node, prev_last_visit),
            frontier_in=[c.node_id for c in frontier],
            prompt_tokens=prompt_tokens,
            prompt_sent=(decision.layer == "L2"),
            debug={"seq": st.seq, "phase": st.phase,
                   "pending_stats": PendingPool.stats(st),
                   "prompt_tokens": prompt_tokens,
                   "prompt_sent": (decision.layer == "L2")},
        )

    @staticmethod
    def _match_option(user_input: str, current: Optional[NodeSpec]) -> Optional[str]:
        if not current or not current.options:
            return None
        s = user_input.strip()
        if not s.isdigit():
            return None
        i = int(s)
        if 1 <= i <= len(current.options):
            return current.options[i - 1].get("next")
        return None

    # ------------------------------------------------------------------
    def build_prompt(self, user_input: str, npc_state: str = "",
                     history_len: int = 6) -> Dict[str, Any]:
        # ⚠ 必须与 C++ `NarrativeEngine::build_prompt` 逐字对齐（同样的原因：
        #   两侧白名单不同 -> prompt 长度不同 -> 跨语言轨迹 diff 失败）。
        #   口径：取 Frontier 前 K_ROUTE_POOL_SIZE 个的 c.score -> MMR 去冗余到 5。
        frontier, _ = self.scorer.refresh(self.catalog, self.state, self.rng)
        shortlist = self.matcher.mmr(
            self.catalog,
            {c.node_id: c.score for c in frontier[:K_ROUTE_POOL_SIZE]},
            K_ROUTE_SHORTLIST)
        routes = [(nid, self.catalog.nodes[nid].intent.description) for nid in shortlist]
        return self.prompter.build(
            persona="你扮演木子因，一个对哥哥有极端依赖的高中生。",
            world_view="现代都市，公寓。",
            phase=self.state.phase, npc_state=npc_state or "平静",
            affinity=self.state.affinity, summary=self.state.summary,
            history=self.state.history_tail[-history_len:], routes=routes)

    # ------------------------------------------------------------------
    def debug_frontier(self) -> str:
        frontier, blocked = self.scorer.refresh(self.catalog, self.state)
        lines = [f"=== Frontier @ seq={self.state.seq} phase={self.state.phase} "
                 f"node={self.state.current_node} ==="]
        for c in frontier[:10]:
            lines.append("  IN   " + c.explain())
        for why, ids in blocked.items():
            lines.append(f"  BLOCK[{why}] {sorted(ids)[:8]}{'...' if len(ids) > 8 else ''}")
        return "\n".join(lines)
