#include <iostream>
#include <string>
#include <map>
#include <vector>
#include <regex>
#include "TrieFilter.h"

// 玩家结构体
struct Player {
    std::string name;
    int affinity = 0; 
};

// 剧情节点配置
struct StoryNode {
    std::string npcDialogue;                  
    std::string hint;                         
    std::map<std::string, int> regexRules;    
    int nextDefaultNode;                      
    std::map<std::string, int> affinityMap;   
};

int main() {
    Player player;
    std::cout << "【系统】请输入你的名字: ";
    std::cin >> player.name;
    std::cout << "欢迎，" << player.name << "！(输入 '退出' 结束游戏)\n\n";

    // 初始化字典树，加入 5 个敏感词
    TrieFilter badWords;
    badWords.insert("滚");
    badWords.insert("烦");
    badWords.insert("讨厌");
    badWords.insert("傻逼");
    badWords.insert("去死");

    // 核心配置表
    std::map<int, StoryNode> gameData = {
        // 节点 0：开局
        {0, {
            "小雅：你好呀，" + player.name + "。今天天气真好呢。",
            "【提示】你可以夸她，或者聊聊天气。",
            {
                {"(漂亮|好看|美丽|可爱|迷人)", 1}, 
                {"(热|天气|太阳|下雨)", 2}                 
            },
            0, 
            {{"(漂亮|好看|美丽|可爱|迷人)", 10}, {"(热|天气|太阳|下雨)", 5}}
        }},
        // 节点 1：夸赞后
        {1, {
            "小雅：哎呀，你嘴真甜... (脸红了) 对了，你平时喜欢做什么呀？",
            "【提示】你可以说 '写代码'、'打游戏' 或 '退出'。",
            {
                {"(代码|编程|写代码)", 3}, 
                {"(游戏|打游戏|玩游戏)", 3},
                {"(退出|结束|再见)", -1}
            },
            1, 
            {{"(代码|编程|写代码)", 10}, {"(游戏|打游戏|玩游戏)", 10}}
        }},
        // 节点 2：聊天气后
        {2, {
            "小雅：是啊，天气有点热，要不要一起去吃冰淇淋？",
            "【提示】你可以回答 '好啊' 或 '不去'。",
            {
                {"(好|去|可以|行)", 3}, 
                {"(不去|不想|不要)", 0},
                {"(退出|结束)", -1}
            },
            0, 
            {{"(好|去|可以|行)", 5}, {"(不去|不想|不要)", -5}}
        }},
        // 节点 3：结局点
        {3, {
            "小雅：太好啦，那我们一言为定！",
            "【提示】输入 '退出' 结束游戏。",
            { {"(退出|结束|再见)", -1} },
            3, 
            {}
        }}
    };

    int currentNode = 0; 
    std::string userInput;

    // 清除输入名字后留在缓冲区的换行符（极其关键）
    std::cin.ignore(); 

    // 游戏主循环
    while (currentNode != -1) {
        std::cout << "--------------------------------------------------\n";
        StoryNode currentStory = gameData[currentNode];
        std::cout << currentStory.npcDialogue << "\n";
        std::cout << currentStory.hint << "\n\n";

        std::cout << "[你] : ";
        // 注意：这里没有 ignore 了！
        std::getline(std::cin, userInput);

        // 1. 全局最高优先级：字典树敏感词拦截
        if (badWords.containsSensitiveWord(userInput)) {
            std::cout << "\n【系统警告】检测到不文明用语！\n";
            std::cout << "[小雅] : 你...你怎么能这么说... (她委屈地跑开了)\n";
            player.affinity -= 50;
            std::cout << "【当前好感度】: " << player.affinity << "\n\n";
            currentNode = -1; 
            continue;
        }

        // 2. 正则匹配语义
        bool matched = false;
        for (const auto& pair : currentStory.regexRules) {
            std::string regexPattern = pair.first;
            int nextNode = pair.second;

            std::regex pattern(regexPattern);
            if (std::regex_search(userInput, pattern)) {
                matched = true;
                std::cout << "\n【系统】识别到意图: " << regexPattern << "\n";
                
                if (currentStory.affinityMap.find(regexPattern) != currentStory.affinityMap.end()) {
                    int change = currentStory.affinityMap[regexPattern];
                    player.affinity += change;
                    std::cout << "【系统】好感度 " << (change > 0 ? "+" : "") << change 
                              << "，当前好感度: " << player.affinity << "\n";
                }

                currentNode = nextNode; 
                break; 
            }
        }

        // 3. 死锁预防：如果没匹配到任何正则
        if (!matched) {
            std::cout << "\n【系统】小雅没听懂你的意思...\n";
            currentNode = currentStory.nextDefaultNode; 
        }
    }

    // 结算
    std::cout << "\n==================================================\n";
    std::cout << "游戏结束！最终好感度: " << player.affinity << "\n";
    if (player.affinity >= 20) {
        std::cout << "结局：小雅对你露出了灿烂的笑容！\n";
    } else if (player.affinity < 0) {
        std::cout << "结局：小雅好像有点怕你了...\n";
    } else {
        std::cout << "结局：你们成为了普通朋友。\n";
    }

    return 0;
}