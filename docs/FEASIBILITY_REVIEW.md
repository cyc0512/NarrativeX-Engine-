# NarrativeX / FluidStory —— 动态迷宫架构可行性评审

> 评审对象：`fs`（现有 C++17 + Qt5 + SQLite3 + libcurl 工程；目录已由 `FluidStory` 统一改名为 `fs`）
> 评审时间：2026-10-30
> 评审方式：通读全部源码 + **实际运行验证**（23 项 Python 仿真 + 17 项 SQLite 约束测试 + C++ 内核编译与跨语言一致性比对）

---

## 〇、一句话结论

**你的三条底层支柱在技术上全部成立，可以直接做；但现有代码里有一个 P0 级致命 bug，在它被修掉之前，任何上层功能都无法正常演示。**

| 结论 | 说明 |
|---|---|
| ✅ **可行** | 时间游标、动态拓扑、事件挽回三套机制的算法已在本机实现并完成端到端验证（24/24 项通过） |
| ⚠️ **非瓶颈** | 你担心的「Frontier 重构算法性能」实测**不是问题**（见 §2.2，比 LLM 往返快 3 个数量级） |
| ✅ **已解决** | 空缺 2（状态栈持久化）已有完整方案 + 可运行 SQL，四道闸门均已实测生效 |
| 🔴 **必须先修** | 现有 `AiEngine.cpp` 的清洗逻辑会**静默吞掉所有中文台词**（见 §4.1），这是当前版本的致死伤 |

---

## 一、三大支柱逐条可行性

### 支柱一：时间游标（Time Cursor & State Phase）—— ✅ 完全可行，成本极低

**设计的价值被低估了。** 你自己说它「根本目的是计算节点生命周期和辅助调试」，实际上它还额外承担了两个更重要的职责：

1. **它是可复现性的锚点**：`state(seq=N) = fold(events[0..N])`，没有它就没有确定性重放。
2. **它是防死锁的节拍器**：`min_step/max_step` 让作者能精确控制"这个支线在第 3~12 步之间可触发"，到了第 13 步自动消失，剧情会自动收束。

**已实测通过这个能力**：
```
=== Frontier @ seq=6 phase=late_night node=n_confront ===
  BLOCK[expired] 0              <- 开场节点过了 max_step=2，自动失效
  BLOCK[phase]   n_kitchen      <- 厨房场景不允许在 late_night 出现
  BLOCK[terminal] -1            <- 终末节点不作为 Frontier 成员
  BLOCK[consumed] n_search n_apologize ...  <- once=true 已消耗
```
这就是你要的「剧情断裂时直接看当前 time_cursor 的 Frontier 列表」。

**风险与对策**：
- ⚠️ **风险**：`max_step` 写太小会导致支线静默消失，作者很难察觉。
- ✅ **对策**：`StoryLoader::lint()` 在启动期检查；运行时所有被拦截的节点都带 reason 落 `nx_route_audit` 表，可以随时查「哪条支线从没被看到过」。

### 支柱二：动态拓扑网络（Dynamic Topology & Frontier）—— ✅ 可行，难点不是性能而是死锁

#### 2.1 真正的难点：不是"怎么算"，而是"算出来为空怎么办"

你的原话提到「如果缺少兜底节点，极容易导致玩家无路可走的死锁」——这正是我在实现里花最大力气的地方，结论是：

> **死锁不能靠作者自觉避免，必须由数据结构保证。**

我的做法是把节点分成 5 个 **硬分桶（bucket）**，并在第三层的时间里拒绝空集：

```
bucket:  ending(40) > scripted(30) > redemption(20) > emergent(10) > fallback(0)
排序键:  (bucket 降序, score 降序, node_id 升序)
保底:    feasible 为空 -> 强制注入 kind=fallback 节点 -> 再为空 -> 抛错拒绝启动
```

**压力测试证据**：连发 40 轮完全无关的自由输入（"随便说点什么"），系统始终停 settling 到 `n_idle` 这个兜底节点，**零死锁**：

```
[OK] V11 任意自由输入连发 40 步无死锁 :: 40 步后停在 n_idle, seq=40
```

