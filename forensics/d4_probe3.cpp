#include <QApplication>
#include <QFutureWatcher>
#include <QTimer>
#include <QtConcurrent>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
struct W : QFutureWatcher<std::string> {
    W(QObject* p=nullptr): QFutureWatcher<std::string>(p) {}
    ~W() override { std::cout << "  [dtor] 已析构" << std::endl; }
};
int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QObject host;
    W* watcher = new W(&host);

    // A) 直接 delete（不经 deleteLater），立刻读 -> 检验 ASan 能否看见 QObject 的 UAF
    QTimer::singleShot(200, [&, watcher]() {
        std::cout << "[A] 直接 delete watcher，随后立刻读 isFinished()" << std::endl;
        delete watcher;
        std::cout << "[A] isFinished() = " << watcher->isFinished() << std::endl;  // 期望 ASan 报警
    });
    QTimer::singleShot(3000, [&]() { app.quit(); });
    return app.exec();
}
