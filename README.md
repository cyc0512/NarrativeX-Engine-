# NarrativeX Engine (FluidStory 驱动引擎)

**NarrativeX** 是一款基于 C++17 与 Qt5 开发的开源文字冒险引擎，旨在为全新的游戏品类 **FluidStory (流态小说)** 提供底层技术支持。

> ⚠️ **【版权与原创声明】**
> **FluidStory (流态小说)** 这一游戏品类概念，以及 **NarrativeX Engine** 的底层架构（包括但不限于：动态拓扑锁机制、双指针调度、场景化情绪标签注入机制），均由本项目作者独立提出并首次实现。
> 本仓库代码基于 MIT 协议发布，欢迎学习与交流。但严禁直接将他人的原创概念与架构思想据为己有进行商业宣传。
---

## 📌 文档入口

| 文档 | 定位 |
|---|---|
| [`docs/ARCH_BLUEPRINT.md`](docs/ARCH_BLUEPRINT.md) | **现行架构蓝图**：实际编译链路、活/悬/重/尸四色盘点、待接线能力清单、分期整理路线 |
| [`docs/CRASH_ROOT_CAUSE.md`](docs/CRASH_ROOT_CAUSE.md) | 自由输入闪退（D-4 / D-7）根因取证 |
| `docs/ARCH_FS.md`、`docs/PRD_FS_SCRIPT.md`、`docs/PROJECT_PLAN.md`、`docs/FEASIBILITY_REVIEW.md` | **历史设计稿**（2026-10-30），主语多为未落地的 `GameSession` / `MainWindow` / `FS_ENGINE`，其中的 Python 验证章节已失效 |

> 本项目代码源为 **C++**，早期因尚无 C++ 调用链而临时搭建的 Python 参考实现与验证脚本已于 2026-11-10 整体移除。
> 未接线但具设计价值的能力（`EventStore` 时间游标、`late_recover` 后向重归指针、动态标签前置条件等）**一律保留**，详见蓝图「待接线能力清单」。

| 目录 | 内容 |
|---|---|
| `engine/include/nx/` | 零依赖 C++17 内核（兼容 C++14）：Frontier 排序、PendingPool 挽回、三级意图漏斗、剧本加载与 lint |
| `persistence/` | 事件溯源 + 快照持久化，四道闸门全部下沉到 SQL 触发器 |
| `src/` | 生产入口与 `EngineAdapter` 编排层 |
| `data/` | 剧本 `story_v2.json`（11 节点）、情绪标签 `tags.json`（14 项） |

**验证方式（纯 C++，一条命令）**
```bash
cd ~/fs && bash persistence/tests/verify_all.sh
```
该脚本做两件事：nx 内核运行期回归（C++17 / C++14 双标准零警告编译 + 32 条断言真实执行），
以及 `EventStore` 真实 SQLite 烟测（15/15，含四道闸门）。
2026-11-10 阶段 D 后实测：通过 7 项 / 失败 0 项 / **跳过 0 项**，`EXIT=0`；编译产物只写 `/tmp`，不在仓库留 ELF。

> `engine/tests/test_core.cpp` 已重建为纯 C++，直接读当前生效剧本 `data/story_v2.json`，
> 覆盖三大支柱（时间游标 / Frontier / PendingPool）、三级漏斗、1200 token 硬上限、状态往返与结局可达。
> `late_recover` 等保护对象只验「内核能解析」，行为验证归阶段 C，用例会打印 `[待接线]` 明确标注，不假装测过。
> 另有一条 `[已知限制]`：`n3_shut.redeem_gate.tag` 用中文「后悔道歉」，而 `effects.add_tags` 全为英文 tag，
> 门禁永不满足；属内容侧词条定义，本测试不改剧本、不删断言。

---

### 💡 缘起

目前市面上的文字游戏（Galgame/Visual Novel）大多停留在“静态分支树”时代；即便引入了 AI，AI 也往往只是一个与主线割裂的“陪聊挂件”。

我们提出并实践了全新的游戏理念——**FluidStory (流态小说)**：

### 💡 品类定义：什么是 FluidStory？

FluidStory（流态小说） 并非传统视觉小说的 AI 套壳，而是一种基于 “半涌现叙事（Semi-Emergent Narrative）” 的全新文字交互门类。它通过以下四大底层机制，彻底重塑了数字叙事体验：

1. **单向时间线约束**：故事像河流一样奔涌向前，绝不生硬倒流。哪怕触发了过往的事件，引擎也会结合当下的时间语境，重新生成符合逻辑的对话，彻底消除“时间悖论”。
2. **动态拓扑网络 (Dynamic Topology)**：打破僵化的单向树状分支。剧情节点不再受限于固定顺序，而是依据玩家的隐藏状态，在时间线上动态涌现、汇合或重构。
3. **事件挽回机制 (Event Redemption)**：打破“错过即永久”的线性遗憾。通过底层的状态捕获，系统能动态唤醒过去挂起的未完成事件，实现剧情逻辑的闭环。
4. **隐式情绪域博弈 (Implicit Emotional Domain)**：剥离所有可视化的数值面板。玩家只能通过 NPC 微妙的语气、标点与神态描写来“察言观色”，在心理试探中走向无法挽回的宿命结局。

