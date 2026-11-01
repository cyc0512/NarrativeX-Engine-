// T03 取证：D-4 悬空 QFutureWatcher 的最小复现。
//
// 复刻 src/qt_main.cpp 发送按钮里的**原始形态**（未修复前）：
//   QFutureWatcher<std::string>* watcher = new QFutureWatcher<std::string>(&window);
//   connect(watcher, &finished,   [&]{ ...; watcher->deleteLater(); });
//   QTimer::singleShot(5000, [&, watcher]{ if (watcher && watcher->isFinished() == false) {...} });
//
// 关键时序（这是 D-4 的成因，也是甲方"自由输入经常崩"的来源）：
//   t=0.3s  后台任务完成 -> finished 回调 -> watcher->deleteLater()（对象被标记析构）
//   t=5.0s  QTimer 到点 -> watcher->isFinished()  ← 这里解引用的已是**已释放**的内存
//
// 注意 `if (watcher && ...)` 这个判空**完全无效**：指针变量本身非空的，
// 它只是指向一块已经被 delete 的内存。所以只要 AI 在 5 秒内返回（正常情况！），
// 5 秒定时器就必然踩一次 use-after-free。
//
// 编译（需 Qt5Widgets / Qt5Concurrent）：
//   g++ -std=c++17 -g -fsanitize=address -fPIC $(pkg-config --cflags Qt5Widgets Qt5Concurrent) \
//       d4_watcher_uaf.cpp -o d4_watcher_uaf $(pkg-config --libs Qt5Widgets Qt5Concurrent)
//   ./d4_watcher_uaf          # 无 X 服务时： QT_QPA_PLATFORM=offscreen ./d4_watcher_uaf

#include <QApplication>
#include <QFutureWatcher>
#include <QTimer>
#include <QtConcurrent>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QObject host;   // 对应 qt_main.cpp 里的 window，用来托管 watcher

    QFutureWatcher<std::string> *watcher = new QFutureWatcher<std::string>(&host);

    QObject::connect(watcher, &QFutureWatcher<std::string>::finished, [&]() {
        std::cout << "[t~0.3s] finished 回调：拿到结果，watcher->deleteLater()" << std::endl;
        watcher->deleteLater();      // ← 对象在此被交还给事件循环析构
    });

    // 5 秒超时保护 —— 与 src/qt_main.cpp 完全同形
    QTimer::singleShot(5000, [&, watcher]() {
        std::cout << "[t=5.0s] 超时定时器触发，即将读 watcher->isFinished() ..." << std::endl;
        if (watcher && watcher->isFinished() == false) {   // ← 悬空解引用点
            std::cout << "[t=5.0s] 走了超时分支" << std::endl;
            watcher->disconnect();
            watcher->deleteLater();
        } else {
            std::cout << "[t=5.0s] 未走超时分支（但 isFinished() 依然被调用了）" << std::endl;
        }
    });

    QFuture<std::string> future = QtConcurrent::run([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));   // 0.3 秒，远快于 5 秒
        return std::string("AI 回复");
    });
    watcher->setFuture(future);

    QTimer::singleShot(8000, [&]() { app.quit(); });
    std::cout << "=== 等待 5 秒定时器... ===" << std::endl;
    return app.exec();
}
