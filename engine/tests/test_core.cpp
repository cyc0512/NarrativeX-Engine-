// ---------------------------------------------------------------------------
//  nx 内核冒烟测试 + 跨语言一致性校验 + 指针用例断言
//
//  编译（MinGW / GCC / Clang 均可，纯 header-only）：
//    cd engine && g++ -std=c++17 -I../include -Wall -Wextra tests/test_core.cpp -o tests/test_core
//
//  运行（必须在 engine/ 下，剧本路径是相对 ../../data 的）：
//    cd engine/tests && ./test_core
//
//  三块内容：
//    ① 旧样例剧本 story_v2.example.json 的冒烟 + 三条铁律断言（保持不退化）
//    ② 新剧本 story_v2.json 的指针用例断言（A-1~A-7 / B-1~B-8），
//       与 reference/cases_pointer.py 的断言一一对应
//    ③ 归一化轨迹输出，与 `python3 reference/cases_pointer.py --emit-trace`
//       逐行 diff（两侧都跑 `tr -d '\r'` 归一化换行后再比）
// ---------------------------------------------------------------------------
#include <algorithm>
#include <cassert>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "nx.hpp"

using namespace nx;

namespace {

int g_fail = 0;
int g_pass = 0;

// 断言宏：既打印也给退出码，不依赖 NDEBUG（assert 在 Release 下会被裁掉）。
#define CHECK(cond, name)                                                     \
    do {                                                                      \
        if (cond) {                                                           \
            ++g_pass;                                                         \
            std::cout << "  [OK] " << (name) << "\n";                         \
        } else {                                                              \
            ++g_fail;                                                         \
            std::cout << "  [!!] " << (name) << "\n";                         \
        }                                                                     \
    } while (0)

#define CHECK_D(cond, name, detail)                                           \
    do {                                                                      \
        if (cond) {                                                           \
            ++g_pass;                                                         \
            std::cout << "  [OK] " << (name) << "  :: " << (detail) << "\n";  \
        } else {                                                              \
            ++g_fail;                                                         \
            std::cout << "  [!!] " << (name) << "  :: " << (detail) << "\n";  \
        }                                                                     \
    } while (0)

std::string read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        std::cerr << "无法打开文件: " << p << "\n";
        std::exit(2);
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string join_sorted(const std::unordered_set<std::string>& s) {
    std::vector<std::string> v(s.begin(), s.end());
    std::sort(v.begin(), v.end());
    std::string out;
    for (const auto& x : v) { if (!out.empty()) out += ","; out += x; }
    return out;
}

std::string join(const std::vector<std::string>& v) {
    std::string out;
    for (const auto& x : v) { if (!out.empty()) out += ","; out += x; }
    return out;
}

bool contains(const std::vector<std::string>& v, const std::string& x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}

bool contains_event(const std::vector<std::string>& events, const std::string& sub) {
    for (const auto& e : events) if (e.find(sub) != std::string::npos) return true;
    return false;
}

struct StubSelector : ILlmSelector {
    NodeId select(const std::string&, const std::vector<Candidate>& shortlist) override {
        ++calls;
        const Candidate* best = nullptr;
        for (const auto& c : shortlist)
            if (best == nullptr || c.score > best->score) best = &c;
        return best ? best->id : NodeId{};
    }
    int calls = 0;
};

// ---- 一步的记录（与 Python 侧 StepRec 字段一一对应）----------------------
struct StepRec {
    std::string from, to, layer, phase;
    Seq seq = 0;
    int aff = 0;
    int phase_index = 0;
    std::vector<std::string> tags, items, frontier_in, redeemed;
    bool variant = false;
    int prompt_tokens = 0;      // 本轮 prompt 实测 token 数（硬上限 kMaxPromptTokens）
    bool prompt_sent = false;   // 是否真的发给 LLM（只有 L2 会发）
};

std::string brief(const std::string& c, int step, const StepRec& r) {
    std::ostringstream o;
    o << "  " << c << " step" << step << " " << r.from << " -> " << r.to
      << " [" << r.layer << "] seq=" << r.seq << " aff=" << r.aff
      << " phase=" << r.phase << (r.variant ? "  [挽回变体]" : "");
    return o.str();
}


std::string trace_line(const std::string& cname, int step, const StepRec& r) {
    std::vector<std::string> rd = r.redeemed;
    std::sort(rd.begin(), rd.end());
    std::vector<std::string> tg = r.tags, it = r.items;
    std::sort(tg.begin(), tg.end());
    std::sort(it.begin(), it.end());
    std::ostringstream o;
    o << "case=" << cname
      << " step=" << step
      << " seq=" << r.seq
      << " from=" << r.from
      << " to=" << r.to
      << " layer=" << r.layer
      << " aff=" << r.aff
      << " phase=" << r.phase
      << " phase_index=" << r.phase_index
      << " tags=[" << join(tg) << "]"
      << " items=[" << join(it) << "]"
      << " in=[" << join(r.frontier_in) << "]"
      << " redeemed=[" << join(rd) << "]"
      << " variant=" << (r.variant ? 1 : 0)
      << " prompt_tokens=" << r.prompt_tokens
      << " prompt_sent=" << (r.prompt_sent ? 1 : 0);
    return o.str();
}

// ---------------------------------------------------------------------------
//  新剧本：指针用例（与 reference/cases_pointer.py 完全同构）
// ---------------------------------------------------------------------------

// 写死的输入（PRD §6.0.0 纪律：禁止中性文本推进）
//
// v1.7 与 reference/cases_pointer.py 对齐：第 2 步由「你口袋里藏的是**什么**？」
// 改为「你口袋里藏了**什么**？」（= n5_search.examples[0]）。
// 两个字符串在 keyword 改为 `藏` 之后命中数相同（口袋 + 藏 = 2/4 -> L0），
// 之所以仍要改，是为了让 C++ / Python 两侧的用例脚本逐字一致，
// 否则任何一侧的输入漂移都会让跨语言 diff 失去意义。
const std::vector<std::string> CASE_A_INPUTS = {
    "1", "你口袋里藏了什么？", "这发卡上面的血是谁的？",
};
const std::vector<std::string> BASE_A0_INPUTS = {
    "1", "对不起，我今天真的有点累", "1", "该睡了，晚安。", "嗯，回头见。",
};
const std::vector<std::string> CASE_B_INPUTS = {
    "1", "没什么，你别管了。", "（沉默）", "其实……我今天真的很累。",
    "那……晚安吧。", "嗯，回头见。",
};
// 对照组 B0：PRD §6.2 的玩家输入是 **4 条**（步 1/2/3 + 第 5 步改成
// 「算了，当我没说」）—— 中间那一行是引擎扫描，不是玩家输入。
// 早期版本在这里多插了一条「那……晚安吧。」，导致 C++ 与 Python 两侧的
// B0 脚本不同构、连 seq 时间线都对不上（实测会在 seq=7 分叉：
// C++ 走 n3_tender、Python 走 n5_search）。现与 cases_pointer.py 对齐。
const std::vector<std::string> CASE_B0_PREFIX = {
    "1", "没什么，你别管了。", "（沉默）", "算了，当我没说",
};
const std::string CASE_B0_FILLER = "算了，当我没说";

struct Engine {
    NarrativeEngine eng;
    explicit Engine(const std::string& sid)
        : eng(make(sid)) {}

