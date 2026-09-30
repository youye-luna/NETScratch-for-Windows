#pragma once

#include <QList>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include "scanner.h"

class IPGridPanel;
class QTableWidget;

/// 单个网段的结果面板：左侧设备表格 + 右侧 IP 分布图。
/// 对应 C# 的 SubnetResultPanel（DataGridView + IPGridPanel）。
class SubnetResultPanel : public QWidget
{
    Q_OBJECT
public:
    explicit SubnetResultPanel(QWidget *parent = nullptr);

    /// 填充数据（清空后重建所有行与分布图颜色）
    void populateData(const QVector<DhcpServerInfo> &devices);

    /// 当前表格所有行数据（每行 7 项：表格前 5 列含 IPv6 + 补回的 DHCP / 状态列，用于导出；
    /// IPv6 显示关闭时该项为横杠）
    QList<QStringList> getRows() const;

    /// 切换语言后刷新列头与单元格文本
    void refreshLanguage();

    /// IPv6 列可见性：关闭时隐藏表格 IPv6 列，导出该列填横杠
    void setIpv6Visible(bool visible);

    /// 预览用：不扫描，填入示例设备后直接弹出设备详情窗（--preview 启动参数）
    void showDetailPreview();

private:
    void initTable();
    void applyRowStyle(int row, const DhcpServerInfo &info);

    /// IP 分布图格子单击：跳转到表格中对应 IP 行并高亮
    void jumpToRowByLastOctet(int ipLast);

    /// 弹出设备详情对话框
    void showDetailDialog(int row);

    QTableWidget *m_grid = nullptr;
    IPGridPanel *m_ipGrid = nullptr;
    QVector<DhcpServerInfo> m_rowInfos;
    bool m_ipv6Visible = true;
};
