#pragma once
#include <iostream>
#include <fstream>
#include <string>
#include <curl/curl.h>
#include "json.hpp"

using json = nlohmann::json;

class AiEngine {
private:
    // 接收网络数据的回调函数
    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
        ((std::string*)userp)->append((char*)contents, size * nmemb);
        return size * nmemb;
    }

public:
        static json analyzeInput(const std::string& userInput, int currentAffinity) {
        // 1. 安全地读取密钥
        std::string api_key = "未配置密钥";
        std::ifstream configFile("../config.json");
        if (configFile.is_open()) {
            json configData;
            try {
                configFile >> configData;
                if (configData.contains("api_key")) {
                    api_key = configData["api_key"].get<std::string>();
                }
            } catch (...) {
                std::cerr << "【配置解析错误】config.json 格式不对！" << std::endl;
            }
            configFile.close();
        }

        std::string url = "https://open.bigmodel.cn/api/paas/v4/chat/completions";
        std::string model_name = "glm-4-flash";

               std::string system_prompt = 
            "你正在扮演一个名叫【木因子】的高中女生。\n"
            "【背景设定】：你从小被主角家收养，主角现在上大学。你对主角有着极度扭曲、病态的依赖感（病娇倾向）。你不能接受主角离开你，一旦主角提到别人或者要离开，你的情绪就会极度不稳定。\n"
            "【性格特征】：表面上乖巧懂事，实则内心占有欲极强。说话时经常带有轻微的试探和哀求，喜欢用省略号……偶尔会说出让人后背发凉的台词。\n"
            "【规则限制】：\n"
            "1. 绝对不要承认自己是人工智能，你就是木因子。\n"
            "2. 玩家说的每一句话，都会严重影响你的情绪。\n"
            "3. 你必须且只能返回严格的 JSON 格式（不要带任何多余文字或 markdown 符号）：\n"
            "{\"affinity_change\": 数值, \"reply\": \"木因子的回复\"}\n"
            "4. affinity_change 范围在 -10 到 +10 之间。玩家夸奖你，数值为正；玩家冷落你或提到其他人，数值为负。当前好感度为：" + std::to_string(currentAffinity);

        json reqBody;
        reqBody["model"] = model_name;
        reqBody["messages"] = json::array({
            {{"role", "system"}, {"content", system_prompt}},
            {{"role", "user"}, {"content", userInput}}
        });
        reqBody["temperature"] = 0.7;

        std::string payload = reqBody.dump();

        CURL* curl = curl_easy_init();
        std::string readBuffer;
        if (curl) {
            struct curl_slist* headers = NULL;
            headers = curl_slist_append(headers, ("Authorization: Bearer " + api_key).c_str());
            headers = curl_slist_append(headers, "Content-Type: application/json");

            //  极其关键的设置：忽略 SSL 证书校验（避免虚拟机缺证书导致连接失败）
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
            
            // 设置超时时间，防止卡死
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);

            CURLcode res = curl_easy_perform(curl);
            if (res != CURLE_OK) {
                // 如果有网络错误，直接打印并返回安全兜底
                std::cerr << "【AI网络请求失败】 " << curl_easy_strerror(res) << std::endl;
                curl_easy_cleanup(curl);
                curl_slist_free_all(headers);
                return {{"affinity_change", 0}, {"reply", "（网络不通，木因子听不见你的声音）"}};
            }

            curl_easy_cleanup(curl);
            curl_slist_free_all(headers);
        } else {
            return {{"affinity_change", 0}, {"reply", "（无法初始化网络库）"}};
        }

        // 👇 终极兜底：如果网络通了但返回空
        if (readBuffer.empty()) {
            std::cerr << "【AI致命错误】 返回数据为空！" << std::endl;
            return {{"affinity_change", 0}, {"reply", "（木因子那边没有信号）"}};
        }

        try {
            std::cout << "【AI原始返回】 " << readBuffer << std::endl; // 看看服务器到底回了什么
            json response = json::parse(readBuffer);
            
            if (response.contains("error")) {
                std::cerr << "【API报错】 " << response["error"].dump() << std::endl;
                return {{"affinity_change", 0}, {"reply", "（AI内部报错，请检查密钥）"}};
            }

            std::string content = response["choices"][0]["message"]["content"];
            std::cout << "【AI原始文本】 " << content << std::endl;

            size_t start = content.find("{");
            size_t end = content.rfind("}");
            
            if (start != std::string::npos && end != std::string::npos) {
                std::string json_str = content.substr(start, end - start + 1);
                return json::parse(json_str); 
            } else {
                return {{"affinity_change", 0}, {"reply", content}}; 
            }
            
        } catch (const std::exception& e) {
            std::cerr << "【AI解析失败】 " << e.what() << "\n原始返回: " << readBuffer << std::endl;
            return {{"affinity_change", 0}, {"reply", "（木因子说了一句你听不懂的话）"}};
        }
    }
};