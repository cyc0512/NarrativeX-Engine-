// forensics/drive_free_input.cpp
//
// T03 / R-03 取证 + 回归驱动器：在**真实 src/qt_main.cpp** 上自动跑 N 次自由输入。
//
// 为什么不用"复刻代码形状"的小探针：复刻品只能证明"这种写法会崩"，不能证明
// **生产二进制里的那条路径会崩**。这里用 #define main 把 qt_main.cpp 的 main 改名，
// 由本文件的 main 启动一个驱动线程，通过 queued metacall 把"点击"投递到 GUI 线程，
// 于是走的是 production 代码里真实的 sendBtn clicked lambda（含 QFutureWatcher +
// 5 秒 QTimer 那段），而不是它的复制品。
//
// 用法（cwd 必须是 build/，AiEngine 读 ../config.json）：
//   g++ ... forensics/drive_free_input.cpp -o build/drive_free_input
//   QT_QPA_PLATFORM=offscreen ./drive_free_input --count 20 --wait 5600
//
// 环境变量：
//   FS_DRIVE_COUNT  覆盖 --count
//   FS_DRIVE_WAIT   覆盖 --wait（毫秒；必须 > 5000 才能观察到 5 秒定时器的行为）
//
// 判定：正常跑完会打印 "DRIVER_RESULT: sends=<n>/<n>  crash=0"。
//       若进程在打印之前死亡（ASan/SIGSEGV），即记为一次崩溃。

// QT_MAIN_SRC 可以在编译时切换：
//   默认            -> 真实生产文件 src/qt_main.cpp
//   -DQT_MAIN_SRC="\"../forensics/qt_main_prefix.cpp\"" -> 修复前冻结副本（before 侧证据）
#if defined(FS_DRIVE_USE_PREFIX)
// -DFS_DRIVE_USE_PREFIX：跑修复前的冻结副本（before 侧证据）。
#define QT_MAIN_SRC "../forensics/qt_main_prefix.cpp"
#else
#define QT_MAIN_SRC "../src/qt_main.cpp"
#endif
#define main qt_main_main
#include QT_MAIN_SRC
#undef main

#include <QApplication>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>
#include <QWidget>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

extern int qt_main_main(int argc, char** argv);

namespace {

// ---------------------------------------------------------------------------
// 20 条自由输入：覆盖验收要求里的 空串 / 超长 / 纯标点 / 纯中文 / emoji，
// 另外补上 SQL 注入串、反引号、引号、控制字符等"历史上最容易炸"的形态。
// ---------------------------------------------------------------------------
// 把一个 UTF-8 片段重复 n 次（不能用 std::string(n, '哈')：多字节字符常量会截断）。
std::string repeat_utf8(const char* piece, int n) {
    std::string out;
    for (int i = 0; i < n; ++i) out += piece;
    return out;
}

std::vector<std::string> build_inputs() {
    std::vector<std::string> v;
    v.push_back("");                                   // 1  空串（会被 isEmpty 早退，不发请求）
    v.push_back("   ");                                // 2  纯空白
    v.push_back("我想看看你口袋里到底是什么");           // 3  纯中文
    v.push_back("😏🌟🌙");                              // 4  纯 emoji
    v.push_back("。。。。。。。。。。");                 // 5  纯标点（全角句号）
    v.push_back("??????????");                         // 6  纯问号
    v.push_back("你把「发卡」藏在哪儿了？");             // 7  中文引号
    v.push_back("O'Brien");                            // 8  单引号（SQL 注入形态）
    v.push_back("'; DROP TABLE player_save;--");       // 9  注入串
    v.push_back("```json {\"a\":1} ```");              // 10 反引号代码围栏
    v.push_back(repeat_utf8("哈", 2000));              // 11 超长中文（2000 字）
    v.push_back(std::string(1000, 'a'));               // 12 超长 ASCII
    v.push_back("\x01\x02\x03 控制字符");              // 13 裸控制字符
    v.push_back("你\\\"/\\\\ 反斜杠与引号");            // 14 转义字符混合
    v.push_back("算了，没什么");                        // 15 普通中文
    v.push_back("1");                                  // 16 纯数字（会被当成选项）
    v.push_back("😏");                                 // 17 单 emoji
    v.push_back("我 想 知 道 真 相");                  // 18 中文带空格
    v.push_back("...");                                // 19 半角省略号
    v.push_back(repeat_utf8("\xF0\x9F\x8C\x9F", 500));  // 20 超长 emoji（4 字节 UTF-8 堆叠）
    return v;
}

int g_count = 20;
int g_wait_ms = 5600;
std::atomic<int> g_sent{0};
std::atomic<bool> g_started{false};
bool g_entry_count_reported = false;
bool g_goto1 = false;

// 把 fn 投递到 GUI 线程的事件循环里执行（跨线程安全：走 queued metacall）。
template <typename Fn>
void post_to_gui(Fn fn) {
    QMetaObject::invokeMethod(qApp, fn, Qt::QueuedConnection);
}

// 在 GUI 线程里延迟 ms 毫秒执行 fn。
template <typename Fn>
void post_delayed(int ms, Fn fn) {
    QTimer::singleShot(ms, qApp, fn);
}

template <typename T>
std::vector<T*> find_all() {
    std::vector<T*> out;
    for (QWidget* w : QApplication::allWidgets()) {
        if (T* t = qobject_cast<T*>(w)) out.push_back(t);
    }
    return out;
}

// 判断 widget 是否处在某个 QInputDialog 里（用来把"新游戏取名"的编辑框
// 和游戏主界面的输入框区分开）。
bool inside_input_dialog(QWidget* w) {
    for (QWidget* p = w; p; p = p->parentWidget()) {
        if (qobject_cast<QInputDialog*>(p)) return true;
    }
    return false;
}

// 数一下界面上到底有几个"自由输入"入口（R-04 去重后应当恒为 1）。
int count_free_input_entries() {
    int n = 0;
    for (QPushButton* b : find_all<QPushButton>()) {
        if (b->text().toStdString().find("自由输入") != std::string::npos) ++n;
    }
    return n;
}

QPushButton* find_button(const std::string& needle) {
    for (QPushButton* b : find_all<QPushButton>()) {
        if (b->text().toStdString().find(needle) != std::string::npos) return b;
    }
    return nullptr;
}

QLineEdit* find_game_input_box() {
    for (QLineEdit* e : find_all<QLineEdit>()) {
        if (!inside_input_dialog(e)) return e;
    }
    return nullptr;
}

void finish(int code, const char* reason) {
    std::printf("DRIVER_RESULT: sends=%d/%d  crash=%d  (%s)\n",
                g_sent.load(), g_count, code, reason);
    std::fflush(stdout);
    if (qApp) qApp->quit();
}

// --- 步骤 4：等回复，然后跑下一条 ------------------------------------------
void step_next(int idx);

}  // namespace

