// FS GUI 自动驾驶（取证 / 回归用，不属于生产代码）
//
// 目的：在无 X 服务的 Ubuntu 上，驱动**真实的 src/qt_main.cpp 二进制**，
//       自动完成「新建玩家 -> 唤起自由输入 -> 输入 -> 发送」若干轮，
//       从而对真实的 5 秒 QTimer + QFutureWatcher 路径做崩溃取证与回归。
//
// 手法：LD_PRELOAD 一个覆盖 `QCoreApplication::exec()`（mangled:
//       _ZN16QCoreApplication4execEv，它是 static 成员，无 this 指针）的 .so。
//       在真正进入事件循环之前挂上 QTimer 自动化脚本。
//       —— 这样**一行生产代码都不用改**，测的就是原汁原味的 fs 二进制。
//
// 编译：
//   g++ -std=gnu++17 -fPIC -shared -o build/libfs_autopilot.so scripts/ui_autopilot.cpp \
//       -I/usr/include/x86_64-linux-gnu/qt5 -I.../QtCore -I.../QtGui -I.../QtWidgets \
//       -DQT_CORE_LIB -DQT_GUI_LIB -DQT_WIDGETS_LIB -lQt5Widgets -lQt5Gui -lQt5Core -ldl
//
// 运行：
//   QT_QPA_PLATFORM=offscreen LD_PRELOAD=./libfs_autopilot.so \
//   FS_AP_ROUNDS=20 FS_AP_GAP=6000 ./fs
//
// 环境变量：
//   FS_AP_ROUNDS  轮数（默认 20）
//   FS_AP_GAP     每轮间隔毫秒（默认 6000，必须 > 5000 才能覆盖 5 秒超时回调）
//   FS_AP_NAME    新玩家名字（默认 fs_apilot）
#include <dlfcn.h>

#include <QApplication>
#include <QCoreApplication>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSet>
#include <QTimer>
#include <QWidget>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using ExecFn = int (*)();

ExecFn g_realExec = nullptr;

int EnvInt(const char *key, int def) {
    const char *v = std::getenv(key);
    return (v && *v) ? std::atoi(v) : def;
}

std::string EnvStr(const char *key, const char *def) {
    const char *v = std::getenv(key);
    return (v && *v) ? std::string(v) : std::string(def);
}

// 界面上所有"当前可见"的按钮。refreshUI 每轮都会重建按钮，
// 所以每次都重新查，不缓存指针。
QList<QPushButton *> VisibleButtons() {
    QList<QPushButton *> out;
    // ⚠️ 必须去重：对 allWidgets() 里每个 widget 调 findChildren 是**递归**的，
    //    同一个按钮会在它的每一个祖先那里被数到一次（实测数出 4 个其实是 1 个）。
    QSet<QPushButton *> seen;
    for (QWidget *w : QApplication::allWidgets()) {
        if (!w || !w->isVisible()) continue;
        for (QPushButton *b : w->findChildren<QPushButton *>()) {
            if (!b->isVisible() || b->text().trimmed().isEmpty()) continue;
            if (seen.contains(b)) continue;
            seen.insert(b);
            out << b;
        }
    }
    return out;
}

QPushButton *FindButton(const QString &needle) {
    for (QPushButton *b : VisibleButtons()) {
        if (b->text().contains(needle)) return b;
    }
    return nullptr;
}

QLineEdit *VisibleInputBox() {
    for (QWidget *w : QApplication::allWidgets()) {
        if (!w || !w->isVisible()) continue;
        for (QLineEdit *e : w->findChildren<QLineEdit *>()) {
            if (e->isVisible()) return e;
        }
    }
    return nullptr;
}

// 对话框文本框：geometry(50,250,900,350) 那个 dialogBox。
QLabel *DialogLabel() {
    for (QWidget *w : QApplication::allWidgets()) {
        if (!w) continue;
        for (QLabel *l : w->findChildren<QLabel *>()) {
            if (l->geometry() == QRect(50, 250, 900, 350)) return l;
        }
    }
    return nullptr;
}

// 20 轮自由输入的语料：空串 / 超长 500 字 / 纯标点 / 纯中文 / emoji
std::vector<std::string> BuildInputs(int rounds) {
    std::vector<std::string> longText;
    std::string long1;
    for (int i = 0; i < 500; ++i) long1 += "\xE5\x93\x88";  // 500 个"哈"
    std::string punct = "。。。。！！？？，，；；……——「」『』（）";
    std::string cn = "我想看看你围裙口袋里到底藏了什么东西";
    std::string emoji = "\xF0\x9F\x98\x8F\xF0\x9F\x8C\x99\xF0\x9F\x92\x95";  // 😏🌙💕
    std::string mixed = "\xE5\x93\x88\xE5\x93\x88" + punct + emoji;

    std::vector<std::string> seeds = {"", long1, punct, cn, emoji, mixed};
    std::vector<std::string> out;
    for (int i = 0; i < rounds; ++i) out.push_back(seeds[i % seeds.size()]);
    return out;
}

