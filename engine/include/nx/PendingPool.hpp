// ---------------------------------------------------------------------------
//  NarrativeX Engine :: nx/PendingPool.hpp
//  事件挽回 —— 迷宫回溯的后向指针。
//
//  "事件挽回"的本质是一次**受限回溯**：
//    允许把当时没走的那条路重新拉回现在的 Frontier；
//    绝不允许把 time_cursor 拉回过去。
//
//  两条硬约束：
//    C1 单调唤醒：awake_seq > birth_seq 恒成立，且 awake_seq == 当前 seq。
//       唤醒只写"当下"，绝不改写 birth_seq。
//    C2 动态重生成：唤醒时不回放旧文本。节点被重新放回 Frontier 后，
//       由 AI 依据**当前的** {phase} / {tags} / {recent history} 重新生成台词。
//       这就是"在过去的时间语境里，用现在的口径把这件事重讲一遍"。
// ---------------------------------------------------------------------------
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "GameState.hpp"
#include "NodeModel.hpp"
#include "Types.hpp"

namespace nx {

// 无状态工具类：Catalog 由调用方按次传入。刻意不持有引用/指针，
// 避免"对象被拷贝后内部指针悬空"这类隐蔽 bug。
class PendingPool {
public:
    // 把岔路口没走的那条路压入挂起池。幂等：同节点同一来源不会重复入池。
    bool push(const Catalog& cat, GameState& st, const NodeId& id, const NodeId& source,
              const std::string& reason = "unselected") {
        const NodeSpec* n = cat.find(id);
        if (n == nullptr) return false;          // 剧本改版后节点消失 -> 静默丢弃
        if (!n->redeemable) return false;        // 作者没声明可挽回 -> 不占池子
        for (const auto& p : st.pending)
            if (p.node_id == id && !p.awake) return false;   // 已在池里
        PendingEntry e;
        e.node_id = id;
        e.birth_seq = st.seq;
        e.reason = reason;
        e.source_node = source;
        st.pending.push_back(e);
        return true;
    }

    // 后向指针扫描：找出"现在终于满足激活条件"的挂起事件。
    // 返回值用于本步的 Frontier 打分加权（pending_age），同时就地更新 st.pending。
    std::unordered_map<NodeId, PendingEntry> scan(const Catalog& cat, GameState& st) {
        std::unordered_map<NodeId, PendingEntry> awakened;
        if (st.pending.empty()) return awakened;

        EvalContext ctx = make_ctx(st);
        std::vector<PendingEntry> alive;
        alive.reserve(st.pending.size());

        for (auto& e : st.pending) {
            const NodeSpec* n = cat.find(e.node_id);
            if (n == nullptr) continue;                       // 节点被删除 -> 出池

            // ---- 防御性检查：游标不得回退（正常流程不可能，但读外部存档要防）----
            if (st.seq < e.birth_seq) {
                throw NxError(NxError::Code::CursorRegress,
                    "挂起事件 [" + e.node_id + "] 的 birth_seq=" + std::to_string(e.birth_seq)
                    + " 超过了当前 time_cursor=" + std::to_string(st.seq)
                    + "，存档存在时空错乱，已拒绝继续");
            }
            // ---- 彻底过期 -> 销毁，不再占池 ----
            // 语义：redeem_expire 是"这件事在叙事上还能被提起多久"。
            // 过期判定**必须排在 awake 判定之前**：一个被唤醒、但玩家始终没有
            // 走上去的事件，同样应该过期销毁 —— 否则挂起池会无限增长（存档泄漏），
            // 且"错过就是错过"不成立（对照组 B0 的 B-6 断言正是要证明这一点）。
            // 旧实现把 awake 判定放在最前面，已唤醒条目会永久滞留；
            // Python 参考实现则是唤醒即丢弃（留档丢失）。现两侧统一为
            // "留档至过期为止"。
            if (st.seq - e.birth_seq > n->redeem_expire) continue;
            if (e.awake) { alive.push_back(e); continue; }    // 已挽回，留档到过期为止
            // 同一步不允许自我唤醒：至少要隔一轮，避免"选了就立刻反悔"的抖动
            if (st.seq - e.birth_seq < 1) { alive.push_back(e); continue; }

            ++e.attempts;
            if (n->redeem_gate(ctx)) {
                e.awake = true;
                e.awake_seq = st.seq;                          // C1：只写当下
                awakened[e.node_id] = e;
            }
            alive.push_back(e);
        }
        st.pending = std::move(alive);
        return awakened;
    }

    static std::size_t open_count(const GameState& st) {
        std::size_t n = 0;
        for (const auto& p : st.pending) if (!p.awake) ++n;
        return n;
    }

private:
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
};

}  // namespace nx
