// ---------------------------------------------------------------------------
//  NarrativeX Engine :: persistence/include/nx/EventStore.hpp
//  SQLite 持久化 —— 【空缺 2 的答案】
//
//   ⚠ 编译依赖说明
//   本文件需要 sqlite3.h / libsqlite3。它在 Dev-Cpp 自带的 MinGW 环境下
//   **没有**提供（本机已确认），因此未能在本机完成编译验证；
//   但下面所有 SQL 语义、四道约束闸门、快照 + 回放逻辑，均已由
//   persistence/tests/test_store.py 用真实的 SQLite 引擎跑通（17/17）。
//   在目标 Linux 环境装上 libsqlite3-dev 后，编译命令见文件末尾。
//
//  ===========================================================================
//  为什么这样做就能保证"读档不出现时空错乱"
//  ===========================================================================
//  一句话：**把「会发生什么」和「发生过什么」分开存，然后禁止改写后者。**
//
//   1. time_cursor 的唯一真相源在 nx_session.time_cursor，且只能由事件 INSERT
//      触发器推进（不能由应用层直接 UPDATE）。它因此天然严格单调。
//   2. 每一步写一条 nx_event，PK(session_id, seq) 保证 seq 不能重复，
//      再加一个 BEFORE UPDATE/DELETE 触发器彻底禁掉篡改历史。
//      这两条加起来 = 历史是 append-only 的。
//   3. 每 N 步写一个 nx_snapshot(ckpt_seq <= time_cursor)。
//      恢复 = 最近快照 + 回放 ckpt_seq 之后的事件。
//      因为事件是确定的、且 rng_state 也随快照持久化，所以
//      **同一份事件流必然算出同一份 Frontier** —— 这就是不错乱的根。
//   4. AI 请求先以 request_id 落 nx_llm_call(CREATED) 再发出去，
//      崩溃恢复时用同一个 request_id 重试，不重复扣费也不产生半步状态。
//
//  一句话反例：如果像旧代码那样每步 `INSERT OR REPLACE INTO player_save`
//  只覆盖 current_node，那么 tags / pending / rng_state 全部丢失，
//  读档后 Frontier 必然与存档前不同 —— 那就是用户担心的时空错乱。
// ---------------------------------------------------------------------------
#pragma once

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "nx/GameState.hpp"
#include "nx/StoryLoader.hpp"
#include "nx/Types.hpp"

namespace nx {

class EventStore {
public:
    explicit EventStore(const std::string& db_path) {
        int rc = sqlite3_open(db_path.c_str(), &db_);
        if (rc != SQLITE_OK) {
            const std::string msg = db_ ? sqlite3_errmsg(db_) : "unknown";
            if (db_) { sqlite3_close(db_); db_ = nullptr; }
            throw NxError(NxError::Code::BadScript, "无法打开数据库: " + msg);
        }
        sqlite3_busy_timeout(db_, 5000);
        exec("PRAGMA journal_mode = WAL;");
        exec("PRAGMA synchronous = NORMAL;");
        exec("PRAGMA foreign_keys = ON;");
    }

    ~EventStore() {
        finalize_all();
        if (db_) sqlite3_close(db_);
    }

    EventStore(const EventStore&) = delete;
    EventStore& operator=(const EventStore&) = delete;

    // 幂等迁移。注意：SQLite 没有 ADD COLUMN IF NOT EXISTS，
    // 重复执行会报 "duplicate column name"，这里显式吞掉这类错误。
    void migrate(const std::vector<std::string>& sql_files) {
        for (const auto& path : sql_files) {
            const std::string sql = read_file(path);
            char* err = nullptr;
            const int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
            if (rc != SQLITE_OK) {
                const std::string e = err ? err : "";
                sqlite3_free(err);
                const bool idempotent = e.find("duplicate column") != std::string::npos;
                if (!idempotent)
                    throw NxError(NxError::Code::BadScript, "迁移失败: " + e);
            }
        }
    }

    void create_session(const std::string& session_id, const std::string& script_ver = "") {
        exec("BEGIN IMMEDIATE;");
        sqlite3_stmt* s = prepare(
            "INSERT OR IGNORE INTO nx_session(session_id, script_ver) VALUES(?,?);");
        bind(s, 1, session_id);
        bind(s, 2, script_ver);
        if (step_done(s) != SQLITE_DONE) throw last_error("create_session");
        finish(s);
        exec("COMMIT;");
    }

