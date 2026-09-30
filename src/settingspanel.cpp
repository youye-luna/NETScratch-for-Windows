#include "settingspanel.h"

#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QDate>
#include <QEvent>
#include <QFrame>
#include <QFont>
#include <QFontMetrics>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPalette>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QString>
#include <QTime>
#include <QVBoxLayout>
#include <QWidget>

#include "lang.h"
#include "scanhistory.h"
#include "toggleswitch.h"
#include "uistyle.h"

namespace {
// 设置内容列的最小宽度与固定高度（列本身无背景无边框，靠左排列）
// 宽度按「关于」区三列并排所需的最小尺寸取定，实际宽度随窗口拉伸。
const int kContentWidth = 710;
const int kContentHeight = 780;

// 设置页整体放大系数：字号与间距一起放大，保持版面比例不变
constexpr double kScale = 1.09;

/// 把按原始设计稿写下的坐标换算成放大后的实际坐标
constexpr int S(int value)
{
    return static_cast<int>(value * kScale + 0.5);
}

// 「关于」区并排三列：左列自上而下是 logo（应用名 + 副标题）、版本号、版权；
// 中列是功能列表；右列是开源组件说明。三列在内容宽度内等间距铺开，
// 具体横坐标在 layoutAbout() 里按实际可用宽度算。
const int kAboutLeft = S(20);      // 内容列左右边距
const int kAboutLogoWidth = 176;   // 左列宽度，按版权文案实测取定
const int kAboutMinGap = S(20);    // 三列之间的最小间距

// 「关于」区的垂直布局（设计稿坐标）
const int kAboutLineTopY = S(600);       // 上分隔线（仅保留这一条）
const int kAboutBlockTop = S(620);       // 中/右两列顶部
const int kAboutNameHeight = S(30);      // 应用名行高
const int kAboutVersionHeight = S(22);   // 版本号行高
const int kAboutCopyrightHeight = S(22); // 版权行高
const int kAboutFeaturesHeight = S(88);  // 功能列表块高度
const int kAboutLicenseHeight = S(20);   // 开源组件单行高度
const int kAboutLicenseLineGap = S(19);  // 开源组件行距

/// 全局正文字体按同一系数放大（仅作用于设置页，不影响其它页面）
QFont scaledLabelFont()
{
    QFont font = UiStyle::labelFont();
    font.setPointSizeF(font.pointSizeF() * kScale);
    return font;
}

/// 指定磅值的雅黑字体，磅值同样随 kScale 放大
QFont scaledYaHei(int pointSize, bool bold = false)
{
    QFont font(QStringLiteral("Microsoft YaHei"), qRound(pointSize * kScale));
    font.setBold(bold);
    return font;
}

// 关于区块配色：统一取全局视觉规范，避免出现多套灰阶
const QColor kAboutNameColor = UiStyle::textPrimaryColor();
const QColor kAboutTitleColor = UiStyle::textSecondaryColor();
const QColor kAboutVersionColor = UiStyle::textSecondaryColor();
const QColor kAboutFeatureColor = UiStyle::textSecondaryColor();
const QColor kAboutCopyrightColor = UiStyle::textSecondaryColor();

const QString kLangSimpleChinese = QStringLiteral("简体中文");
const QString kLangTraditionalTw = QStringLiteral("繁體中文（中国台湾）");
const QString kLangTraditionalHk = QStringLiteral("繁體中文（中国香港/澳门）");
const QString kLangEnglish = QStringLiteral("English");

/// 枚举 → 下拉显示文本
QString languageDisplay(AppLanguage language)
{
    switch (language) {
    case AppLanguage::English:
        return kLangEnglish;
    case AppLanguage::TraditionalChinese:
        return kLangTraditionalTw;
    case AppLanguage::TraditionalChineseHk:
        return kLangTraditionalHk;
    default:
        return kLangSimpleChinese;
    }
}

/// 下拉显示文本 → 枚举
AppLanguage languageParse(const QString &text)
{
    if (text == kLangEnglish)
        return AppLanguage::English;
    if (text == kLangTraditionalTw)
        return AppLanguage::TraditionalChinese;
    if (text == kLangTraditionalHk)
        return AppLanguage::TraditionalChineseHk;
    return AppLanguage::Chinese;
}

/// 按格式串渲染一个当前时间的样例（日期或时间）
QString renderFormatSample(const QString &format, bool isDate)
{
    return isDate ? QDate::currentDate().toString(format)
                  : QTime::currentTime().toString(format);
}

/// 填充格式下拉：首项为「跟随界面语言」（data 为空串），标签里直接展示当前语言内置格式
/// 的实际效果，便于用户判断默认长什么样；其余各项按当前日期/时间渲染样例，
/// data 中保存真正的格式串，写入配置的就是它。
void fillFormatCombo(QComboBox *combo, const QStringList &formats, const QString &selected,
                     bool isDate)
{
    combo->clear();
    const QString defaultFormat = Lang::get(isDate ? QStringLiteral("HistoryDateFormat")
                                                   : QStringLiteral("HistoryTimeFormat"));
    combo->addItem(Lang::fmt(QStringLiteral("FormatFollowLanguage"),
                             renderFormatSample(defaultFormat, isDate)),
                   QString());
    for (const QString &format : formats) {
        const QString sample = isDate ? QDate::currentDate().toString(format)
                                      : QTime::currentTime().toString(format);
        combo->addItem(sample, format);
    }
    const int index = combo->findData(selected);
    combo->setCurrentIndex(index < 0 ? 0 : index);
}

/// 分区标题（加粗、略大于正文），下辖若干设置项
QLabel *makeSectionTitle(QWidget *parent, const QString &text, int y)
{
    QLabel *label = new QLabel(text, parent);
    label->setGeometry(S(15), S(y), S(430), 24);
    label->setFont(scaledYaHei(11, true));
    label->setStyleSheet(QStringLiteral("color: #1f2329;"));
    return label;
}

QRadioButton *makeRadio(QWidget *parent, const QString &text, int x, int y)
{
    QRadioButton *radio = new QRadioButton(text, parent);
    radio->setFont(scaledYaHei(9));
    QPalette palette = radio->palette();
    palette.setColor(QPalette::WindowText, UiStyle::textPrimaryColor());
    radio->setPalette(palette);
    radio->move(x, y);
    radio->adjustSize();
    return radio;
}

/// 关于区块的说明文字（固定位置，颜色/字体由调用方指定）
QLabel *makeInfoLabel(QWidget *parent, const QFont &font, const QColor &color, int x, int y,
                      int width, int height, Qt::Alignment alignment = Qt::AlignLeft)
{
    QLabel *label = new QLabel(parent);
    label->setFont(font);
    label->setGeometry(x, y, width, height);
    label->setAlignment(alignment);
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, color);
    label->setPalette(palette);
    return label;
}

