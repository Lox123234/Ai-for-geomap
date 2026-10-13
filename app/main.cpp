#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QIcon>
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
    return app.exec();
}
