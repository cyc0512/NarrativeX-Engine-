// ---------------------------------------------------------------------------
//  NarrativeX Engine :: nx/IntentMatch.hpp
//  ===========================================================
//  AI 匹配框架：把大模型从"创作者"降级为"分类器"
//  ===========================================================
//
//  三级漏斗：
//    L0  确定性快判       0 token，0 延迟
//        选项序号、系统指令、关键词强命中。命中且无歧义 -> 直接路由，不发请求。
//    L1  本地语义打分     0 token，~0.1ms
//        关键词命中率 + 中文字符二元组 Jaccard 相似度 + 反例扣分 + Frontier 先验
//        + MMR 去冗余，产出有序 Top-K。
//    L2  LLM 受限选择     少量 token
//        只把 Top-K 的 {id, 一句话描述} 注入 {DYNAMIC_ROUTES}，让模型做**单选题**。
//
//  为什么要"受限选择"而不是让 LLM 自由输出节点 ID？
//    自由生成会产生不存在的节点 ID（幻觉）。现有 GameEngine 已经用
//    `storyData["nodes"].contains(nextNode)` 做了事后兜底，但那是**事后**：
//    玩家认真说了半天，结果剧情原地不动，这是最伤沉浸感的失败模式。
//    正确做法是 事前约束（白名单）+ 事后校验（交集检查）双保险。
//
//  顺带的收益：L0/L1 命中时完全不调用 LLM。
//    实测意图分布里 40%~60% 的轮次会被 L0/L1 吃掉 —— 既省钱也省延迟，
//    更重要的是消除了这些轮次上的模型不确定性（利于可复现）。
// ---------------------------------------------------------------------------
#pragma once

#include <algorithm>
#include <cctype>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <unordered_map>
#include <vector>

#include "Frontier.hpp"
#include "GameState.hpp"
#include "NodeModel.hpp"
#include "Types.hpp"

namespace nx {

struct IntentWeights {
    double kw_hit = 0.55;        // 关键词命中率权重
    double kw_full_bonus = 0.30; // 关键词全部命中的额外奖励
    double sim = 0.45;           // 字面相似度权重
    double neg_penalty = 0.5;    // 命中反例的扣分
    double prior = 0.15;         // Frontier 打分作为先验
    double l0_confident = 0.86;  // L0 独占命中阈值
    double l1_margin = 0.12;     // Top1 领先 Top2 多少才算有把握
    double mmr_lambda = 0.25;    // MMR：相关性与去冗余的权衡
    int top_k = 5;
};

// LLM 选择器接口（策略模式 / 依赖注入）。
// 生产实现走 libcurl + QtConcurrent；测试实现返回固定值。
// 这样意图框架本身可以在完全离线的情况下被单元测试覆盖。
class ILlmSelector {
public:
    virtual ~ILlmSelector() = default;
    // 返回被选中的节点 ID；实现方只需在 shortlist 里选，但框架不信任返回值。
    virtual NodeId select(const std::string& user_input,
                          const std::vector<Candidate>& shortlist) = 0;
};

struct IntentDecision {
    NodeId node_id;
    std::string layer;      // L0 / L1 / L2 / fallback
    double confidence = 0.0;
    std::string reason;
    std::vector<std::pair<NodeId, double>> top_scores;   // 便于日志排查
};

// ---- 文本相似度工具（中文友好，无需分词器）--------------------------------

// 把 UTF-8 串按 **Unicode 码点** 切开（不依赖 ICU / locale）。
// 非法尾字节退化为单字节，保证永不越界、永不抛异常。
inline std::vector<std::string> utf8_chars(const std::string& text) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        std::size_t len = 1;
        if      ((c & 0xE0) == 0xC0) len = 2;   // 110xxxxx
        else if ((c & 0xF0) == 0xE0) len = 3;   // 1110xxxx
        else if ((c & 0xF8) == 0xF0) len = 4;   // 11110xxx
        if (i + len > text.size()) len = 1;     // 截断的多字节序列，按单字节兜底
        out.push_back(text.substr(i, len));
        i += len;
    }
    return out;
}

