#pragma once
#include <iostream>
#include <string>
#include <sqlite3.h> // 引入刚刚装好的 SQLite 库

class DatabaseManager {
private:
    sqlite3* db; // 数据库的“遥控器”

public:
    // 构造函数：打开数据库文件，如果没有就自动创建
    DatabaseManager(const std::string& dbPath) {
        int rc = sqlite3_open(dbPath.c_str(), &db);
        if (rc) {
            std::cerr << "【数据库错误】无法打开: " << sqlite3_errmsg(db) << std::endl;
            db = nullptr;
            exit(1);
        } else {
            std::cout << "【数据库】成功连接: " << dbPath << std::endl;
            createTable();
        }
    }

    // 析构函数：程序关闭时，自动断开数据库
     ~DatabaseManager() {
        if (db) {
            sqlite3_close(db);
            std::cout << "【数据库】数据库连接已关闭。" << std::endl;
        }
    }

    // 建表（如果不存在就创建）
    void createTable() {
        const char* sql = "CREATE TABLE IF NOT EXISTS player_save ("
                          "player_name TEXT PRIMARY KEY, " // 玩家名作为主键，防重复
                          "affinity INTEGER, "            // 好感度
                          "current_node TEXT);";          // 当前剧情节点
        
        char* errMsg = nullptr;
        if (sqlite3_exec(db, sql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
            std::cerr << "【建表错误】" << errMsg << std::endl;
            sqlite3_free(errMsg);
              exit(1); 
        }
    }

    void saveData(const std::string& playerName, int affinity, const std::string& currentNode) {
        // 使用 INSERT OR REPLACE：主键存在则更新，不存在则插入
        std::string sql = "INSERT OR REPLACE INTO player_save (player_name, affinity, current_node) "
                          "VALUES ('" + playerName + "', " + std::to_string(affinity) + ", '" + currentNode + "');";
        
        char* errMsg = nullptr;
        if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errMsg) != SQLITE_OK) {
            // 存档失败不一定导致程序崩溃，所以只打印警告，不 exit(1)
            std::cerr << "【数据库警告】存档失败: " << errMsg << std::endl;
            sqlite3_free(errMsg);
        }
    }

    // 读档：根据玩家名字，把好感度和节点读出来
    bool loadData(const std::string& playerName, int& affinity, std::string& currentNode) {
        std::string sql = "SELECT affinity, current_node FROM player_save WHERE player_name = '" + playerName + "';";
        
        sqlite3_stmt* stmt; // Statement 对象，用于处理查询结果
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            // 如果 sqlite3_step 返回 ROW，说明查到了数据
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                affinity = sqlite3_column_int(stmt, 0); // 拿第 0 列（好感度）
                const unsigned char* text = sqlite3_column_text(stmt, 1); // 拿第 1 列（节点）
                currentNode = reinterpret_cast<const char*>(text);
                
                sqlite3_finalize(stmt); // 释放 statement 资源
                return true;
            }
            sqlite3_finalize(stmt);
        }
        return false; // 没查到，说明是新玩家
    }
};