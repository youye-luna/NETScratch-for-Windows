#include "subnetresultpanel.h"

#include "appsettings.h"
#include "ipgridpanel.h"
#include "lang.h"
#include "uistyle.h"

#include <QAbstractItemView>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialog>
#include <QGuiApplication>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPalette>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <qt_windows.h>
#include <string>

namespace
{
const int kGridWidth = 480;

/// 结果表格列数：IP / IPv6 / MAC / 主机名 / 延迟 / 设备类型
const int kColumnCount = 6;

/// 导出时取自表格的列数（设备类型列不参与导出，IPv6 列关闭时导出为横杠）
const int kExportSourceCount = 5;

/// 导出用列数：取自表格的列 + DHCP 服务器列 + 状态列（CSV 导出格式为 7 列数据，另加网段共 8 列）
const int kExportColumnCount = kExportSourceCount + 2;

/// 导出时取自表格的列号：IP / IPv6 / MAC / 主机名 / 延迟
const int kExportColumns[kExportSourceCount] = {0, 1, 2, 3, 4};

/// 结果表格中「设备类型」列的列号
const int kDeviceTypeColumn = 5;

QColor dhcpColor() { return QColor(255, 138, 128); }
QColor dhcpTextColor() { return QColor(211, 47, 47); }
QColor cameraColor() { return QColor(123, 31, 162); }
QColor onlineColor() { return QColor(33, 150, 243); }
QColor offlineColor() { return QColor(76, 175, 80); }

QColor inactiveRowColor() { return QColor(245, 245, 245); }
QColor alternateRowColor() { return QColor(245, 248, 250); }

/// 合并「DHCP服务器 / 摄像头」两列后的设备类型文本；都不是时显示占位符
QString deviceTypeText(const DhcpServerInfo &info)
{
    QStringList types;
    if (info.isDhcpServer)
        types.append(Lang::get(QStringLiteral("ColDhcp")));
    if (info.isCamera)
        types.append(Lang::get(QStringLiteral("ColCamera")));

    return types.isEmpty() ? QStringLiteral("-") : types.join(QStringLiteral(" / "));
}
} // namespace

SubnetResultPanel::SubnetResultPanel(QWidget *parent)
    : QWidget(parent)
{
    m_ipv6Visible = AppSettings::load().ipv6Enabled;

    m_grid = new QTableWidget(this);
    initTable();
    m_grid->setColumnHidden(1, !m_ipv6Visible);

    m_ipGrid = new IPGridPanel(this);
    m_ipGrid->setFixedWidth(kGridWidth);

    connect(m_grid, &QTableWidget::cellDoubleClicked, this,
            [this](int row, int) { showDetailDialog(row); });
    connect(m_ipGrid, &IPGridPanel::cellClicked, this,
            [this](int ipIndex) { jumpToRowByLastOctet(ipIndex + 1); });
    connect(m_ipGrid, &IPGridPanel::cellDoubleClicked, this, [this](int ipIndex) {
        jumpToRowByLastOctet(ipIndex + 1);
        if (m_grid->currentRow() >= 0)
            showDetailDialog(m_grid->currentRow());
    });

    QHBoxLayout *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_grid, 1);
    layout->addWidget(m_ipGrid);
}

