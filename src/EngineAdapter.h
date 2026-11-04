#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <memory>
#include <deque>
#include <functional>

#include "nx.hpp"
#include "DatabaseManager.h"

class AiSelector;

// 单步结果（Qt 版用；终端版只用 processInput 返回的 reply）
struct StepResult {
    std::string reply;
    std::string fromNode;
    std::string toNode;
    std::string layer;       // L0 / L1 / L2 / fallback / normal
    bool moved = false;
    bool isNormal = false;
    std::string newTag;
};

class EngineAdapter {
public:
    explicit EngineAdapter(const std::string& storyPath);
    ~EngineAdapter();

    // ---- 与 GameEngine 同名同签名 ----
    bool ready() const;
    std::string lastError() const;

    void createNewPlayer(const std::string& name);
    bool loadExistingPlayer(const std::string& name);

    std::vector<std::string> getCurrentNodePages();
    std::vector<std::string> getCurrentOptions();
    bool isGameOver();
    std::string getCurrentNode() const;

    std::string processInput(const std::string& input);   // 终端版
    std::string getLastAiReply();

    void applyPenalty(int penalty);

    std::vector<DatabaseManager::SaveInfo> getAllSaves();
    void deleteSaveData(const std::string& name);

    // ---- 新增（Qt 版用）----
    bool isFreeInputAvailable() const;
    void advance();
    StepResult step(const std::string& input);
    void persist();

private:
    std::string storyPath_;
    bool ready_ = false;
    std::string loadError_;

    std::unique_ptr<nx::Catalog> catalog_;
    std::unique_ptr<nx::NarrativeEngine> engine_;
    std::unique_ptr<DatabaseManager> db_;
    std::unique_ptr<AiSelector> selector_;

    std::string playerName_;
    std::string lastAiReply_;
    std::deque<std::pair<std::string, std::string>> history_;

    mutable std::mutex stateMutex_;
    std::atomic<int> inflight_{0};

    bool loadStory();
    std::string resolveDataPath(const std::string& rel) const;
    static std::string stripDotdot(const std::string& p);
    static bool fileReadable(const std::string& p);

    std::string generateReplyForNode(const nx::NodeSpec& node,
                                     const std::string& userInput);
    static std::vector<std::string> splitPages(const std::string& narrator,
                                               const std::string& npcText);
    void pushHistory(const std::string& user, const std::string& reply);

    // 重建 engine_（createNewPlayer / loadExistingPlayer 用）
    void rebuildEngine(nx::GameState st);
};