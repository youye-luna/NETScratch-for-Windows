#include "ipgridpanel.h"

#include "lang.h"

#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QResizeEvent>
#include <QScrollBar>

namespace
{
const int kTotalCells = 255;
const int kCols = 16;
const int kGridPadding = 4;
const int kTitleHeight = 22;
const int kLegendHeight = 22;
const int kLegendDot = 9;
const int kMinCellSize = 12;

QColor defaultCellColor() { return QColor(240, 240, 240); }
} // namespace

IPGridPanel::IPGridPanel(QWidget *parent)
    : QAbstractScrollArea(parent)
{
    m_cellColors.resize(kTotalCells);
    for (int i = 0; i < kTotalCells; ++i)
        m_cellColors[i] = defaultCellColor();

    setFrameShape(QFrame::Box);
    setFrameShadow(QFrame::Plain);
    setLineWidth(1);

    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    viewport()->installEventFilter(this);

    connect(verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this](int) { viewport()->update(); });

    updateScrollRange();
}

int IPGridPanel::rowTotal() { return (kTotalCells + kCols - 1) / kCols; }

int IPGridPanel::cellSize() const
{
    const int size = (viewport()->width() - kGridPadding * 2) / kCols;
    return qMax(size, kMinCellSize);
}

void IPGridPanel::setIpColor(int ipIndex, const QColor &color)
{
    if (ipIndex >= 0 && ipIndex < kTotalCells)
    {
        m_cellColors[ipIndex] = color;
        viewport()->update();
    }
}

void IPGridPanel::resetAllColors()
{
    for (int i = 0; i < kTotalCells; ++i)
        m_cellColors[i] = defaultCellColor();
    m_selectedIndex = -1;
    viewport()->update();
}

int IPGridPanel::cellAt(const QPoint &point) const
{
    const int size = cellSize();
    const int gridWidth = size * kCols;
    const int startX = (viewport()->width() - gridWidth) / 2;
    const int startY = kTitleHeight - verticalScrollBar()->value();

    // 与 C# 一致使用整数除法（负数向零截断），不做额外保护
    const int col = (point.x() - startX) / size;
    const int row = (point.y() - startY) / size;
    if (col < 0 || col >= kCols || row < 0 || row >= rowTotal())
        return -1;

    const int index = row * kCols + col;
    return (index >= 0 && index < kTotalCells) ? index : -1;
}

void IPGridPanel::updateScrollRange()
{
    if (m_updatingRange)
        return;
    m_updatingRange = true;

    QScrollBar *bar = verticalScrollBar();
    const int barWidth = bar->sizeHint().width();
    const int totalHeight = kTitleHeight + cellSize() * rowTotal() + kLegendHeight + kGridPadding;
    const int maxOffset = qMax(0, totalHeight - viewport()->height());

    // 滞回判定：滚动条出现/消失会改变 viewport 宽度，从而改变 cellSize 与总高度，
    // 若按同一阈值切换会来回抖动。
    const bool needBar = bar->isVisible() ? (maxOffset > 0) : (maxOffset > barWidth);
    setVerticalScrollBarPolicy(needBar ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);

    // 策略切换可能即时改变 viewport 宽度，按新宽度重算一次滚动范围
    const int newTotalHeight =
        kTitleHeight + cellSize() * rowTotal() + kLegendHeight + kGridPadding;
    bar->setRange(0, qMax(0, newTotalHeight - viewport()->height()));
    bar->setPageStep(viewport()->height());
    bar->setSingleStep(qMax(1, cellSize()));

    m_updatingRange = false;
}

void IPGridPanel::resizeEvent(QResizeEvent *event)
{
    QAbstractScrollArea::resizeEvent(event);
    updateScrollRange();
}

