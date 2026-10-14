#include <QCommandLineParser>
#include <QDir>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QIcon>
#include <QScreen>
#include <QSettings>
#include <QWindow>
#include <QQmlEngine>
#include "GisEnvironment.h"
#include "MapService.h"
#include "MapView.h"
#include "OdmEngine.h"
#include "Platform.h"
#include "ProcessingService.h"
#include "ProjectManager.h"
#include "ThumbnailProvider.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName("GeoMapAI");
    QCoreApplication::setApplicationName("GeoMap AI");
    QCoreApplication::setApplicationVersion(QStringLiteral(GEOMAP_VERSION));
    QQuickStyle::setStyle("Basic");

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("GeoMap AI — UAV photo projects"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption settingsDir(QStringLiteral("settings-dir"),
        QStringLiteral("Store settings in <dir>/GeoMap AI.ini instead of the user profile."),
        QStringLiteral("dir"));
    parser.addOption(settingsDir);
    const QCommandLineOption dataDir(QStringLiteral("data-dir"),
        QStringLiteral("Keep engines, downloads and work links in <dir> instead of %LOCALAPPDATA%\\GeoMapAI."),
        QStringLiteral("dir"));
    parser.addOption(dataDir);
    parser.process(app);
    if (parser.isSet(dataDir)) Platform::setDataDirectory(parser.value(dataDir));
    if (parser.isSet(settingsDir)) {
        // Must precede the first QSettings use (ProjectManager reads recent projects).
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           QDir(parser.value(settingsDir)).absolutePath());
    }

    ProjectManager projects;
    // Declared after projects and before the QML engine: on exit a running job is stopped and
    // recorded while its project is still open, after the UI is gone.
    ProcessingService processing(new OdmEngine);
    QObject::connect(&projects, &ProjectManager::projectDirectoryChanged, &processing,
                     [&processing, &projects](const QString &directory) {
                         processing.setProject(directory, projects.photoModel());
                     });
    // GDAL/OGR and PROJ; data folders next to the executable in deployed builds.
    GisEnvironment::initialize(QCoreApplication::applicationDirPath());
    MapService map;
    // After the photo model was reset (projectChanged), unlike projectDirectoryChanged.
    QObject::connect(&projects, &ProjectManager::projectChanged, &map, [&map, &projects] {
        if (projects.projectPath() != map.projectDirectory()) map.setProject(projects.projectPath(), projects.photoModel());
    });
    QObject::connect(&projects, &ProjectManager::photosChanged, &map, &MapService::photosChanged);
    QObject::connect(&projects, &ProjectManager::selectionChanged, &map,
                     [&map, &projects] { map.setSelectedPhoto(projects.selectedIndex()); });
    qmlRegisterType<MapView>("GeoMapAI.Map", 1, 0, "MapView");

    QQmlApplicationEngine engine;
    auto *thumbnails = new ThumbnailProvider;   // owned by the engine
    QObject::connect(&projects, &ProjectManager::projectDirectoryChanged, thumbnails,
                     [thumbnails](const QString &directory) { thumbnails->setProjectDirectory(directory); });
    engine.addImageProvider(QStringLiteral("thumbs"), thumbnails);
    engine.rootContext()->setContextProperty("projects", &projects);
    engine.rootContext()->setContextProperty("processing", &processing);
    engine.rootContext()->setContextProperty("gis", &map);
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