#### 2.2 空缺 1 的答案：多节点优先级怎么处理 —— 「硬分层 + 软打分 + 稳定排序」

> **不要试图用一个分数决出胜负。**

原因：一个节点该不该出现，至少同时被五种力量拉扯。把它们压成一个 `int`，作者每调一个地方就要重排全表；而且一旦出现问题，你无法回答"到底是哪一项占了上风"。

我的打分函数（每一项都能独立打印，这就是你想要的调试能力）：

| 因子 | 含义 | 默认权重 | 解决的问题 |
|---|---|---|---|
| `base` | 作者的 `priority` 意图 | 1.00 | 作者必须能手动压过一切 |
| `urgency` | 距 `max_step` 的紧迫度 | 0.80 | 快过期的支线要抢先出来，剧情自动收束 |
| `staleness` | 刚访问过的指数衰减惩罚 τ=6 | −1.20 | 防止在同一个点打转 |
| `recency` | 从未去过则加分 | 0.35 | 引导玩家探索未知分支 |
| `affinity` | 与 ROC 节点设计锚点的贴合度 | −0.40 | 让"适合当前关系状态"的节点浮现 |
| `cooldown` | 冷却剩余惩罚 | −2.00 | 抑制高频重复 |
| `pending_age` | 挂起时长的对数加权 | 0.50 | 挂得越久，挽回时分量越重（情感补偿） |

排序的**第三关键字是 `node_id` 字典序**——这不是偷懒，而是关键设计：它让 Frontier 结果**完全确定**，从而可以写断言测试，也让读档后的行为百分之百可复现。

#### 2.3 关于"全局遍历 vs 倒排索引"——一个反直觉的实测

你担心的是 C++ 里 BFS 刷新的开销。我先做了索引，然后诚实地做了对照实验：

| 节点规模 | 倒排索引 | 全表遍历 | 加速比 |
|---|---|---|---|
| 200 | 300 μs/步 | 638 μs/步 | 2.1× |
| 500 | 783 μs/步 | 1657 μs/步 | 2.1× |
| 1000 | 1472 μs/步 | 3529 μs/步 | 2.4× |

**结论：保留索引（2.1~2.4× 是免费的），但它不重要。**

因为同期一次 LLM 往返是 **0.8~3.0 秒 = 800,000~3,000,000 μs** —— 比重算整个 Frontier 慢 **500 倍以上**。上面还是 Python 的数字，换 C++ 还要再快一截。

> **这里的正确结论是：不要在这里做性能优化，要去砍 LLM 的调用次数。**
> 这正是三级漏斗（L0/L1 本地判定优先）存在的理由。实测在 demo 剧本里，11 轮对话只触发了 **1 次** LLM 调用，其余 10 轮全被本地层吃掉。

### 支柱三：事件驱动与事件挽回（Event & Pending Pool）—— ✅ 可行，但要接受一个额外约束

「打破线性 BFS 只能一路走到黑」是整套设计里最有商业价值的部分，但它有一个必须被显式承认的边界：

> **可以回到岔路口，但不能回到过去的时间。**

我把这一点固化成了两条约束：

- **C1 单调唤醒**：`awake_seq > birth_seq` 恒成立，且 `awake_seq == 当前 seq` 写入后永不修改。
- **C2 动态重生成**：唤醒时**绝不回放旧文本**，而是把节点重新放回 Frontier，由 AI 结合**当下的** `{phase}` / `{tags}` / `{recent history}` 重新生成台词。

第二条是实现你「在过去的时间语境里，用现在的口径重讲一遍」的关键。具体做法：

```jsonc
"n_refuse": {
  "redeemable": true,
  "redeem_gate": { "tag": "trust" },   // 只有拿到 trust 标签才允许被挽回
  "redeem_expire": 10,                 // 超过 10 步彻底销毁
  "effects": { "summary_hint": "哥哥曾经拒绝过沟通。" }  // 给 regeneration 用
}
```

`summary_hint` 是专门为动态重生成加的字段：旧事件的"事实"写在这里，AI  regenerate 时据此 + 当前 phase 生成新台词，避免"深夜问对方要不要吃午饭"的错乱。

