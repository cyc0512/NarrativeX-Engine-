#include <iostream>
#include <string>
#include <vector>

// 定义一个玩家
struct Player {
    std::string name;
    int affinity = 0; // 好感度
};

// 定义一个剧情节点
struct StoryNode {
    std::string npcDialogue; // NPC说的话
    std::vector<std::string> options; // 提供给玩家的选项
    std::vector<int> nextNodes;       // 选择每个选项后跳转的节点ID
    std::vector<int> affinityChanges; // 选择每个选项后的好感度变化
};

int main() {
    Player player;
    std::cout << "【系统】请输入你的名字: ";
    std::cin >> player.name;
    std::cout << "欢迎，" << player.name << "！\n\n";

    // 建立我们的剧情库（节点数组，索引就是节点ID）
    std::vector<StoryNode> story = {
        // 节点 0：开局
        {
            "小雅：你好呀，" + player.name + "。今天天气真好呢。",
            {"1. 是啊，和你一样好。", "2. 一般般吧，有点热。"},
            {1, 2}, // 选1去节点1，选2去节点2
            {10, -5} // 选1好感+10，选2好感-5
        },
        // 节点 1：开心线
        {
            "小雅：哎呀，你嘴真甜... (脸红了)",
            {"1. 趁机表白", "2. 转移话题"},
            {3, 4},
            {20, 0}
        },
        // 节点 2：直男线
        {
            "小雅：哦... 那你多喝点水吧。",
            {"1. 结束对话"}, // 只有一个选项
            {-1},           // 去 -1 代表游戏结束
            {0}
        },
        // 节点 3：表白线
        {
            "小雅：我... 我也觉得你挺好的。(她低下了头)",
            {"1. 结束游戏"},
            {-1},
            {0}
        },
        // 节点 4：转移话题
        {
            "小雅：嗯，对了，你等下要去干嘛？",
            {"1. 结束游戏"},
            {-1},
            {0}
        }
    };

    int currentNode = 0; // 从节点 0 开始

    // 游戏主循环
    while (currentNode != -1) {
        // 1. 打印 NPC 对话
        std::cout << "--------------------------------------------------\n";
        std::cout << story[currentNode].npcDialogue << "\n\n";

        // 2. 打印选项
        for (const std::string& option : story[currentNode].options) {
            std::cout << option << "\n";
        }

        // 3. 接收玩家选择
        int choice;
        std::cout << "\n请输入选项 (1-" << story[currentNode].options.size() << "): ";
        std::cin >> choice;

        // 简单的输入校验
        if (choice < 1 || choice > story[currentNode].options.size()) {
            std::cout << "【系统】无效的选项，请重新输入！\n";
            continue;
        }

        // 4. 处理结果（更新好感度，跳转节点）
        int index = choice - 1; // 数组是从0开始的
        player.affinity += story[currentNode].affinityChanges[index];
        std::cout << "【系统】好感度 " << (story[currentNode].affinityChanges[index] > 0 ? "+" : "") 
                  << story[currentNode].affinityChanges[index] << "，当前总好感度: " << player.affinity << "\n";

        currentNode = story[currentNode].nextNodes[index];
    }

    std::cout << "\n--------------------------------------------------\n";
    std::cout << "【游戏结束】最终好感度: " << player.affinity << "\n";
    if (player.affinity >= 30) {
        std::cout << "结局：你成功攻略了小雅！\n";
    } else {
        std::cout << "结局：你和小雅成为了普通朋友。\n";
    }

    return 0;
}