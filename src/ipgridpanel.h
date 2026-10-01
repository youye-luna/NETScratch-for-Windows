#pragma once

#include <QAbstractScrollArea>
#include <QColor>
#include <QVector>

/// IP 地址分布图：255 个格子（16 列），第 i 格对应 IP 末段 i+1。
/// 对应 C# 的 IPGridPanel（自绘 + 自动滚动）。
class IPGridPanel : public QAbstractScrollArea
{
    Q_OBJECT
public:
    explicit IPGridPanel(QWidget *parent = nullptr);

    void setIpColor(int ipIndex, const QColor &color);
    void resetAllColors();

signals:
    void cellClicked(int ipIndex);
    void cellDoubleClicked(int ipIndex);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    static int rowTotal();

    int cellSize() const;
    int cellAt(const QPoint &point) const;
    void updateScrollRange();

    QVector<QColor> m_cellColors;
    int m_selectedIndex = -1;
    bool m_updatingRange = false;
};
