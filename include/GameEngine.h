#pragma once
#include <iostream>
#include <string>
#include <regex>
#include <fstream>
#include <vector> 
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>
#include "json.hpp" 
#include "DatabaseManager.h" 
#include "AiEngine.h"

using json = nlohmann::json;

class GameEngine {
private:
    json storyData;            
    std::string currentNode;   
    int affinity = 0;          
    std::string playerName;    
    DatabaseManager* db = nullptr;   // ⚠ 必须初始化，否则构造失败时是野指针
    std::string lastAiReply;
    bool ready_ = false;
    std::string load_error_;

    // D-6（跨线程访问）的止血闸门。
    // UI 线程跑 refreshUI / 保存进度，工作线程跑 processInput（里面有十秒量级的网络等待），
    // 两边读写的是同一份 affinity / currentNode / lastAiReply，
    // 其中 lastAiReply 是 std::string —— 一边赋值一边拷贝是会真的炸的。
    // 这把锁只在**极短的临界区**里持有（绝不包含网络调用），所以 UI 不会被卡住。
    mutable std::mutex stateMutex_;

    // 在飞的 processInput 条数。
    // 用途：析构时必须先把它们等完。工作线程是通过裸引用抓 engine 的，
    // 引擎一旦先析构，工作线程下一行就在访问已经释放的 storyData
    // —— 实测：发送后立刻退出，ASan 报 heap-use-after-free，栈落在
    // GameEngine::processInput 的 storyData["nodes"]（见 docs/CRASH_ROOT_CAUSE.md）。
    std::atomic<int> inflight_{0};

    // 写档失败要吭声：R-11 的真实危害就是"失败了还静默"。
    bool persist(const char* where) {
        if (!db) return false;
        if (db->saveData(playerName, affinity, currentNode)) return true;
        std::cerr << "【存档警告】" << where << " 写档失败，玩家进度没有落盘！" << std::endl;
        return false;
    }

    // 数据文件定位：兼容"从 build/ 跑"和"从项目根跑"两种习惯。
    //
    // 原来所有路径都硬编码成 "../data/story.json"，只有在工作目录恰好是 build/
    // 时才成立。一旦从别处启动，std::ifstream 打开失败，构造函数会**静默 return**，
    // db 变成野指针，随后 createNewPlayer() 解引用 -> Ubuntu 上实测直接 SIGSEGV
    // （ASan 报告：member call on null pointer of type 'DatabaseManager'）。
    static bool file_readable(const std::string& p) {
        std::ifstream f(p);
        return f.good();
    }
    static std::string strip_dotdot(const std::string& p) {
        const std::string prefix = "../";
        std::string r = p;
        while (r.compare(0, prefix.size(), prefix) == 0) r = r.substr(prefix.size());
        return r;
    }
    static std::string resolve_data_path(const std::string& rel) {
        std::vector<std::string> cands;
        cands.push_back(rel);
        const std::string flat = strip_dotdot(rel);
        if (flat != rel) cands.push_back(flat);
        cands.push_back("../" + flat);
        cands.push_back("./" + flat);
        for (const auto& c : cands) if (file_readable(c)) return c;
        return rel;   // 都找不到就返回原样，让上层拿到可诊断的错误
    }

public:
    // 构造是否成功。上层**必须**检查，不要再靠"没崩就是好的"。
    bool ready() const { return ready_; }
    const std::string& lastError() const { return load_error_; }

    GameEngine(const std::string& jsonPath) {
        const std::string storyPath = resolve_data_path(jsonPath);
        std::ifstream file(storyPath);
        if (!file.is_open()) {
            load_error_ = "剧本文件打开失败: " + storyPath;
            return;
        }
        try {
            file >> storyData;
        } catch (const std::exception& e) {
            load_error_ = std::string("剧本 JSON 解析失败: ") + e.what();
            return;
        }
        if (!storyData.contains("nodes")) {
            load_error_ = "剧本缺少 nodes 字段: " + storyPath;
            return;
        }
        currentNode = "0";
        db = new DatabaseManager(resolve_data_path("../data/save.db"));
        ready_ = true;
    }

