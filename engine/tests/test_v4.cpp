// ---------------------------------------------------------------------------
//  FluidStory v4 框架回归测试（纯 C++，基于 data/story_v4_example.json）
//
//  验证 v4 内核框架：三类节点解析、动态节点 init_after 武装闸门、可前置节点
//  insert_at 锚点闸门、trigger 二次闸门、以及"禁止回退"lint 不变量。
//
//  编译（header-only；工作目录须为 engine/tests，默认数据路径 ../../data）：
//    g++ -std=c++17 -Wall -Wextra -O1 -I../include -I../../include test_v4.cpp -o /tmp/fs_test_v4
//  运行：
//    cd engine/tests && /tmp/fs_test_v4 [path/to/story_v4_example.json]
// ---------------------------------------------------------------------------
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "nx.hpp"

using namespace nx;

namespace {
int g_fail = 0;
int g_pass = 0;

#define CHECK(cond, name)                                                     \
    do {                                                                      \
        if (cond) { ++g_pass; std::cout << "  [OK] " << name << "\n"; }       \
        else { ++g_fail; std::cout << "  [!!] " << name << "\n"; }            \
    } while (0)

std::string read_file(const std::string& p) {
    std::ifstream f(p);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool in_vec(const std::vector<NodeId>& v, const NodeId& id) {
    for (const auto& x : v) if (x == id) return true;
    return false;
}

// 用一串选项输入驱动引擎，返回最终引擎。
NarrativeEngine drive(const Catalog& cat, const std::string& entry,
                       const std::vector<std::string>& inputs) {
    GameState st = StoryLoader::new_state("v4test", 90, entry);
    NarrativeEngine eng(cat, std::move(st));
    for (const auto& in : inputs) eng.step(in);
    return eng;
}
}  // namespace

int main(int argc, char** argv) {
    const std::string path = (argc > 1) ? argv[1] : "../../data/story_v4_example.json";
    std::cout << "=== v4 框架回归：加载 " << path << " ===\n";

    std::string text;
    try {
        text = read_file(path);
    } catch (...) {
        std::cout << "  [!!] 无法读取文件：" << path << "\n";
        return 1;
    }

    Catalog cat;
    try {
        cat = StoryLoader::load(text);
    } catch (const NxError& e) {
        std::cout << "  [!!] 剧本加载失败：" << e.what() << "\n";
        return 1;
    }
    CHECK(true, "story_v4_example.json 通过 load() 校验（lint 无错）");

    // ---- 1. 节点三分类解析 ----
    const NodeSpec* p_hidden = cat.find("n_hidden_smother");
    const NodeSpec* p_letter = cat.find("n_letter");
    const NodeSpec* p_entry  = cat.find("n1_entry");
    CHECK(p_hidden != nullptr && p_hidden->ntype == NodeType::Dynamic,
          "n_hidden_smother 解析为 Dynamic");
    CHECK(p_letter != nullptr && p_letter->ntype == NodeType::Preposable,
          "n_letter 解析为 Preposable");
    CHECK(p_entry  != nullptr && p_entry->ntype == NodeType::Fixed,
          "n1_entry 解析为 Fixed（缺省）");
    CHECK(p_hidden->init_after == "n_boss_call", "n_hidden_smother.init_after == n_boss_call");
    CHECK(p_hidden->trigger.tags_all.size() == 2
          && p_hidden->trigger.tags_all[0] == "numbness"
          && p_hidden->trigger.tags_all[1] == "heartbroken",
          "n_hidden_smother.trigger.tags_all == [numbness, heartbroken]");
    CHECK(p_letter->insert_at.size() == 2
          && in_vec(p_letter->insert_at, "n4_silence")
          && in_vec(p_letter->insert_at, "n_client_meal"),
          "n_letter.insert_at == [n4_silence, n_client_meal]");
    CHECK(p_entry->timeline == 1, "n1_entry.timeline == 1");

    // ---- 2. 动态节点：未到锚点 => Unarmed，且不进 Frontier ----
    {
        NarrativeEngine eng = drive(cat, "n1_entry", {});  // 停在入口
        auto blocked = eng.blocked_map();
        auto fin = eng.frontier_in_ids();
        CHECK(!in_vec(fin, "n_hidden_smother"),
              "起点：n_hidden_smother 不在 Frontier（未武装）");
        auto it = blocked.find("unarmed");
        CHECK(it != blocked.end() && in_vec(it->second, "n_hidden_smother"),
              "起点：n_hidden_smother 被 unarmed 拦截（init_after 未到）");
    }

    // ---- 3. 动态节点：到过锚点 => 武装；trigger 未满足 => Trigger ----
    {
        // 走到 n_boss_call：n1_entry(1) -> n2_kind(1) -> n_boss_call
        NarrativeEngine eng = drive(cat, "n1_entry", {"1", "1"});
        auto blocked = eng.blocked_map();
        auto fin = eng.frontier_in_ids();
        CHECK(!in_vec(fin, "n_hidden_smother"),
              "到 n_boss_call：n_hidden_smother 仍不在 Frontier（trigger 缺 heartbroken）");
        auto it = blocked.find("unarmed");
        CHECK(it == blocked.end() || !in_vec(it->second, "n_hidden_smother"),
              "到 n_boss_call：n_hidden_smother 不再 unarmed（已武装）");
        auto trg = blocked.find("trigger");
        CHECK(trg != blocked.end() && in_vec(trg->second, "n_hidden_smother"),
              "到 n_boss_call：n_hidden_smother 被 trigger 拦截（缺 heartbroken）");
    }

    // ---- 4. 动态节点：武装 + trigger 满足 => 进入 Frontier（Ok）----
    {
        NarrativeEngine eng = drive(cat, "n1_entry", {"1", "1"});
        eng.mutable_state().tags.insert("heartbroken");  // 补齐 trigger
        auto fin = eng.frontier_in_ids();
        CHECK(in_vec(fin, "n_hidden_smother"),
              "补齐 heartbroken 后：n_hidden_smother 进入 Frontier（Ok）");
    }

    // ---- 5. 可前置节点：非锚点 => GroupLocked ----
    {
        NarrativeEngine eng = drive(cat, "n1_entry", {"1"});  // 停在 n2_kind（非锚点）
        auto blocked = eng.blocked_map();
        auto fin = eng.frontier_in_ids();
        CHECK(!in_vec(fin, "n_letter"), "非锚点 n2_kind：n_letter 不在 Frontier");
        auto gl = blocked.find("group_locked");
        CHECK(gl != blocked.end() && in_vec(gl->second, "n_letter"),
              "非锚点 n2_kind：n_letter 被 group_locked 拦截");
    }

    // ---- 6. 可前置节点：命中锚点但 trigger 未满足 => Trigger ----
    {
        // 走到 n_client_meal：n1_entry(1)->n2_kind(1)->n_boss_call(1)->n_client_meal
        NarrativeEngine eng = drive(cat, "n1_entry", {"1", "1", "1"});
        auto blocked = eng.blocked_map();
        auto fin = eng.frontier_in_ids();
        CHECK(!in_vec(fin, "n_letter"),
              "锚点 n_client_meal：n_letter 仍不在 Frontier（trigger 缺 lonely）");
        auto gl = blocked.find("group_locked");
        CHECK(gl == blocked.end() || !in_vec(gl->second, "n_letter"),
              "锚点 n_client_meal：n_letter 不再 group_locked（已命中锚点）");
        auto trg = blocked.find("trigger");
        CHECK(trg != blocked.end() && in_vec(trg->second, "n_letter"),
              "锚点 n_client_meal：n_letter 被 trigger 拦截（缺 lonely）");
    }

    // ---- 7. 可前置节点：命中锚点 + trigger 满足 => 进入 Frontier（Ok）----
    {
        NarrativeEngine eng = drive(cat, "n1_entry", {"1", "1", "1"});
        eng.mutable_state().tags.insert("lonely");
        auto fin = eng.frontier_in_ids();
        CHECK(in_vec(fin, "n_letter"),
              "补齐 lonely 后：n_letter 在锚点 n_client_meal 进入 Frontier（Ok）");
    }

    // ---- 8. 禁止回退 lint：动态节点指向更早时间线应被 fail-fast ----
    {
        const std::string bad =
            R"({"phases":["evening"],"entry":"a","nodes":{)"
            R"("a":{"type":"fixed","kind":"scene","timeline":1,"presentation":{"npc_text":"a"},"intent":{"description":"a"},"options":[{"text":"go","next":"b"}]},)"
            R"("b":{"type":"fixed","kind":"scene","timeline":2,"presentation":{"npc_text":"b"},"intent":{"description":"b"},"options":[{"text":"go","next":"c"}]},)"
            R"("c":{"type":"fixed","kind":"scene","timeline":3,"presentation":{"npc_text":"c"},"intent":{"description":"c"}},)"
            R"("d":{"type":"dynamic","kind":"scene","timeline":5,"init_after":"b","default_next":"a","presentation":{"npc_text":"d"},"intent":{"description":"d"}})"
            R"(}})";
        bool threw = false;
        std::string msg;
        try {
            auto c = StoryLoader::load(bad);
            (void)c;
        } catch (const NxError& e) {
            threw = true;
            msg = e.what();
        }
        CHECK(threw && msg.find("禁止回退") != std::string::npos,
              "动态节点 d 指向更早时间线 a，被 lint 判为违反禁止回退并 fail-fast");
    }

    // ---- 9. 禁止回退同样约束固定骨架：固定节点指向更早时间线也应 fail-fast ----
    {
        const std::string bad =
            R"({"phases":["evening"],"entry":"a","nodes":{)"
            R"("a":{"type":"fixed","kind":"scene","timeline":1,"presentation":{"npc_text":"a"},"intent":{"description":"a"},"options":[{"text":"go","next":"b"}]},)"
            R"("b":{"type":"fixed","kind":"scene","timeline":2,"presentation":{"npc_text":"b"},"intent":{"description":"b"},"options":[{"text":"go","next":"c"}]},)"
            R"("c":{"type":"fixed","kind":"scene","timeline":3,"presentation":{"npc_text":"c"},"intent":{"description":"c"}},)"
            R"("d":{"type":"fixed","kind":"scene","timeline":5,"default_next":"a","presentation":{"npc_text":"d"},"intent":{"description":"d"}})"
            R"(}})";
        bool threw = false;
        std::string msg;
        try {
            auto c = StoryLoader::load(bad);
            (void)c;
        } catch (const NxError& e) {
            threw = true;
            msg = e.what();
        }
        CHECK(threw && msg.find("禁止回退") != std::string::npos,
              "固定节点 d 指向更早时间线 a，同样被 lint 判为违反禁止回退（骨架向前）");
    }

    std::cout << "\n结果: " << g_pass << " 通过 / " << g_fail << " 失败\n";
    return g_fail == 0 ? 0 : 1;
}
