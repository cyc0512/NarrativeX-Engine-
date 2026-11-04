// forensics/d5_probe.cpp
//
// D-5（db 空指针 / playerName 为空）的排除性验证：
// 构造"未 createNewPlayer 就读档 / 直接自由输入 / 剧本加载失败"这三条路径，
// 看它们到底会不会崩。跑法：
//   g++ -std=gnu++17 -g -O1 -Iinclude forensics/d5_probe.cpp -o build/d5_probe -lsqlite3 -lcurl
//   cd build && ./d5_probe
#include <cstdio>
#include <string>
#include "GameEngine.h"

int main() {
    std::printf("=== D-5 排除性验证 ===\n");

    // ① 剧本加载失败（路径瞎写）→ db 会是 nullptr，看后续操作会不会炸
    {
        GameEngine broken("../data/根本不存在的剧本.json");
        std::printf("① 剧本不存在: ready=%d, lastError=%s\n",
                    broken.ready() ? 1 : 0, broken.lastError().c_str());
        std::string r = broken.processInput("随便说点什么");
        std::printf("   processInput 返回: %s\n", r.c_str());
        std::printf("   getAllSaves().size()=%zu\n", broken.getAllSaves().size());
        broken.applyPenalty(-5);
        broken.deleteSaveData("谁");
        std::printf("   [观察] 全程未崩溃\n");
    }

    // ② 正常加载，但**从不调用 createNewPlayer**（playerName 为空），读一个不存在的档
    {
       EngineAdapter engine("../data/story_v2.json");
        std::printf("\n② 正常构造: ready=%d\n", engine.ready() ? 1 : 0);
        bool loaded = engine.loadExistingPlayer("根本不存在的玩家");
        std::printf("   loadExistingPlayer(不存在)=%d（应为 0）\n", loaded ? 1 : 0);
        std::printf("   playerName 为空时 processInput(\"1\") -> %s\n",
                    engine.processInput("1").c_str());
        std::printf("   playerName 为空时自由输入（走 AI）-> %s\n",
                    engine.processInput("你口袋里是什么？").substr(0, 40).c_str());
        std::printf("   applyPenalty(-20) 后 node=%s\n", engine.getCurrentNode().c_str());
        std::printf("   [观察] 全程未崩溃（空 playerName 只会写进一条空名存档，不会炸）\n");
    }

    std::printf("\n=== D-5 结论：以上两条路径均不崩溃 ===\n");
    return 0;
}
