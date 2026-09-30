#pragma once

#include <QWidget>

#include "appsettings.h"

class QComboBox;
class QEvent;
class QGroupBox;
class QLabel;
class QPushButton;
class QRadioButton;
class QScrollArea;
class QSpinBox;
class ToggleSwitch;

/// 设置页（界面语言、扫描线程数、历史数据保存配置）。
/// 作为主窗口顶部导航栏的「设置」页面显示。
class SettingsPanel : public QWidget
{
    Q_OBJECT
public:
    explicit SettingsPanel(QWidget *parent = nullptr);

    /// 语言切换后刷新面板内文案
    void applyLanguage();

    /// 从设置页切走前调用：有未保存的更改时弹窗询问「保存 / 不保存 / 取消」。
    /// 返回 true 表示可以切走（已保存或已放弃更改），false 表示用户取消了切换。
    bool confirmLeave();

signals:
    /// 点击「保存设置」并成功写入配置后发出
    void settingsSaved();

private:
    void initRangeSelection();
    void updateRangeVisibility();
    void applySaveConfig();

    /// 把界面控件当前值写入 out（不改动 m_settings，「保存设置」与未保存检测共用）
    void collectFromWidgets(AppSettings &out) const;

    /// 控件当前状态是否与已保存的配置不一致
    bool hasUnsavedChanges() const;

    /// 把全部控件回滚到已保存的配置值（构造初始化与「不保存」共用）
    void syncFromSettings();

    /// 刷新底部「关于」区块文案（构造与语言切换时调用）
    void refreshAboutText();

    /// 按内容列当前宽度摆放「关于」区的三列（宽度变化时调用）
    void layoutAbout();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    AppSettings m_settings;

    /// 设置内容列的滚动容器：内容超出标签页高度时垂直滚动
    QScrollArea *m_scrollArea = nullptr;

    /// 设置内容列：宽度随窗口拉伸，内部沿用固定坐标布局
    QWidget *m_content = nullptr;

    /// 分区标题：语言和时间（下辖界面语言 / 日期格式 / 时间格式）
    QLabel *m_lblLangTimeSection = nullptr;

    QLabel *m_lblLanguage = nullptr;
    QComboBox *m_comboLanguage = nullptr;

    QLabel *m_lblDateFormat = nullptr;
    QComboBox *m_comboDateFormat = nullptr;
    QLabel *m_lblTimeFormat = nullptr;
    QComboBox *m_comboTimeFormat = nullptr;

    /// 分区标题：扫描设置（下辖扫描线程数）
    QLabel *m_lblScanSection = nullptr;

    QLabel *m_lblThreads = nullptr;
    QSpinBox *m_numThreads = nullptr;
    QLabel *m_lblHint = nullptr;

    /// IPv6 显示开关（iOS 样式，位于线程数说明下方，同属「扫描设置」区）
    ToggleSwitch *m_swIpv6 = nullptr;
    QLabel *m_lblIpv6 = nullptr;

    /// 分区标题：历史记录设置（下辖「数据保存时长」与「保存范围」）
    QLabel *m_lblHistorySection = nullptr;

    QGroupBox *m_groupMethod = nullptr;
    QRadioButton *m_radioByTime = nullptr;
    QRadioButton *m_radioByCount = nullptr;

    QGroupBox *m_groupRange = nullptr;
    QWidget *m_panelTimeRange = nullptr;
    QWidget *m_panelCountRange = nullptr;

    QRadioButton *m_radioDays14 = nullptr;
    QRadioButton *m_radioDaysHalf = nullptr;
    QRadioButton *m_radioDaysMonth = nullptr;
    QRadioButton *m_radioDaysYear = nullptr;
    QRadioButton *m_radioNever = nullptr;
    QRadioButton *m_radioCustom = nullptr;
    QSpinBox *m_numCustomDays = nullptr;

    QRadioButton *m_radioCount30 = nullptr;
    QRadioButton *m_radioCount60 = nullptr;
    QRadioButton *m_radioCount90 = nullptr;
    QRadioButton *m_radioCount100 = nullptr;

    QPushButton *m_btnSaveSettings = nullptr;

    /// 页面标题（与历史页保持一致）
    QLabel *m_lblTitle = nullptr;

    // 「关于」区块（内嵌在设置项底部）
    QWidget *m_aboutLineTop = nullptr;
    QLabel *m_lblAppName = nullptr;
    QLabel *m_lblVersion = nullptr;
    QLabel *m_lblFeatures = nullptr;
    QLabel *m_lblOpenSource = nullptr;
    QLabel *m_lblQtLicense = nullptr;
    QLabel *m_lblNmapLicense = nullptr;
    QLabel *m_lblCopyright = nullptr;
};
