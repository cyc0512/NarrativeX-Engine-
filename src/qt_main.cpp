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
#include <functional>
#include <vector>
#include <string>
#include "GameEngine.h"
#include "SecurityFilter.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    // 初始化游戏引擎和安全过滤
    GameEngine engine("../data/story.json");
    SecurityFilter security;

    // 让玩家先输入名字
    bool ok;
    QString name = QInputDialog::getText(NULL, "游戏启动", "请输入你的名字:", QLineEdit::Normal, "", &ok);
    if (!ok || name.isEmpty()) return 0;
    engine.createNewPlayer(name.toStdString());

    // 创建主窗口
    QWidget window;
    window.setWindowTitle("高三恋爱文字冒险");
    window.resize(1000, 700); 
    window.setStyleSheet("background-color: #1a1a1a;");

    // 1. 背景图
    QLabel *bgLabel = new QLabel(&window);
    bgLabel->setGeometry(0, 0, 1000, 700);
    QImage bgImage("../assets/bg.jpg");
    if (!bgImage.isNull()) {
        QPixmap bgPixmap = QPixmap::fromImage(bgImage.scaled(1000, 700, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
        bgLabel->setPixmap(bgPixmap);
        bgLabel->setScaledContents(true);
    } else {
        bgLabel->setText("【图片未找到：请检查 assets/bg.jpg】");
        bgLabel->setStyleSheet("color: red; font-size: 24px;");
        bgLabel->setAlignment(Qt::AlignCenter);
    }

    // 2. 旁白区域（占据上方大部分空间，半透明，可滚动阅读）
    QTextEdit *narratorArea = new QTextEdit(&window);
    narratorArea->setGeometry(50, 30, 900, 300); 
    narratorArea->setStyleSheet("background-color: rgba(0, 0, 0, 120); color: #ecf0f1; font-family: 'Microsoft YaHei'; font-size: 16px; border-radius: 10px; padding: 15px; border: none;");
    narratorArea->setReadOnly(true);

    // 3. 对话框（底部居中）
    QLabel *dialogBox = new QLabel(&window);
    dialogBox->setGeometry(50, 450, 900, 150);
    dialogBox->setFont(QFont("Microsoft YaHei", 22));
    dialogBox->setStyleSheet("background-color: rgba(0, 0, 0, 200); color: white; border-radius: 15px; padding: 20px; border: 2px solid #34495e;");
    dialogBox->setWordWrap(true);

    // 4. 选项容器（平时隐藏，覆盖在对话上方）
    QWidget *optionsWidget = new QWidget(&window);
    optionsWidget->setGeometry(250, 250, 500, 180); // 居中显示
    optionsWidget->setStyleSheet("background: transparent;");
    QVBoxLayout *optionsLayout = new QVBoxLayout(optionsWidget);
    optionsLayout->setContentsMargins(0, 0, 0, 0);
    optionsWidget->hide(); // 默认隐藏

    // 5. 自由输入区（平时隐藏，底部）
    QWidget *inputWidget = new QWidget(&window);
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
    inputWidget->hide(); // 默认隐藏

    // ==========================================
    // 核心逻辑：刷新界面
    // ==========================================
    std::function<void()> refreshUI; 
    refreshUI = [&]() {
        // 1. 清理旧选项
        QLayoutItem *item;
        while ((item = optionsLayout->takeAt(0)) != nullptr) {
            if (item->widget()) item->widget()->deleteLater();
            delete item;
        }

        // 默认隐藏选项和输入框
        optionsWidget->hide();
        inputWidget->hide();

        if (engine.isGameOver()) {
            dialogBox->setText("【游戏结束】");
            return;
        }

        // 2. 获取当前节点数据
        std::vector<std::string> pages = engine.getCurrentNodePages();
        if (pages.empty()) return;

        // 3. 区分旁白和对话
        QString narratorText = QString::fromStdString(pages[0]);
        QString npcText = "";
        
        if (pages.size() >= 2 && narratorText.contains("旁白")) {
            // 如果是旁白，追加到旁白区
            narratorArea->append(narratorText);
            npcText = QString::fromStdString(pages[1]);
        } else {
            // 如果没有旁白，pages[0] 就是对话
            npcText = narratorText;
        }

        dialogBox->setText(npcText);

        // 4. 动态获取选项，如果有选项，才显示选项框
        std::vector<std::string> currentOpts = engine.getCurrentOptions();
        if (!currentOpts.empty()) {
            optionsWidget->show(); // 👈 显示选项框
            
            for (size_t i = 0; i < currentOpts.size(); ++i) {
                QString btnText = QString::fromStdString(currentOpts[i]);
                QPushButton *btn = new QPushButton(btnText, &window);
                
                // 高级感按钮样式
                btn->setStyleSheet(
                    "QPushButton { background-color: rgba(30, 30, 30, 200); color: #ecf0f1; padding: 12px 20px; border-radius: 10px; border: 1px solid #7f8c8d; text-align: left; font-family: 'Microsoft YaHei'; font-size: 16px; }"
                    "QPushButton:hover { background-color: rgba(52, 152, 219, 220); border: 1px solid #3498db; }"
                );
                optionsLayout->addWidget(btn);
                
                QObject::connect(btn, &QPushButton::clicked, [&, i, btnText]() {
                    // 如果点击了自由输入（假定第三个选项是自由输入）
                    if (btnText.contains("自由输入") || btnText.contains("3.")) {
                        inputWidget->show(); // 显示输入框
                        inputBox->setFocus();
                    } else {
                        // 普通选项，直接推进剧情
                        engine.processInput(std::to_string(i + 1));
                        refreshUI(); 
                    }
                });
            }
        }
    };

    // 发送自由输入的文本
    QObject::connect(sendBtn, &QPushButton::clicked, [&]() {
        QString text = inputBox->text();
        if (text.isEmpty()) return;
        
        if (security.checkSecurity(text.toStdString())) {
            QMessageBox::warning(&window, "警告", "检测到不文明用语，好感度扣除！");
            engine.applyPenalty(-20);
            inputBox->clear();
            inputWidget->hide();
            refreshUI(); 
            return;
        }

        // 把玩家说的话也当做旁白记录一下
        narratorArea->append("\033[34m> " + text + "\033[0m");
        
        engine.processInput(text.toStdString());
        inputBox->clear();
        inputWidget->hide(); // 发送完隐藏输入框
        refreshUI(); 
    });

    // 初始刷新界面
    refreshUI();
    window.show();
    
    return app.exec();
}