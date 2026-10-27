#pragma once
#include <iostream>
#include <string>
#include <regex>
#include <fstream>
#include <vector>
#include "json.hpp" 
#include "DatabaseManager.h" 

using json = nlohmann::json;

class GameEngine {
private:
    json storyData;            
    std::string currentNode;   
    int affinity = 0;          
    std::string playerName;    
    DatabaseManager* db;       

public:
    GameEngine(const std::string& jsonPath) {
        std::ifstream file(jsonPath);
        if (!file.is_open()) {
            std::cerr << "【引擎错误】找不到剧本文件: " << jsonPath << std::endl;
            return;
        }
        file >> storyData;
        currentNode = "0"; 
        db = new DatabaseManager("../data/save.db"); 
    }

    ~GameEngine() { delete db; }

    void createNewPlayer(const std::string& name) {
        playerName = name;
        affinity = 0;
        currentNode = "0";
        db->saveData(playerName, affinity, currentNode);
        std::cout << "【系统】新存档已创建，欢迎，" << playerName << "！\n";
    }

    bool loadExistingPlayer(const std::string& name) {
        int savedAffinity = 0;
        std::string savedNode = "0";
        if (db->loadData(name, savedAffinity, savedNode)) {
            playerName = name;
            affinity = savedAffinity;
            currentNode = savedNode;
            std::cout << "【系统】欢迎回来，" << playerName << "！当前好感度: " << affinity << "\n";
            return true;
        } else {
            std::cout << "【系统】找不到该玩家的存档！\n";
            return false;
        }
    }

    std::vector<std::string> getCurrentNodePages() {
        std::vector<std::string> pages;
        if (currentNode == "-1") {
            pages.push_back("【游戏结束】");
            return pages;
        }
        
        json node = storyData["nodes"][currentNode];
        
        if (node.contains("narrator")) {
            pages.push_back("\033[36m" + node["narrator"].get<std::string>() + "\033[0m\n");
        }
        
        pages.push_back(node["npc_text"].get<std::string>() + "\n");
        
        std::string optionsAndHint = "";
        if (node.contains("options") && !node["options"].empty()) {
            for (const auto& opt : node["options"]) {
                optionsAndHint += "  " + opt.get<std::string>() + "\n";
            }
        }
        if (node.contains("hint")) {
            optionsAndHint += node["hint"].get<std::string>() + "\n";
        }
        if (!optionsAndHint.empty()) {
            pages.push_back(optionsAndHint);
        }
        
        return pages;
    }

    std::string processInput(const std::string& userInput) {
        if (currentNode == "-1") return "【游戏结束】";

        std::string reply = "";
        // 👇 核心修复：创建一个可修改的副本，不再修改只读的 userInput
        std::string processedInput = userInput; 
        
        json current_node_data = storyData["nodes"][currentNode];
        std::string nextNode = current_node_data["default_next"].get<std::string>();
        bool isOptionSelected = false;

        // 拦截数字选项
        if (current_node_data.contains("options") && !current_node_data["options"].empty()) {
            try {
                int choice = std::stoi(processedInput); 
                if (choice >= 1 && choice <= current_node_data["options"].size()) {
                    // 把转换后的文本赋值给副本
                    processedInput = current_node_data["options"][choice - 1].get<std::string>();
                    isOptionSelected = true; 
                }
            } catch (...) {} 
        }

        // 语义匹配
        if (isOptionSelected) {
            reply = ""; 
            for (const auto& rule : current_node_data["rules"]) {
                std::regex pattern(rule["regex"].get<std::string>());
                // 👇 使用 processedInput 进行匹配
                if (std::regex_search(processedInput, pattern)) { 
                    affinity += rule["affinity"].get<int>();
                    nextNode = rule["next_node"].get<std::string>();
                    break; 
                }
            }
        } else {
            reply = current_node_data["default_reply"].get<std::string>();
            for (const auto& rule : current_node_data["rules"]) {
                if (rule.contains("require_affinity") && affinity < rule["require_affinity"].get<int>()) {
                    continue; 
                }
                std::regex pattern(rule["regex"].get<std::string>());
                // 👇 使用 processedInput 进行匹配
                if (std::regex_search(processedInput, pattern)) {
                    reply = rule["reply"].get<std::string>();
                    affinity += rule["affinity"].get<int>();
                    nextNode = rule["next_node"].get<std::string>();
                    reply += "\n【好感度 +" + std::to_string(rule["affinity"].get<int>()) + "】\n";
                    break; 
                }
            }
        }

        currentNode = nextNode;
        if (db) db->saveData(playerName, affinity, currentNode);
        
        return reply;
    }
     // 专门提供给 UI 获取当前节点的选项列表
    std::vector<std::string> getCurrentOptions() {
        std::vector<std::string> opts;
        if (currentNode == "-1") return opts;
        
        json node = storyData["nodes"][currentNode];
        if (node.contains("options") && !node["options"].empty()) {
            for (const auto& opt : node["options"]) {
                opts.push_back(opt.get<std::string>());
            }
        }
        return opts;
    }

    bool isGameOver() { return currentNode == "-1"; }
    void showAllSaves() { if (db) db->listAllSaves(); }
    void deleteSaveData(const std::string& name) { if (db) db->deleteSave(name); }
    void applyPenalty(int penalty) {
        affinity += penalty;
        if (db && !playerName.empty()) db->saveData(playerName, affinity, currentNode);
    }
};