#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <memory>
#include <deque>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include "nx.hpp"
#include "DatabaseManager.h"
#include "json.hpp"

class AiSelector;

// 单页台词的角色归属：旁白 / 木子因（NPC）/ 我（玩家）
enum class PageRole { Narrator, Npc, Player };

// 一页可显示的剧情文本，携带“谁在说话”的信息
struct StoryPage {
    std::string text;
    PageRole role = PageRole::Narrator;
    std::string speaker; // 解析后的名牌显示名：旁白 / 木子因 / 我
};

// 单步结果（Qt 版用；终端版只用 processInput 返回的 reply）
struct StepResult {
    std::string reply;
    std::string fromNode;
    std::string toNode;
    std::string layer;       // L0 / L1 / L2 / fallback / normal
    std::string speaker = "木子因"; // 被回复节点的说话人：木子因/老板/客户/广播
    bool moved = false;
    bool isNormal = false;
    std::string newTag;
};

struct EndingEntry {
    std::string id;            // ending 节点 id
    std::string hint;          // presentation.hint
    std::string background;    // 节点顶层 background 键
    std::string narrator;      // presentation.narrator
    std::int64_t firstSeq = 0; // >0 = 已收录（跨所有存档取最小首次 seq）；0 = 未收录
};
struct CharacterEntry {
    std::string name;          // "木子因" / "客户" / "老板" / "广播"
    std::string note;          // 一句话描述（中文，贴合剧情）
    bool met = false;          // 跨所有存档是否接触过
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

    // 返回带角色标记的分页：旁白 / 木子因 交替出现（玩家自己的输入由 UI 以“我”呈现）
    std::vector<StoryPage> getCurrentNodePages();
    std::vector<std::string> getCurrentOptions();
    bool isGameOver();
    std::string getCurrentNode() const;
    std::string getCurrentNodeSpeaker() const;   // 当前节点 npc_text 的说话人（木子因/老板/客户/广播）

    std::string processInput(const std::string& input);   // 终端版
    std::string getLastAiReply();
    std::string getLastPlayerInput() const;               // 最近一次玩家输入（UI 以“我”展示）

    void applyPenalty(int penalty);

    std::vector<DatabaseManager::SaveInfo> getAllSaves();
    void deleteSaveData(const std::string& name);

    // ---- 新增（Qt 版用）----
    bool isFreeInputAvailable() const;
    void advance();
    StepResult step(const std::string& input);
    void persist();

    // 当前节点对应的背景键（优先读剧本 background 字段，无则退化到 default）
    std::string getCurrentBackgroundKey() const;

    // 标签管理基础接口
    void addTag(const std::string& tag);
    void removeTag(const std::string& tag);
    bool hasTag(const std::string& tag) const;
    std::vector<std::string> getAllTags() const;

    // 结局 / 角色图鉴：跨存档并集，纯只读推导（不写库、不改 GameState）。
    std::vector<EndingEntry> getEndingGallery() const;
    std::vector<CharacterEntry> getCharacterGallery() const;
private:
    std::string storyPath_;
    bool ready_ = false;
    std::string loadError_;
    nlohmann::json doc_;          // 剧本原始 JSON（用于读取 background 等展示字段，内核不感知）

    std::unique_ptr<nx::Catalog> catalog_;
    std::unique_ptr<nx::NarrativeEngine> engine_;
    std::unique_ptr<DatabaseManager> db_;
    std::unique_ptr<AiSelector> selector_;

    std::string playerName_;
    std::string lastAiReply_;
    std::string lastPlayerInput_;
    std::deque<std::pair<std::string, std::string>> history_;

    mutable std::mutex stateMutex_;
    std::atomic<int> inflight_{0};

    bool loadStory();
    std::string resolveDataPath(const std::string& rel) const;
    static std::string stripDotdot(const std::string& p);
    static bool fileReadable(const std::string& p);

    std::string generateReplyForNode(const nx::NodeSpec& node,
                                     const std::string& userInput,
                                     const std::string& extra_tag = "");
    static std::vector<std::string> splitByBlankLine(const std::string& s);
    std::vector<StoryPage> buildPages(const std::string& narrator,
                                      const std::string& npcText,
                                      const std::string& npcSpeaker);
    void pushHistory(const std::string& user, const std::string& reply);

    // 重建 engine_（createNewPlayer / loadExistingPlayer 用）
    void rebuildEngine(nx::GameState st);
    // 存放标签的上下文，键是标签ID，值是当时发生的情景
    std::unordered_map<std::string, std::string> tag_contexts_;
    std::unordered_set<std::string> all_tags_; // 唯一真相源
    // 大模型判定标签的函数
    std::string detectTag(const std::string& userInput, const std::string& npcState);
};