**实测证据**（玩家在第 2 步选择了"你别管了"，错过了"坦白"）：
```
step=2 ... pending: [n_tired, n_search]  <- 两条没走的路入池
step=3 tags=[distant,regret] redeem=[n_tired]  <- 拿到 regret 后，"坦白"被挽回
```

#### ⚠️ 你自己点出的那个难点，我的判断

> 「如何消解旧事件与当前主线环境的逻辑冲突，是目前的最大挑战」—— 你原文

**我的结论：这个问题无法在框架层彻底解决，只能靠约束把冲突面缩小到可控范围。** 三条约束：

1. **阶段单调**：`set_phase` 只允许向 `catalog.phases` 中索引更大的方向走，物理时间永不倒退。这是硬闸门，实测 `n_goodnight` 想把 `late_night` 改回 `night` 被直接拒绝。
2. **事实 vs 台词分离**：`summary_hint` 存客观事实，台词永远是重新生成的。
3. **挽回到期**：`redeem_expire` 让太久之前的遗憾自然消解，而不是无限期挂着。

**这是本项目唯一需要靠"作者的文笔"兜底的部分，框架只能做到这里。**

---

## 二、空缺 2 的答案：状态栈怎么存才不会时空错乱

### 3.1 为什么当前的存法一定会错乱

现有 `DatabaseManager::saveData()` 是：

```cpp
std::string sql = "INSERT OR REPLACE INTO player_save (player_name, affinity, current_node, ...) "
                  "VALUES ('" + playerName + "', " + std::to_string(affinity) + ", '" + currentNode + "', ...);";
```

它只持久化了 **`current_node` 这一个标量**。而 Frontier 是由 **tags + items + pending + visited + rng_state + phase** 六者共同决定的。

> 读档后这六个量全部回到初始值，重算出来的 Frontier 必然与存档时刻不同 →
> 玩家表现为"读档后突然能去某个原本去不了的地方"，或"某个支线莫名其妙消失了"。
> **这就是你说的时空错乱，它的根因不是死锁也不是 bug，而是持久化粒度不够。**

### 3.2 方案：事件溯源 + 周期快照

```
state(seq=N) = fold( events[0 .. N] )
存档        = snapshot(ckpt) + events(ckpt+1 .. N)
```

sqlite 表结构见 `persistence/migrations/002_bfs_state.sql`。核心是**四道闸门，全部下沉到数据库层**，应用层想绕过也绕不过：

| 闸门 | 实现 | 防住了什么 |
|---|---|---|
| G1 | `PRIMARY KEY(session_id, seq)` | 同一 seq 重复写入 → 拒绝（历史只能追加） |
| G2 | 触发器 `seq == time_cursor + 1` | 游标跳变 / 回退 |
| G3 | 触发器禁止 `UPDATE` / `DELETE nx_event` | 篡改历史 |
| G4 | 触发器 `ckpt_seq <= time_cursor` | 凭空造出一个超前于历史的快照 |

**实测结果（真实 SQLite 引擎，17/17 通过）**：
```
[OK] G2b 跳号写入被数据库拒绝（防游标跳变）
[OK] G1  同一 seq 重复写入 -> UNIQUE 约束拒绝
[OK] G3  对 nx_event 执行 UPDATE/DELETE 被触发器拒绝
[OK] G4b ckpt_seq 超前 time_cursor -> 被拒绝（防凭空造档）
[OK] 回放后状态与在线状态一致 :: n11
[OK] 同一 request_id 重复登记被拒绝   <- LLM 调用幂等
```

### 3.3 三个容易漏掉、但一定会踩的坑

1. **`rng_state` 必须持久化。** 只要代码里用了 `std::random_device` 或任何不可控熵源，同一份存档两次加载就会走出不同剧情——这是最隐蔽的一种"错乱"。我用的是 splitmix64，种子随快照走，**实测两次相同输入序列的 state hash 完全一致**。

2. **AI 调用是外部副作用，必须幂等。** 发请求前先以 `request_id` 落一条 `CREATED`，回来再更新为 `RESOLVED`。崩溃恢复时用同一个 id 重试——否则要么重复扣费，要么出现"剧情走了一半但没记录"的半步状态。

