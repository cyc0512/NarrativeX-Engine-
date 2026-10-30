# -*- coding: utf-8 -*-
"""
用 Python 的 sqlite3 **真实执行** persistence/migrations/*.sql，
验证四道防错乱闸门（G1~G4）确实生效，以及快照 + 回放能精确还原状态。

本机没有 sqlite3 的 C 开发库，无法编译 C++ 版本的 EventStore；
但 SQL 语义与约束在这里已经验证通过 —— C++ 侧只是把同样的语句包成
prepared statement，行为一致。

    python persistence/tests/test_store.py
"""

from __future__ import annotations

import hashlib
import json
import os
import sqlite3
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MIG = os.path.join(ROOT, "persistence", "migrations")
sys.path.insert(0, os.path.join(ROOT, "reference"))

_results: list[tuple[bool, str, str]] = []


def check(ok: bool, name: str, detail: str = "") -> None:
    _results.append((bool(ok), name, detail))
    print(f"{'  [OK]' if ok else '  [!!]'} {name}" + (f"  :: {detail}" if detail else ""))


def exec_script(con: sqlite3.Connection, path: str) -> None:
    """用 executescript 而不是自己 split(';') —— 触发器体里本来就含分号。

    幂等性：001 里的 ALTER TABLE ADD COLUMN 在重复执行时会抛
    'duplicate column name'，这是 SQLite 没有 ADD COLUMN IF NOT EXISTS 的
    固有限制，必须由 C++ 侧的 nx_migrate() 捕获并忽略。
    """
    with open(path, encoding="utf-8") as f:
        sql = f.read()
    try:
        con.executescript(sql)
    except sqlite3.OperationalError as e:
        if "duplicate column" in str(e).lower():
            return
        raise


def fresh_db() -> sqlite3.Connection:
    con = sqlite3.connect(":memory:")
    con.execute("PRAGMA foreign_keys = ON")
    exec_script(con, os.path.join(MIG, "001_init.sql"))
    exec_script(con, os.path.join(MIG, "002_bfs_state.sql"))
    return con


