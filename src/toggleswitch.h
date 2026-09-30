#pragma once

#include <QAbstractButton>

class QPropertyAnimation;

/// iOS 风格滑动开关：胶囊轨道 + 白色圆形滑块，
/// 开（主色蓝）/ 关（浅灰）状态间平滑过渡。
class ToggleSwitch : public QAbstractButton
{
    Q_OBJECT
    Q_PROPERTY(qreal position READ position WRITE setPosition)

public:
    explicit ToggleSwitch(QWidget *parent = nullptr);

    QSize sizeHint() const override;

    /// 滑块进度：0=关 1=开，随动画变化
    qreal position() const { return m_position; }
    void setPosition(qreal position);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void animateTo(bool checked);

    qreal m_position = 1.0;
    QPropertyAnimation *m_animation = nullptr;
};
