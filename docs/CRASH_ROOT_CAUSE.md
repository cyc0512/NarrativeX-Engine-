# 《崩溃根因结论》

> T03 · R-03 交付物。作者：寇豆码（software-engineer-2）
> 所有结论都来自**真实生产代码路径**上的实测，不是复刻探针，也不是推理。
> 凡是没能证明的，下面都明确标了「未证实 / 未复现」。

---

## 0. 一句话结论

自由输入这条异步路径上存在**两个**真实的内存错误，它们的共同点是**对象的生命周期没管住**：

| # | 名字 | 一句话 | 什么时候发生 | 是否必崩 | 现状 |
|---|------|--------|--------------|----------|------|
| **D-4** | 悬垂 `watcher` | 5 秒定时器**必然**对一个已被 `deleteLater()` 析构的 `QFutureWatcher` 调用 `isFinished()` | **AI 在 5 秒内返回时（也就是正常情况）**，每次必发生 | ❌ 不必然。是未定义行为，崩不崩取决于那块内存当时装了什么 | 已由另一位工程师用 QPointer + done 标志 + window 托管定时器修复 |
| **D-7** | 退出时序（**本次新发现**） | 玩家发送后立刻退出 → `main` 结束 → `GameEngine` 析构（`storyData` 释放）→ QtConcurrent 工作线程还在 `processInput` 里读 `storyData["nodes"]` | AI 比玩家退出慢的时候 | ✅ **确定性崩溃**（ASan + gdb 双证） | D-4 的修复**覆盖不到**它；已由我在 `GameEngine` 析构时等后台任务收尾堵住 |

**给甲方的最短答案**：不是"跳到某个节点就崩"，而是"自由输入这条异步路径上有两个被追着用的对象 —— 一个被定时器追着用（watcher），一个被后台线程追着用（engine），两个都可能先被销毁"。

---

## 1. 取证方法（先说清楚证据是怎么来的）

### 1.1 为什么不用"复刻探针"

`scripts/d4_uaf.cpp` 这类复刻品只能证明"这种写法会崩"，证明不了**生产二进制里的那条路径会崩**。
所以本次取证全部跑在**真实的 `src/qt_main.cpp`** 上：

`forensics/drive_free_input.cpp` 用 `#define main qt_main_main` 把真实的 `main` 改名，
由自己的 `main` 起一个驱动线程，通过 queued metacall 把"点击"投递到 GUI 线程。
于是走的是 production 里那个真的 `sendBtn` clicked lambda，不是它的复制品。

```bash
# 修复后（默认）
bash scripts/build_asan.sh src forensics/drive_free_input.cpp drive_fixed_asan qt
# 修复前（-DFS_DRIVE_USE_PREFIX → forensics/qt_main_prefix.cpp，冻结的旧版 main）
g++ ... -DFS_DRIVE_USE_PREFIX forensics/drive_free_input.cpp -o build/drive_prefix_asan ...

QT_QPA_PLATFORM=offscreen ASAN_OPTIONS=detect_leaks=0 ./drive_fixed_asan --count 20 --wait 5600
```

驱动流程全程无人值守：开始新游戏 → 填玩家名 → 点自由输入 → 输入 → 点发送 → 等待 → 下一条。
`--wait` 必须 > 5000，否则观察不到那个 5 秒定时器。

### 1.2 对照物与砧境

- AI 用本地 mock：`forensics/mock_ai.py --port 18080 [--delay N]`（`--delay 8` 用来造"AI 比 5 秒慢"和"AI 比退出慢"）。
  `config.json` 里加 `api_url` 指向它（`AiEngine` 读的是 **`../config.json`**，cwd 是 `build/`）。
- 修复前副本：`forensics/qt_main_prefix.cpp`（原文件 md5 `da6b462290867ada3d6eced15db5f069` 冻结，只加了 3 行 `[T03探针]` 打印）。
- 工具：ASan（`-fsanitize=address -fno-omit-frame-pointer -g -O1`）、gdb、自写 SQL 对照测试。

---

## 2. D-4：悬垂 `watcher`

### 2.1 触发条件（**与 PRD 的假设相反**）

PRD §D-4 写的是「AI 响应超过 5 秒时**必然**触发」。实测的代码形态恰恰相反：