    ~GameEngine() {
        // 等后台的 processInput 收尾再拆引擎。上限 12 秒（覆盖 curl 的 10 秒超时），
        // 宁可退出慢一点，也不要让工作线程在已析构的对象上继续跑。
        for (int i = 0; i < 2400 && inflight_.load() > 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (inflight_.load() > 0) {
            std::cerr << "【引擎警告】退出时仍有后台任务未收尾，强行析构（可能触发崩溃）。" << std::endl;
        }
        delete db;
        db = nullptr;
    }

    std::string getCurrentNode() const {
        std::lock_guard<std::mutex> guard(stateMutex_);
        return currentNode;
    }

    // 👇 新增：给 UI 层判断当前节点是否支持 AI 隐藏分支
    bool hasIntentNodes() const {
        std::lock_guard<std::mutex> guard(stateMutex_);
        if (currentNode == "-1") return false;
        json node = storyData["nodes"][currentNode];
        return node.contains("intent_nodes") && !node["intent_nodes"].empty();
    }

    void createNewPlayer(const std::string& name) {
        std::lock_guard<std::mutex> guard(stateMutex_);
        playerName = name;
        affinity = 90; 
        currentNode = "0";
        persist("createNewPlayer");
    }

    bool loadExistingPlayer(const std::string& name) {
        std::lock_guard<std::mutex> guard(stateMutex_);
        if (!db) return false;
        int savedAffinity = 0;
        std::string savedNode = "0";
        if (db->loadData(name, savedAffinity, savedNode)) {
            playerName = name;
            affinity = savedAffinity;
            currentNode = savedNode;
            return true;
        }
        return false;
    }

    std::vector<std::string> getCurrentNodePages() {
        std::lock_guard<std::mutex> guard(stateMutex_);
        std::vector<std::string> pages;
        if (currentNode == "-1") {
            pages.push_back("【游戏结束】");
            return pages;
        }
        json node = storyData["nodes"][currentNode];
        
        std::string fullText = "";
        if (node.contains("narrator") && node["narrator"].is_string()) {
            fullText += node["narrator"].get<std::string>() + "\n\n";
        }
        if (node.contains("npc_text") && node["npc_text"].is_string()) {
            fullText += node["npc_text"].get<std::string>() + "\n";
        }
        pages.push_back(fullText);
        return pages;
    }

    std::vector<std::string> getCurrentOptions() {
        std::lock_guard<std::mutex> guard(stateMutex_);
        std::vector<std::string> opts;
        if (currentNode == "-1") return opts;
        json node = storyData["nodes"][currentNode];
        if (node.contains("options") && node["options"].is_array() && !node["options"].empty()) {
            for (const auto& opt : node["options"]) {
                if (opt.is_object() && opt.contains("text") && opt["text"].is_string()) {
                    opts.push_back(opt["text"].get<std::string>());
                } else if (opt.is_string()) {
                    opts.push_back(opt.get<std::string>());
                }
            }
        }
        return opts;
    }

    std::string processInput(const std::string& userInput) {
        // 在飞计数（RAII）：析构函数靠它决定能不能安全拆引擎。
        struct InflightGuard {
            std::atomic<int>& counter;
            explicit InflightGuard(std::atomic<int>& c) : counter(c) { counter.fetch_add(1); }
            ~InflightGuard() { counter.fetch_sub(1); }
        } inflightGuard(inflight_);

        // 快照：只在这一小段临界区里读一次状态，之后全程用本地副本。
        // 网络调用（curl 最长 10 秒）绝不能持锁，否则会把 UI 线程一起锁死。
        json current_node_data;
        std::string nextNode;
        {
            std::lock_guard<std::mutex> guard(stateMutex_);
            if (currentNode == "-1") return "【游戏结束】";
            current_node_data = storyData["nodes"][currentNode];
            nextNode = currentNode;
        }

        std::string reply = "";
        std::string processedInput = userInput; 

        if (current_node_data.contains("default_next") && current_node_data["default_next"].is_string()) {
            nextNode = current_node_data["default_next"].get<std::string>();
        }
        
        bool isOptionSelected = false;

        if (current_node_data.contains("options") && current_node_data["options"].is_array() && !current_node_data["options"].empty()) {
            try {
                int choice = std::stoi(processedInput); 
                if (choice >= 1 && choice <= current_node_data["options"].size()) {
                    json selectedOpt = current_node_data["options"][choice - 1];
                    if (selectedOpt.contains("next") && selectedOpt["next"].is_string()) {
                        nextNode = selectedOpt["next"].get<std::string>();
                    }
                    if (selectedOpt.contains("text") && selectedOpt["text"].is_string()) {
                        processedInput = selectedOpt["text"].get<std::string>();
                    }
                    isOptionSelected = true; 
                }
            } catch (...) {} 
        }

        if (isOptionSelected) {
            reply = "你做出了选择。"; 
        } else {
            std::string npcState = "正常状态";
            if (current_node_data.contains("npc_state") && current_node_data["npc_state"].is_string()) {
                npcState = current_node_data["npc_state"].get<std::string>();
            }

            std::string routesDesc = "无特殊路径";
            if (current_node_data.contains("intent_nodes") && current_node_data["intent_nodes"].is_object()) {
                routesDesc = "";
                for (auto& [node_id, node_data] : current_node_data["intent_nodes"].items()) {
                    
                    if (node_data.contains("required_item")) {
                        std::string reqItem = node_data["required_item"].get<std::string>();
                        bool hasItem = false;
                        std::vector<std::string> items;
                        {
                            std::lock_guard<std::mutex> guard(stateMutex_);
                            if (db) items = db->getItems(playerName);
                        }
                        for (const auto& item : items) {
                            if (item == reqItem) { hasItem = true; break; }
                        }
                        if (!hasItem) {
                            std::cout << "【引擎拦截】缺少道具[" << reqItem << "]，屏蔽分支: " << node_id << std::endl;
                            continue; 
                        }
                    }

                    std::string desc = "无描述";
                    if (node_data.contains("description") && node_data["description"].is_string()) {
                        desc = node_data["description"].get<std::string>();
                    }
                    routesDesc += "【路径节点ID】: " + node_id + "\n【语义描述】: " + desc + "\n\n";
                }
            }

            std::cout << "【系统】木子因正在思考中...\n";
            json aiResult = AiEngine::analyzeInput(processedInput, affinity, npcState, routesDesc);
            
            if (aiResult.contains("reply") && aiResult["reply"].is_string()) {
                reply = aiResult["reply"].get<std::string>();
                {
                    std::lock_guard<std::mutex> guard(stateMutex_);
                    lastAiReply = reply;
                }
            } else {
                reply = "（木子因看着你，似乎不知道该说什么。）";
            }
            
            int affinityChange = 0;
            if (aiResult.contains("affinity_change") && aiResult["affinity_change"].is_number()) {
                affinityChange = aiResult["affinity_change"].get<int>();
            }
            {
                std::lock_guard<std::mutex> guard(stateMutex_);
                affinity += affinityChange;
                if (affinityChange != 0) {
                    std::cout << "【后台数值变动】好感度: " << affinityChange << "，当前: " << affinity << std::endl;
                }
            }
            
            if (aiResult.contains("next_node") && !aiResult["next_node"].is_null() && aiResult["next_node"].is_string()) {
                std::string aiNextNode = aiResult["next_node"].get<std::string>();
                if (!aiNextNode.empty()) {
                    
                    if (current_node_data.contains("intent_nodes") && 
                        current_node_data["intent_nodes"].contains(aiNextNode) &&
                        current_node_data["intent_nodes"][aiNextNode].contains("reward_item")) {
                        
                        std::string reward = current_node_data["intent_nodes"][aiNextNode]["reward_item"].get<std::string>();
                        std::cout << "【系统奖励】玩家获得道具: " << reward << "，已存入数据库！" << std::endl;
                        {
                            std::lock_guard<std::mutex> guard(stateMutex_);
                            if (db) db->addItem(playerName, reward);
                        }
                    }
                    
                    nextNode = aiNextNode;
                    std::cout << "【系统】AI 判定跳转至节点: " << nextNode << "\n";
                } else {
                    nextNode = currentNode;
                }
            } else {
                nextNode = currentNode; 
            }
            
            if (!storyData["nodes"].contains(nextNode) && nextNode != "-1") {
                std::cout << "【系统警告】AI 试图跳转到不存在的节点[" << nextNode << "]，强制返回原节点。\n";
                nextNode = currentNode;
            }
        }

        {
            std::lock_guard<std::mutex> guard(stateMutex_);
            currentNode = nextNode;
            persist("processInput");
        }
        
        return reply;
    }

    bool isGameOver() {
        std::lock_guard<std::mutex> guard(stateMutex_);
        return currentNode == "-1";
    }

    // db 可能为 nullptr（构造失败时），这里返回空集而不是崩溃，让上层能体面降级。
    std::vector<DatabaseManager::SaveInfo> getAllSaves() {
        std::lock_guard<std::mutex> guard(stateMutex_);
        if (!db) return {};
        return db->getAllSaves();
    }

    std::string getLastAiReply() {
        std::lock_guard<std::mutex> guard(stateMutex_);
        return lastAiReply;
    }

    void deleteSaveData(const std::string& name) {
        std::lock_guard<std::mutex> guard(stateMutex_);
        if (db) db->deleteSave(name);
    }

    void applyPenalty(int penalty) {
        std::lock_guard<std::mutex> guard(stateMutex_);
        affinity += penalty;
        if (db && !playerName.empty()) persist("applyPenalty");
    }
};