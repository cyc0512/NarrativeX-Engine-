#include <QApplication>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLayoutItem>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QMessageBox>
#include <QTime>
#include <QGridLayout>
#include <QScrollArea>
#include <QEvent>
#include <QPixmap>
#include <QImage>
#include <QPainter>
#include <QPaintEvent>
#include <QColor>
#include <QStackedWidget>
#include <QMenu>
#include <QAction>
#include <QPoint>
#include <QPointer>
#include <QTimer>
#include <QFile>
#include <QTextStream>
#include <QIODevice>
#include <QDebug>
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QEasingCurve>
#include <QSizePolicy>
#include <functional>
#include <memory>
#include <vector>
#include <string>
#include <algorithm>
#include "EngineAdapter.h"
#include <QtConcurrent>
#include <QFutureWatcher>

// 背景层：按当前 size() 以 KeepAspectRatioByExpanding 绘制 ../assets/bg_<key>.jpg，
// 缺图时回退到 ../assets/bg.jpg；其上叠深色半透明遮罩保证文字可读；图片缺失时画纯色底。
class BackgroundWidget : public QWidget {
public:
    explicit BackgroundWidget(QWidget *parent = nullptr) : QWidget(parent) {
        loadScene("default");
    }
    // 切换场景背景；key 不变则跳过重载，避免每次刷新闪烁
    void setScene(const QString& key) {
        if (key == sceneKey_) return;
        loadScene(key);
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.fillRect(rect(), QColor(16, 18, 26));
        if (!bg_.isNull()) {
            QPixmap pm = QPixmap::fromImage(bg_).scaled(
                size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
            p.drawPixmap((width() - pm.width()) / 2,
                         (height() - pm.height()) / 2, pm);
        }
        // 主菜单白天景用较浅遮罩保持明亮；夜晚景与全部游戏场景维持原有深遮罩，
        // 保证游戏内视觉零变化。
        p.fillRect(rect(), sceneKey_ == QStringLiteral("menu_day")
                              ? QColor(8, 10, 18, 60)
                              : QColor(8, 10, 18, 150));
    }
private:
    QImage bg_;
    QString sceneKey_;
    void loadScene(const QString& key) {
        sceneKey_ = key;
        QString path = "../assets/bg_" + key + ".jpg";
        if (!QFile::exists(path)) path = "../assets/bg.jpg";
        bg_.load(path);
        update();
    }
};

// 立绘层：木子因 五态立绘（透明 PNG），叠在背景之上、对话框之下。
// 仅当当前节点说话人为“木子因”时显示；表情由情绪标签或特定结局节点决定。
// 锚定在右下角，按窗口高度等比缩放；父控件（gamePage）缩放时跟随。
class CharacterLayer : public QWidget {
public:
    explicit CharacterLayer(QWidget *parent = nullptr, QWidget *anchorBelow = nullptr)
        : QWidget(parent), anchorBelow_(anchorBelow) {
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setVisible(false);
        effect_ = new QGraphicsOpacityEffect(this);
        effect_->setOpacity(1.0);
        setGraphicsEffect(effect_);
        fade_ = new QPropertyAnimation(effect_, "opacity", this);
        fade_->setDuration(260);
        fade_->setStartValue(0.0);
        fade_->setEndValue(1.0);
        fade_->setEasingCurve(QEasingCurve::OutCubic);
        if (parent) {
            parent->installEventFilter(this);
            setGeometry(parent->rect());
        }
    }
    // key 为空 -> 隐藏；否则加载 ../assets/characters/char_muziyin_<key>.png
    void setExpression(const QString& key) {
        if (key == currentKey_) return;
        currentKey_ = key;
        if (key.isEmpty()) { pix_ = QPixmap(); setVisible(false); update(); return; }
        QString path = "../assets/characters/char_muziyin_" + key + ".png";
        QPixmap pm;
        if (QFile::exists(path)) pm.load(path);
        pix_ = pm;
        if (pix_.isNull()) { setVisible(false); update(); return; }
        setVisible(true);
        update();
        fade_->stop();
        effect_->setOpacity(0.0);
        fade_->start();
    }
protected:
    void paintEvent(QPaintEvent*) override {
        if (pix_.isNull()) return;
        QPainter p(this);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        const int marginR = static_cast<int>(width() * 0.02);
        const int topMargin = static_cast<int>(height() * 0.01);
        // 对话框已压扁下移：立绘占满对话框顶边以上的整个上半区（约界面 2/3），
        // 且底边精确贴齐对话框顶边，两者分界处不留缝。
        int availBottom = anchorBelow_ ? anchorBelow_->y() : height();
        int th = availBottom - topMargin;
        if (th < 1) th = 1;
        int tw = pix_.width() * th / pix_.height();
        const int maxW = static_cast<int>(width() * 0.62);
        if (tw > maxW) { tw = maxW; th = pix_.height() * tw / pix_.width(); }
        int x = width() - tw - marginR;
        int y = availBottom - th;   // 底边 == 对话框顶边
        p.drawPixmap(x, y, tw, th, pix_);
    }
    bool eventFilter(QObject* o, QEvent* e) override {
        if (o == parent() && e->type() == QEvent::Resize) {
            setGeometry(parentWidget()->rect());
            update();
        }
        return QWidget::eventFilter(o, e);
    }
private:
    QString currentKey_;
    QPixmap pix_;
    QWidget *anchorBelow_ = nullptr;
    QGraphicsOpacityEffect *effect_ = nullptr;
    QPropertyAnimation *fade_ = nullptr;
};

// 登录过场：QWidget 没有 clicked 信号，用单个事件过滤器捕获「点击任意处跳过」；
// 同时监听窗口 Resize 以持续按 KeepAspectRatioByExpanding 铺满过场图。
// 回调均由 introActive 门控，过场结束后空转，不干扰游戏内交互。
class IntroEventFilter : public QObject {
public:
    explicit IntroEventFilter(QObject *parent = nullptr) : QObject(parent) {}
    std::function<void()> onSkip;
    std::function<void()> onResize;
    QObject *resizeTarget = nullptr;
protected:
    bool eventFilter(QObject *obj, QEvent *e) override {
        if (e->type() == QEvent::MouseButtonRelease && onSkip) onSkip();
        else if (obj == resizeTarget && e->type() == QEvent::Resize && onResize) onResize();
        return QObject::eventFilter(obj, e);
    }
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    // 视觉增强：QSS 加载失败静默跳过，不作为运行前提。
    {
        QFile styleFile("../assets/style.qss");
        if (styleFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in(&styleFile);
            in.setCodec("UTF-8");
            app.setStyleSheet(in.readAll());
        } else {
            qWarning("FS: ../assets/style.qss not found, fallback theme in use.");
        }
    }

    EngineAdapter engine("../data/story_v2.json");

    if (!engine.ready()) {
        QMessageBox::critical(nullptr, "FS · 启动失败",
                              QString::fromStdString(engine.lastError()) +
                              "\n\n请把工作目录切到 build/ 或项目根目录后重试。");
        return 1;
    }

    BackgroundWidget window;
    window.setWindowTitle("FS · FluidStory");
    window.setObjectName("MainWindow");
    window.resize(1000, 700);
    window.setMinimumSize(720, 520);

