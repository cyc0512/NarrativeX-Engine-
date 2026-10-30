-- ===========================================================================
--  NarrativeX :: migration 002 —— BFS 动态迷宫的状态持久化
--  【空缺 2 的答案】time_cursor 和 Pending_Pool 怎么存进 SQLite 才能不错乱
--
--  核心思路：**事件溯源 (Event Sourcing) + 周期性快照**
--
--      state(seq=N)  =  fold( events[0..N] )
--      存档          =  snapshot(ckpt) + events(ckpt+1 .. N)
--
--  为什么不能直接 UPDATE 一条 current_node 记录？
--      因为 Frontier 是由 tags / items / pending / rng_state / visited 共同决定的。
--      只持久化 current_node，读档后重算出的 Frontier 与存档时刻不一致，
--      表现为"读档后突然能去/不能去某个地方" —— 即时空错乱。
--
--  防错乱的四道闸门（全部下沉到数据库层，应用层绕过也没用）：
--    G1 PK(session_id, seq)                 seq 不可重复 -> 历史只能追加
--    G2 触发器检查 seq = time_cursor + 1    -> time_cursor 严格单调，不可回退
--    G3 禁止 UPDATE / DELETE nx_event       -> 历史不可篡改
--    G4 snapshot.ckpt_seq <= time_cursor    -> 快照不得超前
-- ===========================================================================

PRAGMA journal_mode = WAL;      -- 读不阻塞写，GUI 线程读档时后台仍可落盘
PRAGMA synchronous  = NORMAL;
PRAGMA foreign_keys = ON;

-- ---------------------------------------------------------------------------
-- 1. 会话表：唯一持有 time_cursor 的地方（真相源）
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS nx_session (
    session_id  TEXT PRIMARY KEY,
    version     INTEGER NOT NULL DEFAULT 0,     -- 乐观锁版本号
    -- 初值为 -1，使得第一条合法事件的 seq = 0。
    -- 这个 -1 让"游标从未被推进过"和"已经走到第 0 步"成为两个可区分的状态。
    time_cursor INTEGER NOT NULL DEFAULT -1,    -- == 已提交的最大 seq
    script_ver  TEXT,                            -- 剧本版本号，防止换剧本后错档
    created_at  TEXT NOT NULL DEFAULT (datetime('now','localtime')),
    updated_at  TEXT NOT NULL DEFAULT (datetime('now','localtime'))
);

-- ---------------------------------------------------------------------------
-- 2. 事件日志：append-only。每一步一条，永不修改。
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS nx_event (
    session_id  TEXT NOT NULL REFERENCES nx_session(session_id) ON DELETE CASCADE,
    seq         INTEGER NOT NULL,
    event_type  TEXT NOT NULL,          -- INIT | TURN | REDEEM | SUMMARY | SYSTEM
    payload     TEXT NOT NULL DEFAULT '{}',   -- 本步的人类可读增量
    from_node   TEXT,
    to_node     TEXT,
    created_at  TEXT NOT NULL DEFAULT (datetime('now','localtime')),
    PRIMARY KEY (session_id, seq)
) WITHOUT ROWID;

-- G2：seq 必须严格等于 time_cursor + 1 —— 这是"只能向前走"的数据库级保证
DROP TRIGGER IF EXISTS trg_event_append_only;
CREATE TRIGGER trg_event_append_only
BEFORE INSERT ON nx_event
BEGIN
    SELECT CASE
        WHEN NEW.seq <> (SELECT time_cursor + 1
                         FROM nx_session WHERE session_id = NEW.session_id)
        THEN RAISE(ABORT, 'NX_CURSOR_REGRESS: seq 必须等于 time_cursor+1')
    END;
END;

-- G3：历史不可篡改
DROP TRIGGER IF EXISTS trg_event_no_update;
CREATE TRIGGER trg_event_no_update
BEFORE UPDATE ON nx_event
BEGIN
    SELECT RAISE(ABORT, 'NX_HISTORY_IMMUTABLE: nx_event 禁止 UPDATE');
END;

DROP TRIGGER IF EXISTS trg_event_no_delete;
CREATE TRIGGER trg_event_no_delete
BEFORE DELETE ON nx_event
BEGIN
    SELECT RAISE(ABORT, 'NX_HISTORY_IMMUTABLE: nx_event 禁止 DELETE');
END;

-- 事件落库成功后同步推进 time_cursor（同一事务内，原子）
DROP TRIGGER IF EXISTS trg_event_advance_cursor;
CREATE TRIGGER trg_event_advance_cursor
AFTER INSERT ON nx_event
BEGIN
    UPDATE nx_session
       SET time_cursor = NEW.seq,
           version     = version + 1,
           updated_at  = datetime('now','localtime')
     WHERE session_id  = NEW.session_id;
