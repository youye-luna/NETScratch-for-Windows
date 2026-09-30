#include "toggleswitch.h"

#include <QPainter>
#include <QPaintEvent>
#include <QPropertyAnimation>

#include "uistyle.h"

namespace {
// 关闭态轨道灰；开启态使用全局主色蓝
const QColor kTrackOff(0xcf, 0xd6, 0xde);
constexpr int kAnimationMs = 140;
} // namespace

ToggleSwitch::ToggleSwitch(QWidget *parent)
    : QAbstractButton(parent)
    , m_animation(new QPropertyAnimation(this, "position", this))
{
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    m_animation->setDuration(kAnimationMs);
    m_animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(this, &QAbstractButton::toggled, this, [this](bool checked) { animateTo(checked); });
}

QSize ToggleSwitch::sizeHint() const
{
    return QSize(46, 26);
}

void ToggleSwitch::setPosition(qreal position)
{
    m_position = qBound<qreal>(0.0, position, 1.0);
    update();
}

void ToggleSwitch::animateTo(bool checked)
{
    const qreal end = checked ? 1.0 : 0.0;
    // 尚未显示时（如构造期按配置初始化）直接落位，不播动画
    m_animation->stop();
    if (!isVisible()) {
        setPosition(end);
        return;
    }
    m_animation->setStartValue(m_position);
    m_animation->setEndValue(end);
    m_animation->start();
}

void ToggleSwitch::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const qreal w = width();
    const qreal h = height();
    const qreal p = m_position;

    // 轨道颜色在灰/蓝之间按进度插值
    const QColor &on = UiStyle::accentColor();
    QColor track(static_cast<int>(kTrackOff.red() + (on.red() - kTrackOff.red()) * p),
                 static_cast<int>(kTrackOff.green() + (on.green() - kTrackOff.green()) * p),
                 static_cast<int>(kTrackOff.blue() + (on.blue() - kTrackOff.blue()) * p));
    painter.setPen(Qt::NoPen);
    painter.setBrush(track);
    painter.drawRoundedRect(QRectF(0, 0, w, h), h / 2.0, h / 2.0);

    // 白色圆形滑块，沿轨道从左滑到右
    const qreal margin = h * 0.08;
    const qreal diameter = h - 2 * margin;
    const qreal x = margin + p * (w - diameter - 2 * margin);
    painter.setBrush(Qt::white);
    painter.drawEllipse(QRectF(x, margin, diameter, diameter));
}
