# NarrativeX —— 项目计划书（动态迷宫叙事引擎）

> 配套文档：`docs/FEASIBILITY_REVIEW.md`（可行性评审，回答"能不能做"）
> 本文档回答"**按什么顺序做、每一步做完算完成**"。
> 版本：v1.0 · 2026-10-30 · 依赖 C++14+、SQLite3、可选 libcurl / Qt5

---

## 〇、怎么用这份计划书

三条使用约定：

1. **每个里程碑必须有可勾选的验收标准**。没通过验收就不许往下走——这个项目最容易犯的错，是在一个摇摆的地基上继续垒代码。
2. **新引擎与旧代码并行存在**，通过适配层接入，不做破坏性重写。`engine/` 是零依赖内核，可以独立编译测试。
3. **顺序不可打乱**。M0 没做完就去做 M2，你会在 M2 里反复回来怀疑"是不是 AI 不听话"，而实际上可能是 M0 的 bug 在吞内容。

---

## 一、目标与非目标

### 1.1 本项目要交付什么

一个**状态驱动的叙事引擎**，作者写的是"节点 + 条件 + 效果"，而不是"剧情线"。玩家的每一步输入实时重算可用节点集合（Frontier），错过的分支进入挂起池等待被"挽回"，时间永不倒流。

### 1.2 明确不做（写在这里是为了防止范围蔓延）

| 不做 | 理由 | 什么时候再考虑 |
|---|---|---|
| Embedding 向量召回 | 引入第二个 API + 新失败模式；当前"关键词 + 中文二元组 Jaccard + MMR"在几十个候选量级已足够 | 候选 > 50 且字面相似明显失效时 |
| 真正的图上 BFS 遍历 | 迷宫是"动态重算的前沿集合"，不是静态图最短路。保留 BFS 的比喻，实现是重算 + 分层排序 | 永不 |
| 多人 / Web 服务化 | 单机 Qt 已能完整验证叙事模型 | M5 之后，`engine/` 零依赖可复用 |
| 无限上下文 | 用滚动摘要 + 按预算裁剪解决，不堆历史 | 永远不堆，只优化摘要质量 |

---

## 二、架构分层与新旧代码映射

```
┌──────────────────────────────────────────────────────────────┐
│ L2  表现层      Qt5 Widgets (src/qt_main.cpp)                │
│                 只做渲染与输入，无业务逻辑                     │
├──────────────────────────────────────────────────────────────┤
│ L1  集成适配层  ★本项目的"接缝"★                              │
│   CurlLlmSelector  : ILlmSelector   ← 替换旧的 AiEngine      │
│   GameSession      : EventStore + NarrativeEngine 编排        │
│   QtWorkerBridge   : QtConcurrent → worker 线程               │
├──────────────────────────────────────────────────────────────┤
│ L0  内核（零依赖，可独立单测）                                 │
│   engine/include/nx/*.hpp                                     │
│   Condition · NodeModel · GameState · Frontier · PendingPool  │
│   IntentMatch · PromptAssembler · NarrativeEngine · StoryLoader│
└──────────────────────────────────────────────────────────────┘
      ↓ 依赖
   persistence/include/nx/EventStore.hpp  (+ migrations/*.sql)
```

### 旧代码去向

| 旧文件 | 处置 | 说明 |
|---|---|---|
| `include/AiEngine.h` | **替换** | 接口收窄为 `ILlmSelector::select()`；顺带修掉 P0 中文清洗 bug |
| `include/GameEngine.h` | **降级为调用方** | 编排逻辑迁入 `GameSession`，它只保留 UI 友好的 getter |
| `include/DatabaseManager.h` | **替换** | 全部改 prepared statement，并迁移到 `002_bfs_state.sql` 表结构 |
| `include/SecurityFilter.h` / `TrieFilter.h` | 保留 | 挂到 L1 的输入前置过滤，词库待扩充 |
| `data/story.json` | 迁到 `story_v2` 格式 | 提供一次性转换脚本（M1 产出） |
| `src/main.cpp` / `simple_galgame.cpp` | 保留为 CLI 入口 | 用于无 Qt 环境下的引擎自测 |