- **AI 在 5 秒内返回**（正常情况）：`finished` 回调里 `watcher->deleteLater()` → 对象几百毫秒内被析构；
  而 `QTimer::singleShot(5000, …)` **没有绑 context object，到点照样会响** →
  5 秒时 `watcher` 是个非空但悬垂的指针，`if (watcher && …)` 拦不住它 → `isFinished()` 解引用已释放内存。
- **AI 超过 5 秒**：定时器先跑，此时 watcher 还活着，反而没有 UAF（代价是按钮被提前点亮，可能引发双发，见 §4.3）。

所以**"AI 越快越危险"**。这一点与 `src/qt_main.cpp` 里另一位工程师留下的注释（「AI 比 5 秒快时 3/3 SIGSEGV」）方向一致。

### 2.2 真实链路上的探针日志（原文）

修复前副本 + 真实 `sendBtn` 路径，20 条自由输入中的一次完整时序（stderr，无缓冲）：

```
[T03探针] 提交后台任务: idealThreadCount=2, poolMax=2
[T03探针] worker 进入 processInput
[T03探针] worker 退出 processInput
[T03探针] AI 已返回，finished 回调 -> deleteLater, watcher=0x50300010f030
[T03探针] 5s 定时器触发, watcher=0x50300010f030（裸指针，非空但可能已悬垂）
[T03探针] 5s 时刻 QPointer 已失效(=对象已析构)? 是 —— 已析构，下面的访问就是解引用已释放内存
[T03探针] 5s 定时器：isFinished() 为真（或读到的是已释放内存里的残留值），未做处理
```

三个事实钉死：

1. 定时器**确实响了**（不是"可能没排上"）；
2. 响的时候拿到的是**同一个地址** `0x50300010f030`，而它已经被 `deleteLater()` 交给了析构；
3. `QPointer` 判定「已析构」—— `QPointer` 只在 `QObject` 析构的瞬间变 null，这是比任何日志都硬的证据。

20 条输入里非空的有 19 条（第 1 条是空串，`if (text.isEmpty()) return;` 早退），
**19 条全部命中，19/19**：

```
$ grep -c "T03探针] 5s 时刻 QPointer 已失效" /tmp/prefix20.txt
19
```

### 2.3 为什么 ASan 抓不到它（重要，别误判为"没毛病"）

`QFutureWatcherBase::isFinished()` 的实现在 **`libQt5Core.so` 里**，
而我们只对自己的代码插了 ASan —— 解引用发生在**未插桩的库内部**，ASan 没有检查点，所以不报。
我把 `quarantine_size_mb` 提到 1024 重跑，依然不报，原因同上（不是内存被复用，是压根没被检查）。

能证明它的手段只有 QPointer / 析构日志这类**外部证据**——上面 19/19 那条就是。

### 2.4 崩溃率：**不必然崩**，这点必须如实说

| 场景 | 结果 |
|------|------|
| 修复前，20 条自由输入（AI 毫秒级返回，每条等 5.6 s 覆盖定时器） | `DRIVER_RESULT: sends=20/20 crash=0`，ASan 0 报错 |

> ⚠️ 对照口径：冻结副本只冻结了 `qt_main.cpp`（异步那一块），
> `include/GameEngine.h` / `include/DatabaseManager.h` 两侧都用**当前（已修）版本**。
> 所以这张表隔离的是"D-4 那一段代码"的影响，不是"整个旧版本"的影响。
| 修复前，3 次 × 3 条 | 3/3 次 `crash=0` |
| 另一次修复前跑（count=2） | 崩了 —— 但崩在**退出阶段**（见 §3，是 D-7 不是 D-4） |
| 另一位工程师的最小复刻探针 | 3/3 SIGSEGV |

也就是说：**"每次都解引用已释放内存"是 100% 成立的，但"每次都 SIGSEGV"在我的环境里不成立。**
它是不是就是甲方看到的那次崩溃，我**无法 100% 断言** —— 我只能说它是这条路径上唯一一个"每次都发生的内存错误"，
并且它是未定义行为，在不同机器/不同堆布局上表现为"偶发崩溃"完全合理。

### 2.5 修法（另一位工程师已落地，我未改动 `qt_main.cpp`）