def main() -> None:
    print("=== 迁移能否应用 ===")
    con = fresh_db()
    tables = {r[0] for r in con.execute(
        "SELECT name FROM sqlite_master WHERE type IN ('table','trigger')")}
    need = {"nx_session", "nx_event", "nx_snapshot", "nx_pending",
            "nx_route_audit", "nx_llm_call", "player_save",
            "trg_event_append_only", "trg_event_no_update",
            "trg_event_no_delete", "trg_event_advance_cursor",
            "trg_snapshot_not_ahead"}
    missing = need - tables
    check(not missing, "001+002 迁移全部生效", f"缺失={missing}" if missing else f"{len(tables)} 个对象")

    con.execute("INSERT INTO nx_session(session_id) VALUES ('s1')")

    # ---------- G2：seq 必须 == time_cursor + 1 ----------
    print("\n=== G2 time_cursor 严格单调，不可回退 ===")
    con.execute("INSERT INTO nx_event(session_id,seq,event_type,payload) VALUES('s1',0,'INIT','{}')")
    check(con.execute("SELECT time_cursor FROM nx_session WHERE session_id='s1'").fetchone()[0] == 0,
          "G2a 首事件必须是 seq=0")

    try:
        con.execute("INSERT INTO nx_event(session_id,seq,event_type,payload) VALUES('s1',5,'TURN','{}')")
        ok = True
    except sqlite3.IntegrityError:
        ok = False
    check(not ok, "G2b 跳号写入被数据库拒绝（防游标跳变）")

    con.execute("INSERT INTO nx_event(session_id,seq,event_type,payload) VALUES('s1',1,'TURN','{}')")
    check(con.execute("SELECT time_cursor FROM nx_session WHERE session_id='s1'").fetchone()[0] == 1,
          "G2c 正常推进后 time_cursor=1")

    # ---------- G1：历史不可重复写 ----------
    print("\n=== G1 事件历史只追加 ===")
    try:
        con.execute("INSERT INTO nx_event(session_id,seq,event_type,payload) VALUES('s1',1,'TURN','{}')")
        ok = True
    except sqlite3.IntegrityError:
        ok = False
    check(not ok, "G1 同一 seq 重复写入 -> UNIQUE 约束拒绝")

    for bad, name in ((("UPDATE nx_event SET payload='{}' WHERE session_id='s1'"), "UPDATE"),
                      (("DELETE FROM nx_event WHERE session_id='s1'"), "DELETE")):
        try:
            con.execute(bad)
            ok = True
        except sqlite3.IntegrityError:
            ok = False
        check(not ok, f"G3 对 nx_event 执行 {name} 被触发器拒绝")

    # ---------- G4：快照不得超前 ----------
    print("\n=== G4 快照一致性 ===")
    snap = json.dumps({"seq": 1, "node": "n_kitchen"}, ensure_ascii=False)
    h = hashlib.sha256(snap.encode()).hexdigest()[:16]
    con.execute("INSERT INTO nx_snapshot VALUES('s1',1,?,?,datetime('now'))", (snap, h))
    check(True, "G4a ckpt_seq <= time_cursor 的快照可以写入")

    try:
        con.execute("INSERT INTO nx_snapshot VALUES('s1',9,?,?,datetime('now'))", (snap, h))
        ok = True
    except sqlite3.IntegrityError:
        ok = False
    check(not ok, "G4b ckpt_seq 超前 time_cursor -> 被拒绝（防凭空造档）")

    # ---------- 快照 + 回放还原 ----------
    print("\n=== 快照 + 增量回放 ===")
    for i in range(2, 12):
        con.execute("INSERT INTO nx_event(session_id,seq,event_type,payload,to_node) "
                    "VALUES('s1',?,'TURN',?,?)",
                    (i, json.dumps({"to": f"n{i}"}), f"n{i}"))
    cur = con.execute("SELECT time_cursor FROM nx_session WHERE session_id='s1'").fetchone()[0]
    check(cur == 11, "11 步事件落库后 time_cursor=11", f"实际 {cur}")

    ckpt, state = con.execute(
        "SELECT ckpt_seq, state_json FROM nx_snapshot "
        "WHERE session_id='s1' ORDER BY ckpt_seq DESC LIMIT 1").fetchone()
    after = con.execute(
        "SELECT seq, to_node FROM nx_event WHERE session_id='s1' AND seq > ? ORDER BY seq",
        (ckpt,)).fetchall()
    check(len(after) == 10, "从 ckpt=1 起需要回放 10 条事件", f"实际 {len(after)}")
    restored = json.loads(state)
    for seq, to_node in after:
        restored["seq"] = seq
        restored["node"] = to_node
    check(restored["seq"] == 11 and restored["node"] == "n11",
          "回放后状态与在线状态一致", f"{restored['node']}")

    # ---------- 幂等的 LLM 调用 ----------
    print("\n=== LLM 调用幂等（崩溃恢复不重复扣费）===")
    con.execute("INSERT INTO nx_llm_call(request_id,session_id,seq,status,request_json) "
                "VALUES(?,  's1', 12, 'CREATED', '{}')", ("req-abc-123",))
    try:
        con.execute("INSERT INTO nx_llm_call(request_id,session_id,seq,status) "
                    "VALUES('req-abc-123','s1',12,'RESOLVED')")
        ok = True
    except sqlite3.IntegrityError:
        ok = False
    check(not ok, "同一 request_id 重复登记被拒绝")

    con.execute("UPDATE nx_llm_call SET status='RESOLVED', response_json=?, "
                "resolved_at=datetime('now') WHERE request_id='req-abc-123'",
                (json.dumps({"next_node": "n_search"}),))
    st = con.execute("SELECT status FROM nx_llm_call WHERE request_id='req-abc-123'").fetchone()[0]
    check(st == "RESOLVED", "CREATED -> RESOLVED 状态机可用", st)

    pending = con.execute("SELECT COUNT(*) FROM nx_llm_call WHERE status='CREATED'").fetchone()[0]
    check(pending == 0, "崩溃后可据此找出所有未完成的调用", f"未完成 {pending} 条")

    # ---------- 旧表兼容 ----------
    print("\n=== 与既有三张表的兼容性 ===")
    con.execute("INSERT INTO player_save(player_name,affinity,current_node) VALUES(?,?,?)",
                ("张三", 90, "0"))
    con.execute("INSERT OR IGNORE INTO player_inventory(player_name,item_id) VALUES(?,?)",
                ("张三", "item_hairclip"))
    n_items = con.execute("SELECT COUNT(*) FROM player_inventory WHERE player_name='张三'").fetchone()[0]
    check(n_items == 1, "旧表在新库里仍可用（存档平滑升级）")

    # SQL 注入防护演示：C++ 侧用 prepared statement，这里用参数化查询证明可行
    con.execute("INSERT INTO player_save(player_name,affinity,current_node) VALUES(?,?,?)",
                ("O'Brien'); DROP TABLE player_save;--", 50, "0"))
    exists = con.execute("SELECT COUNT(*) FROM sqlite_master WHERE name='player_save'").fetchone()[0]
    check(exists == 1, "参数化绑定下恶意玩家名不会破坏表（对照：旧代码字符串拼接会炸）")

    print("\n" + "=" * 60)
    passed = sum(1 for ok_, _, _ in _results if ok_)
    for ok_, name, detail in _results:
        if not ok_:
            print(f"  FAILED: {name} :: {detail}")
    print(f"结果: {passed}/{len(_results)} 项通过")
    print("=" * 60)
    sys.exit(0 if passed == len(_results) else 1)


if __name__ == "__main__":
    main()
