#pragma once
#include <iostream>
#include <string>
#include <vector> // 必须加这个
#include <sqlite3.h> 

class DatabaseManager {
private:
    sqlite3* db; 

public:
    // 新增：用来装存档信息的结构体
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
        // 1. 先尝试按最新版本的结构建表（如果是全新玩家，直接成功）
        const char* sql = "CREATE TABLE IF NOT EXISTS player_save ("
                          "player_name TEXT PRIMARY KEY, "
                          "affinity INTEGER, "
                          "current_node TEXT, "
                          "last_login DATETIME DEFAULT CURRENT_TIMESTAMP);";
        
        char* errMsg = nullptr;
        if (sqlite3_exec(db, sql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
            std::cerr << "【建表错误】" << errMsg << std::endl;
            sqlite3_free(errMsg);
            return;
        }

        // 2. 核心检测逻辑：查询当前表里到底有没有 last_login 这个字段
        // 如果表是以前建的（没有时间字段），这句代码会返回 false
        bool hasLastLogin = false;
        const char* checkSql = "PRAGMA table_info(player_save);"; // SQLite 专门用来查表结构的命令
        sqlite3_stmt* stmt;
        
        if (sqlite3_prepare_v2(db, checkSql, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                // 第 1 列是字段名
                std::string colName = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                if (colName == "last_login") {
                    hasLastLogin = true;
                    break;
                }
            }
            sqlite3_finalize(stmt);
        }

        // 3. 如果发现没有这个字段，说明是老玩家的旧库。自动执行热更新！
        if (!hasLastLogin) {
            std::cout << "【数据库升级】检测到老版本存档，正在自动添加时间字段..." << std::endl;
            
            // 第一步：先添加一个没有默认值的空列（SQLite 允许这么做）
            const char* alterSql1 = "ALTER TABLE player_save ADD COLUMN last_login DATETIME;";
            if (sqlite3_exec(db, alterSql1, nullptr, nullptr, &errMsg) == SQLITE_OK) {
                
                // 第二步：把旧数据的时间全部填上当前时间
                const char* alterSql2 = "UPDATE player_save SET last_login = CURRENT_TIMESTAMP WHERE last_login IS NULL;";
                if (sqlite3_exec(db, alterSql2, nullptr, nullptr, &errMsg) == SQLITE_OK) {
                    std::cout << "【数据库升级】升级成功！旧存档数据已保留并填充时间。" << std::endl;
                } else {
                    std::cerr << "【数据库升级失败-填充时间】" << errMsg << std::endl;
                    sqlite3_free(errMsg);
                }
            } else {
                std::cerr << "【数据库升级失败-加列】" << errMsg << std::endl;
                sqlite3_free(errMsg);
            }
        }
        const char* fixTimeSql = "UPDATE player_save SET last_login = datetime('now', 'localtime') WHERE last_login IS NULL OR last_login = '';";
        sqlite3_exec(db, fixTimeSql, nullptr, nullptr, nullptr);
    }

      void saveData(const std::string& playerName, int affinity, const std::string& currentNode) {
        // 核心：在插入/替换时，强行把 last_login 设置为当前时间
        std::string sql = "INSERT OR REPLACE INTO player_save (player_name, affinity, current_node, last_login) "
                          "VALUES ('" + playerName + "', " + std::to_string(affinity) + ", '" + currentNode + "', datetime('now', 'localtime'));";
        
        char* errMsg = nullptr;
        if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errMsg) != SQLITE_OK) {
            std::cerr << "【数据库存档错误】 " << errMsg << std::endl;
            sqlite3_free(errMsg);
        }
    }

    bool loadData(const std::string& playerName, int& affinity, std::string& currentNode) {
        std::string sql = "SELECT affinity, current_node FROM player_save WHERE player_name = '" + playerName + "';";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                affinity = sqlite3_column_int(stmt, 0);
                const unsigned char* text = sqlite3_column_text(stmt, 1);
                currentNode = reinterpret_cast<const char*>(text);
                sqlite3_finalize(stmt);
                return true;
            }
            sqlite3_finalize(stmt);
        }
        return false;
    }

    // 新增：一次性获取最多 3 个存档
       std::vector<SaveInfo> getAllSaves() {
        std::vector<SaveInfo> saves;
        // 0: player_name, 1: affinity, 2: current_node, 3: last_login
        const char* sql = "SELECT player_name, affinity, current_node, last_login FROM player_save ORDER BY last_login DESC LIMIT 3;";
        sqlite3_stmt* stmt;
        
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                SaveInfo info;
                info.playerName = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                info.affinity = sqlite3_column_int(stmt, 1);
                info.currentNode = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                
             
                const unsigned char* timeText = sqlite3_column_text(stmt, 3);
                if (timeText != nullptr) {
                    info.lastLogin = reinterpret_cast<const char*>(timeText);
                } else {
                    info.lastLogin = "未知时间";
                }
                
                saves.push_back(info);
            }
            sqlite3_finalize(stmt);
        } else {
            // 如果 SQL 语句错了，打印错误看看
            std::cerr << "【查询失败】" << sqlite3_errmsg(db) << std::endl;
        }
        return saves;
    }

    void deleteSave(const std::string& playerName) {
        std::string sql = "DELETE FROM player_save WHERE player_name = '" + playerName + "';";
        sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
    }
};