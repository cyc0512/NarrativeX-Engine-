// ---------------------------------------------------------------------------
//  NarrativeX Engine :: nx/StoryLoader.hpp
//  把 data/story_v2.json 编译成 Catalog 与 GameState。
//
//  这是整个内核**唯一依赖第三方头文件**的地方（nlohmann/json）。
//  保持这个边界的意义：nx 内核本身零依赖，可以被服务端 / CLI / 单测直接复用；
//  想换 JSON 库只需重写这一个文件。
// ---------------------------------------------------------------------------
#pragma once

#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "Frontier.hpp"
#include "GameState.hpp"
#include "IntentMatch.hpp"
#include "NodeModel.hpp"
#include "PromptAssembler.hpp"
#include "Types.hpp"

#include "json.hpp"          // 项目已 vendored 于 include/json.hpp (v3.10.1)

namespace nx {

class StoryLoader {
public:
    using Json = nlohmann::json;

    // 编译剧本。发现任何结构性错误立即抛 NxError::BadScript（fail-fast，
    // 绝不让一个坏剧本在运行时才崩）。
    static Catalog load(const std::string& json_text) {
        Json doc = Json::parse(json_text);
        Catalog cat;

        if (doc.contains("phases") && doc["phases"].is_array())
            for (const auto& p : doc["phases"]) cat.phases.push_back(p.get<std::string>());

        const NodeId entry = doc.contains("entry") ? doc["entry"].get<std::string>() : "0";

        if (!doc.contains("nodes") || !doc["nodes"].is_object())
            throw NxError(NxError::Code::BadScript, "剧本缺少 nodes 对象");

        for (auto it = doc["nodes"].begin(); it != doc["nodes"].end(); ++it) {
            const NodeId id = it.key();
            cat.put(compile_node(id, it.value(), cat));
        }

        cat.finalize_index();
        validate(cat, entry);
        return cat;
    }

    static GameState new_state(const std::string& session_id, int initial_affinity = 90,
                               const NodeId& entry = "0", std::uint64_t seed = 0xC0FFEEull) {
        GameState st;
        st.session_id = session_id;
        st.current_node = entry;
        st.affinity = initial_affinity;
        st.rng_state = seed;
        return st;
    }

    // 从快照 JSON 恢复（EventStore 读出来的那一坨）
    static GameState state_from_json(const std::string& text) {
        Json d = Json::parse(text);
        GameState st;
        st.session_id = d.value("session_id", "local");
        st.seq = d.value("seq", 0LL);
        st.phase = d.value("phase", "evening");
        st.phase_index = d.value("phase_index", 0);
        st.current_node = d.value("current_node", "0");
        st.affinity = d.value("affinity", 90);
        if (d.contains("tags"))  for (const auto& t : d["tags"])  st.tags.insert(t.get<std::string>());
        if (d.contains("items")) for (const auto& i : d["items"]) st.items.insert(i.get<std::string>());
        if (d.contains("visited"))
            for (auto it = d["visited"].begin(); it != d["visited"].end(); ++it)
                st.visited[it.key()] = it.value().get<Seq>();
        if (d.contains("last_visit"))
            for (auto it = d["last_visit"].begin(); it != d["last_visit"].end(); ++it)
                st.last_visit[it.key()] = it.value().get<Seq>();
        if (d.contains("visit_count"))
            for (auto it = d["visit_count"].begin(); it != d["visit_count"].end(); ++it)
                st.visit_count[it.key()] = it.value().get<int>();
        if (d.contains("pending")) {
            for (const auto& p : d["pending"]) {
                PendingEntry e;
                e.node_id = p.value("node_id", "");
                e.birth_seq = p.value("birth_seq", 0LL);
                e.reason = p.value("reason", "unselected");
                e.source_node = p.value("source_node", "");
                e.attempts = p.value("attempts", 0);
                e.awake = p.value("awake", false);
                e.awake_seq = p.value("awake_seq", -1LL);
                st.pending.push_back(e);
            }
        }
        st.rng_state = d.value("rng_state", 0xC0FFEEull);
        st.summary = d.value("summary", "");
        if (d.contains("history_tail"))
            for (const auto& h : d["history_tail"])
                st.history_tail.push_back({h.value("role", ""), h.value("content", "")});
        return st;
    }

