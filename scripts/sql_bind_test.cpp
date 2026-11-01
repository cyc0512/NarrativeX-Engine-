// R-11 SQL 参数化验证（真实 DatabaseManager / 真实 GameEngine，非 mock）
//
// 两种跑法：
//   ./sql_bind_test            # 直接对 DatabaseManager 做 CRUD + 表完整性检查
//   ./sql_bind_test engine     # 走 GameEngine 的 新建/保存/读取/删除 全链路
//                              #  （需在沙箱目录下运行，见 scripts/run_sql_bind_test.sh）
//
// 被测的两个恶意玩家名：
//   O'Brien              —— 单引号会截断字符串字面量
//   '; DROP TABLE saves;-- —— 经典注入串
//
// 验收标准：
//   1) 两个名字都能完整走完 新建/保存/读取/删除，不崩、不报错；
//   2) 三张表（player_save / player_inventory / player_tags）在操作后依然存在；
//   3) 名字被原样保存和读回（不再被 SQL 语法吞掉）。
#include <cstdio>
#include <string>
#include <vector>

#include "DatabaseManager.h"
#include "GameEngine.h"

namespace {

const char* const kMaliciousNames[] = {
    "O'Brien",
    "'; DROP TABLE saves;--",
};

int g_fails = 0;

void Check(bool ok, const char* what) {
    std::printf("    [%s] %s\n", ok ? "OK" : "NG", what);
    if (!ok) ++g_fails;
}

// 用一条独立的 sqlite 连接去查表是否还在（不走被测代码，避免自证）
bool TableExists(const std::string& dbPath, const std::string& tableName) {
    sqlite3* conn = nullptr;
    if (sqlite3_open(dbPath.c_str(), &conn) != SQLITE_OK) {
        if (conn) sqlite3_close(conn);
        return false;
    }
    const std::string sql = "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name=?;";
    sqlite3_stmt* stmt = nullptr;
    bool exists = false;
    if (sqlite3_prepare_v2(conn, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, tableName.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) exists = (sqlite3_column_int(stmt, 0) > 0);
        sqlite3_finalize(stmt);
    }
    sqlite3_close(conn);
    return exists;
}

int RunDirect() {
    const std::string dbPath = "/tmp/fs_bind_test.db";
    std::remove(dbPath.c_str());

    std::printf("=== R-11 A：直接对 DatabaseManager 做 CRUD（DB=%s）===\n", dbPath.c_str());
    DatabaseManager db(dbPath);

    for (const char* name : kMaliciousNames) {
        std::printf("\n  --- 玩家名: [%s] ---\n", name);
        const std::string n(name);

        // 新建 / 保存
        db.saveData(n, 90, "0");
        std::printf("    saveData 已调用\n");

        // 读取
        int affinity = -1;
        std::string node;
        const bool loaded = db.loadData(n, affinity, node);
        Check(loaded, "loadData 能读回该玩家");
        Check(loaded && affinity == 90, "好感度读回正确(=90)");
        Check(loaded && node == "0", "节点读回正确(=0)");

        // 背包 / 标签
        db.addItem(n, "item_hairclip");
        std::vector<std::string> items = db.getItems(n);
        Check(items.size() == 1 && items[0] == "item_hairclip", "道具写入并读回");

        db.addTag(n, "tag_test");
        std::vector<std::string> tags = db.getTags(n);
        Check(tags.size() == 1 && tags[0] == "tag_test", "标签写入并读回");

        // 存档列表里能原样看到这个名字
        bool foundInList = false;
        for (const auto& s : db.getAllSaves()) {
            if (s.playerName == n) foundInList = true;
        }
        Check(foundInList, "getAllSaves 里名字被原样保存（未被 SQL 语法吞掉）");

        // 删除
        db.deleteSave(n);
        int affinityAfter = -1;
        std::string nodeAfter;
        Check(!db.loadData(n, affinityAfter, nodeAfter), "deleteSave 后读不到了");
    }

    std::printf("\n  --- 表完整性 ---\n");
    for (const char* t : {"player_save", "player_inventory", "player_tags"}) {
        Check(TableExists(dbPath, t), (std::string("表 ") + t + " 依然存在").c_str());
    }
    return g_fails;
}

int RunEngine(const std::string& dbPathForCheck) {
    std::printf("=== R-11 B：走 GameEngine 全链路（新建/保存/读取/删除）===\n");
    GameEngine engine("../data/story.json");
    if (!engine.ready()) {
        std::printf("    [NG] 引擎构造失败: %s\n", engine.lastError().c_str());
        return g_fails + 1;
    }

    for (const char* name : kMaliciousNames) {
        std::printf("\n  --- 玩家名: [%s] ---\n", name);
        const std::string n(name);

        engine.createNewPlayer(n);          // 新建 + 保存
        std::printf("    createNewPlayer 已调用\n");

        bool okRead = engine.loadExistingPlayer(n);
        Check(okRead, "loadExistingPlayer 读取成功");

        bool inList = false;
        for (const auto& s : engine.getAllSaves()) {
            if (s.playerName == n) inList = true;
        }
        Check(inList, "getAllSaves 中能找到该名字");

        engine.deleteSaveData(n);           // 删除
        Check(!engine.loadExistingPlayer(n), "deleteSaveData 后读取失败（已删除）");
    }

    std::printf("\n  --- 表完整性 ---\n");
    for (const char* t : {"player_save", "player_inventory", "player_tags"}) {
        Check(TableExists(dbPathForCheck, t), (std::string("表 ") + t + " 依然存在").c_str());
    }
    return g_fails;
}

}  // namespace

int main(int argc, char** argv) {
    const bool useEngine = (argc > 1 && std::string(argv[1]) == "engine");
    // B 段在沙箱目录里跑，实际库文件路径由第二个参数给出（默认 data/save.db）。
    const std::string checkPath = (argc > 2) ? std::string(argv[2]) : std::string("data/save.db");
    const int fails = useEngine ? RunEngine(checkPath) : RunDirect();
    std::printf("\n=========================================\n");
    if (fails == 0) {
        std::printf("结果: R-11 %s 全部通过\n", useEngine ? "GameEngine 链路" : "DatabaseManager 直测");
    } else {
        std::printf("结果: R-11 失败 %d 项\n", fails);
    }
    std::printf("=========================================\n");
    return fails == 0 ? 0 : 1;
}
