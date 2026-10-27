#pragma once
#include <iostream>
#include <string>
#include <sqlite3.h> 

class DatabaseManager {
private:
    sqlite3* db; 

public:
    DatabaseManager(const std::string& dbPath) {
        int rc = sqlite3_open(dbPath.c_str(), &db);
        if (rc) {
            std::cerr << "【错误】无法打开数据库: " << sqlite3_errmsg(db) << std::endl;
            db = nullptr;
            exit(1); 
        } else {
            std::cout << "【数据库】成功连接数据库: " << dbPath << std::endl;
            createTable(); 
        }
    }

    ~DatabaseManager() {
        if (db) {
            sqlite3_close(db);
        }
    }

    void createTable() {
        const char* sql = "CREATE TABLE IF NOT EXISTS player_save ("
                          "player_name TEXT PRIMARY KEY, "
                          "affinity INTEGER, "
                          "current_node TEXT);";
        char* errMsg = nullptr;
        if (sqlite3_exec(db, sql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
            std::cerr << "【建表错误】" << errMsg << std::endl;
            sqlite3_free(errMsg);
            exit(1); 
        }
    }

    void saveData(const std::string& playerName, int affinity, const std::string& currentNode) {
        std::string sql = "INSERT OR REPLACE INTO player_save (player_name, affinity, current_node) "
                          "VALUES ('" + playerName + "', " + std::to_string(affinity) + ", '" + currentNode + "');";
        sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
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

    // 👇 补全这两个被搞丢的功能
    void listAllSaves() {
        const char* sql = "SELECT player_name, affinity, current_node FROM player_save;";
        sqlite3_stmt* stmt;
        
        std::cout << "\n==================================\n";
        std::cout << "          📁 存档列表\n";
        std::cout << "==================================\n";
        
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            bool found = false;
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                found = true;
                std::string name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                int affinity = sqlite3_column_int(stmt, 1);
                std::string node = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                std::cout << " 玩家: " << name << " | 好感度: " << affinity << " | 进度: 节点 " << node << "\n";
            }
            if (!found) std::cout << "  (暂无存档)\n";
            sqlite3_finalize(stmt);
        }
        std::cout << "==================================\n";
    }

    void deleteSave(const std::string& playerName) {
        std::string sql = "DELETE FROM player_save WHERE player_name = '" + playerName + "';";
        if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK) {
            std::cout << "【数据库】玩家 " << playerName << " 的存档已成功删除。\n";
        }
    }
};