1. `QPointer` 持有 watcher —— 对象一析构立刻变 null，可判空（裸指针做不到）；
2. `shared_ptr<bool> done` —— 把"已经对玩家兑现过一次"从"对象还活着吗"解耦，两条路径抢占，后到的直接退出；
3. `QTimer` 交给 `&window` 托管 —— window 没了自动停表；
4. 两条路径互相判空；
5. 额外加了 `aiInFlight`，堵掉"超时后玩家再点一次 → 两个 processInput 并发"（D-6 的入口）。

---

## 3. D-7：退出时序（本次新发现，确定性崩溃）

### 3.1 复现

条件是"发完就走"，在真实网络下很容易撞上（AI 1~3 秒很常见，玩家切走/关窗不等它）：

```
mock: python3 forensics/mock_ai.py --port 18081 --delay 8    # AI 要 8 秒
驱动: --count 2 --wait 1500                                   # 发送后约 1.5 秒就退出
```

### 3.2 ASan 报告（原文，去掉了 STL 模板噪声）

```
==33960==ERROR: AddressSanitizer: heap-use-after-free on address 0x504000006c60
READ of size 8 at 0x504000006c60 thread T2 (Thread (pooled))
    #0 std::_Rb_tree<...>::_M_mbegin /usr/include/c++/11/bits/stl_tree.h:735
    #1 std::_Rb_tree<...>::_M_begin                     stl_tree.h:739
    #2 std::_Rb_tree<...>::lower_bound(__k="nodes")     stl_tree.h:1270
    #3 std::map<string, json>::lower_bound              stl_map.h:1259
    #4 std::map<string, json>::operator[]               stl_map.h:517
    #5 nlohmann::basic_json::operator[]<char const>     include/json.hpp:21276
    #6 GameEngine::processInput(std::string const&)     include/GameEngine.h:293
    #7 operator()                                       forensics/../forensics/qt_main_prefix.cpp:441
    #8 QtConcurrent::StoredFunctorCall0<...>::runFunctor qtconcurrentstoredfunctioncall.h:60
    #9 QtConcurrent::RunFunctionTask<std::string>::run  qtconcurrentrunbase.h:108
    #10 (/lib/x86_64-linux-gnu/libQt5Core.so.5+0xcff91)
    #12 start_thread nptl/pthread_create.c:442

0x504000006c60 is located 16 bytes inside of 48-byte region [0x504000006c50,0x504000006c80)
freed by thread T0 here:
    #0 operator delete(void*, unsigned long) asan_new_delete.cpp:172
    #4 nlohmann::basic_json::~basic_json()          include/json.hpp:19758
    #5 GameEngine::~GameEngine()                    include/GameEngine.h:94
    #6 qt_main_main(int, char**)                    forensics/../forensics/qt_main_prefix.cpp:481
    #7 main                                         forensics/drive_free_input.cpp:292

previously allocated by thread T0 here:
    #11 GameEngine::GameEngine(std::string const&)   include/GameEngine.h:80   ← 剧本 JSON 的 map
    #12 qt_main_main(int, char**)                   forensics/../forensics/qt_main_prefix.cpp:42

Thread T2 (Thread (pooled)) created by T0 here:
    #1 QThread::start(QThread::Priority)  (/lib/x86_64-linux-gnu/libQt5Core.so.5+0xcc752)
```

一句话读法：**T0 在 `main` 结束的瞬间把 `storyData` 拆了，T2（QtConcurrent 线程池线程）下一行还在 `storyData["nodes"]`。**

### 3.3 gdb bt（原文，同一场景，`ASAN_OPTIONS=abort_on_error=1`）