---

## 三、已交付内容（本次已完成）

| 路径 | 行数 | 职责 |
|---|---|---|
| `engine/include/nx/Types.hpp` | 107 | 基础类型、Seq、token 估算（CJK 感知） |
| `engine/include/nx/Condition.hpp` | 126 | 条件 AST：tag/item/affinity/step/phase/not/all/any |
| `engine/include/nx/NodeModel.hpp` | 224 | NodeSpec / Catalog / 倒排索引 |
| `engine/include/nx/GameState.hpp` | 73 | tags/items/pending/visited/rng_state —— 缺一个就会错乱 |
| `engine/include/nx/Frontier.hpp` | 298 | **空缺 1 的答案**：硬分桶 + 软打分 + 稳定排序 |
| `engine/include/nx/PendingPool.hpp` | 109 | 后向指针扫描、单调唤醒、挽回到期 |
| `engine/include/nx/IntentMatch.hpp` | 267 | L0/L1 本地漏斗 + `ILlmSelector` 抽象 + MMR |
| `engine/include/nx/PromptAssembler.hpp` | 136 | Token 预算裁剪、必留块保护 |
| `engine/include/nx/NarrativeEngine.hpp` | 239 | 编排：`step()` 状态机，seq 唯一推进点 |
| `engine/include/nx/StoryLoader.hpp` | 405 | JSON → Catalog，含 `lint()` 静态校验 |
| `persistence/include/nx/EventStore.hpp` | 371 | **空缺 2 的答案**：事件溯源 + 快照 + LLM 幂等 |
| `persistence/migrations/*.sql` | 224 | 四道闸门（触发器层面，应用层绕不过） |
| `reference/nxref.py` + `simulate.py` | 1798 | Python 等价参考实现 + 24 项验证 |
| `engine/tests/test_core.cpp` | 127 | C++ 冒烟 + 跨语言一致性比对 |

**验证状态**：Python 24/24 ✓ · SQLite 17/17 ✓ · C++ 编译零 warning 且与 Python 轨迹逐行一致 ✓

---

## 四、里程碑

### M0 —— 止血（建议 1~2 天，必须最先做）

**目标**：让现有 demo 能真正跑出内容。

| # | 任务 | 优先级 | 产出 |
|---|---|---|---|
| M0-1 | 修 `AiEngine` 中文清洗：删除字符集过滤，改为"去 markdown 围栏 + 提取首个完整 JSON 对象 + repair loop" | P0 | `AiEngine.cpp` |
| M0-2 | 所有 SQL 改 prepared statement（6 处） | P0 | `DatabaseManager.cpp` |
| M0-3 | 数据目录改为参数/环境变量 `NX_DATA_DIR`，启动时校验可读写 | P1 | `GameEngine.cpp` |
| M0-4 | 配置文件与 prompts 只加载一次并缓存；API Key 优先读环境变量 | P1 | `AiEngine.cpp` |
| M0-5 | `addItem` + `saveData` 包进一个事务 | P1 | `GameEngine.cpp` |
| M0-6 | 所有 `storyData["nodes"][id]` 改 `find()` + 显式校验（现为隐式插入 null） | P1 | `GameEngine.cpp` |
| M0-7 | 补齐空文件 `data/items.json` | P2 | data |

**验收标准**
- [ ] AI 返回含中文台词的 JSON，界面能完整显示（含省略号、顿号）
- [ ] 玩家名填 `O'Brien` 能存档；填 `x'); DROP TABLE player_save;--` 表还在
- [ ] 从任意目录启动程序都能正确打开 `save.db`
- [ ] 连续 20 轮对话，`save.db` 不出现半写状态（用 `signal_kill` 强杀验证）

---

### M1 —— 内核落地（建议 5~8 天）

**目标**：新引擎在**离线**（无 LLM）状态下跑完整剧本。

