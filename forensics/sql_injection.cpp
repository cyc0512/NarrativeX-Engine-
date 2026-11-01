// forensics/sql_injection.cpp
//
// R-11 回归：玩家名 / 道具名 / 标签名里的引号与注入串，必须能被正常存取，
// 且绝不能影响表结构。同一份测试分别编译两次：
//
//   # 修复前（旧实现，字符串拼接 + 忽略 sqlite3 错误）
//   g++ -std=gnu++17 -g -O1 -Iforensics/db_prefix forensics/sql_injection.cpp \
//       -o build/r11_before -lsqlite3
//
//   # 修复后（prepare + bind + NULL 保护 + 失败上报）
//   g++ -std=gnu++17 -g -O1 -Iinclude forensics/sql_injection.cpp \
//       -o build/r11_after -lsqlite3
//
// 变体由 saveData 的返回类型自动判定：旧实现返回 void（压根不告诉你失败了），
// 新实现返回 bool。旧实现下"保存是否成功"只能靠随后读回来反推 ——
// 这恰恰就是"静默丢档"的定义。
//
// 另含一条崩溃候选（sqlite3_column_text 返回 NULL）的对照用例。

#include <sqlite3.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <type_traits>
#include <vector>

#include "DatabaseManager.h"

namespace {

int g_pass = 0;
int g_fail = 0;

void report(bool ok, const std::string& name, const std::string& detail = "") {
    if (ok) {
        std::printf("  [PASS] %s\n", name.c_str());
        ++g_pass;
    } else {
        std::printf("  [FAIL] %s%s%s\n", name.c_str(),
                    detail.empty() ? "" : "  :: ", detail.c_str());
        ++g_fail;
    }
    std::fflush(stdout);
}

// 旧实现的 saveData 返回 void，新实现返回 bool。这里统一成 bool：
// 旧实现只能靠"随后能不能读回来"来判定，而这正是玩家真正感知到的事实。
template <class DB>
bool saveAndReport(DB& db, const std::string& name, int affinity, const std::string& node) {
    using Ret = decltype(db.saveData(name, affinity, node));
    if constexpr (std::is_same_v<Ret, void>) {
        db.saveData(name, affinity, node);
        int back = -1;
        std::string backNode;
        return db.loadData(name, back, backNode) && back == affinity && backNode == node;
    } else {
        return db.saveData(name, affinity, node);
    }
}

// 造一个 last_login 为 NULL 的旧档 —— 这是"崩溃候选之五"的触发素材。
// 迁移路径上真实存在：ALTER TABLE 加出来的列没有默认值，回填之前就是 NULL。
void makeNullLoginDb(const std::string& path) {
    std::remove(path.c_str());
    sqlite3* h = nullptr;
    if (sqlite3_open(path.c_str(), &h) != SQLITE_OK) {
        std::printf("  [FAIL] 无法创建对照数据库\n");
        return;
    }
    sqlite3_exec(h,
                 "CREATE TABLE player_save (player_name TEXT PRIMARY KEY, "
                 "affinity INTEGER, current_node TEXT, last_login DATETIME);",
                 nullptr, nullptr, nullptr);
    sqlite3_exec(h,
                 "INSERT INTO player_save (player_name, affinity, current_node, last_login) "
                 "VALUES ('旧档玩家', 80, '1', NULL);",
                 nullptr, nullptr, nullptr);
    sqlite3_close(h);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mainDb = (argc > 1) ? argv[1] : "r11_probe.db";
    const std::string nullDb = mainDb + ".null";

    std::remove(mainDb.c_str());

    DatabaseManager probe("__variant_probe__.db");
    using Ret = decltype(probe.saveData(std::string(), 0, std::string()));
    const bool isOldImpl = std::is_same_v<Ret, void>;
    std::remove("__variant_probe__.db");

    std::printf("=== R-11 SQL 参数化回归（变体：%s）===\n",
                isOldImpl ? "修复前 BEFORE" : "修复后 AFTER");
    std::printf("  数据库: %s\n\n", mainDb.c_str());
    std::fflush(stdout);

    DatabaseManager db(mainDb);

    // ---------------------------------------------------------------------
    // 用例 1：带单引号的普通玩家名（验收点里的 O'Brien）
    // ---------------------------------------------------------------------
    const std::string name1 = "O'Brien";
    report(saveAndReport(db, name1, 90, "1"), "O'Brien 可以写入",
           isOldImpl ? "旧实现：saveData 返回 void，只能靠读回验证；读不到就说明是静默丢档" : "");

    int aff = -1;
    std::string node;
    bool loaded = db.loadData(name1, aff, node);
    report(loaded && aff == 90 && node == "1", "O'Brien 可以读回",
           loaded ? ("affinity=" + std::to_string(aff) + " node=" + node) : "读不回来");

    // ---------------------------------------------------------------------
    // 用例 2：注入串 A（验收点里的那一条）
    // 旧实现下这条会拼成语法错误的 SQL，第一条语句就失败，后面的 DROP 根本没机会执行
    // —— 所以"表还在"是运气，不是安全。
    // ---------------------------------------------------------------------
    const std::string injectionA = "'; DROP TABLE player_save;--";
    report(saveAndReport(db, injectionA, 70, "end_1"), "注入串 A 可以写入");

    int aff1b = -1;
    std::string node1b;
    report(db.loadData(name1, aff1b, node1b), "注入串 A 之后 player_save 表还在");

    // ---------------------------------------------------------------------
    // 用例 3：注入串 B —— 一条**在旧实现下真正能生效**的注入。
    // 它先把单引号闭合掉，让第一条 INSERT 合法，再追加一条 DROP TABLE。
    // sqlite3_exec 会顺序执行这两条语句，于是表真的没了。
    // ---------------------------------------------------------------------
    const std::string injectionB = "x', 1, 'y'); DROP TABLE player_save; --";
    report(saveAndReport(db, injectionB, 60, "end_3"), "注入串 B 可以写入（作为普通玩家名）");

    int aff1c = -1;
    std::string node1c;
    const bool tableAlive = db.loadData(name1, aff1c, node1c);
    report(tableAlive, "注入串 B 之后 player_save 表仍在（表结构未被 SQL 改变）",
           tableAlive ? "" : "player_save 已被 DROP —— 存档全没了");

    // ---------------------------------------------------------------------
    // 用例 4：道具 / 标签里的引号
    // ---------------------------------------------------------------------
    db.addItem(name1, "item_hair'clip");
    db.addTag(name1, "tag_'regret'");
    std::vector<std::string> items = db.getItems(name1);
    std::vector<std::string> tags = db.getTags(name1);
    report(items.size() == 1 && items[0] == "item_hair'clip", "含引号的道具名可存可取",
           items.empty() ? "一个都没取回" : items[0]);
    report(tags.size() == 1 && tags[0] == "tag_'regret'", "含引号的标签名可存可取",
           tags.empty() ? "一个都没取回" : tags[0]);

    // ---------------------------------------------------------------------
    // 用例 5：删除（注入串作为主键）
    // ---------------------------------------------------------------------
    db.deleteSave(injectionA);
    int affA = -1;
    std::string nodeA;
    report(!db.loadData(injectionA, affA, nodeA), "deleteSave(注入串) 能删掉自己");
    int aff1d = -1;
    std::string node1d;
    report(db.loadData(name1, aff1d, node1d), "deleteSave(注入串) 没有误伤其他玩家");

    // ---------------------------------------------------------------------
    // 用例 6（崩溃候选之五）：last_login 为 NULL 的旧档
    // 旧实现 info.lastLogin = std::string(reinterpret_cast<const char*>(nullptr)) → 未定义行为
    // ---------------------------------------------------------------------
    std::printf("\n  --- 崩溃候选：last_login 为 NULL 的旧档 ---\n");
    std::fflush(stdout);
    makeNullLoginDb(nullDb);
    {
        DatabaseManager oldSaveDb(nullDb);
        std::printf("  准备读 NULL 档...\n");
        std::fflush(stdout);
        std::vector<DatabaseManager::SaveInfo> saves = oldSaveDb.getAllSaves();
        report(saves.size() == 1 && saves[0].playerName == "旧档玩家" && saves[0].lastLogin.empty(),
               "last_login 为 NULL 时读成空串而不是崩溃",
               saves.empty() ? "一条都没读出" : ("lastLogin=[" + saves[0].lastLogin + "]"));
    }

    std::printf("\n=========================================\n");
    std::printf("结果: %d 项通过, %d 项失败（变体：%s）\n", g_pass, g_fail,
                isOldImpl ? "修复前" : "修复后");
    std::printf("=========================================\n");
    std::fflush(stdout);
    return g_fail == 0 ? 0 : 1;
}
