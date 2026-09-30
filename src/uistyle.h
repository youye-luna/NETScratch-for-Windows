#pragma once

// 全局视觉规范：配色、字体、按钮/卡片/表格样式。
// 主窗口各页面（主页 / 历史 / 设置）统一引用，避免出现新旧两套界面风格。

#include <QAbstractItemView>
#include <QColor>
#include <QComboBox>
#include <QFont>
#include <QModelIndex>
#include <QSize>
#include <QString>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QWidget>

namespace UiStyle
{
// ------------------------------------------------------------------ 配色
inline QColor accentColor() { return QColor(33, 150, 243); }          // 主色
inline QColor textPrimaryColor() { return QColor(31, 35, 41); }       // 主要文字
inline QColor textSecondaryColor() { return QColor(138, 144, 153); }  // 次要文字
inline QColor borderColor() { return QColor(230, 232, 236); }         // 描边
inline QColor pageBackgroundColor() { return QColor(246, 248, 250); } // 页面底色

// ------------------------------------------------------------------ 字体
inline QFont labelFont()
{
    return QFont(QStringLiteral("Microsoft YaHei"), 9);
}

/// 页面标题（如「设置」「扫描历史」）
inline QFont pageTitleFont()
{
    QFont font(QStringLiteral("Microsoft YaHei"), 11);
    font.setBold(true);
    return font;
}

/// 卡片内的小标题（如「搜索范围设置」）
inline QFont sectionTitleFont()
{
    QFont font(QStringLiteral("Microsoft YaHei"), 10);
    font.setBold(true);
    return font;
}

// ------------------------------------------------------------------ 尺寸
const int kButtonWidth = 100;
const int kButtonHeight = 30;

// ------------------------------------------------------------------ 样式表
/// 主色实底按钮（主要操作）
inline QString primaryButtonStyle()
{
    return QStringLiteral(
        "QPushButton { background-color: #2196f3; color: #ffffff; border: none;"
        " border-radius: 6px; }"
        "QPushButton:hover { background-color: #1e88e5; }"
        "QPushButton:pressed { background-color: #1976d2; }"
        "QPushButton:disabled { background-color: #b9d9f7; color: #f0f6fd; }");
}

/// 白底描边按钮（次要操作）
inline QString secondaryButtonStyle()
{
    return QStringLiteral(
        "QPushButton { background-color: #ffffff; border: 1px solid #d5d9de;"
        " border-radius: 6px; color: #2b2f36; }"
        "QPushButton:hover { background-color: #f2f4f7; border-color: #c3c9d1; }"
        "QPushButton:pressed { background-color: #e8ebef; }"
        "QPushButton:disabled { background-color: #f5f6f8; border-color: #e3e6ea;"
        " color: #b0b6bd; }");
}

/// 下拉框（含展开列表）：与次要按钮同款描边圆角，避免出现系统原生外观
inline QString comboBoxStyle()
{
    return QStringLiteral(
        "QComboBox { background-color: #ffffff; border: 1px solid #d5d9de;"
        " border-radius: 6px; padding: 2px 8px; color: #1f2329; }"
        "QComboBox:hover { border-color: #c3c9d1; }"
        "QComboBox:focus { border-color: #2196f3; }"
        "QComboBox::drop-down { border: none; width: 24px; }"
        "QComboBox::down-arrow { image: url(:/arrow-down.png);"
        " width: 12px; height: 7px; }"
        "QComboBox QAbstractItemView { background-color: #ffffff;"
        " border: 1px solid #e6e8ec; border-radius: 6px; padding: 4px; outline: none;"
        " color: #1f2329; selection-background-color: #e8f2fd;"
        " selection-color: #1e88e5; }");
}

/// 数字输入框：白底描边 + 扁平上下箭头
inline QString spinBoxStyle()
{
    return QStringLiteral(
        "QSpinBox { background-color: #ffffff; border: 1px solid #d5d9de;"
        " border-radius: 6px; padding: 2px 4px; color: #1f2329; }"
        "QSpinBox:hover { border-color: #c3c9d1; }"
        "QSpinBox:focus { border-color: #2196f3; }"
        "QSpinBox:disabled { background-color: #f5f6f8; border-color: #e3e6ea;"
        " color: #b0b6bd; }"
        "QSpinBox::up-button, QSpinBox::down-button { width: 16px;"
        " background-color: #f7f8fa; border-left: 1px solid #e6e8ec; }"
        "QSpinBox::up-button { subcontrol-origin: border;"
        " subcontrol-position: top right; border-top-right-radius: 6px; }"
        "QSpinBox::down-button { subcontrol-origin: border;"
        " subcontrol-position: bottom right; border-bottom-right-radius: 6px; }"
        "QSpinBox::up-button:hover, QSpinBox::down-button:hover {"
        " background-color: #eef1f5; }"
        "QSpinBox::up-arrow { image: url(:/arrow-up.png);"
        " width: 10px; height: 6px; }"
        "QSpinBox::down-arrow { image: url(:/arrow-down.png);"
        " width: 10px; height: 6px; }");
}

/// 顶部导航按钮（透明底 + 选中态高亮）
inline QString navButtonStyle()
{
    return QStringLiteral(
        "QPushButton { background-color: transparent; border: none; border-radius: 8px;"
        " padding: 9px 26px; font-size: 11pt; color: #5b6470; }"
        "QPushButton:hover { background-color: #f0f2f5; }"
        "QPushButton:checked { background-color: #e8f2fd; color: #1e88e5;"
        " font-weight: bold; }");
}

/// 结果表格表头（统一使用主色）
inline QString tableHeaderStyle()
{
    return QStringLiteral(
        "QHeaderView::section {"
        "  background-color: #2196f3;"
        "  color: white;"
        "  font-weight: bold;"
        "  border: none;"
        "  padding-left: 4px;"
        "}");
}

/// 表格通用样式：描边 + 浅灰网格线。
/// 故意不给 ::item 设 background-color —— 否则会盖掉
/// item->setBackground() 设置的交替行底色与非活跃行灰底。
/// 同理也不能给 ::item 设 padding：只有被样式表绘制的单元格（即选中行）
/// 才会走样式表、吃到 padding，未选中行会退回默认样式的边距，
/// 结果选中时整行文字右移 8px 甚至被省略号截断。
inline QString tableStyle()
{
    return QStringLiteral(
        "QTableWidget { background-color: #ffffff; border: 1px solid #e6e8ec;"
        " gridline-color: #eef1f5; outline: none; }"
        "QTableWidget::item:selected { background-color: #c8dcf0; color: #1f2329; }");
}

/// 白底圆角卡片
inline QString cardStyle()
{
    return QStringLiteral("QWidget#uiCard { background-color: #ffffff;"
                          " border: 1px solid #e6e8ec; border-radius: 8px; }");
}

/// 扫描进度条
inline QString progressBarStyle()
{
    return QStringLiteral(
        "QProgressBar { border: 1px solid #e6e8ec; border-radius: 6px;"
        " background-color: #f0f2f5; color: #1f2329; text-align: center; }"
        "QProgressBar::chunk { background-color: #2196f3; border-radius: 5px; }");
}

/// 系统消息框（QMessageBox 为原生控件，需在全局样式里统一，否则会残留旧外观）
inline QString messageBoxStyle()
{
    return QStringLiteral(
        "QMessageBox { background-color: #ffffff; }"
        "QMessageBox QLabel { color: #1f2329; }"
        "QMessageBox QPushButton { min-width: 72px; min-height: 22px;"
        " background-color: #ffffff; border: 1px solid #d5d9de; border-radius: 6px;"
        " padding: 2px 12px; color: #2b2f36; }"
        "QMessageBox QPushButton:hover { background-color: #f2f4f7;"
        " border-color: #c3c9d1; }"
        "QMessageBox QPushButton:pressed { background-color: #e8ebef; }"
        "QMessageBox QPushButton:default { background-color: #2196f3;"
        " border: none; color: #ffffff; }"
        "QMessageBox QPushButton:default:hover { background-color: #1e88e5; }"
        "QMessageBox QPushButton:default:pressed { background-color: #1976d2; }");
}

// ------------------------------------------------------------------ 辅助
/// 创建白色圆角卡片容器（调用方自行为其设置布局）
inline QWidget *makeCard(QWidget *parent = nullptr)
{
    QWidget *card = new QWidget(parent);
    card->setObjectName(QStringLiteral("uiCard"));
    card->setAttribute(Qt::WA_StyledBackground, true);
    card->setStyleSheet(cardStyle());
    return card;
}

/// 给页面容器套上统一的浅灰底色
inline void applyPageBackground(QWidget *page)
{
    page->setObjectName(QStringLiteral("uiPage"));
    page->setAttribute(Qt::WA_StyledBackground, true);
    page->setStyleSheet(QStringLiteral("QWidget#uiPage { background-color: #f6f8fa; }"));
}

/// 下拉列表项代理：在默认行高之外再撑高一些，让展开后的选项不至于挤在一起
class ComboItemDelegate : public QStyledItemDelegate
{
public:
    explicit ComboItemDelegate(int extraRowHeight, QObject *parent = nullptr)
        : QStyledItemDelegate(parent), m_extraRowHeight(extraRowHeight)
    {
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(size.height() + m_extraRowHeight);
        return size;
    }

private:
    int m_extraRowHeight = 0;
};

/// 下拉列表的弹出窗口默认是不透明的系统窗口，圆角会被方形窗口裁掉。
/// 去掉边框并开透明背景后，样式表里的 border-radius 才会真正生效。
/// 同时把选项行高撑开约 1mm，避免展开列表过于拥挤。
inline void enableRoundedPopup(QComboBox *combo)
{
    if (!combo)
        return;

    QWidget *popup = combo->view()->window();
    popup->setWindowFlags(Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    popup->setAttribute(Qt::WA_TranslucentBackground);

    // 1mm ≈ 3.78px（96 DPI），取整为 4px
    combo->view()->setItemDelegate(new ComboItemDelegate(4, combo->view()));
}

} // namespace UiStyle
