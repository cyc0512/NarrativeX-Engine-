# FluidStory 剧本 Schema v4 —— 节点三分类与触发/锚点模型

> 本文档是 v4 框架的契约说明。v4 与 v3（`story_v2.json` 形态）**additive 兼容**：
> 旧剧本不写任何新字段即可原样加载，新字段缺省值保证行为不变。
> 验证夹具见 `data/story_v4_example.json`，回归用例见 `engine/tests/test_v4.cpp`。

---

## 1. 设计动机

v3 的 `early_trigger` / `late_recover` 虽然能"全局触发"隐藏节点，但作者很难控制
**时间线先后**与**跨时间线调用的边界**，导致剧情容易乱、调试困难。v4 把节点显式分为三类，
让"前后关系"和"触发条件"一目了然，并引入**物品标签**与**背包界面**。

---

## 2. 三种节点类型（`type` 字段）

`type` 与既有 `kind`（演出形态 / 优先级桶）**正交**。缺省为 `fixed`。

| type | 含义 | 时间线行为 | 触发方式 |
|------|------|-----------|----------|
| `fixed` | 固定支架，确保剧情下限，**绝不被移动** | 线性推进，必填 `timeline>0` | 由 `options` / `default_next` 的相邻边驱动 |
| `dynamic` | 动态节点，需越过锚点才"武装" | 初始化于 `init_after`，之后才能触发（**天然禁止回退**） | 武装后还需满足 `trigger` 二次闸门 |
| `preposable` | 可前置节点（从动态分化的稀缺类型） | 仅当 `current_node` 命中 `insert_at` 锚点才可插入 | 命中锚点后还需满足 `trigger` 二次闸门 |

### 2.1 时间线（`timeline`）
- 整数序号，代表该节点在主线中的先后顺序。
- 固定节点**必填且 >0**；动态 / 可前置节点缺省为 `0`（其推进由锚点与触发驱动）。
- `timeline` 用于作者自审顺序，并被 lint 的**禁止回退**校验引用。

### 2.2 动态节点的初始化锚点（`init_after`）
- 一个节点 id。玩家**到过（visited）**该节点之后，本动态节点才算"武装"。
- 未武装时，本节点根本不会进入候选集（在 `Frontier::gate` 中返回 `Unarmed`）。
- 由于 `seq` 单调、且武装判定基于"是否到过锚点"，动态节点**无法被后向分支提前拉回更早的时间线** —— 这正是"禁止回退"的硬保证。
- `init_after` 为空 ⇒ 一开始就武装（兼容"全局触发"的旧语义，但作者应明确写出锚点）。

### 2.3 可前置节点的插入锚点（`insert_at`）
- 字符串数组，列出**允许插入本节点的当前节点**（显式锚点）。
- 仅当 `st.current_node` 命中其一，本节点才进入候选集（`gate` 返回 `GroupLocked` 表示未命中锚点）。
- 锚点把"可跨时间线调用的剧情"严格收敛到作者点名的少数位置，既减少逻辑出错，也避免剧情乱。

---

## 3. 触发闸门（`trigger` 字段）

`trigger` 是武装 / 命中锚点**之后**的二次闸门，结构复用 `TriggerCondition`：

```json
"trigger": {
  "tags_all": ["numbness", "heartbroken"],   // 须同时拥有的情绪/状态标签
  "tags_any": ["lonely"],                    // 至少拥有其一
  "min_step": 0,                             // 时间游标门槛（seq >= min_step）
  "require_node": "n_boss_call",             // 须到过的节点
  "required_phase": "night"                  // 须处于的物理阶段
}
```

- 空 `trigger`（全默认值）视为**恒真**。
- `trigger` 与既有 `requires`（Condition DSL）**正交**：
  - `requires` 决定节点能否作为"相邻边候选"出现（进入 `candidate_ids` 的 tag/item 倒排）；
  - `trigger` 只在节点已武装 / 已命中锚点后，由 `Frontier::gate` 单独求值，**不污染候选倒排**。
- `trigger` 同时承担"AI 语义触发"的确定性侧：节点的自由输入意图仍由既有 `intent` + `IntentMatcher`（L2）判定，
  而 `trigger` 决定"该节点此刻是否够格进入候选池供 AI 路由"。

### 3.1 AI 语义触发范围
按规划拍板：**仅 `dynamic` / `preposable` 节点**使用自由输入的 AI 语义路由（L2）。
固定节点以确定性选项（L0）为主，不依赖 AI 判意。

---

## 4. 物品标签与背包

- 既有 `GameState.items`（`std::unordered_set<ItemId>`）已存在，无需新增字段。
- 节点通过 `effects.add_items: ["key"]` / `remove_items` 发放 / 收回道具。
- 物品同样可作为 `requires` / `trigger` 的判定依据（`requires: {"item":"key"}`、
  `trigger: {"tags_any":["lonely"]}` 等）。
- 背包界面（UI 层）为游戏页新增按钮 + 开局展示一次，由 `EngineAdapter` 适配层负责，
  内核侧 `items` 已随存档序列化（`state_to_json` / `state_from_json` 已含 `items`）。

---

## 5. 内核改动清单（v4 框架）

| 文件 | 改动 |
|------|------|
| `NodeModel.hpp` | 新增 `enum class NodeType{Fixed,Dynamic,Preposable}` 与 `node_type_name()`；`NodeSpec` 增 `ntype/timeline/init_after/insert_at/trigger`；新增自由函数 `trigger_satisfied()`；移除死的 `check_trigger` 声明 |
| `StoryLoader.hpp` | `compile_node` 解析 v4 字段（additive）；新增 `parse_type` / `parse_trigger`；`lint()` 增加：动态 `init_after` 必存在、可前置 `insert_at` 必存在且非空、动态/可前置**禁止指向更早时间线**（禁止回退） |
| `Frontier.hpp` | `BlockReason` 增 `Unarmed` / `Trigger`；`gate()` 增加动态（武装 + `trigger`）与可前置（锚点 + `trigger`）闸门；`refresh()` 召回已武装动态节点与命中锚点可前置节点入候选集 |

---

## 6. lint 不变量（fail-fast）

1. 动态节点的 `init_after`（若填写）必须存在。
2. 可前置节点必须有非空 `insert_at`，且每个锚点必须存在。
3. 任何节点（含固定骨架）的 `default_next` 与 `options.next` **不得指向时间线更早的非结局节点**（禁止回退）。
   固定骨架是静态链 / 二叉链（不动）；动态 / 可前置节点挂在其分支上可**自由向前分叉**，
   但整图（骨架 + 挂载）一律不得倒流回更早时间线——这是"剧情不紊乱"的硬保证。

---

## 7. 示例骨架（`data/story_v4_example.json`）

演示了：固定主线（n1→n2→n_boss_call→…→endings）、一个 `dynamic` 坏结局
（`init_after: n_boss_call`，`trigger.tags_all:[numbness,heartbroken]`）、一个 `preposable`
旧信节点（`insert_at:[n4_silence,n_client_meal]`，`trigger.tags_any:[lonely]`）、物品 `key`
（`n_client_meal` 发放）、以及 fallback。**具体剧情文本由作者在此基础上扩写。**
