// ---------------------------------------------------------------------------
//  标签「按存档持久化 / 每个存档各自独立」真实运行期 C++ 用例
//
//  验证对象：src/EngineAdapter.cpp::persist() 在序列化前把私有 all_tags_
//  全量反射进 engine_->mutable_state().tags（先 clear 再 insert）。
//
//  为什么必须用 addTag() 注入标签（关键）：
//    - effects.add_tags 会同时写 GameState.tags（NarrativeEngine::step 第 3 步）
//      与 all_tags_（EngineAdapter::step 第 6 步），因此只靠剧本标签无法区分
//      修复前后。
//    - LLM 情绪标签走 detectTag() -> addTag() -> all_tags_，只落在 all_tags_、
//      从不写 GameState.tags，才是唯一能暴露缺陷的输入。detectTag() 需要真实
//      LLM，离线不可达；public addTag() 是它唯一的副作用入口，故用 addTag()
//      精确模拟该路径，制造「只在 all_tags_」的标签。
//
//  纯数字 step("1")：isNumeric=true -> 跳过 detectTag() / 动态调度 / 拓扑锁，
//  台词取 n->npc_text，全程不调用 AiEngine::callLlm（无网络）。
//
//  工作目录必须是项目根：EngineAdapter 的 ../data/save.db 经 resolveDataPath()
//  回退到 data/save.db（项目根下真实存在的库），从而既不污染也不新建库。
//
//  编译（脚本里的命令，C++17 全量编译链接）：
//    g++ -std=c++17 -Wall -Wextra -O1 -Isrc -Iinclude -Iengine/include
//        -Ipersistence/include test_tags_persist.cpp
//        src/AiSelector.cpp src/EngineAdapter.cpp
//        -o /tmp/fs_test_tags_persist -lsqlite3 -lcurl -lpthread
//  C++14 语法门禁：同参数 -fsyntax-only（本文件不含 C++17 专属语法）。
//
//  输出：逐条 "  [OK] ..." / "  [FAIL] ..."，末尾且仅末尾一条
//  "结果: N/N 项通过"。任何失败返回 1。
// ---------------------------------------------------------------------------
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "EngineAdapter.h"

namespace {
int g_pass = 0;
int g_fail = 0;

#define CHECK(cond, name)                                                     \
    do {                                                                      \
        if (cond) {                                                           \
            ++g_pass;                                                         \
            std::cout << "  [OK] " << (name) << "\n";                         \
        } else {                                                              \
            ++g_fail;                                                         \
            std::cout << "  [FAIL] " << (name) << "\n";                       \
        }                                                                     \
    } while (0)

std::string stamp() {
    std::ostringstream os;
    os << static_cast<long long>(std::time(0)) << "_"
       << static_cast<long long>(std::clock());
    return os.str();
}

std::string tags_to_string(std::vector<std::string> v) {
    std::sort(v.begin(), v.end());
    std::string s = "{";
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) s += ", ";
        s += v[i];
    }
    s += "}";
    return s;
}

