#pragma once

#include <QVector>
#include <QWidget>

#include "appsettings.h"
#include "scanhistory.h"

class QLabel;
class QPushButton;
class QTableWidget;

/// 扫描历史页面（查看/删除/清空历史扫描记录）。
/// 作为主窗口导航栏的一个页面，与主页/设置共用同一套界面样式。
class HistoryPanel : public QWidget
{
    Q_OBJECT
public:
    explicit HistoryPanel(QWidget *parent = nullptr);

    /// 重新从磁盘加载历史记录并刷新列表（切换到本页时调用）
    void reload();

    /// 语言切换后刷新页面文案
    void applyLanguage();

signals:
    /// 用户点击「查看详情」或双击某行，请求在主窗口加载该条历史记录
    void recordActivated(const ScanHistoryRecord &record);

private:
    /// 按当前语言把 m_records 渲染到表格，并切换空状态
    void fillTable();

    void viewSelected();
    void deleteSelected();
    void clearAll();

    QVector<ScanHistoryRecord> m_records;

    /// 当前是否处于「不保存」模式：为真时列表区显示专用提示而非记录
    bool m_historyDisabled = false;

    /// 当前配置：用于按设置的日期/时间格式渲染扫描时间
    AppSettings m_settings;

    QLabel *m_lblTitle = nullptr;
    QLabel *m_lblHint = nullptr;
    QTableWidget *m_table = nullptr;
    QLabel *m_lblEmpty = nullptr;
    QPushButton *m_btnView = nullptr;
    QPushButton *m_btnDelete = nullptr;
    QPushButton *m_btnClear = nullptr;
};
