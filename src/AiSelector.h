#pragma once
#include "nx.hpp"
#include <string>

// nx::ILlmSelector 的生产实现。
// 职责：L2 受限选择——从 nx 给的 Top-K 短名单里选一个 node_id。
// 不负责路由裁决（nx 的 IntentMatcher 已裁决），不负责台词生成（EngineAdapter 另调）。
class AiSelector : public nx::ILlmSelector {
public:
    nx::NodeId select(const std::string& user_input,
                      const std::vector<nx::Candidate>& shortlist) override;
};