    static NarrativeEngine make(const std::string& sid) {
        const std::string text = read_file("../../data/story_v2.json");
        Catalog cat = StoryLoader::load(text);
        nlohmann::json doc = nlohmann::json::parse(text);
        GameState st = StoryLoader::new_state(sid, 90, "n1_entry", 0xC0FFEEull);
        return NarrativeEngine(cat, st,
                               StoryLoader::frontier_weights(doc),
                               StoryLoader::intent_weights(doc),
                               StoryLoader::prompt_budget(doc));
    }

    StepRec step(const std::string& input) {
        const NodeId before = eng.state().current_node;
        TurnResult r = eng.step(input);
        const GameState& s = eng.state();
        StepRec rec;
        rec.from = before;
        rec.to = s.current_node;
        rec.layer = r.layer;
        rec.seq = s.seq;
        rec.aff = s.affinity;
        rec.phase = s.phase;
        rec.phase_index = s.phase_index;
        rec.tags = std::vector<std::string>(s.tags.begin(), s.tags.end());
        rec.items = std::vector<std::string>(s.items.begin(), s.items.end());
        rec.frontier_in = r.frontier_in;
        rec.redeemed = r.redeemed;
        rec.variant = r.presentation.is_redeem_variant;
        rec.prompt_tokens = r.prompt_tokens;
        rec.prompt_sent = r.prompt_sent;
        return rec;
    }
};

std::vector<StepRec> run_all(Engine& e, const std::vector<std::string>& inputs) {
    std::vector<StepRec> out;
    for (const auto& s : inputs) out.push_back(e.step(s));
    return out;
}

// A-1/A-2/A-3/B-6/B-7 都依赖"步进之后"的结构化 Frontier
std::vector<std::string> frontier_after(Engine& e) { return e.eng.frontier_in_ids(); }
std::unordered_map<std::string, std::vector<std::string>> blocked_after(Engine& e) {
    return e.eng.blocked_map();
}

bool is_ending(const std::string& id) {
    return id.rfind("end_", 0) == 0 || id == "-1";
}

const StepRec* first_ending(const std::vector<StepRec>& tr) {
    for (const auto& r : tr) if (is_ending(r.to)) return &r;
    return nullptr;
}

// 甲方硬约束：每轮实际发出的 prompt ≤ 1200 token。
// 上限直接引用 nx::kMaxPromptTokens，绝不在这里另写一份魔法数字。
void check_prompt_cap(const std::string& tag, const std::vector<StepRec>& tr) {
    int mx = 0, sent = 0;
    std::string per;
    for (const auto& r : tr) {
        if (r.prompt_tokens > mx) mx = r.prompt_tokens;
        if (r.prompt_sent) ++sent;
        if (!per.empty()) per += ",";
        per += std::to_string(r.prompt_tokens);
    }
    CHECK_D(mx <= nx::kMaxPromptTokens,
            tag + " 每轮 prompt token <= " + std::to_string(nx::kMaxPromptTokens)
                + "（甲方硬约束）",
            "最大值=" + std::to_string(mx) + "，占上限 "
                + std::to_string(mx * 100 / nx::kMaxPromptTokens) + "%，逐步=[" + per
                + "]，实发步数=" + std::to_string(sent));
}

// ---------------------------------------------------------------------------
void case_forward_pointer() {
    std::cout << "\n=== 用例 A：前向指针 —— n7_goodnight 被提前拉进可达集合 ===\n";
    Engine eng("A");
    std::vector<StepRec> tr = run_all(eng, CASE_A_INPUTS);
    for (std::size_t i = 0; i < tr.size(); ++i)
        std::cout << brief("A", static_cast<int>(i) + 1, tr[i]) << "\n";

    // A-5 单调性
    std::vector<Seq> seqs{0};
    std::vector<int> pidx{0};
    for (const auto& r : tr) { seqs.push_back(r.seq); pidx.push_back(r.phase_index); }
    bool mono = true;
    for (std::size_t i = 0; i + 1 < seqs.size(); ++i)
        if (!(seqs[i + 1] > seqs[i] && pidx[i + 1] >= pidx[i])) mono = false;
    CHECK(mono, "A-5 单调性未破坏（seq 严格递增，phase_index 只增不减）");

    // A-6：n5_search 在 seq=1 时 feasible
    CHECK_D(contains(tr[1].frontier_in, "n5_search"),
            "A-6 n5_search 在 seq=1 时 feasible（step_gte:1 生效）",
            "in=[" + join(tr[1].frontier_in) + "]");

    // A-1 / A-2：seq=2 且 phase=evening 的那一刻。
    // 游标只增不减，无法"回到"seq=2，故单独重跑到第 2 步后立刻取样。
    Engine eng2("A");
    StepRec r1 = eng2.step(CASE_A_INPUTS[0]);
    StepRec r2 = eng2.step(CASE_A_INPUTS[1]);
    const std::vector<std::string> in2 = frontier_after(eng2);
    const auto blocked2 = blocked_after(eng2);
    bool a1 = (r2.seq == 2) && (r2.phase == "evening") && contains(in2, "n7_goodnight");
    CHECK_D(a1, "A-1 seq=2 且 phase=evening 时 n7_goodnight ∈ frontier_in",
            "seq=" + std::to_string(r2.seq) + " phase=" + r2.phase
            + " aff=" + std::to_string(r2.aff) + " in=[" + join(in2) + "]");

    auto bp = blocked2.find("phase");
    bool a2 = (r2.phase_index == 0) &&
              (bp == blocked2.end() || !contains(bp->second, "n7_goodnight"));
    CHECK_D(a2, "A-2 它不是靠 phase 进来的（phase_index==0 且不在 BLOCK[phase] 里）",
            "phase_index=" + std::to_string(r2.phase_index));

    const StepRec* endA = first_ending(tr);
    CHECK_D(endA != nullptr && endA->seq <= 4,
            "A-4 用例 A 抵达 ending 时 seq <= 4",
            endA ? ("抵达 " + endA->to + " at seq=" + std::to_string(endA->seq))
                 : std::string("未抵达"));

    // ---- 基线 A0（对照组）----
    std::cout << "  -- 基线 A0（坦白线，对照组）--\n";
    Engine eng0("A0");
    std::vector<StepRec> tr0 = run_all(eng0, BASE_A0_INPUTS);
    for (std::size_t i = 0; i < tr0.size(); ++i)
        std::cout << brief("A0", static_cast<int>(i) + 1, tr0[i]) << "\n";

    Engine eng0b("A0");
    eng0b.step(BASE_A0_INPUTS[0]);
    StepRec r0_2 = eng0b.step(BASE_A0_INPUTS[1]);
    const std::vector<std::string> in0_2 = frontier_after(eng0b);
    CHECK_D((r0_2.seq == 2) && !contains(in0_2, "n7_goodnight"),
            "A-3 对照组在 seq=2（aff=95）时 n7_goodnight ∉ frontier_in",
            "aff=" + std::to_string(r0_2.aff) + " in=[" + join(in0_2) + "]");

    CHECK_D(tr0.size() >= 4 && tr0[3].to == "n7_goodnight",
            "A-7a 基线第 4 步 to_node == n7_goodnight（未被 end_calm 抢跑）",
            tr0.size() >= 4 ? tr0[3].to : std::string("-"));
    const StepRec* end0 = first_ending(tr0);
    CHECK_D(end0 != nullptr && end0->seq == 5,
            "A-7b 基线抵达 ending 时 seq == 5",
            end0 ? ("抵达 " + end0->to + " at seq=" + std::to_string(end0->seq))
                 : std::string("未抵达"));

    // ---- 甲方硬约束：每轮 prompt token ≤ 1200 ----
    check_prompt_cap("A-8", tr);
    check_prompt_cap("A-8(A0 基线)", tr0);
}

void case_backward_pointer() {
    std::cout << "\n=== 用例 B：后向指针 —— n3_tender 被错过后重新拉回 ===\n";
    Engine eng("B");
    std::vector<StepRec> tr = run_all(eng, CASE_B_INPUTS);
    for (std::size_t i = 0; i < tr.size(); ++i)
        std::cout << brief("B", static_cast<int>(i) + 1, tr[i]) << "\n";

    // B-1 / B-2 / B-3 需要 events 与 pending，重跑一次以便逐步取样
    Engine e2("B");
    TurnResult t1 = e2.eng.step(CASE_B_INPUTS[0]);
    TurnResult t2 = e2.eng.step(CASE_B_INPUTS[1]);
    CHECK_D(contains_event(t2.events, "pending <- n3_tender"),
            "B-1 step2 events 含 'pending <- n3_tender'", join(t2.events));

    TurnResult t3 = e2.eng.step(CASE_B_INPUTS[2]);
    CHECK_D(contains_event(t3.events, "redeem n3_tender (born@1 -> awake@3)")
            && contains(t3.redeemed, "n3_tender"),
            "B-2 step4 events【含】'redeem n3_tender (born@1 -> awake@3)'",
            join(t3.events));

    // B-3 C1 单调唤醒
    const PendingEntry* pe = nullptr;
    for (const auto& p : e2.eng.state().pending)
        if (p.node_id == "n3_tender") { pe = &p; break; }
    CHECK_D(pe != nullptr && pe->awake && pe->awake_seq > pe->birth_seq
            && pe->awake_seq == e2.eng.state().seq && pe->birth_seq == 1,
            "B-3 C1 单调唤醒：awake_seq(3) > birth_seq(1)，awake_seq == st.seq，birth_seq 仍为 1",
            pe ? ("birth=" + std::to_string(pe->birth_seq)
                  + " awake=" + std::to_string(pe->awake_seq)
                  + " seq=" + std::to_string(e2.eng.state().seq))
               : std::string("pending 中已无该条目"));

    // B-4 游标不回退
    std::vector<Seq> seqs{0};
    for (const auto& r : tr) seqs.push_back(r.seq);
    bool mono = true;
    for (std::size_t i = 0; i + 1 < seqs.size(); ++i)
        if (seqs[i + 1] <= seqs[i]) mono = false;
    auto lv = eng.eng.state().last_visit.find("n3_tender");
    CHECK_D(mono && lv != eng.eng.state().last_visit.end() && lv->second == 4,
            "B-4 游标不回退：seq 严格递增，且 last_visit['n3_tender']==4（不是 1）",
            "last_visit=" + (lv == eng.eng.state().last_visit.end()
                             ? std::string("-") : std::to_string(lv->second)));

    // B-5 不回放旧文本
    bool b5 = tr.size() >= 4 && tr[3].to == "n3_tender" && tr[3].variant;
    CHECK_D(b5, "B-5 不回放旧文本：n3_tender 呈现 redeem_variant",
            "variant=" + std::string(tr[3].variant ? "1" : "0"));

    // B-7 路径不断裂
    Engine e3("B7");
    e3.step(CASE_B_INPUTS[0]);
    e3.step(CASE_B_INPUTS[1]);
    e3.step(CASE_B_INPUTS[2]);
    e3.step(CASE_B_INPUTS[3]);            // -> n3_tender（seq=4）
    const std::vector<std::string> in4 = frontier_after(e3);
    CHECK_D(!in4.empty() && contains(in4, "n7_goodnight"),
            "B-7 路径不断裂：seq=4 时 frontier_in 非空且 n7_goodnight 可达",
            "in=[" + join(in4) + "]");

    // B-8 挽回线不被 n5_search 抢走
    CHECK_D(tr.size() >= 5 && tr[4].to == "n7_goodnight",
            "B-8a 第 5 步 to_node == n7_goodnight（未被 n5_search 抢走）",
            tr.size() >= 5 ? tr[4].to : std::string("-"));
    CHECK_D(!eng.eng.state().has_item("item_hairclip"),
            "B-8b 全程未获得 item_hairclip",
            "items=[" + join_sorted(eng.eng.state().items) + "]");
    const StepRec& fin = tr.back();
    CHECK_D(fin.to == "end_calm" && fin.aff == 84,
            "B-8c 最终 to_node == end_calm 且 affinity == 84",
            "to=" + fin.to + " aff=" + std::to_string(fin.aff));

    // ---- 甲方硬约束：每轮 prompt token ≤ 1200 ----
    check_prompt_cap("B-9", tr);
}

void case_backward_pointer_neg() {
    std::cout << "\n=== 对照组 B0：错过就是错过（B-6 不可挽回）===\n";

    // ⚠️ 实测发现 PRD §6.2 的 B-6a/B-6b 与"最终落 end_regret"在**真实玩法下互斥**：
    // 走到 n6_silence(seq=3) 时 regret 已到手且 aff=77 <= 81，end_regret 立刻 feasible，
    // 而 bucket_of(Ending)=40 > Scripted=30 —— 于是 seq=3 之后任何输入都会被它收走，
    // B0 在 **seq=4** 就结束了，真实玩法里永远到不了 seq>11。
    // 处理方式与 Python 侧 cases_pointer.py 完全一致（不改断言口径，只拆成两个探针）：
    //   探针 1 = 真实玩法，一到 ending 就停  -> B-6c / B-6d / B-6e / B-6f
    //   探针 2 = 强行越过 ending 继续推进    -> B-6a / B-6b（仅验证过期销毁语义）

    // ================= 探针 1：真实玩法（一到 ending 就停）=================
    Engine eng("B0");
    std::vector<StepRec> tr = run_all(eng, CASE_B0_PREFIX);
    for (std::size_t i = 0; i < tr.size(); ++i)
        std::cout << brief("B0", static_cast<int>(i) + 1, tr[i]) << "\n";

    const StepRec& last = tr.back();
    CHECK_D(last.to == "end_regret",
            "B-6c 对照组最终落 end_regret（与用例 B 的 end_calm 形成对照）",
            "to=" + last.to + " aff=" + std::to_string(last.aff)
            + " seq=" + std::to_string(last.seq));
    CHECK_D(last.to != "n3_tender",
            "B-6d 第 5 步「算了，当我没说」没有回到 n3_tender（错过即错过）",
            "to=" + last.to + " seq=" + std::to_string(last.seq));
    CHECK_D(last.seq == 4,
            "B-6e 对照组在 seq=4 即收束（end_regret 自 seq=3 起 feasible，bucket=40 压过一切）"
            " —— 这是 B-6a/B-6b 在真实玩法里到不了 seq>11 的直接证据",
            "seq=" + std::to_string(last.seq) + " node=" + last.to);
    bool b6f = eng.eng.state().visited.find("n3_tender") == eng.eng.state().visited.end();
    CHECK_D(b6f, "B-6f B0 真实玩法全程从未进入 n3_tender（错过的那条路始终没被补上）",
            "seq=" + std::to_string(eng.eng.state().seq));

    // ============ 探针 2：过期销毁语义（强行越过 ending 继续推进）============
    Engine e2("B0-expire");
    std::vector<StepRec> tr2 = run_all(e2, CASE_B0_PREFIX);
    int guard = 0;
    bool b6a = true;
    std::string last_in;
    while (e2.eng.state().seq <= 11 && guard < 30) {
        tr2.push_back(e2.step(CASE_B0_FILLER));
        ++guard;
        if (e2.eng.state().seq > 11) {
            const std::vector<std::string> in = frontier_after(e2);
            if (contains(in, "n3_tender")) b6a = false;
            last_in = join(in);
        }
    }
    for (std::size_t i = 0; i < tr2.size(); ++i)
        std::cout << brief("B0*", static_cast<int>(i) + 1, tr2[i]) << "\n";

    CHECK_D(b6a, "B-6a [内核探针·非真实玩法] seq>11 后 n3_tender 不再出现在任何 frontier_in",
            "最后时刻 in=[" + last_in + "]");

    bool b6b = true;
    for (const auto& p : e2.eng.state().pending)
        if (p.node_id == "n3_tender") b6b = false;
    CHECK_D(b6b, "B-6b [内核探针·非真实玩法] seq>11 后 n3_tender 已移出 pending"
                 "（redeem_expire=10 到期销毁）",
            "seq=" + std::to_string(e2.eng.state().seq));

    // ---- 甲方硬约束：每轮 prompt token ≤ 1200 ----
    check_prompt_cap("B0-2", tr);
    check_prompt_cap("B0-2(过期探针)", tr2);
}

void emit_trace() {
    std::cout << "\n=== TRACE ===\n";
    struct Plan { const char* name; std::vector<std::string> inputs; bool fill; };
    std::vector<Plan> plans = {
        {"A", CASE_A_INPUTS, false},
        {"A0", BASE_A0_INPUTS, false},
        {"B", CASE_B_INPUTS, false},
        {"B0", CASE_B0_PREFIX, true},
    };
    for (const auto& p : plans) {
        Engine e(p.name);
        std::vector<StepRec> tr = run_all(e, p.inputs);
        if (p.fill) {
            int guard = 0;
            while (e.eng.state().seq <= 11 && guard < 30) {
                tr.push_back(e.step(CASE_B0_FILLER));
                ++guard;
            }
        }
        for (std::size_t i = 0; i < tr.size(); ++i)
            std::cout << trace_line(p.name, static_cast<int>(i) + 1, tr[i]) << "\n";
    }
}

}  // namespace