| # | 任务 | 说明 |
|---|---|---|
| M1-1 | 编写 `tools/story_v1_to_v2.py` | 把 `data/story.json` 转成 story_v2 schema，`requires`/`intent`/`redeem_gate` 留空待补 |
| M1-2 | 用真实剧本填 `intent.description` | **这是 AI 选路的唯一依据**，必须人写，不能留空（`lint()` 会拦） |
| M1-3 | `StoryLoader::lint()` 接进 CI | 任何 lint error 直接 fail build |
| M1-4 | 补充 `story_v2` 的 fallback 节点 | 每个 phase 至少一个 `kind=fallback`，这是死锁保底 |
| M1-5 | 扩大 `engine/tests/test_core.cpp` | Frontier 排序、Pending 单调性、涌现生成、双语一致性各一组断言 |

**验收标准**
- [ ] `StoryLoader::lint()` 在完整剧本上返回 0 error
- [ ] 离线跑通 30 轮随机输入，零死锁（`feasible` 永不为空）
- [ ] 同一串输入跑两次，state hash 完全一致（确定性）
- [ ] `phase` 索引单调不回退（用反例节点验证被拒）

---

### M2 —— AI 意图框架接回生产（建议 3~5 天）

**目标**：三级漏斗在真实 GLM API 上工作，并把调用次数压下来。

**核心是把 LLM 依赖抽象掉**（已完成），生产实现只需填一个虚函数：

```cpp
// L1/adapters/CurlLlmSelector.hpp —— 唯一需要写的新代码
class CurlLlmSelector : public nx::ILlmSelector {
public:
    nx::NodeId select(const std::string& user_input,
                      const std::vector<nx::Candidate>& shortlist) override {
        // 1) 用 nx::PromptAssembler 组装 prompt（已实现预算裁剪）
        // 2) libcurl 调用 GLM，**connect=3s / total=25s**，带指数退避重试
        // 3) ★不要用旧的字符集清洗★：去围栏 → 找首 JSON → 失败则 repair loop
        // 4) 返回值越界 / 不在 shortlist 里 → 框架会自动丢弃并降级 L1
    }
};

// 装配：
engine.set_selector(&curl_selector);      // 生产
engine.set_selector(nullptr);             // 离线 → 自动退化为 L0/L1 本地判定
```

| # | 任务 | 说明 |
|---|---|---|
| M2-1 | 实现 `CurlLlmSelector` | 从旧 `AiEngine` 搬运 curl 代码，**丢弃全部清洗逻辑** |
| M2-2 | JSON 提取工具 `nx::extract_json_object()` | UTF-8 安全 + repair loop，放 `Types.hpp`，单独补测试用例 |
| M2-3 | LLM 调用接 `EventStore::begin_llm_call/resolve_llm_call` | 崩溃恢复用同一个 `request_id` 重试 |
| M2-4 | 打开 TLS 证书校验 | 调试用 `NX_INSECURE_TLS=1` 环境变量绕过 |

**验收标准 — 这是本里程碑最重要的量化指标**
- [ ] 11 轮标准 demo 对话，**LLM 调用次数 ≤ 2**（当前实现已是 1）
- [ ] 断网情况下游戏依然可以继续（降级 L1，不崩溃）
- [ ] AI 返回乱码 JSON 时不崩溃，降级到本地候选并记一条 audit
- [ ] 同一 `request_id` 重复提交不重复扣费

---

### M3 —— 持久化切换（建议 3~5 天）

**目标**：彻底消灭"读档错乱"。

| # | 任务 | 说明 |
|---|---|---|
| M3-1 | 执行 migrations 到现有 `save.db` | `001` → `002`，旧表保留一个版本再删 |
| M3-2 | `GameSession` 编排类 | 每次 `step()` 后：`append_event` → 每 20 步 `put_snapshot` → `audit_route` |
| M3-3 | 读档流程 = `restore()` + replay `events_after()` | **不要相信任何标量存档字段** |
| M3-4 | `nx_route_audit` 写入接上 | 这一步看着不起眼，但它是以后排查"某支线触达不到"的唯一线索 |