    QVBoxLayout *rootLayout = new QVBoxLayout(&window);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    QStackedWidget *stackedWidget = new QStackedWidget(&window);
    stackedWidget->setObjectName("PageStack");
    rootLayout->addWidget(stackedWidget);

    // ==========================================
    // 页面 1：主菜单（居中 #MenuPanel 卡片容器）
    // ==========================================
    QWidget *mainMenuPage = new QWidget();
    mainMenuPage->setObjectName("MenuPage");

    // 昼夜判定：读取当前时间自动切换，不再提供手动切换入口。
    bool menuIsNight = false;
    auto menuSceneKey = [&]() -> QString {
        return menuIsNight ? QStringLiteral("menu_night") : QStringLiteral("menu_day");
    };
    auto switchToMenu = [&]() {
        // 每次返回主菜单都按当前系统时间重新判定昼夜，实现“自动切换”
        const int hour = QTime::currentTime().hour();
        menuIsNight = !(hour >= 6 && hour < 18);
        window.setScene(menuSceneKey());
        stackedWidget->setCurrentWidget(mainMenuPage);
    };

    QVBoxLayout *mainMenuLayout = new QVBoxLayout(mainMenuPage);
    mainMenuLayout->setAlignment(Qt::AlignCenter);
    mainMenuLayout->setContentsMargins(24, 20, 24, 20);
    mainMenuLayout->setSpacing(0);
    mainMenuLayout->addStretch();

    QWidget *menuPanel = new QWidget(mainMenuPage);
    menuPanel->setObjectName("MenuPanel");
    menuPanel->setMaximumWidth(760);
    QVBoxLayout *menuPanelLayout = new QVBoxLayout(menuPanel);
    menuPanelLayout->setContentsMargins(28, 16, 28, 16);
    menuPanelLayout->setSpacing(8);

    QLabel *titleLabel = new QLabel("FS", menuPanel);
    titleLabel->setObjectName("TitleLabel");
    titleLabel->setAlignment(Qt::AlignCenter);
    menuPanelLayout->addWidget(titleLabel);

    QLabel *subTitleLabel = new QLabel("FluidStory Demo", menuPanel);
    subTitleLabel->setObjectName("SubtitleLabel");
    subTitleLabel->setAlignment(Qt::AlignCenter);
    menuPanelLayout->addWidget(subTitleLabel);

    QLabel *menuDivider = new QLabel("◆ ───────── ◆", menuPanel);
    menuDivider->setObjectName("MenuDivider");
    menuDivider->setAlignment(Qt::AlignCenter);
    menuPanelLayout->addWidget(menuDivider);

    // 玩家姓名输入已移至「开始新游戏」独立页 NewGamePage（见下方页面 1b），主菜单不再内嵌。

    auto makeMenuButton = [&](const QString &text, const char *name) -> QPushButton* {
        QPushButton *b = new QPushButton(text, menuPanel);
        b->setObjectName(name);
        return b;
    };

    QPushButton *startBtn = makeMenuButton("开始新游戏", "PrimaryButton");
    QPushButton *openLoadPanelBtn = makeMenuButton("读取存档", "PrimaryButton");
    QPushButton *openEndingBtn = makeMenuButton("已收录结局", "PrimaryButton");
    QPushButton *openCharactersBtn = makeMenuButton("相遇角色", "PrimaryButton");
    QPushButton *quitBtn = makeMenuButton("退出游戏", "DangerButton");
    menuPanelLayout->addWidget(startBtn, 0, Qt::AlignCenter);
    menuPanelLayout->addWidget(openLoadPanelBtn, 0, Qt::AlignCenter);
    menuPanelLayout->addWidget(openEndingBtn, 0, Qt::AlignCenter);
    menuPanelLayout->addWidget(openCharactersBtn, 0, Qt::AlignCenter);
    menuPanelLayout->addWidget(quitBtn, 0, Qt::AlignCenter);

    // 相遇人物栏已移至独立页 CharacterPage（交互样式同“已收录结局”）。
    // 昼夜切换已改为按当前系统时间自动判定，不再提供手动切换按钮。

    mainMenuLayout->addWidget(menuPanel, 0, Qt::AlignCenter);
    mainMenuLayout->addStretch();

    // ==========================================
    // 存档页：居中浮层（作为 QStackedWidget 一页，布局居中）
    // ==========================================
    QWidget *loadPage = new QWidget();
    loadPage->setObjectName("LoadPage");
    QVBoxLayout *loadOuter = new QVBoxLayout(loadPage);
    loadOuter->addStretch();
    QHBoxLayout *loadCenter = new QHBoxLayout();
    loadCenter->addStretch();

    QWidget *loadPanel = new QWidget(loadPage);
    loadPanel->setObjectName("LoadPanel");
    loadPanel->setMinimumWidth(560);
    QVBoxLayout *loadPanelLayout = new QVBoxLayout(loadPanel);
    loadPanelLayout->setContentsMargins(28, 24, 28, 24);
    loadPanelLayout->setSpacing(16);

    QLabel *loadTitle = new QLabel("选择要读取的存档", loadPanel);
    loadTitle->setObjectName("LoadTitle");
    loadTitle->setAlignment(Qt::AlignCenter);
    loadPanelLayout->addWidget(loadTitle);

    QVBoxLayout *slotsLayout = new QVBoxLayout();
    slotsLayout->setSpacing(12);
    loadPanelLayout->addLayout(slotsLayout);

    QPushButton *closePanelBtn = new QPushButton("返回主菜单", loadPanel);
    closePanelBtn->setObjectName("PrimaryButton");
    loadPanelLayout->addWidget(closePanelBtn, 0, Qt::AlignCenter);

    loadCenter->addWidget(loadPanel);
    loadCenter->addStretch();
    loadOuter->addLayout(loadCenter);
    loadOuter->addStretch();

    // ==========================================
    // 页面 1b：开始新游戏（姓名输入作为该流程的一步，不再内嵌主菜单）
    // ==========================================
    QWidget *newGamePage = new QWidget();
    newGamePage->setObjectName("NewGamePage");
    QVBoxLayout *ngOuter = new QVBoxLayout(newGamePage);
    ngOuter->addStretch();
    QHBoxLayout *ngCenter = new QHBoxLayout();
    ngCenter->addStretch();

    QWidget *ngPanel = new QWidget(newGamePage);
    ngPanel->setObjectName("LoadPanel");   // 复用读取存档页的居中浮层样式
    ngPanel->setMinimumWidth(560);
    QVBoxLayout *ngPanelLayout = new QVBoxLayout(ngPanel);
    ngPanelLayout->setContentsMargins(28, 24, 28, 24);
    ngPanelLayout->setSpacing(16);

    QLabel *ngTitle = new QLabel("开启新的故事", ngPanel);
    ngTitle->setObjectName("LoadTitle");
    ngTitle->setAlignment(Qt::AlignCenter);
    ngPanelLayout->addWidget(ngTitle);