    static std::string state_to_json(const GameState& st) {
        Json d;
        d["session_id"] = st.session_id;
        d["seq"] = st.seq;
        d["phase"] = st.phase;
        d["phase_index"] = st.phase_index;
        d["current_node"] = st.current_node;
        d["affinity"] = st.affinity;
        d["tags"] = Json::array();
        for (const auto& t : st.tags) d["tags"].push_back(t);
        d["items"] = Json::array();
        for (const auto& i : st.items) d["items"].push_back(i);
        d["visited"] = Json::object();
        for (const auto& kv : st.visited) d["visited"][kv.first] = kv.second;
        d["last_visit"] = Json::object();
        for (const auto& kv : st.last_visit) d["last_visit"][kv.first] = kv.second;
        d["visit_count"] = Json::object();
        for (const auto& kv : st.visit_count) d["visit_count"][kv.first] = kv.second;
        d["pending"] = Json::array();
        for (const auto& p : st.pending) {
            Json e;
            e["node_id"] = p.node_id; e["birth_seq"] = p.birth_seq; e["reason"] = p.reason;
            e["source_node"] = p.source_node; e["attempts"] = p.attempts;
            e["awake"] = p.awake; e["awake_seq"] = p.awake_seq;
            d["pending"].push_back(e);
        }
        d["rng_state"] = st.rng_state;
        d["summary"] = st.summary;
        d["history_tail"] = Json::array();
        for (const auto& h : st.history_tail) {
            Json t; t["role"] = h.role; t["content"] = h.content;
            d["history_tail"].push_back(t);
        }
        return d.dump();
    }

    // ---- 从剧本 JSON 读取可调参数（作者可以直接在 story.json 里调手感）----
    static FrontierWeights frontier_weights(const Json& doc) {
        FrontierWeights w;
        const Json& c = doc.contains("frontier") ? doc["frontier"] : doc;
        w.base = c.value("w_base", w.base);
        w.urgency = c.value("w_urgency", w.urgency);
        w.staleness = c.value("w_staleness", w.staleness);
        w.affinity = c.value("w_affinity", w.affinity);
        w.recency = c.value("w_recency", w.recency);
        w.cooldown = c.value("w_cooldown", w.cooldown);
        w.pending_age = c.value("w_pending_age", w.pending_age);
        w.staleness_tau = c.value("staleness_tau", w.staleness_tau);
        w.urgency_horizon = c.value("urgency_horizon", w.urgency_horizon);
        w.top_k = c.value("top_k", w.top_k);
        w.temperature = c.value("temperature", w.temperature);
        return w;
    }

    static IntentWeights intent_weights(const Json& doc) {
        IntentWeights w;
        const Json& c = doc.contains("intent") ? doc["intent"] : doc;
        w.kw_hit = c.value("kw_hit_score", w.kw_hit);
        w.kw_full_bonus = c.value("kw_full_bonus", w.kw_full_bonus);
        w.sim = c.value("sim_weight", w.sim);
        w.neg_penalty = c.value("neg_penalty", w.neg_penalty);
        w.l0_confident = c.value("l0_confident", w.l0_confident);
        w.l1_margin = c.value("l1_margin", w.l1_margin);
        w.mmr_lambda = c.value("mmr_lambda", w.mmr_lambda);
        w.top_k = c.value("top_k", w.top_k);
        return w;
    }

    static PromptBudget prompt_budget(const Json& doc) {
        PromptBudget b;
        const Json& c = doc.contains("budget") ? doc["budget"] : doc;
        b.persona = c.value("persona", b.persona);
        b.world = c.value("world", b.world);
        b.summary = c.value("summary", b.summary);
        b.history = c.value("history", b.history);
        b.routes = c.value("routes", b.routes);
        b.output = c.value("output", b.output);
        b.max_history_turns = c.value("max_history_turns", b.max_history_turns);
        return b;
    }

    // ---- 剧本诊断：启动期 lint，有问题直接列出全部 ----
    static std::vector<std::string> lint(const Catalog& cat, const NodeId& entry) {
        std::vector<std::string> errs;
        bool has_fallback = false;
        for (const auto& kv : cat.nodes) {
            const NodeSpec& n = kv.second;
            if (n.kind == NodeKind::Fallback) has_fallback = true;
            auto bad = [&](const std::string& field, const NodeId& tgt) {
                if (!tgt.empty() && cat.find(tgt) == nullptr)
                    errs.push_back("节点 " + n.id + " 的 " + field + "='" + tgt + "' 不存在");
            };
            bad("default_next", n.default_next);
            bad("fallback_next", n.fallback_next);
            for (const auto& o : n.options) bad("options.next", o.next);
            if (n.redeemable && n.redeem_expire <= 0)
                errs.push_back("节点 " + n.id + " redeemable=true 但 redeem_expire<=0，永不可能挽回");
            if (n.intent.description.empty() && n.kind != NodeKind::Fallback)
                errs.push_back("节点 " + n.id + " 缺少 intent.description，AI 将无法判断这条路通向什么");
        }
        if (!has_fallback)
            errs.push_back("剧本缺少 kind=fallback 的兜底节点：一旦 Frontier 重算为空就会死锁");
        if (cat.find(entry) == nullptr)
            errs.push_back("入口节点 '" + entry + "' 不存在");
        return errs;
    }

