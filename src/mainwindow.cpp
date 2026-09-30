#include "mainwindow.h"

#include "appsettings.h"
#include "historypanel.h"
#include "ipaddressedit.h"
#include "lang.h"
#include "netutils.h"
#include "settingspanel.h"
#include "subnetresultpanel.h"
#include "uistyle.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMap>
#include <QMessageBox>
#include <QPalette>
#include <QProgressBar>
#include <QPushButton>
#include <QShowEvent>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTextStream>
#include <QVBoxLayout>
#include <QWinTaskbarButton>
#include <QWinTaskbarProgress>
#include <algorithm>

namespace
{
const char *kTooManySubnetsPrefix = "TOO_MANY_SUBNETS:";

/// C# SaveFileDialog 的 "显示名|通配符|显示名|通配符" 过滤器转换为 Qt 格式
QString toQtFilter(const QString &winFilter)
{
    const QStringList parts = winFilter.split(QLatin1Char('|'));
    QStringList display;
    for (int i = 0; i < parts.size(); i += 2)
        display.append(parts.at(i));
    return display.join(QStringLiteral(";;"));
}

/// CSV 字段转义（与 C# EscapeCsvField 一致）
QString escapeCsvField(const QString &field)
{
    if (field.contains(QLatin1Char(',')) || field.contains(QLatin1Char('"'))
        || field.contains(QLatin1Char('\n')))
    {
        QString escaped = field;
        escaped.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        return QLatin1Char('"') + escaped + QLatin1Char('"');
    }
    return field;
}

void applyWindowColor(QWidget *widget, const QColor &color)
{
    widget->setAutoFillBackground(true);
    QPalette pal = widget->palette();
    pal.setColor(QPalette::Window, color);
    widget->setPalette(pal);
}

/// 扫描完成汇总窗口：对勾图标 + 标题 + 分组统计卡片（替代纯文本堆叠的 QMessageBox）
void showScanSummaryDialog(QWidget *parent, int totalIps, int online, int noDevice,
                           int router, int camera)
{
    struct StatRow
    {
        const char *key;
        int value;
        const char *color;
    };
    const StatRow rows[] = {
        {"Online", online, "#2196f3"},    // 在线
        {"NoDevice", noDevice, "#9e9e9e"}, // 无设备
        {"ColDhcp", router, "#d32f2f"},    // DHCP服务器
        {"ColCamera", camera, "#7b1fa2"},  // 摄像头
    };

    QDialog dialog(parent);
    dialog.setWindowTitle(Lang::get(QStringLiteral("ScanCompletedStatus")));
    dialog.setWindowIcon(QIcon(QStringLiteral(":/app.ico")));
    dialog.setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    dialog.setFixedWidth(380);
    dialog.setFont(QFont(QStringLiteral("Microsoft YaHei"), 9));
    dialog.setStyleSheet(QStringLiteral("QDialog { background-color: #ffffff; }"));

    // ---- 顶部：绿色对勾 + 标题 + 扫描总数 ----
    QLabel *checkIcon = new QLabel(QStringLiteral("\u2713"), &dialog);
    checkIcon->setFixedSize(38, 38);
    checkIcon->setAlignment(Qt::AlignCenter);
    QFont checkFont(QStringLiteral("Segoe UI Symbol"), 16);
    checkFont.setBold(true);
    checkIcon->setFont(checkFont);
    checkIcon->setStyleSheet(
        QStringLiteral("background-color: #4caf50; color: #ffffff; border-radius: 19px;"));

    QLabel *title = new QLabel(Lang::get(QStringLiteral("ScanCompletedStatus")), &dialog);
    QFont titleFont(QStringLiteral("Microsoft YaHei"), 12);
    titleFont.setBold(true);
    title->setFont(titleFont);
    title->setStyleSheet(QStringLiteral("color: #1f2329;"));

    QLabel *subtitle =
        new QLabel(Lang::fmt(QStringLiteral("ScanSummaryTotalIps"), totalIps), &dialog);
    subtitle->setStyleSheet(QStringLiteral("color: #8a9099;"));

    QVBoxLayout *titleLayout = new QVBoxLayout;
    titleLayout->setContentsMargins(0, 0, 0, 0);
    titleLayout->setSpacing(2);
    titleLayout->addWidget(title);
    titleLayout->addWidget(subtitle);

    QHBoxLayout *headerLayout = new QHBoxLayout;
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(14);
    headerLayout->addWidget(checkIcon);
    headerLayout->addLayout(titleLayout, 1);

    // ---- 中部：统计卡片（左名称 / 右数值，数值按类别着色） ----
    QWidget *card = new QWidget(&dialog);
    card->setObjectName(QStringLiteral("summaryCard"));
    card->setStyleSheet(QStringLiteral(
        "QWidget#summaryCard { background-color: #f6f8fa; border-radius: 8px; }"));

    QGridLayout *grid = new QGridLayout(card);
    grid->setContentsMargins(18, 14, 18, 14);
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(9);

    for (int i = 0; i < 4; ++i)
    {
        QLabel *name = new QLabel(Lang::get(QLatin1String(rows[i].key)), card);
        name->setStyleSheet(QStringLiteral("color: #8a9099;"));

        QLabel *value = new QLabel(QString::number(rows[i].value), card);
        QFont valueFont(QStringLiteral("Microsoft YaHei"), 11);
        valueFont.setBold(true);
        value->setFont(valueFont);
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        value->setStyleSheet(QStringLiteral("color: %1;").arg(QLatin1String(rows[i].color)));

        grid->addWidget(name, i, 0);
        grid->addWidget(value, i, 1);
    }
    grid->setColumnStretch(1, 1);

    // ---- 底部：确定按钮 ----
    QPushButton *okButton = new QPushButton(Lang::get(QStringLiteral("Ok")), &dialog);
    okButton->setFixedSize(96, 30);
    okButton->setCursor(Qt::PointingHandCursor);
    okButton->setDefault(true);
    okButton->setStyleSheet(UiStyle::primaryButtonStyle());

    QHBoxLayout *buttonLayout = new QHBoxLayout;
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    buttonLayout->addStretch(1);
    buttonLayout->addWidget(okButton);

    QVBoxLayout *dialogLayout = new QVBoxLayout(&dialog);
    dialogLayout->setContentsMargins(24, 20, 24, 18);
    dialogLayout->setSpacing(16);
    dialogLayout->addLayout(headerLayout);
    dialogLayout->addWidget(card);
    dialogLayout->addLayout(buttonLayout);

    QObject::connect(okButton, &QPushButton::clicked, &dialog, &QDialog::accept);

    dialog.adjustSize();
    dialog.exec();
}
} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QWidget(parent)
{
    // 加载设置（语言、扫描线程数）
    const AppSettings settings = AppSettings::load();
    Lang::setCurrent(settings.language);
    m_defaultAdapter = settings.scanAdapter;

    buildUi();

    m_scanner = new Scanner(this);
    m_scanner->setMaxParallelism(settings.scanThreads);
    m_scanner->setIpv6Enabled(settings.ipv6Enabled);

    connect(m_scanner, &Scanner::scanProgress, this, &MainWindow::onScanProgress);
    connect(m_scanner, &Scanner::scanCompleted, this, &MainWindow::onScanCompleted);
    connect(m_scanner, &Scanner::scanError, this, &MainWindow::onScanError);
    connect(m_scanner, &Scanner::scanFinished, this, &MainWindow::onScanFinished);

    // 应用界面语言
    applyLanguage();

    // 启动时按保存配置清理过期历史记录
    ScanHistoryStore::prune();
}

