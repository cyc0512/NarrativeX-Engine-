# NarrativeX Engine (FluidStory 驱动引擎)

**NarrativeX** 是一款基于 C++17 与 Qt5 开发的开源文字冒险引擎，旨在为全新的游戏品类 **FluidStory (流态小说)** 提供底层技术支持。

> ⚠️ **【版权与原创声明】**
> **FluidStory (流态小说)** 这一游戏品类概念，以及 **NarrativeX Engine** 的底层架构（包括但不限于：动态状态注入机制、大模型意图分发与主线收束机制），均由本项目作者独立提出并首次实现。
> 本仓库代码基于开源协议发布，欢迎学习与交流。但严禁直接将他人的原创概念与架构思想据为己有进行商业宣传。
---

## 📌 新文档入口（2026-10-30 新增）

本仓库新增了「动态迷宫」内核与配套验证工程，与原有 Qt 工程并存，可直接编译、离线运行：

| 文档 | 内容 |
|---|---|
| [`docs/FEASIBILITY_REVIEW.md`](docs/FEASIBILITY_REVIEW.md) | 三大支柱逐条可行性 + 现有代码缺陷清单（含 P0 致命 bug 的实测复刻） |
| [`docs/PROJECT_PLAN.md`](docs/PROJECT_PLAN.md) | 项目计划书：M0~M5 里程碑、任务拆解、验收标准、五步集成路线 |

| 目录 | 内容 |
|---|---|
| `engine/include/nx/` | 零依赖 C++14 内核：Frontier 排序、PendingPool 挽回、三级意图漏斗、剧本加载与 lint |
| `persistence/` | 事件溯源 + 快照持久化，四道闸门全部下沉到 SQL 触发器 |
| `reference/` | Python 等价参考实现（`reference/nxref.py`）+ 24 项仿真（`reference/simulate.py`） |

**验证方式（一条命令）**
```bash
python reference/simulate.py              # 期望 24/24
python persistence/tests/test_store.py    # 期望 17/17
g++ -std=c++14 -Wall -Wextra -I engine/include -I include \
    engine/tests/test_core.cpp -o nx_test && ./nx_test   # 与 Python 轨迹逐行一致
```

> ⚠️ 阅读新文档前请先处理 `docs/FEASIBILITY_REVIEW.md` §4.1 的 P0 bug，它会导致 AI 中文台词被静默清空。

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
4. **拓扑锁防御 (Topology Lock)**：不管 AI 判定去了哪里，只要目标节点不在当前节点的合法后继（`options`/`default_next`）中，引擎将**强制拦截**，判定为“原地闲聊”，彻底封死 AI 的幻觉乱跳，保护基础时间线不崩坏。

## 🕹️ 附带FluidStory游戏实例：

本仓库包含了一款名为 **《FluidStory》** 的示例游戏。您可以通过运行本项目，直接体验这种结合了自然语言输入的新型交互模式，并参考其配置方式，快速构建您自己的 AI 文字游戏。

## 🛠️ 技术栈 (Tech Stack)

- **核心语言**: C++17
- **界面框架**: Qt5 (GUI 渲染与多线程异步通信)
- **网络通信**: libcurl (调用大模型 API)
- **数据库**: SQLite3 (存档、状态持久化与自动迁移)
- **数据格式**: nlohmann/json (剧本与状态序列化)

## 🛠️ 引擎使用指南 (User Guide)

使用 **NarrativeX** 开发游戏，你不需要修改任何一行 C++ 代码。所有的剧情走向、人设与分支，都由 `data/` 目录下的两个 JSON 文件驱动。

### 1. 配置 AI 人设与行为约束 (`data/prompts.json`)

剧本不再依赖死板的全图流转，而是声明式拓扑。开发者只需定义节点的 `options`（可见选项）和 `intent`（隐藏意图）：
**示例：**
```json
"n1_entry": {
  "options": [ { "text": "解释：这是客户。", "next": "n2_kitchen" } ],
  "default_next": "n3_shut",
  "intent": {
    "keywords": ["客户", "工作", "累"],
    "examples": ["这是公司的客户，别瞎想", "我很累，晚点说"]
  }
}
```
## 🚀 快速开始 (Quick Start)

### 环境依赖
```bash
sudo apt install qtbase5-dev libqt5concurrent5 libsqlite3-dev libcurl4-openssl-dev
```

### 构建与运行

> 目录名与 CMake target 已统一为 **FS**：终端版 `fs_terminal`，Qt GUI 版 `fs`。
> 程序内的数据路径（`../data/story.json`、`../assets/bg.jpg`）是相对路径，
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