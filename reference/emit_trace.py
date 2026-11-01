# -*- coding: utf-8 -*-
"""
生成 **预期轨迹** reference/expected_trace.txt，用于校验 C++ 实现与 Python
参考实现的行为完全一致（跨语言 Test Oracle）。

    python reference/emit_trace.py > reference/expected_trace.txt
"""

from __future__ import annotations

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nxref import NarrativeEngine, GameState, StubLLM  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STORY = os.path.join(ROOT, "data", "story_v2.example.json")

# 与 simulate.py 保持一致的输入序列
INPUTS = ["1", "2", "1", "1", "1", "我口袋里好像有东西", "1", "1", "1", "1", "1"]


def main() -> None:
    doc = json.load(open(STORY, encoding="utf-8"))
    eng = NarrativeEngine.from_json(
        doc, state=GameState(session_id="seed", rng_state=0xC0FFEE), llm=StubLLM())

    def line(i, before, res):
        st = eng.state
        return (
            f"step={i} seq={res.debug['seq']} from={before} to={st.current_node} "
            f"layer={res.decision.layer} phase={st.phase} aff={st.affinity} "
            f"tags=[{','.join(sorted(st.tags))}] items=[{','.join(sorted(st.items))}] "
            f"pending_open={sum(1 for p in st.pending if not p.awake)} "
            f"redeemed=[{','.join(res.redeemed)}]")

    for i, inp in enumerate(INPUTS, 1):
        before = eng.state.current_node
        res = eng.step(inp)
        print(line(i, before, res))


if __name__ == "__main__":
    main()