MainWindow::~MainWindow() = default;

void MainWindow::buildUi()
{
    setWindowIcon(QIcon(QStringLiteral(":/app.ico")));
    resize(1200, 800);
    setMinimumSize(1040, 700);
    applyWindowColor(this, Qt::white);

    const QFont labelFont = UiStyle::labelFont();
    const QFont buttonFont = UiStyle::labelFont();

    // ---------------- 页面容器（主页 / 历史 / 设置） ----------------
    m_stack = new QStackedWidget(this);

    m_pageHome = buildHomePage();

    m_pageHistory = new HistoryPanel(m_stack);
    connect(m_pageHistory, &HistoryPanel::recordActivated, this,
            &MainWindow::onHistoryRecordActivated);

    m_pageSettings = new SettingsPanel(m_stack);
    connect(m_pageSettings, &SettingsPanel::settingsSaved, this, &MainWindow::onSettingsSaved);

    m_stack->addWidget(m_pageHome);
    m_stack->addWidget(m_pageHistory);
    m_stack->addWidget(m_pageSettings);

    // ---------------- 进度（显示在 Windows 任务栏图标上） ----------------
    m_taskbarButton = new QWinTaskbarButton(this);
    m_taskbarProgress = m_taskbarButton->progress();

    // ---------------- 扫描进度弹窗（弹出显示真实百分比） ----------------
    m_progressDialog = new QDialog(this);
    m_progressDialog->setWindowModality(Qt::ApplicationModal);
    m_progressDialog->setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    m_progressDialog->setFixedWidth(400);
    m_progressDialog->setStyleSheet(QStringLiteral("QDialog { background-color: #ffffff; }"));

    m_progressRangeLabel = new QLabel(m_progressDialog);
    m_progressRangeLabel->setFont(labelFont);
    m_progressRangeLabel->setStyleSheet(QStringLiteral("color: #1f2329;"));

    m_progressBar = new QProgressBar(m_progressDialog);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setTextVisible(true);
    m_progressBar->setAlignment(Qt::AlignCenter);
    m_progressBar->setFixedHeight(24);
    m_progressBar->setStyleSheet(UiStyle::progressBarStyle());

    m_progressHintLabel = new QLabel(m_progressDialog);
    m_progressHintLabel->setFont(labelFont);
    m_progressHintLabel->setWordWrap(true);
    m_progressHintLabel->setStyleSheet(QStringLiteral("color: #8a9099;"));

    m_buttonStopInDialog = new QPushButton(m_progressDialog);
    m_buttonStopInDialog->setFixedSize(100, 30);
    m_buttonStopInDialog->setFont(buttonFont);
    m_buttonStopInDialog->setCursor(Qt::PointingHandCursor);
    m_buttonStopInDialog->setStyleSheet(UiStyle::secondaryButtonStyle());

    QHBoxLayout *progressButtonLayout = new QHBoxLayout;
    progressButtonLayout->setContentsMargins(0, 0, 0, 0);
    progressButtonLayout->addStretch(1);
    progressButtonLayout->addWidget(m_buttonStopInDialog);
    progressButtonLayout->addStretch(1);

    QVBoxLayout *progressLayout = new QVBoxLayout(m_progressDialog);
    progressLayout->setContentsMargins(22, 18, 22, 16);
    progressLayout->setSpacing(12);
    progressLayout->addWidget(m_progressRangeLabel);
    progressLayout->addWidget(m_progressBar);
    progressLayout->addWidget(m_progressHintLabel);
    progressLayout->addLayout(progressButtonLayout);

    connect(m_buttonStopInDialog, &QPushButton::clicked, this, &MainWindow::onStopClicked);

    // ---------------- 底部状态栏 ----------------
    m_panelBottom = new QWidget(this);
    m_panelBottom->setObjectName(QStringLiteral("bottomBar"));
    m_panelBottom->setAttribute(Qt::WA_StyledBackground, true);
    m_panelBottom->setFixedHeight(30);
    m_panelBottom->setStyleSheet(QStringLiteral(
        "QWidget#bottomBar { background-color: #ffffff; border-top: 1px solid #e6e8ec; }"));

    QStatusBar *statusBar = new QStatusBar(m_panelBottom);
    statusBar->setSizeGripEnabled(false);
    statusBar->setStyleSheet(QStringLiteral(
        "QStatusBar { background: transparent; } QStatusBar::item { border: none; }"));

    m_statusLabel = new QLabel(statusBar);
    m_statusLabel->setFont(labelFont);
    m_statusCount = new QLabel(statusBar);
    m_statusCount->setFont(labelFont);
    statusBar->addWidget(m_statusLabel, 1);
    statusBar->addWidget(m_statusCount, 0);

    QHBoxLayout *bottomLayout = new QHBoxLayout(m_panelBottom);
    // 左右留白，避免状态栏文字顶到窗口边缘（视觉上被裁切）
    bottomLayout->setContentsMargins(8, 0, 14, 0);
    bottomLayout->addWidget(statusBar);

    // ---------------- 窗体布局 ----------------
    QVBoxLayout *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);
    rootLayout->addWidget(buildNavBar());
    rootLayout->addWidget(m_stack, 1);
    rootLayout->addWidget(m_panelBottom);

    switchToPage(PageHome);
}

