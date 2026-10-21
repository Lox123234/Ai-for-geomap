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
#include "AiService.h"
#include "AppInfo.h"
#include "AppLog.h"
#include "ChunkService.h"
#include "CommandRegistry.h"
#include "GeorefImageView.h"
#include "GeorefService.h"
#include "GisEnvironment.h"
#include "Journal.h"
#include "LidarService.h"
#include "MapService.h"
#include "MapView.h"
#include "OdmEngine.h"
#include "PhotoMasks.h"
#include "PhotoQualityService.h"
#include "SurveyControl.h"
#include "Platform.h"
#include "ProcessingService.h"
#include "ProjectManager.h"
#include "ProjectTreeModel.h"
#include "ReportService.h"
#include "ResultExporter.h"
#include "Scene3DService.h"
#include "ThumbnailProvider.h"
#include "View3DItem.h"
#include "SurveyView.h"
#include "TextureEditService.h"
#include "WebTiles.h"
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
    // Chunks: sets of the project's photos processed by their own jobs (chunks.json).
    ChunkService chunks(&projects, &processing);
    // Sharpness of the photos (quality.json); blurred ones switched off in the active chunk.
    PhotoQualityService photoQuality(&projects, &chunks);
    // GDAL/OGR and PROJ; data folders next to the executable in deployed builds.
    GisEnvironment::initialize(QCoreApplication::applicationDirPath());
    // Tiles of online basemaps, shared by all projects.
    webtiles::TileStore::instance()->setCacheDirectory(QDir(Platform::dataDirectory()).filePath(QStringLiteral("cache/tiles")));
    // Local AI models (cards in <app>/models and <data folder>/models); before the map that uses them.
    ai::AiService aiModels;
    MapService map;
    map.setAi(&aiModels);
    // Masks of the photos for the reconstruction (AI selection on the photo).
    PhotoMasks photoMasks(&aiModels);
    QObject::connect(&projects, &ProjectManager::projectDirectoryChanged, &photoMasks, &PhotoMasks::setProject);
    // After the photo model was reset (projectChanged), unlike projectDirectoryChanged.
    QObject::connect(&projects, &ProjectManager::projectChanged, &map, [&map, &projects] {
        if (projects.projectPath() != map.projectDirectory()) map.setProject(projects.projectPath(), projects.photoModel());
    });
    QObject::connect(&projects, &ProjectManager::photosChanged, &map, &MapService::photosChanged);
    // Results of a job being deleted leave the map first (Windows keeps open files).
    QObject::connect(&processing, &ProcessingService::releaseDirectory, &map, &MapService::releaseDirectory, Qt::DirectConnection);
    QObject::connect(&processing, &ProcessingService::resultFileReplacing, &map, &MapService::releaseRasterFile, Qt::DirectConnection);
    QObject::connect(&processing, &ProcessingService::resultFileReplaced, &map, &MapService::reopenRasterFile, Qt::DirectConnection);
    // Ground control points, check points, calibration and scale bars (gcp.json).
    SurveyControl survey;
    QObject::connect(&projects, &ProjectManager::projectChanged, &survey, [&survey, &projects, &map] {
        survey.setProject(projects.projectPath(), projects.photoModel(), &map);
    });
    QObject::connect(&map, &MapService::markersChanged, &survey, &SurveyControl::changed);
    // GeoMap AI's PDF report of a job (Обработка → Генерация отчёта).
    ReportService reports(&projects, &processing, &survey);
    QObject::connect(&projects, &ProjectManager::selectionChanged, &map,
                     [&map, &projects] { map.setSelectedPhoto(projects.selectedIndex()); });
    qmlRegisterType<MapView>("GeoMapAI.Map", 1, 0, "MapView");
    // Copies of processing results in a folder of the user's choice.
    ResultExporter exporter(&processing);
    // 3D view: point clouds, models and terrains of the open project.
    Scene3DService scene3d;
    QObject::connect(&projects, &ProjectManager::projectDirectoryChanged, &scene3d, &Scene3DService::clear);
    qmlRegisterType<View3DItem>("GeoMapAI.View3D", 1, 0, "View3DItem");
    // LiDAR clouds of the project (lidar.json): PDAL of the bundled ODM.
    LidarService lidar(&map, &scene3d);
    // Инструменты → Редактор текстуры (a copy of a job's model).
    TextureEditService textures(&processing, &scene3d, &aiModels);
    QObject::connect(&projects, &ProjectManager::projectDirectoryChanged, &lidar, &LidarService::setProject);
    qmlRegisterType<SurveyView>("GeoMapAI.Survey", 1, 0, "SurveyView");
    // Georeferencing of an image by links (Инструменты → Привязка изображения по точкам).
    GeorefService georef(&map, &projects, &processing);
    qmlRegisterType<GeorefImageView>("GeoMapAI.Georef", 1, 0, "GeorefImageView");

    // Professional workspace: commands, layout, journal, project tree.
    CommandRegistry commands;
    WorkspaceState workspace;
    AppLog appLog(QDir(Platform::dataDirectory()).filePath(QStringLiteral("logs")));
    AppInfo appInfo(appLog.logDirectory());
    ProjectTreeModel projectTree(&projects, &processing, &map);
    projectTree.setSurvey(&survey);
    projectTree.setChunks(&chunks);
    ProjectTreeFilter projectTreeView;
    projectTreeView.setSourceModel(&projectTree);
    connectJournal(&appLog, &projects, &processing, &map);
    QObject::connect(&chunks, &ChunkService::errorOccurred, &appLog,
                     [&appLog](const QString &message) { appLog.add(AppLog::Error, QStringLiteral("Блоки"), message); });
    QObject::connect(&scene3d, &Scene3DService::errorOccurred, &appLog,
                     [&appLog](const QString &message) { appLog.add(AppLog::Error, QStringLiteral("3D"), message); });
    QObject::connect(&reports, &ReportService::finished, &appLog, [&appLog](bool ok, const QString &message) {
        appLog.add(ok ? AppLog::Info : AppLog::Error, QStringLiteral("Отчёт"), message);
    });
    QObject::connect(&photoQuality, &PhotoQualityService::finished, &appLog, [&appLog](bool ok, const QString &message) {
        appLog.add(ok ? AppLog::Info : AppLog::Error, QStringLiteral("Качество снимков"), message);
    });
    QObject::connect(&lidar, &LidarService::finished, &appLog, [&appLog](bool ok, const QString &message) {
        appLog.add(ok ? AppLog::Info : AppLog::Error, QStringLiteral("LiDAR"), message);
    });
    QObject::connect(&georef, &GeorefService::finished, &appLog, [&appLog](bool ok, const QString &message) {
        appLog.add(ok ? AppLog::Info : AppLog::Error, QStringLiteral("Привязка"), message);
    });
    appLog.add(AppLog::Info, QStringLiteral("Программа"),
               QStringLiteral("GeoMap AI %1 запущена · %2").arg(QCoreApplication::applicationVersion(), map.gisVersion()));

    QQmlApplicationEngine engine;
    auto *thumbnails = new ThumbnailProvider;   // owned by the engine
    QObject::connect(&projects, &ProjectManager::projectDirectoryChanged, thumbnails,
                     [thumbnails](const QString &directory) { thumbnails->setProjectDirectory(directory); });
    engine.addImageProvider(QStringLiteral("thumbs"), thumbnails);
    engine.addImageProvider(QStringLiteral("texture"), new TextureImageProvider(&textures));   // owned by the engine
    engine.rootContext()->setContextProperty("projects", &projects);
    engine.rootContext()->setContextProperty("processing", &processing);
    engine.rootContext()->setContextProperty("gis", &map);
    engine.rootContext()->setContextProperty("scene3d", &scene3d);
    engine.rootContext()->setContextProperty("exporter", &exporter);
    engine.rootContext()->setContextProperty("aiModels", &aiModels);
    engine.rootContext()->setContextProperty("photoMasks", &photoMasks);
    engine.rootContext()->setContextProperty("survey", &survey);
    engine.rootContext()->setContextProperty("georef", &georef);
    engine.rootContext()->setContextProperty("lidar", &lidar);
    engine.rootContext()->setContextProperty("reports", &reports);
    engine.rootContext()->setContextProperty("textures", &textures);
    engine.rootContext()->setContextProperty("chunks", &chunks);
    engine.rootContext()->setContextProperty("photoQuality", &photoQuality);
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