void SubnetResultPanel::initTable()
{
    QFont gridFont = m_grid->font();
    gridFont.setPointSizeF(10.0);
    m_grid->setFont(gridFont);

    m_grid->setColumnCount(kColumnCount);
    m_grid->setHorizontalHeaderLabels(QStringList{
        Lang::get(QStringLiteral("ColIp")),
        Lang::get(QStringLiteral("ColIpv6")),
        Lang::get(QStringLiteral("ColMac")),
        Lang::get(QStringLiteral("ColHost")),
        Lang::get(QStringLiteral("ColPing")),
        Lang::get(QStringLiteral("ColDeviceType")),
    });

    m_grid->verticalHeader()->setVisible(false);
    m_grid->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_grid->setSelectionMode(QAbstractItemView::SingleSelection);
    m_grid->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_grid->setFrameShape(QFrame::NoFrame);
    m_grid->setShowGrid(true);
    m_grid->setWordWrap(false);

    QHeaderView *header = m_grid->horizontalHeader();
    header->setSectionsClickable(false);
    header->setHighlightSections(false);
    header->setFixedHeight(35);
    header->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    header->setSectionResizeMode(QHeaderView::Fixed);
    header->setStretchLastSection(false);
    QFont headerFont = gridFont;
    headerFont.setBold(true);
    header->setFont(headerFont);
    header->setStyleSheet(UiStyle::tableHeaderStyle());

    // 列宽按最长内容预留：MAC 17 字符、IPv6 25 字符（fe80::1234:5678:9abc:def0）、
    // 英文表头 Ping(ms) 粗体约 62px、设备类型 DHCP Server 粗体约 93px
    const int widths[kColumnCount] = {108, 176, 140, 120, 64, 96};
    for (int i = 0; i < kColumnCount; ++i)
        m_grid->setColumnWidth(i, widths[i]);

    // 主机名（第 4 列）吸收剩余宽度：表格左侧会被拉伸填满，
    // 若每列都是固定宽度，右侧就会拖出一条与表格同色的空白带。
    header->setSectionResizeMode(3, QHeaderView::Stretch);

    m_grid->setStyleSheet(UiStyle::tableStyle());
    // 数据行比默认略高一点，读起来不挤
    m_grid->verticalHeader()->setDefaultSectionSize(30);
    m_grid->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_grid->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);

    QPalette palette = m_grid->palette();
    palette.setColor(QPalette::Base, Qt::white);
    palette.setColor(QPalette::Highlight, QColor(200, 220, 240));
    palette.setColor(QPalette::HighlightedText, Qt::black);
    m_grid->setPalette(palette);
}

void SubnetResultPanel::setIpv6Visible(bool visible)
{
    m_ipv6Visible = visible;
    m_grid->setColumnHidden(1, !visible);
}

void SubnetResultPanel::populateData(const QVector<DhcpServerInfo> &devices)
{
    m_rowInfos = devices;
    m_grid->clearContents();
    m_grid->setRowCount(devices.size());
    m_ipGrid->resetAllColors();

    for (int row = 0; row < devices.size(); ++row)
    {
        const DhcpServerInfo &info = devices.at(row);

        // 仅显示真实测得的延迟；在线但未测得（内存 -1）时显示占位符，避免出现 "-1"
        const QString pingText =
            (info.isActive && info.pingMs >= 0) ? QString::number(info.pingMs) : QStringLiteral("-");

        const QStringList texts{
            info.ipAddress,
            info.isActive && !info.ipv6Address.isEmpty() ? info.ipv6Address : QStringLiteral("-"),
            info.isActive ? info.macAddress : QStringLiteral("-"),
            info.isActive ? info.hostName : QStringLiteral("-"),
            pingText,
            deviceTypeText(info),
        };

        for (int column = 0; column < kColumnCount; ++column)
            m_grid->setItem(row, column, new QTableWidgetItem(texts.at(column)));

        // 摄像头用识别依据做提示，便于确认判定是否可靠
        if (info.isCamera && !info.cameraEvidence.isEmpty())
        {
            if (QTableWidgetItem *item = m_grid->item(row, kDeviceTypeColumn))
                item->setToolTip(info.cameraEvidence);
        }

        applyRowStyle(row, info);

        // 更新 IP 网格颜色
        const QStringList parts = info.ipAddress.split(QLatin1Char('.'));
        if (parts.size() == 4)
        {
            bool ok = false;
            const int ipLast = parts.at(3).toInt(&ok);
            if (ok)
            {
                const int index = ipLast - 1;
                if (index >= 0 && index < 255)
                {
                    if (info.isCamera)
                        m_ipGrid->setIpColor(index, cameraColor());
                    else if (info.isDhcpServer)
                        m_ipGrid->setIpColor(index, dhcpColor());
                    else if (info.isActive)
                        m_ipGrid->setIpColor(index, onlineColor());
                    else
                        m_ipGrid->setIpColor(index, offlineColor());
                }
            }
        }
    }
}

