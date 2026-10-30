// ---------------------------------------------------------------------------
//  NarrativeX Engine :: nx.hpp  （伞形头文件）
//
//  使用者只需要 #include "nx.hpp"。
//  内核零依赖（仅 C++17 标准库）；StoryLoader 依赖 vendored 的 nlohmann/json。
//
//  最小可用示例：
//      #include "nx.hpp"
//      nx::Catalog cat = nx::StoryLoader::load(read_file("data/story_v2.json"));
//      auto st = nx::StoryLoader::new_state("player1");
//      nx::NarrativeEngine eng(cat, st);
//      eng.set_selector(&my_llm_selector);          // 不设则退化为纯本地 L0/L1 路由
//      auto r = eng.step("1");
//      std::cout << eng.debug_frontier();           // 剧情断裂时第一件事看这个
// ---------------------------------------------------------------------------
#pragma once

#include "nx/Types.hpp"
#include "nx/Condition.hpp"
#include "nx/NodeModel.hpp"
#include "nx/GameState.hpp"
#include "nx/Frontier.hpp"
#include "nx/PendingPool.hpp"
#include "nx/IntentMatch.hpp"
#include "nx/PromptAssembler.hpp"
#include "nx/NarrativeEngine.hpp"
#include "nx/StoryLoader.hpp"
