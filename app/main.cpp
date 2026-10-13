#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QIcon>
#include <QScreen>
#include <QWindow>
#include "ProjectManager.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName("GeoMapAI");
    QCoreApplication::setApplicationName("GeoMap AI");
    QCoreApplication::setApplicationVersion("0.1.0");
    QQuickStyle::setStyle("Basic");

    ProjectManager projects;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("projects", &projects);
    engine.loadFromModule("GeoMapAI", "Main");
    if (engine.rootObjects().isEmpty())
        return -1;
    // 1500x900 does not fit e.g. 1920x1080 at 125 % scaling; maximize instead of
    // hiding the status bar and inspector buttons behind the taskbar.
    if (auto *window = qobject_cast<QWindow *>(engine.rootObjects().constFirst())) {
        const QRect available = window->screen()->availableGeometry();
        if (window->frameGeometry().width() > available.width()
            || window->frameGeometry().height() > available.height())
            window->showMaximized();
    }
    return app.exec();
}