QWidget *MainWindow::buildNavBar()
{
    m_navBar = new QWidget(this);
    m_navBar->setObjectName(QStringLiteral("navBar"));
    m_navBar->setAttribute(Qt::WA_StyledBackground, true);
    m_navBar->setFixedHeight(104);
    m_navBar->setStyleSheet(QStringLiteral(
        "QWidget#navBar { background-color: #ffffff; border-bottom: 1px solid #e6e8ec; }"));

    m_navTitle = new QLabel(Lang::get(QStringLiteral("FormTitle")), m_navBar);
    QFont titleFont = UiStyle::pageTitleFont();
    titleFont.setPointSize(14);
    m_navTitle->setFont(titleFont);
    m_navTitle->setStyleSheet(QStringLiteral("color: #1f2329;"));

    m_navHome = new QPushButton(m_navBar);
    m_navHistory = new QPushButton(m_navBar);
    m_navSettings = new QPushButton(m_navBar);
    QPushButton *navs[] = {m_navHome, m_navHistory, m_navSettings};
    for (QPushButton *button : navs)
    {
        QFont navFont = UiStyle::labelFont();
        navFont.setPointSize(11);
        button->setFont(navFont);
        button->setCheckable(true);
        button->setAutoDefault(false);
        button->setCursor(Qt::PointingHandCursor);
        button->setStyleSheet(UiStyle::navButtonStyle());
    }

    // 标题在上、导航按钮在下，两行靠左排列
    QVBoxLayout *layout = new QVBoxLayout(m_navBar);
    layout->setContentsMargins(20, 14, 20, 14);
    layout->setSpacing(8);

    QHBoxLayout *titleRow = new QHBoxLayout;
    titleRow->setContentsMargins(0, 0, 0, 0);
    titleRow->addWidget(m_navTitle);
    titleRow->addStretch(1);
    layout->addLayout(titleRow);

    QHBoxLayout *buttonRow = new QHBoxLayout;
    buttonRow->setContentsMargins(0, 0, 0, 0);
    buttonRow->setSpacing(14);
    buttonRow->addWidget(m_navHome);
    buttonRow->addWidget(m_navHistory);
    buttonRow->addWidget(m_navSettings);
    buttonRow->addStretch(1);
    layout->addLayout(buttonRow);

    connect(m_navHome, &QPushButton::clicked, this, [this] { switchToPage(PageHome); });
    connect(m_navHistory, &QPushButton::clicked, this, [this] { switchToPage(PageHistory); });
    connect(m_navSettings, &QPushButton::clicked, this, [this] { switchToPage(PageSettings); });

    return m_navBar;
}