/// 关于区块的分隔线（1px 浅灰横线，取全局描边色）
QWidget *makeSeparator(QWidget *parent, int x, int y, int width)
{
    QFrame *line = new QFrame(parent);
    line->setGeometry(x, y, width, 1);
    line->setFrameShape(QFrame::NoFrame);
    line->setStyleSheet(QStringLiteral("background-color: #e6e8ec;"));
    return line;
}
} // namespace

SettingsPanel::SettingsPanel(QWidget *parent)
    : QWidget(parent)
{
    m_settings = AppSettings::load();

    UiStyle::applyPageBackground(this);
    setFont(scaledLabelFont());

    QVBoxLayout *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(16, 16, 16, 16);
    rootLayout->setSpacing(12);

    // 标题行：页面标题 + 右对齐的「保存设置」
    m_lblTitle = new QLabel(Lang::get(QStringLiteral("SettingsTitle")), this);
    QFont titleFont = UiStyle::pageTitleFont();
    titleFont.setPointSizeF(titleFont.pointSizeF() * kScale);
    m_lblTitle->setFont(titleFont);
    m_lblTitle->setStyleSheet(QStringLiteral("color: #1f2329;"));

    m_btnSaveSettings = new QPushButton(Lang::get(QStringLiteral("SaveSettings")), this);
    m_btnSaveSettings->setFont(scaledLabelFont());
    m_btnSaveSettings->setFixedSize(S(UiStyle::kButtonWidth), S(UiStyle::kButtonHeight));
    m_btnSaveSettings->setCursor(Qt::PointingHandCursor);
    m_btnSaveSettings->setStyleSheet(UiStyle::primaryButtonStyle());
    connect(m_btnSaveSettings, &QPushButton::clicked, this, [this]() {
        applySaveConfig();
        m_settings.save();
        ScanHistoryStore::prune();
        // 先同步语言等设置，再用新语言提示保存成功
        emit settingsSaved();
        QMessageBox::information(this, Lang::get(QStringLiteral("Success")),
                                 Lang::get(QStringLiteral("SaveConfigSuccess")));
    });

    QHBoxLayout *titleRowLayout = new QHBoxLayout;
    titleRowLayout->setContentsMargins(0, 0, 0, 0);
    titleRowLayout->setSpacing(10);
    titleRowLayout->addWidget(m_lblTitle);
    titleRowLayout->addStretch(1);
    titleRowLayout->addWidget(m_btnSaveSettings);
    rootLayout->addLayout(titleRowLayout);

    // 设置内容列：宽度随窗口拉伸（不低于最小宽度），高度固定；内容高于可视区时垂直滚动。
    // 内部沿用固定坐标布局，只有底部「关于」区的横坐标随宽度重算。
    // 内容不使用卡片背景与分组边框，直接落在页面底色上，保持单一色调。
    m_content = new QWidget;
    m_content->setMinimumWidth(kContentWidth);
    m_content->setFixedHeight(kContentHeight);
    m_content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_content->installEventFilter(this);
    m_content->setStyleSheet(
        QStringLiteral("QGroupBox { border: none; background: transparent;"
                       " margin-top: 12px; }"
                       "QGroupBox::title { subcontrol-origin: margin;"
                       " subcontrol-position: top left; left: 4px; padding: 0 2px;"
                       " color: #1f2329; }"));

    QWidget *scrollContent = new QWidget;
    QHBoxLayout *leftLayout = new QHBoxLayout(scrollContent);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(0);
    leftLayout->addWidget(m_content);

    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scrollArea->viewport()->setAutoFillBackground(false);
    m_scrollArea->setStyleSheet(QStringLiteral(
        "QScrollArea { background: transparent; border: none; }"
        "QScrollArea > QWidget > QWidget { background: transparent; }"));
    m_scrollArea->setWidget(scrollContent);
    rootLayout->addWidget(m_scrollArea, 1);

    // ------------------------------------------------------------ 语言和时间
    // 分区标题，下辖「界面语言」「日期格式」「时间格式」三项
    m_lblLangTimeSection =
        makeSectionTitle(m_content, Lang::get(QStringLiteral("LanguageTimeSection")), 4);

    m_lblLanguage = new QLabel(Lang::get(QStringLiteral("LanguageLabel")), m_content);
    m_lblLanguage->move(S(20), S(36));
    m_lblLanguage->adjustSize();
    m_lblLanguage->setStyleSheet(QStringLiteral("color: #1f2329;"));

    m_comboLanguage = new QComboBox(m_content);
    m_comboLanguage->setGeometry(S(150), S(32), S(200), S(25));
    m_comboLanguage->setFont(scaledLabelFont());
    m_comboLanguage->setStyleSheet(UiStyle::comboBoxStyle());
    UiStyle::enableRoundedPopup(m_comboLanguage);
    // 中文系列排在英文前面
    m_comboLanguage->addItem(kLangSimpleChinese);
    m_comboLanguage->addItem(kLangTraditionalTw);
    m_comboLanguage->addItem(kLangTraditionalHk);
    m_comboLanguage->addItem(kLangEnglish);
    m_comboLanguage->setCurrentIndex(
        m_comboLanguage->findText(languageDisplay(m_settings.language)));

    m_lblDateFormat = new QLabel(Lang::get(QStringLiteral("DateFormatLabel")), m_content);
    m_lblDateFormat->move(S(20), S(72));
    m_lblDateFormat->adjustSize();
    m_lblDateFormat->setStyleSheet(QStringLiteral("color: #1f2329;"));

    m_comboDateFormat = new QComboBox(m_content);
    m_comboDateFormat->setGeometry(S(150), S(68), S(220), S(25));
    m_comboDateFormat->setFont(scaledLabelFont());
    m_comboDateFormat->setStyleSheet(UiStyle::comboBoxStyle());
    UiStyle::enableRoundedPopup(m_comboDateFormat);
    fillFormatCombo(m_comboDateFormat, AppSettings::supportedDateFormats(), m_settings.dateFormat,
                    true);

    m_lblTimeFormat = new QLabel(Lang::get(QStringLiteral("TimeFormatLabel")), m_content);
    m_lblTimeFormat->move(S(20), S(108));
    m_lblTimeFormat->adjustSize();
    m_lblTimeFormat->setStyleSheet(QStringLiteral("color: #1f2329;"));

    m_comboTimeFormat = new QComboBox(m_content);
    m_comboTimeFormat->setGeometry(S(150), S(104), S(220), S(25));
    m_comboTimeFormat->setFont(scaledLabelFont());
    m_comboTimeFormat->setStyleSheet(UiStyle::comboBoxStyle());
    UiStyle::enableRoundedPopup(m_comboTimeFormat);
    fillFormatCombo(m_comboTimeFormat, AppSettings::supportedTimeFormats(), m_settings.timeFormat,
                    false);

    // ------------------------------------------------------------ 扫描设置
    // 分区标题，下辖「扫描线程数」及其说明
    m_lblScanSection = makeSectionTitle(m_content, Lang::get(QStringLiteral("ScanSettingsSection")),
                                        146);

    m_lblThreads = new QLabel(Lang::get(QStringLiteral("ThreadsLabel")), m_content);
    m_lblThreads->move(S(20), S(178));
    m_lblThreads->adjustSize();
    m_lblThreads->setStyleSheet(QStringLiteral("color: #1f2329;"));

    m_numThreads = new QSpinBox(m_content);
    m_numThreads->setGeometry(S(150), S(174), S(80), S(25));
    m_numThreads->setFont(scaledLabelFont());
    m_numThreads->setStyleSheet(UiStyle::spinBoxStyle());
    m_numThreads->setRange(1, 100);
    m_numThreads->setValue(qBound(1, m_settings.scanThreads, 100));
    m_numThreads->setAlignment(Qt::AlignCenter);

    m_lblHint = new QLabel(Lang::get(QStringLiteral("ThreadsHint")), m_content);
    m_lblHint->setGeometry(S(20), S(208), S(430), S(34));
    m_lblHint->setWordWrap(true);
    m_lblHint->setFont(scaledYaHei(8));
    m_lblHint->setStyleSheet(QStringLiteral("color: #8a9099;"));

    // IPv6 显示开关：iOS 样式滑动开关，位于线程数说明下方，
    // 标签居左与「扫描线程数」等行对齐，开关在控件列
    m_lblIpv6 = new QLabel(Lang::get(QStringLiteral("Ipv6Toggle")), m_content);
    m_lblIpv6->move(S(20), S(250));
    m_lblIpv6->adjustSize();
    m_lblIpv6->setFont(scaledLabelFont());
    m_lblIpv6->setStyleSheet(QStringLiteral("color: #1f2329;"));

    m_swIpv6 = new ToggleSwitch(m_content);
    m_swIpv6->setGeometry(S(150), S(246), S(46), S(26));
    m_swIpv6->setChecked(m_settings.ipv6Enabled);

    // 默认扫描网卡：第 0 项为「自动选择」，其后每张网卡一项（userData 存适配器 GUID）
    m_lblDefaultAdapter = new QLabel(Lang::get(QStringLiteral("DefaultAdapterLabel")), m_content);
    m_lblDefaultAdapter->move(S(20), S(284));
    m_lblDefaultAdapter->adjustSize();
    m_lblDefaultAdapter->setFont(scaledLabelFont());
    m_lblDefaultAdapter->setStyleSheet(QStringLiteral("color: #1f2329;"));

    m_comboDefaultAdapter = new QComboBox(m_content);
    m_comboDefaultAdapter->setGeometry(S(150), S(280), S(220), S(25));
    m_comboDefaultAdapter->setFont(scaledLabelFont());
    m_comboDefaultAdapter->setStyleSheet(UiStyle::comboBoxStyle());
    UiStyle::enableRoundedPopup(m_comboDefaultAdapter);

    // 网卡列表是启动时枚举的快照，插拔网卡（如插上网线、接上扩展坞）后需要手动刷新
    m_btnRefreshAdapter = new QPushButton(Lang::get(QStringLiteral("AdapterRefresh")), m_content);
    m_btnRefreshAdapter->setGeometry(S(378), S(280), S(60), S(25));
    m_btnRefreshAdapter->setFont(scaledLabelFont());
    m_btnRefreshAdapter->setCursor(Qt::PointingHandCursor);
    m_btnRefreshAdapter->setStyleSheet(UiStyle::secondaryButtonStyle());
    connect(m_btnRefreshAdapter, &QPushButton::clicked, this, [this]() {
        // 重新枚举网卡，尽量保留用户当前的选择（按适配器 GUID 恢复）
        UiStyle::fillAdapterCombo(m_comboDefaultAdapter,
                                  m_comboDefaultAdapter->currentData().toString());
    });

    UiStyle::fillAdapterCombo(m_comboDefaultAdapter, m_settings.scanAdapter);

    // ------------------------------------------------------------ 历史记录设置
    // 分区标题，下辖「清理方式」与「保留范围」两个子分组
    m_lblHistorySection =
        makeSectionTitle(m_content, Lang::get(QStringLiteral("HistorySettings")), 322);

    // ------------------------------------------------------------ 清理方式
    // 四个互斥选项，两行两列摆放（英文文案较长，两列可避免单行溢出）：
    // 按时间保存 / 按数量保存 在上，不清除 / 不保存 在下
    m_groupMethod = new QGroupBox(Lang::get(QStringLiteral("SaveMethodGroup")), m_content);
    m_groupMethod->setGeometry(S(15), S(346), S(430), S(94));
    m_groupMethod->setFont(scaledYaHei(9, true));

    m_radioByTime = makeRadio(m_groupMethod, Lang::get(QStringLiteral("SaveByTime")), S(5), S(32));
    m_radioByCount = makeRadio(m_groupMethod, Lang::get(QStringLiteral("SaveByCount")), S(245), S(32));
    m_radioNever = makeRadio(m_groupMethod, Lang::get(QStringLiteral("RangeNever")), S(5), S(58));
    m_radioSaveNever = makeRadio(m_groupMethod, Lang::get(QStringLiteral("SaveNever")), S(245), S(58));

    // ------------------------------------------------------------ 保留范围
    m_groupRange = new QGroupBox(Lang::get(QStringLiteral("SaveRangeGroup")), m_content);
    m_groupRange->setGeometry(S(15), S(446), S(430), S(124));
    m_groupRange->setFont(scaledYaHei(9, true));

    // 时间范围选项（独立容器，避免与数量选项互相排斥）
    m_panelTimeRange = new QWidget(m_groupRange);
    m_panelTimeRange->setGeometry(0, S(22), S(424), S(96));

    m_radioDays14 = makeRadio(m_panelTimeRange, Lang::get(QStringLiteral("Range14Days")), S(5), S(10));
    m_radioDaysHalf =
        makeRadio(m_panelTimeRange, Lang::get(QStringLiteral("RangeHalfMonth")), S(230), S(10));
    m_radioDaysMonth =
        makeRadio(m_panelTimeRange, Lang::get(QStringLiteral("RangeOneMonth")), S(5), S(38));
    m_radioDaysYear =
        makeRadio(m_panelTimeRange, Lang::get(QStringLiteral("RangeOneYear")), S(230), S(38));
    m_radioCustom =
        makeRadio(m_panelTimeRange, Lang::get(QStringLiteral("RangeCustom")), S(5), S(66));

    m_numCustomDays = new QSpinBox(m_panelTimeRange);
    m_numCustomDays->setGeometry(S(105), S(66), S(72), S(25));
    m_numCustomDays->setFont(scaledLabelFont());
    m_numCustomDays->setStyleSheet(UiStyle::spinBoxStyle());
    m_numCustomDays->setRange(1, 3650);
    m_numCustomDays->setValue(30);
    m_numCustomDays->setAlignment(Qt::AlignCenter);

    // 数量范围选项（独立容器）
    m_panelCountRange = new QWidget(m_groupRange);
    m_panelCountRange->setGeometry(0, S(22), S(424), S(72));

    m_radioCount30 =
        makeRadio(m_panelCountRange, Lang::get(QStringLiteral("Range30")), S(5), S(20));
    m_radioCount60 =
        makeRadio(m_panelCountRange, Lang::get(QStringLiteral("Range60")), S(230), S(20));
    m_radioCount90 =
        makeRadio(m_panelCountRange, Lang::get(QStringLiteral("Range90")), S(5), S(48));
    m_radioCount100 =
        makeRadio(m_panelCountRange, Lang::get(QStringLiteral("Range100")), S(230), S(48));

    /** 根据配置初始化选中项 */
    initRangeSelection();
    // 自定义天数输入框仅在“自定义天数”选中时可用
    m_numCustomDays->setEnabled(m_radioCustom->isChecked());
    connect(m_radioCustom, &QRadioButton::toggled, m_numCustomDays, &QWidget::setEnabled);

    // ------------------------------------------------------------ 关于（内嵌在设置项底部）
    // 并排三列：左列自上而下是 logo（应用名 + 副标题）、版本号、版权；
    // 中列是功能列表；右列是开源组件说明。上方只留一条分隔线。
    // 三列的横坐标在 layoutAbout() 里按实际内容宽度算，因此这里只给初始尺寸。
    QFont aboutNameFont(QStringLiteral("Segoe UI Semibold"), qRound(17 * kScale));
    aboutNameFont.setBold(true);
    QFont aboutVersionFont(QStringLiteral("Microsoft YaHei"), qRound(9.5 * kScale));
    QFont aboutSmallFont(QStringLiteral("Microsoft YaHei"), qRound(8.5 * kScale));

    m_lblAppName = makeInfoLabel(m_content, aboutNameFont, kAboutNameColor, kAboutLeft,
                                 kAboutBlockTop, kAboutLogoWidth, kAboutNameHeight);
    m_lblVersion = makeInfoLabel(m_content, aboutVersionFont, kAboutVersionColor, kAboutLeft,
                                 kAboutBlockTop, kAboutLogoWidth, kAboutVersionHeight);
    m_lblCopyright = makeInfoLabel(m_content, aboutSmallFont, kAboutCopyrightColor, kAboutLeft,
                                   kAboutBlockTop, kAboutLogoWidth, kAboutCopyrightHeight);

    m_aboutLineTop = makeSeparator(m_content, kAboutLeft, kAboutLineTopY, kContentWidth);

    m_lblFeatures = makeInfoLabel(m_content, aboutSmallFont, kAboutFeatureColor, kAboutLeft,
                                  kAboutBlockTop, 0, kAboutFeaturesHeight,
                                  Qt::AlignLeft | Qt::AlignTop);

    m_lblOpenSource =
        makeInfoLabel(m_content, aboutSmallFont, kAboutTitleColor, kAboutLeft, kAboutBlockTop, 0,
                      kAboutLicenseHeight, Qt::AlignLeft | Qt::AlignTop);
    m_lblQtLicense = makeInfoLabel(m_content, aboutSmallFont, kAboutFeatureColor, kAboutLeft,
                                   kAboutBlockTop, 0, kAboutLicenseHeight,
                                   Qt::AlignLeft | Qt::AlignTop);
    m_lblNmapLicense = makeInfoLabel(m_content, aboutSmallFont, kAboutFeatureColor, kAboutLeft,
                                     kAboutBlockTop, 0, kAboutLicenseHeight,
                                     Qt::AlignLeft | Qt::AlignTop);

    refreshAboutText();

    // 「按时间保存 / 按数量保存 / 不清除 / 不保存」四选一。
    // 选中「不清除」或「不保存」时整块隐藏「保留范围」，并让「关于」区随之上移，
    // 因此这里必须等「关于」区控件全部建好之后再挂信号、回滚/初始化控件。
    connect(m_radioByTime, &QRadioButton::toggled, this, &SettingsPanel::updateRangeVisibility);
    connect(m_radioByCount, &QRadioButton::toggled, this, &SettingsPanel::updateRangeVisibility);
    connect(m_radioNever, &QRadioButton::toggled, this, &SettingsPanel::updateRangeVisibility);
    connect(m_radioSaveNever, &QRadioButton::toggled, this, &SettingsPanel::updateRangeVisibility);
    syncFromSettings();
}

