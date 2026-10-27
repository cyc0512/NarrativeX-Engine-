#pragma once
#include <iostream>
#include <string>
#include <regex>
#include <fstream>
#include "json.hpp" 
#include "DatabaseManager.h" 
// 注意：这里彻底不需要引入 TrieFilter 了

using json = nlohmann::json;

class GameEngine {
private:
    json storyData;            
    std::string currentNode;   
    int affinity = 0;          
    std::string playerName;    
    DatabaseManager* db;       

public:
    // 构造函数：只负责读剧本和连数据库
    GameEngine(const std::string& jsonPath) {
        std::ifstream file(jsonPath);
        if (!file.is_open()) {
            std::cerr << "【引擎致命错误】找不到剧本文件: " << jsonPath << std::endl;
            exit(1); // 剧本都没有，游戏没法玩，直接退出程序
        }
        
        try {
            file >> storyData;
        } catch (const std::exception& e) {
            std::cerr << "【引擎致命错误】剧本 JSON 格式解析失败！请检查大括号和逗号。" << std::endl;
            std::cerr << "底层报错: " << e.what() << std::endl;
            exit(1);
        }

        currentNode = "0"; 
        
        // 如果数据库连不上，也报错
        db = new DatabaseManager("../data/save.db"); 
        if (!db) {
            std::cerr << "【引擎致命错误】数据库初始化失败！" << std::endl;
            exit(1);
        }
    }
        
    ~GameEngine() {
        delete db;
    }

    // 核心接口：处理玩家输入
    std::string processInput(const std::string& userInput) {
        // 1. 第一次输入，当做玩家名字，尝试读档
        if (playerName.empty()) {
            playerName = userInput;
            int savedAffinity = 0;
            std::string savedNode = "0";
            if (db->loadData(playerName, savedAffinity, savedNode)) {
                affinity = savedAffinity;
                currentNode = savedNode;
                return "【系统】欢迎回来，" + playerName + "！当前好感度: " + std::to_string(affinity) + "\n" + storyData["nodes"][currentNode]["npc_text"].get<std::string>();
            } else {
                return "【系统】欢迎新玩家，" + playerName + "！\n" + storyData["nodes"][currentNode]["npc_text"].get<std::string>();
            }
        }

        // 2. 获取当前节点的数据
        json node = storyData["nodes"][currentNode];
        std::string reply = node["default_reply"];
        std::string nextNode = node["default_next"];

        // 3. 遍历正则规则（包括好感度门槛判定）
        for (const auto& rule : node["rules"]) {
            // 如果有好感度门槛，先判断
            if (rule.contains("require_affinity") && affinity < rule["require_affinity"].get<int>()) {
                continue; // 好感度不够，跳过这条规则
            }

            std::regex pattern(rule["regex"].get<std::string>());
            if (std::regex_search(userInput, pattern)) {
                reply = rule["reply"];
                affinity += rule["affinity"].get<int>();
                nextNode = rule["next_node"];
                reply += "\n【好感度 +" + std::to_string(rule["affinity"].get<int>()) + "】";
                break; // 匹配成功，跳出循环
            }
        }

        // 4. 更新当前节点
        currentNode = nextNode;
        
        // 5. 👈 存档：每次对话后，把最新状态写进数据库
        db->saveData(playerName, affinity, currentNode);
        
        // 6. 拼接下一句台词（如果游戏没结束的话）
        if (currentNode != "-1") {
            reply += "\n" + storyData["nodes"][currentNode]["npc_text"].get<std::string>();
        } else {
            reply += "\n【游戏结束】";
        }
        
        return reply;
    }
};