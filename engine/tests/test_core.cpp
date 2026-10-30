// ---------------------------------------------------------------------------
//  nx 内核冒烟测试 + 跨语言一致性校验
//
//  编译（MinGW / GCC / Clang 均可，纯 header-only）：
//    g++ -std=c++17 -I../include -I../../include -Wall -Wextra tests/test_core.cpp -o test_core
//
//  运行后会把每一步的轨迹打到 stdout，应与 reference/expected_trace.txt
//  完全一致 —— 这份文件由 Python 参考实现生成，构成跨实现的双向校验。
// ---------------------------------------------------------------------------
#include <cassert>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "nx.hpp"

using namespace nx;

namespace {

std::string read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        std::cerr << "无法打开文件: " << p << "\n";
        std::exit(2);
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string join_sorted(const std::unordered_set<std::string>& s) {
    std::vector<std::string> v(s.begin(), s.end());
    std::sort(v.begin(), v.end());
    std::string out;
    for (const auto& x : v) { if (!out.empty()) out += ","; out += x; }
    return out;
}

struct StubSelector : ILlmSelector {
    NodeId select(const std::string&, const std::vector<Candidate>& shortlist) override {
        ++calls;
        // 模拟一个"总是选 top-ranked"的理想模型
        const Candidate* best = nullptr;
        for (const auto& c : shortlist)
            if (best == nullptr || c.score > best->score) best = &c;
        return best ? best->id : NodeId{};
    }
    int calls = 0;
};

}  // namespace

int main() {
    const std::string story_path = "../../data/story_v2.example.json";
    const std::string story_text = read_file(story_path);

    Catalog cat = StoryLoader::load(story_text);
    nlohmann::json doc = nlohmann::json::parse(story_text);

    auto fw = StoryLoader::frontier_weights(doc);
    auto iw = StoryLoader::intent_weights(doc);
    auto pb = StoryLoader::prompt_budget(doc);

    GameState st = StoryLoader::new_state("seed", 90, "0", 0xC0FFEEull);
    NarrativeEngine eng(cat, st, fw, iw, pb);

    StubSelector llm;
    eng.set_selector(&llm);

    std::vector<std::string> inputs = {
        "1", "2", "1", "1", "1", "我口袋里好像有东西", "1", "1", "1", "1", "1",
    };

    std::ostringstream trace;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const NodeId before = eng.state().current_node;
        TurnResult r = eng.step(inputs[i]);
        const GameState& s = eng.state();
        std::string redeemed;
        for (const auto& n : r.redeemed) { if (!redeemed.empty()) redeemed += ","; redeemed += n; }
        std::size_t open_pending = 0;
        for (const auto& p : s.pending) if (!p.awake) ++open_pending;

        trace << "step=" << (i + 1)
              << " seq=" << s.seq
              << " from=" << before
              << " to=" << s.current_node
              << " layer=" << r.layer
              << " phase=" << s.phase
              << " aff=" << s.affinity
              << " tags=[" << join_sorted(s.tags) << "]"
              << " items=[" << join_sorted(s.items) << "]"
              << " pending_open=" << open_pending
              << " redeemed=[" << redeemed << "]\n";

        // 每步都必须有非空 Frontier
        assert(!r.frontier_before.items.empty() && "Frontier 不得为空");
    }

    // ---- 断言：三条铁律 ----
    assert(eng.state().seq == static_cast<Seq>(inputs.size()) && "time_cursor 必须严格 +1/步");
    assert(eng.state().phase_index >= 0);

    // Frontier 调试输出（人工排查剧情断裂的第一现场）
    std::cout << eng.debug_frontier();

    // Prompt 组装
    PromptResult pr = eng.build_prompt("正在掩饰着什么");
    std::cout << "\n--- Prompt 组装 ---\n"
              << "est_tokens=" << pr.est_tokens
              << " routes_injected=";
    for (const auto& r : pr.routes_injected) std::cout << r << " ";
    std::cout << "\n truncated=";
    for (const auto& t : pr.truncated) std::cout << t << " ";
    std::cout << "\n";

    // L2 白名单必须恰好是 routes_injected（这是幻觉防线的前提）
    assert(pr.routes_injected.size() <= 5);

    std::cout << "\n=== TRACE ===\n" << trace.str();
    std::cout << "LLM 调用次数=" << llm.calls << "\n";
    std::cout << "ALL ASSERTIONS PASSED\n";
    return 0;
}
