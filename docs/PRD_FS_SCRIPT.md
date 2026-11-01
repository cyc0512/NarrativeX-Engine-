# PRD：FS 改名 + 新剧本 + 前后指针验证

| 项 | 内容 |
|---|---|
| 文档语言 | 简体中文 |
| 项目名（新） | **FS**（内部全称 FluidStory） |
| 技术栈 | C++17 + Qt5 5.15.3 + libcurl + SQLite3 + nlohmann/json（沿用现有工程，不更换） |
| 文档类型 | 简易 PRD（不含竞品分析与市场调研） |
| 版本 / 日期 | **v1.7** / 2026-10-30（v1.0~v1.6 同日迭代；v1.5 = M-14 条件算子拼写 P0 修正 + §5.8 算子 DSL 全集；v1.6 = M-17 用例 A 输入口径 + M-18 `redeem_variant` 裁定补内核归 T02 + R-12 增两条 lint + 规则 17/18；**v1.7 = M-18 收紧：R-10 升 P0 本轮必做、B-5 恢复为阻塞项（临时降级口径作废） + M-19 keywords 双方案收敛为"两条都要"**） |
| 作者 | 许清楚（产品经理） |
| 阅读对象 | 架构师、开发工程师、测试 |

### 修订记录（v1.0 → v1.1）

| # | 来源 | 修订内容 | 影响 |
|---|---|---|---|
| M-1 | 架构师 修订1 | 7 个对话节点 `phase.allow` 全开，phase 约束下沉到 `requires` | 修复 A-1/A-2/B-5 必然失败 |
| M-2 | 架构师 修订2 | `n5_search` 的 `step_gte` 由 2 改为 1 | 修复用例 A 第 2 步走不起来 |
| M-3 | 架构师 修订3 | 结局阈值由我建议的 86/80 改为 **82/81** | **我原建议会让"挽回线"死锁，已撤回** |
| M-4 | 架构师 修订4 | B-2 断言由"仅含"改为"含" | `n3_tender` 与 `n5_search` 会被同时唤醒 |
| M-5 | **我方新增** | 剧本**必须**有 `kind=fallback` 节点 `n_idle`（`StoryLoader.hpp:207` lint 强制） | 否则 R-05 lint 直接不通过 |
| M-6 | **我方新增** | `once` 默认 `true`（`StoryLoader.hpp:298`），`n6_silence` 必须显式 `once:false` | 否则用例 B 路径在 seq=5 断掉 |
| M-7 | **我方新增** | `fallback_next` **不参与**防死锁（`Frontier::fallbacks()` 只认 `kind=fallback`） | 架构师的 `n7_goodnight.fallback_next` 不足以防死锁 |
| M-8 | 架构师 AR-9 | `n6_silence` 选项①「对不起，我不该那样说」直连 `n3_tender` 会绕过挽回机制 | 我方建议比架构师更彻底：直接删除该选项（**架构师已撤回原方案、采纳我方**） |
| **M-9** | **我方新增（v1.2）** | 离线档 `default_next` **不生效、是死代码**（`IntentMatch.hpp:193-198` + `235`）；`n_idle` 保证 `cands` 永不为空；`llm==nullptr` 时 L1 无置信度门槛 | 修正 §4 决策点 B 与 §6 全部用例表；**离线演示脚本每步输入必须写死** |
| **M-10** | **我方新增（v1.2），架构师已确认并撤回其原方案** | `n7_goodnight` 的 keywords **不要扩增**（架构师偏差 6 措施① 算错了归一化）；**终裁：保持 4 词 + 给对手 `n5_search` 加 `negatives: ["晚安","睡"]`** | `kw = hits / keywords.size()`，加词会摊薄命中率；7 词方案下安全边际从 0.095 掉到 0.009。改用 `negatives` 杠杆后领先扩大到 **约 0.6**，且零全局副作用 |
| **M-11** | 架构师裁定 | 对话节点口径 `count(kind=="scripted") == 7`（原写 `kind != ending` 会把 `n_idle` 算进去变 8） | 最终 12 节点：7 scripted + 1 fallback + 3 ending + 1 terminal |
| **M-12** | 架构师裁定 | **7 个剧情节点全部 `once:true`**，撤回我方 M-6 的 `n6_silence.once:false` | `once:false` 会造成好感度渗漏（反复沉默每次 −3），吃掉 82/81 的 2 点缓冲 |
| **M-13** | **software-engineer 发现 + 我方修正** | 引擎有**两套** kw 算法：`l0()` = `0.5 + 0.25×hits`（**不看**词表长度）、`l1()` = `hits/size`（**加词摊薄**） | 规则 11 加限定语"摊薄只在 L1 成立"；新增**规则 14** + **§7.1.2 两套算法对照表**。**最终结论不变**（离线档全程走 L1），并澄清了发现者的两条反向推论 |
| **M-14** | **software-engineer 用真引擎加载实测发现，P0 文档 bug** | 条件算子的 JSON 拼写是 **`affinity_gte` / `affinity_lte`**，**不是** `affinity_at_least` / `affinity_at_most`（后者是 C++ 函数名）。`StoryLoader` fail-fast，未知算子直接 `throw BadScript` | **PRD v1.0~v1.4 全错**；全文 4 处已修正，**数值一个没动**（82/81/85/90）。新增 **§5.8 条件算子 DSL 全集**供后续查证 |
| **M-15** | **software-engineer 实测，产品侧判定** | **`redeem_variant` 引擎不支持**：`NodeSpec` 无此字段、`StoryLoader` 的 `presentation` 只解析 4 个平铺字段、`NarrativeEngine` 挽回时无切换台词逻辑 | 这是 PendingPool 的 **C2 硬约束**（不回放旧文本）的载体，**不可裁剪**。R-10 标注为"需补内核"；v1.6 曾写"引擎补齐前 B-5 降级为非阻塞项（只断言 B-2/B-3）"—— ⚠️ **该口径 v1.7 已作废（见 M-18），B-5 恢复为阻塞项** |
| **M-16** | **software-engineer 发现，我方采纳并优化** | keywords 是**子串匹配**（`hay.find`），`藏了什么` 永远匹配不上写死输入「你口袋里藏的是**什么**」，**白占归一化分母** | `n5_search.keywords` 优化为 **`口袋`/`藏`/`发卡`/`看看`** → 命中数 1→2，kw 0.25→0.5，且联机档可触发 L0 直达。新增**规则 15/16** |
| **M-17** | **架构师裁定 + 我方补充（v1.6）** | 用例 A 第 2 步输入改为「你口袋里藏了**什么**？」（与 `n5_search.examples[0]` 一致），**改输入不改 keywords** | kw 0.25→0.5。架构师原则"keywords 是作者资产、不该迁就测试常量"我方认同；但同时保留 M-16 的 keyword 优化（目的为真实玩家鲁棒性，且仍满足架构师新定的规则 17）。新增**规则 17/18**。**v1.7 由 team-lead 收敛为"两条都要"（M-19），悬置作废** |
| **M-18** | **team-lead + 架构师裁定（v1.6 裁定 / v1.7 收紧）** | `redeem_variant` **不降级、补内核，归属 T02**（因 B-5/A-1/A-2/A-3/B-6 均依赖它） | R-10 由 **P1 升 P0、标为本轮必做**；§5.5 与 B-5 中"引擎补齐前降级为非阻塞项"的临时口径 **v1.7 起全部作废**，**B-5 恢复为阻塞项**（降级的话甲方看到的就是"回到同一节点但台词一字不差"，与读档重开无差别） |
| **M-19** | **team-lead 收敛裁定（v1.7）** | 我方 M-16（改 **keyword** 编法）与架构师（改**测试输入**为 `examples[0]`）在 keywords 上给出两个方案，team-lead 裁定**两者互补、同时落地**，不是二选一 | ① keyword 编法取 M-16 原则（短而语义特异的子串 `口袋`/`藏`/`发卡`/`看看`）；② 测试输入取架构师规则（写死为该节点 `examples[0]`「你口袋里藏了什么？」）；③ 一致性由 lint（规则 17 / R-12 ④）兜底。**§6 用例 A 第 2 步不再悬置"请架构师二选一"**；最终 JSON 值由 software-engineer 实测确认后落地，**PRD 只定义规则、不落地文件** |

## 0. 原始需求复述（甲方原话要点）

1. **改名 FS**：把项目名改成 FS，方便在 Linux 上操作；同时修改一系列与之相关的地址/路径信息。
2. **崩溃 Bug**：自由输入对话时闪退，日志显示 AI 跳转到了 `end_getitem` 节点。甲方判断原因是"剧情太少，无法充分测试 AI 动态匹配节点"。
3. **重做剧本**：约 1000 字、50~70 句对话交互、**7 个对话节点**、**3 个结局**（甲方只明确了"1 个普通结局 + 1 个隐藏结局"，第三个结局由本 PRD 给出假设定义并待确认）。
4. **验证前后指针能力**：必须包含两个可复现用例 —— ① 未来节点提前触发（前向指针/预感节点）；② 过去节点重新回轨（后向指针/事件挽回）。
5. **Qt 界面重复输入框**：界面出现两个输入框（甲方判断是代码重复），需在修复的同时做一次 Qt 界面交互测试。

---

## 1. 产品目标（一句话）

> **让 FS 在"没有 API key"的条件下，也能用一份 1000 字的新剧本完整演示"AI 动态选路"，并通过两个可复现、可断言的用例，证明前向指针（未来节点提前涌现）与后向指针（错失事件被挽回）真实可用。**

据此派生三个正交子目标：

| # | 目标 | 可度量口径 |
|---|---|---|
| G1 | 能跑、能演示（先解决崩溃与空白，再谈智能） | 全程 0 闪退；所有中文台词 100% 可见；无 key 可完整走完 7 节点 → 3 结局 |
| G2 | 指针能力可证明（不是"看起来像"，是"断言得过") | 两个指针用例各有 ≥5 条机器可断言的判定点，可在无 UI、无 key 的离线模式下复现 |
| G3 | 工程身份统一为 FS | Windows / Ubuntu 两侧目录、CMake target、可执行文件、窗口标题全部一致 |

---

## 2. 用户故事

| # | 视角 | 用户故事 |
|---|---|---|
| US-1 | 玩家 | 作为一个玩家，我希望**自由输入中文**后一定能看到木子因的中文回应，而不是空白或直接闪退，这样我才能相信这个游戏是"活的"。 |
| US-2 | 玩家 | 作为一个玩家，我希望我说的每一句话都能被"听懂"并把我带到**语义上最贴切**的那一段剧情，而不是被死板的选项按钮限制，这样我才有"我在和她说话"的感觉。 |
| US-3 | 玩家 | 作为一个玩家，我希望我**错过的那条路还有机会补回来**（比如我之前伤了她，后来道歉时能重新走到"坦白"那一段），这样我的选择才有重量、后悔才有意义。 |
| US-4 | 玩家 | 作为一个玩家，我希望我的**激进举动会改变故事节奏**（比如一上来就翻她口袋，故事会立刻急转直下、提前收束），这样我的行为才真正影响叙事。 |
| US-5 | 开发者（作者本人） | 作为作者，我希望只改 `data/` 下的 JSON 就能增删节点和调整分支，不需要改一行 C++，这样我才能把精力放在台词上。 |
| US-6 | 开发者 | 作为开发者，我希望有一条**不依赖 API key、不依赖 Qt** 的命令行验证路径（`reference/simulate.py` + `engine/tests/test_core.cpp`），这样我在断网、没 key 的机器上也能证明引擎没坏。 |
| US-7 | 开发者 | 作为开发者，我希望每个节点的 `intent.description` 有写作规范和自动 lint，这样"AI 选错路"能被定位到具体某条描述写得不好，而不是归结为"模型玄学"。 |

---

## 3. 需求池

优先级定义：**P0 = 本次必须交付，缺一项则验收不通过**；**P1 = 应当交付，影响演示完整度/可维护性**；**P2 = 锦上添花**。

| ID | 优先级 | 需求 | 验收标准（可判定） |
|---|---|---|---|
| R-01 | **P0** | **项目改名 FS 与路径同步**：目录已统一为 `fs`；CMake `project()` 改为 FS；target `fluidstory` → `fs`、`nx_terminal` → `fs_terminal`；窗口标题改为 `FS · FluidStory` | ① Windows 与 Ubuntu 两侧目录名一致为 `fs`；② `cmake --build .` 后产出 `build/fs` 与 `build/fs_terminal`；③ 窗口标题字符串全仓库唯一且含 "FS"；④ `grep -rn "NarrativeX" src/ CMakeLists.txt` 仅剩注释/文档中的品牌说明 |
| R-02 | **P0** | **修中文被静默清空**（`include/AiEngine.h:128-134` 的 `if (c >= 32 && c <= 126)` 暴力清洗） | ① 输入 `"哥哥你今天怎么凶我……"` → 玩家侧可见非空中文回复；② 单测：含中文/emoji/中文引号的 JSON 字符串能被完整解析；③ 清洗逻辑改为"只剥离 markdown 代码块围栏与控制字符"，保留所有 UTF-8 |
| R-03 | **P0** | **修自由输入闪退**（日志停在 AI 跳转 `end_getitem`） | ① 连续 20 次自由输入（含空串、超长串、纯标点、纯中文、emoji）0 崩溃；② gdb 复现原崩溃路径后确认已消除；③ **必须输出一份《崩溃根因结论》**，逐一排除 §9.1 的 4 个候选根因，写明"实际是哪个 + 证据" |
| R-04 | **P0** | **Qt 输入框去重 + 交互测试** | ① `grep -c "new QLineEdit" src/qt_main.cpp == 1`；② 运行期 `gamePage->findChildren<QLineEdit*>().size() == 1`（加一条临时断言或调试输出，测完可保留为注释）；③ §8.3 的 12 项交互测试全部通过 |
| R-05 | **P0** | **新剧本（1000 字 / 50~70 句 / 7 对话节点 / 3 结局）** | ① 正文字数 900~1100（不含 JSON 字段名与标点外的空白）；② 台词句数 50~70；③ **`count(kind == "scripted") == 7`**（v1.2 修正：原写 `kind != ending` 会把 `n_idle`(kind=fallback) 算进去变成 8）；④ `kind == "ending"` 且 `id != "-1"` 恰好 3 个；⑤ 通过 `StoryLoader` 的 lint（无孤立节点、无死路、**必须有 `kind == "fallback"` 节点**）。最终构成 12 个节点：7 scripted + 1 fallback + 3 ending + 1 terminal |
| R-06 | **P0** | **前向指针用例可复现**（未来节点提前触发） | 见 §6 用例 A：5 条断言全部成立；对照组与实验组的步数差 ≥ 2；`debug_frontier()` 输出可区分"因 affinity 闸门进入"与"因 phase 被拦" |
| R-07 | **P0** | **后向指针用例可复现**（过去节点重新回轨） | 见 §6 用例 B：6 条断言全部成立；日志出现 `redeem n3_tender (born@X -> awake@Y)` 且 `Y > X`、`Y == 当前 seq`；`birth_seq` 未被改写 |
| R-08 | **P1** | **无 API key 的离线降级方案**（决策点 B） | ① `config.json` 缺 key 或 key 为 `"未配置密钥"` 时，自动进入离线模式且不弹报错对话框；② 离线模式下可点选项 + 输入关键词走完全部 7 节点并抵达 3 个结局中的任意一个；③ 界面顶部有一行不刺眼的"[离线演示模式]"提示 |
| R-09 | **P1** | **nx 内核接线到 Qt UI + AiEngine**（决策点 A 的落地） | ① Qt 走的是 `NarrativeEngine::step()`，不再是 `GameEngine::processInput()` 里的旧分支；② L2（LLM）返回的 `next_node` 必须落在本次 `routes_injected` 白名单内，否则回落 L1；③ `reference/simulate.py` 期望 24/24 仍通过，并新增本 PRD 的 2 个指针用例 |
| R-10 | **🔴 P0（v1.6 由 P1 升格；v1.7 明确为【本轮必做】）** | **挽回变体台词**（`redeem_variant`） | 节点被 PendingPool 唤醒时优先取 `presentation.redeem_variant`；未提供时回落 `presentation` 并在日志告警。用例 B 中 `n3_tender` 的挽回台词与首次访问台词**不同**。<br>⚠️ **v1.5 实测：`NodeSpec` 无此字段、`StoryLoader` 不解析、`NarrativeEngine` 无切换逻辑**（M-15）。<br>✅ **v1.7 终裁（team-lead + 架构师一致）：补内核、不降级、归 T02，本轮必做**。它是 PendingPool **C2 硬约束**（不回放旧文本）的唯一载体，也是 **B-5 / A-1 / A-2 / A-3 / B-6 五条断言的前置条件**——内核不补就没断言可写。<br>🚫 **B-5 不再是"引擎补齐前降级"的临时项**：v1.6 曾暂按"只断言 B-2/B-3"计，该临时口径 **v1.7 起作废**，B-5 恢复为**阻塞项** |
| R-11 | **P1** | **SQL 参数化**（`DatabaseManager.h` 字符串拼接注入风险） | ① 玩家名 `O'Brien` 可正常新建/保存/读取/删除；② 全部 SQL 改用 `sqlite3_bind_*`；③ 单测覆盖 `O'Brien`、`'; DROP TABLE saves;--` 两个名字 |
| R-12 | **P1** | **`intent.description` lint 工具** | 一条命令检查全部描述：① 两两相似度 ≥ 0.85 报冲突；② 缺 `keywords`/`examples`/`negatives` 报警；③ 含节点 ID 报错；④ **（v1.6 新增）每个节点的 `keywords` 必须至少 2 个能被该节点自身 `examples[0]` 同时命中（子串匹配）**，命中 <2 报"白占归一化分母"；⑤ **（v1.6 新增）`requires` 中出现 `compile_cond()` 不支持的算子必须加载期报错** —— 引擎现有抛 `NxError` 行为**是设计不是 bug**，lint 复用即可、只需补友好提示。lint 不通过则剧本不可加载 |
| R-13 | **P2** | **存档/读档覆盖指针状态** | 存档后重开，`pending` 池、`seq`、tags、items 完整还原；`CursorRegress` 异常在篡改存档时被正确抛出 |
| R-14 | **P2** | **Frontier 调试面板**（Qt 内可开关） | F1 呼出面板，实时显示当前 `seq`/`phase`/Frontier Top-K/被 BLOCK 原因/挂起池内容 |
| R-15 | **P1（v1.4 由 P2 升格）** | **意图准确率评测集** | ≥ 60 条玩家原话 → 期望节点，跑 L1/L2 各一遍，输出准确率矩阵，作为后续调优基线。**⚠️ 甲方已确认有 API key（Q-4），本项升格为必做**：必须验证 L2 准确率 ≥ L1，否则判不通过（见 §4 决策点 B「有 key 验收流程」） |

