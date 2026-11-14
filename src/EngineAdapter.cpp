#include "EngineAdapter.h"
#include "AiSelector.h"
#include "AiEngine.h"
#include "json.hpp"

#include <fstream>
#include <iostream>
#include <algorithm>
#include <chrono>
#include <thread>
#include <sstream>

// ===========================================================================
//  ⚠ 适配层契约（EngineAdapter ↔ nx 内核边界）
//  本文件是"应用/编排层"，只应消费 nx 命名空间的【公开 API】：
//    StoryLoader::{load,lint,frontier_weights,intent_weights,prompt_budget,
//                  new_state,state_from_json,state_to_json}
//    Catalog::{find,phase_index_of,candidate_ids,nodes(只读字段)}
//    NarrativeEngine::{state(),present(),step(),is_over(),set_selector(),
//                      build_prompt(),frontier_in_ids(),blocked_map()} 及构造器
//    NodeSpec / GameState / TriggerCondition / Effects 等公开结构体的【只读】字段
//  禁止直接改写引擎内部状态。下列站点当前越界（逾矩），正在直接写
//  engine_->mutable_state() 的内部字段，破坏内核 seq/visited/pending 不变式：
//    - step() 内 Dynamic_Pull 改 current_node（早期触发动态调度）
//    - step() 内 拓扑锁兜底 改 current_node
//    - applyPenalty 改 affinity（且钳制区间 [0,100] 与内核 [-100,200] 不一致）
//    - persist() 整体覆写 tags
//  根因：NarrativeEngine::mutable_state() 把整个 GameState& 暴露出来；理想做法是
//  收窄该接口，改用受控修改方法（如 reroute(NodeId)/applyAffinityDelta(int)/
//  add_tag/remove_tag），让内核成为标签与状态的唯一权威。
//  说明：本边界注释与下方内联 ⚠ 标记均不触碰 generateReplyForNode 的木子因人设文案。
// ===========================================================================

using json = nlohmann::json;

// 检查 TriggerCondition 是否满足当前状态
static bool checkTriggerCondition(const nx::TriggerCondition& tc, const nx::GameState& state) {
    if (tc.tags_all.empty() && tc.tags_any.empty() && tc.min_step == 0 && tc.require_node.empty() && tc.required_phase.empty()) {
        return false;
    }

    std::cout << "【条件检查】当前 seq=" << state.seq << ", 要求的 min_step=" << tc.min_step << std::endl;

    for (const auto& t : tc.tags_all) {
        if (state.tags.count(t) == 0) {
            std::cout << "【条件检查】❌ 缺少必须标签: " << t << std::endl;
            return false;
        }
    }

    if (!tc.tags_any.empty()) {
        bool found_any = false;
        for (const auto& t : tc.tags_any) {
            if (state.tags.count(t) > 0) { found_any = true; break; }
        }
        if (!found_any) {
            std::cout << "【条件检查】❌ 缺少任意满足的标签: ";
            for (const auto& t : tc.tags_any) std::cout << t << " ";
            std::cout << std::endl;
            return false;
        }
    }

    if (state.seq < tc.min_step) {
        std::cout << "【条件检查】❌ 时间线未到！" << std::endl;
        return false;
    }
    
    if (!tc.require_node.empty() && state.visited.count(tc.require_node) == 0) return false;
    if (!tc.required_phase.empty() && state.phase != tc.required_phase) return false;

    std::cout << "【条件检查】✅ 所有条件满足！" << std::endl;
    return true;
}

EngineAdapter::EngineAdapter(const std::string& storyPath)
    : storyPath_(storyPath) {
    loadStory();
}

