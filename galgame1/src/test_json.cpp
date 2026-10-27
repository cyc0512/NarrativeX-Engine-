#include <iostream>
#include <fstream>
#include <string>
#include "../include/json.hpp" // 引入刚才下载的头文件

using json = nlohmann::json;

int main() {
    // 1. 构造数据
    json story;
    story["npc_name"] = "小雅";
    story["dialogue"] = "今天天气真好呢。";
    story["affinity_change"] = 10;
    
    // 2. 存进文件
    std::ofstream outFile("save_data.json");
    outFile << story.dump(4); // 4个空格缩进，好看
    outFile.close();
    std::cout << "数据已保存到 save_data.json\n";

    // 3. 从文件读出来
    std::ifstream inFile("save_data.json");
    if (inFile.is_open()) {
        json loadedData;
        inFile >> loadedData; // 解析文件
        
        std::cout << "读取到 NPC 名字: " << loadedData["npc_name"] << std::endl;
        std::cout << "读取到好感度变化: " << loadedData["affinity_change"] << std::endl;
    }

    return 0;
}