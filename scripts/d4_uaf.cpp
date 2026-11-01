// D-4 最小复现：src/qt_main.cpp 第 369~405 行的「QFutureWatcher + 5 秒 QTimer」形态。
//
// ⚠️ 这不是"看着像"的示意代码，而是逐行转录：变量生命周期、父对象、
//    deleteLater 的位置、singleShot 的捕获方式都与生产代码一致。
//    唯二的差别：
//      1) 后台任务用 sleep 代替真实的 AiEngine HTTP 调用 —— 这样可以
//         用命令行参数精确控制"AI 多久回"，把快慢两条路径都变成确定性事件；
//      2) 回调体里用 printf 代替 dialogBox->setText（无 X 服务，且便于取证）。
//    这两处替换都不影响被检验的对象：watcher 的生命周期。
//
// 用法：
//   ./d4_uaf <AI 响应毫秒数> [总运行时长毫秒]
//   ./d4_uaf 300      # AI 快（0.3s）→ 检验"AI 比 5 秒快"这条路径
//   ./d4_uaf 8000     # AI 慢（8s）  → 检验"AI 比 5 秒慢"这条路径
#include <QApplication>
#include <QFutureWatcher>
#include <QFuture>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_ADDRESS)
#include <sanitizer/asan_interface.h>
#define FS_HAVE_ASAN_INTERFACE 1
#endif

namespace {
// 带析构日志的 QFutureWatcher：用来拿到"watcher 到底死没死"的地面实况。
// QPointer 是 Qt 官方的"对象还活着吗"探针——对象一析构它立刻变 null。
struct Watched : QFutureWatcher<std::string> {
    explicit Watched(QObject *p = nullptr) : QFutureWatcher<std::string>(p) {}
    ~Watched() override {
        std::printf("  [dtor] watcher 已析构, this=%p\n", (void *)this);
    }
};

double ElapsedMs() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - t0).count();
}
}  // namespace

int main(int argc, char *argv[]) {
    // 关掉 stdout 缓冲：ASan 报错时会直接 abort，缓冲里的 printf 会整段丢失，
    // 那样就看不出"到底是 isFinished() 还是后面的探针被拦下"了。
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    QApplication app(argc, argv);

    const int aiDelayMs = (argc > 1) ? std::atoi(argv[1]) : 300;
    const int runMs = (argc > 2) ? std::atoi(argv[2]) : (aiDelayMs + 4000);

    std::printf("[setup] AI 响应延时 = %d ms，本次总运行 = %d ms\n", aiDelayMs, runMs);
    std::printf("[setup] 5 秒超时回调固定发生在 t=5000 ms\n\n");

    QObject host;  // 对应 qt_main.cpp 里的 `&window`（watcher 的父对象）

    // ---- 以下对应 qt_main.cpp:369 ----
    Watched *watcher = new Watched(&host);
    QPointer<Watched> guard(watcher);  // 仅取证观测用；生产代码里没有它，这才是问题所在
    std::printf("[t=%4.0fms] watcher = %p 已创建（parent=&host）\n",
                ElapsedMs(), (void *)watcher);

    // ---- 对应 qt_main.cpp:371-387 ----
    QObject::connect(watcher, &QFutureWatcher<std::string>::finished, [&, watcher]() {
        std::printf("[t=%4.0fms] finished 回调：AI 已返回\n", ElapsedMs());
        // 对应第 386 行：watcher 在这里被排进延迟删除队列
        watcher->deleteLater();
        std::printf("[t=%4.0fms] finished 回调：已调用 watcher->deleteLater()\n", ElapsedMs());
    });

    // ---- 对应 qt_main.cpp:390-399 ----
    // 注意：singleShot 没绑 context object，所以 watcher 死没死它都会响。
    QTimer::singleShot(5000, [&, watcher, guard]() {
        std::printf("\n[t=%4.0fms] 5 秒超时回调触发\n", ElapsedMs());
        std::printf("           裸指针 watcher       = %p（悬垂指针也" "是非空）\n",
                    (void *)watcher);
        std::printf("           QPointer guard 有效? = %s  <- 地面实况：对象还活着吗\n",
                    guard.isNull() ? "否（已析构）" : "是（仍存活）");
        // 对应第 391 行：`if (watcher && watcher->isFinished() == false)`
        // 这里的 watcher->isFinished() 会去读 watcher 的私有数据 d->state。
        // 若 watcher 已被 deleteLater 析构，这就是一次 heap-use-after-free 读。
        // 这一行就是 qt_main.cpp:391 的那一次解引用，先执行它，
        // 好让 ASan 报告里 #0 帧指向真正出问题的那一行。
        const bool finished = watcher->isFinished();
        std::printf("           watcher->isFinished() = %s（读到的是 %s 内存）\n",
                    finished ? "true" : "false",
                    guard.isNull() ? "已释放" : "仍有效");
#ifdef FS_HAVE_ASAN_INTERFACE
        std::printf("           __asan_address_is_poisoned(watcher) = %d  "
                    "(1=ASan 认定该地址已释放)\n",
                    __asan_address_is_poisoned((void *)watcher));
        unsigned char firstByte = 0;
        std::memcpy(&firstByte, (const void *)watcher, 1);  // 兜底裸读，确保下毒可见
        std::printf("           裸读首字节 = 0x%02X\n", firstByte);
#endif
        if (watcher && finished == false) {
            std::printf("           进入超时分支 -> 继续 disconnect()/deleteLater()，"
                        "对已释放对象做写操作\n");
            watcher->disconnect();
            watcher->deleteLater();
        } else {
            std::printf("           未进入超时分支\n");
        }
    });

    // ---- 对应 qt_main.cpp:402-405 ----
    QFuture<std::string> future = QtConcurrent::run([aiDelayMs]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(aiDelayMs));
        return std::string("AI 回复");
    });
    watcher->setFuture(future);

    QTimer::singleShot(runMs, [&]() {
        std::printf("\n[end] 运行结束，退出事件循环\n");
        app.quit();
    });

    return app.exec();
}
