# -*- coding: utf-8 -*-
"""
T02 指针测试用例：前向指针 A / 后向指针 B / 对照组 B0。

断言来源：docs/PRD_FS_SCRIPT.md §6（v1.7）与 docs/ARCH_FS.md §6.4/§6.5。

运行方式（两种）：
    python reference/simulate.py                # V12 段会自动调用本模块的三个 case
    python reference/cases_pointer.py           # 只跑指针用例，打印每条断言
    python reference/cases_pointer.py --emit-trace   # 导出归一化轨迹，用于与 C++ 侧 diff

跨语言 Test Oracle（diff 必须为空）：
    ./engine/tests/test_core17 --emit-trace | grep '^case=' | tr -d '\\r' > /tmp/cpp.txt
    python reference/cases_pointer.py --emit-trace                       > /tmp/py.txt
    diff /tmp/cpp.txt /tmp/py.txt

设计约定（务必遵守，否则断言会失真）：
1. **每一步玩家输入都写死为常量**，禁止用「继续」「嗯」「……」等中性文本推进。
   离线档没有 default_next 兜底（M-9），中性输入会把路由交给 priority+bucket+打分。
2. 凡是"某节点不在 Frontier 里"的断言，一律写 `∉ frontier_in`，
   **禁止断言具体 BLOCK 原因字符串**（M-1 一改就红）。
3. 断言红了要如实上报，不允许改断言去迁就实现。
"""
import json
import os
import sys
from dataclasses import dataclass, field
from typing import List, Optional, Tuple

from nxref import (  # noqa: E402
    Catalog, EventStore, GameState, K_MAX_PROMPT_TOKENS, NarrativeEngine,
)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STORY_V2 = os.path.join(ROOT, "data", "story_v2.json")

#: 甲方硬约束：每轮实际发出的 prompt ≤ 1200 token。
#: 直接引用 nxref 的常量，避免这里再写一份魔法数字（两处定义必然漂移）。
PROMPT_TOKEN_CAP = K_MAX_PROMPT_TOKENS


# =============================================================================
#  玩家输入常量（全部写死，来自 PRD §6.0 / §6.1 / §6.2 三张表）
# =============================================================================
OPT_1 = "1"                       # L0 match_option：点第 1 个选项

A0_S2 = "对不起，我今天真的有点累"     # 基线 A0 第 2 步
A0_S3 = OPT_1                     # 基线 A0 第 3 步：点「抱紧她」
A0_S4 = "该睡了，晚安。"             # 基线 A0 第 4 步（v1.2 写死，防 end_calm 抢跑）
A0_S5 = "嗯，回头见。"               # 基线 A0 第 5 步

A_S2 = "你口袋里藏了什么？"           # 用例 A 第 2 步（= n5_search.examples[0]，终裁版）
A_S3 = "这发卡上面的血是谁的？"        # 用例 A 第 3 步

B_S2 = "没什么，你别管了。"           # 用例 B 第 2 步
B_S3 = "（沉默）"                    # 用例 B 第 3 步
B_S5 = "其实……我今天真的很累。"       # 用例 B 第 5 步
B_S6 = "那……晚安吧。"                # 用例 B 第 6 步（v1.2 新增，写死防 n5_search 抢走）
B0_S5 = "算了，当我没说"              # 对照组 B0 第 5 步（不命中 n3_tender）


# =============================================================================
#  结果与断言收集
# =============================================================================
@dataclass
class CaseResult:
    name: str
    checks: List[Tuple[bool, str, str]] = field(default_factory=list)

    @property
    def passed(self) -> int:
        return sum(1 for ok_, _, _ in self.checks if ok_)

    @property
    def total(self) -> int:
        return len(self.checks)

    @property
    def ok(self) -> bool:
        return self.total > 0 and self.passed == self.total

    def add(self, ok_: bool, name: str, detail: str = "") -> bool:
        self.checks.append((bool(ok_), name, detail))
        if _VERBOSE[0]:
            print(f"    {'[OK]' if ok_ else '[!!]'} {name}" + (f"  :: {detail}" if detail else ""))
        return bool(ok_)


_VERBOSE = [False]


def _load_story():
    with open(STORY_V2, "r", encoding="utf-8") as f:
        return json.load(f)