QWidget *MainWindow::buildHomePage()
{
    const QFont labelFont = UiStyle::labelFont();

    QWidget *page = new QWidget(m_stack);
    UiStyle::applyPageBackground(page);

    QVBoxLayout *layout = new QVBoxLayout(page);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);

    // ---------------- 搜索卡片 ----------------
    QWidget *searchCard = UiStyle::makeCard(page);
    QVBoxLayout *cardLayout = new QVBoxLayout(searchCard);
    cardLayout->setContentsMargins(16, 14, 16, 14);
    cardLayout->setSpacing(10);

    m_labelTitle = new QLabel(searchCard);
    m_labelTitle->setFont(UiStyle::sectionTitleFont());
    m_labelTitle->setStyleSheet(QStringLiteral("color: #1f2329;"));
    cardLayout->addWidget(m_labelTitle);

    // 网卡选择：仅作用于本次扫描，不写回配置；默认值来自设置页的「默认扫描网卡」
    m_labelAdapter = new QLabel(searchCard);
    m_labelAdapter->setFont(labelFont);
    m_labelAdapter->setStyleSheet(QStringLiteral("color: #5b6470;"));

    m_comboAdapter = new QComboBox(searchCard);
    m_comboAdapter->setFont(labelFont);
    m_comboAdapter->setStyleSheet(UiStyle::comboBoxStyle());
    UiStyle::enableRoundedPopup(m_comboAdapter);
    m_comboAdapter->setMinimumWidth(260);
    refreshAdapterCombo(m_defaultAdapter);

    // 网卡列表是启动时枚举的快照，插拔网卡后需要手动刷新
    m_buttonRefreshAdapter = new QPushButton(searchCard);
    m_buttonRefreshAdapter->setFont(labelFont);
    m_buttonRefreshAdapter->setFixedSize(UiStyle::kButtonWidth, UiStyle::kButtonHeight);
    m_buttonRefreshAdapter->setCursor(Qt::PointingHandCursor);
    m_buttonRefreshAdapter->setStyleSheet(UiStyle::secondaryButtonStyle());

    QHBoxLayout *adapterLayout = new QHBoxLayout;
    adapterLayout->setContentsMargins(0, 0, 0, 0);
    adapterLayout->setSpacing(8);
    adapterLayout->addWidget(m_labelAdapter);
    adapterLayout->addWidget(m_comboAdapter);
    adapterLayout->addWidget(m_buttonRefreshAdapter);
    adapterLayout->addStretch(1);
    cardLayout->addLayout(adapterLayout);

    m_labelStartIp = new QLabel(searchCard);
    m_labelTo = new QLabel(searchCard);
    m_labelEndIp = new QLabel(searchCard);
    QLabel *labels[] = {m_labelStartIp, m_labelTo, m_labelEndIp};
    for (QLabel *label : labels)
    {
        label->setFont(labelFont);
        label->setStyleSheet(QStringLiteral("color: #5b6470;"));
    }

    m_ipStart = new IpAddressEdit(searchCard);
    m_ipEnd = new IpAddressEdit(searchCard);
    m_ipStart->setFont(labelFont);
    m_ipEnd->setFont(labelFont);

    // 按当前网卡预填扫描范围（该网卡的 /24 网段，192.168.1.1 ~ 192.168.1.255）
    applyAdapterToRange();

    QHBoxLayout *rangeLayout = new QHBoxLayout;
    rangeLayout->setContentsMargins(0, 0, 0, 0);
    rangeLayout->setSpacing(8);
    rangeLayout->addWidget(m_labelStartIp);
    rangeLayout->addWidget(m_ipStart);
    rangeLayout->addWidget(m_labelTo);
    rangeLayout->addWidget(m_labelEndIp);
    rangeLayout->addWidget(m_ipEnd);
    rangeLayout->addStretch(1);
    cardLayout->addLayout(rangeLayout);

    m_buttonScan = new QPushButton(searchCard);
    m_buttonStop = new QPushButton(searchCard);
    m_buttonClear = new QPushButton(searchCard);
    m_buttonExport = new QPushButton(searchCard);
    m_buttonScan->setStyleSheet(UiStyle::primaryButtonStyle());
    m_buttonStop->setStyleSheet(UiStyle::secondaryButtonStyle());
    m_buttonClear->setStyleSheet(UiStyle::secondaryButtonStyle());
    m_buttonExport->setStyleSheet(UiStyle::secondaryButtonStyle());

    QPushButton *buttons[] = {m_buttonScan, m_buttonStop, m_buttonClear, m_buttonExport};
    for (QPushButton *button : buttons)
    {
        button->setFont(labelFont);
        button->setFixedSize(UiStyle::kButtonWidth, UiStyle::kButtonHeight);
        button->setCursor(Qt::PointingHandCursor);
    }

    QHBoxLayout *buttonLayout = new QHBoxLayout;
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    buttonLayout->setSpacing(8);
    buttonLayout->addWidget(m_buttonScan);
    buttonLayout->addWidget(m_buttonStop);
    buttonLayout->addWidget(m_buttonClear);
    buttonLayout->addWidget(m_buttonExport);
    buttonLayout->addStretch(1);
    cardLayout->addLayout(buttonLayout);

    layout->addWidget(searchCard);

    // ---------------- 结果标签页 ----------------
    m_tabControlResults = new QTabWidget(page);
    m_tabControlResults->setFont(labelFont);
    m_tabControlResults->setDocumentMode(true);
    // 关掉 QTabBar 默认在标签条顶部绘制的那条深色底边线
    m_tabControlResults->tabBar()->setDrawBase(false);
    m_tabControlResults->setStyleSheet(QStringLiteral(
        "QTabWidget::pane { border: 1px solid #e6e8ec; border-radius: 8px;"
        " background-color: #ffffff; top: -1px; }"
        "QTabBar::tab { background-color: #eef1f5; color: #5b6470; border: 1px solid #e6e8ec;"
        " border-bottom: none; border-top-left-radius: 6px; border-top-right-radius: 6px;"
        " padding: 5px 14px; margin-right: 3px; }"
        "QTabBar::tab:selected { background-color: #ffffff; color: #1e88e5;"
        " font-weight: bold; }"
        "QTabBar::tab:hover:!selected { background-color: #e5eaf0; }"));
    layout->addWidget(m_tabControlResults, 1);

    connect(m_buttonScan, &QPushButton::clicked, this, &MainWindow::onScanClicked);
    connect(m_buttonStop, &QPushButton::clicked, this, &MainWindow::onStopClicked);
    connect(m_buttonClear, &QPushButton::clicked, this, &MainWindow::onClearClicked);
    connect(m_buttonExport, &QPushButton::clicked, this, &MainWindow::onExportClicked);
    // 换网卡后扫描范围跟着切到该网卡的网段，用户仍可手动改
    connect(m_comboAdapter, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { applyAdapterToRange(); });
    // 重新枚举网卡，保留当前选中（仅在又插了网卡、自动选择的出口变了时才需要）
    connect(m_buttonRefreshAdapter, &QPushButton::clicked, this, [this]() {
        refreshAdapterCombo(m_comboAdapter->currentData().toString());
    });

    return page;
}