```cpp
// L1/GameSession.hpp —— 每步必须做的事，顺序不可变
nx::TurnResult GameSession::step(const std::string& input) {
    auto tr = engine_.step(input);                       // 1. 推进状态机
    store_.append_event(sid_, tr.seq, "turn",
                        to_json(tr), tr.from_node, tr.to_node);   // 2. 追加事件
    store_.sync_pending(sid_, engine_.state());          // 3. 同步挂起池
    store_.audit_route(sid_, tr.seq, tr.layer,
                       tr.to_node, tr.confidence, tr.reason);     // 4. 审计留痕
    if (tr.seq % 20 == 0) store_.put_snapshot(sid_, tr.seq, engine_.state());
    return tr;
}
```

**验收标准**
- [ ] 玩到第 37 步 → 强杀 → 重开，状态与强杀前完全一致（逐字段 diff）
- [ ] 手工在 sqlite 里插入一条 `ckpt_seq > time_cursor` 的快照 → 被拒绝
- [ ] 手工 `UPDATE nx_event` → 被触发器拒绝
- [ ] 连续 100 步无死锁、无约束冲突、无任何手工修复

---

### M4 —— 前端线程与体验（建议 3~4 天）

| # | 任务 | 说明 |
|---|---|---|
| M4-1 | `processInput()` 移出 GUI 线程 | `QtConcurrent::run` + `QFutureWatcher`，thinking 态 UI |
| M4-2 | 打字机效果期间禁止输入 | 防止 player 连点导致 seq 快速推进 |
| M4-3 | 结构化日志 + `trace_id` | 每个 session 一个 trace_id，串起 UI → LLM → DB |
| M4-4 | 敏感词库扩充，去掉误伤率高的单字 | 现有 `烦` 会误伤大量正常句 |
| M4-5 | 调试面板：显示当前 Frontier + 每个被拦截节点的 reason | 这是你要的"剧情断裂时直观看 its boundary"的能力，必须落到 UI 上 |

**验收标准**
- [ ] AI 思考期间拖动窗口不卡顿
- [ ] 一个 session 的所有日志可用 trace_id 一次性 grep 出来

---

### M5 —— 内容与工具（长期）

| # | 任务 |
|---|---|
| M5-1 | 剧本编辑器（可视化画节点、自动 lint） |
| M5-2 | 覆盖率回放工具：随机 walk 1000 次，统计每个节点的触达率 |
| M5-3 | 基于 `nx_route_audit` 的支线健康度报表 |
| M5-4 | 滚动摘要（`summary` 字段）质量优化 |
| M5-5 | 多结局成就 / 存档槽 UI |

---

## 五、任务优先级总表

| 阶段 | 任务数 | 前置依赖 | 风险 | 建议投入 |
|---|---|---|---|---|
| M0 止血 | 7 | 无 | 低（但价值极高） | 1~2 天 |
| M1 内核落地 | 5 | M0-1 | 中（内容工作量在剧本） | 5~8 天 |
| M2 AI 接回 | 4 | M1 | 中（API 不稳定 + token 成本） | 3~5 天 |
| M3 持久化 | 4 | M1 | **高**（数据迁移不可逆） | 3~5 天 |
| M4 体验 | 5 | M2 + M3 | 低 | 3~4 天 |
| M5 工具生态 | 5 | M4 | 低 | 持续 |

**关键路径**：`M0-1 → M1-2 → M2-1 → M3-2 → M4-1`
**最大单点风险**：M1-2（`intent.description` 全靠人工写），建议一开始就用小剧本试点，不要一上来转全量。

---

## 六、集成路线：五步接回现有工程

不要一次性替换。按下面的顺序，每一步工程都还能编译、还能运行。

**Step 1 · 先把内核放进去，不接线**
把 `engine/include/nx/` 加进现有 CMake 的 include 目录。此时它不参与任何逻辑，只用于跑 `engine/tests/test_core.cpp`。
```cmake
include_directories(${CMAKE_SOURCE_DIR}/engine/include ${CMAKE_SOURCE_DIR}/include)
add_executable(nx_test engine/tests/test_core.cpp)
```

