#pragma once
#include <iostream>
#include <string>
#include <vector> 
#include <sqlite3.h> 

class DatabaseManager {
private:
    sqlite3* db; 

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
        const char* sql = "CREATE TABLE IF NOT EXISTS player_save ("
                          "player_name TEXT PRIMARY KEY, "
                          "affinity INTEGER, "
                          "current_node TEXT, "
                          "last_login DATETIME DEFAULT CURRENT_TIMESTAMP);";
        char* errMsg = nullptr;
        sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);

        // 2. 检查并迁移旧表
        bool hasLastLogin = false;
        const char* checkSql = "PRAGMA table_info(player_save);";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, checkSql, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                std::string colName = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                if (colName == "last_login") { hasLastLogin = true; break; }
            }
            sqlite3_finalize(stmt);
        }
        if (!hasLastLogin) {
            sqlite3_exec(db, "ALTER TABLE player_save ADD COLUMN last_login DATETIME;", nullptr, nullptr, nullptr);
            sqlite3_exec(db, "UPDATE player_save SET last_login = CURRENT_TIMESTAMP WHERE last_login IS NULL;", nullptr, nullptr, nullptr);
        }

        // 3. 玩家背包表
        const char* sql_items = "CREATE TABLE IF NOT EXISTS player_inventory ("
                                "player_name TEXT, "
                                "item_id TEXT, "
                                "PRIMARY KEY (player_name, item_id));";
        sqlite3_exec(db, sql_items, nullptr, nullptr, nullptr);

        // 4. 玩家标签表
        const char* sql_tags = "CREATE TABLE IF NOT EXISTS player_tags ("
                               "player_name TEXT, "
                               "tag_id TEXT, "
                               "tag_type TEXT, "
                               "PRIMARY KEY (player_name, tag_id));";
        sqlite3_exec(db, sql_tags, nullptr, nullptr, nullptr);
    }

    void saveData(const std::string& playerName, int affinity, const std::string& currentNode) {
        std::string sql = "INSERT OR REPLACE INTO player_save (player_name, affinity, current_node, last_login) "
                          "VALUES ('" + playerName + "', " + std::to_string(affinity) + ", '" + currentNode + "', datetime('now', 'localtime'));";
        sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
    }

    bool loadData(const std::string& playerName, int& affinity, std::string& currentNode) {
        std::string sql = "SELECT affinity, current_node FROM player_save WHERE player_name = '" + playerName + "';";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                affinity = sqlite3_column_int(stmt, 0);
                currentNode = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                sqlite3_finalize(stmt);
                return true;
            }
            sqlite3_finalize(stmt);
        }
        return false;
    }

    std::vector<SaveInfo> getAllSaves() {
        std::vector<SaveInfo> saves;
        const char* sql = "SELECT player_name, affinity, current_node, last_login FROM player_save ORDER BY last_login DESC LIMIT 3;";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                SaveInfo info;
                info.playerName = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                info.affinity = sqlite3_column_int(stmt, 1);
                info.currentNode = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                info.lastLogin = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
                saves.push_back(info);
            }
            sqlite3_finalize(stmt);
        }
        return saves;
    }

    void deleteSave(const std::string& playerName) {
        std::string sql = "DELETE FROM player_save WHERE player_name = '" + playerName + "';";
        sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
        sql = "DELETE FROM player_inventory WHERE player_name = '" + playerName + "';";
        sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
        sql = "DELETE FROM player_tags WHERE player_name = '" + playerName + "';";
        sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
    }

    void addItem(const std::string& playerName, const std::string& itemId) {
        std::string sql = "INSERT OR IGNORE INTO player_inventory (player_name, item_id) VALUES ('" + playerName + "', '" + itemId + "');";
        sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
    }

    std::vector<std::string> getItems(const std::string& playerName) {
        std::vector<std::string> items;
        std::string sql = "SELECT item_id FROM player_inventory WHERE player_name = '" + playerName + "';";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                items.push_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
            }
            sqlite3_finalize(stmt);
        }
        return items;
    }

    void addTag(const std::string& playerName, const std::string& tagId) {
        std::string sql = "INSERT OR IGNORE INTO player_tags (player_name, tag_id) VALUES ('" + playerName + "', '" + tagId + "');";
        sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
    }

    std::vector<std::string> getTags(const std::string& playerName) {
        std::vector<std::string> tags;
        std::string sql = "SELECT tag_id FROM player_tags WHERE player_name = '" + playerName + "';";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                tags.push_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
            }
            sqlite3_finalize(stmt);
        }
        return tags;
    }
};