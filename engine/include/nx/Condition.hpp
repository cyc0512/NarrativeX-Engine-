// ---------------------------------------------------------------------------
//  NarrativeX Engine :: nx/Condition.hpp
//  条件 DSL —— 动态节点能否进入 Frontier 的门槛判定。
//
//  设计取舍：C++ 内核里条件表达式以 std::function<bool(const EvalContext&)>
//  闭包形式存在，由 StoryLoader 从 JSON 编译而来。好处是求值零解析开销、
//  类型安全；代价是条件不可序列化回 JSON —— 因此 **剧本 JSON 才是唯一真相源**，
//  状态复现靠"同一份剧本 + 同一份事件流"，而不是靠序列化条件对象。
// ---------------------------------------------------------------------------
#pragma once

#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

#include "Types.hpp"

namespace nx {

// 条件求值所需的**只读**状态切片。
// 刻意做成值语义的快照：保证同一个 Frontier 重算过程中条件不会因为副作用漂移。
struct EvalContext {
    std::unordered_set<TagId> tags;
    std::unordered_set<ItemId> items;
    std::unordered_set<NodeId> visited;
    std::vector<std::pair<NodeId, int>> visit_count;
    int affinity = 0;
    Seq seq = 0;
    std::string phase;
    int phase_index = 0;

    int count_of(const NodeId& id) const {
        for (const auto& kv : visit_count) if (kv.first == id) return kv.second;
        return 0;
    }
};

using Condition = std::function<bool(const EvalContext&)>;

// 恒真条件。无 requires 的节点使用它。
inline bool always_true(const EvalContext&) { return true; }
inline bool always_false(const EvalContext&) { return false; }

// ---- 条件构造器（组合子）---------------------------------------------------
// StoryLoader 会把 JSON 里的 {"all":[...]} / {"tag":"x"} 等翻译成这些闭包的组合。
inline Condition all_of(std::vector<Condition> cs) {
    return [cs = std::move(cs)](const EvalContext& c) {
        for (const auto& x : cs) if (!x(c)) return false;
        return true;
    };
}
inline Condition any_of(std::vector<Condition> cs) {
    return [cs = std::move(cs)](const EvalContext& c) {
        for (const auto& x : cs) if (x(c)) return true;
        return false;
    };
}
inline Condition not_of(Condition c) {
    return [c = std::move(c)](const EvalContext& ctx) { return !c(ctx); };
}

inline Condition has_tag(TagId t) {
    return [t = std::move(t)](const EvalContext& c) { return c.tags.count(t) != 0; };
}
inline Condition has_any_tag(std::vector<TagId> ts) {
    return [ts = std::move(ts)](const EvalContext& c) {
        for (const auto& t : ts) if (c.tags.count(t)) return true;
        return false;
    };
}
inline Condition has_all_tags(std::vector<TagId> ts) {
    return [ts = std::move(ts)](const EvalContext& c) {
        for (const auto& t : ts) if (!c.tags.count(t)) return false;
        return true;
    };
}
inline Condition has_no_tag(std::vector<TagId> ts) {
    return [ts = std::move(ts)](const EvalContext& c) {
        for (const auto& t : ts) if (c.tags.count(t)) return false;
        return true;
    };
}
inline Condition affinity_at_least(int v) {
    return [v](const EvalContext& c) { return c.affinity >= v; };
}
inline Condition affinity_at_most(int v) {
    return [v](const EvalContext& c) { return c.affinity <= v; };
}
inline Condition has_item(ItemId i) {
    return [i = std::move(i)](const EvalContext& c) { return c.items.count(i) != 0; };
}
inline Condition has_all_items(std::vector<ItemId> is) {
    return [is = std::move(is)](const EvalContext& c) {
        for (const auto& i : is) if (!c.items.count(i)) return false;
        return true;
    };
}
inline Condition visited(NodeId n) {
    return [n = std::move(n)](const EvalContext& c) { return c.visited.count(n) != 0; };
}
inline Condition not_visited(NodeId n) {
    return [n = std::move(n)](const EvalContext& c) { return c.visited.count(n) == 0; };
}
inline Condition visit_count_at_least(NodeId n, int k) {
    return [n = std::move(n), k](const EvalContext& c) { return c.count_of(n) >= k; };
}
inline Condition step_at_least(Seq s) {
    return [s](const EvalContext& c) { return c.seq >= s; };
}
inline Condition step_at_most(Seq s) {
    return [s](const EvalContext& c) { return c.seq <= s; };
}
inline Condition phase_in(std::vector<std::string> ps) {
    return [ps = std::move(ps)](const EvalContext& c) {
        for (const auto& p : ps) if (c.phase == p) return true;
        return false;
    };
}
// 阶段单调约束的下游用法：要求已经推进到第 N 阶段以后。
// 这是防止"晚上吃午饭"这类物理时间倒错的硬闸门。
inline Condition phase_index_at_least(int idx) {
    return [idx](const EvalContext& c) { return c.phase_index >= idx; };
}

}  // namespace nx