**Step 2 · 单步替换 AI 调用**
把 `GameEngine::processInput()` 里直接调 `AiEngine` 的那一行，换成 `nx::IntentMatcher::decide()` + `CurlLlmSelector`。**其余逻辑一行不动。**
这一步做完，你应该立刻观察到 LLM 调用次数下降。

**Step 3 · 替换剧本加载**
`GameEngine` 内部改为持有 `nx::Catalog`（由 `StoryLoader::compile()` 产出）。启动时先跑 `lint()`，error 直接拒绝启动而不是运行时崩溃。

**Step 4 · 把状态所有权交给 `nx::GameState`**
逐步把散落在 `GameEngine` 里的 `affinity` / `currentNode` / tags 成员删掉，改为读写 `GameState`。这一步最容易引入回归，建议每删一个成员就跑一次双语一致性测试。

**Step 5 · 最后切换持久化**
状态所有权理清之后再动数据库。**顺序反过来会导致你在乱码状态上调试 IO。**

---

## 七、关键接口速查

```cpp
// 组装引擎
auto res = nx::StoryLoader::compile_file("data/story_v2.json");
if (!res.errors.empty()) { /* 拒绝启动 */ }
nx::NarrativeEngine engine(res.catalog, state);
engine.set_selector(&curl_selector);           // nullptr = 离线模式

// 每步
nx::TurnResult tr = engine.step(user_input);
// tr.layer        L0=点选项 / L1=本地判定 / L2=LLM / fallback=兜底
// tr.frontier_before.items   当前可用节点（含 score 分解，可直接打印调试）
// tr.frontier_before.blocked 被拦截节点 → reason（expired|phase|consumed|locked|cooldown）

// Frontier 打分：每一项可独立打印
//   bucket: ending(40) > scripted(30) > redemption(20) > emergent(10) > fallback(0)
//   score : base + urgency + staleness + recency + affinity + cooldown + pending_age
//   排序  : (bucket 降, score 降, node_id 升)   ← 第三关键字保证完全确定

// 持久化
store.append_event(sid, seq, "turn", payload, from, to);   // seq 必须 == cursor+1
store.put_snapshot(sid, seq, state);                        // ckpt_seq <= cursor
auto rp = store.restore(sid);                               // 快照 + 后续事件 → replay
store.begin_llm_call(request_id, sid, seq, prompt);         // 幂等登记
store.resolve_llm_call(request_id, response_json);
store.audit_route(sid, seq, layer, node, confidence, reason);
```

---

## 八、剧本作者约定（story_v2 schema 速查）

```jsonc
{
  "phases": ["evening", "night", "late_night"],      // 索引只增不减，物理时间单调
  "nodes": {
    "n_search": {
      "kind": "scripted",          // scripted|emergent|redemption|ending|fallback
      "once": true,                // 进入后永久消耗
      "priority": 60,              // 作者意图权重，可压过一切
      "cooldown": 5,
      "window": { "min": 1, "max": 12 },            // 存活窗口（time_cursor）
      "phase": { "allow": ["night"], "min_index": 1 },
      "requires": { "all": [ {"tag": "trust"}, {"affinity_gte": 70} ] },
      "intent": {
        "description": "搜房间翻出发夹，动摇玩家对妹妹的认知",   // ★必填，AI 选路唯一依据
        "keywords": ["搜", "房间", "抽屉"],
        "examples": ["我想看看她的房间", "床头柜里有什么"],
        "negatives": ["算了不看了"],
        "must_freeform": false
      },
      "presentation": { "narrator": "...", "npc_text": "...", "hint": "..." },
      "options": [ { "text": "继续翻", "next": "n_confront" } ],
      "default_next": "n_bridge",
      "effects": {
        "add_tags": ["discovered"], "remove_tags": [],
        "add_items": ["item_hairclip"],
        "affinity_delta": -15,
        "set_phase": "late_night",
        "summary_hint": "哥哥在妹妹房间里发现了陌生的发夹"    // ★动态重生成的"事实"
      },
      "redeemable": true,
      "redeem_gate": { "tag": "regret" },   // 没写 = 不可挽回（缺省不是"随便救"）
      "redeem_expire": 10
    }
  }
}
```

