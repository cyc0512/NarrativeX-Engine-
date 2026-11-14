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
#include "GameState.hpp"
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

// v4：节点三分类。与 NodeKind（演出形态/优先级桶）正交。
//   - Fixed     固定节点：剧情的固定支架，时间线线性推进，绝不被移动。
//   - Dynamic   动态节点：需先"到过"init_after 锚点才武装（时间线闸门，天然禁止回退），
//               武装后还需满足 trigger 二次闸门才进入 Frontier。
//   - Preposable 可前置节点：从动态节点分化出来的稀缺类型，仅当 current_node 命中
//               insert_at 显式锚点时才允许插入当前时间线，随后同样受 trigger 约束。
enum class NodeType {
    Fixed,
    Dynamic,
    Preposable
};

inline const char* node_type_name(NodeType t) noexcept {
    switch (t) {
        case NodeType::Fixed:      return "fixed";
        case NodeType::Dynamic:    return "dynamic";
        case NodeType::Preposable: return "preposable";
    }
    return "unknown";
}

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
    std::string speaker = "木子因";   // 该节点 npc_text 的说话人：木子因/老板/客户/广播…
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

    // 👇 v4：节点类型三分类 + 时间线 + 触发/锚点模型
    //  type：Fixed / Dynamic / Preposable（见上方 enum 注释）。
    //  timeline：时间线序号，用于作者编排顺序与"禁止回退"校验；
    //            固定节点必填且 >0，动态/可前置节点缺省为 0（其推进由锚点/触发驱动）。
    //  init_after：动态节点初始化锚点；只有玩家"到过"该节点（时间线已越过）才武装。
    //  insert_at：可前置节点的显式锚点列表；仅当 current_node 命中其一才允许插入。
    //  trigger：武装/锚点就位后的二次触发闸门（标签/阶段/节点/步数），与 requires 正交。
    NodeType ntype = NodeType::Fixed;
    Seq timeline = 0;
    NodeId init_after = "";
    std::vector<NodeId> insert_at;
    TriggerCondition trigger;

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
};

// v4：评估一个 TriggerCondition 是否满足（armed/anchor 之后的二次闸门）。
// 空 condition（全默认值）视为恒真。与 Condition DSL（requires_fn）正交：
// trigger 不进入 candidate_ids 的 tag/item 依赖倒排，由 Frontier 在 gate 阶段单独求值。
inline bool trigger_satisfied(const TriggerCondition& tc, const GameState& st) {
    for (const auto& t : tc.tags_all) if (!st.has_tag(t)) return false;
    if (!tc.tags_any.empty()) {
        bool any = false;
        for (const auto& t : tc.tags_any) if (st.has_tag(t)) { any = true; break; }
        if (!any) return false;
    }
    if (st.seq < tc.min_step) return false;
    if (!tc.require_node.empty() && st.visited.count(tc.require_node) == 0) return false;
    if (!tc.required_phase.empty() && st.phase != tc.required_phase) return false;
    return true;
}

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

    // 👈 收敛候选集（修复"剧情乱"）：不再把 index.always（所有无 requires 依赖的节点）
    //    在每一步全局召回。候选仅限：当前节点的出边(neighbors) + 已满足的 tag/item 依赖节点。
    //    隐藏/动态节点由 EngineAdapter 的 early_trigger 扫描独立拉取，不走此处全局平铺。
    std::vector<NodeId> candidate_ids(const std::unordered_set<TagId>& tags,
                                      const std::unordered_set<ItemId>& items,
                                      const NodeId& current) const {
        std::unordered_set<NodeId> acc;
        auto nb = index.neighbors.find(current);
        if (nb != index.neighbors.end()) acc.insert(nb->second.begin(), nb->second.end());
        for (const auto& t : tags)  { auto it = index.by_tag.find(t);  if (it != index.by_tag.end())  acc.insert(it->second.begin(), it->second.end()); }
        for (const auto& i : items) { auto it = index.by_item.find(i); if (it != index.by_item.end()) acc.insert(it->second.begin(), it->second.end()); }
        return std::vector<NodeId>(acc.begin(), acc.end());
    }
};

}  // namespace nx