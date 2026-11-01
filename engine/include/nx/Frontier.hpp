// ---------------------------------------------------------------------------
//  NarrativeX Engine :: nx/Frontier.hpp
//  ===========================================================
//  【空缺 1 的答案】Frontier 重构算法：多个节点优先级怎么处理？
//  ===========================================================
//
//  答案一句话：**不要试图用一个分数决出胜负，而是"硬分层 + 软打分 + 稳定排序"。**
//
//  算法四步：
//    Step 1  GATE   硬过滤：terminal / 生命周期窗口 / 物理阶段 / requires /
//                   once / cooldown -> feasible 集合（顺带记录被拦截原因）
//    Step 2  SCORE  多因子线性融合，见 FrontierScorer::score()
//    Step 3  RANK   按 (bucket 降序, score 降序, id 升序) 三级稳定排序
//                   - bucket 是作者意图的硬分层，永不被分数翻越；
//                   - id 升序作为最终 tie-break —— 保证结果完全确定，
//                     这是"读档不错乱"和"可写断言测试"的前提。
//    Step 4  SAFETY feasible 为空 -> 注入 fallback；连 fallback 都没有 -> 抛错。
//                   【死锁不是靠运气避免的，是靠数据结构保证的】
//
//  为什么不用"单一优先级数值"？
//    因为节点的"该不该出现"至少同时受五种力量拉扯：作者意图、时间紧迫度、
//    新鲜度、玩家当前契合度、以及不能被重复的洁癖。把它们压成一个 int
//    会导致作者每调一个地方就要重排全表。分成相加的若干项，作者只需调血量
//    权重或单项 priority 即可，且每一项都能在 debug 日志里单独打印出来 ——
//    这正好满足"剧情断裂时直接看 Frontier 列表定位是哪一项没满足"的诉求。
// ---------------------------------------------------------------------------
#pragma once

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

#include "GameState.hpp"
#include "NodeModel.hpp"
#include "Types.hpp"

namespace nx {

struct FrontierWeights {
    double base = 1.0;
    double urgency = 0.8;
    double staleness = 1.2;
    double affinity = 0.4;
    double recency = 0.35;
    double cooldown = 2.0;
    double group_penalty = 3.0;
    double pending_age = 0.5;
    double staleness_tau = 6.0;
    Seq urgency_horizon = 12;
    int top_k = 5;
    double temperature = 0.0;   // >0 时开启有界随机扰动，保持多样性但可复现
};

struct Candidate {
    NodeId id;
    int bucket = 0;
    Score score = 0.0;
    std::unordered_map<std::string, double> parts;   // 各因子分值，用于调试

    std::string explain() const {
        std::string s = "[" + id + "] bucket=" + std::to_string(bucket)
                      + " score=" + std::to_string(score) + " :: ";
        for (const auto& kv : parts) s += kv.first + "=" + std::to_string(kv.second) + " ";
        return s;
    }
};

// 被拦截的原因。debug 输出时按原因分组，一眼看出缺哪个条件。
enum class BlockReason {
    Ok, Terminal, Future, Expired, Phase, Consumed, Cooldown, Requires, GroupLocked
};

inline const char* block_reason_name(BlockReason r) noexcept {
    switch (r) {
        case BlockReason::Ok:          return "ok";
        case BlockReason::Terminal:    return "terminal";
        case BlockReason::Future:      return "future";
        case BlockReason::Expired:     return "expired";
        case BlockReason::Phase:       return "phase";
        case BlockReason::Consumed:    return "consumed";
        case BlockReason::Cooldown:    return "cooldown";
        case BlockReason::Requires:    return "requires";
        case BlockReason::GroupLocked: return "group_locked";
    }
    return "unknown";
}

class FrontierScorer {
public:
    explicit FrontierScorer(FrontierWeights w = {}) : w_(std::move(w)) {}
    const FrontierWeights& weights() const noexcept { return w_; }
    void set_weights(FrontierWeights w) { w_ = std::move(w); }

    // ---- Step 1：硬过滤 ----
    BlockReason gate(const Catalog& cat, const GameState& st, const NodeId& id) const {
        const NodeSpec* n = cat.find(id);
        if (n == nullptr) return BlockReason::Requires;
        if (n->terminal) return BlockReason::Terminal;
        if (st.seq < n->min_step) return BlockReason::Future;
        if (st.seq > n->max_step) return BlockReason::Expired;     // 永久失效
        if (!n->phase_ok(st.phase, st.phase_index)) return BlockReason::Phase;
        if (n->once && st.count_of(id) >= 1) return BlockReason::Consumed;
        if (n->cooldown > 0) {
            auto it = st.last_visit.find(id);
            if (it != st.last_visit.end() && (st.seq - it->second) < n->cooldown)
                return BlockReason::Cooldown;
        }
        EvalContext ctx = make_ctx(st);
        if (!n->requires_fn(ctx)) return BlockReason::Requires;
        return BlockReason::Ok;
    }