void MainWindow::refreshAdapterCombo(const QString &selected)
{
    UiStyle::fillAdapterCombo(m_comboAdapter, selected);
}

void MainWindow::applyAdapterToRange()
{
    // 选中具体网卡时用该网卡的 IPv4；「自动选择」时用系统默认路由所在的 IPv4
    QString ipv4 = NetUtils::localIpv4Address();
    const QString adapter = m_comboAdapter->currentData().toString();
    if (!adapter.isEmpty())
    {
        const QVector<NetUtils::LocalInterface> interfaces = NetUtils::localInterfaces();
        for (const NetUtils::LocalInterface &iface : interfaces)
        {
            if (iface.adapterName == adapter)
            {
                ipv4 = iface.ipv4;
                break;
            }
        }
    }

    const QStringList parts = ipv4.split(QLatin1Char('.'));
    if (parts.size() != 4)
        return;

    const QString prefix = QStringLiteral("%1.%2.%3").arg(parts.at(0), parts.at(1), parts.at(2));
    m_ipStart->setAddress(prefix + QStringLiteral(".1"));
    m_ipEnd->setAddress(prefix + QStringLiteral(".255"));
}

void MainWindow::applyLanguage()
{
    setWindowTitle(Lang::get(QStringLiteral("FormTitle")));
    m_navTitle->setText(Lang::get(QStringLiteral("FormTitle")));
    m_navHome->setText(Lang::get(QStringLiteral("Home")));
    m_navHistory->setText(Lang::get(QStringLiteral("History")));
    m_navSettings->setText(Lang::get(QStringLiteral("Settings")));

    m_labelTitle->setText(Lang::get(QStringLiteral("ScanRangeTitle")));
    m_labelAdapter->setText(Lang::get(QStringLiteral("AdapterLabel")));
    m_buttonRefreshAdapter->setText(Lang::get(QStringLiteral("AdapterRefresh")));
    // 重建下拉项（「自动选择」文案随语言变化），保留当前选中
    refreshAdapterCombo(m_comboAdapter->currentData().toString());
    m_labelStartIp->setText(Lang::get(QStringLiteral("StartIp")));
    m_labelTo->setText(Lang::get(QStringLiteral("To")));
    m_labelEndIp->setText(Lang::get(QStringLiteral("EndIp")));
    m_buttonScan->setText(Lang::get(QStringLiteral("StartScan")));
    m_buttonStop->setText(Lang::get(QStringLiteral("StopScan")));
    m_buttonClear->setText(Lang::get(QStringLiteral("ClearResults")));
    m_buttonExport->setText(Lang::get(QStringLiteral("ExportResults")));
    m_statusLabel->setText(Lang::get(QStringLiteral("Ready")));
    m_statusCount->setText(Lang::get(QStringLiteral("StatusCountInit")));

    // 扫描进度弹窗
    m_progressDialog->setWindowTitle(Lang::get(QStringLiteral("ScanProgressTitle")));
    m_progressHintLabel->setText(Lang::get(QStringLiteral("ScanningInProgress")));
    m_buttonStopInDialog->setText(Lang::get(QStringLiteral("StopScan")));

    // 历史页与设置页各自刷新文案
    m_pageHistory->applyLanguage();
    m_pageSettings->applyLanguage();

    // 刷新所有已打开的结果面板（表格列头 + IP分布图标题/图例）
    for (int i = 0; i < m_tabControlResults->count(); ++i)
    {
        QWidget *page = m_tabControlResults->widget(i);
        QString subnet = page->property("subnet").toString();
        if (subnet.isEmpty())
            subnet = m_tabControlResults->tabText(i);
        m_tabControlResults->setTabText(i, Lang::fmt(QStringLiteral("SubnetTab"), subnet));
        if (SubnetResultPanel *panel = qobject_cast<SubnetResultPanel *>(page))
            panel->refreshLanguage();
    }
}

