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

std::string EngineAdapter::generateReplyForNode(const nx::NodeSpec& node,
                                                const std::string& userInput) {
    const std::string sys =
        "你是木子因，一个对哥哥有强烈依恋的高中生。当前场景：" + node.npc_state +
        "\n只输出一句台词，符合你的性格和当前状态。不要输出 JSON，不要解释。";
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
    {
        std::lock_guard<std::mutex> g(stateMutex_);
        if (!engine_ || !catalog_) return sr;
        sr.fromNode = engine_->state().current_node;
        tr = engine_->step(input);
    }

    bool isNumeric = !input.empty() && input.find_first_not_of("0123456789") == std::string::npos;

    // 拓扑锁：保护基础拓扑不被破坏，拦截非相邻越界
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

            if (!is_legal) {
                std::cout << "【拓扑锁】AI 试图越界前往 " << tr.to_node 
                          << "，由于未获得钥匙，被拦截并留在原地。\n";
                
                // 👇 核心修复 1：把引擎内部的节点状态硬拽回来！
                engine_->mutable_state().current_node = tr.from_node;
                
                tr.to_node = tr.from_node; 
                tr.layer = "L2_local"; 
            }
        }
    }

    std::cout << "【诊断】input=[" << input << "] layer=" << tr.layer 
              << " from=" << tr.from_node 
              << " to=" << tr.to_node << "\n";

    sr.layer = tr.layer;
    sr.toNode = tr.to_node;
    sr.moved = (tr.to_node != tr.from_node);
    sr.isNormal = false;

    std::string reply;
    {
        std::lock_guard<std::mutex> g(stateMutex_);
        const nx::NodeSpec* n = catalog_->find(tr.to_node);
        if (n) {
            if (tr.layer == "L0" || isNumeric) {
                reply = n->npc_text;
            } else {
                reply = generateReplyForNode(*n, input);
            }
        }
    }
    if (reply.empty()) reply = tr.presentation.npc_text;
    if (reply.empty()) reply = "……";
    sr.reply = reply;

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