void SettingsPanel::layoutAbout()
{
    const int span = m_content->width() - 2 * kAboutLeft;
    if (span <= 0)
        return;

    // 选了「不清除」或「不保存」时「保留范围」分组整块隐藏，「关于」区随之上移，
    // 贴到「清理方式」分组下方，避免中间留出大块空白。
    const int blockShift =
        m_groupRange->isHidden()
            ? -(m_groupRange->geometry().bottom() - m_groupMethod->geometry().bottom())
            : 0;
    const int lineTopY = kAboutLineTopY + blockShift;
    const int blockTop = kAboutBlockTop + blockShift;

    // 中/右两列按各自文本的自然宽度摆放，剩余宽度平均分成两段间距，
    // 这样窗口变宽时三列一起铺开，变窄时收到最小间距为止。
    const QFontMetrics featureMetrics(m_lblFeatures->font());
    int featureWidth = 0;
    const QStringList featureLines = m_lblFeatures->text().split(QLatin1Char('\n'));
    for (const QString &line : featureLines)
        featureWidth = qMax(featureWidth, featureMetrics.horizontalAdvance(line));

    const QFontMetrics licenseMetrics(m_lblNmapLicense->font());
    int licenseWidth = licenseMetrics.horizontalAdvance(m_lblOpenSource->text());
    licenseWidth = qMax(licenseWidth, licenseMetrics.horizontalAdvance(m_lblQtLicense->text()));
    licenseWidth = qMax(licenseWidth, licenseMetrics.horizontalAdvance(m_lblNmapLicense->text()));

    const int gap =
        qMax(kAboutMinGap, (span - kAboutLogoWidth - featureWidth - licenseWidth) / 2);
    const int featureX = kAboutLeft + kAboutLogoWidth + gap;
    const int licenseX = featureX + featureWidth + gap;

    // 唯一的分隔线横跨整个内容宽度
    m_aboutLineTop->setGeometry(kAboutLeft, lineTopY, span, 1);

    // 左列三行叠放，整体与中/右两列垂直居中
    const int leftStack = kAboutNameHeight + kAboutVersionHeight + kAboutCopyrightHeight;
    int y = blockTop + kAboutFeaturesHeight / 2 - leftStack / 2;
    m_lblAppName->setGeometry(kAboutLeft, y, kAboutLogoWidth, kAboutNameHeight);
    y += kAboutNameHeight;
    m_lblVersion->setGeometry(kAboutLeft, y, kAboutLogoWidth, kAboutVersionHeight);
    y += kAboutVersionHeight;
    m_lblCopyright->setGeometry(kAboutLeft, y, kAboutLogoWidth, kAboutCopyrightHeight);

    m_lblFeatures->setGeometry(featureX, blockTop, featureWidth, kAboutFeaturesHeight);
    m_lblOpenSource->setGeometry(licenseX, blockTop, licenseWidth, kAboutLicenseHeight);
    m_lblQtLicense->setGeometry(licenseX, blockTop + kAboutLicenseLineGap, licenseWidth,
                                kAboutLicenseHeight);
    m_lblNmapLicense->setGeometry(licenseX, blockTop + 2 * kAboutLicenseLineGap, licenseWidth,
                                  kAboutLicenseHeight);
}

