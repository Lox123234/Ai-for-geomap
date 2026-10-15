#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QUrl>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QIcon>
#include <QSettings>
#include <QQmlEngine>
#include <QtQml/qqmlextensionplugin.h>
#include "AppInfo.h"
#include "AppLog.h"
#include "CommandRegistry.h"
#include "GisEnvironment.h"
#include "Journal.h"
#include "MapService.h"
#include "MapView.h"
#include "OdmEngine.h"
#include "Platform.h"
#include "ProcessingService.h"
#include "ProjectManager.h"
#include "ProjectTreeModel.h"
#include "ResultExporter.h"
#include "Scene3DService.h"
#include "ThumbnailProvider.h"
#include "View3DItem.h"
#include "WorkspaceState.h"

// The user interface is a QML module in the static library GeoMapUi.
Q_IMPORT_QML_PLUGIN(GeoMapAIPlugin)

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName("GeoMapAI");
    QCoreApplication::setApplicationName("GeoMap AI");
    QCoreApplication::setApplicationVersion(QStringLiteral(GEOMAP_VERSION));
    QQuickStyle::setStyle("Basic");
    app.setWindowIcon(QIcon(QStringLiteral(":/geomap/geomap-256.png")));

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
    parser.addPositionalArgument(QStringLiteral("project"),
        QStringLiteral("Project folder (or its geomap.json) to open at start."), QStringLiteral("[project]"));
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
    // Copies of processing results in a folder of the user's choice.
    ResultExporter exporter(&processing);
    // 3D view: point clouds, models and terrains of the open project.
    Scene3DService scene3d;
    QObject::connect(&projects, &ProjectManager::projectDirectoryChanged, &scene3d, &Scene3DService::clear);
    qmlRegisterType<View3DItem>("GeoMapAI.View3D", 1, 0, "View3DItem");

    // Professional workspace: commands, layout, journal, project tree.
    CommandRegistry commands;
    WorkspaceState workspace;
    AppLog appLog(QDir(Platform::dataDirectory()).filePath(QStringLiteral("logs")));
    AppInfo appInfo(appLog.logDirectory());
    ProjectTreeModel projectTree(&projects, &processing, &map);
    ProjectTreeFilter projectTreeView;
    projectTreeView.setSourceModel(&projectTree);
    connectJournal(&appLog, &projects, &processing, &map);
    QObject::connect(&scene3d, &Scene3DService::errorOccurred, &appLog,
                     [&appLog](const QString &message) { appLog.add(AppLog::Error, QStringLiteral("3D"), message); });
    appLog.add(AppLog::Info, QStringLiteral("Программа"),
               QStringLiteral("GeoMap AI %1 запущена · %2").arg(QCoreApplication::applicationVersion(), map.gisVersion()));

    QQmlApplicationEngine engine;
    auto *thumbnails = new ThumbnailProvider;   // owned by the engine
    QObject::connect(&projects, &ProjectManager::projectDirectoryChanged, thumbnails,
                     [thumbnails](const QString &directory) { thumbnails->setProjectDirectory(directory); });
    engine.addImageProvider(QStringLiteral("thumbs"), thumbnails);
    engine.rootContext()->setContextProperty("projects", &projects);
    engine.rootContext()->setContextProperty("processing", &processing);
    engine.rootContext()->setContextProperty("gis", &map);
    engine.rootContext()->setContextProperty("scene3d", &scene3d);
    engine.rootContext()->setContextProperty("exporter", &exporter);
    engine.rootContext()->setContextProperty("commands", &commands);
    engine.rootContext()->setContextProperty("workspace", &workspace);
    engine.rootContext()->setContextProperty("appLog", &appLog);
    engine.rootContext()->setContextProperty("appInfo", &appInfo);
    engine.rootContext()->setContextProperty("projectTree", &projectTree);
    engine.rootContext()->setContextProperty("projectTreeView", &projectTreeView);
    engine.loadFromModule("GeoMapAI", "Main");
    if (engine.rootObjects().isEmpty())
        return -1;
    // A project given on the command line (shortcut, "Open with", script) opens with the window.
    if (!parser.positionalArguments().isEmpty())
        projects.openProject(QUrl::fromLocalFile(QFileInfo(parser.positionalArguments().constFirst()).absoluteFilePath()));
    // The window restores its saved geometry and maximizes itself when it does not fit (Main.qml).
    return app.exec();
}