// ⚠ **必须按码点切，不能按字节切** —— 详见下方注释。
inline std::set<std::string> bigrams(const std::string& text) {
    // 旧实现是 `for (unsigned char c : text)` 逐字节扫描 + `s.substr(i, 2)`，
    // 也就是在 UTF-8 多字节汉字的**中间**下刀取二元组。
    // Python 参考实现（reference/nxref.py::_bigrams）是按 Unicode 码点切的，
    // 于是同一个输入两侧得到完全不同的二元组集合：
    //     输入「算了，当我没说」 vs n3_tender.examples
    //         Python（码点）：0.0        C++（字节）：0.1064
    //     输入「算了，当我没说」 vs n5_search.examples
    //         Python（码点）：0.0        C++（字节）：0.0556
    // 纯中文输入下 Python 侧 sim 几乎恒为 0，C++ 侧却总有一堆字节级"撞车"，
    // L1 打分因此系统性不同。实测在对照组 B0 的 seq=7 直接分叉：
    // C++ 走 n3_tender、Python 走 n5_search（margin 只有 0.0045，被 0.023 的
    // sim 差整个掀翻）。现把 C++ 对齐到码点切分，两侧打分才真正可比。
    const std::vector<std::string> chars = utf8_chars(text);
    std::vector<std::string> s;
    s.reserve(chars.size());
    for (const std::string& ch : chars) {
        if (ch.size() == 1 && std::isspace(static_cast<unsigned char>(ch[0]))) continue;
        std::string low = ch;
        if (low.size() == 1)
            low[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(low[0])));
        s.push_back(low);
    }
    std::set<std::string> out;
    if (s.size() < 2) { if (!s.empty()) out.insert(s.front()); return out; }
    for (std::size_t i = 0; i + 1 < s.size(); ++i) out.insert(s[i] + s[i + 1]);
    return out;
}

inline double jaccard(const std::set<std::string>& a, const std::set<std::string>& b) {
    if (a.empty() || b.empty()) return 0.0;
    std::size_t inter = 0;
    for (const auto& x : a) if (b.count(x)) ++inter;
    if (inter == 0) return 0.0;
    return static_cast<double>(inter) / static_cast<double>(a.size() + b.size() - inter);
}

inline double contains_count(const std::string& hay, const std::vector<std::string>& needles) {
    double hits = 0.0;
    for (const auto& n : needles) if (!n.empty() && hay.find(n) != std::string::npos) hits += 1.0;
    return hits;
}

class IntentMatcher {
public:
    explicit IntentMatcher(IntentWeights w = {}) : w_(std::move(w)) {}

    // ---- L0：确定性快判 ----
    double l0(const std::string& input, const NodeSpec& n) const {
        if (input.empty() || n.intent.keywords.empty()) return 0.0;
        const double hits = contains_count(input, n.intent.keywords);
        if (hits == 0.0) return 0.0;
        double s = std::min(1.0, 0.5 + 0.25 * hits);
        if (hits >= static_cast<double>(n.intent.keywords.size()))
            s = std::min(1.0, s + w_.kw_full_bonus);
        return s;
    }

    // ---- L1：本地语义打分 ----
    std::vector<std::pair<NodeId, double>> l1(const std::string& input,
                                              const Catalog& cat,
                                              const std::vector<Candidate>& cands) const {
        const std::set<std::string> q = bigrams(input);
        std::vector<std::pair<NodeId, double>> out;
        out.reserve(cands.size());
        for (const auto& c : cands) {
            const NodeSpec& n = *cat.find(c.id);
            std::string corpus = n.intent.description;
            for (const auto& e : n.intent.examples) { corpus += " "; corpus += e; }
            const double sim = jaccard(q, bigrams(corpus));

            double kw = 0.0;
            if (!n.intent.keywords.empty())
                kw = std::min(1.0, contains_count(input, n.intent.keywords)
                                   / static_cast<double>(n.intent.keywords.size()));
            const double neg = contains_count(input, n.intent.negatives);

            const double s = w_.kw_hit * kw + w_.sim * sim + w_.prior * c.score
                             - w_.neg_penalty * neg;
            out.emplace_back(c.id, s);
        }
        std::stable_sort(out.begin(), out.end(),
                         [](const auto& a, const auto& b) {
                             if (a.second != b.second) return a.second > b.second;
                             return a.first < b.first;
                         });
        return out;
    }