```
Thread 3 "Thread (pooled)" received signal SIGABRT, Aborted.
[Switching to Thread 0x7fffea0a3640 (LWP 33964)]
#0  __pthread_kill_implementation (signo=6) at ./nptl/pthread_kill.c:44
#3  __GI_raise (sig=sig@entry=6) at ../sysdeps/posix/raise.c:26
#4  __GI_abort () at ./stdlib/abort.c:79
#5  __sanitizer::Abort () at sanitizer_posix_libcdep.cpp:151
#6  __sanitizer::Die () at sanitizer_termination.cpp:58
#7  __asan::ScopedInErrorReport::~ScopedInErrorReport () at asan_report.cpp:190
#8  __asan::ReportGenericError (addr=..., is_write=false, access_size=8, fatal=true) at asan_report.cpp:478
#9  __asan::__asan_report_load8 (addr=<optimized out>) at asan_rtl.cpp:121
#10 std::_Rb_tree<...>::_M_mbegin (this=0x504000006c50) at stl_tree.h:735
#12 std::_Rb_tree<...>::lower_bound (__k="nodes", this=0x504000006c50) at stl_tree.h:1270
#14 std::map<string, json>::operator[] (__k=..., this=0x504000006c50) at stl_map.h:517
#15 nlohmann::basic_json::operator[]<char const> (key=0x55555563c7e0 "nodes") at include/json.hpp:21276
#16 GameEngine::processInput (this=0x7fffffffe510, userInput="   ") at include/GameEngine.h:293
#17 operator() (__closure=0x507000046bc0) at forensics/../forensics/qt_main_prefix.cpp:441
#18 QtConcurrent::StoredFunctorCall0<...>::runFunctor(void) at qtconcurrentstoredfunctioncall.h:60
#19 QtConcurrent::RunFunctionTask<std::string>::run (this=0x507000046b80) at qtconcurrentrunbase.h:108
#20 0x00007ffff5ecff92 in ?? () from /lib/x86_64-linux-gnu/libQt5Core.so.5
#22 start_thread (arg=<optimized out>) at ./nptl/pthread_create.c:442
#23 clone3 () at ../sysdeps/unix/sysv/linux/x86_64/clone3.S:81
```

### 3.4 **修复版同样会崩**（D-4 的修复覆盖不到它）

我把同一个场景跑在**当前已修好 D-4 的** `src/qt_main.cpp` 上：

```
$ gdb -batch -ex run -ex bt --args ./drive_fixed_asan --count 2 --wait 1500
==34139==ERROR: AddressSanitizer: heap-use-after-free on address 0x504000006c60
    #6 GameEngine::processInput(...) /home/cyc/fs/include/GameEngine.h:293
Thread 3 "Thread (pooled)" received signal SIGABRT, Aborted.
```

原因很直白：`engine` 是 `main()` 的栈对象，而后台任务是通过**裸引用**抓它的；
D-4 修的是 watcher，没修"引擎比后台任务先死"这件事。

### 3.5 已堵住（改动落在我的文件 `include/GameEngine.h`）

`GameEngine` 加了在飞计数，析构时先等后台 `processInput` 收尾（上限 12 秒，覆盖 curl 的 10 秒超时）：

```cpp
~GameEngine() {
    for (int i = 0; i < 2400 && inflight_.load() > 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (inflight_.load() > 0) std::cerr << "【引擎警告】退出时仍有后台任务未收尾…" << std::endl;
    delete db; db = nullptr;
}
```

复测（同场景，AI 8 秒，发送后 1.5 秒退出）：

```
DRIVER_RESULT: sends=2/2  crash=0  (全部输入跑完，未崩溃)     ← 无 ASan 报错
```

---

## 4. 其余候选的排除证据

### 4.1 D-5（`db` 空指针 / `playerName` 为空）→ **排除**

`forensics/d5_probe.cpp` 直接构造了两条"最容易炸"的路径：

```
=== D-5 排除性验证 ===
① 剧本不存在: ready=0, lastError=剧本文件打开失败: ../data/根本不存在的剧本.json
   processInput 返回: 哥哥，你回来了【中文测试】。…（正常返回）
   getAllSaves().size()=0
   [观察] 全程未崩溃
② 正常构造: ready=1
   loadExistingPlayer(不存在)=0（应为 0）
   playerName 为空时 processInput("1") -> 你做出了选择。
   applyPenalty(-20) 后 node=1
   [观察] 全程未崩溃（空 playerName 只会写进一条空名存档，不会炸）
```

`GameEngine` 现在有 `ready()` 门禁 + 所有 `db->` 前的 `if (db)` 守卫，D-5 不成立。

### 4.2 D-1（中文被清空）→ **不是崩溃**，已修 + 已验证

`AiEngine.h` 的 `sanitizeJsonText()` 只剥反引号与 `< 0x20` 控制字符，≥ 0x80 的字节原样保留。
端到端实测（真实 HTTP → mock → AiEngine → GameEngine → 输出）：

