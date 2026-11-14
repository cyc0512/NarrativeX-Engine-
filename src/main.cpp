#include <iostream>
#include <string>
#include <limits>
#include <vector>
#include <termios.h>
#include <unistd.h>
#include "EngineAdapter.h"

void printMainMenu() {
    std::cout << "\n=========================================\n";
    std::cout << "        🌸 FS 🌸\n";
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

    EngineAdapter engine("../data/story_v2.json");

    if (!engine.ready()) {
        std::cerr << "\n【FS】致命错误：" << engine.lastError() << "\n"
                  << "请把工作目录切到 build/ 或项目根目录后重试。\n";
        return 1;
    }

    bool isRunning = true;

    while (isRunning) {
        printMainMenu();
        std::string choice;
        std::getline(std::cin, choice);

        auto enterGame = [&]() {
            bool inGame = true;
            while (inGame) {
                // ========================================================
                // 阶段 1：节点展示（旁白翻页 + 选项打印）
                // 只有进入新节点时，才会执行这一段
                // ========================================================
                auto pages = engine.getCurrentNodePages();

                newt = oldt;
                newt.c_lflag &= ~(ICANON | ECHO);
                tcsetattr(STDIN_FILENO, TCSANOW, &newt);

                for (const auto& page : pages) {
                    if (!page.speaker.empty()) std::cout << "\n[" << page.speaker << "] ";
                    std::cout << page.text;
                    std::cout << "\033[90m(按空格键继续...)\033[0m";

                    char ch = std::cin.get();
                    while (ch != ' ') {
                        ch = std::cin.get();
                    }
                    std::cout << "\n";
                }

                tcsetattr(STDIN_FILENO, TCSANOW, &oldt);

                if (engine.isGameOver()) {
                    std::cout << "\n【FS】游戏结束，返回主菜单。\n";
                    inGame = false;
                    break;
                }

                // ========================================================
                // 阶段 2：原地对话循环
                // 如果玩家在当前节点自由输入，只要没跳节点，就留在这里继续聊
                // ========================================================
                bool stayInNode = true;
                while (stayInNode && inGame) {
                    std::vector<std::string> opts = engine.getCurrentOptions();
                    if (!opts.empty()) {
                        std::cout << "\n\033[36m【可选操作】\033[0m\n";
                        for (size_t i = 0; i < opts.size(); ++i) {
                            std::cout << "  " << (i + 1) << ". " << opts[i] << "\n";
                        }
                    }
                    if (engine.isFreeInputAvailable()) {
                        std::cout << "  (或直接输入你想说的话)\n";
                    }

                    std::cout << "\n\033[34m> \033[0m";
                    std::string userInput;
                    std::getline(std::cin, userInput);

                    if (userInput == "退出" || userInput == "结束") {
                        std::cout << "【FS】已保存进度，返回主菜单。\n";
                        inGame = false;
                        break;
                    }

                    std::string nodeBefore = engine.getCurrentNode();
                    std::string reply = engine.processInput(userInput);
                    std::string nodeAfter = engine.getCurrentNode();

                    std::cout << "\033[32m" << reply << "\033[0m\n";
                    std::cout << "\033[90m[当前节点: " << nodeAfter << "]\033[0m\n";

                    // 👇 核心：如果节点变了，说明剧情推进了，退出内层循环去加载新节点
                    if (nodeBefore != nodeAfter) {
                        stayInNode = false; 
                    }
                }
            }
        };

        if (choice == "1") {
            std::cout << "\n【FS】请输入新玩家的名字: ";
            std::string playerName;
            std::getline(std::cin, playerName);
            engine.createNewPlayer(playerName);
            enterGame();
        }
        else if (choice == "2") {
            std::cout << "\n【FS】请输入要读取的玩家名字: ";
            std::string playerName;
            std::getline(std::cin, playerName);
            if (engine.loadExistingPlayer(playerName)) {
                enterGame();
            } else {
                std::cout << "【FS】找不到该玩家的存档！\n";
            }
        }
        else if (choice == "3") {
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
            std::cout << "【FS】删除操作完成。\n";
        }
        else if (choice == "5") {
            std::cout << "【FS】感谢游玩，再见！\n";
            isRunning = false;
        }
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    return 0;
}