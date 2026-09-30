#include "historypanel.h"

#include <QAbstractItemView>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPalette>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include "lang.h"
#include "uistyle.h"

namespace
{
/// 使单元格只读且不可编辑
QTableWidgetItem *makeCell(const QString &text)
{
    QTableWidgetItem *item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

/// 历史列表列数：时间 / 范围 / 总数 / 在线 / DHCP服务器
const int kColumnCount = 5;
} // namespace

HistoryPanel::HistoryPanel(QWidget *parent)
    : QWidget(parent)
{
    UiStyle::applyPageBackground(this);
    setFont(UiStyle::labelFont());

    QVBoxLayout *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(16, 16, 16, 16);
    rootLayout->setSpacing(12);

    // ------------------------------------------------------------ 标题行
    m_lblTitle = new QLabel(this);
    m_lblTitle->setFont(UiStyle::pageTitleFont());
    m_lblTitle->setStyleSheet(QStringLiteral("color: #1f2329;"));

    m_btnView = new QPushButton(this);
    m_btnView->setStyleSheet(UiStyle::primaryButtonStyle());
    m_btnDelete = new QPushButton(this);
    m_btnDelete->setStyleSheet(UiStyle::secondaryButtonStyle());
    m_btnClear = new QPushButton(this);
    m_btnClear->setStyleSheet(UiStyle::secondaryButtonStyle());

    const QVector<QPushButton *> buttons{m_btnView, m_btnDelete, m_btnClear};
    for (QPushButton *button : buttons)
    {
        button->setFixedSize(UiStyle::kButtonWidth, UiStyle::kButtonHeight);
        button->setFont(UiStyle::labelFont());
        button->setCursor(Qt::PointingHandCursor);
    }

    QHBoxLayout *titleLayout = new QHBoxLayout;
    titleLayout->setContentsMargins(0, 0, 0, 0);
    titleLayout->setSpacing(10);
    titleLayout->addWidget(m_lblTitle);
    titleLayout->addStretch(1);
    titleLayout->addWidget(m_btnView);
    titleLayout->addWidget(m_btnDelete);
    titleLayout->addWidget(m_btnClear);
    rootLayout->addLayout(titleLayout);

    // ------------------------------------------------------------ 列表卡片
    QWidget *card = UiStyle::makeCard(this);
    QVBoxLayout *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(14, 12, 14, 14);
    cardLayout->setSpacing(8);

    m_lblHint = new QLabel(card);
    {
        QFont font(QStringLiteral("Microsoft YaHei"));
        font.setPointSizeF(8.5);
        m_lblHint->setFont(font);
        QPalette palette = m_lblHint->palette();
        palette.setColor(QPalette::WindowText, UiStyle::textSecondaryColor());
        m_lblHint->setPalette(palette);
    }

    m_table = new QTableWidget(card);
    m_table->setColumnCount(kColumnCount);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setShowGrid(true);
    m_table->setWordWrap(false);
    m_table->setFrameShape(QFrame::NoFrame);
    {
        QFont font(QStringLiteral("Microsoft YaHei"));
        font.setPointSizeF(9.5);
        m_table->setFont(font);
    }

    QHeaderView *header = m_table->horizontalHeader();
    header->setSectionResizeMode(QHeaderView::Fixed);
    header->setStretchLastSection(false);
    header->setStyleSheet(UiStyle::tableHeaderStyle());
    // 扫描时间列需容纳「日期 + 时间」，按最长组合留足宽度
    m_table->setColumnWidth(0, 175);
    m_table->setColumnWidth(2, 90);
    m_table->setColumnWidth(3, 90);
    m_table->setColumnWidth(4, 120);
    // 扫描范围列自适应填满剩余宽度
    header->setSectionResizeMode(1, QHeaderView::Stretch);
    // 表头与首页结果表一致：左对齐、加粗、固定高度
    header->setFixedHeight(35);
    header->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    header->setHighlightSections(false);

    m_table->setStyleSheet(UiStyle::tableStyle());
    // 数据行与首页结果表同高，读起来不挤
    m_table->verticalHeader()->setDefaultSectionSize(30);
    m_table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);

    {
        // HideSelection = false —— 失去焦点时仍保持选中高亮
        QPalette palette = m_table->palette();
        palette.setColor(QPalette::Inactive, QPalette::Highlight,
                         palette.color(QPalette::Active, QPalette::Highlight));
        palette.setColor(QPalette::Inactive, QPalette::HighlightedText,
                         palette.color(QPalette::Active, QPalette::HighlightedText));
        m_table->setPalette(palette);
    }

    m_lblEmpty = new QLabel(card);
    m_lblEmpty->setAlignment(Qt::AlignCenter);
    {
        QFont font(QStringLiteral("Microsoft YaHei"), 11);
        m_lblEmpty->setFont(font);
        QPalette palette = m_lblEmpty->palette();
        palette.setColor(QPalette::WindowText, UiStyle::textSecondaryColor());
        m_lblEmpty->setPalette(palette);
    }