---

## 4. 关键技术决策（需拍板）

### 决策点 A：新剧本写在哪套体系里？

#### 两个选项的事实对比

| 维度 | 选项 1：旧 `story.json` + `GameEngine/AiEngine` | 选项 2：`story_v2` + `nx` 新内核 |
|---|---|---|
| 改动量 | 小（只加 JSON，几乎不改 C++） | 大（需把 `NarrativeEngine` 接到 Qt UI 与 AiEngine） |
| 前向指针（节点提前涌现） | **无法表达**。旧模型是 `current_node → intent_nodes` 的静态白名单，AI 只能跳到当前节点已声明的邻居 | **原生支持**。Frontier 全局计算，任一节点只要 `requires` 通过即可被选中，与作者书写顺序无关 |
| 后向指针（事件挽回） | **完全没有**。旧模型无 `seq`、无挂起池、无 `redeemable` 概念 | **原生支持**。`PendingPool` 已实现，`birth_seq/awake_seq` 单调约束 + 动态重生成已落地 |
| 离线可验证 | 不行。选路完全依赖 LLM，无 key 寸步难行 | 可以。`reference/nxref.py` + `simulate.py` 24/24、`test_core.cpp` 逐行一致，可脱离 UI 与网络断言 |
| 崩溃 Bug 能否根治 | 只能打补丁（加白名单），根因（无准入/无窗口/无游标）仍在 | 根因层面消失：`next_node` 必须先过 Frontier 白名单 + `cat_.find()==nullptr` 拦截 |
| 已投入的沉没成本 | — | Python 参考 24/24、SQLite 四闸门 17/17 已通过 |

#### 我的推荐：**选项 2（story_v2 + nx 内核），但以"分阶段、先离线后接线"的方式落地**

**理由（按权重排序）：**

1. **甲方本次的第一诉求就是"验证后指针能力"，而这恰恰是旧体系在结构上做不到的。** 在旧 `story.json` 里"验证后向指针"只能靠作弊（人为把 `end_getitem` 塞进某个节点的 `intent_nodes` 里假装它回来了），这在演示时是自欺欺人，也无法写成断言。选项 1 做不到验收标准 R-07。
2. **崩溃 Bug 的根因不在"剧情太少"。** 甲方自己的判断（"剧情太少所以测不出来"）是不准确的——实际上剧情少只会让 AI **更容易**乱跳，真正让程序崩的是"没有任何准入闸门 + 中文被清空 + 异步回调悬空指针"（详见 §9.1）。这三个问题里，前两个在 nx 内核里**已经从设计上被解决**了。所以修 bug 这件事，选项 2 反而是"顺路"的。
3. **"能跑、能演示"这个最急的诉求，靠选项 2 的离线路径反而更快达成。** nx 有 Python 参考实现，新增剧本可以在**不编译、不接线、不需要 key** 的情况下用 `python reference/simulate.py` 先跑通全部指针用例——也就是说"演示"这件事可以**今天就做**，不必等 Qt 接线完成。
4. **nx 内核已经付过钱了。** 24/24 + 17/17 的测试通过记录摆在那里，不用它等于把已验证的资产闲置，同时维护两套语义重叠的引擎。

**必须坦白的权衡（供架构师参考）：**

- **风险 1（最大）**：接线工作量不可低估。`qt_main.cpp` 现在整条链路都绑在 `GameEngine` 上（`getCurrentNodePages` / `getCurrentOptions` / `hasIntentNodes` / `processInput`），换成 `NarrativeEngine` 几乎要重写刷新逻辑。保守估计是**选项 1 的 3~5 倍工作量**。
- **风险 2**：`GameEngine` 还承担了数据库存档（`DatabaseManager`），而 nx 的持久化在 `persistence/`（事件溯源）。两套持久化并存会产生"Qt 读 `save.db`、nx 写事件表"的分裂。
- **风险 3**：`nx` 内核目前**零 UI 实战记录**，只在单测与 Python 仿真里跑过。第一次接 UI 必然暴露未在仿真中覆盖的问题（例如 `cooldown` 与 `once` 在真实连点下的表现）。

#### 折中落地路径（我建议架构师按此排期）

| 阶段 | 内容 | 产出 | 是否阻塞演示 |
|---|---|---|---|
| **S1（P0，先做）** | 在 `nx` + `story_v2` 上写新剧本；用 `reference/simulate.py` 离线跑通 §6 的两个指针用例 | 新剧本 JSON + 2 个通过的单测 + 一份 Frontier 日志 | **不阻塞**（这是能最早拿出的演示证据） |
| **S2（P0，并行）** | 旧链路止血：R-02 中文清洗、R-03 崩溃、R-04 输入框、R-01 改名 | 一个"不会崩、中文正常、只有一个输入框"的 FS 可执行程序 | 不阻塞 |
| **S3（P1）** | 把 `NarrativeEngine` 接到 Qt UI；`AiEngine` 降级为 nx 的 `ILlmSelector` 实现 | Qt 走新内核，保留旧的 `GameEngine` 路径做 A/B 回退开关 | 阻塞"UI 上演示指针" |
| **S4（P1）** | 持久化统一到 `persistence/`；R-11 参数化 | 存档同时保存 `pending` 池 | — |

**一句话给架构师**：如果只剩 1 天，做 S1+S2；如果有 3 天以上，做 S3。选项 1 只在"甲方明确放弃本次指针验证"时才值得选。

> ⚠️ **此项列入 §10 待甲方确认事项**（需甲方在"快速演示"与"真实验证"之间二选一或确认接受折中路径）。

---

### 决策点 B：AI 没配 key 的情况下怎么验收？

#### 现状

`AiEngine.h:21` 硬编码 `std::string api_key = "未配置密钥";`，因此无 key 时 HTTP 返回 `error` → `analyzeInput` 返回 `{"reply": "（API内部报错）"}`，玩家只能看到一行错误提示，剧情完全走不动。

#### 我的定义：三档降级，档位自动探测

| 档位 | 触发条件 | 选路方式 | 台词来源 |
|---|---|---|---|
| **L2 完整档** | `config.json` 有有效 key 且网络通 | LLM 从 `routes_injected`（Frontier Top-K）中选一个；选中的必须在白名单内 | LLM 实时生成 |
| **L1 离线档**（默认降级） | 无 key / key 无效 / 超时 5s / 返回不在白名单 | `IntentMatcher::l1()`：关键词命中 + 示例句相似度 + 负例惩罚（`kw_hit_score 0.55` / `kw_full_bonus 0.3` / `sim_weight 0.45` / `neg_penalty 0.5` / `l1_margin 0.12`） | 节点预写台词 `presentation.npc_text`（**作者写死在剧本里**） |
| **L0 按钮档** | 任何时刻玩家点了选项按钮 | 直接取 `options[i-1].next`，**不调用 LLM**（`NarrativeEngine::match_option` 已实现） | 节点预写台词 |

#### 关键结论（回答甲方的问题）

> **能。无 key 也可以完整走完整个剧情并抵达 3 个结局中的任意一个。**

前提是满足下面三条，缺一不可：

1. **每个节点都必须预写 `presentation.npc_text` 与 `narrator`**（新剧本已按此设计，见 §5）——台词是作者写的，不是 AI 写的。这本来就应该是 Galgame 的常态：AI 负责**选路**，作者负责**文笔**。
2. **每个节点的 `intent.keywords` 必须覆盖玩家真正会打的中文词**（见 §7 规范）。离线档的选路质量 100% 取决于这几个词。
3. **必须有一个 fallback 节点**（`n_idle` 式保底），保证任何一句听不懂的话都不会让玩家卡死。

另外：**必须保留"选项按钮"**。按钮是 L0 档，天然不依赖 AI —— 所以即使 AI 完全不可用，玩家仍能靠点按钮走完主线。这是离线演示的最后一道保险。

> **⚠️ v1.2 补充（M-9，我方自查发现）**：离线档**没有"无明确意图 → 落 `default_next`"这条路径**。
> `decide()` 只在 `cands.empty()` 时用 `default_next`（`IntentMatch.hpp:193-198`），而 `n_idle` 保证 `cands` 永不为空 → **`default_next` 在离线档是死代码**。
> 实际行为：中性输入 → L1 打分第一的可行节点 ≈ **当前 Frontier 分数最高的节点**。不会卡死（体验上是"她顺着话接下去"），
> 但**作者写的 `default_next` 不起作用，推进方向由 `priority + bucket + 打分` 决定**。
> 对演示的直接影响：**离线演示脚本每一步输入都必须写死并命中目标 keywords，不能靠"点继续"推进**（见 §6 通用纪律）。
> 另注意：`llm == nullptr` 时 L1 的置信度门槛被短路（`IntentMatch.hpp:235` 第一个条件），
> 即**离线档不设最低分与 margin 要求**，任何输入都会选出一个节点 —— 好消息是不卡死，坏消息是"听不懂"时不会礼貌地原地不动。

#### 无 key 验收流程（R-08 的判定步骤）

1. 删除/改名 `config.json`，或把 `api_key` 置为 `"未配置密钥"`；
2. 启动 `./fs`，界面顶部出现 `[离线演示模式]` 提示，**不弹任何报错对话框**；
3. 新建玩家，全程只用**按钮 + 中文关键词**操作；
4. 走完 §6 用例 A → 抵达 `end_hidden`；
5. 重开，走完 §6 用例 B → 抵达 `end_regret`；
6. 重开，走坦白线 → 抵达 `end_calm`；
7. 三条线全程 0 崩溃、0 空白、0 英文报错串。

#### 有 key 验收流程（补充）

1. 写入有效 key，界面无 `[离线演示模式]` 提示；
2. 同一句话在 L1/L2 两档下各跑一次，记录选路是否一致（**允许不一致，但如果 L2 连续 3 次选出白名单外的节点，说明 prompt 或描述有问题，判不通过**）；
3. 断网 10 秒后恢复：必须自动落回 L1 档且不崩溃（R-03 的连带验收）；
4. 用 §7 的 60 条评测集（R-15）跑一遍，L2 准确率应 ≥ L1 准确率；若 L2 < L1，判不通过。

---

## 5. 节点表（7 个对话节点）

### 5.1 全局参数

- `phases`: `["evening", "night", "late_night"]`
- 初始 `affinity = 90`（沿用 `GameEngine::createNewPlayer`），`seq = 0`，`phase = "evening"`，起始节点 `n1_entry`
- 三个 `kind == "ending"` 的节点 + 一个 `kind == "ending", terminal: true` 的 `-1`
- `intent` / `frontier` / `budget` 三组权重直接沿用 `data/story_v2.example.json` 的现有数值，本 PRD 不改动

### 5.2 节点表

| # | 节点 ID | 名称 | 一句话功能 | `phase.allow` | 进入条件 `requires` | `default_next` | `redeemable` |
|---|---|---|---|---|---|---|---|
| 1 | `n1_entry` | 玄关·回家 | 开场寒暄，承接玩家任意开场白（打招呼/夸菜香/沉默进门） | **全开** | 无（`window 0..2`, `once: true`, `priority 100`） | `n2_kitchen` | false |
| 2 | `n2_kitchen` | 厨房·岔路口 | **三岔口**：示弱 / 拒绝 / 追问口袋；未选的两条被压入挂起池 | **全开** | 无（`once: true`, `priority 60`） | `n6_silence` | false |
| 3 | `n3_tender` | 坦白·示弱 | 玩家承认自己累/脆弱，她放下菜刀拥抱 | **全开** | 无（`once: true`, `priority 75`） | `n7_goodnight` | **true**（`expire 10`, `gate: {tag: "regret"}`） |
| 4 | `n4_shut` | 拒绝·冷战 | 玩家防御/疏远/不耐烦，她眼眶红了 | **全开** | 无（`once: true`, `priority 70`） | `n6_silence` | **true**（`expire 10`, `gate: {tag: "trust"}`） |
| 5 | `n5_search` | 口袋·发卡 | 玩家追问/翻她口袋，掏出带血发卡，气氛骤变 | **全开** | `{any: [{step_gte: **1**}, {tag: "suspicious"}]}`（`once: true`, `priority 78`） | `n6_silence` | **true**（`expire 12`, `gate: {tag: "regret"}`） |
| 6 | `n6_silence` | 沉默·后悔 | 长时间沉默，她不回头；玩家开始后悔；**天黑了** | **全开** | 无（**`once: true`**, `priority 55`） | `n7_goodnight` | false |
| 7 | `n7_goodnight` | 晚安·收束 | 收束今晚，通往结局判定 | **全开** | `{any: [{phase_in: ["night","late_night"]}, {affinity_lte: 85}]}`（`once: true`, `priority 50`） | `end_calm` | false |
| — | `n_idle` | **保底闲聊（非剧情节点）** | Frontier 为空时的防死锁闸；**`StoryLoader.hpp:207` lint 强制要求存在** | 全开 | 无（`kind: "fallback"`, `once: false`, `cooldown: 1`, `priority 10`, `must_freeform: true`） | `n6_silence` | false |

