# NarrativeX Engine（FluidStory 流态小说驱动引擎）

**NarrativeX** 是一款基于 C++17 / Qt5 的开源叙事引擎，为一种新的游戏品类 **FluidStory（流态小说）** 提供底层支持：让角色的台词由 AI 实时扮演，让剧情分支由玩家的自由输入驱动，同时把主线与结局牢牢握在作者手里。

> ⚠️ **【版权与原创声明】**
> **FluidStory（流态小说）** 品类概念，以及 **NarrativeX Engine** 的底层机制（意图分级漏斗、动态节点调度、场景化情绪标签注入、按角色隔离的 prompt 分库），均由本项目作者独立提出并首次实现。
> 本仓库代码基于 MIT 协议发布，欢迎学习与交流；但严禁将他人的原创概念与架构思想据为己有进行商业宣传。

---

## 为什么做 FluidStory：定位与卖点

市面上的视觉小说陷入了**"滥而多"**的瓶颈：底层几乎都是静态分支树，每多一个结局就要手写一整条线，于是产品靠堆量取胜，体验却高度同质化。而完全开放的 AI 角色扮演虽然自由，却普遍失忆、人设崩塌、时间线错乱，玩家无从体会"故事"的重量。

FluidStory 选择了一条**刻意保守的路——半开放叙事**：

- 它比全开放 AI 更克制：**作者写定骨架、时间线与结局**，AI 只在骨架内扮演角色、丰满台词。
- 它比静态分支树更自由：**玩家的自由输入是一等公民**，一句话可以触发隐藏节点、改变情绪、走向不同结局。

我们不做"更自由的聊天"，而是把体验感押在一件事上——**宿命感**：玩家反复探索，只为回答"如果当时那样说，会不会是另一种结局"。

为兑现这一定位，引擎提供两大可配置能力：

1. **让角色活过来**：每个 NPC 拥有独立 prompt 库（人设 / 视角 / 专属情绪词条 / 对话样例 / 违规判定）。AI 扮演者以第一人称说出符合人设、紧扣玩家输入、带情绪但内敛的台词——而不是镜像复读或套壳陪聊。
2. **让分支活起来**：剧情节点分 **Fixed / Dynamic / Preposable** 三类；玩家的自由输入经意图漏斗判定后，可触发动态节点与结局分支线。输入即"改写命运的动作"，而非在选项里三选一。

**给后来者的新思路**：把"台词有生命"和"分支会流动"做成引擎能力，创作者不再手写穷举每句话每条线，而是声明节点、意图与角色库，把演绎交给引擎。

---

## 品类定义：什么是 FluidStory？

FluidStory（流态小说）不是传统视觉小说的 AI 套壳，而是一种基于**半涌现叙事（Semi-Emergent Narrative）**的文字交互门类。它的四大机制：

1. **单向时间线约束**：故事向前奔涌，不生硬倒流；动态节点按触发锚点的时间线插入，消除"时间悖论"。
2. **三分类动态节点**：Fixed（固定）/ Dynamic（动态触发）/ Preposable（可前置），配合 `Frontier` 闸门与意图白名单，决定哪些节点此刻合法可达——合法性由闸门与白名单保证，不依赖旧式拓扑锁。
3. **隐式情绪域博弈**：剥离数值面板，玩家只能通过 NPC 的语气、标点与神态"察言观色"；情绪标签只做语气微调，绝不覆盖人设。
4. **多结局宿命收束**：违规累积、关键标签与自由输入共同决定走向哪个结局，失败亦是一种叙事结果。

## 引擎核心业务能力：意图分级漏斗

玩家的自由输入如何变成剧情推进？引擎用三级漏斗逐级判定，逐级省 token：

1. **L0 精确强控（0 token）**：关键词直接匹配，瞬间跳转固定节点；空输入走内部门禁，不进漏斗、不发 LLM。
2. **L1 本地相似度（0 token）**：Jaccard / bigram 与剧本 `examples` 打分，命中隐藏支线直接跳转。
3. **L2 受限大模型（白名单选择）**：前两层无法确定时，把当前合法后继的短名单交给大模型做"单选题"；**模型可弃权（abstain）**，弃权则原地停留，不强行降级到 Top1。
4. **违规守卫（独立）**：判定玩家输入是否偏离其扮演身份（如明知故问"你是谁"），只对木子因线累积坏结局计数，对其它 NPC 只落各自命名空间。

## 角色 prompt 分库

每个 NPC 的 prompt 独立成库，发给 AI 时按说话人只装载对应那份：

```
data/profiles/
  muziyin.json   # 木子因（对象：哥哥）
  linruxi.json   # 林如夕（对象：前辈）
  client.json    # 客户（对象：玩家）
  README.md      # 字段说明与新增角色步骤
```