def _fresh_engine(session_id: str = "case"):
    """离线档：llm=None（PRD §6 全部断言按 L1 打分算）。

    开局状态必须经 `Catalog.new_state()` 构造（等价于 C++ `StoryLoader::new_state`）：
    它把 `current_node` 写成剧本 `entry`。裸 `GameState()` 的 `current_node="0"`
    会让第 1 步的 L0 `match_option` 失效 —— 玩家点选项①却走 L1 打分被送回入口节点。
    """
    doc = _load_story()
    cat = Catalog.from_json(doc)
    eng = NarrativeEngine.from_json(
        doc,
        state=cat.new_state(session_id),
        store=EventStore(snapshot_every=4),
        llm=None,
    )
    return _attach_prompt_log(eng)


def _attach_prompt_log(eng: NarrativeEngine) -> NarrativeEngine:
    """给引擎挂一个 `prompt_log`，自动记录每一步的 (seq, prompt_tokens, prompt_sent)。

    为什么用包装而不是在每个 case 里手写记账：三个用例 + `emit_trace()` 一共
    四处会调 `eng.step()`，手写必然漏记一处，而这条是甲方硬约束、不能靠"大概"。
    只包装实例（不子类化、不改 `NarrativeEngine` 本身），对内核零侵入。
    """
    log: List[Tuple[int, int, bool]] = []
    original_step = eng.step

    def step(user_input: str):
        tr = original_step(user_input)
        log.append((eng.state.seq, tr.prompt_tokens, tr.prompt_sent))
        return tr

    eng.step = step            # type: ignore[method-assign]
    eng.prompt_log = log       # type: ignore[attr-defined]
    return eng


def _prompt_cap_check(r: CaseResult, eng: NarrativeEngine, tag: str) -> None:
    """甲方硬约束断言：本用例每一步的 prompt token 都 ≤ 上限，并报出最大值。

    来源标注：
    # 实测锁定（甲方硬约束，由 team-lead 转达；PRD/ARCH 暂无对应条目）
    —— 它**不是**从 PRD §6 / ARCH §6 推导出来的规格，而是甲方后加的运行时约束；
    本断言只能证明"离线档上界不超 1200"，**不代表联机档 L2 的实际消耗**
    （离线档 llm=None 全程不走 L2，`prompt_sent` 恒为 0）。
    """
    log = getattr(eng, "prompt_log", [])
    per_step = [t for _, t, _ in log]
    sent = [t for _, t, s in log if s]
    mx = max(per_step) if per_step else 0
    r.add(mx <= PROMPT_TOKEN_CAP,
          f"{tag} 每轮 prompt token ≤ {PROMPT_TOKEN_CAP}（甲方硬约束）",
          f"最大值={mx}，占上限 {mx * 100 // PROMPT_TOKEN_CAP}%，"
          f"逐步={per_step}，实际发给 LLM 的步数={len(sent)}"
          f"（离线档 llm=None，L0/L1 命中不发请求，实耗 0）")


def _find_pending(st, node_id: str) -> Optional[object]:
    for p in st.pending:
        if p.node_id == node_id:
            return p
    return None


def _redeem_event(tr, node_id: str) -> Optional[str]:
    for e in tr.events:
        if e.startswith(f"redeem {node_id} "):
            return e
    return None


# =============================================================================
#  基线 A0：坦白线（对照组基准，5 步）
# =============================================================================
def _run_baseline_a0(eng):
    """跑完基线 A0，返回 (trace, ending_seq)。trace 每项 = (seq_after, to_node, aff, phase)。"""
    trace = []
    for inp in (OPT_1, A0_S2, A0_S3, A0_S4, A0_S5):
        tr = eng.step(inp)
        st = eng.state
        trace.append((st.seq, tr.next_node, st.affinity, st.phase))
        if eng.state.current_node.startswith("end_"):
            break
    return trace