    // ---- Step 2：多因子打分 ----
    Candidate score(const Catalog& cat, const GameState& st, const NodeId& id,
                    const PendingEntry* awakened_from = nullptr) const {
        const NodeSpec& n = *cat.find(id);
        Candidate c;
        c.id = id;
        c.bucket = n.bucket();

        auto add = [&c](const char* k, double v) { c.parts[k] = v; };

        // f_base：作者意图
        add("base", w_.base * (n.priority / 100.0));

        // f_urgency：生命周期紧迫度 —— 快到期了就先让你去，这是引导收束的关键
        double urgency = 0.0;
        if (n.max_step < 1000000000LL) {
            const Seq remain = n.max_step - st.seq;
            urgency = std::max(0.0, 1.0 - static_cast<double>(std::max<Seq>(0, remain))
                                         / static_cast<double>(w_.urgency_horizon));
        }
        add("urgency", w_.urgency * urgency);

        // f_staleness：反重复 —— 刚走过的地方指数衰减降权
        double staleness = 0.0;
        auto lv = st.last_visit.find(id);
        if (lv != st.last_visit.end())
            staleness = std::exp(-static_cast<double>(st.seq - lv->second) / w_.staleness_tau);
        add("staleness", -w_.staleness * staleness);

        // f_recency：没去过的地方 +分
        add("recency", w_.recency * (st.visited.count(id) ? 0.0 : 1.0));

        // f_affinity：玩家当前好感度与该节点设计意图的贴合程度
        const int target = affinity_target(n);
        add("affinity", target < 0 ? 0.0
              : -w_.affinity * (std::abs(st.affinity - target) / 100.0));

        // f_cooldown：冷却剩余惩罚
        double cd = 0.0;
        if (n.cooldown > 0 && lv != st.last_visit.end()) {
            const Seq left = std::max<Seq>(0, n.cooldown - (st.seq - lv->second));
            cd = static_cast<double>(left) / static_cast<double>(n.cooldown);
        }
        add("cooldown", -w_.cooldown * cd);

        // f_pending_age：挂起越久，挽回时分量越重（"弥补遗憾"的情感设计）
        if (awakened_from != nullptr) {
            const Seq age = st.seq - awakened_from->birth_seq;
            add("pending_age", w_.pending_age * std::log1p(static_cast<double>(age)) / 3.0);
        }

        double total = 0.0;
        for (const auto& kv : c.parts) total += kv.second;
        c.score = total * n.weight;
        return c;
    }

    // ---- Step 3 + 4：重算整个 Frontier ----
    // 返回排序结果；blocked 输出"每类原因拦下了哪些节点"，直接服务于调试诉求。
    struct FrontierView {
        std::vector<Candidate> items;                              // 已排序，非空（除非抛错）
        std::unordered_map<std::string, std::vector<NodeId>> blocked;
    };

