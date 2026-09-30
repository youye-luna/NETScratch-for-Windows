#pragma once

#include "scanhistory.h"
#include "scanner.h"

#include <QVector>
#include <QWidget>

class HistoryPanel;
class IpAddressEdit;
class QCloseEvent;
class QComboBox;
class QDialog;
class QLabel;
class QProgressBar;
class QPushButton;
class QShowEvent;
class QStackedWidget;
class QTabWidget;
class QWinTaskbarButton;
class QWinTaskbarProgress;
class SettingsPanel;

/// 主窗口：顶部导航栏 + 主页/历史/设置三个页面的容器
class MainWindow : public QWidget
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    /// 预览用：不扫描，直接弹出设备详情窗（--preview 启动参数）
    void showDeviceDetailPreview();

protected:
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;

private slots:
    void onScanClicked();
    void onStopClicked();
    void onClearClicked();
    void onExportClicked();
    void onSettingsSaved();
    void onHistoryRecordActivated(const ScanHistoryRecord &record);

    void onScanProgress(int progress);
    void onScanCompleted(const QVector<DhcpServerInfo> &results);
    void onScanError(const QString &message);
    void onScanFinished();

private:
    /// 顶部导航对应的页面（顺序与 QStackedWidget 一致）
    enum Page
    {
        PageHome = 0,
        PageHistory = 1,
        PageSettings = 2,
    };

    void buildUi();
    QWidget *buildNavBar();
    QWidget *buildHomePage();
    void applyLanguage();
    void switchToPage(int page);
    void setScanningUiEnabled(bool scanning);
    /// 重建首页网卡下拉，选中 selected 指定的网卡（空串 = 自动选择；找不到则回落自动选择）
    void refreshAdapterCombo(const QString &selected);
    /// 按当前选中的网卡把扫描范围重填为该网卡的 /24 网段
    void applyAdapterToRange();
    void populateResultTabs(const QVector<DhcpServerInfo> &results);
    void clearResultTabs();
    void saveScanHistory(const QVector<DhcpServerInfo> &results);
    void displayHistoryRecord(const ScanHistoryRecord &record);
    bool exportToCsv(const QString &filePath, QString *errorMessage);
    void showTooManySubnetsMessage(int count);
    void showProgressDialog();
    void closeProgressDialog();

    // 顶部导航栏
    QWidget *m_navBar = nullptr;
    QLabel *m_navTitle = nullptr;
    QPushButton *m_navHome = nullptr;
    QPushButton *m_navHistory = nullptr;
    QPushButton *m_navSettings = nullptr;

    // 页面容器
    QStackedWidget *m_stack = nullptr;
    QWidget *m_pageHome = nullptr;
    HistoryPanel *m_pageHistory = nullptr;
    SettingsPanel *m_pageSettings = nullptr;

    // 主页：搜索卡片
    QLabel *m_labelTitle = nullptr;
    QLabel *m_labelAdapter = nullptr;
    QComboBox *m_comboAdapter = nullptr;
    QPushButton *m_buttonRefreshAdapter = nullptr;
    QLabel *m_labelStartIp = nullptr;
    IpAddressEdit *m_ipStart = nullptr;
    QLabel *m_labelTo = nullptr;
    QLabel *m_labelEndIp = nullptr;
    IpAddressEdit *m_ipEnd = nullptr;
    QPushButton *m_buttonScan = nullptr;
    QPushButton *m_buttonStop = nullptr;
    QPushButton *m_buttonClear = nullptr;
    QPushButton *m_buttonExport = nullptr;

    // 主页：结果标签页
    QTabWidget *m_tabControlResults = nullptr;

    // 进度（显示在 Windows 任务栏图标上）
    QWinTaskbarButton *m_taskbarButton = nullptr;
    QWinTaskbarProgress *m_taskbarProgress = nullptr;

    // 扫描进度弹窗（带百分比）
    QDialog *m_progressDialog = nullptr;
    QLabel *m_progressRangeLabel = nullptr;
    QProgressBar *m_progressBar = nullptr;
    QLabel *m_progressHintLabel = nullptr;
    QPushButton *m_buttonStopInDialog = nullptr;

    // 状态栏
    QWidget *m_panelBottom = nullptr;
    QLabel *m_statusLabel = nullptr;
    QLabel *m_statusCount = nullptr;

    Scanner *m_scanner = nullptr;
    bool m_isScanning = false;
    QString m_currentStartIp;
    QString m_currentEndIp;
    /// 设置页保存的默认扫描网卡（适配器 GUID，空串 = 自动选择）
    QString m_defaultAdapter;
};