int main(int argc, char** argv) {
    const bool trace_only = (argc > 1 && std::string(argv[1]) == "--emit-trace");

    // =====================================================================
    //  ① 旧样例剧本冒烟（保持不退化）
    // =====================================================================
    if (!trace_only) {
        std::cout << "=== ① 旧样例剧本 story_v2.example.json 冒烟 ===" << std::endl;
    }
    const std::string story_text = read_file("../../data/story_v2.example.json");
    Catalog cat = StoryLoader::load(story_text);
    nlohmann::json doc = nlohmann::json::parse(story_text);

    auto fw = StoryLoader::frontier_weights(doc);
    auto iw = StoryLoader::intent_weights(doc);
    auto pb = StoryLoader::prompt_budget(doc);

    GameState st = StoryLoader::new_state("seed", 90, "0", 0xC0FFEEull);
    NarrativeEngine eng(cat, st, fw, iw, pb);

    StubSelector llm;
    eng.set_selector(&llm);

    std::vector<std::string> inputs = {
        "1", "2", "1", "1", "1", "我口袋里好像有东西", "1", "1", "1", "1", "1",
    };

    std::ostringstream trace;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const NodeId before = eng.state().current_node;
        TurnResult r = eng.step(inputs[i]);
        const GameState& s = eng.state();
        std::string redeemed;
        for (const auto& n : r.redeemed) { if (!redeemed.empty()) redeemed += ","; redeemed += n; }
        std::size_t open_pending = 0;
        for (const auto& p : s.pending) if (!p.awake) ++open_pending;

        trace << "step=" << (i + 1)
              << " seq=" << s.seq
              << " from=" << before
              << " to=" << s.current_node
              << " layer=" << r.layer
              << " phase=" << s.phase
              << " aff=" << s.affinity
              << " tags=[" << join_sorted(s.tags) << "]"
              << " items=[" << join_sorted(s.items) << "]"
              << " pending_open=" << open_pending
              << " redeemed=[" << redeemed << "]\n";

        assert(!r.frontier_before.items.empty() && "Frontier 不得为空");
    }

    assert(eng.state().seq == static_cast<Seq>(inputs.size()) && "time_cursor 必须严格 +1/步");
    assert(eng.state().phase_index >= 0);

    if (!trace_only) {
        std::cout << eng.debug_frontier() << std::endl;

        PromptResult pr = eng.build_prompt("正在掩饰着什么");
        std::cout << "\n--- Prompt 组装 ---\n"
                  << "est_tokens=" << pr.est_tokens << " routes_injected=";
        for (const auto& r : pr.routes_injected) std::cout << r << " ";
        std::cout << "\n truncated=";
        for (const auto& t : pr.truncated) std::cout << t << " ";
        std::cout << "\n";
        assert(pr.routes_injected.size() <= 5);

        std::cout << "\n=== TRACE (example story) ===\n" << trace.str();
        std::cout << "LLM 调用次数=" << llm.calls << "\n";

        // =================================================================
        //  ② 新剧本指针用例
        // =================================================================
        case_forward_pointer();
        case_backward_pointer();
        case_backward_pointer_neg();
    }

    // =====================================================================
    //  ③ 归一化轨迹（供与 Python 侧 diff）
    // =====================================================================
    emit_trace();

    if (!trace_only) {
        std::cout << "\n=== 断言汇总 ===\n";
        std::cout << "指针用例断言: " << g_pass << " 通过 / " << g_fail << " 失败\n";
        std::cout << (g_fail == 0 ? "POINTER CASES: ALL PASS\n"
                                  : "POINTER CASES: HAS FAILURE\n");
        std::cout << "ALL ASSERTIONS PASSED\n";
    }
    return g_fail == 0 ? 0 : 1;
}
