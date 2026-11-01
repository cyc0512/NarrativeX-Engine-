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

// 甲方硬约束：**每轮实际发出的 prompt 不得超过 1200 token**。
// 写成常量而不是散落的魔法数字，是为了让"上限"只有一处定义、可被断言。
// 各块预算（persona/world/summary/history/routes/output）之和允许超过它 ——
// 那是"理想配比"，真正的兜底由 `PromptAssembler::build()` 末尾的硬上限循环负责。
constexpr int kMaxPromptTokens = 1200;

struct PromptBudget {
    // 软预算：各块的理想配比。硬上限是 `kMaxPromptTokens`，
    // `total_tokens` 只能在 [1, kMaxPromptTokens] 区间内收紧它，不能放大。
    int total_tokens = kMaxPromptTokens;
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

        // 硬上限：剧本里配的 total_tokens 只能**收紧**，绝不能放大到 1200 以上。
        const int cap = (b_.total_tokens > 0 && b_.total_tokens < kMaxPromptTokens)
                            ? b_.total_tokens : kMaxPromptTokens;

        // --- 必留块：当前状态 ---
        std::string npc_state_c = clip(npc_state, 60, "npc_state", r);
        std::string summary_c = clip(summary, b_.summary, "summary", r);
        std::string personap = clip(persona, b_.persona, "persona", r);

        auto world_block = [&]() {
            return std::string(
                "【当前时间阶段】" + phase + "\n"
                "【NPC 状态】" + npc_state_c + "\n"
                "【隐藏好感度】" + std::to_string(affinity) + "\n"
                "【长期记忆摘要】" + (summary_c.empty() ? "（暂无）" : summary_c));
        };

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
        // ⚠ 裁剪只能从**尾部**丢：`routes` 是 Frontier 按 bucket+score 排好序的
        //   Top-K，头部是最该走的节点。从头部丢 = 把正确分支裁掉 = 选路失真。
        std::vector<std::pair<NodeId, std::string>> rs = routes;
        std::string routes_block = render_routes(rs);
        while (rs.size() > 1 && static_cast<int>(estimate_tokens(routes_block)) > b_.routes) {
            rs.pop_back();
            routes_block = render_routes(rs);
            r.truncated.push_back("routes");
        }

        // --- 组装 ---
        auto assemble = [&]() {
            std::string out = personap + "\n\n" + world_block();
            if (!routes_block.empty())
                out += "\n\n【可选路径（只能从这里选）】\n" + routes_block;
            if (!hist_block.empty())
                out += "\n\n【刚刚的对话】\n" + hist_block;
            out += "\n\n" + render_output(rs);
            return out;
        };

        std::string out = assemble();

        // ---- 硬上限兜底：无论各块预算怎么配，最终发出的 prompt 必须 ≤ cap ----
        // 放弃顺序严格按"信息价值从低到高"：候选条数 → 历史轮数 → 摘要 → 人设。
        // 收敛保护：若某轮没有任何块变小（理论上不可能），立即跳出，绝不死循环。
        int guard = 0;
        while (static_cast<int>(estimate_tokens(out)) > cap && guard++ < 4096) {
            const std::string prev = out;
            if (rs.size() > 1) {                       // ① 先压候选条数（只丢尾部）
                rs.pop_back();
                routes_block = render_routes(rs);
                r.truncated.push_back("routes:cap");
            } else if (!hist.empty()) {                // ② 再压历史轮数（丢最老的）
                hist.erase(hist.begin());
                hist_block = render_history(hist);
                r.truncated.push_back("history:cap");
            } else if (!summary_c.empty()) {           // ③ 再砍长期摘要
                summary_c.clear();
                r.truncated.push_back("summary:cap");
            } else {                                   // ④ 最后才动人设（保命）
                personap = clip(personap, std::max(64, cap / 3), "persona:cap", r);
            }
            out = assemble();
            if (out == prev) break;                    // 收敛保护
        }

        for (const auto& kv : rs) r.routes_injected.push_back(kv.first);
        r.system_prompt = out;
        r.est_tokens = estimate_tokens(out);
        return r;
    }

    // 本轮 prompt 的 token 上限（供断言与日志使用，避免各处写魔法数字）。
    static int max_prompt_tokens() { return kMaxPromptTokens; }

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

    // =========================================================================
    //  ⚠ 格式对齐（无语义影响）：以下三块模板 / 拼接方式与 Python 参考实现
    //     （`nxref.py::PromptAssembler`）保持逐字一致，**仅为跨语言比对统一**。
    //
    //     这两类差异没有任何语义后果：
    //       · 措辞（"其它任何内容" vs "任何其它内容"、`数值` vs `int`、
    //         "（无，按…）" vs "（无，请按…）"）
    //       · 拼接（行尾是否带 '\n'，影响 estimate_tokens 的取整差 1）
    //
    //     **勿据此推断规范** —— 规范来自设计与意图层（PRD/ARCH），参考实现只做回归锁。
    //     按团队裁定，这类"实现细节层"分歧今后一律**不改被测代码**，而是改比较脚本
    //     （结构化语义比对，`reference/cross_check.py`）。此处已成事实且无害，保留。
    // =========================================================================
    static std::string render_history(const std::vector<HistoryTurn>& h) {
        // 格式对齐：与 Python 参考实现统一，无语义影响；勿据此推断规范
        // （Python 侧是 "\n".join(...)，**没有行尾换行**；多一个 '\n' 就是 0.25 token）
        std::string s;
        for (std::size_t i = 0; i < h.size(); ++i) {
            if (i) s += "\n";
            s += h[i].role + ": " + h[i].content;
        }
        return s;
    }
    static std::string render_routes(const std::vector<std::pair<NodeId, std::string>>& rs) {
        if (rs.empty()) return "（无，按当前情境自然回应）";
        // 格式对齐：与 Python 参考实现统一，无语义影响；勿据此推断规范
        // （Python 侧是 "\n".join(...)，**没有行尾换行**；多一个 '\n' 让 estimate 取整差 1）
        std::string s;
        for (std::size_t i = 0; i < rs.size(); ++i) {
            if (i) s += "\n";
            s += "- <" + rs[i].first + "> " + rs[i].second;
        }
        return s;
    }
    static std::string render_output(const std::vector<std::pair<NodeId, std::string>>& rs) {
        // 格式对齐：与 Python 参考实现统一，无语义影响；勿据此推断规范
        // （措辞差异：其它任何内容 / int /（无，按…）—— 均不改变语义）
        std::string ids;
        for (const auto& kv : rs) { if (!ids.empty()) ids += "|"; ids += "<" + kv.first + ">"; }
        if (ids.empty()) ids = "<none>";
        return "【输出格式】只能输出一个 JSON 对象，不要输出其它任何内容：\n"
               "{\"affinity_change\": int, \"next_node\": \"节点ID\", \"reply\": \"台词\"}\n"
               "约束：next_node 必须是 " + ids + " 之一，或空字符串表示留在当前情境。";
    }
};

}  // namespace nx