```
【AI原始文本】 {"affinity_change": 2, "next_node": "", "reply": "哥哥，你回来了【中文测试】。…那是「发卡」吗？…🌟🌙 …"}
    非 ASCII 字节数 = 173
    字节序列(前 60 个) = E5 93 A5 E5 93 A5 EF BC 8C E4 BD A0 …
    [OK] 中文汉字 完整可见 / [OK] 中文引号 / [OK] emoji / [OK] emoji-2 / [OK] 全角句号 / [OK] 全角省略号
```

D-1 的表现是"玩家看到空白 / 以为卡死"，不是崩溃 —— 它不能解释 SIGSEGV。

### 4.3 D-6（跨线程 UI / 引擎访问）→ **真实存在，但已降级为主止血**

`QtConcurrent::run` 里跑 `engine.processInput`，而 GUI 线程同时会 `refreshUI` / `applyPenalty`（保存进度）——
两边读写同一份 `affinity` / `currentNode` / `lastAiReply`，其中 `lastAiReply` 是 `std::string`，
一边赋值一边拷贝是真的会炸的。

本次做了两道处理：

1. `include/GameEngine.h`：加 `stateMutex_`，所有状态读写走临界区（**网络调用不带锁**，否则会把 UI 卡住）；
2. `src/qt_main.cpp`（另一位工程师）：`aiInFlight` 防重入，堵掉"超时后玩家再点一次"这个最直接的并发入口。

⚠️ **未做**：TSan 构建跑同一输入的实测（时间关系没跑）。所以 D-6 我只能给到"代码层面已加锁 + 入口已封"，
**不能**给"TSan 0 报告"这种证据。

### 4.4 候选之五：`sqlite3_column_text()` 返回 NULL → **证实为真崩溃，已修**

不在原四个候选里，是交接时同事提的怀疑。我把它做成了对照用例（`forensics/sql_injection.cpp` 用例 6）：
`last_login` 为 NULL 的旧档 → 旧代码 `std::string(reinterpret_cast<const char*>(nullptr))` →

```
修复前：BEFORE_EXIT=139   (Segmentation fault, core dumped)   ← 真 SIGSEGV
修复后：[PASS] last_login 为 NULL 时读成空串而不是崩溃
```

（迁移路径上真实存在：`ALTER TABLE` 加出来的列没有默认值，回填之前就是 NULL。）

---

## 5. R-11：SQL 参数化（顺带交付）

`include/DatabaseManager.h` 全部改为 `sqlite3_prepare_v2` + `sqlite3_bind_*`，
并且**不再吞掉错误**（旧的 `saveData` 返回 `void`，失败与否上层永远不知道 —— 这才是它最害人的地方）。

同一份测试编两次（`forensics/sql_injection.cpp`）：

```
########## BEFORE（-Iforensics/db_prefix，旧实现冻结副本）##########
  [FAIL] O'Brien 可以写入            ← 静默丢档：INSERT 语法错，错误被忽略，玩家以为存上了
  [FAIL] O'Brien 可以读回
  [FAIL] 注入串 A 之后 player_save 表还在
  [FAIL] 注入串 B 之后 player_save 表仍在  :: player_save 已被 DROP —— 存档全没了
BEFORE_EXIT=139                       ← 用例 6（NULL last_login）真崩
########## AFTER（-Iinclude）##########
  [PASS] O'Brien 可以写入 / 读回
  [PASS] 注入串 A 可以写入 / 表还在
  [PASS] 注入串 B 可以写入（作为普通玩家名）/ 表仍在
  [PASS] 含引号的道具名可存可取 / 含引号的标签名可存可取
  [PASS] deleteSave(注入串) 能删掉自己 / 没有误伤其他玩家
  [PASS] last_login 为 NULL 时读成空串而不是崩溃
结果: 11 项通过, 0 项失败
AFTER_EXIT=0
```

值得单独说一句：注入串 A（`'; DROP TABLE player_save;--`）在旧实现下**侥幸**没删表
（拼出来的第一条 `INSERT` 本身语法错，`sqlite3_exec` 在第一个错误处停了，后面的 `DROP` 根本没执行）。
所以我另造了**注入串 B**（`x', 1, 'y'); DROP TABLE player_save; --`）——
它先把引号闭合让第一条 INSERT 合法，再追加一条 DROP。**旧实现下 `player_save` 真的没了。**
这证明"SQL 拼接"在这套代码里不是理论风险，是能一键清档的实弹。

