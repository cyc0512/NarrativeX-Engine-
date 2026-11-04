#include "AiSelector.h"
#include "AiEngine.h"

nx::NodeId AiSelector::select(const std::string& user_input,
                              const std::vector<nx::Candidate>& shortlist) {
    if (shortlist.empty()) return {};
    if (shortlist.size() == 1) return shortlist.front().id;

    std::string routes;
    for (const auto& c : shortlist) {
        routes += "- <" + c.id + ">\n";
    }
    const std::string sys =
        "你是剧情路由器。根据玩家的话，从下列节点中选一个最匹配的，"
        "只输出节点 ID（尖括号里的内容），不要输出任何其他字符。\n"
        "注意：如果玩家只是在闲聊、表达情绪，或者这句话接不上任何一个候选节点，"
        "请直接返回空字符串（留空），不要强行选择。\n" 
        "候选节点：\n" + routes;

    std::string picked;
    try {
        picked = AiEngine::callLlm(sys, user_input, 0.0);
    } catch (...) {
        return {}; 
    }

    while (!picked.empty() && (picked.back() == '\n' || picked.back() == ' ' || picked.back() == '\r'))
        picked.pop_back();
    while (!picked.empty() && (picked.front() == '\n' || picked.front() == ' ' || picked.front() == '\r'))
        picked.erase(picked.begin());
    if (picked.size() >= 2 && picked.front() == '<' && picked.back() == '>')
        picked = picked.substr(1, picked.size() - 2);

    for (const auto& c : shortlist) if (c.id == picked) return picked;
    
    return ""; // 如果不匹配，返回空，表示留在原地！不再是强制 Top1
}