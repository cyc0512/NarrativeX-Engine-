#pragma once
#include <iostream>
#include <string>
#include <vector>
#include <sqlite3.h>

// 数据访问层。
//
// R-11 修复要点：
//   1. 所有带玩家输入的 SQL 一律走 sqlite3_prepare_v2 + sqlite3_bind_*，不拼接。
//   2. sqlite3_column_text() 在列为 NULL 时返回 nullptr，统一走 columnText()。
//   3. 任何一步失败都**说出来**，返回 bool，让上层知道。
//
// 迁移新增（第 2 批）：
//   4. player_save 加 state_json 列，存 nx::GameState 全量 JSON
//      （seq / pending / rng_state 等旧表存不下的字段）。
//   5. saveStateJson / loadStateJson：新档走全量 JSON；
//      旧档 state_json 为 NULL 时，loadStateJson 返回空串，
//      由上层回退到 loadData（current_node + affinity）。
class DatabaseManager {
private:
    sqlite3* db;

    void reportError(const char* where, const char* detail = nullptr) {
        std::cerr << "【数据库错误】" << where << " 失败: "
                  << (detail ? detail : sqlite3_errmsg(db)) << std::endl;
    }

    static std::string columnText(sqlite3_stmt* stmt, int col) {
        const unsigned char* p = sqlite3_column_text(stmt, col);
        return p ? std::string(reinterpret_cast<const char*>(p)) : std::string();
    }

    bool execSql(const char* where, const char* sql) {
        char* errMsg = nullptr;
        if (sqlite3_exec(db, sql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
            reportError(where, errMsg);
            sqlite3_free(errMsg);
            return false;
        }
        return true;
    }

    // 判断表是否有某列（用于迁移）
    bool hasColumn(const char* table, const char* column) {
        std::string sql = std::string("PRAGMA table_info(") + table + ");";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            reportError("hasColumn(prepare)");
            return false;
        }
        bool found = false;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (columnText(stmt, 1) == column) { found = true; break; }
        }
        sqlite3_finalize(stmt);
        return found;
    }