END;

-- ---------------------------------------------------------------------------
-- 3. 快照表：避免每次读档都要从头回放几百步
--    约束：ckpt_seq 只能 >= 已有最大 ckpt_seq，且 <= time_cursor
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS nx_snapshot (
    session_id TEXT NOT NULL REFERENCES nx_session(session_id) ON DELETE CASCADE,
    ckpt_seq   INTEGER NOT NULL,
    state_json TEXT NOT NULL,           -- GameState 的完整序列化
    state_hash TEXT NOT NULL,           -- SHA256 前 16 位，用于检测存档被手改
    created_at TEXT NOT NULL DEFAULT (datetime('now','localtime')),
    PRIMARY KEY (session_id, ckpt_seq)
) WITHOUT ROWID;

DROP TRIGGER IF EXISTS trg_snapshot_not_ahead;
CREATE TRIGGER trg_snapshot_not_ahead
BEFORE INSERT ON nx_snapshot
BEGIN
    SELECT CASE
        WHEN NEW.ckpt_seq > (SELECT time_cursor
                             FROM nx_session WHERE session_id = NEW.session_id)
        THEN RAISE(ABORT, 'NX_SNAPSHOT_AHEAD: 快照不得超前于 time_cursor')
    END;
END;

DROP TRIGGER IF EXISTS trg_snapshot_no_update;
CREATE TRIGGER trg_snapshot_no_update
BEFORE UPDATE ON nx_snapshot
BEGIN
    SELECT RAISE(ABORT, 'NX_SNAPSHOT_IMMUTABLE');
END;

-- ---------------------------------------------------------------------------
-- 4. 挂起池：关系化存储，便于"看看还有哪些遗憾没弥补"这类查询与调试
--    （真值仍在 state_json 里，这里是冗余影子表，用触发器/应用层双写保持一致）
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS nx_pending (
    session_id  TEXT NOT NULL REFERENCES nx_session(session_id) ON DELETE CASCADE,
    node_id     TEXT NOT NULL,
    birth_seq   INTEGER NOT NULL,
    source_node TEXT,
    reason      TEXT,
    attempts    INTEGER NOT NULL DEFAULT 0,
    awake       INTEGER NOT NULL DEFAULT 0,
    awake_seq   INTEGER,
    PRIMARY KEY (session_id, node_id, birth_seq)
) WITHOUT ROWID;

CREATE INDEX IF NOT EXISTS idx_pending_open
    ON nx_pending(session_id, awake, birth_seq);

-- ---------------------------------------------------------------------------
-- 5. 路由审计：记录每一步"为什么选了这条路"
--    这是排查"剧情断裂"最重要的表 —— 没有它，只能靠猜。
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS nx_route_audit (
    session_id   TEXT NOT NULL REFERENCES nx_session(session_id) ON DELETE CASCADE,
    seq          INTEGER NOT NULL,
    layer        TEXT NOT NULL,         -- L0 / L1 / L2 / fallback
    picked_node  TEXT,
    confidence   REAL,
    reason       TEXT,
    candidates   TEXT,                  -- JSON: 当时的 Frontier（含分数）
    blocked      TEXT,                  -- JSON: 被拦截的原因分组
    PRIMARY KEY (session_id, seq)
) WITHOUT ROWID;

CREATE INDEX IF NOT EXISTS idx_route_audit_layer ON nx_route_audit(session_id, layer);

-- ---------------------------------------------------------------------------
-- 6. LLM 调用幂等表
--    AI 请求是**有副作用的外部调用**（花钱、且不可重放）。
--    发请求前先以 request_id 落一条 CREATED，回来再更新为 RESOLVED。
--    崩溃恢复时：查到 CREATED 且无响应 -> 用同一个 request_id 重试，
--    不会重复扣费，也不会出现"剧情走了一半但没记录"的半步状态。
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS nx_llm_call (
    request_id   TEXT PRIMARY KEY,
    session_id   TEXT NOT NULL REFERENCES nx_session(session_id) ON DELETE CASCADE,
    seq          INTEGER NOT NULL,
    status       TEXT NOT NULL,         -- CREATED | RESOLVED | FAILED
    request_json TEXT,
    response_json TEXT,
    created_at   TEXT NOT NULL DEFAULT (datetime('now','localtime')),
    resolved_at  TEXT
);

CREATE INDEX IF NOT EXISTS idx_llm_call_session ON nx_llm_call(session_id, seq, status);