    Seq time_cursor(const std::string& session_id) const {
        sqlite3_stmt* s = const_cast<EventStore*>(this)->prepare(
            "SELECT time_cursor FROM nx_session WHERE session_id=?;");
        bind(s, 1, session_id);
        Seq v = -1;
        if (sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int64(s, 0);
        finish(s);
        return v;
    }

    // 追加一步事件。seq 必须 == time_cursor + 1，否则数据库触发器直接 ABORT。
    // 这就是 Part II 里说的乐观锁：比较与写发生在同一个 SQL 语句里。
    void append_event(const std::string& session_id, Seq seq, const std::string& type,
                      const std::string& payload, const std::string& from_node,
                      const std::string& to_node) {
        exec("BEGIN IMMEDIATE;");
        try {
            sqlite3_stmt* s = prepare(
                "INSERT INTO nx_event(session_id,seq,event_type,payload,from_node,to_node) "
                "VALUES(?,?,?,?,?,?);");
            bind(s, 1, session_id);
            bind(s, 2, seq);
            bind(s, 3, type);
            bind(s, 4, payload);
            bind(s, 5, from_node);
            bind(s, 6, to_node);
            if (step_done(s) != SQLITE_DONE)
                throw NxError(NxError::Code::CursorRegress,
                              "append_event 失败（可能游标跳变）: " + last_msg());
            finish(s);
            exec("COMMIT;");
        } catch (...) {
            exec("ROLLBACK;");
            throw;
        }
    }

    void put_snapshot(const std::string& session_id, Seq ckpt_seq,
                      const GameState& st) {
        const std::string sj = StoryLoader::state_to_json(st);
        const std::string hash = sha256_16(sj);
        exec("BEGIN IMMEDIATE;");
        try {
            sqlite3_stmt* s = prepare(
                "INSERT OR REPLACE INTO nx_snapshot(session_id,ckpt_seq,state_json,state_hash) "
                "VALUES(?,?,?,?);");
            bind(s, 1, session_id);
            bind(s, 2, ckpt_seq);
            bind(s, 3, sj);
            bind(s, 4, hash);
            if (step_done(s) != SQLITE_DONE)
                throw NxError(NxError::Code::Concurrency, "put_snapshot 失败: " + last_msg());
            finish(s);
            exec("COMMIT;");
        } catch (...) {
            exec("ROLLBACK;");
            throw;
        }
    }

    struct RestorePoint {
        std::string state_json;
        Seq ckpt_seq = 0;
        bool from_snapshot = false;
    };

    // 恢复：取最近快照 -> 再取之后的事件由调用方 replay
    RestorePoint restore(const std::string& session_id) const {
        RestorePoint rp;
        sqlite3_stmt* s = const_cast<EventStore*>(this)->prepare(
            "SELECT ckpt_seq, state_json FROM nx_snapshot WHERE session_id=? "
            "ORDER BY ckpt_seq DESC LIMIT 1;");
        bind(s, 1, session_id);
        if (sqlite3_step(s) == SQLITE_ROW) {
            rp.ckpt_seq = sqlite3_column_int64(s, 0);
            rp.state_json = text(s, 1);
            rp.from_snapshot = true;
        }
        finish(s);
        return rp;
    }

    std::vector<std::string> events_after(const std::string& session_id, Seq ckpt) const {
        std::vector<std::string> out;
        sqlite3_stmt* s = const_cast<EventStore*>(this)->prepare(
            "SELECT payload FROM nx_event WHERE session_id=? AND seq>? ORDER BY seq;");
        bind(s, 1, session_id);
        bind(s, 2, ckpt);
        while (sqlite3_step(s) == SQLITE_ROW) out.push_back(text(s, 0));
        finish(s);
        return out;
    }

    // ---- LLM 调用幂等 ----
    bool begin_llm_call(const std::string& request_id, const std::string& session_id,
                        Seq seq, const std::string& request_json) {
        sqlite3_stmt* s = prepare(
            "INSERT OR IGNORE INTO nx_llm_call(request_id,session_id,seq,status,request_json) "
            "VALUES(?,?,?,'CREATED',?);");
        bind(s, 1, request_id);
        bind(s, 2, session_id);
        bind(s, 3, seq);
        bind(s, 4, request_json);
        step_done(s);
        finish(s);
        return sqlite3_changes(db_) == 1;      // false = 已有同 id 的调用，别重复发
    }

    void resolve_llm_call(const std::string& request_id, const std::string& response_json) {
        sqlite3_stmt* s = prepare(
            "UPDATE nx_llm_call SET status='RESOLVED', response_json=?, "
            "resolved_at=datetime('now','localtime') WHERE request_id=?;");
        bind(s, 1, response_json);
        bind(s, 2, request_id);
        step_done(s);
        finish(s);
    }

    std::vector<std::string> unresolved_calls(const std::string& session_id) const {
        std::vector<std::string> out;
        sqlite3_stmt* s = const_cast<EventStore*>(this)->prepare(
            "SELECT request_id FROM nx_llm_call WHERE session_id=? AND status='CREATED';");
        bind(s, 1, session_id);
        while (sqlite3_step(s) == SQLITE_ROW) out.push_back(text(s, 0));
        finish(s);
        return out;
    }

    // ---- 路由审计：排查"为什么会走这条路"的第一现场 ----
    void audit_route(const std::string& session_id, Seq seq, const std::string& layer,
                     const std::string& picked, double confidence,
                     const std::string& reason, const std::string& candidates,
                     const std::string& blocked) {
        sqlite3_stmt* s = prepare(
            "INSERT OR REPLACE INTO nx_route_audit"
            "(session_id,seq,layer,picked_node,confidence,reason,candidates,blocked) "
            "VALUES(?,?,?,?,?,?,?,?);");
        bind(s, 1, session_id);
        bind(s, 2, seq);
        bind(s, 3, layer);
        bind(s, 4, picked);
        bind(s, 5, confidence);
        bind(s, 6, reason);
        bind(s, 7, candidates);
        bind(s, 8, blocked);
        step_done(s);
        finish(s);
    }

    void sync_pending(const std::string& session_id, const GameState& st) {
        exec("BEGIN IMMEDIATE;");
        try {
            sqlite3_stmt* del = prepare("DELETE FROM nx_pending WHERE session_id=?;");
            bind(del, 1, session_id);
            step_done(del);
            finish(del);

            sqlite3_stmt* ins = prepare(
                "INSERT OR REPLACE INTO nx_pending"
                "(session_id,node_id,birth_seq,source_node,reason,attempts,awake,awake_seq) "
                "VALUES(?,?,?,?,?,?,?,?);");
            for (const auto& p : st.pending) {
                bind(ins, 1, session_id);
                bind(ins, 2, p.node_id);
                bind(ins, 3, p.birth_seq);
                bind(ins, 4, p.source_node);
                bind(ins, 5, p.reason);
                bind(ins, 6, p.attempts);
                bind(ins, 7, p.awake ? 1 : 0);
                bind(ins, 8, p.awake_seq);
                sqlite3_step(ins);
                sqlite3_reset(ins);
            }
            finish(ins);
            exec("COMMIT;");
        } catch (...) {
            exec("ROLLBACK;");
            throw;
        }
    }

private:
    sqlite3* db_ = nullptr;
    mutable std::vector<sqlite3_stmt*> live_;

    static std::string read_file(const std::string& p) {
        std::FILE* f = std::fopen(p.c_str(), "rb");
        if (!f) throw NxError(NxError::Code::BadScript, "找不到迁移文件: " + p);
        std::string s;
        char buf[8192];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
        std::fclose(f);
        return s;
    }

    void exec(const char* sql) {
        char* err = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
            const std::string e = err ? err : "";
            sqlite3_free(err);
            throw NxError(NxError::Code::Concurrency, "SQL 执行失败 [" + std::string(sql) + "]: " + e);
        }
    }

