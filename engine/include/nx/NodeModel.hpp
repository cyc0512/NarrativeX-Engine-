// ---------------------------------------------------------------------------
//  NarrativeX Engine :: nx/NodeModel.hpp
//  动态节点模型 + 剧本目录 + 倒排索引。
// ---------------------------------------------------------------------------
#pragma once

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Condition.hpp"
#include "Types.hpp"

namespace nx {

enum class NodeKind {
    Scripted,     // 作者编排的主线
    Scene,        //普通的演出/过场节点
    Emergent,     // 由隐藏状态动态涌现
    Redemption,   // 被后向指针挽回的旧事件
    Ending,       // 结局 / 主线收束
    Fallback,     // 兜底闲聊，防死锁
};

inline int bucket_of(NodeKind k) noexcept {
    switch (k) {
        case NodeKind::Ending:     return 40;
        case NodeKind::Scripted:   return 30;
        case NodeKind::Redemption: return 20;
        case NodeKind::Scene:      return 15;
        case NodeKind::Emergent:   return 10;
        case NodeKind::Fallback:   return 0;
    }
    return 0;
}

inline const char* kind_name(NodeKind k) noexcept {
    switch (k) {
        case NodeKind::Scripted:   return "scripted";
        case NodeKind::Scene:      return "scene"; 
        case NodeKind::Emergent:   return "emergent";
        case NodeKind::Redemption: return "redemption";
        case NodeKind::Ending:     return "ending";
        case NodeKind::Fallback:   return "fallback";
    }
    return "unknown";
}

struct OptionDef {
    std::string text;
    NodeId next;
};

struct RedeemVariant {
    bool has = false;
    std::string narrator, npc_text, npc_state;
};

struct Effects {
    std::vector<TagId> add_tags, remove_tags;
    std::vector<ItemId> add_items, remove_items;
    double affinity_delta = 0.0;
    std::string set_phase;              
    std::string summary_hint;           
};

struct IntentHint {
    std::string description;                 
    std::vector<std::string> keywords;       
    std::vector<std::string> examples;       
    std::vector<std::string> negatives;      
    bool must_freeform = false;              
};

// 👇 新增：动态调度触发条件结构体
// 用于声明某个隐藏节点在什么苛刻条件下可以被提前或重新拉入当前时间线
struct TriggerCondition {
    std::vector<TagId> tags_all;    // 必须同时拥有的标签
    std::vector<TagId> tags_any;    // 只需拥有其中之一的标签
    int min_step = 0;               // 时间线游标门槛 (seq 必须 >= min_step)
    NodeId require_node = "";       // 必须曾经去过的节点
    std::string required_phase = "";// 必须处于的阶段 (如 "night")
};

struct NodeSpec {
    NodeId id;
    NodeKind kind = NodeKind::Scripted;
    bool terminal = false;

    std::string narrator, npc_text, npc_state, hint;
    std::vector<OptionDef> options;
    NodeId default_next, fallback_next;

    RedeemVariant redeem_variant;

    // 支柱一：生命周期
    Seq min_step = 0;
    Seq max_step = 1000000000LL;

    // 物理时间闸门
    std::vector<std::string> phases;         
    int phase_index_min = 0;
    int phase_index_max = 1000000000;

    // 支柱二：准入条件
    Condition requires_fn = &always_true;

    // 排序参数
    int priority = 50;
    double weight = 1.0;
    int affinity_anchor = -1;
    bool once = true;
    int cooldown = 0;
    std::string exclusive_group;

    // 支柱三：可挽回性
    bool redeemable = false;
    Condition redeem_gate = &always_true;
    Seq redeem_expire = 1000000000LL;

    Effects effects;
    IntentHint intent;

    // 👇 新增：动态调度的两个核心字段
    TriggerCondition early_trigger; // 提前接入条件
    TriggerCondition late_recover;  // 过去重归条件

    // ---- 判定 helper ------------------------------------------------------
    int bucket() const noexcept { return bucket_of(kind); }

    bool window_ok(Seq seq) const noexcept {
        return seq >= min_step && seq <= max_step;
    }
    bool phase_ok(const std::string& phase, int phase_index) const {
        if (!phases.empty()) {
            bool found = false;
            for (const auto& p : phases) if (p == phase) { found = true; break; }
            if (!found) return false;
        }
        return phase_index >= phase_index_min && phase_index <= phase_index_max;
    }
    
    // 辅助函数：检查动态调度条件是否满足
    bool check_trigger(const TriggerCondition& tc, const struct GameState& st) const;
};

// ---------------------------------------------------------------------------
//  Catalog：剧本字典 + 倒排索引
// ---------------------------------------------------------------------------
class Catalog {
public:
    std::unordered_map<NodeId, NodeSpec> nodes;
    std::vector<std::string> phases;   

    void put(NodeSpec n) { nodes[n.id] = std::move(n); }

    const NodeSpec* find(const NodeId& id) const {
        auto it = nodes.find(id);
        return it == nodes.end() ? nullptr : &it->second;
    }

    int phase_index_of(const std::string& p) const {
        for (std::size_t i = 0; i < phases.size(); ++i) if (phases[i] == p) return static_cast<int>(i);
        return -1;
    }

    struct Index {
        std::unordered_map<TagId, std::unordered_set<NodeId>> by_tag;
        std::unordered_map<ItemId, std::unordered_set<NodeId>> by_item;
        std::unordered_set<NodeId> always;
        std::unordered_map<NodeId, std::unordered_set<NodeId>> neighbors;  
    };
    Index index;

    std::unordered_map<NodeId, std::vector<TagId>>  depends_tags;
    std::unordered_map<NodeId, std::vector<ItemId>> depends_items;

    void finalize_index() {
        index = Index();
        for (const auto& kv : nodes) {
            const NodeId& id = kv.first;
            const NodeSpec& n = kv.second;
            std::unordered_set<NodeId> out;
            for (const auto& o : n.options) if (!o.next.empty()) out.insert(o.next);
            if (!n.default_next.empty())  out.insert(n.default_next);
            if (!n.fallback_next.empty()) out.insert(n.fallback_next);
            index.neighbors[id] = std::move(out);

            auto dt = depends_tags.find(id);
            auto di = depends_items.find(id);
            bool has_dep = (dt != depends_tags.end() && !dt->second.empty())
                        || (di != depends_items.end() && !di->second.empty());
            if (!has_dep) {
                index.always.insert(id);
            } else {
                if (dt != depends_tags.end())
                    for (const auto& t : dt->second) index.by_tag[t].insert(id);
                if (di != depends_items.end())
                    for (const auto& i : di->second) index.by_item[i].insert(id);
            }
        }
    }

    std::vector<NodeId> candidate_ids(const std::unordered_set<TagId>& tags,
                                      const std::unordered_set<ItemId>& items,
                                      const NodeId& current) const {
        std::unordered_set<NodeId> acc = index.always;
        for (const auto& t : tags)  { auto it = index.by_tag.find(t);  if (it != index.by_tag.end())  acc.insert(it->second.begin(), it->second.end()); }
        for (const auto& i : items) { auto it = index.by_item.find(i); if (it != index.by_item.end()) acc.insert(it->second.begin(), it->second.end()); }
        auto nb = index.neighbors.find(current);
        if (nb != index.neighbors.end()) acc.insert(nb->second.begin(), nb->second.end());
        return std::vector<NodeId>(acc.begin(), acc.end());
    }
};

}  // namespace nx