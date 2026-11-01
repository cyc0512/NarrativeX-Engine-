#pragma once
#include <iostream>
#include <fstream>
#include <string>
#include <curl/curl.h>
#include "json.hpp"

using json = nlohmann::json;

class AiEngine {
private:
    // 清洗 LLM 返回的花括号片段：**只**去掉 markdown 代码围栏（反引号）与裸控制字符。
    // 所有 >= 0x80 的字节都是 UTF-8 多字节序列的一部分，必须原样保留——
    // 中文 / emoji / 「」/ 全角标点全靠它们。这里一旦做字符集过滤就会静默吞掉内容。
    static std::string sanitizeJsonText(const std::string& in) {
        std::string out;
        out.reserve(in.size());
        for (unsigned char c : in) {
            if (c == 0x60) continue;   // '`' markdown 代码围栏
            if (c < 0x20) continue;    // 裸控制字符（JSON 字符串里本就不合法）
            out.push_back(static_cast<char>(c));
        }
        return out;
    }

    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
        ((std::string*)userp)->append((char*)contents, size * nmemb);
        return size * nmemb;
    }

public:
    static json analyzeInput(const std::string& userInput, int currentAffinity, const std::string& npcState, const std::string& routesDesc) {
        try {
            // 1. 读取 API Key（以及可选的 api_url 覆盖，供本地 mock / 自建网关 / 取证复现使用）
            std::string api_key = "未配置密钥";
            std::string api_url;
            std::ifstream configFile("../config.json");
            if (configFile.is_open()) {
                json configData;
                configFile >> configData;
                if (configData.contains("api_key")) {
                    api_key = configData["api_key"].get<std::string>();
                }
                if (configData.contains("api_url") && configData["api_url"].is_string()) {
                    api_url = configData["api_url"].get<std::string>();
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
            // 默认走智谱官方端点；config.json 里配了 api_url 就走它（本地 mock / 自建网关）。
            std::string url = api_url.empty()
                            ? "https://open.bigmodel.cn/api/paas/v4/chat/completions"
                            : api_url;
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
            std::cout << "【AI原始文本】 " << content << std::endl;

            // 6. 提取并清洗 JSON
            size_t start = content.find("{");
            size_t end = content.rfind("}");
            
            if (start != std::string::npos && end != std::string::npos) {
                std::string json_str = content.substr(start, end - start + 1);

                // 只剥离 markdown 代码围栏（反引号）与控制字符。
                // ⚠️ 严禁任何字符集过滤：旧实现用 `c >= 32 && c <= 126` 只保留 ASCII，
                //    于是全部 UTF-8 多字节字节（>= 0x80）被静默删光 —— 中文、emoji、
                //    「」与全角标点全没了，JSON 照样解析成功，玩家只看到一片空白（R-02）。
                std::string clean_str = sanitizeJsonText(json_str);

                try {
                    json result = json::parse(clean_str);
                    
                    // 核心修复：确保 JSON 解析成功后，真的包含了 reply 字段！
                    if (result.contains("reply") && result["reply"].is_string()) {
                        return result; // 完美返回
                    } else {
                        std::cerr << "【AI解析警告】JSON没有reply字段！" << std::endl;
                        return {{"affinity_change", 0}, {"next_node", ""}, {"reply", "（木子因似乎不知道该说什么。）"}};
                    }
                } catch (const std::exception& e) {
                    std::cerr << "【AI解析失败】" << e.what() << "\n清洗后: [" << clean_str << "]" << std::endl;
                    // 如果彻底解析失败，不要把带乱码的 content 丢给玩家，给一个兜底
                    return {{"affinity_change", 0}, {"next_node", ""}, {"reply", "（木子因走神了，没有听清你的话。）"}};
                }
            } else {
                // 如果连花括号都没有，说明 AI 回了纯文本
                return {{"affinity_change", 0}, {"next_node", ""}, {"reply", content}}; 
            }
        } catch (const std::exception& e) {
            return {{"affinity_change", 0}, {"next_node", ""}, {"reply", "（系统异常）"}};
        } catch (...) {
            return {{"affinity_change", 0}, {"next_node", ""}, {"reply", "（未知故障）"}};
        }
        // 上面每条分支都 return 了，但编译器推不出"无路径漏返回"
        // （会警告 control reaches end of non-void function）。
        // 这里显式兜一层，把"所有路径都有返回值"变成事实而不是推断。
        return {{"affinity_change", 0}, {"next_node", ""}, {"reply", "（未知故障）"}};
    } 
}; 