> **⚠️ v1.1/v1.2 关键修订（M-1 / M-2 / M-5 / M-6 / M-9 / M-10），全部是源码级硬约束，工程师照此落地：**
>
> 1. **`phase.allow` 一律全开 `["evening","night","late_night"]`**（架构师修订1，已核对 `Frontier.hpp:103`）。
>    `FrontierScorer::gate()` 的判定顺序是 **`phase_ok` 硬门槛（L103）在 `requires_fn`（L111）之前**，
>    且 `min_step/max_step`（L101-102）更早。所以"phase + requires 双写"时，requires 里的 affinity 分支**永远没机会生效** → 用例 A 的 A-1/A-2 必然失败；
>    同理 `n3_tender` 若在 `phase=night` 时还限制 `evening`，则**永远救不回来** → B-5 失败。
>    phase 的叙事意义（"天黑了"）改由 `n6_silence.effects.set_phase="night"` 承载、由 3 个 ending 的 `requires` 消费。
> 2. **`n5_search.step_gte` 必须是 1 不是 2**（架构师修订2，已核对）。用例 A 第 2 步发起时 `st.seq == 1`，
>    `step_at_least(2)` = `1>=2` = false → `BLOCK[future/requires]` → 「你口袋里藏的是什么」只能落到 L1 打分第一的其他节点，**用例 A 根本走不起来**。
> 3. **必须存在 `kind:"fallback"` 的 `n_idle`**（我方新增 M-5）。`StoryLoader.hpp:207-208` 明确报错
>    `"剧本缺少 kind=fallback 的兜底节点：一旦 Frontier 重算为空就会死锁"` → 没有它 **R-05 的 lint 验收直接不通过**。
>    且 `Frontier::fallbacks()`（L281-295）**只认 `kind == Fallback`**，所以 `fallback_next` 字段**不参与**防死锁（M-7）——
>    架构师给的 `n7_goodnight.fallback_next = "end_regret"` 可以作为外层跳转兜底保留，但**不能替代 `n_idle`**。
> 4. **【v1.5 新增 M-14，P0】条件算子的 JSON 拼写是 `affinity_gte` / `affinity_lte`，不是 `affinity_at_least` / `affinity_at_most`**（software-engineer 用真引擎加载实测发现，PRD v1.0~v1.4 全错）。
>    `StoryLoader::compile_cond()`（`StoryLoader.hpp:379-402`）只认下表的算子，**其余一律 `throw NxError("未知条件算子: " + op)`**，
>    而 `StoryLoader::load` 是 fail-fast → **照旧文档抄，剧本连编译都过不去，直接 `BadScript` 退出**。
>    `at_least` / `at_most` 是 **C++ 侧函数名**（`Condition.hpp:84/87`），不是 JSON DSL 拼写 —— 这是我写文档时的口语化串台。
>    **全文 4 处已修正**（§5.2 `n7_goodnight`、§5.6 `end_calm`/`end_regret`、§6.1 触发前提、§7.2 lint 注记）。**数值一个没动**（82 / 81 / 85 / 90）。
>    完整算子表见 §5.8。

> 5. **`once` 默认是 `true`**（`StoryLoader.hpp:298` `n.once = raw.value("once", true)`，我方新增 M-6）。
>    ⚠️ **v1.2 修正**：M-6 我原提的"给 `n6_silence` 加 `once:false, cooldown:2`"**已被架构师否决、我方接受**。
>    否决理由成立：`once:false` 会让"沉默"可重复进入，**每次 −3 好感 → 好感度渗漏**，玩家反复沉默就能把 aff 一路磨下去，
>    直接吃掉刚固化的 82/81 缓冲（本来只有 2 点）。
>    **最终方案：7 个剧情节点全部 `once:true`，只有 `n_idle` 是 `once:false`。**
>    断路径问题改由 `n3_tender.default_next` 由 `n6_silence` 改指 **`n7_goodnight`** 单独解决（架构师已复核其余 6 个节点 `once:true` 无冲突）。
> 6. **【v1.2 新增 M-9，P0】离线档 `default_next` 是死代码** —— 见下方专章，它会同时影响 §4 决策点 B 与全部用例表格。
> 7. **【v1.2 新增 M-10，P1】`n7_goodnight` 的 keywords **不要扩增** —— 见下方专章（架构师的补 keywords 方案算错了归一化）。

---

#### ⚠️ M-9（v1.2 新增，P0）：离线档 `default_next` 不生效，是死代码

**源码事实（`IntentMatch.hpp:188-198` + `235`）：**

```cpp
IntentDecision decide(input, cat, cands, default_next, llm) {
    if (cands.empty()) { d.node_id = default_next; ... return d; }   // L193-198 唯一出口
    ...
    if (llm == nullptr || (scored[0].second >= 0.5 && margin >= w_.l1_margin)) {
        d.node_id = scored[0].first; ... return d;                    // L235 离线档必走这里
    }
}
```

两条推论，都是硬的：

1. **`cands` 在离线档永不为空**。`n_idle` 无 `requires` → `finalize_index()` 把它登记进 `index.always`
   → `candidate_ids()` 每步都返回它 → `gate()` 对它恒 Ok（`once:false`、无窗口、无 phase 限制）。
   所以 `cands.empty()` 永远不成立 → **`default_next` 永远不会被用到**。
2. **`llm == nullptr` 时 L1 无置信度门槛**。L235 的第一个条件短路，所以**哪怕所有节点 kw/sim 全 0、margin 远低于 `l1_margin=0.12`，
   也照样返回 `scored[0]`**（即 Frontier 分数最高的那个可行节点）。

**对我自己 PRD 的纠正（我 v1.0/v1.1 写错了）：**

| 位置 | 原表述（错） | 修正后（对） |
|---|---|---|
| §4 决策点 B | "无明确意图 → 落 `default_next`" | 离线档**不存在"落 default_next"**这条路径。无明确意图 → 落到 **L1 打分第一的可行节点**（≈ 当前 Frontier 分数最高的节点）。`default_next` 只在 `cands` 为空时生效，而 `cands` 永不为空 |
| §5.2 表格 | `default_next` 列当作"推进主线的主要机制" | 降级为**文档性标注**（说明作者意图），**不是**运行期机制 |
| §6 用例表 | 多步写"无明确意图 → `default_next`" | 改为"输入命中目标节点 keywords → L1 选中"，**输入必须写死** |

**行为上是否可接受？** 可接受，但性质变了：
- 好消息：中性输入不会卡死，会推进到"当下最贴合的节点"（Frontier 分数最高的那个），体验上是"她顺着话接下去"。
- 坏消息：**推进方向不可控**，由 `priority + bucket + 打分` 决定，作者写 `default_next` 是**不管用的**。
  这正是架构师偏差 6 的根因，也是下面两条后果的来源。

**衍生：L0 只在最高 bucket 的那一梯队里找（`IntentMatch.hpp:200-221`）**
```cpp
const int top_bucket = max_element(cands, by bucket)->bucket;
for (c : cands) { if (c.bucket != top_bucket) continue; ... }   // 非最高 bucket 一律跳过
```
因为 `bucket_of(Ending)=40 > Scripted=30`，**一旦任一 ending feasible，L0 就只会在 ending 里找**，
所有剧情节点的 keywords 在 L0 层被完全忽略 → 落到 L1。这是设计意图（防低优先级节点抢答），
但也意味着 **ending 的 keywords 一旦被玩家说中就会立刻结束**（如 `end_regret` 的 `算了/当我没说/没什么`），
作者在写 ending keywords 时必须比剧情节点更谨慎。

---

#### ⚠️ M-10（v1.2 新增，P1）：`n7_goodnight` 的 keywords **不要扩增**，架构师这条算错了归一化

架构师偏差 6 的固化措施① 是"给 `n7_goodnight` 补强 keywords `["晚安","睡觉","该睡了","睡吧"]`"。
**我核过 `IntentMatch::l1()`（L133-136）后认为这会适得其反，建议不采纳。**

根因是 **kw 按关键词总数归一化**：
```cpp
kw = min(1.0, contains_count(input, n.intent.keywords) / (double)n.intent.keywords.size());
s  = w_.kw_hit(0.55) * kw + w_.sim(0.45) * sim + w_.prior(0.15) * c.score - w_.neg_penalty * neg;
```

**加词会摊薄命中率。** 实测代入：

| keywords 集合 | 输入「该睡了，晚安。」 | hits / size | kw | kw 项得分 |
|---|---|---|---|---|
| 现有 4 个 `晚安,睡了,困了,回房` | 命中「晚安」+「睡了」（`该睡了` 含子串 `睡了`） | 2 / 4 = **0.50** | 0.50 | **0.275** |
| 架构师 7 个 `晚安,睡了,困了,回房,睡觉,该睡了,睡吧` | 命中「晚安」+「睡了」+「该睡了」 | 3 / 7 = **0.43** | 0.43 | **0.236** |

| keywords 集合 | 输入「那……晚安吧。」 | hits / size | kw | kw 项得分 |
|---|---|---|---|---|
| 现有 4 个 | 1 / 4 = **0.25** | 0.25 | **0.1375** |
| 架构师 7 个 | 1 / 7 = **0.14** | 0.14 | **0.0786** ← 掉了 43% |

**结论：在"测试输入已写死"的前提下，加词只会降低目标节点的 L1 分数。**
`n5_search`（priority 78）的先验项就有 `0.15 × 1.13 ≈ 0.17`，
架构师 7 词方案下 `n7` 总分 `0.0786 + 0.1275 = 0.206`，只比 `n5_search` 高 0.036；
4 词方案下 `0.1375 + 0.1275 = 0.265`，高出 0.095 —— **安全边际翻了 2.6 倍**。

**我的建议（三选一，推荐①）：**
1. **保持 4 个 keywords 不变**，靠"输入写死 + 命中 2 个"解决（推荐，`kw` 稳定在 0.5）；
2. 若确实想加词，**同时**把 `n7_goodnight.priority` 由 50 提到 **62**（先验项 `0.15×(0.62+0.35)=0.1455`），用 prior 而非 kw 补强；
3. 若采用架构师的 7 词方案，则测试输入必须命中 ≥3 个词（如「我困了，该睡了，晚安。」→ 4/7=0.57）。

---

> ## ✅ M-10 终裁（架构师已确认我方算术并撤回其补词方案）
>
> **最终方案：① 保持 `n7_goodnight` 4 个 keywords 不变 ＋ 架构师追加的精准杠杆 `n5_search.negatives += "睡"`。**
>
> 架构师否决了我方的方案②（`priority` 50→62），理由成立，**我方接受**：
> > `priority` 是**作者意图层的旋钮**，改它会**全局改变 Frontier 排序** —— n7 从 50 提到 62 会越过 `n6_silence` 的 55，
> > 影响**所有路径的每一步**，而不只是要修的那一步。用作者意图层的钝器去修一个"测试输入"层的问题，是**改错了层**。
> > 而且 n7 priority 低（收束位应当靠后）本来就是刻意设计。
>
> 架构师否决方案③的理由也成立：会逼出「我困了，该睡了，晚安。」这种不自然的台词，污染 1000 字剧本的语感。
>
> **他追加的 `negatives` 杠杆确实比我的两个方案都好**（我方确认）：
> | 项 | 值 |
> |---|---|
> | `neg` 权重 | **−0.5**，且**只在命中时触发**（精准，零全局副作用） |
> | `n5_search`（输入含"晚安"） | `0.17(prior) − 0.5 = **−0.33**` |
> | `n7_goodnight` | `0.1375(kw) + 0.1275(prior) + sim ≈ **0.265**` |
> | **领先** | **约 0.6** —— 比纯 4 词方案的 0.095 **高一个量级** |
>
> 且**语义完全自洽**：「我要睡了／晚安」本来就是「翻她口袋」的强反例信号，不是为过测试硬塞的 hack。
>
> **落地指令**：`n5_search.negatives` 由 `["算了","晚安"]` 改为 **`["算了","晚安","睡"]`**；`n7_goodnight.keywords` 保持 `["晚安","睡了","困了","回房"]` 不变。
> 已同步进 §7.2 表格与 §7.1 规则 11/12/13。

> 顺带提炼成一条**通用写作规则**（并入 §7.1 规则 11/13）：
> **① keywords 不是越多越好 —— 每加一个词都会摊薄所有已有词的命中率。目标是"小而准"，3~4 个为佳，
> 且必须能被同一句玩家原话同时命中 2 个以上。**
> **② 想压制某个对手节点时，优先给【对手】加 `negatives`，而不是给自己加 keywords、更不要动 `priority`。**

### 5.8 条件算子 DSL 全集（v1.5 新增，M-14 —— 写 JSON 前必查）

> **来源**：software-engineer 用真引擎加载实测，抓出 PRD v1.0~v1.4 的 4 处拼写错误（`affinity_at_least/at_most` 引擎不认识）。
> **教训**：`at_least` / `at_most` 是 **C++ 侧函数名**（`Condition.hpp:84/87`），**JSON DSL 层是 `gte` / `lte`**。写文档时串台了。

**`StoryLoader::compile_cond()`（`StoryLoader.hpp:379-402`）支持的算子全集 —— 表外一律抛异常：**

| 算子 | 参数 | 含义 |
|---|---|---|
| `all` / `any` | 数组（条件对象） | 与 / 或 |
| `not` | 条件对象 | 非 |
| `tag` | 字符串 | 拥有该 tag |
| `tags_all` / `tags_any` / `tag_none` | 字符串数组 | 全含 / 任一 / 全不含 |
| `item` / `items_all` | 字符串 / 数组 | 拥有道具 |
| **`affinity_gte`** | 数字 | 好感度 ≥ |
| **`affinity_lte`** | 数字 | 好感度 ≤ |
| `visited` / `not_visited` | 节点 ID | 访问过 / 未访问过 |
| `visit_count_gte` | `[nodeId, n]` | 访问次数 ≥ n |
| `step_gte` / `step_lte` | 数字 | `seq` ≥ / ≤ |
| `phase_in` | 字符串数组 | 当前 phase 在列表内 |
| `phase_index_gte` | 数字 | 阶段序号 ≥ |

**⚠️ 四条硬事实：**
1. **`StoryLoader::load` 是 fail-fast —— 这是设计，不是 bug。**
   遇到未知算子直接 `throw NxError(BadScript, "未知条件算子: " + op)`，**剧本连编译都过不去**（实测 `EXIT=3`）。
   刻意不做"忽略未知算子"的宽容处理，是因为静默忽略会让作者以为条件生效了、实际从未生效 —— 症状会伪装成"剧情走向不对"，极难定位。**快速失败优于静默错误。**
2. **注意区分两类宽容度（行为相反，别混淆）**：
   `requires` 里的**未知算子** → **抛异常**（fail-fast）；
   **未知节点字段**（如本 PRD 的 `redeem_variant`）→ **静默忽略**（前向兼容占位）。
3. **`affinity_anchor` 是在 `affinity_gte/lte` 分支里累加出来的**（`StoryLoader.hpp:386-387`：`deps.affinity_sum += v; ++deps.affinity_n;`）。
   它决定 Frontier 打分的 `f_affinity` 项（"玩家当前好感度与该节点设计意图的贴合度"）。
   → **用错算子不只是加载失败**：若将来有人误用别的合法写法绕开，锚点不会累加，**症状会伪装成"剧情走向不对"而不是"剧本报错"**，排查成本极高。这也是为什么 `end_calm`（`affinity_gte: 82`）在坦白线 aff=94 时仍拿到不错的先验分。