    static void validate(const Catalog& cat, const NodeId& entry) {
        auto errs = lint(cat, entry);
        if (!errs.empty()) {
            std::string msg = "剧本校验失败:\n";
            for (const auto& e : errs) msg += "  - " + e + "\n";
            throw NxError(NxError::Code::BadScript, msg);
        }
    }

private:

    struct Deps {
        std::vector<TagId> tags;
        std::vector<ItemId> items;
        long long affinity_sum = 0;
        int affinity_n = 0;
    };

    // 剧本是给人手写的，必须容忍 null / 类型写错。
    // 所有取值一律走这两个 helper，绝不在生产代码里裸调 get<T>()。
    static std::string opt_str(const Json& obj, const char* key, const std::string& def = "") {
        if (!obj.is_object() || !obj.contains(key)) return def;
        const Json& v = obj[key];
        if (v.is_null()) return def;
        if (!v.is_string())
            throw NxError(NxError::Code::BadScript,
                          std::string("字段 '") + key + "' 应为字符串，实际为 " + v.type_name());
        return v.get<std::string>();
    }
    static std::vector<std::string> opt_strs(const Json& obj, const char* key) {
        std::vector<std::string> v;
        if (!obj.is_object() || !obj.contains(key)) return v;
        const Json& a = obj[key];
        if (!a.is_array()) return v;
        for (const auto& x : a)
            if (x.is_string()) v.push_back(x.get<std::string>());
        return v;
    }

    static NodeSpec compile_node(const NodeId& id, const Json& raw, Catalog& cat) {
        NodeSpec n;
        n.id = id;
        n.kind = parse_kind(opt_str(raw, "kind", "scripted"));
        n.terminal = raw.value("terminal", false) || id == "-1";

        if (raw.contains("presentation") && raw["presentation"].is_object()) {
            const Json& p = raw["presentation"];
            n.narrator  = opt_str(p, "narrator");
            n.npc_text  = opt_str(p, "npc_text");
            n.npc_state = opt_str(p, "npc_state");
            n.hint      = opt_str(p, "hint");
        }
        if (raw.contains("options") && raw["options"].is_array())
            for (const auto& o : raw["options"]) {
                OptionDef d;
                d.text = opt_str(o, "text");
                d.next = opt_str(o, "next");
                n.options.push_back(d);
            }
        n.default_next  = opt_str(raw, "default_next");
        n.fallback_next = opt_str(raw, "fallback_next");

        if (raw.contains("window")) {
            const Json& w = raw["window"];
            n.min_step = w.value("min_step", 0LL);
            n.max_step = w.value("max_step", 1000000000LL);
        }
        if (raw.contains("phase") && raw["phase"].is_object()) {
            const Json& p = raw["phase"];
            n.phases = opt_strs(p, "allow");
            n.phase_index_min = p.value("min_index", 0);
            n.phase_index_max = p.value("max_index", 1000000000);
        }

        Deps deps;
        n.requires_fn = raw.contains("requires")
            ? compile_cond(raw["requires"], deps) : Condition(&always_true);
        cat.depends_tags[id] = deps.tags;
        cat.depends_items[id] = deps.items;
        n.affinity_anchor = deps.affinity_n > 0
            ? static_cast<int>(deps.affinity_sum / deps.affinity_n) : -1;

        n.priority = raw.value("priority", 50);
        n.weight = raw.value("weight", 1.0);
        n.once = raw.value("once", true);
        n.cooldown = raw.value("cooldown", 0);
        n.exclusive_group = opt_str(raw, "exclusive_group");

        n.redeemable = raw.value("redeemable", false);
        n.redeem_expire = raw.value("redeem_expire", 1000000000LL);
        if (raw.contains("redeem_gate")) {
            // 作者显式写了赎回条件 -> 按作者意图来
            Deps gdeps;                   // 独立统计，赎回条件的依赖不影响主索引
            n.redeem_gate = compile_cond(raw["redeem_gate"], gdeps);
        } else {
            // 没写 -> 退化为节点自身的准入条件，语义是：
            // "只要它重新变得可达，就允许被拉回当下的 Frontier"。
            // 这样 redeemable=true 就不会写成"永远救不回来"的死配置。
            n.redeem_gate = n.requires_fn;
        }

        if (raw.contains("effects") && raw["effects"].is_object()) {
            const Json& e = raw["effects"];
            n.effects.add_tags = opt_strs(e, "add_tags");
            n.effects.remove_tags = opt_strs(e, "remove_tags");
            n.effects.add_items = opt_strs(e, "add_items");
            n.effects.remove_items = opt_strs(e, "remove_items");
            n.effects.affinity_delta = e.value("affinity_delta", 0.0);
            n.effects.set_phase = opt_str(e, "set_phase");
            n.effects.summary_hint = opt_str(e, "summary_hint");
        }

        if (raw.contains("intent") && raw["intent"].is_object()) {
            const Json& it = raw["intent"];
            n.intent.description = it.contains("description") ? opt_str(it, "description") : n.hint;
            n.intent.keywords = opt_strs(it, "keywords");
            n.intent.examples = opt_strs(it, "examples");
            n.intent.negatives = opt_strs(it, "negatives");
            n.intent.must_freeform = it.value("must_freeform", false);
        }
        return n;
    }

