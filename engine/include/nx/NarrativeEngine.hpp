// ---------------------------------------------------------------------------
//  NarrativeX Engine :: nx/NarrativeEngine.hpp
//  编排层：把 Frontier / PendingPool / IntentMatcher 串成一次"走一步迷宫"。
//
//  单步顺序被严格固定（顺序本身就是设计的一部分，不要随意调换）：
//
//      step(t):
//        0. 断言 seq >= 0
//        1. refresh Frontier        <- 此刻尚无任何副作用，Frontier 是"干净的"
//        2. 三级漏斗选路            <- L0/L1/L2
//        3. 应用被选中节点的 effects（tags / items / affinity / phase）
//        4. 把"这次没走的、作者声明可挽回的兄弟节点"压入 PendingPool
//        5. seq += 1               <- time_cursor 唯一推进点
//        6. PendingPool.scan()     <- 后向指针唤醒
//        7. 返回 TurnResult，事件由上层写入持久层（单一事务）
//
//  为什么 LLM 的生成放在下一次该怎么办？见 #集成注意 注释。
// ---------------------------------------------------------------------------
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "Frontier.hpp"
#include "GameState.hpp"
#include "IntentMatch.hpp"
#include "NodeModel.hpp"
#include "PendingPool.hpp"
#include "PromptAssembler.hpp"
#include "Types.hpp"

namespace nx {

struct TurnResult {
    NodeId from_node;
    NodeId to_node;
    int affinity_change = 0;
    std::string layer;             // L0 / L1 / L2 / fallback
    std::string reason;
    double confidence = 0.0;
    std::vector<std::string> events;               // 人类可读副作用日志
    std::vector<NodeId> redeemed;                  // 本步被挽回的节点
    FrontierScorer::FrontierView frontier_before;  // 用于调试/回放
    std::vector<NodeId> routes_injected;           // 本次喂给 LLM 的白名单
    Seq seq = 0;
};

class NarrativeEngine {
public:
    NarrativeEngine(Catalog cat, GameState st, FrontierWeights fw = {},
                    IntentWeights iw = {}, PromptBudget pb = {})
        : cat_(std::move(cat)), st_(std::move(st)),
          scorer_(std::move(fw)), matcher_(std::move(iw)),
          prompter_(std::move(pb)), rng_(st_.rng_state) {}

    const GameState& state() const noexcept { return st_; }
    GameState& mutable_state() noexcept { return st_; }
    const Catalog& catalog() const noexcept { return cat_; }
    FrontierScorer& scorer() noexcept { return scorer_; }
    IntentMatcher& matcher() noexcept { return matcher_; }
    PromptAssembler& prompter() noexcept { return prompter_; }

    void set_selector(ILlmSelector* s) noexcept { llm_ = s; }