**三条作者纪律**
1. `intent.description` 必填。`lint()` 会拦。
2. 终末节点必须标 `terminal: true`，否则它会因属于 `ending` 桶而永远占据 Frontier 榜首。
3. 每个 phase 至少一个 `kind: "fallback"`，这是死锁的最后一道保险。

---

## 九、风险登记表

### 已发现并已修复（留存以备忘）

| # | 缺陷 | 怎么发现的 |
|---|---|---|
| R1 | 终末节点 `-1` 属 `ending` 桶 → 永远占 Frontier 榜首，开局直奔"游戏结束" | 仿真第 1 步输出即异常 |
| R2 | `redeem_gate` 缺省值语义错误 → 写了 `redeemable: true` 却永远救不回来 | 跨语言 trace 比对不一致 |
| R3 | 点选项按钮也走 LLM → 11 轮白烧 11 次请求（修后 1 次） | 双语比对 + 调用计数 |
| R4 | 现有 `AiEngine` 静默吞中文 | 离线复刻实测 |
| R10 | `EventStore::bind/step_done/finish` 缺 `const` → 带 libsqlite3 的环境直接编译不过 | 首次在 Linux 编译持久化层时 gcc 报错 |
| R11 | `step_done()` 把 `SQLITE_CONSTRAINT` 压平成 `SQLITE_DONE` → **G1/G2/G4 三道闸门在 C++ 侧形同虚设**（Python 侧却是拒绝的，两侧结论相反） | Linux 上 C++ 烟测 11/15，三道闸门"居然成功" |
| R12 | `prepare()` 登记进 `live_` 后调用点又手动 `sqlite3_finalize()` → 析构二次释放，**随机 segfault** | Linux 上重复运行出现 EXIT=139（时崩时不崩） |

> R10~R12 三个 bug 在 Windows 上一辈子也发现不了：Dev-Cpp 自带 MinGW **没有 sqlite3.h**，
> 持久化层从未被真正编译过。详见 §附录 C。

### 未决风险

| # | 风险 | 等级 | 缓解 |
|---|---|---|---|
| R5 | 挽回节点的台词与当前语境冲突 | **中** | 框架只能做三件事：阶段单调、事实/台词分离（`summary_hint`）、挽回到期。**剩下的靠作者文笔**，框架到此为止 |
| R6 | `intent.description` 质量决定 AI 选路准确率 | 高 | 建立"描述模板"+回放工具量化准确率，低于阈值就返工改描述 |
| R7 | LLM 返回不稳定导致 route 抖动 | 中 | `confidence` 低于阈值降级本地；所有决策落 `nx_route_audit` 便于回放分析 |
| R8 | 剧本规模增长后 authoring 成本失控 | 中 | M5-1 编辑器 + M5-2 触达率回放 |
| R9 | SQLite 迁移不可逆 | 中 | M3-1 执行前先整库备份；迁移脚本自带版本号校验 |

---

## 十、测试与 CI 策略

| 层 | 工具 | 必过的门槛 |
|---|---|---|
| 内核 | `engine/tests/test_core.cpp`（g++ / MinGW 实测零 warning） | 断言全通过 + 与 Python 轨迹逐行一致 |
| 参考实现 | `reference/simulate.py` | 24/24 |
| 持久化 | `persistence/tests/test_store.py`（真实 SQLite） | 17/17 |
| 剧本 | `StoryLoader::lint()` | 0 error |
| 集成 | 手工回归清单（见各 M 的验收标准） | 全勾选 |

**为什么保留 Python 参考实现**：它不是冗余代码，而是**独立的第二实现**。两套不同语言、不同作者在同样输入下产出一致结果，这是当前能对引擎正确性给出的最强证据。任何时候改动 C++ 内核后，重跑一次双语比对，能在 30 秒内发现排序/by-value/by-reference 级别的错误。