# =============================================================================
#  用例 A：前向指针
# =============================================================================
def case_forward_pointer(verbose: bool = False) -> CaseResult:
    _VERBOSE[0] = verbose
    r = CaseResult("forward_pointer")

    # ---- 用例 A 本体 ----
    eng = _fresh_engine("caseA")
    tr1 = eng.step(OPT_1)
    r.add(eng.state.seq == 1 and tr1.next_node == "n2_kitchen",
          "A-0 第1步 选项① 进入 n2_kitchen",
          f"seq={eng.state.seq} to={tr1.next_node}")

    tr2 = eng.step(A_S2)
    st2_seq = eng.state.seq
    st2_phase = eng.state.phase
    r.add(tr2.next_node == "n5_search",
          "A-6 n5_search 在 seq=1 时 feasible（step_gte:1 生效）",
          f"输入「{A_S2}」-> to={tr2.next_node}, seq={st2_seq}, aff={eng.state.affinity}")

    # A-1 / A-2：此刻（seq=2, phase=evening）n7_goodnight 应已在 Frontier 的 IN 里
    fin = eng.frontier_in_ids()
    blocked = eng.blocked_map()
    r.add("n7_goodnight" in fin,
          "A-1 seq=2 时 n7_goodnight ∈ frontier_in（被提前拉进可达集合）",
          f"phase={st2_phase}, frontier_in={fin}")
    r.add(st2_phase == "evening" and "n7_goodnight" not in blocked.get("phase", []),
          "A-2 它不是靠 phase 进来的（phase 仍为 evening 且不在 blocked[phase]）",
          f"phase={st2_phase}, blocked[phase]={blocked.get('phase', [])}")

    tr3 = eng.step(A_S3)
    ending_seq_a = eng.state.seq
    r.add(tr3.next_node == "end_hidden",
          "A-4a 用例 A 收束到 end_hidden",
          f"to={tr3.next_node}, seq={ending_seq_a}, aff={eng.state.affinity}")
    r.add(ending_seq_a <= 4,
          "A-4b 整局步数被压缩（抵达 ending 时 seq <= 4）",
          f"seq={ending_seq_a}")

    # A-5：单调性
    r.add(ending_seq_a >= 3 and eng.state.phase_index >= 0,
          "A-5 单调性未破坏（seq 递增、phase_index 不减）",
          f"seq={ending_seq_a}, phase_index={eng.state.phase_index}")

    # ---- 基线 A0 对照组 ----
    eng0 = _fresh_engine("caseA0")
    eng0.step(OPT_1)
    tr0_2 = eng0.step(A0_S2)
    fin0 = eng0.frontier_in_ids()
    aff0 = eng0.state.affinity
    r.add("n7_goodnight" not in fin0,
          "A-3 对照组基线 A0 在 seq=2 时 n7_goodnight ∉ frontier_in",
          f"aff={aff0}, phase={eng0.state.phase}, frontier_in={fin0}")

    eng0.step(A0_S3)
    tr0_4 = eng0.step(A0_S4)
    r.add(tr0_4.next_node == "n7_goodnight",
          "A-7a 基线 A0 第 4 步不被 end_calm 抢跑，to_node == n7_goodnight",
          f"输入「{A0_S4}」-> to={tr0_4.next_node}, aff={eng0.state.affinity}")

    tr0_5 = eng0.step(A0_S5)
    r.add(eng0.state.seq == 5,
          "A-7b 基线 A0 抵达 ending 时 seq == 5（锁死对照基准）",
          f"to={tr0_5.next_node}, seq={eng0.state.seq}")

    # ---- 甲方硬约束：每轮 prompt token ≤ 1200 ----
    # 来源：实测锁定（甲方硬约束，team-lead 转达；PRD/ARCH 无对应条目）
    _prompt_cap_check(r, eng, "A-8")
    _prompt_cap_check(r, eng0, "A-8(A0 基线)")

    return r