    FrontierView refresh(const Catalog& cat, const GameState& st,
                         std::unordered_map<NodeId, PendingEntry>* awakened = nullptr,
                         Rng* rng = nullptr) const {
        std::vector<NodeId> cand = cat.candidate_ids(st.tags, st.items, st.current_node);

        // 挂起池里的节点必须参与召回，否则"挽回"永远轮不到它们。
        //
        // ⚠ **已唤醒（awake）的条目同样要召回** —— 它只是"重新变得可达"，
        //   玩家还没有真正走进去。此前这里写成 `if (!p.awake)`，导致一个刚被
        //   后向指针唤醒、但 requires 里带**永不被满足的 tag 依赖**的节点
        //   （本例是 n5_search 的 `{any:[{step_gte:1},{tag:"suspicious"}]}`）
        //   会立刻从候选集里消失 —— 刚被救回来又当场变回不可达。
        //   这是对照组 B0 在 C++ 侧与 Python 侧走出不同分支的根因
        //   （C++ step7 走 n3_tender，Python 走 n5_search）。
        //   Python 参考实现（Catalog::candidate_ids）用 set 收集全部 pending，
        //   没有这个过滤；现把 C++ 对齐过去。
        for (const auto& p : st.pending)
            if (cat.find(p.node_id) != nullptr) cand.push_back(p.node_id);

        // 去重 + 定序：pending 与 index.always / neighbors 可能重叠，不去重会让
        // 同一个节点在 Frontier 里出现两次（Python 侧用 set 天然去重，两侧必须一致）。
        // 顺带把 unordered_set 的任意迭代序收敛成确定的字典序，便于跨语言 diff。
        std::sort(cand.begin(), cand.end());
        cand.erase(std::unique(cand.begin(), cand.end()), cand.end());

        FrontierView view;
        for (const auto& id : cand) {
            const BlockReason r = gate(cat, st, id);
            if (r != BlockReason::Ok) {
                view.blocked[block_reason_name(r)].push_back(id);
                continue;
            }
            const PendingEntry* src = nullptr;
            if (awakened != nullptr) {
                auto it = awakened->find(id);
                if (it != awakened->end()) src = &it->second;
            }
            view.items.push_back(score(cat, st, id, src));
        }

        // 三级稳定排序
        std::stable_sort(view.items.begin(), view.items.end(),
                         [](const Candidate& a, const Candidate& b) {
                             if (a.bucket != b.bucket) return a.bucket > b.bucket;
                             if (a.score  != b.score)  return a.score  > b.score;
                             return a.id < b.id;                 // 最终确定性兜底
                         });

        if (rng != nullptr && w_.temperature > 0.0)
            view.items = perturb(view.items, rng);

        // Step 4：防死锁
        if (view.items.empty()) {
            const std::vector<NodeId> fb = fallbacks(cat, st);
            if (fb.empty())
                throw NxError(NxError::Code::Deadlock,
                    "剧本死锁: seq=" + std::to_string(st.seq)
                    + " node=" + st.current_node + " 无任何可用节点且缺少 fallback");
            Candidate c;
            c.id = fb.front();
            c.bucket = cat.find(c.id)->bucket();
            c.score = -999.0;
            c.parts["fallback"] = -999.0;
            view.items.push_back(c);
        }
        return view;
    }

    // 只取 Top-K（配合 token 预算使用）
    static std::vector<Candidate> top_k(const FrontierView& v, std::size_t k) {
        if (v.items.size() <= k) return v.items;
        return std::vector<Candidate>(v.items.begin(), v.items.begin() + k);
    }

private:
    FrontierWeights w_;

    static EvalContext make_ctx(const GameState& st) {
        EvalContext c;
        c.tags = st.tags;
        c.items = st.items;
        for (const auto& kv : st.visited) c.visited.insert(kv.first);
        for (const auto& kv : st.visit_count) c.visit_count.emplace_back(kv.first, kv.second);
        c.affinity = st.affinity;
        c.seq = st.seq;
        c.phase = st.phase;
        c.phase_index = st.phase_index;
        return c;
    }

    // 从 requires 里提取作者设定的好感度锚点。条件是编译后的闭包无法反解，
    // 因此由 StoryLoader 在编译阶段把均值写入 NodeSpec.affinity_anchor。
    static int affinity_target(const NodeSpec& n) { return n.affinity_anchor; }

    std::vector<Candidate> perturb(std::vector<Candidate> in, Rng* rng) const {
        // 按 bucket 分组后在组内加有界噪声；噪声本身来自可控 RNG -> 可复现
        std::stable_sort(in.begin(), in.end(), [](const Candidate& a, const Candidate& b) {
            return a.bucket > b.bucket;
        });
        std::vector<Candidate> out;
        std::size_t i = 0;
        while (i < in.size()) {
            std::size_t j = i;
            while (j < in.size() && in[j].bucket == in[i].bucket) ++j;
            const double top = std::abs(in[i].score);
            for (std::size_t k = i; k < j; ++k)
                in[k].score += rng->next_double() * w_.temperature * std::max(1.0, top);
            std::stable_sort(in.begin() + i, in.begin() + j,
                             [](const Candidate& a, const Candidate& b) {
                                 if (a.score != b.score) return a.score > b.score;
                                 return a.id < b.id;
                             });
            out.insert(out.end(), in.begin() + i, in.begin() + j);
            i = j;
        }
        return out;
    }

    static std::vector<NodeId> fallbacks(const Catalog& cat, const GameState& st) {
        std::vector<NodeId> out;
        EvalContext ctx = make_ctx(st);
        for (const auto& kv : cat.nodes) {
            const NodeSpec& n = kv.second;
            if (n.kind != NodeKind::Fallback) continue;
            if (n.terminal) continue;
            if (!n.window_ok(st.seq) || !n.phase_ok(st.phase, st.phase_index)) continue;
            if (n.once && st.count_of(n.id) >= 1) continue;
            if (!n.requires_fn(ctx)) continue;
            out.push_back(n.id);
        }
        std::sort(out.begin(), out.end());
        return out;
    }
};

}  // namespace nx
