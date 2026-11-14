#include <QApplication>
#include <QFutureWatcher>
#include <QTimer>
#include <QPointer>
#include <iostream>
#include <string>
#include <cstring>
struct W : QFutureWatcher<std::string> {
    W(QObject* p=nullptr): QFutureWatcher<std::string>(p) {}
    ~W() override { std::cout << "  [dtor] 已析构, this=" << (void*)this << std::endl; }
};
int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QObject host;
    W* w = new W(&host);
    QPointer<W> g(w);
    QTimer::singleShot(200, [&, w, g]() {
        std::cout << "[1] this=" << (void*)w << std::endl;
        delete w;
        std::cout << "[2] delete 完成; QPointer 有效? " << (g.isNull()?"否(已析构)":"是") << std::endl;
        unsigned char b = 0;
        std::memcpy(&b, (const void*)w, 1);       // 直接读 1 字节，检验 ASan 是否给这块内存下毒
        std::cout << "[3] 裸读 1 字节 = " << (int)b << "  (若 ASan 生效此处应报错)" << std::endl;
        std::cout << "[4] w->isFinished() = " << w->isFinished() << std::endl;
    });
    QTimer::singleShot(2500, [&]() { app.quit(); });
    return app.exec();
}