bool SettingsPanel::eventFilter(QObject *watched, QEvent *event)
{
    // 内容列宽度变化时重排「关于」区的三列
    if (watched == m_content && event->type() == QEvent::Resize)
        layoutAbout();
    return QWidget::eventFilter(watched, event);
}


void SettingsPanel::refreshAboutText()
{
    m_lblAppName->setText(QStringLiteral("NETScratch"));
    m_lblVersion->setText(
        Lang::fmt(QStringLiteral("AboutVersion"), QApplication::applicationVersion()));

    // 功能简介：配置里以 | 分隔，这里每行一项
    QStringList featureLines;
    const QStringList features =
        Lang::get(QStringLiteral("AboutFeatures")).split(QLatin1Char('|'));
    for (const QString &feature : features)
        featureLines.append(QStringLiteral("  ") + feature);
    m_lblFeatures->setText(featureLines.join(QLatin1Char('\n')));

    m_lblOpenSource->setText(Lang::get(QStringLiteral("AboutOpenSource")));
    m_lblQtLicense->setText(QStringLiteral("  ") + Lang::get(QStringLiteral("AboutQtLicense")));
    m_lblNmapLicense->setText(QStringLiteral("  ") + Lang::get(QStringLiteral("AboutNmapLicense")));
    m_lblCopyright->setText(QStringLiteral("Copyright \u00A9 %1 %2")
                                .arg(QDate::currentDate().year())
                                .arg(QStringLiteral("youye-luna")));

    // 文案定了之后再按实际宽度摆三列的位置
    layoutAbout();
}