    QWidget *namePanel = new QWidget(ngPanel);
    namePanel->setObjectName("NamePanel");
    QVBoxLayout *namePanelLayout = new QVBoxLayout(namePanel);
    namePanelLayout->setContentsMargins(18, 10, 18, 10);
    namePanelLayout->setSpacing(6);
    QLabel *nameHint = new QLabel("请输入你的名字", namePanel);
    nameHint->setObjectName("NameHint");
    nameHint->setAlignment(Qt::AlignCenter);
    namePanelLayout->addWidget(nameHint);
    QLineEdit *nameInput = new QLineEdit(namePanel);
    nameInput->setObjectName("NameInput");
    nameInput->setPlaceholderText("在此输入名字（最多 24 字）");
    nameInput->setMaxLength(24);
    nameInput->setAlignment(Qt::AlignCenter);
    namePanelLayout->addWidget(nameInput);
    ngPanelLayout->addWidget(namePanel);

    QPushButton *ngStartBtn = new QPushButton("开始", ngPanel);
    ngStartBtn->setObjectName("PrimaryButton");
    ngPanelLayout->addWidget(ngStartBtn, 0, Qt::AlignCenter);

    QPushButton *ngBackBtn = new QPushButton("返回主菜单", ngPanel);
    ngBackBtn->setObjectName("PrimaryButton");
    ngPanelLayout->addWidget(ngBackBtn, 0, Qt::AlignCenter);

    ngCenter->addWidget(ngPanel);
    ngCenter->addStretch();
    ngOuter->addLayout(ngCenter);
    ngOuter->addStretch();

    // ==========================================
    // 页面 4：已收录结局（独立页，居中浮层骨架）
    // ==========================================
    QWidget *endingPage = new QWidget();
    endingPage->setObjectName("EndingPage");
    QVBoxLayout *endingOuter = new QVBoxLayout(endingPage);
    endingOuter->addStretch();
    QHBoxLayout *endingCenter = new QHBoxLayout();
    endingCenter->addStretch();

    QWidget *endingPanel = new QWidget(endingPage);
    endingPanel->setObjectName("EndingPanel");
    endingPanel->setMinimumWidth(760);
    endingPanel->setMaximumWidth(860);
    QVBoxLayout *endingPanelLayout = new QVBoxLayout(endingPanel);
    endingPanelLayout->setContentsMargins(26, 22, 26, 22);
    endingPanelLayout->setSpacing(12);

    QLabel *endingTitle = new QLabel("已收录结局", endingPanel);
    endingTitle->setObjectName("GalleryTitle");
    endingTitle->setAlignment(Qt::AlignCenter);
    endingPanelLayout->addWidget(endingTitle);

    QLabel *endingStat = new QLabel("已收录 0 / 7", endingPanel);
    endingStat->setObjectName("GalleryStat");
    endingStat->setAlignment(Qt::AlignCenter);
    endingPanelLayout->addWidget(endingStat);

    // 卡片区放进 QScrollArea，保证 1000x700 窗口内可滚动而不溢出
    QScrollArea *endingScroll = new QScrollArea(endingPanel);
    endingScroll->setObjectName("EndingScroll");
    endingScroll->setWidgetResizable(true);
    // 不设最小高度时 QScrollArea 的 sizeHint 会把 viewport 压到几十像素，
    // 7 张卡片只剩缩略图顶边可见；440 保证约两行卡片完整显示且可滚动。
    endingScroll->setMinimumHeight(440);
    endingScroll->viewport()->setObjectName("EndingScrollViewport");
    endingScroll->viewport()->setAutoFillBackground(false);
    QWidget *endingGridHost = new QWidget();
    endingGridHost->setObjectName("EndingGridHost");
    QGridLayout *endingGrid = new QGridLayout(endingGridHost);
    endingGrid->setContentsMargins(4, 4, 4, 4);
    endingGrid->setSpacing(12);
    endingScroll->setWidget(endingGridHost);
    endingPanelLayout->addWidget(endingScroll, 1);

    QLabel *endingDetail = new QLabel("", endingPanel);
    endingDetail->setObjectName("EndingDetail");
    endingDetail->setWordWrap(true);
    endingDetail->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    endingPanelLayout->addWidget(endingDetail);

    QPushButton *endingBackBtn = new QPushButton("返回主菜单", endingPanel);
    endingBackBtn->setObjectName("PrimaryButton");
    endingPanelLayout->addWidget(endingBackBtn, 0, Qt::AlignCenter);

    endingCenter->addWidget(endingPanel);
    endingCenter->addStretch();
    endingOuter->addLayout(endingCenter);
    endingOuter->addStretch();

    // 每次进入该页都重新取 getEndingGallery() 并重建卡片，避免读到旧数据
    auto refreshEndingGallery = [&]() {
        QLayoutItem *it;
        while ((it = endingGrid->takeAt(0)) != nullptr) {
            if (it->widget()) it->widget()->deleteLater();
            delete it;
        }
        endingDetail->setText("");
        auto endings = engine.getEndingGallery();
        int collected = 0;
        for (const auto &e : endings) {
            if (e.firstSeq > 0) ++collected;
        }
        endingStat->setText(QStringLiteral("已收录 %1 / %2")
                            .arg(collected).arg(static_cast<int>(endings.size())));

        for (int i = 0; i < static_cast<int>(endings.size()); ++i) {
            const auto &e = endings[i];
            const bool got = e.firstSeq > 0;
            const bool bad = QString::fromStdString(e.id).contains(QStringLiteral("bad"));

            QPushButton *card = new QPushButton(endingGridHost);
            card->setObjectName("EndingCard");
            card->setFixedSize(330, 220);
            const QString border = got
                ? (bad ? QStringLiteral("#a8574f") : QStringLiteral("#c9a86a"))
                : QStringLiteral("rgba(201, 168, 106, 0.22)");
            card->setStyleSheet(QStringLiteral(
                "QPushButton#EndingCard {"
                " background-color: rgba(38, 40, 52, 0.72);"
                " border: 1px solid %1; border-radius: 10px; padding: 0px;"
                " }").arg(border));
            QVBoxLayout *cl = new QVBoxLayout(card);
            cl->setContentsMargins(10, 10, 10, 10);
            cl->setSpacing(4);

            QLabel *thumb = new QLabel(card);
            thumb->setObjectName("EndingThumb");
            thumb->setFixedSize(220, 124);
            thumb->setAlignment(Qt::AlignCenter);
            thumb->setAttribute(Qt::WA_TransparentForMouseEvents, true);
            QPixmap pm(QStringLiteral("../assets/bg_%1.jpg")
                       .arg(QString::fromStdString(e.background)));
            if (!pm.isNull()) {
                thumb->setPixmap(pm.scaled(220, 124, Qt::KeepAspectRatioByExpanding,
                                           Qt::SmoothTransformation));
            }
            if (!got) {
                QGraphicsOpacityEffect *dim = new QGraphicsOpacityEffect(thumb);
                dim->setOpacity(0.28);
                thumb->setGraphicsEffect(dim);
            }
            cl->addWidget(thumb, 0, Qt::AlignCenter);

            QLabel *idL = new QLabel(card);
            idL->setObjectName("EndingId");
            idL->setAlignment(Qt::AlignCenter);
            idL->setAttribute(Qt::WA_TransparentForMouseEvents, true);
            idL->setText(got ? QString::fromStdString(e.id) : QStringLiteral("？？？"));
            idL->setStyleSheet(QStringLiteral("QLabel#EndingId { color:%1; }")
                               .arg(got ? QStringLiteral("#c9a86a") : QStringLiteral("#8a8579")));
            cl->addWidget(idL);

            QLabel *hintL = new QLabel(card);
            hintL->setObjectName("EndingHint");
            hintL->setWordWrap(true);
            hintL->setFixedHeight(40);   // 12px 两行行高约 36px，留足避免末行被裁
            hintL->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
            hintL->setAttribute(Qt::WA_TransparentForMouseEvents, true);
            QString hintText = got ? QString::fromStdString(e.hint)
                                   : QStringLiteral("尚未抵达");
            if (hintText.size() > 40)   // 12px / 310px 内宽下两行可容纳的上限
                hintText = hintText.left(39) + QStringLiteral("…");
            hintL->setText(hintText);
            hintL->setStyleSheet(QStringLiteral("QLabel#EndingHint { color:%1; }")
                                 .arg(got ? QStringLiteral("#b9b2a6") : QStringLiteral("#8a8579")));
            cl->addWidget(hintL);
            cl->addStretch();   // 吸收卡片剩余高度，避免 hint 被推到卡片底边

            if (got) {
                const QString narrator = QString::fromStdString(e.narrator);
                QObject::connect(card, &QPushButton::clicked, [&, narrator]() {
                    endingDetail->setText(narrator);
                });
            }
            endingGrid->addWidget(card, i / 2, i % 2, Qt::AlignCenter);
        }
    };

