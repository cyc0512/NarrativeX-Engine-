#include <QApplication>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLayoutItem>
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
#include "EngineAdapter.h"
#include <QtConcurrent>
#include <QFutureWatcher>

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    EngineAdapter engine("../data/story_v2.json");

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
    dialogBox->setGeometry(50, 100, 900, 450);
    dialogBox->setFont(QFont("Microsoft YaHei", 18));
    dialogBox->setStyleSheet("background-color: rgba(0, 0, 0, 180); color: white; border-radius: 15px; padding: 30px; border: 2px solid #34495e;");
    dialogBox->setWordWrap(true);
    dialogBox->setAlignment(Qt::AlignTop | Qt::AlignLeft);

    QWidget *optionsContainer = new QWidget(gamePage);
    optionsContainer->setStyleSheet("background: transparent;");
    optionsContainer->setGeometry(50, 560, 900, 150);
    QVBoxLayout *optionsLayout = new QVBoxLayout(optionsContainer);
    optionsLayout->setContentsMargins(0, 0, 0, 0);
    optionsLayout->setSpacing(10);
    optionsContainer->hide();

    QWidget *inputWidget = new QWidget(gamePage);
    inputWidget->setGeometry(50, 630, 900, 50);
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

    stackedWidget->addWidget(mainMenuPage);
    stackedWidget->addWidget(gamePage);
    stackedWidget->setCurrentWidget(mainMenuPage);

    std::function<void()> showInput;
    std::function<void()> refreshUI;
    std::function<void()> refreshLoadPanel;
    int currentPageIndex = 0;
    std::vector<std::string> currentPagesCache;
    bool aiInFlight = false;

    showInput = [&]() {
        inputWidget->show();
        inputBox->setFocus();
        for (int j = 0; j < optionsLayout->count(); ++j) {
            QLayoutItem *it = optionsLayout->itemAt(j);
            if (it && it->widget()) it->widget()->hide();
        }
    };

    refreshUI = [&]() {
        QLayoutItem *item;
        while ((item = optionsLayout->takeAt(0)) != nullptr) {
            if (item->widget()) item->widget()->deleteLater();
            delete item;
        }
        optionsContainer->hide();
        inputWidget->hide();

        if (engine.isGameOver()) {
            dialogBox->setText("【游戏结束】");
            return;
        }

        std::vector<std::string> pages = engine.getCurrentNodePages();
        if (pages.empty()) return;

        if (currentPageIndex == 0 || pages[0] != currentPagesCache[0]) {
            currentPagesCache = pages;
            currentPageIndex = 0;
        }

        dialogBox->setText(QString::fromStdString(currentPagesCache[currentPageIndex]));

        if (currentPageIndex < currentPagesCache.size() - 1) {
            QPushButton *continueBtn = new QPushButton("点击继续...", gamePage);
            continueBtn->setGeometry(350, 580, 300, 50);
            continueBtn->setStyleSheet(
                "QPushButton { background-color: rgba(52, 152, 219, 220); color: white; border-radius: 10px; font-size: 18px; font-weight: bold; }"
                "QPushButton:hover { background-color: rgba(41, 128, 185, 255); }"
            );
            continueBtn->show();

            QObject::connect(continueBtn, &QPushButton::clicked, [&, continueBtn]() {
                continueBtn->deleteLater();
                currentPageIndex++;
                refreshUI();
            });
            return;
        }

        optionsContainer->show();
        optionsContainer->raise();

        std::vector<std::string> currentOpts = engine.getCurrentOptions();

        if (currentOpts.empty()) {
            QPushButton *continueBtn = new QPushButton("点击继续...", gamePage);
            continueBtn->setFixedSize(300, 50);
            continueBtn->setStyleSheet(
                "QPushButton { background-color: rgba(52, 152, 219, 220); color: white; border-radius: 10px; font-size: 16px; font-weight: bold; }"
                "QPushButton:hover { background-color: rgba(41, 128, 185, 255); }"
            );
            optionsLayout->addWidget(continueBtn, 0, Qt::AlignCenter);

            QObject::connect(continueBtn, &QPushButton::clicked, [&]() {
                engine.advance();
                currentPageIndex = 0;
                refreshUI();
            });
        } else {
            int count = currentOpts.size();
            int h = count * 55;
            optionsContainer->setGeometry(100, 550 - h + 50, 800, h);

            // 1. 固定选项（只做跳转，不判断"自由输入"）
            for (size_t i = 0; i < currentOpts.size(); ++i) {
                QString btnText = QString::fromStdString(currentOpts[i]);
                QPushButton *btn = new QPushButton(btnText, gamePage);
                btn->setFixedSize(800, 45);
                btn->setStyleSheet(
                    "QPushButton { background-color: rgba(30, 30, 30, 200); color: #ecf0f1; padding: 10px 20px; border-radius: 10px; border: 1px solid #7f8c8d; text-align: left; font-family: 'Microsoft YaHei'; font-size: 16px; }"
                    "QPushButton:hover { background-color: rgba(52, 152, 219, 220); border: 1px solid #3498db; }"
                );
                optionsLayout->addWidget(btn);

                QObject::connect(btn, &QPushButton::clicked, [&, i]() {
                    engine.processInput(std::to_string(i + 1));
                    currentPageIndex = 0;
                    refreshUI();
                });
            }

            // 2. 自由输入独立入口（由 NodeSpec.intent 判断，不猜文本）
            if (engine.isFreeInputAvailable()) {
                QPushButton *freeBtn = new QPushButton("【自由输入】对她说点什么...", gamePage);
                freeBtn->setFixedSize(800, 45);
                freeBtn->setStyleSheet(
                    "QPushButton { background-color: rgba(230, 126, 34, 200); color: white; padding: 10px 20px; border-radius: 10px; border: 1px solid #e67e22; text-align: left; font-family: 'Microsoft YaHei'; font-size: 16px; font-weight: bold; }"
                    "QPushButton:hover { background-color: rgba(211, 84, 0, 255); }"
                );
                optionsLayout->addWidget(freeBtn);
                QObject::connect(freeBtn, &QPushButton::clicked, [&]() {
                    showInput();
                });
            }
        }
    };

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

                QLabel *line1 = new QLabel(QString::fromStdString("玩家: " + info.playerName + "  |  节点: " + info.currentNode), loadBtn);
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

    QObject::connect(sendBtn, &QPushButton::clicked, [&]() {
        QString text = inputBox->text();
        if (text.isEmpty()) return;

        if (aiInFlight) {
            QMessageBox::information(&window, "FS · 提示",
                                     "上一条还在等木子因的回复，请稍等一下。");
            return;
        }

        inputBox->clear();
        inputWidget->hide();

        dialogBox->setText("木子因正在思考中......");
        sendBtn->setDisabled(true);

        auto done = std::make_shared<bool>(false);

        QTimer* timeoutTimer = new QTimer(&window);
        timeoutTimer->setSingleShot(true);
        QPointer<QTimer> timerGuard(timeoutTimer);

        QFutureWatcher<StepResult>* watcher = new QFutureWatcher<StepResult>(&window);
        QPointer<QFutureWatcher<StepResult>> watcherGuard(watcher);

               QObject::connect(watcher, &QFutureWatcher<StepResult>::finished,
                         [&, watcherGuard, timerGuard, done]() {
            *done = true;
            aiInFlight = false;

            if (!timerGuard.isNull()) {
                timerGuard->stop();
                timerGuard->deleteLater();
            }

            if (watcherGuard.isNull()) return;

            StepResult sr = watcherGuard->result();
            
            // 1. 只要拿到了 AI 的回复，不管跳没跳节点，先显示出来！
            if (!sr.reply.empty()) {
                dialogBox->setText(QString::fromStdString(sr.reply));
            } else {
                dialogBox->setText("（木子因似乎陷入了沉默）");
            }
            sendBtn->setDisabled(false);

            // 2. 如果节点发生了变化（合法跳转），刷新整个 UI
            if (sr.moved) {
                currentPageIndex = 0;
                QTimer::singleShot(600, [&]() { refreshUI(); });
            } else {
                // 3. 如果节点没变（被拦截或原地闲聊），重新显示输入框和选项
                inputWidget->show();
                inputBox->setFocus();
                
                // 恢复选项按钮显示
                for (int j = 0; j < optionsLayout->count(); ++j) {
                    QLayoutItem *it = optionsLayout->itemAt(j);
                    if (it && it->widget()) it->widget()->show();
                }
            }

            watcherGuard->deleteLater();
        });

        QObject::connect(timeoutTimer, &QTimer::timeout,
                         [&, timerGuard, done]() {
            if (*done) return;
            *done = true;

            dialogBox->setText("（木子因似乎陷入了长久的沉默，没有回应。）");
            inputWidget->show();
            inputBox->setFocus();
            if (!timerGuard.isNull()) timerGuard->deleteLater();
        });

        aiInFlight = true;
        timeoutTimer->start(5000);

        QFuture<StepResult> future = QtConcurrent::run([&, text]() {
            return engine.step(text.toStdString());
        });
        watcher->setFuture(future);
    });

    QObject::connect(inputBox, &QLineEdit::returnPressed, sendBtn, &QPushButton::click);

    QObject::connect(startBtn, &QPushButton::clicked, [&]() {
        bool ok;
        QString name = QInputDialog::getText(&window, "新游戏", "请输入你的名字:", QLineEdit::Normal, "", &ok);
        if (!ok || name.isEmpty()) return;

        engine.createNewPlayer(name.toStdString());
        currentPageIndex = 0;
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