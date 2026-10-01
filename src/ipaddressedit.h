#pragma once

#include <QSize>
#include <QString>
#include <QWidget>

class QLineEdit;

/// 点分四段 IP 地址输入控件。
/// 复刻 C# 版 SysIPAddress32 原生控件的行为：每段 0-255、获得焦点全选、
/// 输满 3 位或按点号自动跳到下一段。
class IpAddressEdit : public QWidget
{
    Q_OBJECT
public:
    explicit IpAddressEdit(QWidget *parent = nullptr);

    /// 当前输入的 IP 文本；任一段为空时返回空字符串
    QString address() const;

    /// 设置 IP 地址（仅接受合法的 IPv4 地址，非法输入忽略）
    void setAddress(const QString &ip);

    QSize sizeHint() const override;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    int indexOfSegment(const QObject *object) const;
    void focusSegment(int index);

    QLineEdit *m_segments[4] = {nullptr, nullptr, nullptr, nullptr};
};