4. **命名风格不统一，不要凭函数名猜**：`step_gte/step_lte`、`affinity_gte/lte`、`phase_index_gte`、`visit_count_gte` 都是 **`gte/lte` 风格**；
   只有 `tag`/`item`/`phase_in`/`visited` 是**单词**。而 C++ 侧辅助函数恰恰叫 `affinity_at_least()` / `affinity_at_most()`（`Condition.hpp:84/87`）—— **DSL 名与 C++ 函数名不一致，这是本次 P0 的根因**。

> **v1.6 补充（引擎侧配套，team-lead 已安排）**：工程师将在 `compile_cond()` 里为 `affinity_at_least`/`affinity_at_most` **加别名支持并给 warning**，防止下一个写剧本的人再踩。
> **但本 PRD 一律使用规范拼写 `affinity_gte` / `affinity_lte`** —— 别名只是容错兜底，不是推荐写法。

**本剧本实际用到的算子（工程师照此写）**：`any` / `all` / `tag` / `item` / `phase_in` / `step_gte` / `affinity_gte` / `affinity_lte`。

---

### 5.3 各节点的 effects（确定性副作用）

| 节点 | `add_tags` | `add_items` | `affinity_delta` | `set_phase` | `summary_hint` |
|---|---|---|---|---|---|
| `n1_entry` | — | — | 0 | — | 哥哥回了家。 |
| `n2_kitchen` | — | — | 0 | — | 她察觉哥哥心不在焉，并捂住了围裙口袋。 |
| `n3_tender` | `trust` | — | **+5** | — | 哥哥坦白自己很累，她抱住了他。 |
| `n4_shut` | `distant` | — | **−10** | — | 哥哥拒绝沟通，她被刺痛了。 |
| `n5_search` | `discovered` | `item_hairclip` | **−15** | — | 哥哥从她口袋里掏出了带血的发卡。 |
| `n6_silence` | `regret` | — | **−3** | **`night`** | 长时间的沉默，哥哥开始后悔；天黑了。 |
| `n7_goodnight` | — | — | **+2** | — | 夜深了，准备休息。 |

### 5.4 各节点的选项（`options`，L0 档用）

| 节点 | 选项文本 → `next` |
|---|---|
| `n1_entry` | ① 走过去打招呼 → `n2_kitchen` |
| `n2_kitchen` | ① 对不起，我今天有点累 → `n3_tender`；② 没什么，你别管了 → `n4_shut`；③ 你口袋里藏了什么 → `n5_search` |
| `n3_tender` | ① 抱紧她 → `n6_silence`；② 问她口袋里的事 → `n5_search` |
| `n4_shut` | ① 沉默地看着她做饭 → `n6_silence`；② 岔开话题 → `n7_goodnight` |
| `n5_search` | ① 拿着发卡质问她 → `end_hidden`；② 还给她，当作没发生 → `n6_silence` |
| `n6_silence` | **只保留 ① 继续沉默 → `n7_goodnight`**（v1.1 修订 M-8，见下方说明） |
| `n7_goodnight` | ① 回房 → `end_calm` |
| `n_idle` | ① 回去继续刚才的话题 → `n6_silence` |

> **⚠️ v1.1 修订 M-8（对应架构师 AR-9，我方建议比架构师更彻底）**
> v1.0 里 `n6_silence` 的选项①「对不起，我不该那样说」直连 `n3_tender`。问题在于：
> `NarrativeEngine::match_option()`（L219-230）是 **L0 层，在三级漏斗之前**执行，**完全绕过 Frontier 与 PendingPool** ——
> 玩家点这个按钮走过去，既不会触发 `redeem`，也**看不到 `redeem_variant`**，等于用按钮绕过了"事件挽回"这个本次最想验证的机制。
>
> - 架构师建议：把该按钮改指向 `n7_goodnight`。
> - **我方建议（更彻底）：直接删除该选项。** 理由：① 改指向 `n7_goodnight` 后，「道歉」这个动作在按钮上变成了"结束今晚"，语义是歪的，玩家会觉得莫名其妙；
>   ② 删掉之后，`n6_silence` 只剩一个出口，**"想道歉"就只能靠自由输入** → L1 命中被唤醒的 `n3_tender` → **玩家必然经过 PendingPool、必然看到 `redeem_variant`**。
>   这恰好把"事件挽回"变成了**唯一通路**，用例 B 的演示效果反而最强。
>
> 请架构师二选一拍板；若采纳我方方案，`n6_silence` 的文本 hint 需相应改为引导自由输入（如「【提示】你可以直接对她说点什么。」）。

> 注：`n2_kitchen` 的三个选项目标**全部声明 `redeemable: true`**，这是用例 B 能成立的前提（`PendingPool::push` 只接受作者声明可挽回的兄弟节点）。

### 5.5 剧本提纲（台词由工程师阶段落地，此处给出骨架与关键样例）

**字数/句数配额**：合计约 1000 字、61~65 句（含挽回变体 4 句），落在 50~70 区间内。

> **⚠️ v1.5 实测标注（M-15）：`redeem_variant` 当前引擎不支持**
> software-engineer 核 `NodeModel.hpp` / `StoryLoader.hpp` / `NarrativeEngine.hpp` 后确认：
> - `NodeSpec` **没有** `redeem_variant` 字段（只有 `redeemable` / `redeem_gate` / `redeem_expire`）
> - `StoryLoader` 的 `presentation` **只解析** `narrator` / `npc_text` / `npc_state` / `hint` 四个平铺字段
> - `NarrativeEngine` 挽回时**只写** `tr.redeemed` 与 `tr.events`，**没有切换台词的逻辑**
>
> **当前处置**：这 4 句仍写进 `n3_tender.redeem_variant`，但**是前向兼容占位、运行期不生效**（`StoryLoader` 对未知节点字段静默忽略，不会报错）。
> **性质判定（产品口径）**：这不是可裁剪的锦上添花 —— **"不回放旧文本、在过去的时间语境里用现在的口径重讲一遍"正是 PendingPool 设计文档里 C2 硬约束的核心**，
> 也是"事件挽回"区别于"读档重来"的唯一特征。若不做，玩家只会看到一段似曾相识的旧台词，挽回的情感价值归零。
>
> ✅ **v1.6：team-lead 已裁定「补内核」，不降级**（理由同上，且明确写出：降级的话甲方看到的就是"回到同一节点但台词一字不差"，跟重开没区别）。
> ✅ **架构师已裁定归属：`redeem_variant` 三件套（字段 + loader 解析 + 挽回时切换）归 T02，不推 T03/T04** ——
> 因为 B-5 / A-1 / A-2 / A-3 / B-6 都是 T02 的验收项，**内核不补就没断言可写**。
>
> **落地要求（本轮必做，R-10 已由 P1 升 P0）**：`NodeSpec` 增 `redeem_variant`；`StoryLoader` 解析该字段；`NarrativeEngine` 在 `PendingPool` 唤醒时**优先取用** `redeem_variant`（未提供则回落 `presentation` 并打日志告警）。
>
> 🚫 **"引擎补齐前 B-5 降级为非阻塞项"的临时口径已作废（v1.7）**：B-5 恢复为**阻塞项**，与 B-1~B-4/B-6~B-8 同级，不通过即判用例 B 不通过。
>
> 📌 **产品侧口径（供工程师写断言时对齐）**：本条要证明的不是"能回到旧节点"，而是"**回到旧节点时，NPC 用的是现在的口径重讲了一遍**"——这两件事在玩家眼里完全不同，前者等于读档重来。

| 节点 | 句数配额 | 骨架（旁白 1 句 + 台词 2~4 句） |
|---|---|---|
| `n1_entry` | 6 | 旁白：周末傍晚，你推开家门，厨房里飘出香味，木子因系着围裙背对着你切菜。／她：「哥哥，你回来啦。今天大学那边累不累？晚饭马上就好。」 |
| `n2_kitchen` | 10 | 旁白：你走过去，她切菜的动作停了，**左手死死捂住围裙口袋**。／她：「哥哥……你今天好像心不在焉的。是有什么心事吗？」／提示：岔路口 |
| `n3_tender` | 8 | 旁白：听到你坦白，她放下菜刀，轻轻抱住你。／她：「没关系的，哥哥。无论发生什么，我都会一直陪着你。」 |
| `n3_tender` **挽回变体** | +4 | 旁白：她没有回头，但握刀的手停了很久。／她：「……你现在才肯说啊。笨蛋哥哥，我最讨厌你逞强了。」 |
| `n4_shut` | 8 | 旁白：她愣住了，眼眶瞬间红了。／她：「我……我只是关心你而已，你干嘛凶我……」 |
| `n5_search` | 8 | 旁白：你抓住她的手腕，从口袋里掏出一枚沾着暗红血迹的旧发卡。／她：「啊！还给我……哥哥，那个不能看！求你了！」 |
| `n6_silence` | 6 | 旁白：厨房里只剩刀落在砧板上的声音，她始终没有回头。窗外一寸寸暗下去。／她：「……」（她把菜盛进盘子，指尖微微发抖） |
| `n7_goodnight` | 6 | 旁白：你道了晚安，她盯着你的背影看了很久。／她：「嗯……晚安，哥哥。」 |
| 3 个结局 | 各 3 | 见 §5.6 |

### 5.6 结局规格表（3 个结局）

| # | 结局 ID | 类型 | 达成条件 `requires` | `priority` | 区分度说明（玩家能感知到的差异） |
|---|---|---|---|---|---|
| 1 | `end_calm` | **普通结局**（温暖/日常）—— 甲方原话明确要求 | **`{all: [{phase_in: ["night","late_night"]}, {affinity_gte: 82}]}`** | 60 | 她道了晚安，一切如常。**玩家始终不知道她口袋里藏着什么** —— 悬念被完整地保留到下一周目。文本示例：旁白「这一页就这样翻过去了。她有没有睡着，你并不知道。」／她：「嗯……明天也要早点回来。」 |
| 2 | `end_hidden` | **隐藏结局**（病娇/真相）—— 甲方原话明确要求 | `{item: "item_hairclip"}` | **100（最高）** | 伪装被彻底撕开，人设反转。文本示例：旁白「你举起那枚带血的发卡，她的表情一点点空了下去。」／她：「（眼神变得空洞）既然哥哥发现了……那我也没必要装成乖乖女了吧？」**只有走 `n5_search` 拿到发卡才能见到，普通线 100% 看不到** |
| 3 | `end_regret` | **遗憾/失败向结局** —— ✅ **甲方已于 2026-10-30 确认（Q-1 已闭环，不再是假设）** | **`{all: [{any: [{tag: "regret"}, {step_gte: 9}]}, {affinity_lte: 81}]}`** | 90 | 话说得太晚，错过的那条路最终没能补回来。文本示例：旁白「天亮之前，厨房里再没有响起过任何声音。」／她：「……」（她把最后一个盘子收进柜子，没有回头）**与 `end_calm` 的差别不是"坏结局 vs 好结局"，而是"和解了 vs 和解的机会已经过期了"** |

> **⚠️ v1.1 修订 M-3（架构师修订3）：阈值由 v1.0 的 83/82 → `end_calm ≥ 82` / `end_regret ≤ 81`。**
> **我在 v1.0 §5.6 末尾建议的"86 / 80 留 6 点缓冲"是错的，正式撤回。** 它会导致"拒绝线·挽回成功"（最终 aff = 90−10−3+5+2 = **84**）
> 既不满足 `end_calm ≥ 86`、也不满足 `end_regret ≤ 80`、又没有发卡 → **三个结局全部不可达 → 死锁**。
> 而"挽回成功 → 结局从遗憾翻转为和解"恰恰是**事件挽回唯一的可见价值证明**，我的建议会亲手毁掉它。架构师的 82/81 是对的：
> ① 挽回线 84 vs 82 有 2 点缓冲，不挽回线 79 vs 81 也有 2 点；② 82/81 在整数轴上**互斥且穷尽**，没有 82/83 紧贴 1 点的抖动；
> ③ `end_regret` 新增的 `any[{tag:"regret"},{step_gte:9}]` 是**收束兜底**，玩家拖到 seq≥9 也能结束，不会卡死；
> ④ `affinity_lte:81` 保证 aff≥82 时绝不会误落 `end_regret`，坦白线（94）不会被 priority 90 反杀。

#### 三个结局的互斥性验算（用 §5.3 的 effects 反推）

| 走法 | 关键节点与 effects | 最终 `aff` | tags / items | 命中的结局 | 为什么 |
|---|---|---|---|---|---|
| **坦白线**（基线 A0） | `n3_tender`(+5) → `n6_silence`(−3) → `n7_goodnight`(+2) | **94** | trust, regret | **`end_calm`** | 94 ≥ **82** 且已入夜 ✓；无发卡 → `end_hidden` 淘汰；94 > **81** → `end_regret` 淘汰 |
| **拒绝线且不挽回**（对照组 B0） | `n4_shut`(−10) → `n6_silence`(−3) → `n7_goodnight`(+2) | **79** | distant, regret | **`end_regret`** | 79 ≤ **81** 且带 regret ✓；无发卡 → `end_hidden` 淘汰；79 < **82** → `end_calm` 淘汰 |
| **拒绝线但成功挽回**（用例 B） | `n4_shut`(−10) → `n6_silence`(−3) → **`n3_tender` 被挽回**(+5) → `n7_goodnight`(+2) | **84** | distant, regret, trust | **`end_calm`** | 84 ≥ **82** ✓ —— **这正是"事件挽回"的可见价值：挽回成功 → 结局从遗憾翻转为和解**。⚠️ 本行成立**依赖 B-8**（第 5 步必须走到 `n7_goodnight` 而不是被 `n5_search` 抢走），否则会掉进 `end_hidden` |
| **搜身线**（用例 A） | `n5_search`(−15) → `n7_goodnight`(+2) | **77** | discovered, **item_hairclip** | **`end_hidden`** | 有发卡 ✓，priority 100 压过 `end_regret`（90） |
| **搜身后又沉默**（混合线） | `n5_search`(−15) → `n6_silence`(−3) → `n7_goodnight`(+2) | **74** | discovered, regret, hairclip | **`end_hidden`** | `end_hidden`(100) 与 `end_regret`(90) 条件同时满足 → **priority 高者胜出** → 真相优先于遗憾，符合叙事直觉 |

> 结论：三个结局在"达成条件"与"文本观感"上均两两互斥，且**每一条线都能稳定落到唯一一个结局**（不存在同时命中两个的歧义）。
>
> **v1.1 已按架构师的 82/81 定稿**（v1.0 末尾"建议改 86/80"的段落已删除并撤回，理由见上方修订框）。
> 另附一条给工程师的**边界提醒**：`bucket_of()`（`NodeModel.hpp:35-44`）中 **Ending = 40 > Scripted = 30**，
> 即**任一结局一旦 feasible 就会无条件排在所有剧情节点之上**。因此必须保证结局的 `requires` 足够紧：
> - `end_calm` 靠 `phase_in night` 把守 → 只有 `n6_silence` 推进入夜后才可能 feasible（用例 A 在 seq=2 时 phase 仍为 evening，安全）；
> - `end_hidden` 靠 `item_hairclip` 把守 → 只有走 `n5_search` 才可能 feasible；
> - `end_regret` 靠 `aff ≤ 81` 把守 → 坦白线（94）安全。
> 建议在仿真里补一条断言：**"在 `seq < 3` 时，三个 ending 均 ∉ frontier_in（用例 A 的搜身线除外，其 `end_hidden` 自 seq=2 起合法 feasible）"**。

### 5.7 防死锁与兜底（v1.1 新增）