3. **多步写入必须在一个事务里。** 现在"加道具 + 存档 + 加标签"是三次独立的 `sqlite3_exec`，中途崩溃会留下半个剧情状态。`EventStore` 里全部包进 `BEGIN IMMEDIATE ... COMMIT`。

---

## 三、🔴 现有代码的缺陷清单（按修复优先级）

### P0 —— 修好之前，任何演示都无法正常进行

#### 4.1 `AiEngine` 的"暴力清洗"会静默吞掉所有中文台词

`include/AiEngine.h:128-132`：

```cpp
std::string clean_str;
for (char c : json_str) {
    if (c >= 32 && c <= 126) clean_str += c;   // ← 只保留 ASCII
}
```

这段代码的本意是去除 markdown 符号，但它把所有 UTF-8 中文字节也一并删掉了。**我在本机完整复刻了这段逻辑并实测**：

| AI 原始返回 | 清洗后 | 玩家实际看到的 |
|---|---|---|
| `"reply": "哥哥你今天怎么凶我……"` | `"reply": ""` | **空白**（JSON 解析成功，无任何报错） |
| `"reply": "没关系啦，我去做饭。Wait, 你先休息一下。"` | `"reply": "Wait, "` | `Wait,` |
| `"reply": "我、我才没有担心你呢"` | `"reply": ""` | **空白** |

**为什么这是 P0 而不是 P2**：它不抛异常、不打日志，JSON 照样解析成功，玩家只会看到对话框是空的。这是一个"看起来能跑、实际上完全不可用"的状态。

**修法**（已在 `nx::IntentMatch` / `PromptAssembler` 体系中用另一条路解决）：
```cpp
// 只做三件事：去 markdown 代码围栏、去控制字符、提取第一个完整 JSON 对象
// 绝不做字符集过滤。UTF-8 字节 (>= 0x80) 必须原样保留。
// 并且：解析失败时不要直接返回兜底字符串，先尝试一次 repair loop，
//       再失败才降级到 L1 本地候选。
```

#### 4.2 SQL 注入 / 名字带引号即崩溃

`include/DatabaseManager.h:78, 84, 117, 126, 132, 144` —— 全部是字符串拼接。

```cpp
 std::string sql = "INSERT OR REPLACE INTO player_save ... VALUES ('" + playerName + "', ...);";
```

玩家名填 `O'Brien` → SQL 语法错误直接崩；填 `x'); DROP TABLE player_save;--` → 表没了。

**已实测**：改用 `prepared statement` 后，同一个恶意玩家名被当作普通字符串存进去，表完好无损。参看 `persistence/tests/test_store.py` 最后一项。

#### 4.3 构造函数静默失败 → 野指针 → 进入游戏必崩 ✅ **已修复**

> 这一条是 2026-10-30 把工程同步到 Ubuntu 上**实际编译并运行**时才暴露的，在此之前 Windows 侧从未发现。

`include/GameEngine.h` 原写法：

```cpp
DatabaseManager* db;                      // ⚠ 未初始化，是野指针
GameEngine(const std::string& jsonPath) {
    std::ifstream file(jsonPath);
    if (!file.is_open()) return;          // ⚠ 静默 return，db 从未被赋值
    ...
    db = new DatabaseManager("../data/save.db");
}
```

而 `main.cpp:27` / `qt_main.cpp:30` 传进来的是硬编码相对路径 `"../data/story.json"`，**只有在工作目录恰好是 `build/` 时才成立**。一旦从项目根目录（或任何其他地方）启动：路径失效 → 构造函数静默返回 → `db` 是野指针 → 玩家输入名字后调用 `createNewPlayer()` → `db->saveData(...)`。

在 Ubuntu 上实测就是直接 **SIGSEGV（`dumped core`）**。用 `-fsanitize=address,undefined` 拿到精确调用栈：