    // ------------------------------------------------------------------
    TurnResult step(const std::string& user_input) {
        TurnResult tr;
        tr.from_node = st_.current_node;

        // ---- 0. 单调性断言 ----
        if (st_.seq < 0)
            throw NxError(NxError::Code::CursorRegress, "time_cursor 为负值，存档已损坏");

        const NodeSpec* current = cat_.find(st_.current_node);

        // ---- 1. 干净的 Frontier ----
        tr.frontier_before = scorer_.refresh(cat_, st_, nullptr, nullptr);

        // ---- 2. 三级漏斗 ----
        const NodeId default_next = current ? current->default_next : NodeId{};
        // 先看是不是点了选项按钮。**必须排在三级漏斗之前**：
        // 玩家点按钮是最强的确定性意图，为它调用 LLM 既浪费钱和延迟，
        // 又平白引入一次不必要的模型不确定性。
        NodeId picked = match_option(user_input, current);
        if (!picked.empty()) {
            tr.layer = "L0";
            tr.reason = "玩家点了选项按钮";
            tr.confidence = 1.0;
        } else {
            const IntentDecision d = matcher_.decide(user_input, cat_, tr.frontier_before.items,
                                                     default_next, llm_);
            tr.layer = d.layer;
            tr.reason = d.reason;
            tr.confidence = d.confidence;
            picked = d.node_id;
        }

        NodeId next = picked.empty() ? default_next : picked;
        if (next.empty()) next = st_.current_node;
        if (cat_.find(next) == nullptr) {
            tr.events.push_back("拦截：目标节点 '" + next + "' 不存在，留在原地");
            next = st_.current_node;
        }
        const NodeSpec& node = *cat_.find(next);

        // ---- 3. 确定性副作用 ----
        const int before = st_.affinity;
        const Effects& fx = node.effects;
        for (const auto& t : fx.add_tags)
            if (st_.tags.insert(t).second) tr.events.push_back("+tag " + t);
        for (const auto& t : fx.remove_tags)
            if (st_.tags.erase(t))          tr.events.push_back("-tag " + t);
        for (const auto& i : fx.add_items)
            if (st_.items.insert(i).second) tr.events.push_back("+item " + i);
        for (const auto& i : fx.remove_items)
            if (st_.items.erase(i))         tr.events.push_back("-item " + i);

        st_.affinity += static_cast<int>(fx.affinity_delta);
        if (st_.affinity < -100) st_.affinity = -100;
        if (st_.affinity > 200)  st_.affinity = 200;

        // 物理时间只能向前推进 —— 这里是"晚上不能吃午饭"的最后一道闸
        if (!fx.set_phase.empty()) {
            const int idx = cat_.phase_index_of(fx.set_phase);
            if (idx >= 0 && idx > st_.phase_index) {
                st_.phase = fx.set_phase;
                st_.phase_index = idx;
                tr.events.push_back("phase -> " + st_.phase);
            }
        }

        // ---- 4. 压入挂起池 ----
        if (current != nullptr) {
            std::vector<NodeId> alts;
            for (const auto& o : current->options) {
                if (o.next.empty() || o.next == next) continue;
                bool dup = false;
                for (const auto& a : alts) if (a == o.next) dup = true;
                if (!dup) alts.push_back(o.next);
            }
            for (const auto& a : alts)
                if (pool_.push(cat_, st_, a, current->id))
                    tr.events.push_back("pending <- " + a);
        }

        // ---- 5. 时间游标推进（唯一入口）----
        ++st_.seq;
        st_.rng_state = rng_.state();
        st_.visit_count[next] += 1;
        st_.last_visit[next] = st_.seq;
        st_.visited.emplace(next, st_.seq);
        st_.current_node = next;

        st_.history_tail.push_back({"你", user_input});
        st_.history_tail.push_back({"她", default_reply(node)});
        if (st_.history_tail.size() > 40)
            st_.history_tail.erase(st_.history_tail.begin(),
                                   st_.history_tail.end() - 40);

        // ---- 6. 后向指针：事件挽回 ----
        std::unordered_map<NodeId, PendingEntry> awake = pool_.scan(cat_, st_);
        for (const auto& kv : awake) {
            tr.redeemed.push_back(kv.first);
            tr.events.push_back("redeem " + kv.first + " (born@"
                                + std::to_string(kv.second.birth_seq) + " -> awake@"
                                + std::to_string(kv.second.awake_seq) + ")");
        }

        tr.to_node = next;
        tr.affinity_change = st_.affinity - before;
        tr.seq = st_.seq;
        return tr;
    }

    // 组装本步要发给 LLM 的 prompt。返回的白名单必须与 tr.routes_injected 一致，
    // 上层在做 L2 校验时要拿它做交集检查。
    PromptResult build_prompt(const std::string& npc_state) const {
        auto view = scorer_.refresh(cat_, st_, nullptr, nullptr);
        auto head = FrontierScorer::top_k(view, static_cast<std::size_t>(scorer_.weights().top_k));
        auto scored = matcher_.l1("", cat_, head);
        auto ids = matcher_.mmr(cat_, scored, 5u);
        std::vector<std::pair<NodeId, std::string>> routes;
        for (const auto& id : ids) routes.emplace_back(id, cat_.find(id)->intent.description);
        return prompter_.build("你扮演木子因，一个对哥哥有极端依赖的高中生。",
                               st_.phase, npc_state, st_.affinity,
                               st_.summary, st_.history_tail, routes);
    }

    bool is_over() const {
        const NodeSpec* n = cat_.find(st_.current_node);
        return n != nullptr && n->terminal;
    }

    // 调试用：直接看当前 time_cursor 下的 Frontier 与被拦截原因
    std::string debug_frontier() const {
        auto v = scorer_.refresh(cat_, st_, nullptr, nullptr);
        std::string s = "=== Frontier @ seq=" + std::to_string(st_.seq)
                      + " phase=" + st_.phase + " node=" + st_.current_node + " ===\n";
        for (const auto& c : v.items) s += "  IN    " + c.explain() + "\n";
        for (const auto& kv : v.blocked) {
            s += "  BLOCK[" + kv.first + "] ";
            for (const auto& id : kv.second) s += id + " ";
            s += "\n";
        }
        return s;
    }

private:
    Catalog cat_;
    GameState st_;
    FrontierScorer scorer_;
    IntentMatcher matcher_;
    PromptAssembler prompter_;
    PendingPool pool_;
    Rng rng_;
    ILlmSelector* llm_ = nullptr;

    static NodeId match_option(const std::string& input, const NodeSpec* cur) {
        if (cur == nullptr || cur->options.empty()) return {};
        if (input.empty() || input.size() > 3) return {};
        std::size_t val = 0;
        for (char ch : input) {
            if (ch < '0' || ch > '9') return {};
            val = val * 10 + static_cast<std::size_t>(ch - '0');
        }
        const std::size_t i = val;
        if (i < 1 || i > cur->options.size()) return {};
        return cur->options[i - 1].next;
    }

    static std::string default_reply(const NodeSpec& n) {
        if (!n.npc_text.empty()) return n.npc_text;
        if (!n.narrator.empty()) return n.narrator;
        return "……";
    }
};

}  // namespace nx
