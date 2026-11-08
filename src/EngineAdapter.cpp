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

std::vector<std::string> EngineAdapter::splitPages(const std::string& n,
                                                   const std::string& p) {
    std::string full = n;
    if (!full.empty() && !p.empty()) full += "\n\n";
    full += p;
    std::vector<std::string> out;
    const std::string d = "\n\n";
    std::size_t pos = 0, prev = 0;
    while ((pos = full.find(d, prev)) != std::string::npos) {
        auto t = full.substr(prev, pos - prev);
        if (!t.empty()) out.push_back(t);
        prev = pos + d.size();
    }
    if (prev < full.size()) {
        auto t = full.substr(prev);
        if (!t.empty()) out.push_back(t);
    }
    if (out.empty()) out.push_back("...");
    return out;
}

std::vector<std::string> EngineAdapter::getCurrentNodePages() {
    std::lock_guard<std::mutex> g(stateMutex_);
    if (!engine_ || !catalog_) return {"..."};
    const std::string id = engine_->state().current_node;
    const nx::NodeSpec* n = catalog_->find(id);
    if (!n) return {"..."};
    nx::Presentation p = engine_->present(id);
    return splitPages(p.narrator, p.npc_text);
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

    std::string sys = 
        "你是一个心理分析器。请根据当前场景和玩家输入，判断玩家的状态。\n"
        "【当前场景】：" + npcState + "\n"
        "【可选标签池】：\n" + tag_rules + "\n"
        "【规则】：只能从池子中选一个最匹配的标签输出；如果不匹配任何标签，输出 'none'。\n"
        "只输出标签ID，不要输出其他任何字符。";

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

    const std::string sys =
        "你是一个名叫木子因的高中生，对哥哥有极端的依赖和病态的依恋。\n"
        "【当前物理场景】：" + node.npc_state + "\n"
        "【你此刻的心理状态】：\n" + tags_emotion + "\n"
        "【对话对象】：哥哥（玩家）\n"
        "【你的任务】：结合上述场景回应哥哥刚才说的话，并深刻体会上述的心理状态。你嘴里的台词，必须强烈体现出这些情绪，不要平铺直叙！比如情绪是委屈/嫉妒，就表现出吃醋和试探；比如是极度依恋，就表现出偏执和占有欲。\n"
        "【严禁】：不允许输出JSON，同时避免不带有情绪的表达使玩家热情降低，控制回复在30字以内。";
    
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
    std::lock_guard<std::mutex> g(stateMutex_);
    if (!engine_) return;
    engine_->step("");
    persist();
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
    }

    bool isNumeric = !input.empty() && input.find_first_not_of("0123456789") == std::string::npos;

    // 2. 标签判定
    std::string detected_tag = "none";
    std::string cur_state = "";
    if (!isNumeric) {
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

    // =========================================================
    // 3. 动态调度检测（直接用主库 all_tags_ 构造纯净状态去查）
    // =========================================================
    if (!isNumeric) {
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
                std::cout << "【拓扑锁】AI 试图越界前往 " << tr.to_node << "，拦截在原地。\n";
                engine_->mutable_state().current_node = tr.from_node;
                tr.to_node = tr.from_node; 
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
    if (tr.layer == "L0" || isNumeric) {
        std::lock_guard<std::mutex> g(stateMutex_);
        const nx::NodeSpec* n = catalog_->find(tr.to_node);
        if (n) reply = n->npc_text;
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
        }
    }

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

void EngineAdapter::applyPenalty(int p) {
    std::lock_guard<std::mutex> g(stateMutex_);
    if (!engine_) return;
    engine_->mutable_state().affinity += p;
    auto& a = engine_->mutable_state().affinity;
    if (a < 0) a = 0;
    if (a > 100) a = 100;
    persist();
}

void EngineAdapter::persist() {
    if (!db_ || !engine_ || playerName_.empty()) return;
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