void MainWindow::switchToPage(int page)
{
    if (m_stack == nullptr)
        return;

    // 从设置页切走时，若有未保存的更改先弹窗询问；用户取消则留在设置页
    if (page != PageSettings && m_stack->currentIndex() == PageSettings
        && m_pageSettings != nullptr && !m_pageSettings->confirmLeave()) {
        // 导航按钮是 checkable 的，点击时已被勾上，取消切换要把勾选还原回当前页
        const int current = m_stack->currentIndex();
        m_navHome->setChecked(current == PageHome);
        m_navHistory->setChecked(current == PageHistory);
        m_navSettings->setChecked(current == PageSettings);
        return;
    }

    m_stack->setCurrentIndex(page);
    m_navHome->setChecked(page == PageHome);
    m_navHistory->setChecked(page == PageHistory);
    m_navSettings->setChecked(page == PageSettings);

    if (page == PageHome)
    {
        // 网卡可能在使用过程中插拔：切回首页时重新枚举一遍，保留当前选择
        refreshAdapterCombo(m_comboAdapter->currentData().toString());
    }
    else if (page == PageHistory)
    {
        m_pageHistory->reload();
    }
}

void MainWindow::onHistoryRecordActivated(const ScanHistoryRecord &record)
{
    if (record.filePath.isEmpty() && record.devices.isEmpty())
        return;

    displayHistoryRecord(record);
    switchToPage(PageHome);
}

void MainWindow::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // 任务栏进度需要窗口句柄，首次显示后绑定一次
    if (m_taskbarButton != nullptr && m_taskbarButton->window() != windowHandle())
        m_taskbarButton->setWindow(windowHandle());
}

void MainWindow::setScanningUiEnabled(bool scanning)
{
    m_buttonScan->setEnabled(!scanning);
    m_buttonStop->setEnabled(scanning);
    m_buttonClear->setEnabled(!scanning);
    m_buttonExport->setEnabled(!scanning);
    m_comboAdapter->setEnabled(!scanning);
    m_buttonRefreshAdapter->setEnabled(!scanning);
    m_navSettings->setEnabled(!scanning);
}

void MainWindow::onScanClicked()
{
    if (m_isScanning)
    {
        QMessageBox::information(this, Lang::get(QStringLiteral("Tip")),
                                 Lang::get(QStringLiteral("ScanningInProgress")));
        return;
    }

    const QString startIp = m_ipStart->address();
    const QString endIp = m_ipEnd->address();

    if (startIp.isEmpty() || endIp.isEmpty())
    {
        QMessageBox::critical(this, Lang::get(QStringLiteral("Error")),
                              Lang::get(QStringLiteral("InputIpRequired")));
        return;
    }

    // 限制只能扫描内网地址
    if (!NetUtils::isPrivateIp(startIp) || !NetUtils::isPrivateIp(endIp))
    {
        QMessageBox::critical(this, Lang::get(QStringLiteral("Error")),
                              Lang::get(QStringLiteral("PrivateIpOnly")));
        return;
    }

    // 清除之前的标签页
    clearResultTabs();

    // 记录本次扫描范围（用于自动保存历史）
    m_currentStartIp = startIp;
    m_currentEndIp = endIp;

    // 更新UI状态
    m_isScanning = true;
    setScanningUiEnabled(true);
    m_taskbarProgress->setRange(0, 100);
    m_taskbarProgress->setValue(0);
    m_taskbarProgress->setVisible(true);
    m_statusLabel->setText(Lang::fmt(QStringLiteral("ScanningRange"), startIp, endIp));
    m_statusCount->setText(Lang::get(QStringLiteral("StatusCountScanning")));

    // 弹出进度弹窗（百分比来自 Scanner 的真实进度信号）
    showProgressDialog();

    QString errorMessage;
    // 网卡只影响本次扫描：传空串表示交给 nmap 按系统路由自动选择
    const QString adapter = m_comboAdapter->currentData().toString();
    if (!m_scanner->startIpRangeScan(startIp, endIp, adapter, &errorMessage))
    {
        const QString prefix = QString::fromLatin1(kTooManySubnetsPrefix);
        if (errorMessage.startsWith(prefix))
        {
            showTooManySubnetsMessage(errorMessage.mid(prefix.size()).toInt());
        }
        else
        {
            QMessageBox::critical(this, Lang::get(QStringLiteral("Error")),
                                  Lang::fmt(QStringLiteral("ScanErrorStatus"), errorMessage));
        }

        // 与 C# 的 finally 一致：恢复界面状态
        m_isScanning = false;
        setScanningUiEnabled(false);
        m_taskbarProgress->setVisible(false);
        closeProgressDialog();
    }
}

void MainWindow::onStopClicked()
{
    if (m_isScanning)
    {
        m_scanner->stopScan();
        m_statusLabel->setText(Lang::get(QStringLiteral("ScanStopped")));
    }
}

void MainWindow::onClearClicked()
{
    clearResultTabs();
    m_taskbarProgress->setVisible(false);
    m_statusCount->setText(Lang::get(QStringLiteral("StatusCountInit")));
    m_statusLabel->setText(Lang::get(QStringLiteral("Ready")));
}

