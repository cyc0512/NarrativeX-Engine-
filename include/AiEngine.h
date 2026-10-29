#pragma once
#include <iostream>
#include <fstream>
#include <string>
#include <curl/curl.h>
#include "json.hpp"

using json = nlohmann::json;

class AiEngine {
private:
    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
        ((std::string*)userp)->append((char*)contents, size * nmemb);
        return size * nmemb;
    }

public:
    static json analyzeInput(const std::string& userInput, int currentAffinity, const std::string& npcState, const std::string& routesDesc) {
        try {
            // 1. 读取 API Key
            std::string api_key = "未配置密钥";
            std::ifstream configFile("../config.json");
            if (configFile.is_open()) {
                json configData;
                configFile >> configData;
                if (configData.contains("api_key")) {
                    api_key = configData["api_key"].get<std::string>();
                }
                configFile.close();
            }

            // 2. 读取 prompts.json
            std::string system_prompt = "你是一个AI助手"; 
            std::ifstream promptFile("../data/prompts.json");
            if (promptFile.is_open()) {
                try {
                    json promptData;
                    promptFile >> promptData;
                    system_prompt = promptData["muziyin"]["system_prompt"].get<std::string>();
                } catch (const std::exception& e) {
                    std::cerr << "【配置解析错误】prompts.json 格式不对！" << std::endl;
                }
                promptFile.close();
            }

            // 3. 替换占位符
            size_t statePos = system_prompt.find("{NPC_STATE}");
            if (statePos != std::string::npos) {
                system_prompt.replace(statePos, 11, npcState);
            }
            size_t affinityPos = system_prompt.find("{AFFINITY}");
            if (affinityPos != std::string::npos) {
                system_prompt.replace(affinityPos, 10, std::to_string(currentAffinity));
            }
            size_t routesPos = system_prompt.find("{DYNAMIC_ROUTES}");
            if (routesPos != std::string::npos) {
                system_prompt.replace(routesPos, 15, routesDesc);
            }

            // 4. 发送网络请求
            std::string url = "https://open.bigmodel.cn/api/paas/v4/chat/completions";
            std::string model_name = "glm-4-flash";

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

                curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
                curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
                curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
                curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
                curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
                curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);

                CURLcode res = curl_easy_perform(curl);
                if (res != CURLE_OK) {
                    curl_easy_cleanup(curl);
                    curl_slist_free_all(headers);
                    return {{"affinity_change", 0}, {"next_node", ""}, {"reply", "（网络不通）"}};
                }

                curl_easy_cleanup(curl);
                curl_slist_free_all(headers);
            } else {
                return {{"affinity_change", 0}, {"next_node", ""}, {"reply", "（无法初始化网络库）"}};
            }

            if (readBuffer.empty()) {
                return {{"affinity_change", 0}, {"next_node", ""}, {"reply", "（未收到响应）"}};
            }

            // 5. 解析网络返回
            json response;
            try {
                response = json::parse(readBuffer);
            } catch (...) {
                return {{"affinity_change", 0}, {"next_node", ""}, {"reply", "（服务器乱码）"}};
            }
            
            if (response.contains("error")) {
                return {{"affinity_change", 0}, {"next_node", ""}, {"reply", "（API内部报错）"}};
            }

            std::string content = response["choices"][0]["message"]["content"];
            
            // 6. 提取并清洗 JSON
            size_t start = content.find("{");
            size_t end = content.rfind("}");
            
            if (start != std::string::npos && end != std::string::npos) {
                std::string json_str = content.substr(start, end - start + 1);
                std::string clean_str;
                for (char c : json_str) {
                    if (c >= 32 && c <= 126) clean_str += c;
                }
                
                try {
                    return json::parse(clean_str); 
                } catch (const std::exception& e) {
                    return {{"affinity_change", 0}, {"next_node", ""}, {"reply", content}};
                }
            } else {
                return {{"affinity_change", 0}, {"next_node", ""}, {"reply", content}}; 
            }
            
        } catch (const std::exception& e) {
            return {{"affinity_change", 0}, {"next_node", ""}, {"reply", "（系统故障）"}};
        } catch (...) {
            return {{"affinity_change", 0}, {"next_node", ""}, {"reply", "（未知故障）"}};
        }
    }
}; 