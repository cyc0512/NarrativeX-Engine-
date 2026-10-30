// ---------------------------------------------------------------------------
//  NarrativeX Engine :: nx/PromptAssembler.hpp
//  Prompt 组装 + Token 预算管理器。
//
//  解决的问题：长上下文导致的 "Lost in the Middle"。
//  做法：不分 swapping，而是**分块 + 预算 + 降级裁剪**。
//
//  裁剪优先级（预算不足时按此顺序放弃低优先级块）：
//     1. 砍 routes（候选条数 K -> K-1 ... -> 1）
//     2. 砍 history（保留最近 N 轮 -> 更少）
//     3. 砍 summary（长期摘要）
//  永远保留且不可裁剪（砍掉 = 人设崩塌 / 格式崩坏）：
//     [persona 人设] [当前时间阶段] [NPC 状态] [输出格式约束]
// ---------------------------------------------------------------------------
#pragma once

#include <string>
#include <vector>

#include "GameState.hpp"
#include "NodeModel.hpp"
#include "Types.hpp"

namespace nx {

struct PromptBudget {
    int persona = 260;
    int world = 140;
    int summary = 180;
    int history = 360;
    int routes = 320;
    int output = 140;
    int max_history_turns = 6;
};

struct PromptResult {
    std::string system_prompt;
    std::vector<std::string> truncated;      // 被裁剪的块名，用于日志
    std::vector<NodeId> routes_injected;     // **这就是 L2 层的白名单**
    std::size_t est_tokens = 0;
};

class PromptAssembler {
public:
    explicit PromptAssembler(PromptBudget b = {}) : b_(std::move(b)) {}

    PromptResult build(const std::string& persona,
                       const std::string& phase,
                       const std::string& npc_state,
                       int affinity,
                       const std::string& summary,
                       const std::vector<HistoryTurn>& history,
                       const std::vector<std::pair<NodeId, std::string>>& routes) const {
        PromptResult r;

        // --- 必留块：当前状态 ---
        std::string npc_state_c = clip(npc_state, 60, "npc_state", r);
        std::string summary_c = clip(summary, b_.summary, "summary", r);

        std::string world =
            "【当前时间阶段】" + phase + "\n"
            "【NPC 状态】" + npc_state_c + "\n"
            "【隐藏好感度】" + std::to_string(affinity) + "\n"
            "【长期记忆摘要】" + (summary_c.empty() ? "（暂无）" : summary_c);

        // --- 可裁剪块 1：历史轮数 ---
        std::vector<HistoryTurn> hist;
        const int n = static_cast<int>(history.size());
        const int from = n > b_.max_history_turns ? n - b_.max_history_turns : 0;
        for (int i = from; i < n; ++i) hist.push_back(history[i]);
        std::string hist_block = render_history(hist);
        while (static_cast<int>(estimate_tokens(hist_block)) > b_.history && !hist.empty()) {
            hist.erase(hist.begin());
            hist_block = render_history(hist);
            r.truncated.push_back("history");
        }

        // --- 可裁剪块 2：候选路径 ---
        std::vector<std::pair<NodeId, std::string>> rs = routes;
        std::string routes_block = render_routes(rs);
        while (rs.size() > 1 && static_cast<int>(estimate_tokens(routes_block)) > b_.routes) {
            rs.pop_back();
            routes_block = render_routes(rs);
            r.truncated.push_back("routes");
        }
        for (const auto& kv : rs) r.routes_injected.push_back(kv.first);

        // --- 组装 ---
        std::string personap = clip(persona, b_.persona, "persona", r);
        std::string out = personap + "\n\n" + world;
        if (!routes_block.empty())
            out += "\n\n【可选路径（只能从这里选）】\n" + routes_block;
        if (!hist_block.empty())
            out += "\n\n【刚刚的对话】\n" + hist_block;
        out += "\n\n" + render_output(rs);

        r.system_prompt = out;
        r.est_tokens = estimate_tokens(out);
        return r;
    }

    // 按 token 预算做字符级截断（保守估计：每 token ≈ 1.4 个中文字符）
    std::string clip(const std::string& text, int limit, const std::string& tag,
                     PromptResult& r) const {
        if (text.empty()) return text;
        if (static_cast<int>(estimate_tokens(text)) <= limit) return text;
        r.truncated.push_back(tag);
        const std::size_t keep = static_cast<std::size_t>(limit * 1.4);
        return text.substr(0, keep) + "…";
    }

private:
    PromptBudget b_;

    static std::string render_history(const std::vector<HistoryTurn>& h) {
        std::string s;
        for (const auto& t : h) { s += t.role + ": " + t.content + "\n"; }
        return s;
    }
    static std::string render_routes(const std::vector<std::pair<NodeId, std::string>>& rs) {
        if (rs.empty()) return "（无，请按当前情境自然回应）";
        std::string s;
        for (const auto& kv : rs) s += "- <" + kv.first + "> " + kv.second + "\n";
        return s;
    }
    static std::string render_output(const std::vector<std::pair<NodeId, std::string>>& rs) {
        std::string ids;
        for (const auto& kv : rs) { if (!ids.empty()) ids += "|"; ids += "<" + kv.first + ">"; }
        if (ids.empty()) ids = "<none>";
        return "【输出格式】只能输出一个 JSON 对象，不要输出任何其它内容：\n"
               "{\"affinity_change\": 数值, \"next_node\": \"节点ID\", \"reply\": \"台词\"}\n"
               "约束：next_node 必须是 " + ids + " 之一，或空字符串表示留在当前情境。";
    }
};

}  // namespace nx