int g_round = 0;
int g_rounds = 20;
int g_gap = 6000;
std::string g_name = "fs_apilot";
std::vector<std::string> g_inputs;
int g_sent = 0;
int g_skippedEmpty = 0;

void Finish() {
    std::printf("\n[autopilot] ===== 结束 =====\n");
    std::printf("[autopilot] 计划轮数 = %d，实际点击发送 = %d，"
                "空串被 UI 拦截(未进入异步路径) = %d\n",
                g_rounds, g_sent, g_skippedEmpty);
    std::printf("[autopilot] 进程仍然存活 —— 未崩溃\n");
    std::fflush(stdout);
    QCoreApplication::quit();
}

void Cycle();

void DoSend() {
    QLineEdit *box = VisibleInputBox();
    QPushButton *send = FindButton(QString::fromUtf8("\xE5\x8F\x91\xE9\x80\x81"));  // "发送"
    if (!box || !send) {
        std::printf("[autopilot] 第 %d 轮：找不到可见输入框或发送按钮，跳过\n", g_round + 1);
        QTimer::singleShot(g_gap, &Cycle);
        return;
    }

    const std::string &text = g_inputs[g_round];
    box->setText(QString::fromStdString(text));

    // 复刻 qt_main.cpp:349 的行为：空串在这里就被 return 掉了，
    // 根本不会进入异步路径。如实记录，不粉饰。
    if (text.empty()) {
        ++g_skippedEmpty;
        std::printf("[autopilot] 第 %2d 轮：输入为空串 -> UI 层 qt_main.cpp:349 直接 return，"
                    "未进入 AI 异步路径\n", g_round + 1);
    } else {
        std::printf("[autopilot] 第 %2d 轮：发送 %zu 字符", g_round + 1, text.size());
        std::printf("（首字节 %02X）\n", (unsigned char)text[0]);
    }
    std::fflush(stdout);

    send->click();
    ++g_sent;
    ++g_round;

    if (g_round >= g_rounds) {
        // 最后多等一个 5 秒超时窗口 + 余量，确认超时回调不会炸。
        QTimer::singleShot(g_gap + 1500, &Finish);
    } else {
        QTimer::singleShot(g_gap, &Cycle);
    }
}

void Cycle() {
    // 若输入框已可见，直接填字发送；否则先点「自由输入」唤起它。
    if (VisibleInputBox() != nullptr) {
        DoSend();
        return;
    }
    QPushButton *freeBtn = FindButton(QString::fromUtf8("\xE8\x87\xAA\xE7\x94\xB1\xE8\xBE\x93\xE5\x85\xA5"));
    // "自由输入"
    if (freeBtn) {
        std::printf("[autopilot] 第 %2d 轮：点击「%s」唤起输入框\n",
                    g_round + 1, freeBtn->text().toStdString().c_str());
        std::fflush(stdout);
        freeBtn->click();
        QTimer::singleShot(300, &DoSend);
    } else {
        std::printf("[autopilot] 第 %2d 轮：既无可见输入框也无自由输入按钮，跳过\n", g_round + 1);
        ++g_round;
        if (g_round >= g_rounds) QTimer::singleShot(g_gap + 1500, &Finish);
        else QTimer::singleShot(g_gap, &Cycle);
    }
}

void Cycle();

// 剧本里的节点 0 只有一个普通选项，要先推进到带 intent_nodes 的节点 1
// 才有自由输入入口。这里做通用推进：一路点"既不是系统按钮也不是发送"的选项按钮。
void AdvanceToFreeInput() {
    static int hops = 0;
    ++hops;

    const QString kFree = QString::fromUtf8("\xE8\x87\xAA\xE7\x94\xB1\xE8\xBE\x93\xE5\x85\xA5");  // 自由输入
    if (FindButton(kFree) != nullptr) {
        // R-04 核查：把当前界面上所有含「自由输入」的按钮列出来，
        // 正常情况下应该**只有一个**。
        int n = 0;
        for (QPushButton *b : VisibleButtons()) {
            if (b->text().contains(kFree)) {
                ++n;
                std::printf("[autopilot][R-04] 自由输入入口 #%d: 「%s」\n",
                            n, b->text().toStdString().c_str());
            }
        }
        std::printf("[autopilot][R-04] 自由输入入口总数 = %d（期望 1）\n", n);
        std::printf("[autopilot] 推进 %d 跳后到达可自由输入的节点\n", hops - 1);
        std::fflush(stdout);
        QTimer::singleShot(300, &Cycle);
        return;
    }
    if (hops > 12) {
        std::printf("[autopilot] 推进 12 跳仍未找到自由输入入口，放弃\n");
        std::fflush(stdout);
        Finish();
        return;
    }

    static const char *kSystemButtons[] = {
        "\xE5\x8F\x91\xE9\x80\x81",                          // 发送
        "\xE8\x8F\x9C\xE5\x8D\x95",                          // 菜单
        "\xE8\xBF\x94\xE5\x9B\x9E\xE4\xB8\xBB\xE8\x8F\x9C\xE5\x8D\x95",  // 返回主菜单
        "\xE9\x80\x80\xE5\x87\xBA\xE6\xB8\xB8\xE6\x88\x8F",  // 退出游戏
        "\xE8\xAF\xBB\xE5\x8F\x96\xE5\xAD\x98\xE6\xA1\xA3",  // 读取存档
        "\xE5\xBC\x80\xE5\xA7\x8B\xE6\x96\xB0\xE6\xB8\xB8\xE6\x88\x8F",  // 开始新游戏
    };
    for (QPushButton *b : VisibleButtons()) {
        bool isSystem = false;
        for (const char *s : kSystemButtons) {
            if (b->text().contains(QString::fromUtf8(s))) { isSystem = true; break; }
        }
        if (isSystem) continue;
        std::printf("[autopilot] 推进第 %d 跳：点击「%s」\n",
                    hops, b->text().toStdString().c_str());
        std::fflush(stdout);
        b->click();
        QTimer::singleShot(400, &AdvanceToFreeInput);
        return;
    }
    std::printf("[autopilot] 第 %d 跳：无可点击的选项按钮\n", hops);
    std::fflush(stdout);
    QTimer::singleShot(400, &AdvanceToFreeInput);
}

