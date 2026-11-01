#include <QApplication>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QTextEdit>
#include <QFont>
#include <QMessageBox>
#include <QInputDialog>
#include <QPixmap>
#include <QImage>
#include <QStackedWidget>
#include <QMenu>
#include <QAction>
#include <QPoint>
#include <QPointer>
#include <QTimer>
#include <functional>
#include <memory>
#include <vector>
#include <string>
#include "GameEngine.h"
#include "SecurityFilter.h"
#include <QtConcurrent>
#include <QFutureWatcher>

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    GameEngine engine("../data/story.json");
    SecurityFilter security;

    // 构造失败必须显式暴露：以前是静默的，随后任何数据库操作都会 SIGSEGV。
    if (!engine.ready()) {
        QMessageBox::critical(nullptr, "FS · 启动失败",
                              QString::fromStdString(engine.lastError()) +
                              "\n\n请把工作目录切到 build/ 或项目根目录后重试。");
        return 1;
    }

    QWidget window;
    window.setWindowTitle("FS · FluidStory");
    window.resize(1000, 700); 
    window.setStyleSheet("background-color: #1a1a1a;");

    // 背景图
    QLabel *bgLabel = new QLabel(&window);
    bgLabel->setGeometry(0, 0, 1000, 700);
    QImage bgImage("../assets/bg.jpg");
    if (!bgImage.isNull()) {
        QPixmap bgPixmap = QPixmap::fromImage(bgImage.scaled(1000, 700, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
        bgLabel->setPixmap(bgPixmap);
        bgLabel->setScaledContents(true);
    } else {
        bgLabel->setText("【图片未找到】");
        bgLabel->setAlignment(Qt::AlignCenter);
    }

    QStackedWidget *stackedWidget = new QStackedWidget(&window);
    stackedWidget->setGeometry(0, 0, 1000, 700);
    stackedWidget->setStyleSheet("background: transparent;");

    // ==========================================
    // 页面 1：主菜单
    // ==========================================
    QWidget *mainMenuPage = new QWidget();
    QVBoxLayout *mainMenuLayout = new QVBoxLayout(mainMenuPage);
    mainMenuLayout->setAlignment(Qt::AlignCenter);

    QLabel *titleLabel = new QLabel("FS", mainMenuPage);
    titleLabel->setFont(QFont("Microsoft YaHei", 60, QFont::Bold));
    titleLabel->setStyleSheet("color: white; margin-bottom: 0px;");
    titleLabel->setAlignment(Qt::AlignCenter);
    mainMenuLayout->addWidget(titleLabel);

    QLabel *subTitleLabel = new QLabel("FluidStory Demo", mainMenuPage);
    subTitleLabel->setFont(QFont("Microsoft YaHei", 16));
    subTitleLabel->setStyleSheet("color: #bdc3c7; margin-bottom: 40px;");
    subTitleLabel->setAlignment(Qt::AlignCenter);
    mainMenuLayout->addWidget(subTitleLabel);

    QPushButton *startBtn = new QPushButton("开始新游戏", mainMenuPage);
    startBtn->setFixedSize(250, 50);
    startBtn->setStyleSheet("QPushButton { background-color: rgba(30, 30, 30, 200); color: white; font-size: 18px; border-radius: 10px; border: 1px solid #7f8c8d; } QPushButton:hover { background-color: rgba(52, 152, 219, 220); }");
    mainMenuLayout->addWidget(startBtn, 0, Qt::AlignCenter);

    QPushButton *openLoadPanelBtn = new QPushButton("读取存档", mainMenuPage);
    openLoadPanelBtn->setFixedSize(250, 50);
    openLoadPanelBtn->setStyleSheet(startBtn->styleSheet());
    mainMenuLayout->addWidget(openLoadPanelBtn, 0, Qt::AlignCenter);

    QPushButton *quitBtn = new QPushButton("退出游戏", mainMenuPage);
    quitBtn->setFixedSize(250, 50);
    quitBtn->setStyleSheet(startBtn->styleSheet());
    mainMenuLayout->addWidget(quitBtn, 0, Qt::AlignCenter);

    // ==========================================
    // 存档面板
    // ==========================================
    QWidget *loadPanel = new QWidget(&window);
    loadPanel->setGeometry(150, 100, 700, 500);
    loadPanel->setStyleSheet("background-color: rgba(20, 20, 20, 240); border-radius: 15px; border: 2px solid #7f8c8d;");
    loadPanel->hide();

    QVBoxLayout *loadPanelLayout = new QVBoxLayout(loadPanel);
    QLabel *loadTitle = new QLabel("选择要读取的存档", loadPanel);
    loadTitle->setFont(QFont("Microsoft YaHei", 24, QFont::Bold));
    loadTitle->setStyleSheet("color: white;");
    loadTitle->setAlignment(Qt::AlignCenter);
    loadPanelLayout->addWidget(loadTitle);

    QVBoxLayout *slotsLayout = new QVBoxLayout();
    loadPanelLayout->addLayout(slotsLayout);

    QPushButton *closePanelBtn = new QPushButton("返回主菜单", loadPanel);
    closePanelBtn->setFixedSize(200, 40);
    closePanelBtn->setStyleSheet("background-color: #c0392b; color: white; border-radius: 5px; font-weight: bold;");
    loadPanelLayout->addWidget(closePanelBtn, 0, Qt::AlignCenter);

    // ==========================================
    // 页面 2：游戏界面
    // ==========================================
    QWidget *gamePage = new QWidget();
    
    QLabel *dialogBox = new QLabel(gamePage);
    dialogBox->setGeometry(50, 250, 900, 350); 
    dialogBox->setFont(QFont("Microsoft YaHei", 20)); 
    dialogBox->setStyleSheet("background-color: rgba(0, 0, 0, 180); color: white; border-radius: 15px; padding: 30px; border: 2px solid #34495e;");
    dialogBox->setWordWrap(true);
    dialogBox->setAlignment(Qt::AlignTop | Qt::AlignLeft); 

    QWidget *optionsWidget = new QWidget(gamePage);
    optionsWidget->setGeometry(250, 450, 500, 180); 
    optionsWidget->setStyleSheet("background: transparent;");
    QVBoxLayout *optionsLayout = new QVBoxLayout(optionsWidget);
    optionsLayout->setContentsMargins(0, 0, 0, 0);
    optionsWidget->hide();

    QWidget *inputWidget = new QWidget(gamePage);
    inputWidget->setGeometry(50, 620, 900, 50);
    QHBoxLayout *inputLayout = new QHBoxLayout(inputWidget);
    inputLayout->setContentsMargins(0, 0, 0, 0);

    QLineEdit *inputBox = new QLineEdit(inputWidget);
    inputBox->setFont(QFont("Microsoft YaHei", 16));
    inputBox->setStyleSheet("background-color: rgba(255, 255, 255, 230); border-radius: 8px; padding: 5px 10px;");
    
    QPushButton *sendBtn = new QPushButton("发送", inputWidget);
    sendBtn->setStyleSheet("background-color: #e74c3c; color: white; font-size: 16px; font-weight: bold; padding: 8px 20px; border-radius: 8px;");
    
    inputLayout->addWidget(inputBox);
    inputLayout->addWidget(sendBtn);
    inputWidget->hide();

    QPushButton *sysMenuBtn = new QPushButton("⚙ 菜单", gamePage);
    sysMenuBtn->setGeometry(850, 20, 100, 40);
    sysMenuBtn->setStyleSheet(
        "QPushButton { background-color: rgba(0, 0, 0, 180); color: white; border-radius: 8px; font-weight: bold; border: 1px solid #555; }"
        "QPushButton:hover { background-color: rgba(52, 152, 219, 220); }"
    );

    QMenu *systemMenu = new QMenu();
    QAction *saveAction = systemMenu->addAction("保存进度");
    QAction *backToMenuAction = systemMenu->addAction("返回主菜单");
    
    QObject::connect(sysMenuBtn, &QPushButton::clicked, [&, systemMenu, sysMenuBtn]() {
        systemMenu->exec(sysMenuBtn->mapToGlobal(QPoint(0, sysMenuBtn->height())));
    });

    QObject::connect(saveAction, &QAction::triggered, [&]() {
        engine.applyPenalty(0);
        QMessageBox::information(&window, "FS · 系统", "进度已保存！");
    });

    QObject::connect(backToMenuAction, &QAction::triggered, [&]() {
        engine.applyPenalty(0); 
        stackedWidget->setCurrentWidget(mainMenuPage);
    });

    // ==========================================
    // 自由输入的**唯一**唤起入口（R-04）
    //
    // 以前有两个入口同时存在：
    //   ① 剧本 data/story.json 里写死的那个选项「3. 【自由输入】我想自己说点什么...」，
    //      它走的是"选项按钮"的分支，点了就唤起输入框；
    //   ② 代码里 hasIntent 为真时又强制生成的一个「【自由输入】对她说点什么...」按钮。
    // 甲方看到的"两个自由输入"就是这么来的。
    // 处置：剧本里那个选项删掉（自由输入是引擎能力，不该写成剧本选项），
    //      代码里只留下面这一个 showInput()，所有唤起路径都走它。
    // ==========================================
    std::function<void()> showInput;
    showInput = [&]() {
        inputWidget->show();
        inputBox->setFocus();
        for (int j = 0; j < optionsLayout->count(); ++j) {
            QLayoutItem *it = optionsLayout->itemAt(j);
            if (it && it->widget()) it->widget()->hide();
        }
    };

    // ==========================================
    // 核心逻辑：刷新界面
    // ==========================================
    std::function<void()> refreshUI;
    refreshUI = [&]() {
        QLayoutItem *item;
        while ((item = optionsLayout->takeAt(0)) != nullptr) {
            if (item->widget()) item->widget()->deleteLater();
            delete item;
        }

        optionsWidget->hide();
        inputWidget->hide();

        if (engine.isGameOver()) {
            dialogBox->setText("【游戏结束】");
            return;
        }

        std::vector<std::string> pages = engine.getCurrentNodePages();
        if (pages.empty()) return;
        dialogBox->setText(QString::fromStdString(pages[0])); 

        std::vector<std::string> currentOpts = engine.getCurrentOptions();
        bool hasIntent = engine.hasIntentNodes(); // 引擎提供的全新判断接口
        
        if (currentOpts.empty() && !hasIntent) {
            // 纯文本节点，没有任何操作，显示继续
            optionsWidget->show();
            QPushButton *continueBtn = new QPushButton("点击继续...", gamePage);
            continueBtn->setStyleSheet(
                "QPushButton { background-color: rgba(52, 152, 219, 220); color: white; padding: 12px 20px; border-radius: 10px; font-size: 16px; font-weight: bold; }"
                "QPushButton:hover { background-color: rgba(41, 128, 185, 255); }"
            );
            optionsLayout->addWidget(continueBtn);
            
            QObject::connect(continueBtn, &QPushButton::clicked, [&]() {
                engine.processInput(" "); 
                refreshUI();
            });
        } else {
            optionsWidget->show();
            
            // 1. 渲染 JSON 里的固定选项
            for (size_t i = 0; i < currentOpts.size(); ++i) {
                QString btnText = QString::fromStdString(currentOpts[i]);
                QPushButton *btn = new QPushButton(btnText, gamePage);
                
                btn->setStyleSheet(
                    "QPushButton { background-color: rgba(30, 30, 30, 200); color: #ecf0f1; padding: 12px 20px; border-radius: 10px; border: 1px solid #7f8c8d; text-align: left; font-family: 'Microsoft YaHei'; font-size: 16px; }"
                    "QPushButton:hover { background-color: rgba(52, 152, 219, 220); border: 1px solid #3498db; }"
                );
                optionsLayout->addWidget(btn);
                
                // 固定选项一律走 processInput(序号)，不再做文本特征猜测。
                // ⚠️ 旧代码这里有一段"按钮文字含 自由输入 **或者** 含 3. 就当自由输入入口"的特判。
                //    后半条是给旧剧本擦屁股的：任何文字里带 "3." 的选项都会被误判成自由输入。
                //    R-04 删掉剧本里那个写死的自由输入选项后它已无匹配对象，
                //    只剩误伤，故整段特判删除 —— 自由输入入口只保留下面 hasIntent 那一处。
                QObject::connect(btn, &QPushButton::clicked, [&, i]() {
                    engine.processInput(std::to_string(i + 1));
                    refreshUI();
                });
            }

            // 2. 强制生成自由输入按钮（如果该节点有隐藏 AI 分支）
            if (hasIntent) {
                QPushButton *btn = new QPushButton("【自由输入】对她说点什么...", gamePage);
                btn->setStyleSheet(
                    "QPushButton { background-color: rgba(230, 126, 34, 200); color: white; padding: 12px 20px; border-radius: 10px; border: 1px solid #e67e22; text-align: left; font-family: 'Microsoft YaHei'; font-size: 16px; font-weight: bold; }"
                    "QPushButton:hover { background-color: rgba(211, 84, 0, 255); }"
                );
                optionsLayout->addWidget(btn);

                QObject::connect(btn, &QPushButton::clicked, [&]() {
                    showInput();
                });
            }
        }
    };

    // ==========================================
    // 刷新存档面板
    // ==========================================
    std::function<void()> refreshLoadPanel;
    refreshLoadPanel = [&]() {
        QLayoutItem *item;
        while ((item = slotsLayout->takeAt(0)) != nullptr) {
            if (item->widget()) item->widget()->deleteLater();
            delete item;
        }

        std::vector<DatabaseManager::SaveInfo> saves = engine.getAllSaves();

        for (int i = 0; i < 3; ++i) {
            QWidget *slotWidget = new QWidget();
            slotWidget->setFixedSize(600, 80);
            slotWidget->setStyleSheet("background-color: #34495e; border-radius: 8px;");

            if (i < saves.size()) {
                DatabaseManager::SaveInfo info = saves[i];
                
                QPushButton *loadBtn = new QPushButton(slotWidget);
                loadBtn->setGeometry(0, 0, 600, 80);
                loadBtn->setStyleSheet("QPushButton { background: transparent; border-radius: 8px; } QPushButton:hover { background-color: #3498db; }");
                
                QVBoxLayout *slotLayout = new QVBoxLayout(loadBtn);
                slotLayout->setContentsMargins(20, 10, 20, 10);
                slotLayout->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

                QLabel *line1 = new QLabel(QString::fromStdString("玩家: " + info.playerName + "  |  好感度: " + std::to_string(info.affinity)), loadBtn);
                line1->setStyleSheet("color: white; font-size: 18px; font-weight: bold; background: transparent;");
                
                QString timeStr = QString::fromStdString(info.lastLogin);
                if (timeStr.trimmed().isEmpty()) timeStr = "时间读取失败";

                QLabel *line2 = new QLabel("最后存档时间: " + timeStr, loadBtn);
                line2->setStyleSheet("color: #bdc3c7; font-size: 14px; background: transparent;");

                slotLayout->addWidget(line1);
                slotLayout->addWidget(line2);
                
                QObject::connect(loadBtn, &QPushButton::clicked, [&, info]() {
                    if (engine.loadExistingPlayer(info.playerName)) {
                        refreshUI();
                        stackedWidget->setCurrentWidget(gamePage);
                        loadPanel->hide();
                    }
                });

                QPushButton *delBtn = new QPushButton("X", slotWidget);
                delBtn->setGeometry(560, 5, 30, 30);
                delBtn->setStyleSheet("QPushButton { background-color: #e74c3c; color: white; border-radius: 15px; font-weight: bold; } QPushButton:hover { background-color: #c0392b; }");
                
                QObject::connect(delBtn, &QPushButton::clicked, [&, info]() {
                    QMessageBox::StandardButton reply = QMessageBox::question(&window, "FS · 删除存档", "确定要删除这个存档吗？", QMessageBox::Yes | QMessageBox::No);
                    if (reply == QMessageBox::Yes) {
                        engine.deleteSaveData(info.playerName);
                        refreshLoadPanel(); 
                    }
                });

            } else {
                QLabel *emptyLabel = new QLabel("【空存档位】", slotWidget);
                emptyLabel->setGeometry(0, 0, 600, 80);
                emptyLabel->setAlignment(Qt::AlignCenter);
                emptyLabel->setStyleSheet("color: #7f8c8d; font-size: 16px;");
            }

            slotsLayout->addWidget(slotWidget, 0, Qt::AlignCenter);
        }
    };

    // ==========================================
    // 发送按钮及多线程逻辑 (原生安全版)
    // ==========================================
    // 同一时刻只允许一个 AI 请求在飞。
    // 旧代码在 5 秒超时后就把发送按钮重新点亮，可后台的 processInput 还在跑，
    // 玩家再点一次就会有**两个线程同时写同一个 GameEngine 和同一个 sqlite 连接**（D-6）。
    bool aiInFlight = false;

    QObject::connect(sendBtn, &QPushButton::clicked, [&]() {
        QString text = inputBox->text();
        if (text.isEmpty()) return;

        if (aiInFlight) {
            QMessageBox::information(&window, "FS · 提示",
                                     "上一条还在等木子因的回复，请稍等一下。");
            return;
        }

        if (security.checkSecurity(text.toStdString())) {
            QMessageBox::warning(&window, "FS · 警告", "检测到不文明用语！");
            engine.applyPenalty(-20);
            inputBox->clear();
            inputWidget->hide();
            refreshUI(); 
            return;
        }
        
        inputBox->clear();
        inputWidget->hide();
        
        dialogBox->setText("木子因正在思考中......");
        sendBtn->setDisabled(true);

        std::string nodeBeforeSend = engine.getCurrentNode();

        // ==================================================================
        // 异步 AI 请求（R-03 / D-4 修复）
        //
        // 【旧实现的致命缺陷】finished 回调里调了 watcher->deleteLater()，watcher
        // 在 AI 返回后约几百毫秒内就被析构；而下面那个 5 秒 QTimer::singleShot
        // **没有绑 context object，到点照样会响**。于是 5 秒时 watcher 已经是个
        // 非空但悬垂的指针，`if (watcher && ...)` 拦不住它，
        // `watcher->isFinished()` 就成了对已释放内存的解引用（heap-use-after-free）。
        // 实测：AI 比 5 秒快时 3/3 SIGSEGV；AI 比 5 秒慢时 3/3 不崩。
        //
        // 【修法】四条一起上，缺一条都会留缝：
        //   1) QPointer 持有 watcher —— 对象一被析构它立刻变 null，可判空；
        //      （裸指针做不到这件事，它永远"非空"）
        //   2) shared_ptr<bool> done 把"已经对玩家兑现过一次"从
        //      "对象还活着吗"里解耦出来，两条路径抢占，后到的直接退出；
        //   3) QTimer 交给 &window 托管 —— window 没了定时器自动停表，
        //      不会再打出无主的回调；
        //   4) 两条路径互相判空（isNull()），绝不碰可能已被析构的对象。
        // ==================================================================
        auto done = std::make_shared<bool>(false);

        QTimer* timeoutTimer = new QTimer(&window);
        timeoutTimer->setSingleShot(true);
        // 定时器同样要用 QPointer 持有：超时路径会把它 deleteLater()，
        // 而慢速 AI 的结果 8 秒后才回来，那时 finished 路径若再对同一个
        // 定时器调 stop()/deleteLater() 就是 UAF（我第一版修复就踩了这个坑，
        // 实测慢路径直接 SIGSEGV）。
        QPointer<QTimer> timerGuard(timeoutTimer);

        QFutureWatcher<std::string>* watcher = new QFutureWatcher<std::string>(&window);
        QPointer<QFutureWatcher<std::string>> watcherGuard(watcher);

        QObject::connect(watcher, &QFutureWatcher<std::string>::finished,
                         [&, watcherGuard, timerGuard, done, nodeBeforeSend]() {
            // AI 正常返回：无论如何都要把按钮还给玩家，这是唯一的复位点。
            *done = true;
            aiInFlight = false;

            if (!timerGuard.isNull()) {
                timerGuard->stop();
                timerGuard->deleteLater();
            }

            if (watcherGuard.isNull()) return;

            std::string aiText = engine.getLastAiReply();
            if (aiText.empty()) aiText = "（木子因似乎陷入了沉默）";

            dialogBox->setText(QString::fromStdString(aiText));
            sendBtn->setDisabled(false);

            std::string nodeAfterSend = engine.getCurrentNode();
            if (nodeBeforeSend != nodeAfterSend) {
                refreshUI();
            } else {
                inputWidget->show();
                inputBox->setFocus();
            }

            watcherGuard->deleteLater();
        });

        // 5 秒超时保护：定时器由 &window 托管，window 析构时自动停表。
        QObject::connect(timeoutTimer, &QTimer::timeout,
                         [&, timerGuard, done]() {
            if (*done) return;   // AI 已经兑现过了，这次只是空转
            *done = true;

            dialogBox->setText("（木子因似乎陷入了长久的沉默，没有回应。）");
            inputWidget->show();
            inputBox->setFocus();
            if (!timerGuard.isNull()) timerGuard->deleteLater();
            // ⚠️ 这里**故意不** disconnect / delete watcher：后台的 processInput
            //    可能还在跑，留着它，等结果回来时由 finished 路径统一收尾，
            //    同时 aiInFlight 保持为 true，避免同一时间跑两个 processInput。
        });

        aiInFlight = true;
        timeoutTimer->start(5000);

        // 启动后台线程执行 engine.processInput
        QFuture<std::string> future = QtConcurrent::run([&, text]() {
            return engine.processInput(text.toStdString());
        });
        watcher->setFuture(future);
    });

    // ==========================================
    // 页面与主菜单绑定
    // ==========================================
    stackedWidget->addWidget(mainMenuPage);
    stackedWidget->addWidget(gamePage);
    stackedWidget->setCurrentWidget(mainMenuPage);

    QObject::connect(startBtn, &QPushButton::clicked, [&]() {
        bool ok;
        QString name = QInputDialog::getText(&window, "新游戏", "请输入你的名字:", QLineEdit::Normal, "", &ok);
        if (!ok || name.isEmpty()) return;
        
        engine.createNewPlayer(name.toStdString());
        stackedWidget->setCurrentWidget(gamePage);
        refreshUI(); 
    });

    QObject::connect(openLoadPanelBtn, &QPushButton::clicked, [&]() {
        refreshLoadPanel(); 
        loadPanel->show();  
    });

    QObject::connect(closePanelBtn, &QPushButton::clicked, [&]() {
        loadPanel->hide(); 
    });

    QObject::connect(quitBtn, &QPushButton::clicked, [&]() {
        window.close();
    });

    window.show();
    return app.exec();
}