| 项 | 规定 |
|---|---|
| `n_idle` | **必须存在**，`kind: "fallback"`，`once: false`，`cooldown: 1`，`priority: 10`，`intent.must_freeform: true`，`keywords: []`。`StoryLoader.hpp:207` lint 强制 |
| `fallback_next` | 各节点**可选**填写（架构师指定 `n7_goodnight.fallback_next = "end_regret"`）。⚠️ 但 **`Frontier::fallbacks()` 只认 `kind: "fallback"` 的节点**，`fallback_next` 对外层跳转兜底有效、**对防死锁无效**，不能替代 `n_idle` |
| 死锁路径 | Frontier 为空 → 注入 `n_idle`；连 `n_idle` 都没有 → `throw NxError(Deadlock)`。**死锁不是靠运气避免的，是靠数据结构保证的** |
| R-05 连带 | 若 `n_idle` 被计入"7 个对话节点"，则剧情节点只能有 6 个 → 与甲方"7 个对话节点"的硬要求冲突。**已列入 §10 Q-9 待甲方确认** |

---

## 6. 指针测试用例（重头戏）

> 两个用例都必须在 **`python reference/simulate.py` 的离线仿真里先跑通**，再要求 UI 复现。
> 断言一律写成代码断言，**不允许"肉眼看日志觉得对"**。
> 符号约定：`seq` = `GameState::seq`（time_cursor），`aff` = 好感度。

### 6.0.0 通用纪律（v1.2 新增，架构师偏差 6 提炼，工程师必须遵守）

1. **每一步的玩家输入必须写死进测试脚本，禁止用「继续」「……」「嗯」这类中性文本推进剧情。**
   离线档没有 `default_next` 兜底（M-9），中性输入会把路由交给 `priority + bucket + 打分` 决定 —— 而 `Ending bucket=40` 一旦 feasible 就会压过所有剧情节点，结果不可控。
2. **剧本的"主线怎么走"由 `priority + bucket + L1 打分` 决定，不要指望 `default_next`。** `default_next` 只在 Frontier 空集时生效，而 `n_idle` 保证它永不为空。
3. **凡涉及"某个节点不在 Frontier 里"的断言，一律写 `∉ frontier_in`，禁止断言具体 BLOCK 原因字符串**（M-1 一改就红）。
4. **每新增/修改一个节点的 keywords，必须重跑 A-6 / A-7 / B-2b / B-8 四条收敛性断言** —— 这些是"路由没被抢走"的哨兵。
5. **调 keywords 前先确认这一步走 L0 还是 L1**（v1.3 新增，M-13）：两套算法对"加词"的反应不同，见 §7.1.2 规则 14。**离线档全程走 L1，故本 PRD 所有断言数值均按 L1 计算，成立。**

### 6.0 前置：对照组（两条基线的期望轨迹）

**基线 A0（坦白线，正常节奏）** —— 用于对照"提前"这件事真的发生了。

| seq | 节点 | **输入（v1.2 起写死，禁止用「继续」等中性文本）** | 命中依据 | aff | tags | phase |
|---|---|---|---|---|---|---|
| 0→1 | `n1_entry` → `n2_kitchen` | （点选项 ①） | L0 `match_option` | 90 | — | evening |
| 1→2 | `n2_kitchen` → `n3_tender` | 「对不起，我今天真的有点累」 | L1 命中 `累`/`对不起` | 95 | trust | evening |
| 2→3 | `n3_tender` → `n6_silence` | （点选项 ① 抱紧她） | L0 `match_option` | 92 | trust, regret | **night** |
| 3→4 | `n6_silence` → `n7_goodnight` | **「该睡了，晚安。」** | L1 命中 `晚安`+`睡了` | 94 | +regret | night |
| 4→5 | `n7_goodnight` → `end_calm` | **「嗯，回头见。」** | L1 命中 `end_calm` 的 `回头见` | 94 | — | night |

> **⚠️ v1.2 修正（架构师偏差 6 后果 a）**：第 4 步的输入**必须**写死为「该睡了，晚安。」，不能用「（沉默）」「继续」这类中性文本。
> 原因：`n6_silence` 在 seq=3 已把 phase 推到 night，此时 `end_calm`（`phase_in night` ✓ + `aff=92 ≥ 82` ✓）**已经 feasible**，
> 且 `bucket_of(Ending)=40 > Scripted=30`，`end_calm` 的 Frontier 先验分（≈0.95×0.15=0.143）会压过 `n7_goodnight`（≈0.85×0.15=0.128）。
> 中性输入下 kw/sim 全为 0，L1 退化为纯先验比较 → **`end_calm` 险胜，直接跳过 `n7_goodnight` 抵达结局** →
> 基线从 5 步变 4 步，**A-3 的对照组（"n7 首次进 Frontier 是 seq=3"）失去意义，A-4 的步数对比也失效**。
> 写死输入后 `n7_goodnight` 的 kw = 2/4 = 0.5 → 0.275 + 0.128 = **0.403**，稳胜 `end_calm` 的 0.143。

**基线 A0 的关键断言**：
- `n7_goodnight` **第一次进入 Frontier 的时刻是 seq=3**（`n6_silence` 的 `set_phase: night` 解锁）；
- **A-7（v1.2 新增）**：第 4 步 `to_node == "n7_goodnight"`，且**抵达 ending 时 `seq == 5`**（锁死"基线 5 步"这个对照组基准）。

---

### 6.1 用例 A：前向指针 —— 未来节点被**提前**拉进可达集合

#### 语义说明

`n7_goodnight` 是作者笔顺上的**最后一个对话节点**（收束位），按设计意图应当"入夜之后"才出现。
用例 A 证明：当玩家在极早期就把关系打崩（好感度跌破 85），**这个时间线靠后的节点会无视"尚未入夜"的 phase 门槛，被提前拉进可达集合**，整局节奏被压缩。

#### 触发前提

- 起始 `aff = 90`，`phase = evening`，`seq = 0`，`current = n1_entry`
- `n7_goodnight.requires = {any: [{phase_in: ["night","late_night"]}, {affinity_lte: 85}]}`，且 **`phase.allow` 全开**（M-1，否则 requires 里的 affinity 分支永不见天日）
- `n5_search.effects.affinity_delta = -15`（唯一的掉分手段），且 **`n5_search.requires = {any: [{step_gte: 1}, {tag: "suspicious"}]}`**（M-2，必须是 1 不是 2，否则第 2 步就走不起来）

#### 玩家输入（逐步）

| 步 | seq（进入前） | 玩家输入 | 期望命中 | 期望跳转 |
|---|---|---|---|---|
| 1 | 0 | （点选项 ① 走过去打招呼） | L0 选项 | `n1_entry` → `n2_kitchen`，seq=1 |
| 2 | 1 | **「你口袋里藏了什么？」**（v1.6 改，原为「你口袋里藏的是**什么**？」——见下方说明） | L1（`n5_search` 的 keywords：`口袋`/`藏`/`发卡`/`看看`） | `n2_kitchen` → `n5_search`，seq=2，aff **75**，+`discovered`，+`item_hairclip` |
| 3 | 2 | **「这发卡上面的血是谁的？」** | L1/L2 | `n5_search` → `end_hidden`（`requires: {item: item_hairclip}` 已满足） |

> **注意：第 3 步跳过了 `n6_silence` 和 `n7_goodnight` 本身在实际路径中被跳过**；关键是 —— 在 **seq=2 结束时（即 `n5_search` 的 effects 刚生效）**，`n7_goodnight` 就已经出现在 Frontier 里了。

> **⚠️ v1.6 修订（M-17，架构师裁定 + 我方补充）：第 2 步输入由「你口袋里藏的是**什么**？」改为「你口袋里藏了**什么**？」**
>
> **原因**：`contains_count` 是**子串匹配**。「你口袋里藏的是**什么**」**不含**子串「藏了什么」→ `n5_search` 的 4 个 keyword 里有 2 个永远命中不了，**白占归一化分母**，kw 只有 0.25。
> 改为「你口袋里藏了什么？」后与 `n5_search.examples[0]` 完全一致 → 命中 `口袋` + `藏了什么` = 2/4 → **kw 0.25 → 0.5，kw 项 0.1375 → 0.275**。
>
> **架构师的裁定原则（我方认同）**：**改测试输入，不改 keywords** —— 测试输入是脚本常量，keywords 是作者资产，不该迁就一句输入去改作者资产（那正是 R-12 lint 要抓的 keywords/examples 漂移）。
>
> **但我方同时保留 M-16 的 keyword 优化（`藏了什么` → `藏`），理由与架构师的原则并不冲突：**
> - 优化的目的**不是**迁就那句测试输入，而是让**真实玩家的多变说法**都能命中（`藏` 能命中"藏了 / 藏的 / 藏着 / 藏什么"）；
> - 它**仍然满足架构师新定的规则 17**：`examples[0]` = 「你口袋里藏了什么？」→ 命中 `口袋` + `藏` = **2 个** ✓，lint 通过；
> - 两者叠加后鲁棒性最高：无论玩家说「藏了什么」还是「藏的是什么」，都能命中 2 个。
>
> ✅ **v1.7 收敛裁定（M-19，team-lead）：两条都要，不是二选一 —— 本段"请架构师二选一"的悬置作废。**
>
> | 层 | 采纳谁 | 落地值 | 理由 |
> |---|---|---|---|
> | **keywords 编法** | 我方 M-16 | **`口袋` / `藏` / `发卡` / `看看`**（短而语义特异的子串） | 单字/短词泛化更好；整句型词组只对一句生效，太脆弱 |
> | **测试输入** | 架构师规则 | 写死为 `n5_search.examples[0]` = 「你口袋里藏了什么？」 | 测试输入是脚本常量，keywords 是作者资产，**方向不能反** |
> | **一致性兜底** | 双方各提一条，合并为同一条 lint | 每个节点 keywords **至少 2 个能被自身 `examples[0]` 同时命中**（规则 17 / R-12 ④） | 命中 <2 即"白占归一化分母"，L1 摊薄、L0 拿不到直连 |
>
> **合并后验算**：keywords `[口袋, 藏, 发卡, 看看]` + 输入「你口袋里藏了什么？」→ 命中 `口袋` ✓ + `藏` ✓ = **2 个** → L1 `kw = 0.5`（kw 项 0.1375 → 0.275）；联机档 hits=2 ≥ `l0_confident` 门槛亦可触发 **L0 直达（0 token）**。
>
> 🚫 **落地纪律**：**PRD 只定义规则，不落地文件**。`story_v2.json` 的最终 keywords 由 **software-engineer 实测确认后落地**，并把**实际命中数与 margin** 报给 team-lead。我方与架构师**不再各自去改 JSON**。
> 我方在产品侧核对时只按上表规则复核（命中数 ≥2、kw ≥ 0.5、margin 为正），不介入取值。

#### 期望跳转路径（最短完整路径）

```
n1_entry (seq1) → n2_kitchen (seq2) → n5_search (seq3, aff 75) → end_hidden (seq4)
```

对照基线 A0：`end_calm` 要 seq=5 才到达。
**用例 A 用 4 步收束，比基线快 1 步；且 `n7_goodnight` 的解锁时点从 seq=3 提前到 seq=2，提前量 = 1 seq 步。**

> 若架构师希望提前量更显著，可把 `n7_goodnight` 的 affinity 阈值放宽到 `affinity_lte: 88`，则 `n4_shut`（−10 → aff 80）也能触发，路径变为 `n1_entry → n2_kitchen → n4_shut(seq2, aff 80) → n7_goodnight(seq3) → end_regret(seq4)`，提前量同样是 1 步。**提前量取决于图的深度，7 节点图的理论上限就是 1~2 步**；若甲方要求 ≥3 步的提前量，需增加中间节点（列入 §10 待确认）。

#### 如何验证（v1.1 为 6 条断言）

| # | 断言 | 判定方式 |
|---|---|---|
| **A-1** | 在 `seq = 2`、`phase == "evening"` 的时刻，`n7_goodnight` **在 Frontier 的 `IN` 列表里** | `engine.debug_frontier()` 输出含 `IN n7_goodnight`，且同一行 `phase=evening` |
| **A-2（v1.1 改）** | 断言它**不是**靠 phase 进来的 | 同一时刻 `st.phase == "evening"` 且 `st.phase_index == 0`；且 `n7_goodnight` **不在 `view.blocked["phase"]` 列表里**。⚠️ phase.allow 全开后不会再输出 `BLOCK[phase]`，本条改为"不在 blocked[phase] 中" |
| **A-3（v1.1 固化）** | 对照组在相同时刻**不可达** | 基线 A0 在 `seq=2`（aff=95、phase=evening）时 **`n7_goodnight ∉ frontier_in`**。⚠️ 其 BLOCK 原因已由 `phase` 变为 `requires`，**断言一律写成"∉ frontier_in"，禁止断言具体 BLOCK 原因字符串**（否则 M-1 一改就红） |
| **A-4** | 整局步数被压缩 | 用例 A 抵达任一 ending 的 `seq <= 4`；基线 A0 为 `seq == 5` |
| **A-5** | 单调性未被破坏 | 全程 `seq` 严格递增 0→1→2→3→4；`phase_index` 只增不减 |
| **A-6（v1.1 新增）** | `n5_search` 在 `seq=1` 时 feasible | `step_gte:1` 生效，第 2 步能真正命中 `n5_search`（防 M-2 回归；若写成 2 则此处直接红） |
| **A-7（v1.2 新增，对应架构师偏差 6a）** | **基线 A0 不被 `end_calm` 抢跑** | 基线第 4 步输入「该睡了，晚安。」→ 断言 `to_node == "n7_goodnight"`；断言**抵达 ending 时 `seq == 5`**（锁死对照组基准，否则 A-3/A-4 全部失去意义） |

#### 一条命令复现（离线，无需 UI/Key）

```bash
python reference/simulate.py --case forward_pointer   # 期望打印 A-1..A-6 全部 PASS
```

---

### 6.2 用例 B：后向指针 —— 过去错过的节点被**重新拉回**

#### 语义说明

`n2_kitchen` 是三岔口。玩家选了"拒绝沟通"（`n4_shut`），**"坦白示弱"（`n3_tender`）当场被错过**，按传统 Galgame 的规则它就永久消失了。
用例 B 证明：当玩家后来表现出悔意（拿到 `regret` 标签），引擎会**把 `n3_tender` 重新拉回可达集合**——但**不回放旧文本、不回退时间游标**，而是让它在"现在"的语境里被重新讲一遍。

#### 触发前提

- `n3_tender` 声明 `redeemable: true`、`redeem_expire: 10`、`redeem_gate: {tag: "regret"}`，**`phase.allow` 全开**（M-1）
- `n6_silence.effects.add_tags = ["regret"]`（**这是唤醒开关的唯一来源**）
- `PendingPool::push` 只接受 `redeemable` 的兄弟节点（`n2_kitchen` 的三个选项目标都满足）
- 硬约束 C1：`awake_seq > birth_seq` 且 `awake_seq == 当前 seq`；`birth_seq` **永不被改写**
- **`n6_silence` 必须 `once: false, cooldown: 2`**（M-6），否则第 5 步之后指回 `n6_silence` 会被 `BLOCK[consumed]` 打断
- **`n6_silence` 不得保留直连 `n3_tender` 的选项按钮**（M-8 / 架构师 AR-9），否则玩家能用 L0 按钮绕过 PendingPool

#### 玩家输入（逐步）

| 步 | seq（进入前） | **玩家输入（v1.2 起写死）** | 命中依据 | 结果 |
|---|---|---|---|---|
| 1 | 0 | （点选项 ① 走过去打招呼） | L0 `match_option` | → `n2_kitchen`，seq=1 |
| 2 | 1 | **「没什么，你别管了。」** | L1 命中 `n4_shut`（`别管`/`不用你管`/`没有`） | → `n4_shut`，seq=2，aff **80**，+`distant`；**`pending <- n3_tender`（birth_seq=1）**、`pending <- n5_search`（birth_seq=1） |
| 3 | 2 | **「（沉默）」** | L1 命中 `n6_silence`（`沉默`）⚠️ **不是** `default_next`（见 M-9） | → `n6_silence`，seq=3，aff **77**，**+`regret`** |
| 4 | 3 | （进入 `n6_silence` 后，引擎在 step 6 执行 `PendingPool::scan`） | `redeem_gate{tag:regret}` 满足 | 事件日志：`redeem n3_tender (born@1 -> awake@3)`（**同时** `redeem n5_search`） |
| 5 | 3 | **「其实……我今天真的很累。」** | L1 命中 `n3_tender`（`累`），且带 `pending_age` 加权 | → **`n3_tender`**（回到过去错过的那条路），seq=4，aff **82**，+`trust` |
| **6** | **4** | **「那……晚安吧。」**（v1.2 新增，必须写死） | L1 命中 `n7_goodnight`（`晚安`） | → **`n7_goodnight`**，seq=5，aff **84** → 再一步 → **`end_calm`** |