```bash
# CI 三件套（建议做成一条命令）
python reference/simulate.py                       # 期望 24/24
python persistence/tests/test_store.py             # 期望 17/17
python reference/emit_trace.py > /tmp/exp.txt && \
  g++ -std=c++14 -Wall -Wextra -I engine/include -I include \
      engine/tests/test_core.cpp -o /tmp/nx_test && \
  diff <(tr -d '\r' </tmp/exp.txt) <(/tmp/nx_test | tr -d '\r')   # 期望无输出
```

---

## 十一、完成的定义（DoD）

一个任务只有在以下全部满足才算完成：

1. 代码编译**零警告**；
2. 双语一致性比对通过（如涉及内核逻辑）；
3. 涉及的判定表 / 门槛值有对应断言；
4. 剧本侧 `lint()` 无 error；
5. 在文档里留下了"怎么验证我改对了"的方法。

---

## 十二、下一步立即可以做的一件事

跑一遍验收流程，确认地基是稳的：

```bash
cd fs
python reference/simulate.py            # 24/24
python persistence/tests/test_store.py  # 17/17
```

然后打开 `docs/FEASIBILITY_REVIEW.md` §4.1，照着把 `AiEngine.cpp` 的清洗逻辑改掉——**这是唯一一个改 10 行就能让整个项目从"看起来能跑"变成"真的能跑"的地方**。

---

## 附录 C：Linux 交叉验证（2026-10-30）

### 为什么要上 Linux 跑一遍

Windows 侧的开发环境是 Dev-Cpp 自带的 MinGW，它有两个硬伤：

1. 最高只到 `-std=gnu++14`，**没有 C++17**；
2. **不提供 sqlite3 开发库** —— 这意味着 `persistence/include/nx/EventStore.hpp`
   在写完之后从未被编译过，它的正确性只靠"读代码"和 Python 侧的等价测试背书。

第 2 点的风险极高，因为"看起来对的 C++ 代码"和"能编译且行为正确的 C++ 代码"是两回事。

### 结果

在 VMware 里的 Ubuntu 22.04（GCC 11.4 + libsqlite3）上首次编译并运行，立刻抓到 **3 个真 bug**（R10/R11/R12，见 §九），其中最严重的一个会让**四道闸门里的三道彻底失效**——G1 重复 seq、G2 游标跳号、G4 超前快照全部静默通过。这正是"读档不错乱"的全部保障，失效意味着之前那套设计实际上没有落地。

### 怎么复现

```bash
# Windows 侧：连通 VM 后同步代码
scp -F D:/SSH_config/conifg nx-linux.tar.gz ubuntu:/home/cyc/

# Linux 侧：一条命令跑完四层验证
cd ~/nx-linux && bash verify_all.sh
```

四层验证各自的意义：

| 层 | 说明 |
|---|---|
| C++ 内核 × C++17/C++14 | 双标准编译零警告，且**与 Windows MinGW 的输出逐行一致**（跨 OS 确定性） |
| EventStore C++ 烟测 | 唯一能在真实 sqlite3 上验证四道闸门的地方；另用 ASan+UBSan+LeakSan 复跑，干净 |
| Python `simulate.py` | 24/24，与 Windows 一致 |
| Python `test_store.py` | 17/17，与 C++ 烟测形成互证（两语言、同一份 SQL） |

### 这一层的三个意外收获

1. **`step_done()` 的返回值不能压平。** 返回原始 `sqlite3_step()` 结果码，让调用方自己判断
   `== SQLITE_DONE`。任何"为了方便"的封装都可能吞掉约束冲突。
2. **资源所有权必须单一。** `prepare()` 登记进 `live_`，调用点就只能调 `finish()`，
   不能自己 `sqlite3_finalize()`。二次释放表现为随机崩溃，是最难查的一类 UB。
3. **const 正确性会在最意想不到的地方咬人。** 只读语义的方法（`time_cursor`/`restore`/`events_after`）
   一旦被声明 `const`，它调用的所有辅助函数都必须跟着 `const`，否则换个编译器就编译不过。