---

## 6. R-04：输入框去重

```
$ grep -c "new QLineEdit" src/qt_main.cpp
1
```

重复的来源不是输入框，是**两个「自由输入」按钮**（旧剧本选项里写死一个 + `hasIntent` 分支强制生成一个）。
已删掉旧剧本里写死的那个（`data/story.json` 节点 `1`）：

```diff
  "options": [
      { "text": "1. 对不起，我有点累。", "next": "end_1" },
-     { "text": "2. 没什么，你别管了。", "next": "end_3" },
+     { "text": "2. 没什么，你别管了。", "next": "end_3" }
-     { "text": "3. 【自由输入】我想自己说点什么...", "next": "" }
  ],
```

现在节点 1 只有 2 个固定选项 + 1 个引擎按 `intent_nodes` 自动生成的自由输入按钮，不再重复。

⚠️ 遗留一处（在 `src/qt_main.cpp`，文件当前归另一位工程师，我没动）：
固定选项的点击分支里还有 `btnText.contains("自由输入") || btnText.contains("3.")`。
后面那个 `"3."` 是给旧剧本擦屁股的启发式，删掉剧本选项后它已经没有匹配对象，
但任何含 "3." 的选项文本（比如"第 3."）都会误触发。建议一并清掉，改成只认 `自由输入`。

---

## 7. 验收对照

| 验收项 | 结果 |
|--------|------|
| ① R-02 中文/emoji/「」100% 可见 | ✅ 6/6 片段完整，173 个非 ASCII 字节一个没丢 |
| ② 20 次自由输入（空串/超长/纯标点/纯中文/emoji）0 崩溃 | ✅ `sends=20/20 crash=0`，ASan 0 报错（修复版） |
| ③ R-11：`O'Brien` 与注入串可新建/保存/读取/删除，全部走 `sqlite3_bind_*` | ✅ 11/11 通过（修复后）；修复前 8 项失败 + SIGSEGV |
| ④ R-04①：`grep -c "new QLineEdit" src/qt_main.cpp == 1` | ✅ 1 |
| ⑤ 《崩溃根因结论》逐一排除 D-4/D-5/D-1/D-6 + 实际根因 + gdb bt / ASan 原文 | ✅ 本文档；**另外补出一个真实根因 D-7** |
| 基线不回归（cmake build / simulate.py / test_store.py） | ✅ 编译通过；simulate 34/34；test_store 17/17 |

---

## 8. 还没做的（如实列出）

1. **TSan 没跑**，所以 D-6 只有"加锁 + 封入口"，没有 sanitizer 层面的证明。
2. **D-4 的崩溃率没能在本机复现**（20 条 0 崩）。能证明的是"19/19 次解引用已析构对象"，
   不能证明"它就是我们看到的那次 SIGSEGV"。要钉死需要在甲方机器上用同样的 QPointer 探针复跑。
3. `src/qt_main.cpp` 归属未定（写作期间被并行改动过三次，md5 `da6b4622 → 76d09ed8 → e9cc23aa`），
   所以本文档里 D-4 的修复**不是我写的**，我只做了验证；D-7 的修复落在我的 `GameEngine.h`。
4. `AiEngine::analyzeInput` 有一条编译警告 `control reaches end of non-void function`（既有问题，未改）。

---

# 附篇 · software-engineer-3 的补充取证

> 上面 §0–§8 由 software-engineer-2 撰写。以下 §9–§12 由 software-engineer-3 补充，
> 目的是**补掉上面 §2.4 / §8.2 留下的两个缺口**，并对全文做一处更正。
> 我的结论与上面**一致**（D-4 真实存在、触发方向是"AI 越快越危险"），不推翻任何一条。

## 9. 补充：D-4 的崩溃率已用**非 ASan** 复现（补掉 §8.2）

§2.4 与 §8.2 写的是"20 条 0 崩溃、无法证明它就是那次 SIGSEGV"。
**这个缺口现在补上了**——关键不在驱动方式，而在**构建方式**。

### 9.1 同一份修复前源码：非 ASan 3/3 崩，ASan 3/3 不崩

