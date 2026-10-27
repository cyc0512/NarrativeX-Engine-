#pragma once
#include <string>
#include "TrieFilter.h"

class SecurityFilter {
private:
    TrieFilter badWords;
public:
    SecurityFilter() {
        // 这里可以放大量的敏感词
        badWords.insert("滚");
        badWords.insert("讨厌");
        badWords.insert("烦");
    }

    // 提供统一的安全检查接口
    bool checkSecurity(const std::string& input) {
        return badWords.containsSensitiveWord(input);
    }
};