// 自包含最小剧本：不依赖 data/story_v2.json（另一个 worker 正在改写它）。
// lint 要求：entry 节点存在、至少一个 kind=fallback、每个非 fallback 节点
// 有非空 intent.description、所有 next 目标存在。
const char* kStory = R"fsstory({
  "phases": ["evening", "night", "late_night"],
  "entry": "n1_entry",
  "nodes": {
    "n1_entry": {
      "kind": "scripted",
      "once": false,
      "priority": 100,
      "phase": { "allow": ["evening", "night", "late_night"] },
      "presentation": { "narrator": "开场", "npc_text": "哥哥。", "npc_state": "平静", "hint": "继续" },
      "options": [ { "text": "继续", "next": "n_add" } ],
      "default_next": "n_add",
      "intent": { "description": "开场：继续进入写入标签节点。" }
    },
    "n_add": {
      "kind": "scripted",
      "once": false,
      "priority": 60,
      "phase": { "allow": ["evening", "night", "late_night"] },
      "presentation": { "narrator": "写标签", "npc_text": "已写入。", "npc_state": "平静" },
      "options": [ { "text": "继续", "next": "n_remove" } ],
      "default_next": "n_remove",
      "effects": { "add_tags": ["script_tag_alpha"] },
      "intent": { "description": "写入脚本标签，并可继续进入删除标签节点。" }
    },
    "n_remove": {
      "kind": "scripted",
      "once": false,
      "priority": 60,
      "phase": { "allow": ["evening", "night", "late_night"] },
      "presentation": { "narrator": "删标签", "npc_text": "已删除。", "npc_state": "平静" },
      "options": [ { "text": "继续", "next": "n_add" } ],
      "default_next": "n_add",
      "effects": { "remove_tags": ["script_tag_alpha"] },
      "intent": { "description": "删除脚本标签。" }
    },
    "n_idle": {
      "kind": "fallback",
      "priority": 1,
      "presentation": { "narrator": "兜底", "npc_text": "沉默", "npc_state": "平静" },
      "intent": { "description": "无明确意图时的保底闲聊。" }
    }
  }
})fsstory";

bool write_story(const std::string& path) {
    std::ofstream f(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!f.is_open()) return false;
    f << kStory;
    f.close();
    return true;
}
}  // namespace

