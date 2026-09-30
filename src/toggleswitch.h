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
    /// setChecked() 以编程方式改动状态时会回调此处（状态未变化时不发 toggled），
    /// 用来把滑块直接落到与 isChecked() 一致的位置
    void checkStateSet() override;

private:
    void animateTo(bool checked);

    qreal m_position = 0.0;
    QPropertyAnimation *m_animation = nullptr;
};
