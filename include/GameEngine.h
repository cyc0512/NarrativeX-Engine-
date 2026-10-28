#pragma once
#include <iostream>
#include <string>
#include <regex>
#include <fstream>
#include <vector> 
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
    DatabaseManager* db;
    std::string lastAiReply;       

public:
    GameEngine(const std::string& jsonPath) {
        std::ifstream file(jsonPath);
        if (!file.is_open()) return;
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
    }

    bool loadExistingPlayer(const std::string& name) {
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
        if (!optionsAndHint.empty()) pages.push_back(optionsAndHint);
        
        return pages;
    }

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

    std::string processInput(const std::string& userInput) {
        if (currentNode == "-1") return "【游戏结束】";
        std::string reply = "";
        std::string processedInput = userInput; 
        
        json current_node_data = storyData["nodes"][currentNode];
        std::string nextNode = current_node_data["default_next"].get<std::string>();
        bool isOptionSelected = false;

        if (current_node_data.contains("options") && !current_node_data["options"].empty()) {
            try {
                int choice = std::stoi(processedInput); 
                if (choice >= 1 && choice <= current_node_data["options"].size()) {
                    processedInput = current_node_data["options"][choice - 1].get<std::string>();
                    isOptionSelected = true; 
                }
            } catch (...) {} 
        }

        if (isOptionSelected) {
            reply = ""; 
            for (const auto& rule : current_node_data["rules"]) {
                std::regex pattern(rule["regex"].get<std::string>());
                if (std::regex_search(processedInput, pattern)) { 
                    affinity += rule["affinity"].get<int>();
                    nextNode = rule["next_node"].get<std::string>();
                    break; 
                }
            }
        } else {
            // 👇 把玩家的话丢给 AI 处理
            std::cout << "【系统】木因子正在思考中...\n";
            json aiResult = AiEngine::analyzeInput(processedInput, affinity);
            
            // 👇 这里加上了最强防御！避免任何崩溃！
            if (aiResult.contains("reply") && aiResult["reply"].is_string()) {
                reply = aiResult["reply"].get<std::string>();
                lastAiReply = reply;
            } else {
                reply = "（木因子看着你，似乎不知道该说什么。）";
            }
            
            int affinityChange = 0;
            if (aiResult.contains("affinity_change") && aiResult["affinity_change"].is_number()) {
                affinityChange = aiResult["affinity_change"].get<int>();
            }
            
            affinity += affinityChange;
            reply += "\n【好感度 " + std::to_string(affinityChange) + "，当前好感: " + std::to_string(affinity) + "】\n";
            
            nextNode = current_node_data["default_next"].get<std::string>();
        }

        currentNode = nextNode;
        if (db) db->saveData(playerName, affinity, currentNode);
        
        return reply;
    }

    bool isGameOver() { return currentNode == "-1"; }

    std::vector<DatabaseManager::SaveInfo> getAllSaves() {
        return db->getAllSaves();
    }
        std::string getLastAiReply() {
        return lastAiReply;
    }

    void deleteSaveData(const std::string& name) {
        db->deleteSave(name);
    }

    void applyPenalty(int penalty) {
        affinity += penalty;
        if (db && !playerName.empty()) db->saveData(playerName, affinity, currentNode);
    }
};