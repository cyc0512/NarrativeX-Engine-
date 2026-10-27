#include <iostream>
#include <string>
#include <limits>
#include <vector>
#include "GameEngine.h"
#include "SecurityFilter.h"

void printMainMenu() {
    std::cout << "\n=========================================\n";
    std::cout << "        🌸 高三恋爱文字冒险 🌸\n";
    std::cout << "=========================================\n";
    std::cout << "  1. 开始新游戏\n";
    std::cout << "  2. 读取已有存档\n";
    std::cout << "  3. 查看所有存档\n";
    std::cout << "  4. 删除存档\n";
    std::cout << "  5. 退出游戏\n";
    std::cout << "=========================================\n";
    std::cout << "请选择 (1-5): ";
}

int main() {
    GameEngine engine("../data/story.json");
    SecurityFilter security;

    bool isRunning = true;
    
    while (isRunning) {
        printMainMenu();
        std::string choice;
        std::getline(std::cin, choice);

        // 公共游戏主循环
        auto enterGame = [&]() {
            bool inGame = true;
            while (inGame) {
                std::vector<std::string> pages = engine.getCurrentNodePages();
                
                // 翻页逻辑：按 Tab + 回车 翻页
                for (const auto& page : pages) {
                    std::cout << "\n" << page;
                    std::cout << "\033[90m(按 Tab 键然后回车继续...)\033[0m\n";
                    
                    std::string input;
                    std::getline(std::cin, input); // 等待玩家输入并回车
                    
                    // 👇 只要玩家输入的包含 Tab 或者直接按了回车，就翻页
                    // 这里我们宽容一点，如果玩家直接按回车也让他翻，免得卡住
                }

                if (engine.isGameOver()) {
                    std::cout << "\n【系统】游戏结束，返回主菜单。\n";
                    inGame = false;
                    break;
                }

                // 等待玩家对话输入
                std::cout << "\n\033[34m> \033[0m";
                std::string userInput;
                std::getline(std::cin, userInput);

                if (userInput == "退出" || userInput == "结束") {
                    std::cout << "【系统】已保存进度，返回主菜单。\n";
                    inGame = false;
                    break;
                }

                if (security.checkSecurity(userInput)) {
                    std::cout << "\033[31m【系统警告】检测到不文明用语，木子因委屈地低下了头...\n【好感度 -20】\033[0m\n";
                    engine.applyPenalty(-20); 
                    continue; 
                }

                std::string reply = engine.processInput(userInput);
                std::cout << "\033[32m" << reply << "\033[0m\n";
            }
        };

        if (choice == "1") {
            std::cout << "\n【系统】请输入新玩家的名字: ";
            std::string playerName;
            std::getline(std::cin, playerName);
            engine.createNewPlayer(playerName); 
            enterGame(); 
        } 
        else if (choice == "2") {
            std::cout << "\n【系统】请输入要读取的玩家名字: ";
            std::string playerName;
            std::getline(std::cin, playerName);
            if (engine.loadExistingPlayer(playerName)) {
                enterGame(); 
            }
        } 
        else if (choice == "3") {
            engine.showAllSaves();
        }
        else if (choice == "4") {
            std::cout << "请输入要删除的玩家名: ";
            std::string delName;
            std::getline(std::cin, delName);
            engine.deleteSaveData(delName);
        }
        else if (choice == "5") {
            std::cout << "【系统】感谢游玩，再见！\n";
            isRunning = false;
        }
    }

    return 0;
}