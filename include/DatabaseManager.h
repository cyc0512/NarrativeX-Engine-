#pragma once
#include <iostream>
#include <string>
#include <vector>
#include <sqlite3.h>

// 数据访问层。
//
// R-11 修复要点（本次改动的全部动机）：
//   1. 所有带玩家输入的 SQL 一律走 sqlite3_prepare_v2 + sqlite3_bind_*，
//      不再用字符串拼接。旧代码把 playerName 直接拼进 VALUES ('" + name + "')，
//      于是 O'Brien 会让整条 INSERT 语法错误 → sqlite3_exec 返回错误 → 错误被完全忽略
//      → 玩家点了保存、以为存上了，实际一行都没写进去（静默丢档）。
//   2. sqlite3_column_text() 在列为 NULL 时返回 nullptr，
//      旧代码 `std::string(reinterpret_cast<const char*>(...))` 在 NULL 上是未定义行为
//      （实测 SIGSEGV）。这里统一走 columnText()，NULL 一律当空串。
//   3. 任何一步失败都**说出来**。旧代码的最大问题不是会被注入，
//      而是失败了还不吭声，让上层以为成功了。
class DatabaseManager {
private:
    sqlite3* db;

    // 统一的失败上报点。detail 为 nullptr 时取 sqlite3_errmsg。
    void reportError(const char* where, const char* detail = nullptr) {
        std::cerr << "【数据库错误】" << where << " 失败: "
                  << (detail ? detail : sqlite3_errmsg(db)) << std::endl;
    }

    // NULL 安全的文本列读取。
    // ⚠️ 绝不能把 sqlite3_column_text() 的返回值直接喂给 std::string：
    //    列为 NULL 时它是 nullptr，std::string(nullptr) 是未定义行为（崩溃候选之五）。
    static std::string columnText(sqlite3_stmt* stmt, int col) {
        const unsigned char* p = sqlite3_column_text(stmt, col);
        return p ? std::string(reinterpret_cast<const char*>(p)) : std::string();
    }

    // 不带参数的 DDL / 维护性 SQL（无外部输入，无需绑定）。
    bool execSql(const char* where, const char* sql) {
        char* errMsg = nullptr;
        if (sqlite3_exec(db, sql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
            reportError(where, errMsg);
            sqlite3_free(errMsg);
            return false;
        }
        return true;
    }

public:
    struct SaveInfo {
        std::string playerName;
        int affinity;
        std::string currentNode;
        std::string lastLogin;
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

        // 2. 检查并迁移旧表（PRAGMA 无外部输入，直接 prepare 即可）
        bool hasLastLogin = false;
        const char* checkSql = "PRAGMA table_info(player_save);";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, checkSql, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                if (columnText(stmt, 1) == "last_login") { hasLastLogin = true; break; }
            }
            sqlite3_finalize(stmt);
        } else {
            reportError("createTable(pragma)");
        }
        if (!hasLastLogin) {
            execSql("createTable(alter)", "ALTER TABLE player_save ADD COLUMN last_login DATETIME;");
            execSql("createTable(backfill)",
                    "UPDATE player_save SET last_login = CURRENT_TIMESTAMP WHERE last_login IS NULL;");
        }

        // 3. 玩家背包表
        execSql("createTable(player_inventory)",
                "CREATE TABLE IF NOT EXISTS player_inventory ("
                "player_name TEXT, "
                "item_id TEXT, "
                "PRIMARY KEY (player_name, item_id));");

        // 4. 玩家标签表
        execSql("createTable(player_tags)",
                "CREATE TABLE IF NOT EXISTS player_tags ("
                "player_name TEXT, "
                "tag_id TEXT, "
                "tag_type TEXT, "
                "PRIMARY KEY (player_name, tag_id));");
    }

    // 写档。返回 false 表示**没写进去**，上层必须知道（旧实现返回 void，等于把失败藏了）。
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

    std::vector<SaveInfo> getAllSaves() {
        std::vector<SaveInfo> saves;
        const char* sql = "SELECT player_name, affinity, current_node, last_login "
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
            info.lastLogin = columnText(stmt, 3);   // 可能是 NULL（旧档迁移前），现在是空串而不是崩
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