> **⚠️ v1.2 新增第 6 步 + 修正（架构师偏差 6 后果 b）**：走到 `n3_tender`(seq=4) 之后，**绝不能**用中性输入推进。
> 此时 `n5_search` 是 `n3_tender` 选项②的邻居 → 仍在候选里，`priority 78 > n7 的 50`，还带着唤醒后的 `pending_age` 加分，
> 先验项就有 `0.15 × 1.31 ≈ 0.197`；而 `n7_goodnight` 只有 `0.15 × 0.85 ≈ 0.128`。
> 中性输入下 **`n5_search` 抢走路由** → `+item_hairclip`、`aff 82−15 = 67` → `end_hidden`（bucket 40 无条件压过一切）
> → **"挽回成功 → end_calm"这个唯一的价值证明彻底没了**。
> 写死「那……晚安吧。」后 `n7` 的 kw = 1/4 = 0.25 → `0.1375 + 0.128 = 0.265` > `n5_search` 的 `0.197` ✓
> （这也是 M-10 反对扩增 n7 keywords 的原因——扩到 7 个词后 kw 掉到 1/7，只剩 0.206，安全边际只剩 0.009）。
>
> **⚠️ v1.2 终裁加强（M-10）**：再给 `n5_search.negatives` 加 `晚安`/`睡` →
> 该输入下 `n5_search` = `0.17 − 0.5 = −0.33`，**领先从 0.095 扩大到约 0.6（高一个量级）**。
> B-8 从"稳过"升级为"绝对不会翻"。

#### 期望跳转路径

```
n1_entry(1) → n2_kitchen(2) → n4_shut(3, aff80, +distant)
   → n6_silence(4, aff77, +regret) ──[redeem: n3_tender born@1 → awake@3]──
   → n3_tender(5, aff82, +trust) → n7_goodnight(6, aff84) → end_calm(7)
```

#### 对照组 B0（证明"错过"确实会发生）

把第 5 步改成「算了，当我没说」→ 不命中 `n3_tender`，继续沉默直到 `seq - birth_seq > 10` → `n3_tender` **出池销毁**，此后**永远无法再进入** `n3_tender`。
**期望**：对照组 B0 最终落在 `end_regret`（aff 79 ≤ 81 且带 `regret`），而用例 B 因挽回成功 `aff` 回升至 84 ≥ 82 → 落在 `end_calm`。
**这条对照才是"事件挽回"的价值证明：同一段剧情，挽回与不挽回，结局不同。**

#### 如何验证（v1.2 为 8 条断言）

| # | 断言 | 判定方式 |
|---|---|---|
| **B-1** | 错过即入池 | step 2 的 `tr.events` 含 `pending <- n3_tender`；且 `st.pending` 中该条目 `birth_seq == 1`、`awake == false` |
| **B-2（v1.1 改）** | 唤醒真的发生 | step 4 的 `tr.events` **【含】** `redeem n3_tender (born@1 -> awake@3)`，`tr.redeemed` 含 `n3_tender`。⚠️ **必须写成"含"而不是"仅含"**：`n2_kitchen` 有 3 个 options，选了 `n4_shut` 后 `PendingPool::push`（`NarrativeEngine.hpp:136-144`）会把**另外两条都**压入池，而 `n3_tender` 与 `n5_search` 的 `redeem_gate` **同为 `{tag:"regret"}`** → **`seq=3` 拿到 regret 时两条一起被唤醒**。写"仅含"必然红 |
| **B-2b（v1.1 新增）** | 唤醒后能被正确收敛 | step 5 输入「其实……我今天真的很累。」→ L1 把 `n3_tender`（keywords `累/撑不住/压力大` 命中）排在 `n5_search`（keywords `口袋/发卡/藏了什么`，不命中）之前；断言 `tr.to_node == "n3_tender"` |
| **B-3** | **C1 单调唤醒** | 断言 `awake_seq(3) > birth_seq(1)`；断言 `awake_seq == st_.seq`；断言唤醒后 `entry.birth_seq` **仍为 1**（未被改写） |
| **B-4** | **时间游标不回退** | 全程 `seq` 序列 `0,1,2,3,4` 严格递增；断言 `st.last_visit["n3_tender"] == 4`（不是 1） |
| **B-5（v1.1 加固；v1.6 曾临时降级；✅ **v1.7 起恢复为阻塞项**）** | **不回放旧文本** | 被挽回的 `n3_tender` 呈现的是 `redeem_variant` 台词（「……你现在才肯说啊。笨蛋哥哥，我最讨厌你逞强了。」），**与首次访问的 `presentation` 文本不相等**。<br>🔴 **本条为阻塞项，不可降级**。v1.6 曾因引擎未支持而暂按"只断言 B-2/B-3"计——**该临时口径 v1.7 起作废**（team-lead 裁定：降级的话甲方看到的就是"回到同一节点但台词一字不差"，跟读档重开没区别，本次最想验证的后向指针情感价值归零）。<br>**前置（T02 本轮必做，R-10）**：`NodeSpec` 增 `redeem_variant` 字段 + `StoryLoader` 解析 + `NarrativeEngine` 在 PendingPool 唤醒时优先取用（未提供则回落 `presentation` 并打告警日志）。<br>⚠️ 另依赖 **M-1**：若 `n3_tender.phase.allow` 仍限 `evening`，`seq=4`（phase=night）时会被 `BLOCK[phase]` 挡住，**永远救不回来** |
| **B-6** | **对照组不可挽回** | B0 路径下，引擎运行到 `seq > 11` 后，`n3_tender` 不再出现在任何 Frontier 的 IN 列表；且 `st.pending` 中该条目已被移除 |
| **B-7（v1.1 新增）** | 路径不断裂（防 M-6 回归） | 用例 B 走到 `n3_tender`(seq=4) 之后，其 `default_next = n7_goodnight` 可达（不被 `once` 消耗挡住）；断言 `seq=5` 时 `frontier_in` 非空 |
| **B-8（v1.2 新增，对应架构师偏差 6b）** | **挽回线不被 `n5_search` 抢走** | 第 6 步输入「那……晚安吧。」→ 断言 `to_node == "n7_goodnight"`；断言全程 **未获得 `item_hairclip`**；断言最终 `to_node == "end_calm"` 且 `affinity == 84` |

#### 一条命令复现（离线，无需 UI/Key）

```bash
python reference/simulate.py --case backward_pointer   # 期望 B-1..B-6 全部 PASS
python reference/simulate.py --case backward_pointer_neg  # 对照组 B0，期望 B-6 PASS
```

#### UI 侧的补充观察项（人工，非断言）

- 玩家能明显感到"她重新提起了刚才没接上的那句话"，而不是"画面突然跳回两分钟前"。
- 界面上**没有任何**时间倒流提示（无"回到上一步"、无进度条倒退）。

---

## 7. `intent.description` 写作规范

> **这份描述的质量 = AI 选路准确率的天花板。** 写得含糊，换什么模型都救不回来。

### 7.1 硬规则（lint 会拦，违反则剧本加载失败）

| # | 规则 | 反例 → 正例 |
|---|---|---|
| 1 | **一句话，25~45 字**，陈述"玩家在什么语境下、表达了什么意图" | ✗「玩家心情不好时可能说的话，或者也可能是在转移话题，要看情况」 |
| 2 | **必须含一个可判别的动作动词**（示弱 / 拒绝 / 追问 / 道歉 / 告别 / 指责 / 安抚） | ✗「玩家表现出负面情绪」→ ✓「玩家拒绝沟通，表现出防御、疏远或不耐烦」 |
| 3 | **描述之间两两不重叠**：任意两条的相似度 ≥ 0.85 判为冲突，必须改写 | — |
| 4 | **必须配 `keywords` 3~5 个**，且是玩家原话里真会出现的口语词，**不用书面语** | ✗`["疲惫","沮丧"]` → ✓`["累","好困","撑不住"]` |
| 5 | **必须配 `examples` 2~3 条**，用真实玩家口吻，**带语气词** | ✗`"我今天很疲劳"` → ✓`"我今天真的快撑不住了……"` |
| 6 | **必须配 `negatives` 1~3 条**，写"最容易被误判成自己"的邻节点说法 | `n3_tender.negatives: ["不用你管"]`；`n4_shut.negatives: ["对不起"]` |
| 7 | **禁止出现节点 ID、选项编号、引擎术语**（Frontier/PendingPool/tag/affinity） | ✗「当 affinity 下降时走 n4_shut」 |
| 8 | **结局节点的描述必须以终局语义开头**（"结束本次演出"/"今夜到此为止"），避免中途被命中 | ✓「今夜到此为止：哥哥准备结束对话、道别或离开。」 |
| 9 | **保底节点必须标 `must_freeform: true`**，且 keywords 为空数组 | `n_idle` |
| 10 | 描述中**不得包含会被 ASCII 清洗破坏的字符**（中文引号、破折号）——R-02 修复前必须遵守，修复后作为回归用例保留 | — |
| **11（v1.2 新增 M-10，v1.3 修订 M-13）** | **keywords 不是越多越好 —— 每加一个词都会摊薄已有词的命中率。目标是"小而准"：3~4 个为佳，且必须能被同一句玩家原话同时命中 ≥2 个。**<br>⚠️ **摊薄效应只在 L1 成立**；L0 用的是另一套公式（见规则 14）。但**结论不变**：离线档（`llm_ == nullptr`）走 L1，本节所有用例断言都按 L1 计算 | ✗ 从 `["晚安","睡了","困了","回房"]` 扩到 7 个 → 输入「那……晚安吧。」的 L1 kw 从 0.25 掉到 0.14，得分掉 43%，被 priority 78 的邻居反超；✓ 保持 4 个，让测试输入命中 2 个 |
| **14（v1.3 新增，M-13，工程师 software-engineer 发现）** | **引擎里有【两套】kw 算法，加词对它们的影响方向相反 —— 调 keywords 前必须先确认这一步走的是 L0 还是 L1。**<br>**L0**（`IntentMatch.hpp:111-117`）：`s = 0.5 + 0.25 × hits`，**只看命中个数的绝对值，与词表长度无关**；`hits ≥ keywords.size()` 时才加 `kw_full_bonus`。<br>**L1**（`IntentMatch.hpp:135-136`）：`kw = hits / keywords.size()`，**除以词表长度 → 加词摊薄**。 | 见 §7.1.2 的完整对照表 |
| **15（v1.5 新增，M-16，工程师 software-engineer 发现）** | **keywords 是【子串匹配】，不是分词匹配**（`contains_count` 用 `hay.find(n)`）。写长词组极易永远匹配不上，**白占归一化分母**。优先选**短而语义特异的子串**（1~2 字为佳，如 `藏` 能同时命中"藏了/藏着/藏的/藏什么"） | ✗ `藏了什么` —— 用例 A 写死的输入「你口袋里藏的是**什么**」**不含**这个子串，永远命中不了；✓ `藏` —— 命中且只占 1 个分母位 |
| **16（v1.5 新增，M-16）** | **每写完一组 keywords，必须拿测试脚本里写死的输入反查一遍命中数**。目标是**命中 ≥2 个**（既能让 L1 的 kw 达到 0.5，又能在联机档触发 L0 直达、0 token）。命中 0~1 个说明词表选错了 | 用 §6 写死的输入反查 `n5_search`：旧 4 词只命中 1 个（kw=0.25）；优化后命中 `口袋`+`藏` 2 个（kw=0.5，且 L0 可触发） |
| **17（v1.6 新增，架构师裁定 + 我方并入）** | **可执行的 lint 版本**：每个节点的 `keywords` 必须**至少 2 个能被该节点自身的 `examples[0]` 同时命中**（子串匹配）。命中 <2 即报"白占归一化分母" —— 它在 L1 是**纯负资产**（摊薄 kw），在 L0 也拿不到直连（需 `hits ≥ 2` 才 `s = 1.0`） | 本规则与规则 11 是同一原则的**可执行版本**，互补。`n5_search.examples[0]` = 「你口袋里藏了什么？」→ 需命中 ≥2 个 keyword |
| **18（v1.6 新增）** | **keywords 是【作者资产】，服务于真实玩家的多变说法，不是服务于测试脚本常量**。优化 keywords 的正当理由是"让真实玩家的各种说法都能命中"，**不是**"迁就某一句写死的测试输入"（后者正是架构师反对的漂移） | ✓ `藏` 能命中"藏了 / 藏的 / 藏着 / 藏什么" —— 对真实玩家鲁棒，且仍满足规则 17（`examples[0]` 命中 `口袋`+`藏` = 2 个）；✗ 为了让某一句测试输入命中而硬写一个只在那句里出现的长词组 |

### 7.1.2 两套 kw 算法对照（v1.3 新增，M-13）

> 来源：software-engineer 核 `IntentMatch.hpp` 时发现"L0 不受词表长度影响"。**这个发现本身成立且重要**，
> 但其附带的两条推论经我方复核**有误**，一并澄清如下。

| 项 | **L0**（`decide()` 内，`IntentMatch.hpp:111-117`） | **L1**（`IntentMatch.hpp:135-136`） |
|---|---|---|
| 公式 | `s = 0.5 + 0.25 × hits`（封顶 1.0） | `kw = hits / keywords.size()`（封顶 1.0） |
| **词表变长的影响** | **基础分不变**（不看分母） | **基础分被摊薄**（分母变大） |
| 全命中奖励 | `hits ≥ keywords.size()` 才给 `kw_full_bonus` | 无 |
| 触发条件 | `s ≥ l0_confident(0.86)` **且** 同 bucket 内无并列对手 | `llm_ == nullptr` 时**无条件**取 `scored[0]` |
| 生效范围 | **只在最高 bucket 的那一梯队内**（`IntentMatch.hpp:200-221`） | 全部 `cands` |
| 我们的用例走哪个 | 点选项按钮时**根本不进** `decide()`（见下） | **离线档全程走 L1** |

**⚠️ 需要澄清的两点（engineer 的两条推论方向反了）：**

1. **"点按钮（L0）推进"与 keywords 完全无关。**
   `NarrativeEngine::match_option()`（`NarrativeEngine.hpp:219-230`）**只接受全数字、长度 ≤3 的输入**，
   命中后直接返回 `options[i-1].next`，**根本不调用 `decide()`、也不看 keywords**。
   所以"点按钮"这一步，词表多长都影响不到 —— 不存在"加词让按钮更容易命中"这回事。
   真正用 keywords 的 L0 是 `decide()` 内部那一层，它处理的是**自由输入的自然语言**。

2. **加词会让 `kw_full_bonus` 更难拿，不是更容易。**
   条件是 `hits >= keywords.size()` —— 词表从 4 变 7，全命中门槛从"说中 4 个"涨到"说中 7 个"。方向是**更严**。

**但这两点不影响最终结论，反而强化了规则 11：**

- **L0 需要至少命中 2 个词才能触发**：`hits=1` → `0.5+0.25 = 0.75 < l0_confident(0.86)` → 不触发；
  `hits=2` → `1.0 ≥ 0.86` → 触发、直接路由且 **0 token**。
  → 这正好印证了规则 11 的"必须能被同一句原话**同时命中 ≥2 个**"：命中 2 个不仅 L1 拿满 kw，还能在 L0 层直接跳过 LLM。
