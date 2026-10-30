-- ===========================================================================
--  NarrativeX :: migration 001 —— 兼容并加固既有三张表
--
--  背景：现有 DatabaseManager 已经建了 player_save / player_inventory /
--  player_tags 三张表，但存在两个必须修掉的工程问题：
--    1) 所有 SQL 都是字符串拼接 -> SQL 注入 + 名字里带单引号直接崩；
--    2) 多步写入（加道具 + 存档 + 加标签）没有事务 -> 会写一半。
--
--  本迁移只做"向后兼容的加固"，不改变表语义，旧存档可直接升级。
-- ===========================================================================

PRAGMA foreign_keys = ON;

-- 1. 三张表补建（对全新库等价 CREATE；对老库无副作用）
CREATE TABLE IF NOT EXISTS player_save (
    player_name  TEXT PRIMARY KEY,
    affinity     INTEGER NOT NULL DEFAULT 0,
    current_node TEXT NOT NULL DEFAULT '0',
    last_login   DATETIME DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS player_inventory (
    player_name TEXT NOT NULL,
    item_id     TEXT NOT NULL,
    obtained_at DATETIME DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (player_name, item_id)
);

CREATE TABLE IF NOT EXISTS player_tags (
    player_name TEXT NOT NULL,
    tag_id      TEXT NOT NULL,
    tag_type    TEXT DEFAULT 'state',
    obtained_at DATETIME DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (player_name, tag_id)
);

-- 2. 补缺失列（等价于旧代码里那段 PRAGMA table_info 自动迁移，用 SQL 表达）
--    SQLite 不支持 IF NOT EXISTS 的 ADD COLUMN，这里用应用层 try/catch 更安全；
--    但为了脚本可重复执行，下面语句在已存在列时会报错，由 nx_migrate() 捕获忽略。
ALTER TABLE player_inventory ADD COLUMN obtained_at DATETIME;

-- 3. 索引： getAllSaves 走 ORDER BY last_login，没有索引会全表扫
CREATE INDEX IF NOT EXISTS idx_player_save_login ON player_save(last_login DESC);
CREATE INDEX IF NOT EXISTS idx_tags_player ON player_tags(player_name);
CREATE INDEX IF NOT EXISTS idx_items_player ON player_inventory(player_name);