```
include/GameEngine.h:46:21: runtime error: member call on null pointer of type 'DatabaseManager'
ERROR: AddressSanitizer: SEGV on unknown address 0x000000000000
    #0 DatabaseManager::saveData(...)        include/DatabaseManager.h:80
    #1 GameEngine::createNewPlayer(...)      include/GameEngine.h:46
    #2 main                                  src/main.cpp:90
```

**为什么这个 bug 特别值得单独记一笔**：它同时犯了三个错，缺一不可地凑成了一次崩溃——
1. 成员变量裸指针不初始化；
2. 构造函数用"静默 return"而不是报错来表达失败（调用方完全无感）；
3. 数据路径硬编码且依赖当前工作目录。

任何一个单独存在都不会崩，凑一起就是"在你最需要 demo 的时候崩"。

**已做的修复**（`GameEngine.h` / `main.cpp` / `qt_main.cpp`）：

| 改动 | 说明 |
|---|---|
| `DatabaseManager* db = nullptr;` | 消除未定义行为，让"未初始化"变成可检测的空值 |
| `resolve_data_path()` | 依次尝试 `../data/X`、`data/X`、`./data/X`，兼容从 `build/` 和项目根目录两种启动方式 |
| `ready()` / `lastError()` | 构造状态显式暴露，调用方必须检查 |
| 所有 `db->` 调用加 `if (db)` 守卫 | 原本只有部分位置加了（`GameEngine.h:209,219`），不一致 |
| 两个 `main()` 里增加启动检查 | 失败时打印明确原因并 `return 1`，而不是继续跑下去等着崩 |

**修复后的实测结果**：

| 场景 | 修复前 | 修复后 |
|---|---|---|
| 从 `build/` 运行 | 正常 | 正常 |
| 从项目根目录运行 | **SIGSEGV** | 正常（自动解析到 `data/save.db`） |
| 数据文件都不存在时 | 崩溃 | 打印 `【致命错误】剧本文件打开失败: ...` 并退出 |
| ASan + UBSan | 报 null pointer / SEGV | **完全干净** |

顺带确认：整个 Qt 工程在 **Ubuntu 22.04 + GCC 11 + Qt 5.15.3 + libcurl/openssl + libsqlite3** 下 `cmake` 配置通过、`fs_terminal` 与 `fs` **两个目标均编译链接成功**，运行时中文显示正常、剧本加载正常、存档数据库连接正常（AI 那一步因未配置 API key 返回"（API内部报错）"，属预期）。

### P1 —— 影响稳定性与可维护性

| # | 位置 | 问题 | 修法 |
|---|---|---|---|
| 3 | `GameEngine.h:28` `DatabaseManager("../data/save.db")` | 相对路径依赖 CWD，换个目录启动就打不开/写错库 | 改成命令行参数或环境变量 `NX_DATA_DIR`，启动时校验 |
| 4 | `AiEngine.h:22,34` | 每轮对话都重开并重新 parse `config.json` 和 `prompts.json`；API Key 明文落盘 | 加载一次缓存；Key 优先读环境变量 `NX_API_KEY`，config.json 仅作 fallback |
| 5 | `GameEngine.h:190,209` | `db->addItem()` 与 `db->saveData()` 是两次独立写，无事务 | 包进一个事务 |
| 6 | `GameEngine.h:22` `AiEngine.h:80` | `CURLOPT_SSL_VERIFYPEER=0` 关闭了证书校验 → 中间人风险 | 生产必须开启；调试用 `NX_INSECURE_TLS=1` 环境变量控制 |
| 7 | `GameEngine.h:38,67,83,101` | `storyData["nodes"][currentNode]`：nlohmann 的 `operator[]` 在 key 不存在时**会插入一个 null**，既是 UB 风险也让 `nodes` map 悄悄变大 | 统一改用 `find()` + 显式校验 |
| 8 | `AiEngine.h:82` | `CURLOPT_TIMEOUT=10`（总超时）。GLM-4-Flash 生成较长 JSON 时偏紧，且无重试 | 拆分 connect timeout(3s) / total timeout(25s)，并加带退避的重试 |
| 9 | `DatabaseManager.h:144` | `addTag` 只插 2 列，但表有 3 列（`tag_type`），结果 `tag_type` 恒为 NULL | 补上 tag_type |