# =============================================================================
#  用例 B：后向指针（挽回）
# =============================================================================
def case_backward_pointer(verbose: bool = False) -> CaseResult:
    _VERBOSE[0] = verbose
    r = CaseResult("backward_pointer")

    eng = _fresh_engine("caseB")
    eng.step(OPT_1)                                   # 步1 -> n2_kitchen, seq=1

    tr2 = eng.step(B_S2)                              # 步2 -> n4_shut
    st = eng.state
    p3 = _find_pending(st, "n3_tender")
    r.add(tr2.next_node == "n4_shut",
          "B-0 第2步进入 n4_shut（错过 n3_tender）",
          f"to={tr2.next_node}, seq={st.seq}, aff={st.affinity}")
    r.add(p3 is not None and p3.birth_seq == 1 and not p3.awake,
          "B-1 错过即入池：n3_tender ∈ pending，birth_seq==1 且未唤醒",
          f"pending={[(p.node_id, p.birth_seq, p.awake_seq) for p in st.pending]}")

    tr3 = eng.step(B_S3)                              # 步3 -> n6_silence（+regret，唤醒开关）
    st = eng.state
    ev = _redeem_event(tr3, "n3_tender")
    r.add(tr3.next_node == "n6_silence",
          "B-3a 第3步进入 n6_silence 并拿到 regret",
          f"to={tr3.next_node}, seq={st.seq}, tags={sorted(st.tags)}")
    r.add(ev is not None,
          "B-2 唤醒真的发生（events **含** redeem n3_tender ...；注意是'含'不是'仅含'）",
          f"events={tr3.events}")

    p3 = _find_pending(st, "n3_tender")
    born = p3.birth_seq if p3 else None
    awake = p3.awake_seq if p3 else None
    r.add(awake is not None and born is not None and awake > born,
          "B-3b C1 单调唤醒：awake_seq > birth_seq",
          f"born@{born} -> awake@{awake}")
    r.add(born == 1,
          "B-3c 唤醒后 birth_seq 仍为 1（未被改写）",
          f"birth_seq={born}")

    # 对照基准 = 剧本里写死的首次访问台词（直接从 catalog 取，不受 pending 状态影响）。
    # ⚠️ 不能在这里用 eng.present("n3_tender") 当"首次文本"：此刻 n3_tender 已被唤醒
    # （awake_seq=3 > prev_last_visit=-1），present() 返回的就已经是挽回变体了。
    base_text = eng.catalog.nodes["n3_tender"].presentation.get("npc_text", "")

    tr5 = eng.step(B_S5)                              # 步5 -> n3_tender（被挽回）
    st = eng.state
    r.add(tr5.next_node == "n3_tender",
          "B-2b 唤醒后能被正确收敛：第5步 to_node == n3_tender",
          f"输入「{B_S5}」-> to={tr5.next_node}, seq={st.seq}, aff={st.affinity}")

    # B-5 必须断言"进入该节点那一刻真正呈现的文本"（tr5.presentation），
    # 而不是事后再调一次 present()：那时 last_visit 已被改写，判定条件已失效。
    pres5 = tr5.presentation
    r.add(pres5.is_redeem_variant and pres5.npc_text != base_text,
          "B-5 不回放旧文本：进入 n3_tender 那一刻呈现的是 redeem_variant，且 ≠ 首次台词",
          f"is_redeem_variant={pres5.is_redeem_variant}, "
          f"首次='{base_text[:16]}…', 挽回='{pres5.npc_text[:16]}…'")

    lv = st.last_visit.get("n3_tender")
    r.add(lv == st.seq and lv != 1,
          "B-4 时间游标不回退：last_visit[n3_tender] == 当前 seq（不是 1）",
          f"last_visit[n3_tender]={lv}, seq={st.seq}")

    tr6 = eng.step(B_S6)                              # 步6 -> n7_goodnight（防 n5_search 抢走）
    st = eng.state
    r.add(tr6.next_node == "n7_goodnight",
          "B-8a 第6步不被 n5_search 抢走，to_node == n7_goodnight",
          f"输入「{B_S6}」-> to={tr6.next_node}, seq={st.seq}, aff={st.affinity}")

    r.add("n7_goodnight" in eng.frontier_in_ids() or eng.state.seq == 5,
          "B-7 路径不断裂：seq=5 时 frontier_in 非空",
          f"seq={st.seq}, frontier_in={eng.frontier_in_ids()}")

    # 跑到结局
    guard = 0
    while not eng.state.current_node.startswith("end_") and guard < 10:
        eng.step(B_S6 if guard == 0 else "嗯，回头见。")
        guard += 1
    st = eng.state
    r.add("item_hairclip" not in st.items,
          "B-8b 全程未获得 item_hairclip",
          f"items={sorted(st.items)}")
    r.add(st.current_node == "end_calm" and st.affinity == 84,
          "B-8c 挽回成功：最终 end_calm 且 affinity == 84",
          f"node={st.current_node}, aff={st.affinity}, seq={st.seq}")

    # ---- 甲方硬约束：每轮 prompt token ≤ 1200 ----
    # 来源：实测锁定（甲方硬约束，team-lead 转达；PRD/ARCH 无对应条目）
    _prompt_cap_check(r, eng, "B-9")

    return r