void AfterNewGame() {
    std::printf("[autopilot] 已进入游戏界面，开始自由输入循环\n");
    std::fflush(stdout);
    QTimer::singleShot(500, &AdvanceToFreeInput);
}

void FillNameDialog() {
    static int tries = 0;
    ++tries;
    QWidget *modal = QApplication::activeModalWidget();
    QInputDialog *dlg = qobject_cast<QInputDialog *>(modal);

    if (!dlg) {
        // 兜底：activeModalWidget() 在 offscreen 平台下可能拿不到，直接遍历找。
        for (QWidget *w : QApplication::allWidgets()) {
            QInputDialog *cand = qobject_cast<QInputDialog *>(w);
            if (cand && cand->isVisible()) {
                dlg = cand;
                break;
            }
        }
    }
    if (tries <= 3 || tries % 20 == 0) {
        std::printf("[autopilot] 第 %d 次找输入名字对话框: activeModal=%p cls=%s -> dlg=%p\n",
                    tries, (void *)modal,
                    modal ? modal->metaObject()->className() : "null", (void *)dlg);
        std::fflush(stdout);
    }
    if (!dlg) {
        if (tries > 150) {
            std::printf("[autopilot] 放弃等待输入名字对话框\n");
            std::fflush(stdout);
            QTimer::singleShot(400, &AfterNewGame);
            return;
        }
        // 对话框还没弹出来，再等等
        QTimer::singleShot(200, &FillNameDialog);
        return;
    }
    dlg->setTextValue(QString::fromStdString(g_name));
    std::printf("[autopilot] 填写玩家名 = %s\n", g_name.c_str());
    std::fflush(stdout);
    dlg->accept();
    QTimer::singleShot(400, &AfterNewGame);
}

void ClickStart() {
    QPushButton *start = FindButton(QString::fromUtf8("\xE5\xBC\x80\xE5\xA7\x8B\xE6\x96\xB0\xE6\xB8\xB8\xE6\x88\x8F"));
    // "开始新游戏"
    if (!start) {
        QTimer::singleShot(200, &ClickStart);
        return;
    }
    std::printf("[autopilot] 点击「开始新游戏」\n");
    std::fflush(stdout);
    // ⚠️ 顺序至关重要：click() 会同步弹起 QInputDialog 并在其嵌套事件循环里阻塞，
    //    必须**先**把后续步骤挂上定时器，再去 click，否则永远排不上。
    QTimer::singleShot(300, &FillNameDialog);
    start->click();
}

void Install() {
    g_rounds = EnvInt("FS_AP_ROUNDS", 20);
    g_gap = EnvInt("FS_AP_GAP", 6000);
    g_name = EnvStr("FS_AP_NAME", "fs_apilot");
    g_inputs = BuildInputs(g_rounds);
    std::printf("[autopilot] 已挂载：rounds=%d gap=%dms name=%s\n",
                g_rounds, g_gap, g_name.c_str());
    std::printf("[autopilot] 注意：gap 必须 > 5000ms，才能覆盖 qt_main.cpp 的 5 秒超时回调\n");
    std::fflush(stdout);
    QTimer::singleShot(300, &ClickStart);
}

}  // namespace

// —— LD_PRELOAD 的注入点：覆盖 QCoreApplication::exec() ——
extern "C" int _ZN16QCoreApplication4execEv() {
    if (!g_realExec) {
        g_realExec = reinterpret_cast<ExecFn>(
            dlsym(RTLD_NEXT, "_ZN16QCoreApplication4execEv"));
    }
    static bool installed = false;
    if (!installed) {
        installed = true;
        Install();
    }
    if (!g_realExec) {
        std::fprintf(stderr, "[autopilot] FATAL: 找不到真实的 QCoreApplication::exec()\n");
        return 1;
    }
    return g_realExec();
}
