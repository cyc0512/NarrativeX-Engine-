#include <iostream>
#include <string>
#include <limits>
#include <vector>
#include <termios.h> 
#include <unistd.h>  
#include "GameEngine.h"
#include "SecurityFilter.h"

void printMainMenu() {
    std::cout << "\n=========================================\n";
    std::cout << "        🌸ISOLATE 🌸\n";
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
    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);

    GameEngine engine("../data/story.json");
    SecurityFilter security;

    bool isRunning = true;
    
    while (isRunning) {
        printMainMenu();
        std::string choice;
        std::getline(std::cin, choice);

        auto enterGame = [&]() {
            bool inGame = true;
            while (inGame) {
                std::vector<std::string> pages = engine.getCurrentNodePages();
                
                newt = oldt;
                newt.c_lflag &= ~(ICANON | ECHO);
                tcsetattr(STDIN_FILENO, TCSANOW, &newt);

                for (const auto& page : pages) {
                    std::cout << "\n" << page;
                    std::cout << "\033[90m(按空格键继续...)\033[0m";
                    
                    char ch = std::cin.get(); 
                    while (ch != ' ') {
                        ch = std::cin.get();
                    }
                    std::cout << "\n"; 
                }

                if (engine.isGameOver()) {
                    std::cout << "\n【系统】游戏结束，返回主菜单。\n";
                    inGame = false;
                    break;
                }

                tcsetattr(STDIN_FILENO, TCSANOW, &oldt);

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
            } else {
                std::cout << "【系统】找不到该玩家的存档！\n";
            }
        } 
        else if (choice == "3") {
            // 👇 终端版直接调用 getAllSaves 打印
            auto saves = engine.getAllSaves();
            std::cout << "\n--- 存档列表 ---\n";
            for (const auto& s : saves) {
                std::cout << "玩家: " << s.playerName << " | 好感度: " << s.affinity << "\n";
            }
            std::cout << "----------------\n";
        }
        else if (choice == "4") {
            std::cout << "请输入要删除的玩家名: ";
            std::string delName;
            std::getline(std::cin, delName);
            engine.deleteSaveData(delName);
            std::cout << "【系统】删除操作完成。\n";
        }
        else if (choice == "5") {
            std::cout << "【系统】感谢游玩，再见！\n";
            isRunning = false;
        }
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    return 0;
}