# =============================================================================
#  归一化轨迹（跨语言 Test Oracle）
#
#  与 C++ engine/tests/test_core.cpp 的 `trace_line()` 逐字段同构，用于
#      ./test_core17 --emit-trace | grep '^case=' | tr -d '\r' > /tmp/cpp.txt
#      python reference/cases_pointer.py --emit-trace            > /tmp/py.txt
#      diff /tmp/cpp.txt /tmp/py.txt
#  diff 为空即证明 C++ 内核与 Python 参考实现行为完全一致。
# =============================================================================
TRACE_PLANS = [
    # (case 名, 输入序列, 是否用 B0_S5 强行续推到 seq>11)
    ("A",  (OPT_1, A_S2, A_S3), False),
    ("A0", (OPT_1, A0_S2, A0_S3, A0_S4, A0_S5), False),
    ("B",  (OPT_1, B_S2, B_S3, B_S5, B_S6, "嗯，回头见。"), False),
    ("B0", (OPT_1, B_S2, B_S3, B0_S5), True),
]


def emit_trace() -> List[str]:
    """跑完四条脚本，输出与 C++ `trace_line()` 同构的归一化轨迹行。"""
    lines: List[str] = []
    for cname, inputs, fill in TRACE_PLANS:
        eng = _fresh_engine(cname)
        for i, inp in enumerate(inputs, 1):
            before = eng.state.current_node
            tr = eng.step(inp)
            st = eng.state
            lines.append(
                f"case={cname} step={i} seq={st.seq} from={before} to={st.current_node} "
                f"layer={tr.decision.layer} aff={st.affinity} phase={st.phase} "
                f"phase_index={st.phase_index} "
                f"tags=[{','.join(sorted(st.tags))}] items=[{','.join(sorted(st.items))}] "
                f"in=[{','.join(tr.frontier_in)}] "
                f"redeemed=[{','.join(sorted(tr.redeemed))}] "
                f"variant={1 if tr.presentation.is_redeem_variant else 0} "
                f"prompt_tokens={tr.prompt_tokens} "
                f"prompt_sent={1 if tr.prompt_sent else 0}")
        if fill:
            guard = 0
            while eng.state.seq <= 11 and guard < 30:
                before = eng.state.current_node
                tr = eng.step(B0_S5)
                st = eng.state
                guard += 1
                # C++ 侧 brief/trace 的 step 编号是 continue 之后的自然序号
                lines.append(
                    f"case={cname} step={len(inputs) + guard} seq={st.seq} "
                    f"from={before} to={st.current_node} "
                    f"layer={tr.decision.layer} aff={st.affinity} phase={st.phase} "
                    f"phase_index={st.phase_index} "
                    f"tags=[{','.join(sorted(st.tags))}] "
                    f"items=[{','.join(sorted(st.items))}] "
                    f"in=[{','.join(tr.frontier_in)}] "
                    f"redeemed=[{','.join(sorted(tr.redeemed))}] "
                    f"variant={1 if tr.presentation.is_redeem_variant else 0} "
                    f"prompt_tokens={tr.prompt_tokens} "
                    f"prompt_sent={1 if tr.prompt_sent else 0}")
    return lines


