#pragma once
#include <iostream>
#include <string>
#include <vector>

class TrieFilter {
private:
    struct TrieNode {
        bool isEnd = false; 
        std::vector<std::pair<std::string, TrieNode*>> children; 
    };
    
    TrieNode* root;

    // 按 UTF-8 规则安全提取字符
    std::string getUTF8Char(const std::string& text, size_t& i) {
        if (i >= text.length()) return "";
        unsigned char c = text[i];
        int len = 1;
        if ((c >> 5) == 0x06) len = 2;      
        else if ((c >> 4) == 0x0E) len = 3; 
        else if ((c >> 3) == 0x1E) len = 4; 
        
        std::string ch = text.substr(i, len);
        i += len - 1; 
        return ch;
    }

public:
    TrieFilter() { root = new TrieNode(); }

    void insert(const std::string& word) {
        TrieNode* current = root;
        size_t i = 0;
        while (i < word.length()) {
            std::string ch = getUTF8Char(word, i);
            bool found = false;
            for (auto& pair : current->children) {
                if (pair.first == ch) {
                    current = pair.second;
                    found = true;
                    break;
                }
            }
            if (!found) {
                TrieNode* newNode = new TrieNode();
                current->children.push_back({ch, newNode});
                current = newNode;
            }
            i++;
        }
        current->isEnd = true; 
    }

    bool containsSensitiveWord(const std::string& text) {
        size_t i = 0;
        while (i < text.length()) {
            TrieNode* current = root;
            size_t j = i;
            while (j < text.length()) {
                std::string ch = getUTF8Char(text, j);
                bool found = false;
                for (auto& pair : current->children) {
                    if (pair.first == ch) {
                        current = pair.second;
                        found = true;
                        break;
                    }
                }
                if (!found) break; 
                if (current->isEnd) return true; 
                j++;
            }
            i++;
        }
        return false;
    }
};