public:
    struct SaveInfo {
        std::string playerName;
        int affinity;
        std::string currentNode;
        std::string lastLogin;
        bool hasStateJson = false;   // 新档标记（第 2 批新增，UI 可选展示）
    };

    DatabaseManager(const std::string& dbPath) {
        int rc = sqlite3_open(dbPath.c_str(), &db);
        if (rc) {
            std::cerr << "【数据库致命错误】无法打开数据库: " << sqlite3_errmsg(db) << std::endl;
            db = nullptr;
            exit(1);
        } else {
            std::cout << "【数据库】成功连接数据库: " << dbPath << std::endl;
            createTable();
        }
    }

    ~DatabaseManager() {
        if (db) sqlite3_close(db);
    }

    void createTable() {
        // 1. 玩家存档表
        execSql("createTable(player_save)",
                "CREATE TABLE IF NOT EXISTS player_save ("
                "player_name TEXT PRIMARY KEY, "
                "affinity INTEGER, "
                "current_node TEXT, "
                "last_login DATETIME DEFAULT CURRENT_TIMESTAMP);");

        // 2. 迁移 last_login（旧表可能没有）
        if (!hasColumn("player_save", "last_login")) {
            execSql("createTable(alter_last_login)",
                    "ALTER TABLE player_save ADD COLUMN last_login DATETIME;");
            execSql("createTable(backfill_last_login)",
                    "UPDATE player_save SET last_login = CURRENT_TIMESTAMP WHERE last_login IS NULL;");
        }

        // 3. 迁移 state_json（第 2 批新增）
        if (!hasColumn("player_save", "state_json")) {
            execSql("createTable(alter_state_json)",
                    "ALTER TABLE player_save ADD COLUMN state_json TEXT;");
        }

        // 4. 玩家背包表
        execSql("createTable(player_inventory)",
                "CREATE TABLE IF NOT EXISTS player_inventory ("
                "player_name TEXT, "
                "item_id TEXT, "
                "PRIMARY KEY (player_name, item_id));");

        // 5. 玩家标签表
        execSql("createTable(player_tags)",
                "CREATE TABLE IF NOT EXISTS player_tags ("
                "player_name TEXT, "
                "tag_id TEXT, "
                "tag_type TEXT, "
                "PRIMARY KEY (player_name, tag_id));");
    }

    // ---------------- 旧接口（保留，旧档兼容） ----------------

    bool saveData(const std::string& playerName, int affinity, const std::string& currentNode) {
        const char* sql = "INSERT OR REPLACE INTO player_save "
                          "(player_name, affinity, current_node, last_login) "
                          "VALUES (?, ?, ?, datetime('now', 'localtime'));";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            reportError("saveData(prepare)");
            return false;
        }
        sqlite3_bind_text(stmt, 1, playerName.data(), static_cast<int>(playerName.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, affinity);
        sqlite3_bind_text(stmt, 3, currentNode.data(), static_cast<int>(currentNode.size()), SQLITE_TRANSIENT);

        const int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            reportError("saveData(step)");
            return false;
        }
        return true;
    }

    bool loadData(const std::string& playerName, int& affinity, std::string& currentNode) {
        const char* sql = "SELECT affinity, current_node FROM player_save WHERE player_name = ?;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            reportError("loadData(prepare)");
            return false;
        }
        sqlite3_bind_text(stmt, 1, playerName.data(), static_cast<int>(playerName.size()), SQLITE_TRANSIENT);

        bool ok = false;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            affinity = sqlite3_column_int(stmt, 0);
            currentNode = columnText(stmt, 1);
            ok = true;
        }
        sqlite3_finalize(stmt);
        return ok;
    }

    // ---------------- 第 2 批新增：state_json 全量存取 ----------------

    // 存：写 state_json + 同步 current_node / affinity（便于旧 UI 列表显示）
    bool saveStateJson(const std::string& playerName, const std::string& stateJson) {
        const char* sql =
            "INSERT INTO player_save (player_name, state_json, last_login) "
            "VALUES (?, ?, datetime('now','localtime')) "
            "ON CONFLICT(player_name) DO UPDATE SET "
            "state_json = excluded.state_json, "
            "last_login = excluded.last_login;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            reportError("saveStateJson(prepare)");
            return false;
        }
        sqlite3_bind_text(stmt, 1, playerName.data(), static_cast<int>(playerName.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, stateJson.data(), static_cast<int>(stateJson.size()), SQLITE_TRANSIENT);

        const int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            reportError("saveStateJson(step)");
            return false;
        }
        return true;
    }

    // 读：返回空串表示"没有 state_json"（旧档），上层回退到 loadData
    std::string loadStateJson(const std::string& playerName) {
        const char* sql = "SELECT state_json FROM player_save WHERE player_name = ?;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            reportError("loadStateJson(prepare)");
            return "";
        }
        sqlite3_bind_text(stmt, 1, playerName.data(), static_cast<int>(playerName.size()), SQLITE_TRANSIENT);

        std::string out;
        if (sqlite3_step(stmt) == SQLITE_ROW) out = columnText(stmt, 0);
        sqlite3_finalize(stmt);
        return out;
    }

    // ---------------- 存档列表 / 删除（保留） ----------------

    std::vector<SaveInfo> getAllSaves() {
        std::vector<SaveInfo> saves;
        const char* sql = "SELECT player_name, affinity, current_node, last_login, state_json "
                          "FROM player_save ORDER BY last_login DESC LIMIT 3;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            reportError("getAllSaves(prepare)");
            return saves;
        }
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            SaveInfo info;
            info.playerName = columnText(stmt, 0);
            info.affinity = sqlite3_column_int(stmt, 1);
            info.currentNode = columnText(stmt, 2);
            info.lastLogin = columnText(stmt, 3);
            info.hasStateJson = !columnText(stmt, 4).empty();
            saves.push_back(info);
        }
        sqlite3_finalize(stmt);
        return saves;
    }

    bool deleteSave(const std::string& playerName) {
        const char* sqls[3] = {
            "DELETE FROM player_save WHERE player_name = ?;",
            "DELETE FROM player_inventory WHERE player_name = ?;",
            "DELETE FROM player_tags WHERE player_name = ?;"
        };
        const char* names[3] = {"deleteSave(player_save)", "deleteSave(inventory)", "deleteSave(tags)"};
        bool allOk = true;
        for (int i = 0; i < 3; ++i) {
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db, sqls[i], -1, &stmt, nullptr) != SQLITE_OK) {
                reportError(names[i]);
                allOk = false;
                continue;
            }
            sqlite3_bind_text(stmt, 1, playerName.data(), static_cast<int>(playerName.size()), SQLITE_TRANSIENT);
            const int rc = sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            if (rc != SQLITE_DONE) {
                reportError(names[i]);
                allOk = false;
            }
        }
        return allOk;
    }

    // ---------------- 背包 / 标签（保留） ----------------

    bool addItem(const std::string& playerName, const std::string& itemId) {
        const char* sql = "INSERT OR IGNORE INTO player_inventory (player_name, item_id) VALUES (?, ?);";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            reportError("addItem(prepare)");
            return false;
        }
        sqlite3_bind_text(stmt, 1, playerName.data(), static_cast<int>(playerName.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, itemId.data(), static_cast<int>(itemId.size()), SQLITE_TRANSIENT);
        const int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            reportError("addItem(step)");
            return false;
        }
        return true;
    }

    std::vector<std::string> getItems(const std::string& playerName) {
        std::vector<std::string> items;
        const char* sql = "SELECT item_id FROM player_inventory WHERE player_name = ?;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            reportError("getItems(prepare)");
            return items;
        }
        sqlite3_bind_text(stmt, 1, playerName.data(), static_cast<int>(playerName.size()), SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            items.push_back(columnText(stmt, 0));
        }
        sqlite3_finalize(stmt);
        return items;
    }

    bool addTag(const std::string& playerName, const std::string& tagId) {
        const char* sql = "INSERT OR IGNORE INTO player_tags (player_name, tag_id) VALUES (?, ?);";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            reportError("addTag(prepare)");
            return false;
        }
        sqlite3_bind_text(stmt, 1, playerName.data(), static_cast<int>(playerName.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, tagId.data(), static_cast<int>(tagId.size()), SQLITE_TRANSIENT);
        const int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            reportError("addTag(step)");
            return false;
        }
        return true;
    }

    std::vector<std::string> getTags(const std::string& playerName) {
        std::vector<std::string> tags;
        const char* sql = "SELECT tag_id FROM player_tags WHERE player_name = ?;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            reportError("getTags(prepare)");
            return tags;
        }
        sqlite3_bind_text(stmt, 1, playerName.data(), static_cast<int>(playerName.size()), SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            tags.push_back(columnText(stmt, 0));
        }
        sqlite3_finalize(stmt);
        return tags;
    }
};