int main() {
    std::cout << "=== 标签按存档持久化 / 存档隔离 · 真实运行期 C++ 用例 ===\n";
    const std::string tag = stamp();
    const std::string story = "/tmp/fs_tags_story_" + tag + ".json";
    const std::string nameA = "zz_tags_probe_A_" + tag;
    const std::string nameB = "zz_tags_probe_B_" + tag;
    const std::string nameC = "zz_tags_probe_C_" + tag;

    CHECK(write_story(story), "写出最小临时剧本（/tmp，不碰 data/story_v2.json）");

    // ---- 1. 实例 A：真实 step("1") 运行期路径 + addTag() 注入 LLM 型标签 ----
    EngineAdapter a(story);
    CHECK(a.ready(), "实例 A 构造成功（剧本 load + lint 通过）");
    if (!a.ready()) std::cerr << "  loadError: " << a.lastError() << "\n";

    a.createNewPlayer(nameA);
    CHECK(a.getAllTags().empty(), "createNewPlayer 后 all_tags_ 初始为空");

    StepResult sr = a.step("1");
    CHECK(sr.layer == "L0", "step(1) 走 L0 选项路由（未调用 LLM/selector）");
    CHECK(!sr.reply.empty(), "step(1) 有台词（纯数字分支取 npc_text）");
    CHECK(a.hasTag("script_tag_alpha"), "step 第6步把 effects.add_tags 写入 all_tags_");

    // 模拟 detectTag() 的唯一副作用：标签只进 all_tags_，不进 GameState.tags。
    a.addTag("probe_llm_only_1");
    a.addTag("probe_llm_only_2");
    CHECK(a.getAllTags().size() == 3u, "all_tags_ 现有 3 个标签（1 脚本 + 2 个仅 all_tags_）");
    std::cout << "  [证据] A 落盘前 all_tags_ = " << tags_to_string(a.getAllTags()) << "\n";
    a.persist();  // step 内部已自动 persist 一次，此处显式再存以写入 addTag 注入的标签

    // ---- 2. 新实例读档：标签必须全部恢复 ----
    EngineAdapter b(story);
    CHECK(b.ready(), "实例 B（读档用）构造成功");
    CHECK(b.loadExistingPlayer(nameA), "loadExistingPlayer(A) 成功");
    std::cout << "  [证据] B 读档后 all_tags_ = " << tags_to_string(b.getAllTags()) << "\n";
    CHECK(b.hasTag("script_tag_alpha"), "读档恢复脚本标签 script_tag_alpha");
    CHECK(b.hasTag("probe_llm_only_1"), "读档恢复仅 all_tags_ 标签 1（persist 全量反射生效）");
    CHECK(b.hasTag("probe_llm_only_2"), "读档恢复仅 all_tags_ 标签 2（persist 全量反射生效）");
    CHECK(b.getAllTags().size() == 3u, "读档标签集合大小与落盘前一致（3）");

    // ---- 3. 存档 B 隔离：不串到 A ----
    EngineAdapter c(story);
    c.createNewPlayer(nameB);
    c.step("1");
    c.addTag("probe_b_only");
    c.persist();
    std::cout << "  [证据] C(B档) all_tags_ = " << tags_to_string(c.getAllTags()) << "\n";
    CHECK(c.hasTag("probe_b_only"), "存档 B 持有自己的标签");
    CHECK(!c.hasTag("probe_llm_only_1"), "实例 C（B 档）不含 A 的标签");

    EngineAdapter d(story);
    CHECK(d.loadExistingPlayer(nameA), "写 B 后 A 仍可读档");
    std::cout << "  [证据] A 重读 all_tags_ = " << tags_to_string(d.getAllTags()) << "\n";
    CHECK(d.hasTag("probe_llm_only_1"), "写 B 后 A 的标签仍在");
    CHECK(!d.hasTag("probe_b_only"), "A 档不含 B 的标签（两存档互不串）");

    // ---- 4. removeTag 删除也能落盘（全量反射而非并集的关键证据）----
    EngineAdapter e(story);
    CHECK(e.loadExistingPlayer(nameA), "重新读 A 用于删除测试");
    e.removeTag("script_tag_alpha");
    e.removeTag("probe_llm_only_2");
    e.persist();
    EngineAdapter f(story);
    CHECK(f.loadExistingPlayer(nameA), "删除后再次读 A");
    std::cout << "  [证据] F 删除后读档 all_tags_ = " << tags_to_string(f.getAllTags()) << "\n";
    CHECK(!f.hasTag("script_tag_alpha"), "removeTag(script_tag_alpha) 删除已落盘");
    CHECK(!f.hasTag("probe_llm_only_2"), "removeTag(probe_llm_only_2) 删除已落盘");
    CHECK(f.hasTag("probe_llm_only_1"), "未删除的标签仍恢复");

    // ---- 5. 剧本 effects.remove_tags 路径（step 第 6 步 removeTag）----
    EngineAdapter g(story);
    g.createNewPlayer(nameC);
    g.step("1");
    CHECK(g.hasTag("script_tag_alpha"), "存档 C 第一步拿到脚本标签");
    g.step("1");
    CHECK(!g.hasTag("script_tag_alpha"), "剧本 effects.remove_tags 经 step 第6步删除 all_tags_");
    g.addTag("probe_c_only");
    g.persist();
    EngineAdapter h(story);
    CHECK(h.loadExistingPlayer(nameC), "存档 C 读档成功");
    CHECK(!h.hasTag("script_tag_alpha"), "剧本 remove_tags 的删除已落盘");
    CHECK(h.hasTag("probe_c_only"), "存档 C 其余标签恢复");

    // ---- 6. 清理临时存档，绝不碰 6 个生产存档 ----
    a.deleteSaveData(nameA);
    a.deleteSaveData(nameB);
    a.deleteSaveData(nameC);
    EngineAdapter z(story);
    CHECK(!z.loadExistingPlayer(nameA), "临时存档 A 已 deleteSaveData 清理");
    CHECK(!z.loadExistingPlayer(nameB), "临时存档 B 已 deleteSaveData 清理");
    CHECK(!z.loadExistingPlayer(nameC), "临时存档 C 已 deleteSaveData 清理");

    std::remove(story.c_str());

    std::cout << "结果: " << g_pass << "/" << (g_pass + g_fail) << " 项通过\n";
    return g_fail == 0 ? 0 : 1;
}
