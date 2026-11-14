// D-4 最终取证：完全复刻 src/qt_main.cpp 的 deleteLater + 5s QTimer 形态，
// 在 5 秒回调里既做裸字节读（让 ASan 下毒可见），也调 isFinished()。
#include <QApplication>
#include <QFutureWatcher>
#include <QTimer>
#include <QPointer>
#include <QtConcurrent>
#include <iostream>
#include <string>
#include <cstring>
#include <thread>
#include <chrono>
struct W : QFutureWatcher<std::string> {
    W(QObject* p=nullptr): QFutureWatcher<std::string>(p) {}
    ~W() override { std::cout << "  [dtor] watcher 已析构 this=" << (void*)this << std::endl; }
};
int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QObject host;
    W* watcher = new W(&host);
    QPointer<W> guard(watcher);
    QObject::connect(watcher, &QFutureWatcher<std::string>::finished, [&]() {
        std::cout << "[t~0.3s] finished 回调 -> watcher->deleteLater()" << std::endl;
        watcher->deleteLater();
    });
    QTimer::singleShot(5000, [&, watcher, guard]() {
        std::cout << "[t=5.0s] QPointer 仍有效? " << (guard.isNull()?"否(已析构)":"是") << std::endl;
        unsigned char b=0; std::memcpy(&b,(const void*)watcher,1);
        std::cout << "[t=5.0s] 裸读首字节=" << (int)b << " <- ASan 应在此报 heap-use-after-free" << std::endl;
        std::cout << "[t=5.0s] isFinished()=" << watcher->isFinished() << std::endl;
    });
    QFuture<std::string> f = QtConcurrent::run([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        return std::string("AI 回复");
    });
    watcher->setFuture(f);
    QTimer::singleShot(9000, [&]() { app.quit(); });
    return app.exec();
}