void SettingsPanel::applyLanguage()
{
    m_lblTitle->setText(Lang::get(QStringLiteral("SettingsTitle")));

    m_lblLangTimeSection->setText(Lang::get(QStringLiteral("LanguageTimeSection")));
    m_lblLanguage->setText(Lang::get(QStringLiteral("LanguageLabel")));
    m_lblLanguage->adjustSize();
    m_lblDateFormat->setText(Lang::get(QStringLiteral("DateFormatLabel")));
    m_lblDateFormat->adjustSize();
    m_lblTimeFormat->setText(Lang::get(QStringLiteral("TimeFormatLabel")));
    m_lblTimeFormat->adjustSize();
    // 格式样例本身与语言无关，只需重建「跟随界面语言」这一项，并保留用户当前的选择
    fillFormatCombo(m_comboDateFormat, AppSettings::supportedDateFormats(),
                    m_comboDateFormat->currentData().toString(), true);
    fillFormatCombo(m_comboTimeFormat, AppSettings::supportedTimeFormats(),
                    m_comboTimeFormat->currentData().toString(), false);

    m_lblScanSection->setText(Lang::get(QStringLiteral("ScanSettingsSection")));
    m_lblThreads->setText(Lang::get(QStringLiteral("ThreadsLabel")));
    m_lblThreads->adjustSize();
    m_lblHint->setText(Lang::get(QStringLiteral("ThreadsHint")));
    m_lblIpv6->setText(Lang::get(QStringLiteral("Ipv6Toggle")));
    m_lblIpv6->adjustSize();
    m_lblDefaultAdapter->setText(Lang::get(QStringLiteral("DefaultAdapterLabel")));
    m_lblDefaultAdapter->adjustSize();
    m_btnRefreshAdapter->setText(Lang::get(QStringLiteral("AdapterRefresh")));
    // 重建下拉项（「自动选择」文案随语言变化），保留用户当前选择
    UiStyle::fillAdapterCombo(m_comboDefaultAdapter,
                              m_comboDefaultAdapter->currentData().toString());

    m_lblHistorySection->setText(Lang::get(QStringLiteral("HistorySettings")));

    m_groupMethod->setTitle(Lang::get(QStringLiteral("SaveMethodGroup")));
    m_radioByTime->setText(Lang::get(QStringLiteral("SaveByTime")));
    m_radioByCount->setText(Lang::get(QStringLiteral("SaveByCount")));
    m_radioNever->setText(Lang::get(QStringLiteral("RangeNever")));
    m_radioSaveNever->setText(Lang::get(QStringLiteral("SaveNever")));

    m_groupRange->setTitle(Lang::get(QStringLiteral("SaveRangeGroup")));
    m_radioDays14->setText(Lang::get(QStringLiteral("Range14Days")));
    m_radioDaysHalf->setText(Lang::get(QStringLiteral("RangeHalfMonth")));
    m_radioDaysMonth->setText(Lang::get(QStringLiteral("RangeOneMonth")));
    m_radioDaysYear->setText(Lang::get(QStringLiteral("RangeOneYear")));
    m_radioCustom->setText(Lang::get(QStringLiteral("RangeCustom")));
    m_radioCount30->setText(Lang::get(QStringLiteral("Range30")));
    m_radioCount60->setText(Lang::get(QStringLiteral("Range60")));
    m_radioCount90->setText(Lang::get(QStringLiteral("Range90")));
    m_radioCount100->setText(Lang::get(QStringLiteral("Range100")));

    // 选项文本长度随语言变化，重算自适应尺寸
    const QVector<QRadioButton *> radios{m_radioByTime,    m_radioByCount, m_radioNever,
                                         m_radioSaveNever, m_radioDays14,  m_radioDaysHalf,
                                         m_radioDaysMonth, m_radioDaysYear, m_radioCustom,
                                         m_radioCount30,   m_radioCount60, m_radioCount90,
                                         m_radioCount100};
    for (QRadioButton *radio : radios)
        radio->adjustSize();

    m_btnSaveSettings->setText(Lang::get(QStringLiteral("SaveSettings")));

    refreshAboutText();
}