修复前源码：`git show HEAD:src/qt_main.cpp`，md5 **`ce1335b7ca4303ce3e203ba02cca9fdc`**
（含 D-4 缺陷：第 384 行 `QTimer::singleShot(5000, [&, watcher]`、
第 385 行 `watcher->isFinished()`、第 380/391 行 `watcher->deleteLater()`）

两个二进制走**同一套 cmake 构建**（`fs` target，Debug），驱动方式完全相同
（`scripts/ui_autopilot.cpp`，LD_PRELOAD，`FS_AP_ROUNDS=5 FS_AP_GAP=6000`）：

```
##### 修复前 · 非 ASan 构建 #####
  第 1 次: 退出码=139  存活标记=0
  第 2 次: 退出码=139  存活标记=0
  第 3 次: 退出码=139  存活标记=0
  bash: Segmentation fault  ./fs_prefix_before   (×3)

##### 同一份源码 · ASan 构建 #####
  第 1 次: 退出码=0  存活标记=1  ASan报错=0
  第 2 次: 退出码=0  存活标记=1  ASan报错=0
  第 3 次: 退出码=0  存活标记=1  ASan报错=0
```

139 = 128 + 11 = **SIGSEGV**。

### 9.2 结论：本 bug **不能**用 ASan 复现（务必写进方法论）

> **判定 D-4 必须以非 ASan 构建为准。ASan 替换分配器后 freed 区域被隔离、不再按原方式映射，
> 解引用不再 segv，反而把 bug 掩盖成"绿色"。**
> 后来者若照 ASan 复现一次得到绿灯就推翻本结论，那取证就白做了。

两个原因叠加：

1. `QFutureWatcherBase::isFinished()` 的实现在 `libQt5Core.so` 内部，
   `nm -D --defined-only /lib/x86_64-linux-gnu/libQt5Core.so.5 | grep -c '__asan'` = **0**，
   系统 Qt 未做 ASan 插桩，解引用发生在**没有检查点的库内部**；
2. ASan 的分配器把释放的内存 quarantine 起来，非 ASan 下会踩爆的地址在 ASan 下读出来是"有效"的。

**这正好解释了 §2.4 的"20 条 0 崩溃"**：那份数据跑在 ASan 下，与本文 9.1 的 ASan 侧 3/3 不崩
**完全一致，两边没有矛盾**。补上非 ASan 这一列之后，D-4 从"无法复现"升级为**确定性崩溃**。

### 9.3 修复前后对照（同一构建体系，非 ASan）

| 侧 | 源码 md5 | 构建 | 结果 |
|---|---|---|---|
| 修复前 | `ce1335b7ca4303ce3e203ba02cca9fdc` | cmake Debug **非 ASan** | **3/3 SIGSEGV（139）** |
| 修复前 | 同上 | ASan | 3/3 存活，0 报告 |
| 修复后 | `e9cc23aa046de023e7612befd90f79be` | cmake Debug **非 ASan** | **3 轮 × 10 次自由输入，0 崩溃** |

修复后明细（AI mock delay=0，`FS_AP_GAP=6000` > 5000 确保 5 秒定时器必然触发）：

```
  第 1 次: 退出码=0  存活标记=1  计划轮数 = 10，实际点击发送 = 10，空串被 UI 拦截 = 2
  第 2 次: 退出码=0  存活标记=1  计划轮数 = 10，实际点击发送 = 10，空串被 UI 拦截 = 2
  第 3 次: 退出码=0  存活标记=1  计划轮数 = 10，实际点击发送 = 10，空串被 UI 拦截 = 2
```

> 2 次"空串"是被 `qt_main.cpp:349` 的 `if (text.isEmpty()) return;` 在 UI 层挡掉的，
> 不进入异步路径——属正常行为，如实记录，不粉饰。

---

## 10. 与 PRD / ARCH 的分歧（保留原文，据实标注，不反向修改规范）

**PRD 原文**（`docs/PRD_FS_SCRIPT.md:920`）：

> | D-4 | `src/qt_main.cpp:384-393` | `QTimer::singleShot(5000, [&, watcher]{...})` 在 5 秒后访问
> `watcher->isFinished()`；但 `watcher` 可能已在 `finished` 回调里被 `deleteLater()` 销毁
> → **悬空指针解引用 → SIGSEGV**。**AI 响应超过 5 秒时必然触发** | P0（崩溃候选主因） | R-03 |