    // ---- MMR：去冗余后取多样性候选 ----
    std::vector<NodeId> mmr(const Catalog& cat,
                            const std::vector<std::pair<NodeId, double>>& scored,
                            std::size_t k) const {
        if (scored.empty()) return {};
        std::unordered_map<NodeId, std::set<std::string>> grams;
        for (const auto& kv : scored) {
            const NodeSpec& n = *cat.find(kv.first);
            std::string corpus = n.intent.description;
            for (const auto& e : n.intent.examples) { corpus += " "; corpus += e; }
            grams[kv.first] = bigrams(corpus);
        }
        std::vector<NodeId> picked;
        picked.push_back(scored.front().first);
        while (picked.size() < std::min(k, scored.size())) {
            NodeId best;
            double best_v = -1e9;
            bool found = false;
            for (const auto& kv : scored) {
                bool dup = false;
                for (const auto& p : picked) if (p == kv.first) dup = true;
                if (dup) continue;
                double redundancy = 0.0;
                for (const auto& p : picked)
                    redundancy = std::max(redundancy, jaccard(grams[kv.first], grams[p]));
                const double v = w_.mmr_lambda * kv.second - (1.0 - w_.mmr_lambda) * redundancy;
                if (v > best_v || (std::abs(v - best_v) < 1e-9 && (!found || kv.first < best))) {
                    best_v = v; best = kv.first; found = true;
                }
            }
            if (!found) break;
            picked.push_back(best);
        }
        return picked;
    }

    // ---- 三级漏斗总入口 ----
    IntentDecision decide(const std::string& input, const Catalog& cat,
                          const std::vector<Candidate>& cands,
                          const NodeId& default_next,
                          ILlmSelector* llm) const {
        IntentDecision d;
        if (cands.empty()) {
            d.node_id = default_next;
            d.layer = "fallback";
            d.reason = "Frontier 为空，落到 default_next";
            return d;
        }

        // L0：只在最高 bucket 的那一梯队里找，避免被低优先节点抢答
        const int top_bucket = std::max_element(
            cands.begin(), cands.end(),
            [](const Candidate& a, const Candidate& b) { return a.bucket < b.bucket; })->bucket;

        NodeId l0_best;
        double l0_v = -1.0;
        for (const auto& c : cands) {
            if (c.bucket != top_bucket) continue;
            const double s = l0(input, *cat.find(c.id));
            if (s > l0_v || (std::abs(s - l0_v) < 1e-9 && c.id < l0_best)) { l0_v = s; l0_best = c.id; }
        }
        int l0_rivals = 0;
        for (const auto& c : cands)
            if (c.bucket == top_bucket && c.id != l0_best && l0(input, *cat.find(c.id)) >= l0_v)
                ++l0_rivals;
        if (!l0_best.empty() && l0_v >= w_.l0_confident && l0_rivals == 0) {
            d.node_id = l0_best; d.layer = "L0"; d.confidence = l0_v;
            d.reason = "关键词强命中且无歧义，跳过 LLM（0 token）";
            d.top_scores.push_back({l0_best, l0_v});
            return d;
        }

        // L1
        auto scored = l1(input, cat, cands);
        const auto shortlist_ids = mmr(cat, scored, static_cast<std::size_t>(w_.top_k));
        std::vector<Candidate> shortlist;
        for (const auto& c : cands)
            for (const auto& id : shortlist_ids)
                if (c.id == id) { shortlist.push_back(c); break; }

        const double margin = scored.size() > 1 ? (scored[0].second - scored[1].second) : 1.0;
        d.top_scores.assign(scored.begin(),
                            scored.begin() + std::min<std::size_t>(5u, scored.size()));

        if (llm == nullptr || (scored[0].second >= 0.5 && margin >= w_.l1_margin)) {
            d.node_id = scored[0].first; d.layer = "L1"; d.confidence = scored[0].second;
            d.reason = "本地语义打分已足够判别，跳过 LLM";
            return d;
        }

        // L2：受限选择
        NodeId picked;
        try {
            picked = llm->select(input, shortlist);
        } catch (...) {
            d.node_id = scored[0].first; d.layer = "L1"; d.confidence = scored[0].second;
            d.reason = "LLM 调用异常，降级到本地打分";
            return d;
        }
        bool legal = false;
        for (const auto& id : shortlist_ids) if (id == picked) legal = true;
        if (legal) {
            d.node_id = picked; d.layer = "L2"; d.confidence = 1.0;
            d.reason = "LLM 受限选择";
            return d;
        }
        // 幻觉防线：白名单外的返回值一律不信任，但记录留痕便于回溯排错
        d.node_id = scored[0].first; d.layer = "L1"; d.confidence = scored[0].second;
        d.reason = "LLM 返回白名单外的节点 '" + picked + "'，已降级";
        return d;
    }

private:
    IntentWeights w_;
};

}  // namespace nx