- **离线档（`llm_ == nullptr`）全程走 L1**，所以 §6 全部断言、A-7/B-8 的边际计算**都按 L1 算，规则 11 与 M-10 终裁的数值全部有效**。
- 只有当未来做**联机档（有 LLM）**时，L0 才可能介入 —— 那时调 keywords 需按上表重新评估。

**给工程师的落地指令（不变）**：
- `n7_goodnight.keywords` = `["晚安","睡了","困了","回房"]`（4 个，不要扩）
- `n5_search.negatives` = `["算了","晚安","睡"]`
- 想让某句输入在**联机档**也能 0 token 直达 → 让它同时命中目标节点的 2 个 keywords
| **12（v1.2 新增，M-9）** | **节点的"下一步"不要指望 `default_next`**。离线档 `default_next` 是死代码，推进方向由 `priority + bucket + L1 打分` 决定。想让某条路更容易被走到，正确工具是 **`negatives`**（见规则 13），其次是 keywords，**最后才考虑 `priority`** | ✗「我在 `default_next` 里写了 n7，所以聊完会自然走到 n7」 |
| **13（v1.2 终裁，架构师裁定 + 我方确认）** | **想压制某个对手节点时，优先给【对手】加 `negatives`，而不是给自己加 keywords、更不要动 `priority`。**<br>理由：`neg` 项权重 **−0.5**，且**只在命中时触发**（精准）；`priority` 是**作者意图层的旋钮**，改动会**全局改变 Frontier 排序**、影响所有路径的每一步（钝器）；给自己加 keywords 则会被归一化摊薄（规则 11） | ✓ 想让「那……晚安吧。」走到 `n7_goodnight` 而不是被 `n5_search` 抢走 → **给 `n5_search.negatives` 加 `晚安`/`睡`**，而不是把 `n7` 的 priority 从 50 提到 62（那会越过 `n6_silence` 的 55，改坏所有路径） |

---

### ⭐ 关键公式：为什么 keywords 必须能被**同一句**原话同时命中 **≥2 个**

> **本节是 §7 最重要的一条，工程师写 keywords 时请先看这里。**
> 主理人 2026-10-30 指示：此条洞察需显眼呈现，不要埋在细则里。

**L0 的触发公式**（`IntentMatch.hpp:111-117`，阈值 `l0_confident = 0.86`）：

| 命中 keyword 个数 | L0 得分 `0.5 + 0.25 × hits` | 是否触发 L0 |
|---|---|---|
| **0** | 0（直接跳过） | ❌ 落到 L1 |
| **1** | **0.75** | ❌ **0.75 < 0.86 → 不触发**，落到 L1 或 L2 |
| **2** | **1.0** | ✅ **触发 → 直接路由，0 token、0 延迟** |
| 3+ | 1.0（封顶） | ✅ 触发 |

**三条直接结论：**

1. **一个节点的 keywords，如果找不到任何一句玩家原话能同时命中 2 个 —— 它永远走不了 L0**，每一轮都要付 LLM 的钱和延迟。
   → 写 keywords 时请自问："玩家会怎么一口气说出这句话？" 若答不上来，说明词表选错了（选了同义但不同现的词）。
2. **这也给了规则 11 一个公式级的依据**：`3~4 个为佳` 的上限不是拍脑袋——词表越长，同一句话能凑够 2 个词的可能性越低（L0 更难触发），且 L1 的 `hits / size` 被摊薄。
3. **命中 2 个的额外好处**：L0 触发后**完全不调用 LLM**，`IntentMatch.hpp` 的设计文档实测 40%~60% 轮次可被 L0/L1 吃掉 —— 既省钱省延迟，又**消除了这些轮次上的模型不确定性（利于可复现）**。

**⚠️ 两个限制条件（别误用）：**
- L0 **只在最高 bucket 的那一梯队内**找候选（`IntentMatch.hpp:200-221`）。若某 ending 已 feasible（bucket 40），则 L0 **只看 ending**，所有剧情节点的 keywords 在 L0 层被完全忽略 → 落到 L1。
- L0 触发还需 `l0_rivals == 0`（同 bucket 内无并列同分对手），否则仍落到 L1。
- **离线档（`llm_ == nullptr`）全程走 L1**（`decide()` L235 第一个条件短路），L0 只在**联机档**介入。所以本 PRD 的 A-1~A-7 / B-1~B-8 全部按 L1 计算，不受本节影响。
- **⚠️ 前提：keyword 必须真的能被命中**。匹配是**子串**（`hay.find(n)`），写长词组极易永远匹配不上（规则 15/16）。命中数不够 2 个，本节的一切好处都拿不到。

---

### 7.1.1 三层旋钮的选择顺序（v1.2 终裁，工程师按此顺序调参）

| 优先级 | 旋钮 | 作用范围 | 副作用 | 适用 |
|---|---|---|---|---|
| **1（最优先）** | **给对手加 `negatives`** | 仅在该词被说中时扣分 | **零全局副作用**；语义自洽（"说晚安"本来就是"翻口袋"的强反例） | 压制特定语境下的某个对手 |
| 2 | 加强自己的 `keywords` | 仅影响该节点自身 | 加词会被归一化摊薄（规则 11），**3~4 个为上限** | 想让自己更容易被命中 |
| 3 | 调自己的 `priority` | **全局 Frontier 排序** | ⚠️ 会影响**所有路径的每一步**，可能越过相邻节点 | 仅当上述两者都不够，且确认不会打乱作者意图的笔顺 |
| **4（无效）** | 写 `default_next` | — | **离线档是死代码**（M-9） | —— 不要用 |

### 7.2 本剧本 10 个节点的描述初稿（工程师落地时可微调，但必须通过 lint）

| 节点 | `intent.description` | `keywords` | `examples` | `negatives` |
|---|---|---|---|---|
| `n1_entry` | 开场：哥哥刚回到家，木子因在厨房做饭，用于承接任何开场寒暄。 | `回来了`,`好香`,`我饿` | 「我回来了」「好香啊，今天做什么」 | — |
| `n2_kitchen` | 厨房对话：她察觉哥哥心不在焉并主动关心，玩家含糊其辞或没有明确意图时落在这里。 | `没事`,`还好`,`没什么` | 「嗯，还好吧」「没什么特别的」 | `口袋`,`对不起` |
| `n3_tender` | 哥哥坦白自己很累、很脆弱，向木子因示弱求助。 | `累`,`撑不住`,`对不起`,`压力大` | 「我今天真的快撑不住了」「对不起，我最近压力很大」 | `不用你管`,`别管` |
| `n4_shut` | 哥哥拒绝沟通，表现出防御、疏远或不耐烦。 | `别管`,`不用你管`,`没有`,`烦` | 「没什么，你别管了」「不用你操心」 | `对不起`,`累` |
| `n5_search` | 哥哥要求检查她的口袋、拿走或追问她藏起来的东西。 | **`口袋`,`藏`,`发卡`,`看看`**（v1.5 优化，见下方 M-16） | 「你口袋里藏了什么」「把那个给我看看」 | **`算了`,`晚安`,`睡`**（`睡` 为 v1.2 新增，见下方 M-10 终裁） |
| `n6_silence` | 长时间的沉默、回避、不说话，或双方一时无话可说。 | `沉默`,`不说话`,`……` | 「……」「我什么都不想说」 | `晚安`,`对不起` |
| `n7_goodnight` | 哥哥准备去睡觉、结束今晚的对话。 | `晚安`,`睡了`,`困了`,`回房` | 「我先去睡了」「晚安」 | `等一下` |
| `end_calm` | 今夜到此为止：在彼此都没有受伤的氛围里结束今晚。 | `结束`,`回头见` | 「那我回房了」 | — |
| `end_hidden` | 今夜到此为止：哥哥握着那枚带血的发卡逼问到底，伪装被彻底撕开。 | `发卡`,`血`,`是谁的`,`说实话` | 「这上面的血到底是谁的」 | `算了`,`还给你` |
| `end_regret` | 今夜到此为止：话说得太晚，她已经不想再听了。 | `算了`,`当我没说`,`没什么` | 「算了，当我没说」 | `对不起`,`累` |

> ⚠️ `n6_silence` 与 `end_regret` 的 keywords 有重叠（`没什么`），`n2_kitchen` 与 `end_regret` 也有重叠（`没什么`）。
> **这正是 lint 要抓的东西**：落地时要么把 `end_regret` 的关键词收紧为 `算了`/`当我没说`，要么给 `end_regret` 加 `requires` 硬门槛（它本来就有 `affinity_lte: 81` + `tag: regret`，实际已足够互斥）。**请工程师在 lint 报告中明确写出处置结论。**
>
> **v1.2 终裁补记（M-10）**：`n5_search.negatives` 由 `["算了","晚安"]` 扩为 **`["算了","晚安","睡"]`**。
> 用途：让「那……晚安吧。」/「该睡了，晚安。」这类收束语境**必然**走到 `n7_goodnight`，而不是被 priority 78 的 `n5_search` 抢走。
> 语义自洽（说晚安时不会去翻她口袋），**不是为过测试硬塞的 hack**。
> 效果（输入「那……晚安吧。」）：`n5_search` 由 `0.17` 变为 `0.17 − 0.5 = −0.33`；
> `n7_goodnight` = `0.1375(kw) + 0.1275(prior) + sim ≈ 0.265` → **领先约 0.6**，比纯 4 词方案的 0.095 **高一个量级**。

---

## 8. UI 需求

### 8.1 改名后的窗口标题

| 位置 | 现值 | 新值 |
|---|---|---|
| 窗口标题（`qt_main.cpp:42`） | `FluidStory - Powered by NarrativeX` | **`FS · FluidStory`** |
| 主菜单大标题（`qt_main.cpp:70`） | `FluidStory` | **`FS`**（副标题小字 `FluidStory Demo`） |
| 可执行文件 | `fluidstory` | **`fs`** |
| CMake `project()` | `NarrativeX` | **`FS`** |
| 目录（Win / Ubuntu） | 旧解压目录名（已废弃，**勿再引用**） | **`fs`**（Win: `D:/2026-10-30-16-55-07/fs`；Ubuntu: **`/home/cyc/fs`**） |
| 对话框标题（存档/警告） | `系统` / `警告` | 前缀统一 `FS · `，如 `FS · 系统` |

### 8.2 输入框唯一性（R-04）

**代码级硬约束：**

1. `src/qt_main.cpp` 中 `new QLineEdit` **只允许出现 1 次**；
2. 输入框容器 `inputWidget` 必须是**单例**，只通过 `show()` / `hide()` 切换显隐，**禁止在 `refreshUI()` 中 `new`**；
3. `refreshUI()` 开头必须无条件 `inputWidget->hide()`（现有代码已有，保留）；
4. 所有"唤起输入框"的入口，**只允许有一个统一的 `showInput()` 函数**，所有按钮都调它（消除复制粘贴）。

**关于"两个输入框"的真实成因（我读代码后的判断，供工程师验证）：**

现有 `qt_main.cpp` 里其实**只有 1 个 `QLineEdit`**。真正的重复是**两个"自由输入"按钮**：

- 第 229 行：`if (btnText.contains("自由输入") || btnText.contains("3."))` → 由 JSON 里的选项 `"3. 【自由输入】我想自己说点什么..."` 生成的按钮；
- 第 245 行：`if (hasIntent)` → 引擎又强制生成了一个 `"【自由输入】对她说点什么..."` 按钮。

两者同时出现时，界面上会看到**两个功能完全相同的橙色/蓝色按钮**。玩家会以为自己看到的是"两个输入框区域"。

> **给工程师的排查指令**：
> ① 先确认甲方看到的究竟是"两个 QLineEdit"还是"两个自由输入按钮"（截图取证）；
> ② 若是后者 —— **删掉第 245 行那个强制生成块**，改为：当节点 `hasIntent` 为真时，**只渲染一个**统一文案的「【自由输入】对她说点什么…」按钮，并从新剧本的 `options` 里移除 `"3. 【自由输入】"` 这一项（自由输入是引擎能力，不该写成剧本选项）；
> ③ 若是前者 —— 按上面的 4 条硬约束重构。

### 8.3 Qt 界面交互测试清单（12 项，全部通过才算 R-04 完成）

| # | 用例 | 期望 |
|---|---|---|
| T-01 | 启动 `./fs`（工作目录 = `build/`） | 主菜单出现，标题 `FS`，无报错弹窗，背景图正常 |
| T-02 | 启动（工作目录 = 项目根） | 也能正常加载剧本（路径回退逻辑生效），无 SIGSEGV |
| T-03 | 新建游戏，名字填 `O'Brien` | 正常进入游戏，存档/读档/删除全部不崩（R-11 连带验收） |
| T-04 | 主菜单 → 开始新游戏 → 取消输入名字 | 回到主菜单，不进入游戏页 |
| T-05 | 游戏页：**数输入框** | 可见的输入框**恰好 1 个**；"自由输入"类按钮**恰好 1 个** |
| T-06 | 点击"自由输入"按钮 | 选项按钮全部隐藏，输入框出现并自动获得焦点，光标可直接打字 |
| T-07 | 输入中文「你口袋里藏了什么」→ 发送 | 显示"正在思考…"，随后出现**非空中文**回应（**不是空白、不是 `（API内部报错）`**） |
| T-08 | 输入空串 / 纯空格 → 发送 | 无任何反应（现有 `if (text.isEmpty()) return;` 已覆盖纯空；需补 `trimmed()` 处理纯空格） |
| T-09 | 连续快速点"发送" 5 次 | 不重复提交；发送按钮在请求期间禁用；**5 秒后不崩溃**（针对 §9.1 候选根因 ③ 的回归测试） |
| T-10 | 触发不文明用语 | 弹出一次警告、好感度 −20、回到正常界面，**不重复弹窗** |
| T-11 | ⚙ 菜单 → 保存进度 → 返回主菜单 → 读取存档 | 存档列表出现该记录，点击后回到**同一个节点**、好感度一致 |
| T-12 | 走到任一结局 | 显示结局文本，随后显示【游戏结束】，再输入无任何反应；退出进程干净（退出码 0） |

---

## 9. 约束与风险

### 9.1 技术债（已知，不列入"新功能"，但 R-02/R-03/R-11 必须消化）

| ID | 位置 | 问题 | 等级 | 消化方式 |
|---|---|---|---|---|
| D-1 | `include/AiEngine.h:128-134` | `if (c >= 32 && c <= 126)` 只保留 ASCII，**把所有 UTF-8 中文台词静默清空**。实测 `"哥哥你今天怎么凶我……"` → 玩家看到**空白**，且 JSON 照样解析成功、不报错 | **P0** | R-02 |
| D-2 | `include/DatabaseManager.h` 多处 | SQL 用字符串拼接，存在注入风险；玩家名 `O'Brien` 会崩 | **P1** | R-11 |
| D-3 | `engine/include/nx/` 整套内核 | 已实现且 Python 参考 24/24、SQLite 四闸门 17/17 通过，**但尚未接线到 Qt UI 与 AiEngine** | — | R-09 |
| D-4 | `src/qt_main.cpp:384-393` | `QTimer::singleShot(5000, [&, watcher]{...})` 在 5 秒后访问 `watcher->isFinished()`；但 `watcher` 可能已在 `finished` 回调里被 `deleteLater()` 销毁 → **悬空指针解引用 → SIGSEGV**。AI 响应超过 5 秒时**必然**触发 | **P0（崩溃候选主因）** | R-03 |

### 9.2 「自由输入闪退」根因候选清单（R-03 必须逐一排查并给出结论）

> 甲方判断"是因为剧情太少" —— **这个判断不准确**。剧情少只会让 AI 更容易乱跳（跳到 `end_getitem` 是症状，不是死因）。真正的死因在下面四个候选里，请工程师用 gdb + ASan 逐一验证，并在《崩溃根因结论》里写明"实际是哪个 + 证据"。