EngineAdapter::~EngineAdapter() {
    for (int i = 0; i < 2400 && inflight_.load() > 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
}

bool EngineAdapter::fileReadable(const std::string& p) {
    std::ifstream f(p);
    return f.good();
}

std::string EngineAdapter::stripDotdot(const std::string& p) {
    const std::string pre = "../";
    std::string r = p;
    while (r.compare(0, pre.size(), pre) == 0) r = r.substr(pre.size());
    return r;
}

std::string EngineAdapter::resolveDataPath(const std::string& rel) const {
    std::vector<std::string> cands{rel};
    const std::string flat = stripDotdot(rel);
    if (flat != rel) cands.push_back(flat);
    cands.push_back("../" + flat);
    cands.push_back("./" + flat);
    for (auto& x : cands) if (fileReadable(x)) return x;
    return rel;
}

bool EngineAdapter::loadStory() {
    const std::string path = resolveDataPath(storyPath_);
    std::ifstream f(path);
    if (!f.is_open()) {
        loadError_ = "剧本打开失败: " + path;
        return false;
    }
    std::stringstream ss; ss << f.rdbuf();
    const std::string text = ss.str();
    f.close();

    try {
        catalog_ = std::make_unique<nx::Catalog>(nx::StoryLoader::load(text));
    } catch (const std::exception& e) {
        loadError_ = std::string("剧本加载失败: ") + e.what();
        return false;
    }

    auto errs = nx::StoryLoader::lint(*catalog_, "n1_entry");
    if (!errs.empty()) {
        loadError_ = "剧本 lint 失败:\n";
        for (auto& e : errs) loadError_ += "  - " + e + "\n";
        return false;
    }

    json doc;
    try { doc = json::parse(text); }
    catch (...) { loadError_ = "剧本 JSON 解析失败"; return false; }
    doc_ = doc;

    const std::string entry = doc.value("entry", "n1_entry");
    nx::FrontierWeights fw = nx::StoryLoader::frontier_weights(doc);
    nx::IntentWeights   iw = nx::StoryLoader::intent_weights(doc);
    nx::PromptBudget    pb = nx::StoryLoader::prompt_budget(doc);

    nx::GameState st = nx::StoryLoader::new_state("local", 90, entry);
    engine_ = std::make_unique<nx::NarrativeEngine>(*catalog_, st, fw, iw, pb);

    selector_ = std::make_unique<AiSelector>();
    engine_->set_selector(selector_.get());

    const std::string dbPath = resolveDataPath("../data/save.db");
    std::cout << "【EngineAdapter】剧本: " << path << "\n";
    std::cout << "【EngineAdapter】数据库: " << dbPath << "\n";
    db_ = std::make_unique<DatabaseManager>(dbPath);

    ready_ = true;
    return true;
}

bool EngineAdapter::ready() const { return ready_; }
std::string EngineAdapter::lastError() const { return loadError_; }

std::string EngineAdapter::getCurrentNode() const {
    std::lock_guard<std::mutex> g(stateMutex_);
    return engine_ ? engine_->state().current_node : "";
}

std::string EngineAdapter::getCurrentNodeSpeaker() const {
    std::lock_guard<std::mutex> g(stateMutex_);
    if (!engine_ || !catalog_) return "木子因";
    const nx::NodeSpec* n = catalog_->find(engine_->state().current_node);
    return n ? n->speaker : "木子因";
}

// 背景键：优先读剧本节点里的 background 字段；缺失时退化到 default（由 UI 回退到 bg.jpg）
std::string EngineAdapter::getCurrentBackgroundKey() const {
    std::lock_guard<std::mutex> g(stateMutex_);
    if (!engine_ || doc_.is_null()) return "default";
    const std::string id = engine_->state().current_node;
    try {
        const auto& node = doc_.at("nodes").at(id);
        if (node.contains("background") && node["background"].is_string())
            return node["background"].get<std::string>();
    } catch (...) {}
    return "default";
}

bool EngineAdapter::isGameOver() {
    std::lock_guard<std::mutex> g(stateMutex_);
    return engine_ ? engine_->is_over() : true;
}

void EngineAdapter::rebuildEngine(nx::GameState st) {
    if (!catalog_) return;
    engine_ = std::make_unique<nx::NarrativeEngine>(*catalog_, st);
    engine_->set_selector(selector_.get());
}

void EngineAdapter::createNewPlayer(const std::string& name) {
    std::lock_guard<std::mutex> g(stateMutex_);
    playerName_ = name;
    if (!catalog_) return;
    nx::GameState st = nx::StoryLoader::new_state(name, 90, "n1_entry");
    rebuildEngine(st);
    history_.clear();
    lastAiReply_.clear();
    tag_contexts_.clear();
    all_tags_.clear(); // 👈 清空主库账本
    persist();
}

bool EngineAdapter::loadExistingPlayer(const std::string& name) {
    std::lock_guard<std::mutex> g(stateMutex_);
    if (!db_ || !catalog_) return false;

    std::string sj = db_->loadStateJson(name);
    nx::GameState st;
    if (!sj.empty()) {
        try { st = nx::StoryLoader::state_from_json(sj); }
        catch (...) { return false; }
    } else {
        int aff = 0; std::string node;
        if (!db_->loadData(name, aff, node)) return false;
        st = nx::StoryLoader::new_state(name, aff, node);
    }
    playerName_ = name;
    rebuildEngine(st);
    history_.clear();
    lastAiReply_.clear();
    tag_contexts_.clear();
    all_tags_.clear();
    // 👈 同步底层的标签到主库
    for (const auto& t : engine_->state().tags) {
        all_tags_.insert(t);
    }
    return true;
}

std::vector<std::string> EngineAdapter::splitByBlankLine(const std::string& s) {
    std::vector<std::string> out;
    if (s.empty()) return out;
    const std::string d = "\n\n";
    std::size_t pos = 0, prev = 0;
    while ((pos = s.find(d, prev)) != std::string::npos) {
        auto t = s.substr(prev, pos - prev);
        if (!t.empty()) out.push_back(t);
        prev = pos + d.size();
    }
    if (prev < s.size()) {
        auto t = s.substr(prev);
        if (!t.empty()) out.push_back(t);
    }
    return out;
}

// 把旁白与 NPC 台词分别切成带角色标记的分页；旁白→旁白、npc_text→说话人（由节点声明）
std::vector<StoryPage> EngineAdapter::buildPages(const std::string& narrator,
                                                 const std::string& npcText,
                                                 const std::string& npcSpeaker) {
    std::vector<StoryPage> out;
    for (const auto& seg : splitByBlankLine(narrator))
        out.push_back({seg, PageRole::Narrator, "旁白"});
    for (const auto& seg : splitByBlankLine(npcText))
        out.push_back({seg, PageRole::Npc, npcSpeaker});
    if (out.empty()) out.push_back({"……", PageRole::Narrator, "旁白"});
    return out;
}

std::vector<StoryPage> EngineAdapter::getCurrentNodePages() {
    std::lock_guard<std::mutex> g(stateMutex_);
    if (!engine_ || !catalog_) return {{"……", PageRole::Narrator, "旁白"}};
    const std::string id = engine_->state().current_node;
    const nx::NodeSpec* n = catalog_->find(id);
    if (!n) return {{"……", PageRole::Narrator, "旁白"}};
    nx::Presentation p = engine_->present(id);
    return buildPages(p.narrator, p.npc_text, n->speaker);
}

std::vector<std::string> EngineAdapter::getCurrentOptions() {
    std::lock_guard<std::mutex> g(stateMutex_);
    std::vector<std::string> out;
    if (!engine_ || !catalog_) return out;
    const nx::NodeSpec* n = catalog_->find(engine_->state().current_node);
    if (!n) return out;
    for (const auto& o : n->options) out.push_back(o.text);
    return out;
}

bool EngineAdapter::isFreeInputAvailable() const {
    std::lock_guard<std::mutex> g(stateMutex_);
    if (!engine_ || !catalog_) return false;
    const nx::NodeSpec* n = catalog_->find(engine_->state().current_node);
    if (!n) return false;
    return !n->intent.description.empty() || n->intent.must_freeform;
}

void EngineAdapter::pushHistory(const std::string& u, const std::string& r) {
    history_.emplace_back(u, r);
    while (history_.size() > 3) history_.pop_front();
}

// ---- 标签基础接口实现 ----
void EngineAdapter::addTag(const std::string& tag) {
    if (tag.empty() || tag == "none") return;
    std::lock_guard<std::mutex> g(stateMutex_);
    all_tags_.insert(tag);
}

void EngineAdapter::removeTag(const std::string& tag) {
    std::lock_guard<std::mutex> g(stateMutex_);
    all_tags_.erase(tag);
}

bool EngineAdapter::hasTag(const std::string& tag) const {
    std::lock_guard<std::mutex> g(stateMutex_);
    return all_tags_.count(tag) > 0;
}

std::vector<std::string> EngineAdapter::getAllTags() const {
    std::lock_guard<std::mutex> g(stateMutex_);
    std::vector<std::string> res;
    for (const auto& t : all_tags_) res.push_back(t);
    return res;
}

std::string EngineAdapter::detectTag(const std::string& userInput, const std::string& npcState) {
    std::string tag_rules = "";
    std::ifstream f("../data/tags.json");
    if (f.is_open()) {
        try {
            nlohmann::json j; f >> j;
            for (auto& [id, info] : j["tags"].items()) {
                tag_rules += "- 标签ID: [" + id + "] | 判定线索: " + info["desc"].get<std::string>() + "\n";
            }
        } catch (...) {}
        f.close();
    }

    // 廉价前置：纯中性附和 / 应答（无情绪倾向）直接判 none，避免模型强行贴标签
    {
        std::string t = userInput;
        std::size_t a = t.find_first_not_of(" \t\r\n"), b = t.find_last_not_of(" \t\r\n");
        if (a != std::string::npos) t = t.substr(a, b - a + 1);
        static const char* kNeutral[] = {"好的","好","嗯","嗯嗯","哦","噢","知道了","知道",
                                         "行","可以","好吧","没问题","OK","ok","收到",
                                         "行吧","同意","好哒","好嘞","嗯好"};
        for (const char* w : kNeutral) if (t == w) {
            std::cout << "【标签判定】中性应答，直接判 none。" << std::endl;
            return "none";
        }
    }

    std::string sys = 
        "你是一个心理分析器。请根据当前场景和玩家输入，判断玩家的状态。\n"
        "【当前场景】：" + npcState + "\n"
        "【可选标签池】：\n" + tag_rules + "\n"
        "【规则】：\n"
        "1. 只能从池子中选一个最匹配的标签ID输出；若玩家输入不匹配任何标签，必须输出 'none'。\n"
        "2. 重要：若玩家输入是中性 / 敷衍 / 无感情色彩的回应——例如「好的」「嗯」「哦」\n"
        "「知道了」「行」「好吧」「没问题」「OK」等单纯附和、应答，或只陈述事实 / 动作而无情绪倾向——\n"
        "一律输出 'none'，绝不要强行匹配。\n"
        "3. 只输出标签ID或 'none'，不要输出其他字符、不要带括号或引号。\n"
        "【示例】：输入「好的」→ none；输入「嗯，知道了」→ none；\n"
        "输入「我只爱你，别瞎想」→ trust 或 deeply_touched；输入「你烦不烦」→ grievance";

    std::cout << "【标签判定】请求分析情绪..." << std::endl;
    std::string result = AiEngine::callLlm(sys, userInput, 0.1);
    std::cout << "【标签判定】返回: " << result << std::endl;
    
    result.erase(std::remove_if(result.begin(), result.end(), [](char c) {
        return c == '[' || c == ']' || std::isspace(static_cast<unsigned char>(c));
    }), result.end());
    
    if (!result.empty() && result != "none" && tag_rules.find("- 标签ID: [" + result + "]") != std::string::npos) {
        return result;
    }
    return "none";
}

std::string EngineAdapter::generateReplyForNode(const nx::NodeSpec& node,
                                                const std::string& userInput,
                                                const std::string& extra_tag) {
    std::string tags_emotion = "平静。";
    
    {
        std::lock_guard<std::mutex> g(stateMutex_);
        if (!extra_tag.empty() && extra_tag != "none") {
            all_tags_.insert(extra_tag); // 强塞最新标签
        }

        if (!all_tags_.empty()) {
            tags_emotion = "你此刻心里带着这些情绪：\n";
            std::ifstream f("../data/tags.json");
            nlohmann::json j;
            if (f.is_open()) {
                try { f >> j; } catch (...) {}
                f.close();
            }
            for (const auto& t : all_tags_) {
                if (j.contains("tags") && j["tags"].contains(t) && j["tags"][t].contains("emotion")) {
                    tags_emotion += "- " + j["tags"][t]["emotion"].get<std::string>() + "\n";
                } else {
                    tags_emotion += "- " + t + "\n"; 
                }
            }
        }
    }

    std::cout << "================= 发给 AI 的 Prompt =================" << std::endl;
    std::cout << "心理状态：" << tags_emotion << std::endl;

    std::string sys;
    if (node.speaker == "客户") {
        // 客户专属人设（名字暂不显示）：优先从 data/prompts.json 的 client.persona 读取，
        // 便于用户自行微调；读不到时退回内联文案，保证回复不中断。
        std::string persona =
            "你扮演一个创业多年的青年女精英，你和玩家的公司是合作关系，出于对这些刚毕业大学生的同情，你会适量的照顾玩家。你目前对玩家的好感与情绪（由双方互动中产生的标签决定）为：{EMOTIONS}。因为你们刚刚认识，你对这段合作关系的容忍阈值很低——只要出现一两条明显负面情绪，你便会冷静地切断所有合作关系与往来、结束一切对话，不会勉强维系。";
        {
            std::ifstream pf("../data/prompts.json");
            if (pf.is_open()) {
                try {
                    nlohmann::json pj; pf >> pj;
                    if (pj.contains("client") && pj["client"].contains("persona"))
                        persona = pj["client"]["persona"].get<std::string>();
                } catch (...) {}
                pf.close();
            }
        }
        // 把占位情绪替换为当前真实情绪上下文
        const std::string ph = "{EMOTIONS}";
        std::size_t pos = persona.find(ph);
        if (pos != std::string::npos) persona.replace(pos, ph.size(), tags_emotion);
        else persona += "\n【你此刻对玩家的情绪】：\n" + tags_emotion;

        sys = persona
            + "\n【当前物理场景】：" + node.npc_state + "\n"
            + "【对话对象】：玩家（刚毕业的职场新人）\n"
            + "【你的任务】：结合上述场景与情绪，用一句贴合你身份的话回应玩家，控制在 30 字以内。若对玩家的负面情绪累积过多，你会冷静地切断合作、结束对话——但当前先按中性专业态度回应。\n"
            + "【严禁】：只输出你说出口的话，不输出 JSON，不解释，不加引号。";
    } else {
        // 默认：木子因（高中生妹妹）人设 —— 原有文案与红线逐字保留，不改动
        sys =
            "你是一个名叫木子因的高中生，对哥哥有极端的依赖和病态的依恋。\n"
            "【当前物理场景】：" + node.npc_state + "\n"
            "【你此刻的心理状态】：\n" + tags_emotion + "\n"
            "【对话对象】：哥哥（玩家）\n"
            "【你的任务】：结合上述场景回应哥哥刚才说的话，并深刻体会上述的心理状态。你嘴里的台词，必须强烈体现出这些情绪，不要平铺直叙！比如情绪是委屈/嫉妒，就表现出吃醋和试探；比如是极度依恋，就表现出偏执和占有欲。\n"
            "【严禁】：只能输出说出的话,不允许输出JSON，同时避免不带有情绪的表达使玩家热情降低，浓缩关键信息控制回复在20字以内。";
    }
    
    try {
        std::cout << "【台词生成】正在请求 AI..." << std::endl;
        std::string raw = AiEngine::callLlm(sys, userInput, 0.7);
        if (!raw.empty()) {
            std::cout << "【台词生成】AI 返回: " << raw << std::endl;
            return raw;
        }
        std::cout << "【台词生成】AI 返回为空，降级为原剧本台词。" << std::endl;
    } catch (...) {
        std::cout << "【台词生成】异常，降级为原剧本台词。" << std::endl;
    }
    return node.npc_text.empty() ? "……" : node.npc_text;
}

void EngineAdapter::advance() {
    // options:[] 节点在 Qt 里走这里。旧实现只有 engine_->step("") + persist()，
    // 会整条绕过标签判定 / effects 同步 / 动态调度，导致剧本 effects 永久丢失。
    // 改为复用 step("") 的后处理链。注意此处不能再持有 stateMutex_：
    // std::mutex 非递归，step() 内的 addTag()/removeTag() 会二次加锁导致死锁。
    step("");
}

StepResult EngineAdapter::step(const std::string& input) {
    struct G {
        std::atomic<int>& c;
        explicit G(std::atomic<int>& x) : c(x) { c.fetch_add(1); }
        ~G() { c.fetch_sub(1); }
    } guard(inflight_);

    StepResult sr;
    nx::TurnResult tr;
    
    // 1. 步进
    {
        std::lock_guard<std::mutex> g(stateMutex_);
        if (!engine_ || !catalog_) return sr;
        sr.fromNode = engine_->state().current_node;
        tr = engine_->step(input);
        lastPlayerInput_ = input;
    }

    bool isNumeric = !input.empty() && input.find_first_not_of("0123456789") == std::string::npos;
    // advance() 复用 step("")：空输入代表「内部推进」，跳过 LLM 标签判定与台词生成，
    // 但保留 effects 同步、动态调度与 persist。
    const bool internalAdvance = input.empty();

    // 2. 标签判定
    std::string detected_tag = "none";
    std::string cur_state = "";
    if (!isNumeric && !internalAdvance) {
        {
            std::lock_guard<std::mutex> g(stateMutex_);
            const nx::NodeSpec* cur = catalog_->find(tr.from_node);
            if (cur) cur_state = cur->npc_state;
        }
        if (!cur_state.empty()) {
            detected_tag = detectTag(input, cur_state);
            if (detected_tag != "none") {
                addTag(detected_tag); // 👈 走接口入库
                std::lock_guard<std::mutex> g(stateMutex_);
                tag_contexts_[detected_tag] = "当时场景[" + cur_state + "]，玩家说：" + input;
                std::cout << "【标签系统】获得标签: " << detected_tag << " | 上下文已记录\n";
            }
        }
    }

    // 2b. 先把「本步引擎原始目标节点」的 effects 同步进 all_tags_。
    // NarrativeEngine::step() 已把 effects 写进 st_.tags，但第 3 步的 check_state
    // 用 all_tags_ 覆盖，不同步就会晚一步才看见标签：n3_shut 写入 heartbroken
    // 的那一步本该触发动态节点，却因标签滞后而漏掉。
    // 第 6 步保留：它对【最终】to_node 再应用一次 effects，正好在触发当步
    // 由 n_hidden_smother 的 remove_tags 清掉两个绝望标签，防止重复拉取。
    if (catalog_ && tr.to_node != "-1") {
        const nx::NodeSpec* sync_node = catalog_->find(tr.to_node);
        if (sync_node) {
            for (const auto& t : sync_node->effects.add_tags) addTag(t);
            for (const auto& t : sync_node->effects.remove_tags) removeTag(t);
        }
    }

    // =========================================================
    // 3. 动态调度检测
    // 旧实现锁在 !isNumeric 里，玩家点数字选项推进时永不执行；
    // 现改为无条件执行，凑齐两个绝望标签即自动接入隐藏节点。
    // =========================================================
    {
        std::lock_guard<std::mutex> g(stateMutex_);
        nx::GameState check_state = engine_->state();
        check_state.tags.clear();
        for (const auto& t : all_tags_) {
            check_state.tags.insert(t);
        }

        for (const auto& [id, node] : catalog_->nodes) {
            if (id == tr.to_node || node.terminal) continue; 
            if (checkTriggerCondition(node.early_trigger, check_state)) {
                std::cout << "【动态调度】苛刻条件满足！提前接入隐藏节点: " << id << "\n";
                tr.to_node = id;
                tr.layer = "Dynamic_Pull"; // 👈 盖上合法通行证
                // ⚠ 逾矩：直接写 mutable_state().current_node 把节点"瞬移"进 current_node，
                //    绕过了 step() 的 seq/visited/pending/redeem 记账；内核 NodeSpec::early_trigger
                //    已被 StoryLoader 解析却从未被 step() 使用，动态调度本应内聚进内核。
                //    应改为内核方法 reroute(NodeId) 或在 step() 内消费 early_trigger。
                engine_->mutable_state().current_node = id; // 强改底层节点
                break; 
            }
        }
    }

    // 4. 拓扑锁拦截（严格安保）
    if (!isNumeric && (tr.layer == "L1" || tr.layer == "L2" || tr.layer == "fallback")) {
        std::lock_guard<std::mutex> g(stateMutex_);
        const nx::NodeSpec* cur = catalog_->find(tr.from_node);
        if (cur && tr.to_node != tr.from_node) {
            bool is_legal = false;
            if (tr.to_node == cur->default_next || tr.to_node == cur->fallback_next) {
                is_legal = true;
            } else {
                for (const auto& o : cur->options) {
                    if (o.next == tr.to_node) { is_legal = true; break; }
                }
            }

            // 👇 只有持 Dynamic_Pull 通行证的，才允许放行！
            if (!is_legal) {
                const nx::NodeSpec* target_node = catalog_->find(tr.to_node);
                if (target_node && tr.layer == "Dynamic_Pull") {
                    std::cout << "【拓扑锁】检测到合法动态拉取，放行前往: " << tr.to_node << "\n";
                    is_legal = true; 
                }
            }

            if (!is_legal) {
                // 兜底：非法跳转不直接卡死原地，改走当前节点的默认出口，保证剧情能推进；
                // 真正的修复是下一轮把候选集收敛到当前节点出边（消除全局召回）。
                const nx::NodeSpec* from = catalog_->find(tr.from_node);
                nx::NodeId fb = (from && !from->default_next.empty() && from->default_next != tr.from_node)
                                ? from->default_next : tr.from_node;
                std::cout << "【拓扑锁】AI 试图越界前往 " << tr.to_node
                          << "，非法，改走默认出口 " << fb << "。\n";
                // ⚠ 逾矩：非法跳转时直接改 mutable_state().current_node 兜底，导致 is_over()/
                //    present() 读取的 current_node 与 step() 实际记录的节点不一致。
                //    应改为内核合法跳转判定（clamp_to_legal）后在 step() 内修正，而非事后改写。
                engine_->mutable_state().current_node = fb;
                tr.to_node = fb;
                tr.layer = "L2_local";
            }
        }
    }

    std::cout << "【诊断】input=[" << input << "] layer=" << tr.layer 
              << " from=" << tr.from_node << " to=" << tr.to_node << "\n";

    sr.layer = tr.layer;
    sr.toNode = tr.to_node;
    sr.moved = (tr.to_node != tr.from_node);

    // 5. 台词生成
    std::string reply;
    std::string reply_speaker = "木子因";
    if (internalAdvance) {
        // advance() 的 StepResult 不被 UI 使用：跳过台词生成，避免空输入触发 LLM。
    } else if (tr.layer == "L0" || isNumeric) {
        std::lock_guard<std::mutex> g(stateMutex_);
        const nx::NodeSpec* n = catalog_->find(tr.to_node);
        if (n) { reply = n->npc_text; reply_speaker = n->speaker; }
    } else {
        nx::NodeSpec node_copy;
        bool has_node = false;
        {
            std::lock_guard<std::mutex> g(stateMutex_);
            const nx::NodeSpec* n = catalog_->find(tr.to_node);
            if (n) { node_copy = *n; has_node = true; }
        }

        if (has_node) {
            reply = generateReplyForNode(node_copy, input, detected_tag);
            reply_speaker = node_copy.speaker;
        }
    }
    sr.speaker = reply_speaker;

    if (reply.empty()) reply = tr.presentation.npc_text;
    if (reply.empty()) reply = "……";
    sr.reply = reply;

    // 6. 应用节点 Effects（剧本里的 add_tags / remove_tags）
    if (catalog_ && tr.to_node != "-1") {
        const nx::NodeSpec* target_node = catalog_->find(tr.to_node);
        if (target_node) {
            for (const auto& t : target_node->effects.add_tags) addTag(t);
            for (const auto& t : target_node->effects.remove_tags) removeTag(t);
        }
    }

    // 7. 收尾
    {
        std::lock_guard<std::mutex> g(stateMutex_);
        pushHistory(input, reply);
        lastAiReply_ = reply;
        persist();
    }
    return sr;
}

std::string EngineAdapter::processInput(const std::string& input) {
    return step(input).reply;
}

std::string EngineAdapter::getLastAiReply() {
    std::lock_guard<std::mutex> g(stateMutex_);
    return lastAiReply_;
}

std::string EngineAdapter::getLastPlayerInput() const {
    std::lock_guard<std::mutex> g(stateMutex_);
    return lastPlayerInput_;
}

void EngineAdapter::applyPenalty(int p) {
    std::lock_guard<std::mutex> g(stateMutex_);
    if (!engine_) return;
    // ⚠ 逾矩：直接改 mutable_state().affinity，且钳制区间 [0,100] 与内核 step() 的
    //    [-100,200] 不一致；改动不进内核事件日志/历史。应改为内核方法
    //    applyAffinityDelta(int)，用内核规范钳制区间并记事件。
    engine_->mutable_state().affinity += p;
    auto& a = engine_->mutable_state().affinity;
    if (a < 0) a = 0;
    if (a > 100) a = 100;
    persist();
}

void EngineAdapter::persist() {
    if (!db_ || !engine_ || playerName_.empty()) return;
    // all_tags_ 是唯一真相源：detectTag()/addTag() 的 LLM 情绪标签只落在这里，
    // 从不写 GameState.tags。必须先 clear 再全量 insert，否则 removeTag() 的
    // 删除会残留在 state_json 里，读档后被 loadExistingPlayer() 重新灌回来。
    // ⚠ 逾矩：persist() 把并行维护的 all_tags_ 整体覆写回 mutable_state().tags，
    //    与 step() 经 effects 写入 st_.tags 的链路形成冗余易碎的往返。
    //    应让标签以内核 add_tag/remove_tag 为权威，persist 仅 state_to_json(state())。
    nx::GameState& st = engine_->mutable_state();
    st.tags.clear();
    for (const auto& t : all_tags_) st.tags.insert(t);
    const std::string js = nx::StoryLoader::state_to_json(engine_->state());
    db_->saveStateJson(playerName_, js);
}

std::vector<DatabaseManager::SaveInfo> EngineAdapter::getAllSaves() {
    std::lock_guard<std::mutex> g(stateMutex_);
    return db_ ? db_->getAllSaves() : std::vector<DatabaseManager::SaveInfo>{};
}

void EngineAdapter::deleteSaveData(const std::string& n) {
    std::lock_guard<std::mutex> g(stateMutex_);
    if (db_) db_->deleteSave(n);
}

// ===== 结局 / 角色图鉴：跨存档并集，纯只读推导，不写库 =====
namespace {

// 合并每个存档 state_json 的 visited：同一节点取最小首次 seq。
// 坏行（解析失败 / 缺 visited / 字段非整数）跳过；全部存档无 visited 时返回空 map。
std::unordered_map<std::string, std::int64_t> collectGlobalVisited(DatabaseManager* db) {
    std::unordered_map<std::string, std::int64_t> out;
    if (!db) return out;
    for (const auto& raw : db->getAllStateJsons()) {
        try {
            json j = json::parse(raw);
            if (!j.contains("visited") || !j["visited"].is_object()) continue;
            for (auto it = j["visited"].begin(); it != j["visited"].end(); ++it) {
                std::int64_t seq = 0;
                try { seq = it.value().get<std::int64_t>(); }
                catch (...) { continue; }
                auto res = out.emplace(it.key(), seq);
                if (!res.second && seq < res.first->second) res.first->second = seq;
            }
        } catch (...) { continue; }  // 坏 state_json 跳过，不影响其它存档
    }
    return out;
}

}  // namespace

std::vector<EndingEntry> EngineAdapter::getEndingGallery() const {
    std::lock_guard<std::mutex> g(stateMutex_);
    std::vector<EndingEntry> out;
    if (!db_ || doc_.is_null()) return out;
    const auto visited = collectGlobalVisited(db_.get());
    try {
        // doc_ object 底层是 std::map，按节点 id 字典序返回（与剧本插入顺序不同，可接受）。
        const auto& nodes = doc_.at("nodes");
        for (auto it = nodes.begin(); it != nodes.end(); ++it) {
            const std::string id = it.key();
            if (id == "-1") continue;    // 唯一 terminal 节点，从不进 visited
            const auto& node = it.value();
            auto kind = node.find("kind");
            if (kind == node.end() || !kind->is_string() || kind->get<std::string>() != "ending") continue;
            EndingEntry e;
            e.id = id;
            auto pres = node.find("presentation");
            if (pres != node.end() && pres->is_object()) {
                auto h = pres->find("hint");
                if (h != pres->end() && h->is_string()) e.hint = h->get<std::string>();
                auto nr = pres->find("narrator");
                if (nr != pres->end() && nr->is_string()) e.narrator = nr->get<std::string>();
            }
            auto bg = node.find("background");
            e.background = (bg != node.end() && bg->is_string()) ? bg->get<std::string>() : std::string("default");
            auto v = visited.find(id);
            e.firstSeq = (v != visited.end()) ? v->second : 0;   // >0 = 已收录
            out.push_back(e);
        }
    } catch (...) { return {}; }
    return out;
}

std::vector<CharacterEntry> EngineAdapter::getCharacterGallery() const {
    std::lock_guard<std::mutex> g(stateMutex_);
    std::vector<CharacterEntry> out = {
        {"木子因", "病弱内向的妹妹，在家时散着长发", false},
        {"客户",   "创业多年的青年女精英，与玩家公司合作", false},
        {"老板",   "一通电话把明天分成两条路的人", false},
        {"广播",   "出差列车上的播报声", false},
    };
    if (!db_ || doc_.is_null()) return out;
    // 动态建立 speaker -> 节点 id 集合：无 presentation.speaker 时回退 "木子因"
    // （与 StoryLoader.hpp:262 默认值一致，否则 17 个无该字段的节点会被漏掉）。
    std::unordered_map<std::string, std::unordered_set<std::string>> speakerNodes;
    try {
        const auto& nodes = doc_.at("nodes");
        for (auto it = nodes.begin(); it != nodes.end(); ++it) {
            std::string speaker = "木子因";
            const auto& node = it.value();
            auto pres = node.find("presentation");
            if (pres != node.end() && pres->is_object()) {
                auto sp = pres->find("speaker");
                if (sp != pres->end() && sp->is_string()) speaker = sp->get<std::string>();
            }
            speakerNodes[speaker].insert(it.key());
        }
    } catch (...) { return out; }
    const auto visited = collectGlobalVisited(db_.get());
    for (auto& c : out) {
        auto sn = speakerNodes.find(c.name);
        if (sn == speakerNodes.end()) continue;
        for (const auto& nid : sn->second) {
            if (visited.count(nid)) { c.met = true; break; }
        }
    }
    return out;
}