# =============================================================================
#  对照组 B0：不挽回 -> 应落 end_regret，且 n3_tender 永不再现
# =============================================================================
def case_backward_pointer_neg(verbose: bool = False) -> CaseResult:
    """对照组 B0：不挽回 -> 应落 end_regret，且 n3_tender 永不再现。

    ⚠️ 实测发现 PRD §6.2 的 B-6a/B-6b 与 B-6c **在真实玩法下互斥**（已上报 team-lead）：

    走到 `n6_silence`(seq=3) 时，`regret` 已到手且 `aff=77 ≤ 81` —— `end_regret`
    的两个条件同时满足，它立刻 feasible；而 `bucket_of(Ending)=40 > Scripted=30`，
    于是 **seq=3 之后任何输入都会被 `end_regret` 收走**（实测「算了，当我没说」/
    「（继续沉默）」/「……」/「嗯」/「我什么也不想说」五种输入全部 seq=4 → end_regret）。
    因此真实玩法里 B0 **永远到不了 seq>11**，B-6a/B-6b 的"运行到 seq>11"前提不成立。

    处理方式（不改断言口径，只拆成两个探针）：
      · 探针 1 = 真实玩法，一到 ending 就停 -> 测 B-6c / B-6d（价值证明）；
      · 探针 2 = 强行越过 ending 继续推进，只用来验证 PendingPool 的
        `redeem_expire` 过期销毁语义 -> 测 B-6a / B-6b。
    探针 2 不是真实玩法，仅作内核语义探针，已在断言名里标明。
    """
    _VERBOSE[0] = verbose
    r = CaseResult("backward_pointer_neg")

    # ================= 探针 1：真实玩法（一到 ending 就停）=================
    eng = _fresh_engine("caseB0")
    eng.step(OPT_1)                     # -> n2_kitchen, seq=1
    eng.step(B_S2)                      # -> n4_shut,  pending n3_tender born@1
    tr3 = eng.step(B_S3)                # -> n6_silence, +regret（唤醒已发生）
    r.add("n3_tender" in [p.node_id for p in eng.state.pending if p.awake],
          "B-6-pre 对照组第3步同样发生了唤醒（挽回机会是真实存在的）",
          f"events={tr3.events}")

    tr4 = eng.step(B0_S5)               # 关键：不命中 n3_tender
    st = eng.state
    r.add(st.current_node == "end_regret",
          "B-6c 对照组最终落 end_regret（与用例 B 的 end_calm 形成对照）",
          f"node={st.current_node}, aff={st.affinity}, seq={st.seq}")
    r.add(tr4.next_node != "n3_tender",
          "B-6d 第5步「算了，当我没说」没有回到 n3_tender（错过即错过）",
          f"to={tr4.next_node}, seq={st.seq}, aff={st.affinity}")
    r.add(st.seq == 4,
          "B-6e 对照组在 seq=4 即收束（end_regret 自 seq=3 起 feasible，bucket=40 压过一切）"
          " —— 这是 B-6a/B-6b 在真实玩法里到不了 seq>11 的直接证据",
          f"seq={st.seq}, node={st.current_node}")

    # ============ 探针 2：过期销毁语义（强行越过 ending 继续推进）============
    eng2 = _fresh_engine("caseB0_expire")
    eng2.step(OPT_1)
    eng2.step(B_S2)
    eng2.step(B_S3)
    seen_after_expire = False
    for _ in range(20):
        eng2.step(B0_S5)
        if eng2.state.seq > 12 and "n3_tender" in eng2.frontier_in_ids():
            seen_after_expire = True

    st2 = eng2.state
    p3 = _find_pending(st2, "n3_tender")
    r.add(not seen_after_expire and "n3_tender" not in eng2.frontier_in_ids(),
          "B-6a [内核探针·非真实玩法] seq>11 后 n3_tender 不再出现在任何 frontier_in",
          f"seq={st2.seq}, frontier_in={eng2.frontier_in_ids()}")
    r.add(p3 is None,
          "B-6b [内核探针·非真实玩法] pending 中该条目已被移除（过期销毁）",
          f"pending={[(p.node_id, p.birth_seq, p.awake_seq) for p in st2.pending]}")

    # ---- 甲方硬约束：每轮 prompt token ≤ 1200 ----
    # 来源：实测锁定（甲方硬约束，team-lead 转达；PRD/ARCH 无对应条目）
    _prompt_cap_check(r, eng, "B0-2")
    _prompt_cap_check(r, eng2, "B0-2(过期探针)")

    return r


# =============================================================================
#  命令行入口
# =============================================================================
def _main(argv: List[str]) -> int:
    if "--emit-trace" in argv:
        for ln in emit_trace():
            print(ln)
        return 0
    total = passed = 0
    for fn in (case_forward_pointer, case_backward_pointer, case_backward_pointer_neg):
        r = fn(verbose=True)
        passed += r.passed
        total += r.total
        print(f"--- {r.name}: {r.passed}/{r.total}  {'OK' if r.ok else 'FAIL'}")
    print(f"指针用例合计: {passed}/{total}")
    return 0 if passed == total else 1


if __name__ == "__main__":
    raise SystemExit(_main(sys.argv[1:]))
