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

    static void loadConfig(std::string& api_key, std::string& api_url) {
        api_key = "未配置密钥";
        api_url.clear();
        std::ifstream cf("../config.json");
        if (!cf.is_open()) return;
        try {
            json c; cf >> c;
            if (c.contains("api_key") && c["api_key"].is_string())
                api_key = c["api_key"].get<std::string>();
            if (c.contains("api_url") && c["api_url"].is_string())
                api_url = c["api_url"].get<std::string>();
        } catch (...) {}
    }

public:
    static std::string callLlm(const std::string& systemPrompt,
                               const std::string& userInput,
                               double temperature = 0.7) {
        std::string api_key, api_url;
        loadConfig(api_key, api_url);

        if (api_key.empty() || api_key == "未配置密钥") {
            std::cerr << "【AiEngine】致命错误：未读取到 api_key！" << std::endl;
            return "";
        }

        // 👈 核心修复：URL 必须写域名！
        const std::string url = "https://open.bigmodel.cn/api/paas/v4/chat/completions";
        
        json reqBody;
        reqBody["model"] = "glm-4-flash";
        reqBody["messages"] = json::array({
            {{"role", "system"}, {"content", systemPrompt}},
            {{"role", "user"},   {"content", userInput}}
        });
        reqBody["temperature"] = temperature;
        const std::string payload = reqBody.dump();

        CURL* curl = curl_easy_init();
        if (!curl) return "";
        std::string readBuffer;
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, ("Authorization: Bearer " + api_key).c_str());
        headers = curl_slist_append(headers, "Content-Type: application/json");

        // 👈 这里：用域名 + 真实的 IP 欺骗底层网络
        struct curl_slist* resolve = nullptr;
        resolve = curl_slist_append(resolve, "open.bigmodel.cn:443:39.108.52.113");

        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_RESOLVE, resolve);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);

        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
        curl_slist_free_all(resolve);

        if (res != CURLE_OK) {
            std::cerr << "【AiEngine】网络请求报错: " << curl_easy_strerror(res) << std::endl;
            return "";
        }
        if (readBuffer.empty()) {
            std::cerr << "【AiEngine】收到空响应！" << std::endl;
            return "";
        }

        try {
            json response = json::parse(readBuffer);
            if (response.contains("error")) {
                std::cerr << "【AiEngine】API 报错: " << response["error"].dump() << std::endl;
                return "";
            }
            if (response.contains("choices") && response["choices"].is_array()
                && !response["choices"].empty()) {
                return response["choices"][0]["message"]["content"].get<std::string>();
            }
        } catch (const std::exception& e) {
            std::cerr << "【AiEngine】JSON解析失败: " << e.what() << "\n原文: " << readBuffer << std::endl;
        }
        return "";
    }
};