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
        affinity = 90; // 初始好感度90
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
        if (currentNode == "-1") return "【游戏结束】";
        std::string reply = "";
        std::string processedInput = userInput; 
        
        json current_node_data = storyData["nodes"][currentNode];
        
        // 安全获取默认节点
        std::string nextNode = currentNode;
        if (current_node_data.contains("default_next") && current_node_data["default_next"].is_string()) {
            nextNode = current_node_data["default_next"].get<std::string>();
        }
        
        bool isOptionSelected = false;

        // 1. 固定选项拦截
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

        // 2. 自由输入 / AI 介入
        if (isOptionSelected) {
            reply = "你做出了选择。"; 
        } else {
            std::string npcState = "正常状态";
            if (current_node_data.contains("npc_state") && current_node_data["npc_state"].is_string()) {
                npcState = current_node_data["npc_state"].get<std::string>();
            }

            // 👇 完美兼容 intent_nodes 和 dynamic_routes
            std::string routesDesc = "无特殊路径";
            if (current_node_data.contains("intent_nodes") && current_node_data["intent_nodes"].is_object()) {
                routesDesc = "";
                for (auto& [node_id, node_data] : current_node_data["intent_nodes"].items()) {
                    std::string desc = "无描述";
                    if (node_data.contains("description") && node_data["description"].is_string()) {
                        desc = node_data["description"].get<std::string>();
                    }
                    routesDesc += "【路径节点ID】: " + node_id + "\n【语义描述】: " + desc + "\n\n";
                }
            } else if (current_node_data.contains("dynamic_routes") && current_node_data["dynamic_routes"].is_array()) {
                routesDesc = "";
                for (const auto& route : current_node_data["dynamic_routes"]) {
                    routesDesc += "- 描述：" + route["description"].get<std::string>() + 
                                  " | 跳转节点：" + route["next_node"].get<std::string>() + "\n";
                }
            }

            std::cout << "【系统】木子因正在思考中...\n";
            json aiResult = AiEngine::analyzeInput(processedInput, affinity, npcState, routesDesc);
            
            if (aiResult.contains("reply") && aiResult["reply"].is_string()) {
                reply = aiResult["reply"].get<std::string>();
                lastAiReply = reply;
            } else {
                reply = "（木子因看着你，似乎不知道该说什么。）";
            }
            
            int affinityChange = 0;
            if (aiResult.contains("affinity_change") && aiResult["affinity_change"].is_number()) {
                affinityChange = aiResult["affinity_change"].get<int>();
            }
            affinity += affinityChange;
            reply += "\n【好感度 " + std::to_string(affinityChange) + "，当前好感: " + std::to_string(affinity) + "】\n";
            
            // 👇 核心：处理节点映射与跳转
            if (aiResult.contains("next_node") && !aiResult["next_node"].is_null() && aiResult["next_node"].is_string()) {
                std::string aiNextNode = aiResult["next_node"].get<std::string>();
                if (!aiNextNode.empty()) {
                    // 检查有没有配置语义映射（比如把"黑化类结局"翻译成"danger_end"）
                    if (current_node_data.contains("event_mapping") && current_node_data["event_mapping"].contains(aiNextNode)) {
                        nextNode = current_node_data["event_mapping"][aiNextNode].get<std::string>();
                        std::cout << "【系统】语义标签[" << aiNextNode << "] 映射为节点: " << nextNode << std::endl;
                    } else {
                        nextNode = aiNextNode;
                        std::cout << "【系统】AI 判定跳转至节点: " << nextNode << "\n";
                    }
                } else {
                    nextNode = currentNode;
                }
            } else {
                nextNode = currentNode; 
            }
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