// ---------------------------------------------------------------------------
//  NarrativeX Engine :: nx/GameState.hpp
//  可完整序列化的一份剧情状态 + 挂起事件池的数据结构。
//
//  三条铁律（写入 Excellence 检查表，任何改动都不得打破）：
//    IRON-1 时间游标单调：seq 只能 +1，永不回退。
//    IRON-2 历史只追加：状态变化以事件形式 append，禁止原地改写。
//    IRON-3 结果可复现：随机性全部来自随存档持久化的 rng_state。
// ---------------------------------------------------------------------------
#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Types.hpp"

namespace nx {

struct PendingEntry {
    NodeId node_id;
    Seq birth_seq = 0;        // 入池时刻（= 当时的 time_cursor）
    std::string reason;       // unselected | expired | locked
    NodeId source_node;       // 是在哪个岔路口被放弃的
    int attempts = 0;         // 被唤醒尝试的次数
    bool awake = false;
    Seq awake_seq = -1;       // 唤醒时刻，必然 > birth_seq（单调）
};

struct HistoryTurn {
    std::string role;         // "你" / "她"
    std::string content;
};

struct GameState {
    std::string session_id = "local";

    // ---- 支柱一：时间游标 ----
    Seq seq = 0;                       // == time_cursor
    std::string phase = "evening";
    int phase_index = 0;

    NodeId current_node = "0";
    int affinity = 90;

    // ---- 支柱二：判定依据 ----
    std::unordered_set<TagId> tags;
    std::unordered_set<ItemId> items;
    std::unordered_map<NodeId, Seq> visited;      // 首次进入的 seq
    std::unordered_map<NodeId, Seq> last_visit;   // 最近一次进入的 seq（冷却用）
    std::unordered_map<NodeId, int> visit_count;

    // ---- 支柱三：挂起池 ----
    std::vector<PendingEntry> pending;

    // ---- 可复现性 ----
    std::uint64_t rng_state = 0x12345678ABCDEF01ull;

    // ---- 长期记忆（抗 Lost-in-the-Middle）----
    std::string summary;
    std::vector<HistoryTurn> history_tail;

    int count_of(const NodeId& id) const {
        auto it = visit_count.find(id);
        return it == visit_count.end() ? 0 : it->second;
    }

    bool has_tag(const TagId& t) const { return tags.count(t) != 0; }
    bool has_item(const ItemId& i) const { return items.count(i) != 0; }
};

}  // namespace nx