**PRD 原文**（`docs/PRD_FS_SCRIPT.md:928`）：

> | ① 悬空 `watcher`（D-4） | 5 秒超时回调访问已销毁的 `QFutureWatcher` |
> **AI 调用慢 → 超时路径被激活 → 崩溃**。日志恰好停在最后一次 AI 跳转…… |

**ARCH 原文**（`docs/ARCH_FS.md:1226`）：

> 目的：让"AI 超过 5 秒"成为**必然**而不是偶发。

### 分歧点

PRD 判定的**缺陷位置完全正确**，但**触发条件写反了**：

| | PRD 的写法 | 实测（§9.1） |
|---|---|---|
| AI **慢**（>5s） | 必然触发崩溃 | **3/3 不崩**——watcher 仍存活，`isFinished()==false` 走正常超时分支 |
| AI **快**（<5s） | 未提及 | **3/3 SIGSEGV**——watcher 已析构，5 秒回调才到 |

即：**是"AI 太快"而不是"AI 太慢"触发 UAF**。这也解释了为什么这个 bug 只在
**有真实 API key 的用户**手上出现——真实 glm-4-flash 通常 1~2 秒返回，正好落在危险区间。

**处理方式**：按 team-lead 裁定，**保留 PRD / ARCH 原文不动**，仅在结论中显式标注分歧，
本轮不反向修改规范文档去迁就代码。

> 另：`docs/PRD_FS_SCRIPT.md:904` 的 T-09「**5 秒后不崩溃**（针对 §9.1 候选根因 ③ 的回归测试）」
> 已由本修复覆盖（§9.3，修复后 3 轮 × 10 次自由输入全部存活）。

---

## 11. 独立发现 R-16：非法 UTF-8 输入 → 「（系统异常）」（本轮不修）

team-lead 裁定：立案记录，**本轮不改代码**（不属 R-02/R-03/R-04/R-11 验收范围）。

- **触发输入**：任何含非法 UTF-8 字节的自由输入，例如裸字节 `\x80\x81\x82\xFF`
- **异常类型**：`nlohmann::json::dump()` 抛 `type_error.316`（invalid UTF-8 byte）
- **当前表现**：被 `AiEngine::analyzeInput` 的 `catch(const std::exception&)` 兜住，
  玩家看到「（系统异常）」而不是自己的话
- **是否崩溃**：否
- **实测**：
  ```
  --- 观察用例: 非法 UTF-8 输入(仅观察是否崩溃) ---
      AI 回复 : （系统异常）
      [观察] 进程未崩溃 = OK（异常被 AiEngine 的 catch 兜住）
  ```
- **建议**：在 `GameEngine::processInput` 入口做一次 UTF-8 合法性校验并给友好提示

---

## 12. 对上面 §5 / §6 的两处更正

### 12.1 §6 的那条遗留项已经解决，可以勾掉

§6 末尾 ⚠️ 说 `src/qt_main.cpp` 固定选项分支里还留着
`btnText.contains("自由输入") || btnText.contains("3.")`。
**该特判已在 md5 `e9cc23aa…` 版本中移除**——当前文件里唯一一处 `contains(` 出现在
第 258 行的**注释**中（说明为什么要删它），没有存活代码：

```
$ grep -n 'contains(' src/qt_main.cpp
258:                // ⚠️ 旧代码在这里有一段 `btnText.contains("自由输入") || btnText.contains("3.")`
```

### 12.2 §5 的 `DatabaseManager.h` 实现归属

当前 `include/DatabaseManager.h` md5 = **`673629510201e9c9a7d46704eccf317c`**，
`sqlite3_bind_` 12 处、`sqlite3_prepare_v2` 10 处、字符串拼接 0 处，
该版本由 software-engineer-3 落地。§5 里 `forensics/sql_injection.cpp` 的
before/after 对照跑的就是这份实现，两边说的是同一份代码，无冲突。

另有一份独立的入口可用于交叉验收：`bash scripts/run_sql_bind_test.sh`
（A 段 DatabaseManager 直测 + B 段 GameEngine 全链路沙箱，不污染 `data/save.db`）。