每库含 persona、视角契约、专属情绪词条、对话样例、违规基准。这样木子因的内敛口吻不会污染林如夕，新增角色只需加一个 JSON 文件。库缺失时自动回退旧 `prompts.json` / `tags.json` / `fewshot.json`。

## 🕹️ 示例游戏

本仓库附带一款名为 **《FluidStory》** 的示例游戏，可直接运行体验"自然语言输入 + 动态节点 + 多结局"的交互，并参考其 `data/` 配置构建自己的 AI 文字游戏。

## 技术栈

- **核心语言**：C++17（内核兼容 C++14）
- **界面框架**：Qt5（Widgets）
- **网络通信**：libcurl（调用大模型 API）
- **数据库**：SQLite3（存档、状态持久化、事件溯源）
- **数据格式**：nlohmann/json（剧本、角色库与状态序列化）

---

## 引擎使用指南

用 NarrativeX 开发游戏**无需修改 C++**：剧情、意图与角色由数据驱动。

| 文件 | 作用 |
|---|---|
| `data/story_v1_home.json` | 当前生效剧本（21 节点，入口 `n1_entry`），声明式节点 + 意图 |
| `data/story_v2.json` | 回归基准剧本 |
| `data/tags.json` | 16 个情绪标签（作者定义词条，代码只做算法） |
| `data/fewshot.json` | 木子因口吻的 ICL 对话样例（兼容回退用） |
| `data/profiles/*.json` | 各 NPC 独立 prompt 库（优先读取） |

声明式节点示例（结构真实，台词省略）：

```json
"n1_entry": {
  "type": "fixed",
  "kind": "scene",
  "timeline": 1,
  "presentation": { "narrator": "...", "npc_text": "..." },
  "intent": { "description": "前情提要：……" },
  "options": [ { "text": "（睁开眼）", "next": "n_wake" } ]
}
```

节点通过 `type`（fixed / dynamic / preposable）、`timeline`、`trigger` / 锚点、`effects.add_tags` 等字段描述；完整契约见 `docs/STORY_SCHEMA_v4.md`。

## 文档入口

| 文档 | 定位 |
|---|---|
| [`docs/ARCH_BLUEPRINT.md`](docs/ARCH_BLUEPRINT.md) | 架构蓝图：编译链路、代码盘点、待接线能力清单、整理路线 |
| [`docs/STORY_SCHEMA_v4.md`](docs/STORY_SCHEMA_v4.md) | v4 剧本节点字段契约 |
| [`docs/Vision_and_Design.md`](docs/Vision_and_Design.md) | 品类愿景与设计 |
| [`docs/CRASH_ROOT_CAUSE.md`](docs/CRASH_ROOT_CAUSE.md) | 自由输入闪退根因取证 |
| `docs/ARCH_FS.md`、`docs/PRD_FS_SCRIPT.md`、`docs/PROJECT_PLAN.md`、`docs/FEASIBILITY_REVIEW.md` | 早期设计稿（2026-10-30），部分描述为已废弃的历史机制，仅作沿革参考 |

> 代码源为 **C++**。早期无 C++ 调用链时临时搭建的 Python 参考实现与验证脚本已于 2026-11-10 整体移除。
> 未接线但具设计价值的能力（`EventStore` 时间游标等）一律保留，详见蓝图。

## 验证方式（纯 C++，一条命令）

```bash
cd ~/fs && bash persistence/tests/verify_all.sh
```

覆盖内核运行期断言（C++17 / C++14 双标准零警告编译）、`EventStore` 真实 SQLite 烟测、标签按存档持久化。当前实测：**通过 11 项 / 失败 0 项 / 跳过 0 项，`EXIT=0`**；编译产物只写 `/tmp`，不在仓库留 ELF。

## 🚀 快速开始

### 环境依赖

```bash
sudo apt install qtbase5-dev libqt5concurrent5 libsqlite3-dev libcurl4-openssl-dev
```

### 构建与运行

> CMake target：`fs_terminal`（终端版）/ `fs`（Qt GUI 版）。
> 数据路径（`../data/story_v1_home.json` 等）是相对路径，**须切到 `build/` 再运行**。

```bash
cd fs
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build -j 2

# 终端版
cd build && ./fs_terminal

# Qt GUI 版（无 X 服务时可加 QT_QPA_PLATFORM=offscreen）
cd build && ./fs
```

> ⚠️ 目录改名后须删除 `build/` 重新 cmake：`build/CMakeCache.txt` 记录绝对源码目录，直接复用会报错。

### 配置大模型

在 `config.json` 填入 `api_key`（智谱 GLM）。示例游戏的用户台词、情绪判定与角色扮演均通过该网关调用。
