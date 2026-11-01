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

#include <algorithm>
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

// 喂给 LLM 的候选池 / 白名单大小（判据见 build_prompt 注释：候选池必须**大于**短名单，
// 否则 MMR 退化为恒等变换、去冗余机制失效）。
//   kRoutePoolSize   = 先按 bucket+score 取前 12 个作为 MMR 的候选池
//   kRouteShortlist  = MMR 去冗余后真正注入 prompt 的白名单条数
constexpr std::size_t kRoutePoolSize = 12;
constexpr std::size_t kRouteShortlist = 5;

// 本步应当呈现给玩家的文本（旁白 / 台词 / NPC 状态）。
// is_redeem_variant=true 表示用的是 presentation.redeem_variant，即
// "这个节点是被后向指针拉回来的，台词按现在的口径重讲了一遍"。
struct Presentation {
    std::string narrator, npc_text, npc_state;
    bool is_redeem_variant = false;
};

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
    Presentation presentation;                     // 本步呈现的文本（B-5 断言来源）
    std::vector<NodeId> frontier_in;               // 本步 IN 列表（A-1/A-3/B-6/B-7 来源）
    std::unordered_map<std::string, std::vector<NodeId>> blocked;  // reason -> ids
    // 本轮 prompt 的**实测** token 数（estimate_tokens 算），硬上限 kMaxPromptTokens。
    // 注意这是"如果要发 L2 会发多大"，不等于实际消耗 —— 见 prompt_sent。
    int prompt_tokens = 0;
    // 本轮是否**真的**把 prompt 发给了 LLM。L0/L1 命中时不发请求，实耗 0 token；
    // 只有 L2 才会真的发出去。甲方"每轮 ≤1200 token"约束要的是本字段为 true 时
    // 的 prompt_tokens；离线档（llm_ == nullptr）全程为 false。
    bool prompt_sent = false;
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

        // ---- 2b. 本轮 prompt 的实测 token 数 ----
        // 必须排在副作用（第 3 步）**之前**采样：L2 若真的要发请求，用的就是此刻
        // 这份尚未被 effects 污染的 state；放在之后会算出"下一步"的 prompt。
        // prompt_sent 区分"会发多大"与"真的发了"：L0/L1 命中不发请求，实耗 0。
        tr.prompt_tokens = static_cast<int>(build_prompt(
            (current && !current->npc_state.empty()) ? current->npc_state : "平静"
        ).est_tokens);
        tr.prompt_sent = (tr.layer == "L2");

        // 抓取"本次进入之前"的 last_visit，供 present() 判断是否该用挽回变体。
        // 必须早于第 5 步的 visit 更新，否则刚写入的 seq 会让判定失效。
        Seq prev_last_visit = -1;
        {
            auto it0 = st_.last_visit.find(next);
            if (it0 != st_.last_visit.end()) prev_last_visit = it0->second;
        }

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
        tr.presentation = present(next, prev_last_visit);
        // 与 Python 参考实现对齐：`frontier_in` / `blocked` 记录的是**本步 refresh**
        // 的结果（副作用生效**之前**的干净 Frontier），而不是步进之后的状态。
        // 想看步进之后的 Frontier，请显式调 frontier_in_ids() / blocked_map()。
        tr.frontier_in.clear();
        tr.frontier_in.reserve(tr.frontier_before.items.size());
        for (const auto& c : tr.frontier_before.items) tr.frontier_in.push_back(c.id);
        tr.blocked.clear();
        for (const auto& kv : tr.frontier_before.blocked) {
            std::vector<NodeId> ids = kv.second;
            std::sort(ids.begin(), ids.end());
            tr.blocked[kv.first] = std::move(ids);
        }
        return tr;
    }

    // ------------------------------------------------------------------
    // 结构化 API：断言必须建立在这上面，不能靠 grep debug_frontier() 的字符串。
    // 原因：节点名互为前缀时（n5_search / n5_search_x）子串匹配会误判，
    //       且 C++ 与 Python 两份日志格式一旦漂移就无从比对。
    // ------------------------------------------------------------------

    // 当前 time_cursor 下 Frontier 的 IN 列表（已按 bucket/score/id 排好序）。
    std::vector<NodeId> frontier_in_ids() const {
        const FrontierScorer::FrontierView v = scorer_.refresh(cat_, st_, nullptr, nullptr);
        std::vector<NodeId> out;
        out.reserve(v.items.size());
        for (const auto& c : v.items) out.push_back(c.id);
        return out;
    }

    // {拦截原因: [节点 id...]}，每种原因内部按字典序排好，保证输出可 diff。
    std::unordered_map<std::string, std::vector<NodeId>> blocked_map() const {
        const FrontierScorer::FrontierView v = scorer_.refresh(cat_, st_, nullptr, nullptr);
        std::unordered_map<std::string, std::vector<NodeId>> out;
        for (const auto& kv : v.blocked) {
            std::vector<NodeId> ids = kv.second;
            std::sort(ids.begin(), ids.end());
            out[kv.first] = std::move(ids);
        }
        return out;
    }

    // 决定本步呈现哪一套文本：首次访问的 presentation，还是挽回变体。
    //
    // 判定规则（比 ARCH §4.2-(2) 原文的 `awake_seq == st_.seq` 更通用）：
    //     存在 pending 条目 e 使 e.node_id == id 且 e.awake，并且
    //         e.awake_seq == st_.seq              （本步刚被唤醒）
    //     或  e.awake_seq > prev_last_visit        （唤醒发生在上一次访问之后）
    //
    // ARCH 原文只覆盖第一种情形；但实际流程里"唤醒"发生在第 N 步的 step 6，
    // 玩家真正走进这个节点是第 N+1 步，此刻 awake_seq != st_.seq。
    // 只按原文实现的话，用例 B 的 n3_tender 会错误地回放首次台词，
    // B-5（不回放旧文本）直接失败。故补上第二种情形。
    Presentation present(const NodeId& id, Seq prev_last_visit = -1) const {
        Presentation p;
        const NodeSpec* n = cat_.find(id);
        if (n == nullptr) return p;

        bool use_variant = false;
        if (n->redeem_variant.has) {
            for (const auto& e : st_.pending) {
                if (e.node_id != id || !e.awake) continue;
                if (e.awake_seq == st_.seq || prev_last_visit < 0
                    || e.awake_seq > prev_last_visit) {
                    use_variant = true;
                    break;
                }
            }
        }
        if (use_variant) {
            p.narrator  = n->redeem_variant.narrator.empty()  ? n->narrator
                                                              : n->redeem_variant.narrator;
            p.npc_text  = n->redeem_variant.npc_text.empty()  ? n->npc_text
                                                              : n->redeem_variant.npc_text;
            p.npc_state = n->redeem_variant.npc_state.empty() ? n->npc_state
                                                              : n->redeem_variant.npc_state;
            p.is_redeem_variant = true;
            return p;
        }
        p.narrator = n->narrator;
        p.npc_text = n->npc_text;
        p.npc_state = n->npc_state;
        p.is_redeem_variant = false;
        return p;
    }

    // 组装本步要发给 LLM 的 prompt。返回的白名单必须与 tr.routes_injected 一致，
    // 上层在做 L2 校验时要拿它做交集检查。
    PromptResult build_prompt(const std::string& npc_state) const {
        // 候选池的取法（**判据来自机制意图层，不是"因为 Python 写的是 12"**）：
        //
        //   MMR 的设计意图是"从一个**大于短名单**的候选池里，在保留相关性的同时
        //   去掉语义重复项，最终取回 K 条"。当候选池大小 == K 时，MMR 退化为恒等
        //   变换 —— 池子里每一条都会被原样取回，去冗余这一步等于没跑，整个机制失效。
        //
        //   旧代码用 `scorer_.weights().top_k`（= K = 5）当候选池，正是这个失效形态。
        //   故候选池必须显式大于短名单：`kRoutePoolSize(12) > kRouteShortlist(5)`。
        //   打分口径也据此定为 Frontier 的 `c.score`（旧代码用 `matcher_.l1("", ...)`
        //   拿空输入重算一遍分，空输入下 l1 分与 c.score 不同，MMR 排序会无故漂移）。
        //
        //   ⚠ 这条改动的判据是"MMR 不能退化"，属 ARCH 机制层；不要把本注释读成
        //     "为了和 Python 对齐才改成 12" —— 规范来自设计与意图层，参考实现只做回归锁。
        auto view = scorer_.refresh(cat_, st_, nullptr, nullptr);
        auto head = FrontierScorer::top_k(view, kRoutePoolSize);
        std::vector<std::pair<NodeId, double>> scored;
        scored.reserve(head.size());
        for (const auto& c : head) scored.emplace_back(c.id, c.score);
        auto ids = matcher_.mmr(cat_, scored, kRouteShortlist);
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
