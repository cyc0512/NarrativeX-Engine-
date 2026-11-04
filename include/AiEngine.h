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
    // 只负责底层的 HTTP 请求与大模型通信
    static std::string callLlm(const std::string& systemPrompt,
                               const std::string& userInput,
                               double temperature = 0.7) {
        std::string api_key, api_url;
        loadConfig(api_key, api_url);

        const std::string url = api_url.empty()
            ? "https://open.bigmodel.cn/api/paas/v4/chat/completions"
            : api_url;
        const std::string model_name = "glm-4-flash";

        json reqBody;
        reqBody["model"] = model_name;
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

        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);

        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
        if (res != CURLE_OK || readBuffer.empty()) return "";

        try {
            json response = json::parse(readBuffer);
            if (response.contains("error")) return "";
            if (response.contains("choices") && response["choices"].is_array()
                && !response["choices"].empty()) {
                return response["choices"][0]["message"]["content"].get<std::string>();
            }
        } catch (...) {}
        return "";
    }
};