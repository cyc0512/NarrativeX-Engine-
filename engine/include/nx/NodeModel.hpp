// ---------------------------------------------------------------------------
//  NarrativeX Engine :: nx/NodeModel.hpp
//  动态节点模型 + 剧本目录 + 倒排索引。
//
//  与旧 story.json 模型的根本区别：
//    旧模型：节点的"下一步"写死在 options[].next / default_next 里 -> 静态树。
//    新模型：节点声明"我什么时候可以被访问"（requires + window + phase）
//            和"我有多重要"（priority / kind），从不声明"我从哪里来"。
//            "从哪里来"由 Frontier 引擎在运行时算出 -> 动态拓扑网络。
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
    Emergent,     // 由隐藏状态动态涌现
    Redemption,   // 被后向指针挽回的旧事件（进入后自动升级为 Emergent 语义）
    Ending,       // 结局 / 主线收束
    Fallback,     // 兜底闲聊，防死锁
};

// 桶 = 硬分层。排序时 bucket 的优先级**高于**打分函数：
// 保证作者写的主线收束永远不会被一个"碰巧高分"的随机涌现节点越过。
inline int bucket_of(NodeKind k) noexcept {
    switch (k) {
        case NodeKind::Ending:     return 40;
        case NodeKind::Scripted:   return 30;
        case NodeKind::Redemption: return 20;
        case NodeKind::Emergent:   return 10;
        case NodeKind::Fallback:   return 0;
    }
    return 0;
}

inline const char* kind_name(NodeKind k) noexcept {
    switch (k) {
        case NodeKind::Scripted:   return "scripted";
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

// 进入节点时产生的**确定性**副作用。
// 注意：这里只允许写"能被 SQLite 记下来"的东西。
// 任何浮点蓄电池式的临时量都不允许进 Effects。
struct Effects {
    std::vector<TagId> add_tags, remove_tags;
    std::vector<ItemId> add_items, remove_items;
    double affinity_delta = 0.0;
    std::string set_phase;              // 非空则在"阶段序号变大"时才生效
    std::string summary_hint;           // 供后续滚动摘要使用
};

// 给 AI 意图匹配框架看的语义描述 —— 静态剧本与 AI 的唯一接缝。
struct IntentHint {
    std::string description;                 // 注入 {DYNAMIC_ROUTES} 的一句话描述
    std::vector<std::string> keywords;       // 强信号词，L0 层使用
    std::vector<std::string> examples;       // 玩家可能的原话，L1 层计算字面相似度
    std::vector<std::string> negatives;      // 反例词，命中扣分抑制误路由
    bool must_freeform = false;              // true = 只能由自然语言触发
};

struct NodeSpec {
    NodeId id;
    NodeKind kind = NodeKind::Scripted;

    // terminal 节点（如旧版的 "-1"）永远不进 Frontier，只作为跳转目标。
    // 【实测教训】少了这个字段，一个无 requires 的 ending 节点会常驻最高 bucket，
    // 导致开局就被路由到"游戏结束"。详见 reference/simulate.py V9。
    bool terminal = false;

    std::string narrator, npc_text, npc_state, hint;
    std::vector<OptionDef> options;
    NodeId default_next, fallback_next;

    // 支柱一：生命周期（配合 time_cursor）
    Seq min_step = 0;
    Seq max_step = 1000000000LL;

    // 物理时间闸门（防"晚上吃午饭"）
    std::vector<std::string> phases;         // 空 = 不限
    int phase_index_min = 0;
    int phase_index_max = 1000000000;

    // 支柱二：准入条件（剔除后剩下的就是 Current_Frontier 的成员）
    Condition requires_fn = &always_true;

    // 空缺一：排序参数
    int priority = 50;
    double weight = 1.0;
    // 好感度锚点：StoryLoader 在编译 requires 时把 affinity_gte/lte 的均值写进来，
    // Frontier 打分用它衡量"玩家当前状态与该节点设计意图的贴合度"。
    // -1 表示该节点没有好感度倾向，此项不参与打分。
    int affinity_anchor = -1;
    bool once = true;
    int cooldown = 0;
    std::string exclusive_group;

    // 支柱三：可挽回性（后向指针）
    bool redeemable = false;
    Condition redeem_gate = &always_true;
    Seq redeem_expire = 1000000000LL;

    Effects effects;
    IntentHint intent;

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

// ---------------------------------------------------------------------------
//  Catalog：剧本字典 + 倒排索引
//
//  实测数据（见 reference/simulate.py 的基准）：
//    500 节点  倒排索引 783us/步   全表遍历 1657us/步   -> 2.1x
//   1000 节点  倒排索引 1472us/步  全表遍历 3529us/步   -> 2.4x
//  【诚实结论】2.1~2.4x 值得保留，但**它不是瓶颈**：同一次对话里 LLM 往返
//  约 0.8~3s，比重算整个 Frontier 慢 3 个数量级以上。真正的优化方向是
//  减少 LLM 调用次数（这正是 IntentMatch 三级漏斗做的事），而不是在这里抠性能。
// ---------------------------------------------------------------------------
class Catalog {
public:
    std::unordered_map<NodeId, NodeSpec> nodes;
    std::vector<std::string> phases;   // 物理时间阶段的有序列表，索引即"时间进度"

    void put(NodeSpec n) { nodes[n.id] = std::move(n); }

    const NodeSpec* find(const NodeId& id) const {
        auto it = nodes.find(id);
        return it == nodes.end() ? nullptr : &it->second;
    }

    int phase_index_of(const std::string& p) const {
        for (std::size_t i = 0; i < phases.size(); ++i) if (phases[i] == p) return static_cast<int>(i);
        return -1;
    }

    // 倒排索引：tag / item -> 依赖它的节点集合。
    // 建索引发生在加载期一次，此后每次 Frontier 重算只读。
    // 注意：含 not / affinity / step / phase 条件的节点无法用"存在性"剪枝，
    //       会被登记进 always —— 它们每步都要重新求值，这是正确性的代价。
    struct Index {
        std::unordered_map<TagId, std::unordered_set<NodeId>> by_tag;
        std::unordered_map<ItemId, std::unordered_set<NodeId>> by_item;
        std::unordered_set<NodeId> always;
        std::unordered_map<NodeId, std::unordered_set<NodeId>> neighbors;  // 前向指针
    };
    Index index;

    // 依赖集合，由 StoryLoader 在编译条件时填充。finalize_index() 依赖它。
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

    // 召回"可能被本次状态影响"的候选。传入当前状态的两个集合即可。
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