    static NodeKind parse_kind(const std::string& s) {
        if (s == "scripted")   return NodeKind::Scripted;
        if (s == "emergent")   return NodeKind::Emergent;
        if (s == "redemption") return NodeKind::Redemption;
        if (s == "ending")     return NodeKind::Ending;
        if (s == "fallback")   return NodeKind::Fallback;
        throw NxError(NxError::Code::BadScript, "未知 kind: " + s);
    }

    // 条件编译器。未知算子 -> 直接抛错，避免拼错 key 导致条件静默恒真/恒假。
    static Condition compile_cond(const Json& c, Deps& deps) {
        if (!c.is_object())
            throw NxError(NxError::Code::BadScript, "条件必须是 JSON 对象");
        if (c.empty()) return Condition(&always_true);
        if (c.size() != 1)
            throw NxError(NxError::Code::BadScript, "每个条件对象只能有一个算子");

        const std::string op = c.begin().key();
        const Json& arg = c.begin().value();

        if (op == "true")  return Condition(&always_true);
        if (op == "false") return Condition(&always_false);

        if (op == "all" || op == "any") {
            std::vector<Condition> cs;
            if (!arg.is_array()) throw NxError(NxError::Code::BadScript, op + " 需要数组");
            for (const auto& sub : arg) cs.push_back(compile_cond(sub, deps));
            return op == "all" ? all_of(std::move(cs)) : any_of(std::move(cs));
        }
        if (op == "not") return not_of(compile_cond(arg, deps));

        auto list = [&]() {
            std::vector<std::string> v;
            if (!arg.is_array()) throw NxError(NxError::Code::BadScript, op + " 需要数组");
            for (const auto& x : arg) v.push_back(x.get<std::string>());
            return v;
        };
        auto num = [&]() {
            if (!arg.is_number()) throw NxError(NxError::Code::BadScript, op + " 需要数字");
            return arg.get<long long>();
        };

        if (op == "tag")       { auto s = arg.get<std::string>(); deps.tags.push_back(s); return has_tag(s); }
        if (op == "tags_all")  { auto v = list(); deps.tags.insert(deps.tags.end(), v.begin(), v.end()); return has_all_tags(v); }
        if (op == "tags_any")  { auto v = list(); deps.tags.insert(deps.tags.end(), v.begin(), v.end()); return has_any_tag(v); }
        if (op == "tag_none")  return has_no_tag(list());
        if (op == "item")      { auto s = arg.get<std::string>(); deps.items.push_back(s); return has_item(s); }
        if (op == "items_all") { auto v = list(); deps.items.insert(deps.items.end(), v.begin(), v.end()); return has_all_items(v); }

        if (op == "affinity_gte") { long long v = num(); deps.affinity_sum += v; ++deps.affinity_n; return affinity_at_least(static_cast<int>(v)); }
        if (op == "affinity_lte") { long long v = num(); deps.affinity_sum += v; ++deps.affinity_n; return affinity_at_most(static_cast<int>(v)); }

        if (op == "visited")    return visited(arg.get<std::string>());
        if (op == "not_visited") return not_visited(arg.get<std::string>());
        if (op == "visit_count_gte") {
            if (!arg.is_array() || arg.size() != 2)
                throw NxError(NxError::Code::BadScript, "visit_count_gte 需要 [nodeId, n]");
            return visit_count_at_least(arg[0].get<std::string>(), arg[1].get<int>());
        }
        if (op == "step_gte") return step_at_least(static_cast<Seq>(num()));
        if (op == "step_lte") return step_at_most(static_cast<Seq>(num()));
        if (op == "phase_in") return phase_in(list());
        if (op == "phase_index_gte") return phase_index_at_least(static_cast<int>(num()));

        throw NxError(NxError::Code::BadScript, "未知条件算子: " + op);
    }
};

}  // namespace nx