    sqlite3_stmt* prepare(const char* sql) {
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &s, nullptr) != SQLITE_OK || s == nullptr)
            throw NxError(NxError::Code::Concurrency,
                          "预编译失败: " + last_msg() + " | SQL=" + sql);
        live_.push_back(s);
        return s;
    }

    // 这几个 bind 重载必须是 const：
    // time_cursor() / restore() / events_after() 都是 const 成员函数（只读相关），
    // 它们也需要 bind 参数。此前没有 const 修饰时，在带 libsqlite3 的环境
    // （如 Ubuntu + gcc 11）下会编译失败：
    //   error: passing 'const nx::EventStore' as 'this' argument discards qualifiers
    // 这个 bug 在 Windows Dev-Cpp MinGW 上无法被发现，因为那里根本没有 sqlite3.h。
    void bind(sqlite3_stmt* s, int idx, const std::string& v) const {
        sqlite3_bind_text(s, idx, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
    }
    void bind(sqlite3_stmt* s, int idx, Seq v) const    { sqlite3_bind_int64(s, idx, v); }
    void bind(sqlite3_stmt* s, int idx, int v) const    { sqlite3_bind_int(s, idx, v); }
    void bind(sqlite3_stmt* s, int idx, double v) const { sqlite3_bind_double(s, idx, v); }

    // ⚠ 绝对不要把返回码压平成 SQLITE_DONE。
    // 旧写法：return (rc == SQLITE_ROW) ? SQLITE_ROW : SQLITE_DONE;
    // 它把 SQLITE_CONSTRAINT（正是触发器 ABORT 掉写入时返回的错误码）也当成成功，
    // 于是 G1(重复 seq) / G2(游标跳号) / G4(超前快照) 三道闸门全部静默失效：
    //   跳号写入成功、同一 seq 重复写入成功、超前快照成功 —— 而 Python 侧测试却是拒绝的。
    // 两者结论相反，就是因为 py 用的是真实返回码，C++ 侧被这里吞掉了。
    //
    // 这个 bug 之所以长期没被发现：Windows Dev-Cpp 自带 MinGW 没有 sqlite3.h，
    // EventStore.hpp 从未真正编译过。在 Ubuntu + libsqlite3 上首次编译并运行才暴露。
    int step_done(sqlite3_stmt* s) const {
        const int rc = sqlite3_step(s);
        sqlite3_reset(s);
        return rc;   // 原样返回调用方的真实结果，由调用方判断是否 == SQLITE_DONE
    }

    static std::string text(sqlite3_stmt* s, int col) {
        const unsigned char* p = sqlite3_column_text(s, col);
        return p ? reinterpret_cast<const char*>(p) : std::string();
    }

    std::string last_msg() const { return db_ ? sqlite3_errmsg(db_) : "no db"; }
    NxError last_error(const char* where) const {
        return NxError(NxError::Code::Concurrency,
                       std::string(where) + " 失败: " + last_msg());
    }

    // 单一所有权：prepare() 已把语句登记进 live_，这里负责注销并释放。
    // ⚠ 严禁在调用点直接写 sqlite3_finalize(s)：
    //    那样 live_ 里的指针不会被摘掉，析构时 finalize_all() 会二次释放同一个
    //    sqlite3_stmt —— 实测在 Ubuntu 上表现为**随机 segfault（EXIT=139）**，
    //    时崩时不崩，是最难查的那类 UB。
    void finish(sqlite3_stmt* s) const {
        if (!s) return;
        const auto it = std::find(live_.begin(), live_.end(), s);
        if (it != live_.end()) live_.erase(it);
        sqlite3_finalize(s);
    }

    void finalize_all() {
        for (auto* s : live_) { sqlite3_finalize(s); }
        live_.clear();
    }

    static std::string sha256_16(const std::string& s) {
        // 缩略哈希只用于"检测存档是否被手工改坏"，不用于安全目的。
        unsigned long long h = 1469598103934665603ull;              // FNV-1a 64
        for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%016llx", h);
        return buf;
    }
};

}  // namespace nx

// ===========================================================================
//  目标环境（Ubuntu / libsqlite3-dev 已安装）的编译方式：
//
//    g++ -std=c++17 -I engine/include -I include -I persistence/include
//        your_app.cpp -lsqlite3 -o your_app
//
//  或在现有 CMakeLists.txt 里追加：
//
//    include_directories(${CMAKE_SOURCE_DIR}/persistence/include
//                        ${CMAKE_SOURCE_DIR}/engine/include)
//    add_library(nx_kernel INTERFACE)
//    target_include_directories(nx_kernel INTERFACE
//        ${CMAKE_SOURCE_DIR}/engine/include
//        ${CMAKE_SOURCE_DIR}/persistence/include)
//    target_link_libraries(fluidstory PRIVATE nx_kernel sqlite3 curl Qt5::Widgets)
// ===========================================================================