void SettingsPanel::initRangeSelection()
{
    switch (m_settings.historySaveDays) {
    case 14:
        m_radioDays14->setChecked(true);
        break;
    case 15:
        m_radioDaysHalf->setChecked(true);
        break;
    case 30:
        m_radioDaysMonth->setChecked(true);
        break;
    case 365:
        m_radioDaysYear->setChecked(true);
        break;
    case 0:
        // 「不清除」由「清理方式」分组单独处理，这里不再选任何时间选项
        break;
    default:
        // 自定义天数
        m_radioCustom->setChecked(true);
        m_numCustomDays->setValue(qBound(1, m_settings.historySaveDays, 3650));
        break;
    }

    switch (m_settings.historySaveMaxRecords) {
    case 30:
        m_radioCount30->setChecked(true);
        break;
    case 60:
        m_radioCount60->setChecked(true);
        break;
    case 90:
        m_radioCount90->setChecked(true);
        break;
    default:
        m_radioCount100->setChecked(true);
        break;
    }
}

void SettingsPanel::updateRangeVisibility()
{
    const bool noRange = m_radioNever->isChecked() || m_radioSaveNever->isChecked();
    // 选了「不清除」或「不保存」时整块「保留范围」都不需要
    m_groupRange->setVisible(!noRange);
    m_panelTimeRange->setVisible(m_radioByTime->isChecked());
    m_panelCountRange->setVisible(m_radioByCount->isChecked());
    // 可见性变了，「关于」区要跟着上移或回落
    layoutAbout();
}