    // ==========================================
    // 页面 4b：相遇角色（独立页，样式同“已收录结局”）
    // 点进按钮 -> 卡片网格 -> 点已相遇角色弹出人物介绍（CharacterEntry.note）
    // ==========================================
    QWidget *characterPage = new QWidget();
    characterPage->setObjectName("CharacterPage");
    QVBoxLayout *characterOuter = new QVBoxLayout(characterPage);
    characterOuter->addStretch();
    QHBoxLayout *characterCenter = new QHBoxLayout();
    characterCenter->addStretch();

    QWidget *characterPanel = new QWidget(characterPage);
    characterPanel->setObjectName("CharacterPanel");
    characterPanel->setMinimumWidth(760);
    characterPanel->setMaximumWidth(860);
    QVBoxLayout *characterPanelLayout = new QVBoxLayout(characterPanel);
    characterPanelLayout->setContentsMargins(26, 22, 26, 22);
    characterPanelLayout->setSpacing(12);

    QLabel *characterTitleL = new QLabel("相遇角色", characterPanel);
    characterTitleL->setObjectName("GalleryTitle");
    characterTitleL->setAlignment(Qt::AlignCenter);
    characterPanelLayout->addWidget(characterTitleL);

    QLabel *characterStat = new QLabel("已相遇 0 / 0", characterPanel);
    characterStat->setObjectName("GalleryStat");
    characterStat->setAlignment(Qt::AlignCenter);
    characterPanelLayout->addWidget(characterStat);

    QScrollArea *characterScroll = new QScrollArea(characterPanel);
    characterScroll->setObjectName("CharacterScroll");
    characterScroll->setWidgetResizable(true);
    // 与结局页一致：不设最小高度时 viewport 被压扁；440 保证约两行卡片完整显示且可滚动。
    characterScroll->setMinimumHeight(440);
    characterScroll->viewport()->setObjectName("CharacterScrollViewport");
    characterScroll->viewport()->setAutoFillBackground(false);
    QWidget *characterGridHost = new QWidget();
    characterGridHost->setObjectName("CharacterGridHost");
    QGridLayout *characterGrid = new QGridLayout(characterGridHost);
    characterGrid->setContentsMargins(4, 4, 4, 4);
    characterGrid->setSpacing(12);
    characterScroll->setWidget(characterGridHost);
    characterPanelLayout->addWidget(characterScroll, 1);

    QLabel *characterDetail = new QLabel("", characterPanel);
    characterDetail->setObjectName("CharacterDetail");
    characterDetail->setWordWrap(true);
    characterDetail->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    characterDetail->setStyleSheet(QStringLiteral("color:#f2ede3; background:transparent;"));
    characterPanelLayout->addWidget(characterDetail);

    QPushButton *characterBackBtn = new QPushButton("返回主菜单", characterPanel);
    characterBackBtn->setObjectName("PrimaryButton");
    characterPanelLayout->addWidget(characterBackBtn, 0, Qt::AlignCenter);

    characterCenter->addWidget(characterPanel);
    characterCenter->addStretch();
    characterOuter->addLayout(characterCenter);
    characterOuter->addStretch();

    // 每次进入该页都重新取 getCharacterGallery() 并重建卡片，避免读到旧数据
    auto refreshCharacterGallery = [&]() {
        QLayoutItem *it;
        while ((it = characterGrid->takeAt(0)) != nullptr) {
            if (it->widget()) it->widget()->deleteLater();
            delete it;
        }
        characterDetail->setText(QStringLiteral("点击上方角色，查看人物介绍"));

        auto chars = engine.getCharacterGallery();
        int metCount = 0;
        for (const auto &ce : chars) {
            if (ce.met) ++metCount;
        }
        characterStat->setText(QStringLiteral("已相遇 %1 / %2")
                              .arg(metCount).arg(static_cast<int>(chars.size())));

        for (int i = 0; i < static_cast<int>(chars.size()); ++i) {
            const auto &ce = chars[i];
            const bool known = ce.met;

            QPushButton *card = new QPushButton(characterGridHost);
            card->setObjectName("CharacterCard");
            card->setFixedSize(330, 220);
            const QString border = known
                ? QStringLiteral("#c9a86a")
                : QStringLiteral("rgba(201, 168, 106, 0.22)");
            card->setStyleSheet(QStringLiteral(
                "QPushButton#CharacterCard {"
                " background-color: rgba(38, 40, 52, 0.72);"
                " border: 1px solid %1; border-radius: 10px; padding: 0px;"
                " }").arg(border));
            QVBoxLayout *cl = new QVBoxLayout(card);
            cl->setContentsMargins(10, 10, 10, 10);
            cl->setSpacing(4);

            // 木子因有立绘缩略图；其余角色无资源则省略，仅显示名字与介绍
            if (known && ce.name == "木子因") {
                QLabel *thumb = new QLabel(card);
                thumb->setObjectName("CharacterThumb");
                thumb->setFixedSize(200, 104);
                thumb->setAlignment(Qt::AlignCenter);
                thumb->setAttribute(Qt::WA_TransparentForMouseEvents, true);
                QPixmap pm(QStringLiteral("../assets/characters/char_muziyin_calm.png"));
                if (!pm.isNull()) {
                    thumb->setPixmap(pm.scaled(200, 104, Qt::KeepAspectRatioByExpanding,
                                               Qt::SmoothTransformation));
                }
                cl->addWidget(thumb, 0, Qt::AlignCenter);
            }

            QLabel *nameL = new QLabel(card);
            nameL->setObjectName("CharacterName");
            nameL->setAlignment(Qt::AlignCenter);
            nameL->setAttribute(Qt::WA_TransparentForMouseEvents, true);
            nameL->setText(known ? QString::fromStdString(ce.name) : QStringLiteral("？？？"));
            nameL->setStyleSheet(QStringLiteral("QLabel#CharacterName { color:%1; }")
                                 .arg(known ? QStringLiteral("#f2ede3") : QStringLiteral("#8a8579")));
            cl->addWidget(nameL);

            QLabel *noteL = new QLabel(card);
            noteL->setObjectName("CharacterNote");
            noteL->setWordWrap(true);
            noteL->setFixedHeight(40);   // 12px 两行行高约 36px，留足避免末行被裁
            noteL->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
            noteL->setAttribute(Qt::WA_TransparentForMouseEvents, true);
            QString noteText = known ? QString::fromStdString(ce.note)
                                     : QStringLiteral("尚未相遇");
            if (noteText.size() > 40)   // 12px / 310px 内宽下两行可容纳的上限
                noteText = noteText.left(39) + QStringLiteral("…");
            noteL->setText(noteText);
            noteL->setStyleSheet(QStringLiteral("QLabel#CharacterNote { color:%1; }")
                                 .arg(known ? QStringLiteral("#b9b2a6") : QStringLiteral("#8a8579")));
            cl->addWidget(noteL);
            cl->addStretch();   // 吸收卡片剩余高度，避免 note 被推到卡片底边

            if (known) {
                const QString bioName = QString::fromStdString(ce.name);
                const QString bioNote = QString::fromStdString(ce.note);
                QObject::connect(card, &QPushButton::clicked, [&, bioName, bioNote]() {
                    // 点已相遇角色 -> 弹出该角色人物介绍（名称 + 一句话描述）
                    characterDetail->setText(
                        QStringLiteral("<b>%1</b><br><br>%2").arg(bioName).arg(bioNote));
                });
            }
            characterGrid->addWidget(card, i / 2, i % 2, Qt::AlignCenter);
        }
    };