### P2 —— 体验与工程化

| # | 位置 | 问题 |
|---|---|---|
| 10 | `src/qt_main.cpp` | 虽然 `#include <QtConcurrent>`，但 `engine.processInput()` 实为同步调用 → AI 延迟期间 UI 卡死。必须移到 worker 线程，或至少显示 thinking 态 |
| 11 | 全局 | 没有 request_id / trace_id / 结构化日志 → 线上问题无法回溯 |
| 12 | `include/SecurityFilter.h:11-13` | 敏感词库只有 `滚/讨厌/烦` 三个词，且"烦"过于常见，误伤率高 |
| 13 | `data/items.json` | **0 字节空文件**（已确认） |
| 14 | `data/save.db` | 现存 3 条存档、2 条道具、0 条标签 —— 标签系统从未真正跑通过 |

---

## 四、为什么我做了一份 Python 参考实现

这不是为了多写代码，而是为了**让你不必信任我的说法**。

| 验证手段 | 结果 |
|---|---|
| `reference/simulate.py` | **24/24 通过**（含反死锁压力测试、确定性、阶段单调） |
| `persistence/tests/test_store.py` | **17/17 通过**（真实 SQLite 引擎验证四道闸门） |
| `engine/tests/test_core.cpp` | 用 g++ 编译、**零 warning**，运行后与 Python 参考实现 **11 行轨迹逐字节一致** |

第三项尤其重要：C++ 与 Python 是两个人（好吧，同一个模型的两次独立输出）写出来的两套实现，它们在同一份剧本、同一串输入下产出完全相同的结果，这是目前能对正确性给出的最强证据。

**而这个过程也确实抓到了 4 个只有跑起来才会暴露的缺陷**（详见 `docs/PROJECT_PLAN.md` 的风险登记）：
- 终末节点 `-1` 因为属于 `ending` 桶，会永远占据 Frontier 榜首 → 开局就直奔"游戏结束"
- `redeem_gate` 缺省值语义错误 → 写 `redeemable: true` 却永远救不回来
- 选项按钮点击也触发 LLM 调用 → 11 轮对话白烧 11 次请求（修后仅需 1 次）

---

## 五、不做 / 暂缓的部分

| 项 | 判断 | 理由 |
|---|---|---|
| **Embedding 向量层** | 暂缓 | 需要第二个 API（或本地模型），引入新依赖与新失败模式。当前的"关键词 + 中文二元组 Jaccard + MMR"在几十个候选的量级上已足够；当候选 > 50 且字面相似失效时再引入 |
| **真正的 BFS 图遍历** | 不做 | 这是命名上的误导。你的迷宫是**动态重算的前沿集合**，不是静态图上的最短路。保留 BFS 的比喻，但实现是「每步重算 feasible 集 + 分层排序」，不做图遍历 |
| **Web / 多人服务化** | 暂缓 | 单机 Qt 已经能完整验证叙事模型。先把引擎正确性做实，服务化是后话（届时 `engine/` 内核零依赖，可直接复用） |
| **长时间上下文（>8k token）** | 用滚动摘要解决 | 不堆历史，而是每 K 轮压缩成 `summary`。`PromptAssembler` 已实现按预算裁剪，实测把 20 轮长历史压到 776 tokens 且必留块完好 |

---

## 六、给你的三条建议

1. **先把 P0 修了再谈新功能。** 特别是 4.1——那个 bug 意味着你现有的 demo 事实上是跑不出内容的。修完也许你会发现很多之前以为的"AI 不听话"，其实是内容被静默清空了。

2. **不要先做索引优化，先做 LLM 调用次数优化。** 数据在这里：Frontier 重算 1472μs，LLM 往返 800,000μs+。省一次 LLM 的收益，抵得上把 Frontier 优化两千次。

3. **把"`为什么走这条路`"的审计数据先存起来。** `nx_route_audit` 表看着不起眼，但等你开始写第二条剧本、发现某条支线永远触达不到时，它是唯一能告诉你真相的东西。这需要真实运行数据才能提供价值，所以越早开始积累越好。
