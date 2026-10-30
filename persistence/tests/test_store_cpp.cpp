// persistence/tests/test_store_cpp.cpp
//
// EventStore.hpp 的 C++ 侧编译+运行验证。
// 这一层在 Windows (Dev-Cpp MinGW 无 sqlite3) 上从未被编译过，
// 只有在带 libsqlite3 的 Linux 上才能真正验证。
//
// 编译：
//   g++ -std=c++17 -Wall -Wextra -O1
//       -I../include -I../../engine/include -I../../include
//       test_store_cpp.cpp -o test_store_cpp -lsqlite3
// 运行：./test_store_cpp（cwd 需为 persistence/tests，因为 migrate 脚本路径相对）

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "nx/EventStore.hpp"
#include "nx/GameState.hpp"

static int g_pass = 0;
static int g_fail = 0;

static void ok(const std::string& name) {
    std::cout << "  [OK] " << name << "\n";
    ++g_pass;
}
static void fail(const std::string& name, const std::string& detail) {
    std::cout << "  [FAIL] " << name << " :: " << detail << "\n";
    ++g_fail;
}

// 期望这段代码抛异常；不抛则算失败
template <typename Fn>
static void expect_throw(const std::string& name, Fn fn) {
    try {
        fn();
        fail(name, "预期被拒绝，但居然成功了");
    } catch (const nx::NxError&) {
        ok(name);
    } catch (const std::exception& e) {
        fail(name, std::string("抛了非 NxError: ") + e.what());
    }
}

int main() {
    using nx::Seq;
    using nx::GameState;

    const std::string db = "cpp_smoke.db";
    std::remove(db.c_str());
    std::remove((db + "-wal").c_str());
    std::remove((db + "-shm").c_str());

    std::cout << "=== EventStore C++ 烟测（真实 libsqlite3） ===\n";

    nx::EventStore store(db);
    store.migrate({"../../persistence/migrations/001_init.sql",
                   "../../persistence/migrations/002_bfs_state.sql"});
    std::cout << "  [OK] 001 + 002 迁移执行成功\n";
    ++g_pass;

    // 幂等性：再迁移一次不应炸
    try {
        store.migrate({"../../persistence/migrations/001_init.sql",
                       "../../persistence/migrations/002_bfs_state.sql"});
        ok("重复 migrate 幂等（无异常）");
    } catch (const std::exception& e) {
        fail("重复 migrate 幂等", e.what());
    }

    store.create_session("s1", "v2-example");
    if (store.time_cursor("s1") == -1) ok("初始 time_cursor == -1");
    else fail("初始 time_cursor", "期望 -1，实际 " + std::to_string(store.time_cursor("s1")));

    store.append_event("s1", 0, "turn", "{\"to\":\"n_kitchen\"}", "0", "n_kitchen");
    store.append_event("s1", 1, "turn", "{\"to\":\"n_search\"}", "n_kitchen", "n_search");
    store.append_event("s1", 2, "turn", "{\"to\":\"n_confront\"}", "n_search", "n_confront");
    if (store.time_cursor("s1") == 2) ok("连续追加 3 条后 time_cursor == 2");
    else fail("time_cursor 推进", "期望 2，实际 " + std::to_string(store.time_cursor("s1")));

    expect_throw("G2 游标跳号（直接写 seq=5）被拒绝", [&] {
        store.append_event("s1", 5, "turn", "{}", "x", "y");
    });
    expect_throw("G1 同一 seq 重复写入被拒绝", [&] {
        store.append_event("s1", 2, "turn", "{}", "x", "y");
    });

    GameState st;
    st.session_id = "s1";
    st.seq = 2;
    st.current_node = "n_confront";
    store.put_snapshot("s1", 2, st);
    ok("快照写入成功（ckpt_seq=2 == time_cursor）");

    expect_throw("G4 超前快照（ckpt_seq=9 > cursor）被拒绝", [&] {
        store.put_snapshot("s1", 9, st);
    });

    auto rp = store.restore("s1");
    if (rp.from_snapshot && rp.ckpt_seq == 2) ok("restore 取回最新快照 ckpt_seq=2");
    else fail("restore", "from_snapshot=" + std::to_string(rp.from_snapshot) +
                             " ckpt_seq=" + std::to_string(rp.ckpt_seq));

    store.sync_pending("s1", st);
    ok("sync_pending 执行成功");

    store.audit_route("s1", 2, "L0", "n_confront", 1.0, "玩家点了选项按钮",
                      "n_confront|n_idle", "n_kitchen:phase");
    ok("audit_route 落库成功");

    store.begin_llm_call("req-1", "s1", 2, "prompt-text");
    auto un = store.unresolved_calls("s1");
    if (un.size() == 1) ok("LLM 调用登记后 unresolved=1");
    else fail("unresolved_calls", "期望 1，实际 " + std::to_string(un.size()));

    // 注意：begin_llm_call 的契约是返回 bool（false = 已有同 id 调用），不是抛异常。
    // 这样设计是为了让调用方能自己决定「跳过」而不是被迫 try/catch。
    if (store.begin_llm_call("req-1", "s1", 2, "prompt-text") == false)
        ok("同一 request_id 重复登记返回 false（幂等，不重复发）");
    else
        fail("begin_llm_call 幂等", "期望返回 false，实际返回 true");

    store.resolve_llm_call("req-1", "{\"node\":\"n_confront\"}");
    if (store.unresolved_calls("s1").empty()) ok("resolve 后 unresolved 清空");
    else fail("resolve_llm_call", "仍有未解决调用");

    // 回放一致性：快照(2) 之后应有 0 条事件
    auto tail = store.events_after("s1", 2);
    if (tail.empty()) ok("events_after(2) 为空 —— 快照与游标对齐");
    else fail("events_after", "期望 0 条，实际 " + std::to_string(tail.size()));

    std::cout << "\n结果: " << g_pass << "/" << (g_pass + g_fail) << " 项通过\n";
    return g_fail == 0 ? 0 : 1;
}