    // ==========================================
    // 页面 5：登录过场 CG（轻量版，复用现有图；3 屏淡入淡出）
    // ==========================================
    QWidget *introPage = new QWidget();
    introPage->setObjectName("IntroPage");

    // 不透明黑底衬：让每屏从黑淡入，避免淡入时透出较亮的主菜单白天背景。
    QLabel *introBackdrop = new QLabel(introPage);
    introBackdrop->setStyleSheet("background-color:#080a12;");
    introBackdrop->lower();

    QLabel *introImage = new QLabel(introPage);
    introImage->setObjectName("IntroImage");
    introImage->setAlignment(Qt::AlignCenter);

    QLabel *introCaption = new QLabel("", introPage);
    introCaption->setObjectName("IntroCaption");
    introCaption->setWordWrap(true);
    introCaption->setAlignment(Qt::AlignCenter);

    QPushButton *introSkipBtn = new QPushButton("跳过 ▸", introPage);
    introSkipBtn->setObjectName("IntroSkip");

    // 演出层：沿袭对话淡入的工程约束 —— effect/animation 托管为 introImage 子对象、
    // 复用单一动画对象；推进用单发 QTimer + introStep 计数器实现取消语义。
    QGraphicsOpacityEffect *introEffect = new QGraphicsOpacityEffect(introImage);
    introImage->setGraphicsEffect(introEffect);
    introEffect->setOpacity(1.0);
    QPropertyAnimation *introAnim = new QPropertyAnimation(introEffect, "opacity", introImage);
    introAnim->setDuration(360);
    introAnim->setStartValue(0.0);
    introAnim->setEndValue(1.0);
    introAnim->setEasingCurve(QEasingCurve::OutCubic);

    const char *introImages[3] = {
        "../assets/bg_apartment_eve.jpg",
        "../assets/bg_street_cross.jpg",
        "../assets/bg_night_room.jpg"
    };
    const char *introCaptions[3] = {
        "傍晚的出租屋只亮着一盏灯。",
        "她把碗沿擦得干干净净，像要把这清苦的日子也一并擦亮。",
        "明天，是她的家长会。"
    };

    QPixmap introBase;
    auto applyIntroImage = [&]() {
        const int W = window.width();
        const int H = window.height();
        introImage->setGeometry(0, 0, W, H);
        introBackdrop->setGeometry(0, 0, W, H);
        const int cw = std::min(760, W - 80);
        introCaption->setGeometry((W - cw) / 2, H - 120, cw, 58);
        introSkipBtn->setGeometry(W - 150, H - 54, 126, 38);
        if (!introBase.isNull()) {
            introImage->setPixmap(introBase.scaled(
                introImage->size(), Qt::KeepAspectRatioByExpanding,
                Qt::SmoothTransformation));
        }
    };

    int introStep = -1;          // -1 未播放；0..2 当前屏；>=3 已结束
    bool introActive = false;    // 跳过门控 + pending singleShot 取消语义
    std::function<void()> finishIntro;
    std::function<void(int)> showIntroStep;
    std::function<void()> startIntro;

    finishIntro = [&]() {
        if (!introActive) return;
        introActive = false;
        ++introStep;              // 使所有 pending singleShot 回调失效
        introAnim->stop();
        introEffect->setOpacity(1.0);
        switchToMenu();
    };

    showIntroStep = [&](int step) {
        if (!introActive || step != introStep) return;
        introBase = QPixmap(QString::fromUtf8(introImages[step]));
        applyIntroImage();
        introCaption->setText(QString::fromUtf8(introCaptions[step]));
        introAnim->stop();
        introEffect->setOpacity(0.0);
        introAnim->start();
        QTimer::singleShot(1400, &app, [&, step]() {
            if (!introActive || step != introStep) return;
            if (step + 1 >= 3) { finishIntro(); return; }
            ++introStep;
            showIntroStep(introStep);
        });
    };

    startIntro = [&]() {
        introActive = true;
        introStep = 0;
        showIntroStep(0);
    };

    // 点击页面任意处跳过：QWidget 无 clicked 信号，用单个事件过滤器实现；
    // onResize/onSkip 均由 introActive 门控，过场结束后空转，不干扰游戏内交互。
    IntroEventFilter introFilter;
    introFilter.resizeTarget = &window;
    introFilter.onSkip = [&]() { if (introActive) finishIntro(); };
    introFilter.onResize = [&]() { if (introActive) applyIntroImage(); };
    window.installEventFilter(&introFilter);
    introPage->installEventFilter(&introFilter);
    introImage->installEventFilter(&introFilter);
    introCaption->installEventFilter(&introFilter);
    introSkipBtn->installEventFilter(&introFilter);

    // ==========================================
    // 页面 2：游戏界面
    // ==========================================
    QWidget *gamePage = new QWidget();
    gamePage->setObjectName("GamePage");
    QVBoxLayout *gameLayout = new QVBoxLayout(gamePage);
    gameLayout->setContentsMargins(28, 10, 28, 12);
    gameLayout->setSpacing(6);

    // 顶部条：状态指示（左） + 系统菜单按钮（右）
    QHBoxLayout *topBar = new QHBoxLayout();
    QLabel *statusLabel = new QLabel("", gamePage);
    statusLabel->setObjectName("StatusLabel");
    QPushButton *sysMenuBtn = new QPushButton("⚙ 菜单", gamePage);
    sysMenuBtn->setObjectName("SysMenuButton");
    topBar->addWidget(statusLabel, 1);
    topBar->addWidget(sysMenuBtn, 0, Qt::AlignRight);
    gameLayout->addLayout(topBar);

    // 上部演出留白区，把对话框推到下部
    gameLayout->addStretch(1);

