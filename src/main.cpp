#include "mainwindow.h"
#include "scanner.h"
#include "uistyle.h"

#include <QApplication>
#include <QFont>
#include <QIcon>
#include <QMetaType>

int main(int argc, char *argv[])
{
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("NETScratch"));
    QApplication::setApplicationVersion(QStringLiteral("2.0-beta"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/app.ico")));
    app.setFont(QFont(QStringLiteral("Microsoft YaHei"), 9));

    // 统一系统消息框外观：QMessageBox 是原生控件，不受各页面样式影响
    app.setStyleSheet(UiStyle::messageBoxStyle());

    // 跨线程信号需要注册自定义类型
    qRegisterMetaType<DhcpServerInfo>("DhcpServerInfo");
    qRegisterMetaType<QVector<DhcpServerInfo>>("QVector<DhcpServerInfo>");

    MainWindow window;
    window.show();

    // --preview：不扫描，直接用示例数据弹出设备详情窗，便于检查界面样式
    if (app.arguments().contains(QStringLiteral("--preview")))
        window.showDeviceDetailPreview();

    return app.exec();
}