### 💡 引擎核心业务能力：赋能开发者构建 FluidStory

NarrativeX 采用 **“意图分级漏斗 + 拓扑锁”** 的双引擎驱动：

1. **L0 精确强控 (0 Token)**：通过剧本预设的 `keywords`，瞬间匹配关键动作，直接跳转固定节点，保证响应速度与剧情下限。
2. **L1 本地相似度 (0 Token)**：利用 Jaccard 算法与剧本 `examples` 进行本地语义相似度打分，命中隐藏支线则直接跳转。
3. **L2 受限大模型 (Top-K 选择)**：当前两层无法确定时，将**当前节点允许连接的短名单**交由大模型，大模型只做“单选题”。
4. **拓扑锁防御 (Topology Lock)**：当 L1 本地相似度 / L2 大模型 / fallback 给出的目标节点不在当前节点的合法后继（`options` / `default_next` / `fallback_next`）中时，回退到原节点（`layer=L2_local`），并照常生成一段 AI 台词。`Dynamic_Pull`（由 `early_trigger` 前置条件触发的动态拉取）是明确的例外放行通道。实现见 `src/EngineAdapter.cpp:431-461`。

## 🕹️ 附带FluidStory游戏实例：

本仓库包含了一款名为 **《FluidStory》** 的示例游戏。您可以通过运行本项目，直接体验这种结合了自然语言输入的新型交互模式，并参考其配置方式，快速构建您自己的 AI 文字游戏。

## 🛠️ 技术栈 (Tech Stack)

- **核心语言**: C++17
- **界面框架**: Qt5 (GUI 渲染与多线程异步通信)
- **网络通信**: libcurl (调用大模型 API)
- **数据库**: SQLite3 (存档、状态持久化与自动迁移)
- **数据格式**: nlohmann/json (剧本与状态序列化)

## 🛠️ 引擎使用指南 (User Guide)

使用 **NarrativeX** 开发游戏，你不需要修改任何一行 C++ 代码。剧情走向、分支与情绪由
`data/story_v2.json`（11 节点，声明式拓扑）与 `data/tags.json`（14 个情绪标签）驱动。

> 注：`data/prompts.json` 是早期人设模板，当前生产链路未读取，作为待接线能力保留。

### 1. 声明式节点（`data/story_v2.json`）

开发者只需定义节点的 `options`（可见选项）、`default_next` / `fallback_next` 和 `intent`（隐藏意图）。
下面摘自当前剧本的 `n1_entry`（结构真实，台词已省略）：
```json
"n1_entry": {
  "kind": "scripted",
  "once": true,
  "window": { "min_step": 0, "max_step": 2 },
  "phase":  { "allow": ["evening", "night", "late_night"] },
  "presentation": { "narrator": "...", "npc_text": "...", "npc_state": "...", "hint": "..." },
  "options": [
    { "text": "解释：这是我们公司安排要见的客户。", "next": "n2_kitchen" },
    { "text": "回避：我很累，晚一点和你解释。", "next": "n3_shut" }
  ],
  "default_next": "n3_shut",
  "fallback_next": "n_idle",
  "effects": { "add_tags": ["suspicious"] },
  "intent": { "keywords": [], "examples": ["..."] }
}
```
节点还支持 `requires` 条件 DSL、`early_trigger` 前置条件拉取（`n_hidden_breakdown`）、
`effects.add_tags/remove_tags` 动态标签增减等，字段全集见 `engine/include/nx/StoryLoader.hpp:250-355`。
## 🚀 快速开始 (Quick Start)

### 环境依赖
```bash
sudo apt install qtbase5-dev libqt5concurrent5 libsqlite3-dev libcurl4-openssl-dev
```

### 构建与运行

> 目录名与 CMake target 已统一为 **FS**：终端版 `fs_terminal`，Qt GUI 版 `fs`。
> 程序内的数据路径（`../data/story_v2.json`、`../data/tags.json`、`../assets/bg.jpg`）是相对路径，
> 所以**必须把工作目录切到 `build/`**（或项目根目录）再运行。

```bash
cd fs
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build -j 2

# 终端版
cd build && ./fs_terminal

# Qt GUI 版（无 X 服务时可加 QT_QPA_PLATFORM=offscreen）
cd build && ./fs
```

> ⚠️ 目录改名后 **必须删掉 `build/` 重新 cmake**：`build/CMakeCache.txt` 里记录了绝对源码目录，
> 直接复用会报 `CMakeCache.txt ... is different than the directory ...`。