    // 对话框卡片：压扁并贴底，上部空间整块让给立绘。
    // 名牌/提示收进顶行，正文压扁，选项区与自由输入区横排收进卡片底行 —— 与对话框
    // 区域合并（重叠），不再单独占一行垂直空间，从而把界面约 2/3 的高度留给角色立绘。
    QWidget *dialogCard = new QWidget(gamePage);
    dialogCard->setObjectName("DialogBox");
    dialogCard->setMinimumHeight(150);
    dialogCard->setMaximumHeight(196);
    QVBoxLayout *cardLayout = new QVBoxLayout(dialogCard);
    cardLayout->setContentsMargins(22, 10, 22, 10);
    cardLayout->setSpacing(6);

    QHBoxLayout *topRow = new QHBoxLayout();
    QLabel *speakerLabel = new QLabel("木子因", dialogCard);
    speakerLabel->setObjectName("SpeakerLabel");
    QLabel *hintLabel = new QLabel("", dialogCard);
    hintLabel->setObjectName("HintLabel");
    hintLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    topRow->addWidget(speakerLabel, 1);
    topRow->addWidget(hintLabel, 0);
    cardLayout->addLayout(topRow);

    QLabel *dialogBox = new QLabel(dialogCard);
    dialogBox->setObjectName("DialogText");
    dialogBox->setWordWrap(true);
    dialogBox->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    dialogBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    dialogBox->setMinimumHeight(58);
    dialogBox->setMaximumHeight(98);
    cardLayout->addWidget(dialogBox, 1);

    // 底行：选项按钮横排、自由输入横排，二者互斥且都叠在对话框卡片内部
    QHBoxLayout *bottomRow = new QHBoxLayout();
    QWidget *optionsContainer = new QWidget(dialogCard);
    optionsContainer->setObjectName("OptionsArea");
    QHBoxLayout *optionsLayout = new QHBoxLayout(optionsContainer);
    optionsLayout->setContentsMargins(0, 0, 0, 0);
    optionsLayout->setSpacing(8);
    bottomRow->addWidget(optionsContainer, 1);
    optionsContainer->hide();

    QWidget *inputWidget = new QWidget(dialogCard);
    inputWidget->setObjectName("InputArea");
    QHBoxLayout *inputLayout = new QHBoxLayout(inputWidget);
    inputLayout->setContentsMargins(0, 0, 0, 0);
    inputLayout->setSpacing(10);

    QLineEdit *inputBox = new QLineEdit(inputWidget);
    inputBox->setObjectName("InputBox");
    inputBox->setPlaceholderText("对她说点什么……");
    inputBox->setMinimumHeight(38);

    QPushButton *sendBtn = new QPushButton("发送", inputWidget);
    sendBtn->setObjectName("SendButton");
    sendBtn->setMinimumHeight(38);

    inputLayout->addWidget(inputBox, 1);
    inputLayout->addWidget(sendBtn, 0);
    bottomRow->addWidget(inputWidget, 1);
    inputWidget->hide();
    cardLayout->addLayout(bottomRow);

    gameLayout->addWidget(dialogCard, 0);

    // 立绘层：木子因五态立绘（透明 PNG），叠于背景之上、对话框之下；仅木子因场景显示
    CharacterLayer *charLayer = new CharacterLayer(gamePage, dialogCard);
    charLayer->lower();   // 置于对话框 / 选项等控件之下，避免遮挡交互
    charLayer->hide();