void SettingsPanel::collectFromWidgets(AppSettings &out) const
{
    // 语言、时间格式与线程数随「保存设置」一并写入配置
    out.language = languageParse(m_comboLanguage->currentText());
    out.scanThreads = m_numThreads->value();
    out.ipv6Enabled = m_swIpv6->isChecked();
    out.scanAdapter = m_comboDefaultAdapter->currentData().toString();
    out.dateFormat = m_comboDateFormat->currentData().toString();
    out.timeFormat = m_comboTimeFormat->currentData().toString();

    if (m_radioSaveNever->isChecked()) {
        // 不保存：天数/条数保持原值，便于切回其它方式时恢复原先的选择
        out.historySaveMode = HistorySaveMode::None;
        return;
    }

    if (m_radioByCount->isChecked()) {
        out.historySaveMode = HistorySaveMode::ByCount;
        if (m_radioCount30->isChecked())
            out.historySaveMaxRecords = 30;
        else if (m_radioCount60->isChecked())
            out.historySaveMaxRecords = 60;
        else if (m_radioCount90->isChecked())
            out.historySaveMaxRecords = 90;
        else
            out.historySaveMaxRecords = 100;
    } else {
        out.historySaveMode = HistorySaveMode::ByTime;
        if (m_radioNever->isChecked())
            out.historySaveDays = 0; // 不清除
        else if (m_radioDays14->isChecked())
            out.historySaveDays = 14;
        else if (m_radioDaysHalf->isChecked())
            out.historySaveDays = 15;
        else if (m_radioDaysMonth->isChecked())
            out.historySaveDays = 30;
        else if (m_radioDaysYear->isChecked())
            out.historySaveDays = 365;
        else
            out.historySaveDays = m_numCustomDays->value(); // 自定义天数
    }
}

