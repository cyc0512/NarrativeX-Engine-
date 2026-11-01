#include <QApplication>
#include <QFutureWatcher>
#include <QPointer>
#include <QTimer>
#include <QtConcurrent>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>

// 子类化只为记录真实析构时刻 + 用 QPointer 观测存活状态
struct W : QFutureWatcher<std::string> {
    W(QObject* p = nullptr) : QFutureWatcher<std::string>(p) {}
    ~W() override { std::cout << "  [dtor] watcher 真正被析构" << std::endl; }
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QObject host;
    W* watcher = new W(&host);
    QPointer<W> guard(watcher);   // 对象析构后自动变 nullptr

    QObject::connect(watcher, &QFutureWatcher<std::string>::finished, [&]() {
        std::cout << "[t~0.3s] finished 回调 -> deleteLater()" << std::endl;
        watcher->deleteLater();
    });

    QTimer::singleShot(5000, [&, watcher, guard]() {
        std::cout << "[t=5.0s] QPointer 是否仍有效: " << (guard.isNull() ? "否(已析构)" : "是(仍存活)") << std::endl;
        std::cout << "[t=5.0s] 现在调用 watcher->isFinished() ..." << std::endl;
        bool f = watcher->isFinished();
        std::cout << "[t=5.0s] isFinished() = " << f << std::endl;
    });

    QFuture<std::string> fu = QtConcurrent::run([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        return std::string("ok");
    });
    watcher->setFuture(fu);
    QTimer::singleShot(8000, [&]() { app.quit(); });
    return app.exec();
}