    // 系统菜单
    QMenu *systemMenu = new QMenu(&window);
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
        switchToMenu();
    });

    stackedWidget->addWidget(mainMenuPage);
    stackedWidget->addWidget(newGamePage);
    stackedWidget->addWidget(gamePage);
    stackedWidget->addWidget(loadPage);
    stackedWidget->addWidget(endingPage);
    stackedWidget->addWidget(characterPage);
    stackedWidget->addWidget(introPage);

    // headless 截图 / 显式跳过时直接进主菜单，完全不启动过场动画。
    // FS_SHOT_PAGE 仅用于 offscreen 回归截图（day|night|ending|intro）；不传时行为完全不变。
    const QString shotPageReq = QString::fromLocal8Bit(qgetenv("FS_SHOT_PAGE")).toLower();
    bool skipIntro = !qgetenv("FS_SKIP_INTRO").isEmpty() || !qgetenv("FS_SHOT_OUT").isEmpty();
    if (shotPageReq == QLatin1String("intro")) skipIntro = false;   // 过场截图需真正播放
    if (skipIntro) {
        switchToMenu();                 // 含昼夜背景自动判定 + 切主菜单
    } else {
        const int hour = QTime::currentTime().hour();
        menuIsNight = !(hour >= 6 && hour < 18);
        window.setScene(menuSceneKey());
        stackedWidget->setCurrentWidget(introPage);
        startIntro();                   // 正常启动才播放 3 屏过场
    }

    // ==========================================
    // 演出层：对话文本淡入
    // effect 与 animation 都托管为 dialogBox 的 QObject 子对象，
    // 复用同一动画对象，不堆积、不悬空，且不使用 QTimer，不与 AI 异步链抢定时器。
    // ==========================================
    QGraphicsOpacityEffect *dialogEffect = new QGraphicsOpacityEffect(dialogBox);
    dialogBox->setGraphicsEffect(dialogEffect);
    dialogEffect->setOpacity(1.0);
    QPropertyAnimation *fadeAnim = new QPropertyAnimation(dialogEffect, "opacity", dialogBox);
    fadeAnim->setDuration(220);
    fadeAnim->setStartValue(0.0);
    fadeAnim->setEndValue(1.0);
    fadeAnim->setEasingCurve(QEasingCurve::OutCubic);
    auto fadeIn = [&]() {
        fadeAnim->stop();
        dialogEffect->setOpacity(0.0);
        fadeAnim->start();
    };

    // 轻量状态指示（只读 EngineAdapter 既有接口，不改动引擎）
    auto updateStatus = [&]() {
        QString node = QString::fromStdString(engine.getCurrentNode());
        if (node.isEmpty()) node = QStringLiteral("序章");
        int tagCount = static_cast<int>(engine.getAllTags().size());
        statusLabel->setText(QStringLiteral("◇ 章节 %1      ·      印记 %2").arg(node).arg(tagCount));
    };

    std::function<void()> showInput;
    std::function<void()> refreshUI;
    std::function<void()> refreshLoadPanel;
    int currentPageIndex = 0;
    std::vector<StoryPage> currentPagesCache;
    bool aiInFlight = false;

    // R-04：showInput 是自由输入的唯一显示入口；自由输入是否可用只由
    // engine.isFreeInputAvailable() 决定，不引入任何按按钮文本判断的启发式。
    showInput = [&]() {
        inputWidget->show();
        inputBox->setFocus();
        for (int j = 0; j < optionsLayout->count(); ++j) {
            QLayoutItem *it = optionsLayout->itemAt(j);
            if (it && it->widget()) it->widget()->hide();
        }
    };

    // 木子因立绘表情决策：说话人须为“木子因”；再按情绪标签或坏结局节点选择五态之一
    auto pickExpression = [&](const std::string& speaker) -> QString {
        if (speaker != "木子因") return QString();   // 老板 / 客户 / 广播 等无立绘
        std::string node = engine.getCurrentNode();
        if (node == "end_bad_smother" || node == "n_hidden_smother") return "badend";  // 被闷死坏结局（唯一动态分支）
        auto tags = engine.getAllTags();
        auto has = [&](const char* t){
            return std::find(tags.begin(), tags.end(), std::string(t)) != tags.end();
        };
        if (has("shyness"))                                                          return "peek";      // 害羞心动 / 暗暗喜欢
        if (has("deeply_touched") || has("trust") || has("relieved"))                 return "affection"; // 温柔含情
        if (has("grievance") || has("loneliness") || has("guilt") || has("regret") ||
            has("heartbroken") || has("anxious") || has("fearful") ||
            has("numbness") || has("suspicious"))                                     return "sad";       // 委屈低落
        return "calm";                                                                // 内敛（默认）
    };

    refreshUI = [&]() {
        QLayoutItem *item;
        while ((item = optionsLayout->takeAt(0)) != nullptr) {
            if (item->widget()) item->widget()->deleteLater();
            delete item;
        }
        optionsContainer->hide();
        inputWidget->hide();

        updateStatus();
        hintLabel->setText("");

        // 立绘：按当前节点说话人 + 情绪标签切换木子因表情（坏结局节点用 badend）
        charLayer->setExpression(pickExpression(engine.getCurrentNodeSpeaker()));

        if (engine.isGameOver()) {
            speakerLabel->setText("旁白");
            speakerLabel->setStyleSheet("color:#8a93a6;");
            dialogBox->setText("【游戏结束】");
            hintLabel->setText("— 故事已抵达终点 —");
            fadeIn();
            return;
        }

        // 按当前节点切换背景（缺图自动回退到默认 bg.jpg）
        window.setScene(QString::fromStdString(engine.getCurrentBackgroundKey()));

        std::vector<StoryPage> pages = engine.getCurrentNodePages();
        if (pages.empty()) return;

        if (currentPageIndex == 0 || pages[0].text != currentPagesCache[0].text) {
            currentPagesCache = pages;
            currentPageIndex = 0;
        }

        const StoryPage& pg = currentPagesCache[currentPageIndex];
        speakerLabel->setText(QString::fromStdString(pg.speaker));
        if (pg.role == PageRole::Narrator)      speakerLabel->setStyleSheet("color:#8a93a6;");
        else if (pg.role == PageRole::Npc)      speakerLabel->setStyleSheet("color:#c9a86a;");
        else                                    speakerLabel->setStyleSheet("color:#7fb3d5;");
        dialogBox->setText(QString::fromStdString(pg.text));
        fadeIn();

        if (currentPageIndex < currentPagesCache.size() - 1) {
            hintLabel->setText("点击继续，展开下一段 ▸");
            QPushButton *continueBtn = new QPushButton("点击继续...", optionsContainer);
            continueBtn->setObjectName("ContinueButton");
            optionsLayout->addWidget(continueBtn, 0, Qt::AlignCenter);
            optionsContainer->show();
            optionsContainer->raise();

            QObject::connect(continueBtn, &QPushButton::clicked, [&, continueBtn]() {
                continueBtn->deleteLater();
                currentPageIndex++;
                refreshUI();
            });
            return;
        }

        hintLabel->setText("— 请做出你的选择 —");
        optionsContainer->show();
        optionsContainer->raise();

        std::vector<std::string> currentOpts = engine.getCurrentOptions();

        if (currentOpts.empty()) {
            QPushButton *continueBtn = new QPushButton("点击继续...", optionsContainer);
            continueBtn->setObjectName("ContinueButton");
            optionsLayout->addWidget(continueBtn, 0, Qt::AlignCenter);

            QObject::connect(continueBtn, &QPushButton::clicked, [&]() {
                engine.advance();
                currentPageIndex = 0;
                refreshUI();
            });
        } else {
            // 1. 固定选项（只做跳转，不判断"自由输入"）
            for (size_t i = 0; i < currentOpts.size(); ++i) {
                QString btnText = QString::fromStdString(currentOpts[i]);
                QPushButton *btn = new QPushButton(btnText, optionsContainer);
                btn->setObjectName("OptionButton");
                optionsLayout->addWidget(btn);

                QObject::connect(btn, &QPushButton::clicked, [&, i]() {
                    engine.processInput(std::to_string(i + 1));
                    currentPageIndex = 0;
                    refreshUI();
                });
            }

            // 2. 自由输入独立入口（由 NodeSpec.intent 判断，不猜文本）
            if (engine.isFreeInputAvailable()) {
                QPushButton *freeBtn = new QPushButton("【自由输入】对她说点什么...", optionsContainer);
                freeBtn->setObjectName("FreeInputButton");
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
            slotWidget->setObjectName("SaveSlot");
            slotWidget->setMinimumWidth(500);
            QHBoxLayout *slotRow = new QHBoxLayout(slotWidget);
            slotRow->setContentsMargins(16, 12, 16, 12);
            slotRow->setSpacing(12);

            if (i < saves.size()) {
                DatabaseManager::SaveInfo info = saves[i];

                QString timeStr = QString::fromStdString(info.lastLogin);
                if (timeStr.trimmed().isEmpty()) timeStr = "时间读取失败";

                // 直接用按钮自身文本承载存档信息（颜色由 QSS #SlotButton 控制），
                // 避免 QLabel 嵌套在 QPushButton 内被按钮自身绘制覆盖导致黑字看不清。
                QPushButton *loadBtn = new QPushButton(slotWidget);
                loadBtn->setObjectName("SlotButton");
                loadBtn->setMinimumHeight(64);
                loadBtn->setCursor(Qt::PointingHandCursor);
                // 自包含富文本：颜色内嵌，不依赖 QSS/按钮绘制优先级，确保白字可读
                loadBtn->setText(QStringLiteral(
                    "<font color='#f2ede3'>玩家: %1   |   节点: %2</font><br>"
                    "<font color='#b9b2a6'>最后存档时间: %3</font>")
                                .arg(QString::fromStdString(info.playerName))
                                .arg(QString::fromStdString(info.currentNode))
                                .arg(timeStr));

                slotRow->addWidget(loadBtn, 1);

                QObject::connect(loadBtn, &QPushButton::clicked, [&, info]() {
                    if (engine.loadExistingPlayer(info.playerName)) {
                        refreshUI();
                        stackedWidget->setCurrentWidget(gamePage);
                    }
                });

                QPushButton *delBtn = new QPushButton("✕", slotWidget);
                delBtn->setObjectName("DeleteButton");
                slotRow->addWidget(delBtn, 0, Qt::AlignVCenter);

                QObject::connect(delBtn, &QPushButton::clicked, [&, info]() {
                    QMessageBox::StandardButton reply = QMessageBox::question(&window, "FS · 删除存档", "确定要删除这个存档吗？", QMessageBox::Yes | QMessageBox::No);
                    if (reply == QMessageBox::Yes) {
                        engine.deleteSaveData(info.playerName);
                        refreshLoadPanel();
                    }
                });

            } else {
                QLabel *emptyLabel = new QLabel("【空存档位】", slotWidget);
                emptyLabel->setObjectName("SlotEmpty");
                emptyLabel->setAlignment(Qt::AlignCenter);
                slotRow->addWidget(emptyLabel, 1);
            }

            slotsLayout->addWidget(slotWidget, 0, Qt::AlignCenter);
        }
    };

    // ==========================================
    // D-4 闪退修复：完整保留，任何美化不得破坏此异步链
    // ==========================================
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

        // 先以“我”呈现玩家自己的这句输入，再以提示行进入木子因的思考态
        speakerLabel->setText("我");
        speakerLabel->setStyleSheet("color:#7fb3d5;");
        dialogBox->setText(text);
        hintLabel->setText("对方正在思考中......");
        fadeIn();
        sendBtn->setDisabled(true);

        auto done = std::make_shared<bool>(false);
        auto replySpeaker = std::make_shared<std::string>(engine.getCurrentNodeSpeaker());

        QTimer* timeoutTimer = new QTimer(&window);
        timeoutTimer->setSingleShot(true);
        QPointer<QTimer> timerGuard(timeoutTimer);

        QFutureWatcher<StepResult>* watcher = new QFutureWatcher<StepResult>(&window);
        QPointer<QFutureWatcher<StepResult>> watcherGuard(watcher);

               QObject::connect(watcher, &QFutureWatcher<StepResult>::finished,
                         [&, watcherGuard, timerGuard, done, replySpeaker]() {
            *done = true;
            aiInFlight = false;

            if (!timerGuard.isNull()) {
                timerGuard->stop();
                timerGuard->deleteLater();
            }

            if (watcherGuard.isNull()) return;

            StepResult sr = watcherGuard->result();
            *replySpeaker = sr.speaker;

            // 1. 只要拿到了 AI 的 回复，不管跳没跳节点，先显示出来！
            speakerLabel->setText(QString::fromStdString(*replySpeaker));
            speakerLabel->setStyleSheet("color:#c9a86a;");
            if (!sr.reply.empty()) {
                dialogBox->setText(QString::fromStdString(sr.reply));
            } else {
                dialogBox->setText(QString::fromStdString("（" + *replySpeaker + "似乎陷入了沉默）"));
            }
            charLayer->setExpression(pickExpression(*replySpeaker));
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
                         [&, timerGuard, done, replySpeaker]() {
            if (*done) return;
            *done = true;

            speakerLabel->setText(QString::fromStdString(*replySpeaker));
            speakerLabel->setStyleSheet("color:#c9a86a;");
            dialogBox->setText(QString::fromStdString("（" + *replySpeaker + "似乎陷入了长久的沉默，没有回应。）"));
            charLayer->setExpression(pickExpression(*replySpeaker));
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
        // 进入「开始新游戏」流程页，姓名输入放在该页内
        stackedWidget->setCurrentWidget(newGamePage);
        nameInput->setFocus();
    });

    QObject::connect(ngStartBtn, &QPushButton::clicked, [&]() {
        QString name = nameInput->text().trimmed();
        if (name.isEmpty()) name = QStringLiteral("玩家");   // 不再弹系统模态框
        engine.createNewPlayer(name.toStdString());
        currentPageIndex = 0;
        stackedWidget->setCurrentWidget(gamePage);
        refreshUI();
    });

    QObject::connect(ngBackBtn, &QPushButton::clicked, [&]() {
        switchToMenu();
    });

    QObject::connect(nameInput, &QLineEdit::returnPressed, ngStartBtn, &QPushButton::click);

    QObject::connect(openLoadPanelBtn, &QPushButton::clicked, [&]() {
        refreshLoadPanel();
        stackedWidget->setCurrentWidget(loadPage);
    });

    QObject::connect(closePanelBtn, &QPushButton::clicked, [&]() {
        switchToMenu();
    });

    QObject::connect(quitBtn, &QPushButton::clicked, [&]() {
        window.close();
    });

    QObject::connect(openEndingBtn, &QPushButton::clicked, [&]() {
        refreshEndingGallery();
        stackedWidget->setCurrentWidget(endingPage);
    });

    QObject::connect(openCharactersBtn, &QPushButton::clicked, [&]() {
        refreshCharacterGallery();
        stackedWidget->setCurrentWidget(characterPage);
    });

    QObject::connect(endingBackBtn, &QPushButton::clicked, [&]() {
        switchToMenu();
    });

    QObject::connect(characterBackBtn, &QPushButton::clicked, [&]() {
        switchToMenu();
    });

    QObject::connect(introSkipBtn, &QPushButton::clicked, [&]() {
        if (introActive) finishIntro();
    });

    window.show();

    // ==========================================
    // 截图预览：仅由环境变量触发，不传时行为完全不变
    // ==========================================
    const QString shotOut = QString::fromLocal8Bit(qgetenv("FS_SHOT_OUT"));
    const bool shotGame = !qgetenv("FS_SHOT_GAME").isEmpty();
    const bool shotExit = !qgetenv("FS_SHOT_EXIT").isEmpty();

    if (!shotOut.isEmpty()) {
        QTimer::singleShot(300, &app, [&, shotOut, shotGame, shotExit]() {
            fadeAnim->stop();
            dialogEffect->setOpacity(1.0);
            // 防御性：若过场仍在，停掉动画并让 pending singleShot 全部失效。
            introActive = false;
            ++introStep;
            introAnim->stop();
            introEffect->setOpacity(1.0);

            // FS_SHOT_PAGE: day|night|ending|intro，仅测试截图时切页，正常启动不受影响。
            if (shotPageReq == QLatin1String("day") ||
                shotPageReq == QLatin1String("night")) {
                // 截图时按请求强制昼夜；直接切页，不调用 switchToMenu（避免被真实时间覆盖）
                menuIsNight = (shotPageReq == QLatin1String("night"));
                window.setScene(menuSceneKey());
                stackedWidget->setCurrentWidget(mainMenuPage);
            } else if (shotPageReq == QLatin1String("characters")) {
                refreshCharacterGallery();
                stackedWidget->setCurrentWidget(characterPage);
            } else if (shotPageReq == QLatin1String("newgame")) {
                stackedWidget->setCurrentWidget(newGamePage);
            } else if (shotPageReq == QLatin1String("load")) {
                refreshLoadPanel();
                stackedWidget->setCurrentWidget(loadPage);
            } else if (shotPageReq == QLatin1String("ending")) {
                refreshEndingGallery();
                stackedWidget->setCurrentWidget(endingPage);
            } else if (shotPageReq == QLatin1String("intro")) {
                introStep = 3;               // 冻结，避免 1400ms 推进回调生效
                introBase = QPixmap(QString::fromUtf8(introImages[0]));
                applyIntroImage();
                introCaption->setText(QString::fromUtf8(introCaptions[0]));
                introAnim->stop();
                introEffect->setOpacity(1.0);
                stackedWidget->setCurrentWidget(introPage);
            }
            window.grab().save(shotOut);

            if (!shotGame) {
                if (shotExit) QTimer::singleShot(0, &app, &QCoreApplication::quit);
                return;
            }

            engine.createNewPlayer("preview");
            stackedWidget->setCurrentWidget(gamePage);
            refreshUI();
            QTimer::singleShot(150, &app, [&, shotOut, shotExit]() {
                fadeAnim->stop();
                dialogEffect->setOpacity(1.0);
                window.grab().save(shotOut + ".game.png");
                if (shotExit) QTimer::singleShot(0, &app, &QCoreApplication::quit);
            });
        });
    }

    return app.exec();
}