// 全局输入表（在 main 里填充，GUI 线程只读）
static std::vector<std::string> g_inputs;

namespace {

void step_after_send(int idx) {
    // 一条输入跑完（等够了 5 秒定时器的时间窗），继续下一条。
    if (idx + 1 >= g_count) {
        finish(0, "全部输入跑完，未崩溃");
        return;
    }
    step_next(idx + 1);
}

void step_do_input(int idx) {
    QPushButton* send = find_button("发送");
    QLineEdit* box = find_game_input_box();
    QPushButton* freeBtn = find_button("自由输入");

    if (!box || !send) {
        // 界面还没就绪，稍后重试。
        post_delayed(200, [idx]() { step_do_input(idx); });
        return;
    }

    if (idx == 0 && !g_entry_count_reported) {
        g_entry_count_reported = true;
        const int n = count_free_input_entries();
        std::printf("[driver] R-04 校验：界面上「自由输入」入口数 = %d（去重后应为 1）\n", n);
        std::fflush(stdout);
    }

    // 第一次需要点"自由输入"把输入框显示出来；之后输入框会自己保持可见。
    if (!box->isVisible() && freeBtn) {
        std::printf("[driver] #%d 点击「自由输入」按钮\n", idx + 1);
        freeBtn->click();
    }
    if (!send->isEnabled()) {
        // 上一条还在飞，等一等。
        post_delayed(200, [idx]() { step_do_input(idx); });
        return;
    }

    const std::string text = g_inputs[static_cast<size_t>(idx)];
    std::printf("[driver] #%d 发送(len=%zu): %s\n", idx + 1, text.size(),
                text.size() > 40 ? (text.substr(0, 40) + "...").c_str() : text.c_str());
    std::fflush(stdout);

    box->setText(QString::fromStdString(text));
    send->click();
    g_sent.fetch_add(1);

    post_delayed(g_wait_ms, [idx]() { step_after_send(idx); });
}

void step_next(int idx) { step_do_input(idx); }

// --goto1：先点第一个固定选项，从节点 0 走到带 intent_nodes 的节点 1。
// 不加这个开关时，驱动器一直停在节点 0 —— 而节点 0 压根没有 intent_nodes，
// 本来就不该有自由输入入口，所以"入口数"必须到节点 1 去数才有意义（R-04）。
void step_goto_intent_node() {
    bool clicked = false;
    for (QPushButton* b : find_all<QPushButton>()) {
        const std::string t = b->text().toStdString();
        if (t.rfind("1.", 0) == 0 && t.find("自由输入") == std::string::npos) {
            std::printf("[driver] 点击固定选项 [%s] 以便进入 intent 节点\n", t.c_str());
            std::fflush(stdout);
            b->click();
            clicked = true;
            break;
        }
    }
    if (!clicked) std::printf("[driver] 未找到固定选项，直接开始输入\n");
    post_delayed(400, []() { step_do_input(0); });
}

void dump_widgets(const char* tag) {
    std::printf("[driver] --- widget dump (%s) ---\n", tag);
    for (QWidget* w : QApplication::allWidgets()) {
        QString t;
        if (QPushButton* b = qobject_cast<QPushButton*>(w)) t = b->text();
        else if (QLineEdit* e = qobject_cast<QLineEdit*>(w)) t = QString("[edit]") + e->text();
        std::printf("    %-24s vis=%d en=%d text=[%s]\n", w->metaObject()->className(),
                    w->isVisible() ? 1 : 0, w->isEnabled() ? 1 : 0,
                    t.toStdString().c_str());
    }
    std::fflush(stdout);
}

// --- 步骤 3：关掉"请输入你的名字"对话框 ------------------------------------
void step_fill_name_dialog(int retry) {
    if (retry == 15) dump_widgets("取名对话框阶段，重试 15 次仍未找到");
    QLineEdit* dlgEdit = nullptr;
    for (QLineEdit* e : find_all<QLineEdit>()) {
        if (inside_input_dialog(e)) { dlgEdit = e; break; }
    }
    QPushButton* ok = nullptr;
    for (QPushButton* b : find_all<QPushButton>()) {
        std::string t = b->text().toStdString();
        if ((t == "OK" || t == "&OK" || t == "确定" || t == "Ok") && inside_input_dialog(b)) {
            ok = b;
            break;
        }
    }
    if (!dlgEdit || !ok) {
        post_delayed(200, [retry]() { step_fill_name_dialog(retry + 1); });
        return;
    }
    dlgEdit->setText("取证玩家");
    std::printf("[driver] 填写玩家名并确认\n");
    std::fflush(stdout);
    ok->click();
    if (g_goto1) {
        post_delayed(400, []() { step_goto_intent_node(); });
    } else {
        post_delayed(400, []() { step_do_input(0); });
    }
}

// --- 步骤 2：点"开始新游戏" ------------------------------------------------
void step_start_game() {
    QPushButton* start = find_button("开始新游戏");
    if (!start) {
        post_delayed(200, []() { step_start_game(); });
        return;
    }
    std::printf("[driver] 点击「开始新游戏」\n");
    std::fflush(stdout);
    // ⚠ 顺序关键：QInputDialog::getText 是**模态**的，click() 会一直阻塞到对话框关闭。
    // 所以"填名字"那一步必须在 click() **之前**就排进事件循环。
    post_delayed(400, []() { step_fill_name_dialog(0); });
    start->click();
}

// --- 步骤 1：等 QApplication 起来，把第一段逻辑投递进去 --------------------
void driver_thread() {
    for (int i = 0; i < 500; ++i) {
        if (QCoreApplication::instance()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!QCoreApplication::instance()) {
        std::fprintf(stderr, "[driver] QApplication 始终没起来，放弃\n");
        return;
    }
    g_started.store(true);
    // 注意：这里还在驱动线程里，**不能**直接 post_delayed（那会在本线程建 QTimer，
    // Qt 会报 "Timers can only be used with threads started with QThread"）。
    // 先用 queued metacall 切到 GUI 线程，之后所有定时都在 GUI 线程里做。
    post_to_gui([]() { post_delayed(1000, []() { step_start_game(); }); });
}

}  // namespace

int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--count") && i + 1 < argc) g_count = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--wait") && i + 1 < argc) g_wait_ms = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--goto1")) g_goto1 = true;
    }
    if (const char* c = std::getenv("FS_DRIVE_COUNT")) g_count = std::atoi(c);
    if (const char* w = std::getenv("FS_DRIVE_WAIT")) g_wait_ms = std::atoi(w);

    g_inputs = build_inputs();
    if (g_count > static_cast<int>(g_inputs.size())) g_count = static_cast<int>(g_inputs.size());

    std::printf("[driver] 目标：%d 条自由输入，每条等待 %d ms（> 5000 才能覆盖 5 秒定时器）\n",
                g_count, g_wait_ms);
    std::fflush(stdout);

    std::thread th(driver_thread);
    int rc = qt_main_main(argc, argv);
    th.join();
    std::printf("[driver] qt_main 返回 rc=%d\n", rc);
    std::fflush(stdout);
    return rc;
}