    cardLayout->addWidget(m_lblHint, 0);
    cardLayout->addWidget(m_table, 1);
    cardLayout->addWidget(m_lblEmpty, 1);
    rootLayout->addWidget(card, 1);

    connect(m_btnView, &QPushButton::clicked, this, &HistoryPanel::viewSelected);
    connect(m_btnDelete, &QPushButton::clicked, this, &HistoryPanel::deleteSelected);
    connect(m_btnClear, &QPushButton::clicked, this, &HistoryPanel::clearAll);
    connect(m_table, &QTableWidget::cellDoubleClicked, this,
            [this](int, int) { viewSelected(); });

    applyLanguage();
    reload();
}

void HistoryPanel::reload()
{
    m_settings = AppSettings::load();
    m_historyDisabled = (m_settings.historySaveMode == HistorySaveMode::None);
    m_records = ScanHistoryStore::load();
    fillTable();
}

void HistoryPanel::fillTable()
{
    m_table->clearContents();
    m_table->setRowCount(m_records.size());

    for (int row = 0; row < m_records.size(); ++row)
    {
        const ScanHistoryRecord &record = m_records.at(row);
        int total = 0;
        int online = 0;
        int dhcp = 0;
        for (const ScanHistoryDevice &device : record.devices)
        {
            ++total;
            if (device.isActive)
                ++online;
            if (device.isDhcpServer)
                ++dhcp;
        }

        m_table->setItem(
            row, 0,
            makeCell(record.scanTime.toString(m_settings.effectiveDateFormat() + QLatin1Char(' ')
                                              + m_settings.effectiveTimeFormat())));
        m_table->setItem(row, 1,
                         makeCell(Lang::fmt(QStringLiteral("HistoryRangeFormat"),
                                            record.startIp, record.endIp)));
        m_table->setItem(row, 2, makeCell(QString::number(total)));
        m_table->setItem(row, 3, makeCell(QString::number(online)));
        m_table->setItem(row, 4, makeCell(QString::number(dhcp)));
    }

    const bool empty = m_records.isEmpty();
    // 「不保存」模式下给出专用提示，与普通的「暂无扫描历史」区分开
    m_lblEmpty->setText(Lang::get(m_historyDisabled ? QStringLiteral("HistoryDisabledNotice")
                                                     : QStringLiteral("HistoryEmpty")));
    m_lblEmpty->setVisible(empty);
    m_table->setVisible(!empty);
}

void HistoryPanel::applyLanguage()
{
    m_lblTitle->setText(Lang::get(QStringLiteral("HistoryTitle")));
    m_lblHint->setText(Lang::get(QStringLiteral("HistoryHint")));
    m_btnView->setText(Lang::get(QStringLiteral("HistoryView")));
    m_btnDelete->setText(Lang::get(QStringLiteral("HistoryDelete")));
    m_btnClear->setText(Lang::get(QStringLiteral("HistoryClear")));

    m_table->setHorizontalHeaderLabels({Lang::get(QStringLiteral("ColHistoryTime")),
                                        Lang::get(QStringLiteral("ColHistoryRange")),
                                        Lang::get(QStringLiteral("ColHistoryTotal")),
                                        Lang::get(QStringLiteral("ColHistoryOnline")),
                                        Lang::get(QStringLiteral("ColHistoryDhcp"))});

    // 日期/时间格式来自设置页，先重新读取配置再渲染
    m_settings = AppSettings::load();
    m_historyDisabled = (m_settings.historySaveMode == HistorySaveMode::None);

    // 时间/范围的显示格式随语言变化，按当前语言重新渲染行
    fillTable();
}

void HistoryPanel::viewSelected()
{
    const int row = m_table->currentRow();
    if (m_table->selectedItems().isEmpty() || row < 0 || row >= m_records.size()) {
        QMessageBox::information(this, Lang::get(QStringLiteral("Tip")),
                                 Lang::get(QStringLiteral("HistorySelectFirst")));
        return;
    }
    emit recordActivated(m_records.at(row));
}

void HistoryPanel::deleteSelected()
{
    const int row = m_table->currentRow();
    if (m_table->selectedItems().isEmpty() || row < 0 || row >= m_records.size()) {
        QMessageBox::information(this, Lang::get(QStringLiteral("Tip")),
                                 Lang::get(QStringLiteral("HistorySelectFirst")));
        return;
    }
    if (QMessageBox::question(this, Lang::get(QStringLiteral("Confirm")),
                              Lang::get(QStringLiteral("HistoryConfirmDelete")),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes)
        != QMessageBox::Yes) {
        return;
    }

    ScanHistoryStore::remove(m_records.at(row));
    m_records.removeAt(row);
    fillTable();
}

void HistoryPanel::clearAll()
{
    if (m_records.isEmpty())
        return;
    if (QMessageBox::question(this, Lang::get(QStringLiteral("Confirm")),
                              Lang::get(QStringLiteral("HistoryConfirmClear")),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes)
        != QMessageBox::Yes) {
        return;
    }

    ScanHistoryStore::clear();
    m_records.clear();
    fillTable();
}