void SettingsPanel::applySaveConfig()
{
    collectFromWidgets(m_settings);
}

bool SettingsPanel::hasUnsavedChanges() const
{
    AppSettings pending = m_settings;
    collectFromWidgets(pending);
    return pending != m_settings;
}

void SettingsPanel::syncFromSettings()
{
    m_comboLanguage->setCurrentIndex(
        m_comboLanguage->findText(languageDisplay(m_settings.language)));
    fillFormatCombo(m_comboDateFormat, AppSettings::supportedDateFormats(), m_settings.dateFormat,
                    true);
    fillFormatCombo(m_comboTimeFormat, AppSettings::supportedTimeFormats(), m_settings.timeFormat,
                    false);
    m_numThreads->setValue(qBound(1, m_settings.scanThreads, 100));
    m_swIpv6->setChecked(m_settings.ipv6Enabled);
    // 配置里的网卡可能已被拔出或禁用：此时回落到「自动选择」，并清掉失效的 GUID，
    // 免得把不存在的设备名传给 nmap 导致扫描失败
    if (!m_settings.scanAdapter.isEmpty()
        && m_comboDefaultAdapter->findData(m_settings.scanAdapter) < 0)
        m_settings.scanAdapter.clear();
    UiStyle::fillAdapterCombo(m_comboDefaultAdapter, m_settings.scanAdapter);

    if (m_settings.historySaveMode == HistorySaveMode::None) {
        m_radioSaveNever->setChecked(true);
    } else if (m_settings.historySaveMode == HistorySaveMode::ByCount) {
        m_radioByCount->setChecked(true);
    } else if (m_settings.historySaveDays == 0) {
        m_radioNever->setChecked(true);
    } else {
        m_radioByTime->setChecked(true);
    }
    initRangeSelection();
    updateRangeVisibility();
}

bool SettingsPanel::confirmLeave()
{
    if (!hasUnsavedChanges())
        return true;

    QMessageBox box(this);
    box.setWindowTitle(Lang::get(QStringLiteral("UnsavedChangesTitle")));
    box.setText(Lang::get(QStringLiteral("UnsavedChangesText")));
    box.setIcon(QMessageBox::Question);
    QPushButton *saveButton =
        box.addButton(Lang::get(QStringLiteral("SaveSettings")), QMessageBox::AcceptRole);
    box.addButton(Lang::get(QStringLiteral("DiscardChanges")), QMessageBox::DestructiveRole);
    QPushButton *cancelButton =
        box.addButton(Lang::get(QStringLiteral("Cancel")), QMessageBox::RejectRole);
    box.setDefaultButton(saveButton);
    box.exec();

    QAbstractButton *clicked = box.clickedButton();
    if (clicked == cancelButton)
        return false; // 取消：留在设置页

    if (clicked == saveButton) {
        // 与「保存设置」按钮走同一条保存路径，但不重复弹「已应用」提示
        applySaveConfig();
        m_settings.save();
        ScanHistoryStore::prune();
        emit settingsSaved();
        return true;
    }

    // 不保存：把控件回滚到已保存的配置，避免残留的半改动状态再次触发提示
    syncFromSettings();
    return true;
}