bool IPGridPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == viewport())
    {
        switch (event->type())
        {
        case QEvent::MouseButtonRelease:
        {
            QMouseEvent *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->button() == Qt::LeftButton)
            {
                const int index = cellAt(mouseEvent->pos());
                if (index >= 0)
                {
                    m_selectedIndex = index;
                    viewport()->update();
                    emit cellClicked(index);
                }
            }
            break;
        }
        case QEvent::MouseButtonDblClick:
        {
            QMouseEvent *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->button() == Qt::LeftButton)
            {
                const int index = cellAt(mouseEvent->pos());
                if (index >= 0)
                {
                    m_selectedIndex = index;
                    viewport()->update();
                    emit cellDoubleClicked(index);
                }
            }
            break;
        }
        case QEvent::Resize:
            updateScrollRange();
            break;
        default:
            break;
        }
    }

    return QAbstractScrollArea::eventFilter(watched, event);
}

void IPGridPanel::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)

    QPainter painter(viewport());
    painter.fillRect(viewport()->rect(), Qt::white);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    const int size = cellSize();
    const int inner = size - 1;
    const int gridWidth = size * kCols;
    const int gridHeight = size * rowTotal();
    const int startX = (viewport()->width() - gridWidth) / 2;
    const int scrollY = verticalScrollBar()->value();
    const int startY = kTitleHeight - scrollY;

    // 标题
    QFont titleFont = font();
    titleFont.setPointSizeF(9.0);
    titleFont.setBold(true);
    const QFontMetrics titleMetrics(titleFont);
    const QString title = Lang::get(QStringLiteral("IpDistribution"));
    const int titleWidth = titleMetrics.horizontalAdvance(title);
    painter.setFont(titleFont);
    painter.setPen(QColor(105, 105, 105));
    painter.drawText(QPointF((viewport()->width() - titleWidth) / 2.0,
                             2 - scrollY + titleMetrics.ascent()),
                     title);

    // 数字
    QFont numFont = font();
    numFont.setPointSizeF(size >= 22 ? 8.0 : (size >= 16 ? 7.0 : 6.0));
    numFont.setBold(true);
    const QFontMetricsF numMetrics(numFont);
    const qreal lineHeight = numMetrics.height();
    painter.setFont(numFont);

    for (int i = 0; i < kTotalCells; ++i)
    {
        const int row = i / kCols;
        const int col = i % kCols;
        const int x = startX + col * size;
        const int y = startY + row * size;

        const QRect rect(x, y, inner, inner);
        if (rect.width() > 0 && rect.height() > 0)
            painter.fillRect(rect, m_cellColors[i]);

        if (i == m_selectedIndex)
        {
            painter.setPen(QPen(QColor(255, 87, 34), 2.0));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(rect);
        }

        const QString text = QString::number(i + 1);
        const qreal textWidth = numMetrics.horizontalAdvance(text);
        if (textWidth < inner && lineHeight < inner)
        {
            const QColor bg = m_cellColors[i];
            const qreal brightness =
                (bg.red() * 0.299 + bg.green() * 0.587 + bg.blue() * 0.114) / 255.0;
            painter.setPen(brightness < 0.5 ? QColor(Qt::white) : QColor(Qt::black));
            const qreal tx = x + (inner - textWidth) / 2.0;
            const qreal ty = y + (inner - lineHeight) / 2.0;
            painter.drawText(QPointF(tx, ty + numMetrics.ascent()), text);
        }
    }

    // 图例
    const int legendY = startY + gridHeight + 3;
    int legendX = startX;

    struct LegendItem
    {
        QColor color;
        const char *key;
    };
    const LegendItem legendItems[5] = {
        {QColor(240, 240, 240), "NotScanned"},
        {QColor(76, 175, 80), "NoDevice"},
        {QColor(33, 150, 243), "Online"},
        {QColor(123, 31, 162), "ColCamera"},
        {QColor(255, 138, 128), "ColDhcp"},
    };

    QFont legendFont = font();
    legendFont.setPointSizeF(6.5);
    const QFontMetrics legendMetrics(legendFont);
    painter.setFont(legendFont);

    for (const LegendItem &item : legendItems)
    {
        painter.fillRect(QRect(legendX, legendY, kLegendDot, kLegendDot), item.color);
        const QString text = Lang::get(QString::fromLatin1(item.key));
        painter.setPen(QColor(105, 105, 105));
        painter.drawText(QPointF(legendX + kLegendDot + 2, legendY + legendMetrics.ascent()),
                         text);
        legendX += legendMetrics.horizontalAdvance(text) + 16;
    }
}