void SubnetResultPanel::applyRowStyle(int row, const DhcpServerInfo &info)
{
    const QFont gridFont = m_grid->font();

    // 交替行底色；非活跃行整行覆盖为灰色
    const QColor rowColor = info.isActive
                                ? ((row % 2 == 1) ? alternateRowColor() : QColor(Qt::white))
                                : inactiveRowColor();
    for (int column = 0; column < kColumnCount; ++column)
    {
        if (QTableWidgetItem *item = m_grid->item(row, column))
            item->setBackground(rowColor);
    }

    // 设备类型单元格强调：摄像头优先着色（与 IP 网格、详情弹窗一致）
    if (info.isDhcpServer || info.isCamera)
    {
        if (QTableWidgetItem *item = m_grid->item(row, kDeviceTypeColumn))
        {
            item->setForeground(info.isCamera ? cameraColor() : dhcpTextColor());
            QFont font = gridFont;
            font.setBold(true);
            item->setFont(font);
        }
    }

    // IP 单元格用下划线强调特殊设备（摄像头优先于 DHCP 服务器）
    if (info.isCamera || info.isDhcpServer)
    {
        if (QTableWidgetItem *item = m_grid->item(row, 0))
        {
            item->setForeground(info.isCamera ? cameraColor() : dhcpTextColor());
            QFont font = gridFont;
            font.setUnderline(true);
            item->setFont(font);
        }
    }
}

QList<QStringList> SubnetResultPanel::getRows() const
{
    QList<QStringList> rows;
    rows.reserve(m_grid->rowCount());

    for (int row = 0; row < m_grid->rowCount(); ++row)
    {
        QStringList values;
        values.reserve(kExportColumnCount);
        // 取 IP / IPv6 / MAC / 主机名 / 延迟 5 列，设备类型列不参与导出
        for (int i = 0; i < kExportSourceCount; ++i)
        {
            const QTableWidgetItem *item = m_grid->item(row, kExportColumns[i]);
            values.append(item != nullptr ? item->text() : QString());
        }
        // IPv6 显示关闭时该列整列填横杠
        if (!m_ipv6Visible)
            values[1] = QStringLiteral("-");

        // DHCP 服务器标记按设备信息补回，保持 CSV 的 7 列格式不变
        const bool isDhcp = row < m_rowInfos.size() && m_rowInfos.at(row).isDhcpServer;
        values.append(isDhcp ? Lang::get(QStringLiteral("Yes")) : Lang::get(QStringLiteral("No")));

        // 表格中已不显示「状态」列，导出时按设备信息补回，保持 CSV 的 7 列格式不变
        const bool active = row < m_rowInfos.size() && m_rowInfos.at(row).isActive;
        values.append(active ? Lang::get(QStringLiteral("Online"))
                             : Lang::get(QStringLiteral("NoDevice")));

        rows.append(values);
    }

    return rows;
}

void SubnetResultPanel::refreshLanguage()
{
    m_grid->setHorizontalHeaderLabels(QStringList{
        Lang::get(QStringLiteral("ColIp")),
        Lang::get(QStringLiteral("ColIpv6")),
        Lang::get(QStringLiteral("ColMac")),
        Lang::get(QStringLiteral("ColHost")),
        Lang::get(QStringLiteral("ColPing")),
        Lang::get(QStringLiteral("ColDeviceType")),
    });

    // 刷新「设备类型」单元格中的文本（DHCP服务器 / 摄像头）
    for (int row = 0; row < m_grid->rowCount() && row < m_rowInfos.size(); ++row)
    {
        if (QTableWidgetItem *item = m_grid->item(row, kDeviceTypeColumn))
            item->setText(deviceTypeText(m_rowInfos.at(row)));
    }

    m_ipGrid->update();
}

void SubnetResultPanel::jumpToRowByLastOctet(int ipLast)
{
    const QString suffix = QStringLiteral(".") + QString::number(ipLast);

    for (int row = 0; row < m_grid->rowCount(); ++row)
    {
        const QTableWidgetItem *item = m_grid->item(row, 0);
        const QString ip = item != nullptr ? item->text() : QString();
        if (!ip.endsWith(suffix))
            continue;

        m_grid->clearSelection();
        m_grid->setCurrentCell(row, 0);
        m_grid->selectRow(row);
        m_grid->scrollToItem(m_grid->item(row, 0), QAbstractItemView::PositionAtTop);
        return;
    }
}