void MainWindow::onExportClicked()
{
    if (m_tabControlResults->count() == 0)
    {
        QMessageBox::information(this, Lang::get(QStringLiteral("Tip")),
                                 Lang::get(QStringLiteral("NoDataToExport")));
        return;
    }

    const QString filter = toQtFilter(Lang::get(QStringLiteral("ExportFilter")));
    const QString defaultName =
        QStringLiteral("%1_%2.csv")
            .arg(Lang::get(QStringLiteral("ExportFileName")),
                 QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));

    const QString filePath = QFileDialog::getSaveFileName(
        this, Lang::get(QStringLiteral("ExportResults")), defaultName, filter);
    if (filePath.isEmpty())
        return;

    QString errorMessage;
    if (exportToCsv(filePath, &errorMessage))
    {
        QMessageBox::information(this, Lang::get(QStringLiteral("Success")),
                                 Lang::fmt(QStringLiteral("ExportSuccess"), filePath));
    }
    else
    {
        QMessageBox::critical(this, Lang::get(QStringLiteral("Error")),
                              Lang::fmt(QStringLiteral("ExportFailed"), errorMessage));
    }
}

bool MainWindow::exportToCsv(const QString &filePath, QString *errorMessage)
{
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        if (errorMessage != nullptr)
            *errorMessage = file.errorString();
        return false;
    }

    QTextStream out(&file);
    out.setCodec("UTF-8");
    out.setGenerateByteOrderMark(true);
    out << Lang::get(QStringLiteral("CsvExportHeader")) << "\r\n";

    for (int i = 0; i < m_tabControlResults->count(); ++i)
    {
        QWidget *page = m_tabControlResults->widget(i);
        SubnetResultPanel *panel = qobject_cast<SubnetResultPanel *>(page);
        if (panel == nullptr)
            continue;

        QString subnet = page->property("subnet").toString();
        if (subnet.isEmpty())
            subnet = m_tabControlResults->tabText(i);

        const QList<QStringList> rows = panel->getRows();
        for (const QStringList &row : rows)
        {
            // 8 列：网段 + 表格前 5 列（含 IPv6，关闭时为横杠）+ 补回的 DHCP / 状态
            QStringList fields;
            fields.reserve(8);
            fields.append(escapeCsvField(subnet));
            for (int column = 0; column < 7; ++column)
                fields.append(escapeCsvField(column < row.size() ? row.at(column) : QString()));
            out << fields.join(QLatin1Char(',')) << "\r\n";
        }
    }

    out.flush();
    file.close();
    return true;
}

void MainWindow::showProgressDialog()
{
    m_progressRangeLabel->setText(
        Lang::fmt(QStringLiteral("ScanningRange"), m_currentStartIp, m_currentEndIp));
    m_progressBar->setValue(0);
    m_progressDialog->show();
}

void MainWindow::closeProgressDialog()
{
    if (m_progressDialog != nullptr && m_progressDialog->isVisible())
        m_progressDialog->hide();
}

void MainWindow::onScanProgress(int progress)
{
    const int value = qMin(progress, 100);
    m_taskbarProgress->setValue(value);
    m_progressBar->setValue(value);
    m_statusLabel->setText(Lang::fmt(QStringLiteral("ScanProgressPercent"), progress));
}

void MainWindow::onScanCompleted(const QVector<DhcpServerInfo> &results)
{
    m_taskbarProgress->setValue(100);
    closeProgressDialog();
    m_statusLabel->setText(Lang::get(QStringLiteral("OrganizingResults")));

    // 自动保存扫描历史
    saveScanHistory(results);

    populateResultTabs(results);

    int totalOnline = 0;
    int totalRouter = 0;
    int totalCamera = 0;
    for (const DhcpServerInfo &info : results)
    {
        if (info.isActive)
            ++totalOnline;
        if (info.isDhcpServer)
            ++totalRouter;
        if (info.isCamera)
            ++totalCamera;
    }
    const int totalNoDevice = results.size() - totalOnline;

    showScanSummaryDialog(this, results.size(), totalOnline, totalNoDevice, totalRouter,
                          totalCamera);
}

void MainWindow::saveScanHistory(const QVector<DhcpServerInfo> &results)
{
    ScanHistoryRecord record;
    record.scanTime = QDateTime::currentDateTime();
    record.startIp = m_currentStartIp;
    record.endIp = m_currentEndIp;
    record.devices.reserve(results.size());
    for (const DhcpServerInfo &info : results)
        record.devices.append(ScanHistoryDevice::from(info));
    ScanHistoryStore::save(&record);
}

void MainWindow::populateResultTabs(const QVector<DhcpServerInfo> &results)
{
    // 按网段分组（前三段），网段名按字符串升序
    QMap<QString, QVector<DhcpServerInfo>> groups;
    for (const DhcpServerInfo &info : results)
    {
        const QStringList parts = info.ipAddress.split(QLatin1Char('.'));
        const QString key = parts.size() >= 3 ? parts.mid(0, 3).join(QLatin1Char('.'))
                                              : info.ipAddress;
        groups[key].append(info);
    }

    clearResultTabs();

    for (auto it = groups.constBegin(); it != groups.constEnd(); ++it)
    {
        QVector<DhcpServerInfo> subnetResults = it.value();
        std::sort(subnetResults.begin(), subnetResults.end(),
                  [](const DhcpServerInfo &left, const DhcpServerInfo &right) {
                      return NetUtils::ipToLong(left.ipAddress)
                             < NetUtils::ipToLong(right.ipAddress);
                  });

        SubnetResultPanel *panel = new SubnetResultPanel(m_tabControlResults);
        panel->populateData(subnetResults);
        panel->setProperty("subnet", it.key());
        m_tabControlResults->addTab(panel, Lang::fmt(QStringLiteral("SubnetTab"), it.key()));
    }

    // 统计总数
    int totalOnline = 0;
    int totalRouter = 0;
    for (const DhcpServerInfo &info : results)
    {
        if (info.isActive)
            ++totalOnline;
        if (info.isDhcpServer)
            ++totalRouter;
    }
    const int totalNoDevice = results.size() - totalOnline;

    m_statusCount->setText(Lang::fmt(QStringLiteral("StatusCountDone"), totalOnline,
                                     totalNoDevice, totalRouter));
    m_statusLabel->setText(Lang::get(QStringLiteral("ScanCompletedStatus")));
}