| 候选 | 机理 | 为什么"跳到 `end_getitem`"会成为触发点 |
|---|---|---|
| ① **悬空 `watcher`**（D-4） | 5 秒超时回调访问已销毁的 `QFutureWatcher` | AI 调用慢 → 超时路径被激活 → 崩溃。**日志恰好停在最后一次 AI 跳转，看起来很像"跳到某节点后崩了"** |
| ② **跨线程 UI 访问** | `QtConcurrent::run` 里跑 `engine.processInput`，同时 `finished` 回调里 `dialogBox->setText()`；`engine` 的 `currentNode` 无锁 | 竞态，崩溃点随机，难复现 |
| ③ **`db` 空指针 / `playerName` 为空** | `end_getitem` 有 `reward_item` → `db->addItem(playerName, reward)`；若玩家名未被 `createNewPlayer` 设置（例如从读档路径进入），SQL 拼接空值 | 只在拿到道具的节点崩，与"日志显示跳到 `end_getitem`"高度吻合 |
| ④ **中文被清空后的空串**（D-1） | `reply` 为空 → `aiText.empty()` → 显示兜底串；本身不崩，但会让玩家以为"卡死/闪退" | 不是崩溃，是"看起来坏了" |

### 9.3 风险

| 风险 | 影响 | 缓解 |
|---|---|---|
| **R-09 接线工作量超预期**（决策点 A 的风险 1） | 阻塞"在 UI 上演示指针" | 已按 S1→S4 分阶段：S1 离线验证不依赖接线，可独立交付演示证据 |
| **两套持久化并存**（风险 2） | Qt 写 `save.db`、nx 写事件表，读档状态不一致 | S4 统一前，**明确只让 nx 写状态**，Qt 侧只读；禁止两边同时写同一张表 |
| **`nx` 内核无 UI 实战记录**（风险 3） | 首次接线暴露未覆盖问题 | 接 UI 前先在 `fs_terminal`（终端版）里跑通同一份剧本，终端版没有 Qt 线程问题，定位更快 |
| **7 节点图的"提前量"只有 1~2 步**，甲方可能觉得不够震撼 | 演示说服力不足 | §6.1 已注明理论上限；若甲方要求更大提前量需加中间节点（列入 §10） |
| **双机（Win + Ubuntu）不同步** | 一边改一边忘 | 每次改动后必须 `scp` 同步 + 在 Ubuntu 侧重跑 `python reference/simulate.py`，两侧结果一致才算完成 |
| **无 key 时无法验证 L2 选路质量** | R-15 评测集跑不了 | 先把 L1 档做到"闭眼能走完"，L2 作为增强；不要阻塞在 key 上 |
| **剧本 JSON 手写易错**（缺字段、节点孤立） | 加载失败或死路 | 依赖 `StoryLoader` 的 lint；lint 不通过则拒绝加载，不要把错误留到运行时 |

---

## 10. 待甲方确认事项

> **状态总览（v1.4，2026-10-30 更新）**
> ✅ 已闭环：**Q-1 / Q-3 / Q-4 / Q-9 / Q-10 / Q-11**
> ⏳ 仍待甲方：**Q-5**（前向提前量）—— 全员按"接受 1~2 步"口径继续，**不要准备分支版本**
> 🔧 已转技术裁定（不占甲方时间）：**Q-2 / Q-6 / Q-7 / Q-8**

| # | 事项 | 我的默认建议 | 需要甲方回答什么 |
|---|---|---|---|
| **Q-1** | **第三个结局的定义**。甲方原文只列了"1 个普通结局 + 1 个隐藏结局"，但总数要求 3 个 | ✅ **甲方已确认（2026-10-30）：第三个结局 = 遗憾/失败向 `end_regret`。不再是本 PRD 的假设，按已确认执行。**<br>定义：话说得太晚、错过的那条路最终没能补回来，天亮前她没再开口。**达成条件**（定稿）：`{all: [{any: [{tag:"regret"},{step_gte:9}]}, {affinity_lte: 81}]}`；**区分度**：与 `end_calm`（和解、aff ≥ 82 + 已入夜）和 `end_hidden`（拿到发卡、真相撕裂）在触发条件与文本上完全互斥 | **已闭环，无需再问** |
| **Q-2** | **决策点 A 的取向**：新剧本写在旧 `story.json` 还是新 `story_v2` | **推荐选项 2（`story_v2` + nx 内核）+ 分阶段落地（S1 离线验证先行）**。理由见 §4 决策点 A | 能否接受 S1→S4 的分阶段节奏？即"先用离线仿真证明指针能力，再接线 UI"？如果甲方**只要 UI 上能演示**、不在乎断言，则可退回选项 1（但那样后指针无法真实验证） |
| **Q-3** | **改名的范围**：是否连**目录名**一起改 | ✅ **已确认并执行**：Win 与 Ubuntu 两侧目录名均已统一为 `fs`（README、CMake、构建脚本、scp 路径已同步改完） | 已闭环：目录名与 target 名一起改，风险可控（改名未触及业务逻辑，回滚只需把目录名改回） |
| **Q-4** | **API key 是否可得** | ✅ **甲方已确认（2026-10-30）：有 key，稍后配置。验收走 L2 完整档，但 L1/L0 降级通道必须保留。**<br>影响：① §4 决策点 B 的三档降级设计不变，默认档由 L1 变为 L2；② R-15（意图准确率评测集）**从 P2 升格为必做**——有 key 就必须跑 L2 准确率并断言 `L2 ≥ L1`；③ Q-11 的"离线照脚本念"仍作为**降级演示预案**保留，不是主要验收路径 | **已闭环，无需再问**（待 key 到位后执行 L2 验收流程） |
| **Q-5** | **"提前量"是否够**。7 节点图的理论提前量只有 1~2 个 seq 步 | 接受 1~2 步，用"整局步数从 5 步压缩到 4 步 + Frontier 日志可证明"来呈现 | ⏳ **仍待甲方（已问两次）。全员按"接受 1~2 步"口径继续，不要准备分支版本。** 若要求更显著，需把对话节点从 7 个扩到 9~10 个，与"7 个对话节点"硬要求冲突 |
| **Q-6** | **"两个输入框"的实际形态** | 我读代码的判断是"两个自由输入**按钮**"，而非两个 `QLineEdit`（见 §8.2） | 能否提供一张截图确认？如果确实是按钮重复，修复方案是"从剧本 options 里删掉 `【自由输入】` 选项 + 合并引擎侧强制生成的按钮" |
| **Q-7** | **是否保留旧 `GameEngine` 链路** | 建议保留一个 A/B 回退开关（环境变量或启动参数），S3 出问题时能秒切回旧链路继续演示 | 是否同意保留回退开关？（会增加一点维护成本） |
| **Q-8** | **离线档的台词由作者预写**——这意味着无 key 时木子因**不会即兴发挥**，只会说作者写好的话 | 这本来就是 Galgame 的常态（AI 负责选路，作者负责文笔） | 是否接受？如果甲方希望"无 key 时也要有即兴感"，则需要额外的本地模板拼接方案（P2，本 PRD 不展开） |
| **Q-9（v1.1 新增，P0 阻塞）** | **`n_idle` 是否计入"7 个对话节点"**。`StoryLoader.hpp:207` **强制要求**剧本必须有一个 `kind:"fallback"` 的兜底节点，否则 lint 直接报错、剧本加载不了。但它是"保底闲聊"，不是作者编排的剧情 | **我方口径：不计入。** `n_idle` 是防死锁的闸门，属于引擎设施而非剧情内容；甲方要的"7 个对话节点"指的是 **7 个作者编排的剧情节点**（`kind: scripted/emergent`）。即最终 JSON 里有 7 个剧情节点 + 1 个 `n_idle` + 3 个 ending + 1 个 terminal `-1` | 是否同意这个口径？若甲方坚持"JSON 里除了结局就只能有 7 个节点"，那就只能砍掉一个剧情节点（会削弱用例的分支丰富度），请甲方取舍 |
| **Q-10（v1.1 新增，已裁定）** | **`n6_silence` 的「对不起，我不该那样说」按钮怎么处置**。该按钮直连 `n3_tender`，走的是 L0（`match_option`），**完全绕过 PendingPool**，玩家看不到 `redeem_variant` | **✅ 已裁定：采纳我方"直接删除"方案**（架构师已撤回其"改指向 `n7_goodnight`"的原建议）。删掉后"想道歉"只能靠自由输入 → 必然经过 PendingPool → **必然看到 `redeem_variant`**。`hint` 改为「【提示】你可以直接对她说点什么。」 | 无需再确认，仅告知 |
| **Q-11（v1.2 新增）** | **离线演示是否接受"输入必须照脚本念"**。离线档没有 `default_next` 兜底（M-9），走错一句话就可能岔到别的分支/提前结局 | ✅ **主理人已裁定（2026-10-30）：接受。** 理由：这是无 LLM 兜底的**固有代价，不是缺陷**，演示时把脚本摆手边即可。<br>⚠️ v1.4 补充：甲方已有 key（Q-4），故"照脚本念"降级为**降级演示预案**，主验收路径为 L2 自由输入 | **已闭环，无需再问**（保留为降级预案） |

---

## 10.1 交付归属与验收分工口径（v1.7 新增，team-lead 裁定）

> 本节只记录**口径**，不指派工作。目的是避免"同一份剧本两人各写一份"和"产品侧核对找错交付人"。

| 任务 | 负责人 | 交付范围 | 产品侧核对时的对象 |
|---|---|---|---|
| **T02** 剧本 + 断言 + 内核改动 | **software-engineer** | `data/story_v2.json`、`reference/`、`tools/`、`engine/include/nx/` | ✅ **产品侧核对的成品来自 software-engineer** |
| **T03** 崩溃取证 + 旧链路止血 | **software-engineer-2** | `include/`、`src/`、`scripts/`、`docs/CRASH_ROOT_CAUSE.md`、`data/story.json`（旧剧本） | ❌ **不产出新剧本**，产品侧不对其做叙事核对 |

⚠️ **作废声明**：此前团队内曾出现"T02 → engineer-2"的分工表，**已作废，以本表为准**。

### 产品侧核对清单（`story_v2.json` 交付后，由产品经理执行）

> 结构侧（节点数/可达性/算子合法性/断言通过率）由**架构师**复核；本节只列**内容侧**，两条线结论各自报 team-lead 汇总，**不与工程师直连**。

| # | 核对项 | 判据 | 不通过时的处置 |
|---|---|---|---|
| C-1 | **字数配额** | 正文字数落在 **900~1100**（不含字段名与标点外空白） | 退回补写/删减，不调整区间 |
| C-2 | **句数配额** | 台词句数落在 **50~70**（含 `redeem_variant` 的 4 句） | 同上 |
| C-3 | **节点/结局计数** | `count(kind=="scripted") == 7`；`kind=="ending"` 且 `id != "-1"` 恰好 **3** 个 | 硬要求，计数不对即退回 |
| C-4 | **三结局区分度** | 三个结局在**触发条件**与**文本语义**上完全互斥，且各自符合设定意图：<br>• `end_calm` —— 和解、aff ≥ 82 且已入夜<br>• `end_regret` —— 话说得太晚、没补回来，天亮前她没再开口（aff ≤ 81）<br>• `end_hidden` —— 拿到发卡、真相撕裂 | 若两个结局读起来"像同一个结局的不同措辞"，判区分度不足，退回重写文本（**不改阈值**） |
| C-5 | **叙事质量（主观项）** | ① 每句台词能独立读懂、不依赖上下文补全；② 木子因的语气在 7 个节点间有可辨识的情绪曲线（暖 → 僵 → 冷 → 破 → 和解/遗憾）；③ 无"作者旁白替玩家说话"的越界句 | 标注具体句号退回改文，**不动节点结构** |
| C-6 | **keywords 编码合规（规则 15/17/18）** | 每个节点 keywords 为**短而语义特异的子串**；且**至少 2 个能被自身 `examples[0]` 同时命中**（lint 已兜底，此处做人工抽检） | 命中 <2 报"白占归一化分母"，按 M-19 规则改**编码方式**，不改测试输入 |
| C-7 | **实测命中数与 margin 复核** | 工程师上报的 `n5_search` 实际命中数 **≥ 2**、kw **≥ 0.5**、与 `n7_goodnight` 的分差 **≥ 0.3**（终裁后实测约 0.6） | margin 收窄说明 keywords 被改动过，回查是否违反 M-19 合并口径 |
| C-8 | **`redeem_variant` 文本存在且不同** | `n3_tender.redeem_variant` 的 4 句已落地，且与首次 `presentation` 文本**不相等**（语义上体现"用现在的口径重讲一遍"） | 缺失即判 B-5 不通过（**阻塞项，不可降级**） |

---

## 附录 A：验收门禁（ checklist ）

- [x] R-01 改名：**已完成（T01 验收 7/7 全绿）** —— `build/fs` 可运行，窗口标题 `FS · FluidStory`，Win `D:/2026-10-30-16-55-07/fs` 与 Ubuntu `/home/cyc/fs` 两侧一致（10 个文件 md5 一致，旧目录名 grep 归零）
- [ ] R-02 中文：含中文的 AI 返回 100% 可见
- [ ] R-03 崩溃：20 次自由输入 0 崩溃 + 《崩溃根因结论》已交付
- [ ] R-04 UI：`new QLineEdit` 计数 == 1，T-01~T-12 全通过
- [ ] R-05 剧本：900~1100 字 / 50~70 句 / **`count(kind=="scripted") == 7`** / 3 结局 / **含 `kind:"fallback"` 的 `n_idle`** / lint 通过
- [ ] R-06 前向指针：**A-1~A-7** 全 PASS（v1.2 由 6 条增为 7 条）
- [ ] R-07 后向指针：**B-1~B-8** 全 PASS（v1.2 由 7 条增为 8 条，含 B-2b 收敛、B-7 防断裂、**B-8 防被 n5_search 抢走**，含对照组 B0）
- [ ] **🔴 R-10 `redeem_variant`（v1.7 升 P0，本轮必做，归 T02）**：`NodeSpec` 增字段 + `StoryLoader` 解析 + `NarrativeEngine` 唤醒时优先取用。**与之配套的 B-5 是阻塞项**——v1.6"引擎补齐前降级为非阻塞"的临时口径已作废，B-5 不过即判用例 B 不过
- [ ] R-08 离线：删掉 `config.json` 后仍能走完三条线（**每步输入必须照 §6 写死的脚本**）
- [ ] **M-1 回归**：7 个剧情节点 `phase.allow` 全开，且 `n3_tender` 在 `phase=night` 时仍可被唤醒
- [ ] **M-2 回归**：`n5_search.step_gte == 1`，`seq=1` 时 feasible
- [ ] **M-3 回归**：结局阈值 `end_calm ≥ 82` / `end_regret ≤ 81`，"拒绝线·挽回成功"(aff=84) 必须落到 `end_calm` 而非死锁
- [ ] **M-12 回归**：7 个剧情节点**全部 `once:true`**，只有 `n_idle` 是 `once:false`（防好感度渗漏）
- [ ] **M-9 回归**：离线档全部推进由 L1 打分完成，**没有任何一步依赖 `default_next`**（若发现某步只有靠 default_next 才走得通，说明该步输入没写对）
- [ ] **M-10 回归**：`n7_goodnight.keywords` 保持 4 个不变；`n5_search.negatives` 含 `晚安`/`睡`；A-7/B-8 的收敛边际 ≥ 0.3（终裁后实测约 0.6）
- [ ] `python reference/simulate.py` → 24/24（原有）+ 3/3（新增：forward / backward / backward_neg）
- [ ] `python persistence/tests/test_store.py` → 17/17（未 regression）
- [ ] Windows 与 Ubuntu 两侧运行结果一致