void SubnetResultPanel::showDetailPreview()
{
    // --preview 预览用：填入几台不同状态的示例设备，并直接弹出第一台的详情窗，
    // 便于不扫描就能检查详情窗的样式
    DhcpServerInfo router;
    router.ipAddress = QStringLiteral("192.168.31.1");
    router.macAddress = QStringLiteral("3C:84:6A:11:22:33");
    router.hostName = QStringLiteral("router.lan");
    router.isActive = true;
    router.isDhcpServer = true;
    router.ipv6Address = QStringLiteral("fe80::3e84:6aff:fe11:2233");
    router.pingMs = 3;

    DhcpServerInfo camera;
    camera.ipAddress = QStringLiteral("192.168.31.64");
    camera.macAddress = QStringLiteral("C4:2F:90:AA:BB:CC");
    camera.hostName = QStringLiteral("IPC-64");
    camera.isActive = true;
    camera.isCamera = true;
    camera.pingMs = 12;

    DhcpServerInfo offline;
    offline.ipAddress = QStringLiteral("192.168.31.100");

    populateData(QVector<DhcpServerInfo>{router, camera, offline});
    showDetailDialog(0);
}

void SubnetResultPanel::showDetailDialog(int row)
{
    if (row < 0 || row >= m_grid->rowCount())
        return;

    const QTableWidgetItem *ipItem = m_grid->item(row, 0);
    const QTableWidgetItem *ipv6Item = m_grid->item(row, 1);
    const QTableWidgetItem *macItem = m_grid->item(row, 2);
    const QTableWidgetItem *hostItem = m_grid->item(row, 3);
    const QString ip = ipItem != nullptr ? ipItem->text() : QString();
    const QString mac = macItem != nullptr ? macItem->text() : QString();
    const QString host = hostItem != nullptr ? hostItem->text() : QString();
    const QString ipv6 = ipv6Item != nullptr ? ipv6Item->text() : QString();

    bool isActive = false;
    bool isDhcp = false;
    bool isCamera = false;
    qint64 pingMs = -1;
    if (row < m_rowInfos.size())
    {
        isActive = m_rowInfos.at(row).isActive;
        isDhcp = m_rowInfos.at(row).isDhcpServer;
        isCamera = m_rowInfos.at(row).isCamera;
        pingMs = m_rowInfos.at(row).pingMs;
    }

    // 与表格「设备类型」列保持一致：DHCP 服务器 / 摄像头 合并为一项
    QStringList deviceTypes;
    if (isDhcp)
        deviceTypes.append(Lang::get(QStringLiteral("ColDhcp")));
    if (isCamera)
        deviceTypes.append(Lang::get(QStringLiteral("ColCamera")));
    const QString deviceType =
        deviceTypes.isEmpty() ? QStringLiteral("-") : deviceTypes.join(QStringLiteral(" / "));

    const QColor statusColor = isCamera ? cameraColor()
                              : (isDhcp ? dhcpColor()
                                        : (isActive ? onlineColor() : QColor(158, 158, 158)));
    const QString pingText =
        (isActive && pingMs >= 0) ? QStringLiteral("%1 ms").arg(pingMs) : QStringLiteral("-");

    struct Field
    {
        QString label;
        QString value;
        QColor color;
    };
    QList<Field> fields;
    fields.append({Lang::get(QStringLiteral("FieldIp")), ip, UiStyle::textPrimaryColor()});
    fields.append({Lang::get(QStringLiteral("FieldMac")), mac, UiStyle::textPrimaryColor()});
    fields.append({Lang::get(QStringLiteral("FieldHost")), host, UiStyle::textPrimaryColor()});
    if (m_ipv6Visible)
        fields.append({Lang::get(QStringLiteral("ColIpv6")), ipv6, UiStyle::textPrimaryColor()});
    fields.append({Lang::get(QStringLiteral("FieldPing")),
                   pingText,
                   pingText != QStringLiteral("-") ? QColor(46, 125, 50)
                                                   : UiStyle::textSecondaryColor()});
    fields.append({Lang::get(QStringLiteral("ColDeviceType")),
                   deviceType,
                   isCamera ? cameraColor()
                            : (isDhcp ? dhcpTextColor() : UiStyle::textPrimaryColor())});

    // 非模态显示：详情窗口不阻塞主窗口，可边看详情边继续操作
    QDialog *dialog = new QDialog(window());
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setModal(false);
    dialog->setWindowModality(Qt::NonModal);
    dialog->setWindowTitle(Lang::fmt(QStringLiteral("DeviceDetail"), ip));
    // 去掉标题栏上的「?」帮助按钮，标题栏更干净
    dialog->setWindowFlags(dialog->windowFlags() & ~Qt::WindowContextHelpButtonHint);
    dialog->setStyleSheet(QStringLiteral("QDialog { background-color: #ffffff; }"));
    dialog->setFixedWidth(480);

    QVBoxLayout *rootLayout = new QVBoxLayout(dialog);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    // 顶部状态色条：颜色随设备状态变化
    QWidget *colorBar = new QWidget(dialog);
    colorBar->setFixedHeight(5);
    colorBar->setAutoFillBackground(true);
    QPalette barPalette = colorBar->palette();
    barPalette.setColor(QPalette::Window, statusColor);
    colorBar->setPalette(barPalette);
    rootLayout->addWidget(colorBar);

    // 头部：IP 主标题 + 状态徽章；底色取状态色的极浅色调
    const QColor headerTint(qRound(statusColor.red() * 0.10 + 229.5),
                            qRound(statusColor.green() * 0.10 + 229.5),
                            qRound(statusColor.blue() * 0.10 + 229.5));
    QWidget *header = new QWidget(dialog);
    header->setAutoFillBackground(true);
    QPalette headerPalette = header->palette();
    headerPalette.setColor(QPalette::Window, headerTint);
    header->setPalette(headerPalette);

    QVBoxLayout *headerLayout = new QVBoxLayout(header);
    headerLayout->setContentsMargins(24, 18, 24, 16);
    headerLayout->setSpacing(10);

    QLabel *lblTitle = new QLabel(ip, header);
    QFont titleFont = dialog->font();
    titleFont.setPointSizeF(17.0);
    titleFont.setBold(true);
    lblTitle->setFont(titleFont);
    lblTitle->setStyleSheet(QStringLiteral("color: #1f2329; background-color: transparent;"));
    headerLayout->addWidget(lblTitle);

    struct Tag
    {
        QString text;
        QColor color;
    };
    QList<Tag> tags;
    if (isActive)
        tags.append(Tag{Lang::get(QStringLiteral("Online")), onlineColor()});
    if (isCamera)
        tags.append(Tag{Lang::get(QStringLiteral("ColCamera")), cameraColor()});
    if (isDhcp)
        tags.append(Tag{Lang::get(QStringLiteral("ColDhcp")), dhcpColor()});
    if (tags.isEmpty())
        tags.append(Tag{Lang::get(QStringLiteral("NoDevice")), QColor(158, 158, 158)});

    QHBoxLayout *tagLayout = new QHBoxLayout;
    tagLayout->setContentsMargins(0, 0, 0, 0);
    tagLayout->setSpacing(6);
    for (const Tag &tag : tags)
    {
        QLabel *pill = new QLabel(tag.text, header);
        pill->setFixedHeight(22);
        pill->setAlignment(Qt::AlignCenter);
        QFont pillFont = dialog->font();
        pillFont.setPointSizeF(8.5);
        pillFont.setBold(true);
        pill->setFont(pillFont);
        pill->setStyleSheet(QStringLiteral("color: #ffffff; background-color: %1;"
                                          " border-radius: 11px; padding: 0 12px;")
                                .arg(tag.color.name()));
        tagLayout->addWidget(pill);
    }
    tagLayout->addStretch(1);
    headerLayout->addLayout(tagLayout);
    rootLayout->addWidget(header);

    // 信息卡片：白底、圆角描边，行间用浅色分隔线
    QWidget *body = new QWidget(dialog);
    QVBoxLayout *bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(20, 18, 20, 0);
    bodyLayout->setSpacing(0);

    QWidget *infoCard = new QWidget(body);
    infoCard->setObjectName(QStringLiteral("detailCard"));
    infoCard->setAttribute(Qt::WA_StyledBackground, true);
    infoCard->setStyleSheet(QStringLiteral("QWidget#detailCard { background-color: #ffffff;"
                                           " border: 1px solid #e6e8ec; border-radius: 8px; }"));

    QVBoxLayout *rowsLayout = new QVBoxLayout(infoCard);
    rowsLayout->setContentsMargins(0, 6, 0, 6);
    rowsLayout->setSpacing(0);

    QFont labelFont = dialog->font();
    labelFont.setPointSizeF(9.5);
    QFont valueFont = labelFont;
    valueFont.setBold(true);

    for (int i = 0; i < fields.size(); ++i)
    {
        const Field &field = fields.at(i);

        QWidget *rowWidget = new QWidget(infoCard);
        rowWidget->setFixedHeight(36);
        QHBoxLayout *rowLayout = new QHBoxLayout(rowWidget);
        rowLayout->setContentsMargins(16, 0, 16, 0);
        rowLayout->setSpacing(10);

        QLabel *label = new QLabel(field.label, rowWidget);
        label->setFixedWidth(96);
        label->setFont(labelFont);
        label->setStyleSheet(QStringLiteral("color: #8a9099; background-color: transparent;"));
        rowLayout->addWidget(label);

        // 用可选中的 QLabel 展示取值：文字更干净，也能按住选中复制单个字段
        QLabel *value = new QLabel(field.value, rowWidget);
        value->setFont(valueFont);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        value->setStyleSheet(QStringLiteral("color: %1; background-color: transparent;")
                                 .arg(field.color.name()));
        rowLayout->addWidget(value, 1);

        rowsLayout->addWidget(rowWidget);

        if (i != fields.size() - 1)
        {
            QWidget *lineHolder = new QWidget(infoCard);
            lineHolder->setFixedHeight(1);
            QHBoxLayout *lineLayout = new QHBoxLayout(lineHolder);
            lineLayout->setContentsMargins(16, 0, 16, 0);
            lineLayout->setSpacing(0);

            QWidget *line = new QWidget(lineHolder);
            line->setAutoFillBackground(true);
            QPalette linePalette = line->palette();
            linePalette.setColor(QPalette::Window, UiStyle::borderColor());
            line->setPalette(linePalette);
            lineLayout->addWidget(line);

            rowsLayout->addWidget(lineHolder);
        }
    }

    bodyLayout->addWidget(infoCard);
    rootLayout->addWidget(body);

    const AppLanguage language = Lang::current();
    QString copyDetailsText;
    QString copiedText;
    switch (language)
    {
    case AppLanguage::English:
        copyDetailsText = QStringLiteral("Copy details");
        copiedText = QStringLiteral("Copied");
        break;
    case AppLanguage::TraditionalChinese:
        copyDetailsText = QStringLiteral("複製資訊");
        copiedText = QStringLiteral("已複製");
        break;
    case AppLanguage::TraditionalChineseHk:
        copyDetailsText = QStringLiteral("複製資料");
        copiedText = QStringLiteral("已複製");
        break;
    default:
        copyDetailsText = QStringLiteral("复制详情");
        copiedText = QStringLiteral("已复制");
        break;
    }

    const int btnW = 130;
    const int btnH = 36;

    QFont buttonFont = dialog->font();
    buttonFont.setPointSizeF(9.5);
    QFont boldButtonFont = buttonFont;
    boldButtonFont.setBold(true);

    // 次要操作：白底描边，统一取全局按钮样式
    QPushButton *btnCopy = new QPushButton(copyDetailsText, dialog);
    btnCopy->setFixedSize(btnW, btnH);
    btnCopy->setCursor(Qt::PointingHandCursor);
    btnCopy->setFont(buttonFont);
    btnCopy->setStyleSheet(UiStyle::secondaryButtonStyle());

    QPushButton *btnPing = new QPushButton(QStringLiteral("Ping"), dialog);
    btnPing->setFixedSize(btnW, btnH);
    btnPing->setCursor(Qt::PointingHandCursor);
    btnPing->setFont(boldButtonFont);
    btnPing->setStyleSheet(UiStyle::secondaryButtonStyle());

    QPushButton *btnWeb = new QPushButton(Lang::get(QStringLiteral("AccessAdmin")), dialog);
    btnWeb->setFixedSize(btnW, btnH);
    btnWeb->setCursor(Qt::PointingHandCursor);
    btnWeb->setFont(boldButtonFont);
    btnWeb->setStyleSheet(UiStyle::secondaryButtonStyle());

    QPushButton *btnIe = new QPushButton(Lang::get(QStringLiteral("IeAccess")), dialog);
    btnIe->setFixedSize(btnW, btnH);
    btnIe->setCursor(Qt::PointingHandCursor);
    btnIe->setFont(boldButtonFont);
    btnIe->setStyleSheet(UiStyle::secondaryButtonStyle());

    QPushButton *btnClose = new QPushButton(Lang::get(QStringLiteral("Close")), dialog);
    btnClose->setFixedSize(btnW, btnH);
    btnClose->setCursor(Qt::PointingHandCursor);
    btnClose->setFont(boldButtonFont);
    btnClose->setStyleSheet(UiStyle::secondaryButtonStyle());

    connect(btnCopy, &QPushButton::clicked, dialog,
            [fields, btnCopy, copyDetailsText, copiedText]() {
                QStringList lines;
                for (const Field &field : fields)
                    lines.append(QStringLiteral("%1: %2").arg(field.label, field.value));

                if (QClipboard *clipboard = QGuiApplication::clipboard())
                    clipboard->setText(lines.join(QStringLiteral("\r\n")));

                btnCopy->setText(copiedText);
                QTimer::singleShot(1400, btnCopy, [btnCopy, copyDetailsText]() {
                    btnCopy->setText(copyDetailsText);
                });
            });

    connect(btnPing, &QPushButton::clicked, dialog, [ip]() {
        // 本程序是 GUI 程序、自身没有控制台。Qt 启动子进程时会检测到这一点并给
        // 子进程附加 CREATE_NO_WINDOW，导致 cmd 控制台窗口被“隐藏创建”（进程在跑，
        // 却看不到窗口）。这里绕开 Qt 的标志处理，直接调用 Win32 API 并显式指定
        // CREATE_NEW_CONSOLE，确保 ping 窗口正常弹出。
        std::wstring command = QStringLiteral("cmd.exe /k ping %1 -t").arg(ip).toStdWString();
        command.push_back(L'\0'); // CreateProcessW 需要可写、以 null 结尾的命令行缓冲

        STARTUPINFOW startupInfo;
        ZeroMemory(&startupInfo, sizeof(startupInfo));
        startupInfo.cb = sizeof(startupInfo);
        PROCESS_INFORMATION processInfo;
        ZeroMemory(&processInfo, sizeof(processInfo));

        if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE,
                           nullptr, nullptr, &startupInfo, &processInfo))
        {
            CloseHandle(processInfo.hThread);
            CloseHandle(processInfo.hProcess);
        }
    });

    connect(btnWeb, &QPushButton::clicked, dialog,
            [ip]() { QDesktopServices::openUrl(QUrl(QStringLiteral("http://") + ip)); });

    connect(btnIe, &QPushButton::clicked, dialog, [dialog, ip]() {
        QSettings ieReg(QStringLiteral("HKEY_CLASSES_ROOT\\InternetExplorer.Application"),
                        QSettings::NativeFormat);
        if (ieReg.allKeys().isEmpty() && ieReg.childGroups().isEmpty())
        {
            QMessageBox::information(dialog, Lang::get(QStringLiteral("Tip")),
                                     Lang::get(QStringLiteral("IeNotRegistered")));
            return;
        }

        if (!QProcess::startDetached(QStringLiteral("iexplore.exe"),
                                     QStringList{QStringLiteral("http://") + ip}))
        {
            QMessageBox::information(
                dialog, Lang::get(QStringLiteral("Tip")),
                Lang::fmt(QStringLiteral("IeLaunchFailed"), QStringLiteral("iexplore.exe")));
        }
    });

    connect(btnClose, &QPushButton::clicked, dialog, &QDialog::reject);

    const QList<QPushButton *> allButtons{btnCopy, btnPing, btnWeb, btnIe, btnClose};
    QList<QPushButton *> visibleButtons;
    for (QPushButton *button : allButtons)
    {
        const bool visible = (button == btnWeb || button == btnIe) ? (isDhcp || isCamera) : true;
        button->setVisible(visible);
        if (visible)
            visibleButtons.append(button);
    }

    // 底部按钮区：每行最多 3 个，行内居中，宽度保持一致
    QWidget *footer = new QWidget(dialog);
    QVBoxLayout *footerLayout = new QVBoxLayout(footer);
    footerLayout->setContentsMargins(20, 16, 20, 20);
    footerLayout->setSpacing(10);

    const int columns = 3;
    for (int i = 0; i < visibleButtons.size(); i += columns)
    {
        QHBoxLayout *rowLayout = new QHBoxLayout;
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->setSpacing(10);
        rowLayout->addStretch(1);

        const int rowCount = qMin(columns, visibleButtons.size() - i);
        for (int c = 0; c < rowCount; ++c)
            rowLayout->addWidget(visibleButtons.at(i + c));

        rowLayout->addStretch(1);
        footerLayout->addLayout(rowLayout);
    }
    rootLayout->addWidget(footer);

    // 以主窗口为参照居中，避免新窗口出现在屏幕角落
    dialog->adjustSize();
    if (QWidget *owner = window())
    {
        const QPoint ownerCenter = owner->frameGeometry().center();
        dialog->move(ownerCenter.x() - dialog->width() / 2, ownerCenter.y() - dialog->height() / 2);
    }

    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}