void MainWindow::showDeviceDetailPreview()
{
    // --preview 预览用：新建一个结果页签（含示例设备）并直接弹出设备详情窗
    SubnetResultPanel *panel = new SubnetResultPanel(m_tabControlResults);
    m_tabControlResults->addTab(panel, QStringLiteral("Preview"));
    m_tabControlResults->setCurrentWidget(panel);
    panel->showDetailPreview();
}

void MainWindow::displayHistoryRecord(const ScanHistoryRecord &record)
{
    QVector<DhcpServerInfo> results;
    results.reserve(record.devices.size());
    for (const ScanHistoryDevice &device : record.devices)
        results.append(device.toServerInfo());

    populateResultTabs(results);
    m_statusLabel->setText(
        Lang::fmt(QStringLiteral("HistoryLoaded"), record.startIp, record.endIp));
}

void MainWindow::onScanError(const QString &message)
{
    closeProgressDialog();
    m_statusLabel->setText(Lang::fmt(QStringLiteral("ScanErrorStatus"), message));
    QMessageBox::critical(this, Lang::get(QStringLiteral("Error")),
                          Lang::fmt(QStringLiteral("ScanErrorDialog"), message));
}

void MainWindow::onScanFinished()
{
    m_isScanning = false;
    setScanningUiEnabled(false);
    // 扫描结束后隐藏任务栏进度与进度弹窗
    m_taskbarProgress->setVisible(false);
    closeProgressDialog();
}

void MainWindow::onSettingsSaved()
{
    // 重新加载已保存的设置并应用
    const AppSettings settings = AppSettings::load();
    Lang::setCurrent(settings.language);
    m_scanner->setMaxParallelism(settings.scanThreads);
    m_scanner->setIpv6Enabled(settings.ipv6Enabled);
    // 首页网卡下拉回到新保存的默认网卡，并把扫描范围切到该网段的 /24
    m_defaultAdapter = settings.scanAdapter;
    refreshAdapterCombo(m_defaultAdapter);
    applyAdapterToRange();
    // 已打开的结果页签同步 IPv6 列的显示与导出行为
    for (int i = 0; i < m_tabControlResults->count(); ++i)
    {
        if (SubnetResultPanel *panel = qobject_cast<SubnetResultPanel *>(
                m_tabControlResults->widget(i)))
            panel->setIpv6Visible(settings.ipv6Enabled);
    }
    applyLanguage();
}

void MainWindow::clearResultTabs()
{
    m_tabControlResults->clear();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_isScanning)
    {
        const QMessageBox::StandardButton result = QMessageBox::question(
            this, Lang::get(QStringLiteral("Confirm")),
            Lang::get(QStringLiteral("ExitWhileScanning")),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

        if (result != QMessageBox::Yes)
        {
            event->ignore();
            return;
        }

        m_scanner->stopScan();
    }

    event->accept();
}

void MainWindow::showTooManySubnetsMessage(int count)
{
    QDialog dialog(this);
    dialog.setWindowTitle(Lang::get(QStringLiteral("Tip")));
    dialog.setFixedSize(360, 180);
    dialog.setModal(true);
    applyWindowColor(&dialog, Qt::white);

    QLabel *labelMain = new QLabel(Lang::get(QStringLiteral("MaxSubnets")), &dialog);
    QFont mainFont(QStringLiteral("Microsoft YaHei"), 12);
    mainFont.setBold(true);
    labelMain->setFont(mainFont);
    labelMain->setStyleSheet(QStringLiteral("color: #1f2329;"));
    labelMain->setAlignment(Qt::AlignCenter);
    labelMain->setGeometry(20, 25, 310, 35);

    QLabel *labelSub = new QLabel(Lang::fmt(QStringLiteral("TooManySubnetsSub"), count), &dialog);
    labelSub->setFont(QFont(QStringLiteral("Microsoft YaHei"), 9));
    labelSub->setStyleSheet(QStringLiteral("color: #8a9099;"));
    labelSub->setAlignment(Qt::AlignCenter);
    labelSub->setWordWrap(true);
    labelSub->setGeometry(20, 60, 310, 45);

    QPushButton *buttonOk = new QPushButton(Lang::get(QStringLiteral("Ok")), &dialog);
    buttonOk->setGeometry(135, 105, 80, 30);
    buttonOk->setFont(QFont(QStringLiteral("Microsoft YaHei"), 9));
    buttonOk->setCursor(Qt::PointingHandCursor);
    buttonOk->setStyleSheet(UiStyle::primaryButtonStyle());
    connect(buttonOk, &QPushButton::clicked, &dialog, &QDialog::accept);

    dialog.adjustSize();
    dialog.setFixedSize(360, 180);
    const QPoint center = geometry().center();
    dialog.move(center.x() - dialog.width() / 2, center.y() - dialog.height() / 2);

    dialog.exec();
}
