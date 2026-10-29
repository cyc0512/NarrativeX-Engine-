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
#include <functional>
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

    QWidget window;
    window.setWindowTitle("FluidStory - Powered by NarrativeX");
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

    // 页面 1：主菜单
    QWidget *mainMenuPage = new QWidget();
    QVBoxLayout *mainMenuLayout = new QVBoxLayout(mainMenuPage);
    mainMenuLayout->setAlignment(Qt::AlignCenter);

    QLabel *titleLabel = new QLabel("FluidStory", mainMenuPage);
    titleLabel->setFont(QFont("Microsoft YaHei", 60, QFont::Bold));
    titleLabel->setStyleSheet("color: white; margin-bottom: 40px;");
    titleLabel->setAlignment(Qt::AlignCenter);
    mainMenuLayout->addWidget(titleLabel);

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

    // 存档面板
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

    // 页面 2：游戏界面
    QWidget *gamePage = new QWidget();
    
    // 必须先定义这些控件！
    QTextEdit *narratorArea = new QTextEdit(gamePage);
    narratorArea->setGeometry(50, 30, 900, 300); 
    narratorArea->setStyleSheet("background-color: rgba(0, 0, 0, 120); color: #ecf0f1; font-family: 'Microsoft YaHei'; font-size: 16px; border-radius: 10px; padding: 15px; border: none;");
    narratorArea->setReadOnly(true);

    QLabel *dialogBox = new QLabel(gamePage);
    dialogBox->setGeometry(50, 450, 900, 150);
    dialogBox->setFont(QFont("Microsoft YaHei", 22));
    dialogBox->setStyleSheet("background-color: rgba(0, 0, 0, 200); color: white; border-radius: 15px; padding: 20px; border: 2px solid #34495e;");
    dialogBox->setWordWrap(true);

    QWidget *optionsWidget = new QWidget(gamePage);
    optionsWidget->setGeometry(250, 250, 500, 180);
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

    // 👇 现在 narratorArea 已经存在了，可以加系统菜单了
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
        QMessageBox::information(&window, "系统", "进度已保存！");
    });

    QObject::connect(backToMenuAction, &QAction::triggered, [&]() {
        engine.applyPenalty(0); // 退出前存一次档
        stackedWidget->setCurrentWidget(mainMenuPage);
        narratorArea->clear(); // 此时编译器认识 narratorArea 了，不会报错
    });

    // 刷新界面核心逻辑
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

        QString narratorText = QString::fromStdString(pages[0]);
        QString npcText = "";
        
        if (pages.size() >= 2 && narratorText.contains("旁白")) {
            narratorArea->append(narratorText);
            npcText = QString::fromStdString(pages[1]);
        } else {
            npcText = narratorText;
        }

        dialogBox->setText(npcText);

        std::vector<std::string> currentOpts = engine.getCurrentOptions();
        if (!currentOpts.empty()) {
            optionsWidget->show();
            
            for (size_t i = 0; i < currentOpts.size(); ++i) {
                QString btnText = QString::fromStdString(currentOpts[i]);
                QPushButton *btn = new QPushButton(btnText, gamePage);
                
                btn->setStyleSheet(
                    "QPushButton { background-color: rgba(30, 30, 30, 200); color: #ecf0f1; padding: 12px 20px; border-radius: 10px; border: 1px solid #7f8c8d; text-align: left; font-family: 'Microsoft YaHei'; font-size: 16px; }"
                    "QPushButton:hover { background-color: rgba(52, 152, 219, 220); border: 1px solid #3498db; }"
                );
                optionsLayout->addWidget(btn);
                
                QObject::connect(btn, &QPushButton::clicked, [&, i, btnText]() {
                    if (btnText.contains("自由输入") || btnText.contains("3.")) {
                        inputWidget->show();
                        inputBox->setFocus();
                        for (int j = 0; j < optionsLayout->count(); ++j) {
                            if (optionsLayout->itemAt(j)->widget()) {
                                optionsLayout->itemAt(j)->widget()->hide();
                            }
                        }
                    } else {
                        engine.processInput(std::to_string(i + 1));
                        refreshUI(); 
                    }
                });
            }
        }
    };

    // 刷新存档面板
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
                
                               // 强行把时间读出来，并在终端打印看看
                std::cout << "【UI DEBUG】准备渲染的时间是: [" << info.lastLogin << "]" << std::endl;
                
                QString timeStr = QString::fromStdString(info.lastLogin);
                if (timeStr.trimmed().isEmpty()) {
                    timeStr = "时间读取失败"; // 如果C++里是空的，界面上强行显示这句，方便排查
                }

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
                    QMessageBox::StandardButton reply = QMessageBox::question(&window, "删除存档", "确定要删除这个存档吗？", QMessageBox::Yes | QMessageBox::No);
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
        
        // 安检拦截
        if (security.checkSecurity(text.toStdString())) {
            QMessageBox::warning(&window, "警告", "检测到不文明用语，好感度扣除！");
            engine.applyPenalty(-20);
            inputBox->clear();
            inputWidget->hide();
            refreshUI(); 
            return;
        }

        // 记录玩家发言
        narratorArea->append("\033[34m> " + text + "\033[0m");
        
        inputBox->clear();
        inputWidget->hide();
        
        // 锁定界面，显示正在思考
        dialogBox->setText("木因子正在思考中......");
        sendBtn->setDisabled(true);

        // 启动后台线程发送网络请求
        QFuture<std::string> future = QtConcurrent::run([&, text]() {
            std::string reply = engine.processInput(text.toStdString());
            return reply;
        });

        // 监控任务完成
        QFutureWatcher<std::string> *watcher = new QFutureWatcher<std::string>(&window);
        
        QObject::connect(watcher, &QFutureWatcher<std::string>::finished, [&, watcher]() {
            std::string fullReply = watcher->result();
            std::string aiText = engine.getLastAiReply();
            
            if (aiText.empty()) {
                aiText = fullReply;
            }

            // 直接把 AI 的话显示在大黑框里！
            dialogBox->setText(QString::fromStdString(aiText));
            
            // 在旁白区域记录一下好感度变化
            narratorArea->append("【木因子】" + QString::fromStdString(aiText));
            narratorArea->append(QString::fromStdString(fullReply));

            sendBtn->setDisabled(false);
            watcher->deleteLater(); 
            
            // 注意：这里绝对不要调用 refreshUI()，否则 AI 的台词会被剧本原有台词覆盖！
        });
        
        watcher->setFuture(future);
    });
    // 页面与按钮绑定
    stackedWidget->addWidget(mainMenuPage);
    stackedWidget->addWidget(gamePage);
    stackedWidget->setCurrentWidget(mainMenuPage);

    QObject::connect(startBtn, &QPushButton::clicked, [&]() {
        bool ok;
        QString name = QInputDialog::getText(&window, "新游戏", "请输入你的名字:", QLineEdit::Normal, "", &ok);
        if (!ok || name.isEmpty()) return;
        
        engine.createNewPlayer(name.toStdString());
        refreshUI(); 
        stackedWidget->setCurrentWidget(gamePage); 
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