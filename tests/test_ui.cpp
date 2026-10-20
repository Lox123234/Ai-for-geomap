#include <QtTest>
#include <QSignalSpy>
#include <QDir>
#include <QJsonDocument>
#include <QPainter>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSettings>
#include <QTemporaryDir>
#include <QtQml/qqmlextensionplugin.h>
#include <gdal.h>
#include "Crs.h"
#include <ogr_srs_api.h>
#include "AiService.h"
#include "AppInfo.h"
#include "AppLog.h"
#include "CommandRegistry.h"
#include "GeorefImageView.h"
#include "GeorefService.h"
#include "GisEnvironment.h"
#include "ChunkService.h"
#include "Journal.h"
#include "LidarService.h"
#include "OdmTools.h"
#include "MapService.h"
#include "MapView.h"
#include "MockEngine.h"
#include "PhotoMasks.h"
#include "ProcessingService.h"
#include "SurveyControl.h"
#include "TestImages.h"
#include <cmath>
#include "ProjectManager.h"
#include "ProjectTreeModel.h"
#include "ResultExporter.h"
#include "Scene3DService.h"
#include "ThumbnailProvider.h"
#include "View3DItem.h"
#include "SurveyView.h"
#include "WorkspaceState.h"

Q_IMPORT_QML_PLUGIN(GeoMapAIPlugin)

// The real main window (Main.qml with the real services and a scripted processing engine),
// offscreen with the software renderer: menus and toolbar from the registry, command handlers,
// shortcuts, panels, layout persistence and the project tree's actions.
// GEOMAP_TEST_OUTPUT=<folder> keeps screenshots of the views.
namespace {
struct Session
{
    std::shared_ptr<MockEngine::Record> record = std::make_shared<MockEngine::Record>();
    ProjectManager projects;
    ProcessingService processing{new MockEngine(record)};
    ChunkService chunks{&projects, &processing};
    ai::AiService aiModels;
    MapService map;
    PhotoMasks photoMasks{&aiModels};
    SurveyControl survey;
    Scene3DService scene3d;
    ResultExporter exporter{&processing};
    CommandRegistry commands;
    WorkspaceState workspace;
    AppLog log;
    AppInfo info{QString()};
    ProjectTreeModel tree{&projects, &processing, &map};
    ProjectTreeFilter treeView;
    GeorefService georef{&map, &projects, &processing};
    LidarService lidar{&map, &scene3d};
    QQmlApplicationEngine engine;
    QQuickWindow *window = nullptr;

    explicit Session(const QString &logDirectory) : log(logDirectory)
    {
        QObject::connect(&projects, &ProjectManager::projectDirectoryChanged, &processing,
                         [this](const QString &directory) { processing.setProject(directory, projects.photoModel()); });
        QObject::connect(&projects, &ProjectManager::projectChanged, &map, [this] {
            if (projects.projectPath() != map.projectDirectory()) map.setProject(projects.projectPath(), projects.photoModel());
        });
        QObject::connect(&projects, &ProjectManager::projectDirectoryChanged, &scene3d, &Scene3DService::clear);
        QObject::connect(&projects, &ProjectManager::projectDirectoryChanged, &lidar, &LidarService::setProject);
        treeView.setSourceModel(&tree);
        tree.setSurvey(&survey);
        tree.setChunks(&chunks);
        map.setAi(&aiModels);
        QObject::connect(&projects, &ProjectManager::projectDirectoryChanged, &photoMasks, &PhotoMasks::setProject);
        QObject::connect(&projects, &ProjectManager::projectChanged, &survey,
                         [this] { survey.setProject(projects.projectPath(), projects.photoModel(), &map); });
        QObject::connect(&map, &MapService::markersChanged, &survey, &SurveyControl::changed);
        QObject::connect(&processing, &ProcessingService::releaseDirectory, &map, &MapService::releaseDirectory, Qt::DirectConnection);
        QObject::connect(&projects, &ProjectManager::photosChanged, &map, &MapService::photosChanged);
        QObject::connect(&processing, &ProcessingService::resultFileReplacing, &map, &MapService::releaseRasterFile, Qt::DirectConnection);
        QObject::connect(&processing, &ProcessingService::resultFileReplaced, &map, &MapService::reopenRasterFile, Qt::DirectConnection);
        connectJournal(&log, &projects, &processing, &map);
        auto *thumbnails = new ThumbnailProvider;
        QObject::connect(&projects, &ProjectManager::projectDirectoryChanged, thumbnails,
                         [thumbnails](const QString &directory) { thumbnails->setProjectDirectory(directory); });
        engine.addImageProvider(QStringLiteral("thumbs"), thumbnails);
        QQmlContext *c = engine.rootContext();
        c->setContextProperty("projects", &projects);
        c->setContextProperty("processing", &processing);
        c->setContextProperty("gis", &map);
        c->setContextProperty("scene3d", &scene3d);
        c->setContextProperty("exporter", &exporter);
        c->setContextProperty("aiModels", &aiModels);
        c->setContextProperty("photoMasks", &photoMasks);
        c->setContextProperty("survey", &survey);
        c->setContextProperty("georef", &georef);
        c->setContextProperty("lidar", &lidar);
        c->setContextProperty("chunks", &chunks);
        c->setContextProperty("commands", &commands);
        c->setContextProperty("workspace", &workspace);
        c->setContextProperty("appLog", &log);
        c->setContextProperty("appInfo", &info);
        c->setContextProperty("projectTree", &tree);
        c->setContextProperty("projectTreeView", &treeView);
        engine.loadFromModule("GeoMapAI", "Main");
        window = engine.rootObjects().isEmpty() ? nullptr : qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    }

    QVariant call(const char *function, const QVariant &a = {}, const QVariant &b = {})
    {
        QVariant result;
        if (!b.isValid() && !a.isValid()) QMetaObject::invokeMethod(window, function, Q_RETURN_ARG(QVariant, result));
        else if (!b.isValid()) QMetaObject::invokeMethod(window, function, Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, a));
        else QMetaObject::invokeMethod(window, function, Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, a), Q_ARG(QVariant, b));
        return result;
    }
    bool run(const QString &id) { return call("runCommand", id).toBool(); }
    bool enabled(const QString &id) { return call("commandEnabled", id).toBool(); }
    bool checked(const QString &id) { return call("commandChecked", id).toBool(); }
    QVariant prop(const char *name) const { return window->property(name); }
    QObject *findByCommand(const QString &id, const char *className) const
    {
        QList<QQuickItem *> stack{window->contentItem()};
        while (!stack.isEmpty()) {
            QQuickItem *item = stack.takeLast();
            if (QString::fromLatin1(item->metaObject()->className()).startsWith(QLatin1String(className))
                && item->property("commandId").toString() == id)
                return item;
            stack << item->childItems();
        }
        return nullptr;
    }
    // By objectName in the item tree: view delegates are not QObject children of the window.
    QQuickItem *findItem(const QString &name) const
    {
        QList<QQuickItem *> stack{window->contentItem()};
        while (!stack.isEmpty()) {
            QQuickItem *item = stack.takeLast();
            if (item->objectName() == name) return item;
            stack << item->childItems();
        }
        return nullptr;
    }
    void shot(const QString &name)
    {
        const QString keep = qEnvironmentVariable("GEOMAP_TEST_OUTPUT");
        if (keep.isEmpty() || !QDir().mkpath(keep)) return;
        QTest::qWait(300);
        window->grabWindow().save(QDir(keep).filePath(name));
    }
};

bool makeOrthophoto(const QString &path)
{
    GDALDatasetH ds = GDALCreate(GDALGetDriverByName("GTiff"), path.toUtf8().constData(), 200, 150, 3, GDT_Byte, nullptr);
    if (!ds) return false;
    double gt[6] = {576650, 0.05, 0, 5188230, 0, -0.05};
    GDALSetGeoTransform(ds, gt);
    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    OSRImportFromEPSG(srs, 32615);
    GDALSetSpatialRef(ds, srs);
    OSRDestroySpatialReference(srs);
    std::vector<unsigned char> row(200);
    for (int b = 1; b <= 3; ++b)
        for (int y = 0; y < 150; ++y) {
            for (int x = 0; x < 200; ++x) row[x] = static_cast<unsigned char>(b == 2 ? 160 : (x + y) % 120);
            if (GDALRasterIO(GDALGetRasterBand(ds, b), GF_Write, 0, y, 200, 1, row.data(), 200, 1, GDT_Byte, 0, 0) != CE_None) return false;
        }
    GDALClose(ds);
    return true;
}
} // namespace

class UiTest : public QObject
{
    Q_OBJECT
    QTemporaryDir m_settings;
    QTemporaryDir m_data;

    std::unique_ptr<Session> open()
    {
        auto s = std::make_unique<Session>(m_data.filePath(QStringLiteral("logs")));
        if (s->window) {
            s->window->show();
            if (!QTest::qWaitForWindowExposed(s->window)) qWarning("window not exposed");
            s->window->requestActivate();
            if (!QTest::qWaitForWindowActive(s->window, 2000)) qWarning("window not active");
        }
        return s;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_settings.isValid());
        QVERIFY(m_data.isValid());
        QCoreApplication::setOrganizationName(QStringLiteral("GeoMapAI-Tests"));
        QCoreApplication::setApplicationName(QStringLiteral("Ui"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settings.path());
        GisEnvironment::initialize(QCoreApplication::applicationDirPath());
        qmlRegisterType<MapView>("GeoMapAI.Map", 1, 0, "MapView");
        qmlRegisterType<View3DItem>("GeoMapAI.View3D", 1, 0, "View3DItem");
        qmlRegisterType<SurveyView>("GeoMapAI.Survey", 1, 0, "SurveyView");
        qmlRegisterType<GeorefImageView>("GeoMapAI.Georef", 1, 0, "GeorefImageView");
    }

    void windowHasMenusToolbarAndHandlers()
    {
        auto s = open();
        QVERIFY(s->window);
        // Every ready command has a handler: no button or menu item that does nothing.
        QCOMPARE(s->call("unhandledCommands").toStringList(), QStringList());
        // Menu bar: the eight menus of the registry.
        QObject *menuBar = s->prop("menuBar").value<QObject *>();
        QVERIFY(menuBar);
        QCOMPARE(menuBar->property("count").toInt(), 8);
        // Toolbar: a button for every command of every group; planned ones disabled.
        for (const QVariant &g : s->commands.toolbar())
            for (const QVariant &id : g.toMap().value("items").toList()) {
                QObject *button = s->findByCommand(id.toString(), "ToolIconButton");
                QVERIFY2(button, qPrintable(id.toString()));
                if (!s->commands.isReady(id.toString())) QVERIFY2(!button->property("enabled").toBool(), qPrintable(id.toString()));
            }
        QVERIFY(s->findByCommand(QStringLiteral("file.new"), "ToolIconButton")->property("enabled").toBool());
        QVERIFY(!s->findByCommand(QStringLiteral("file.save"), "ToolIconButton")->property("enabled").toBool());   // no project
        // Planned commands do not run and say why.
        QVERIFY(!s->run(QStringLiteral("proc.tiled")));
        QVERIFY(s->prop("transientMessage").toString().contains(QStringLiteral("после 1.0")));
        QVERIFY(!s->run(QStringLiteral("proc.optimize")));
        QVERIFY(s->prop("transientMessage").toString().contains(QStringLiteral("движком")));
        s->shot(QStringLiteral("ui-01-welcome.png"));
        // Menus are built from the registry: open "Обработка" to see ready, planned and engine-blocked items.
        QStringList titles;
        for (const QVariant &m : s->commands.menus()) titles << m.toMap().value("title").toString();
        QList<QObject *> menus;
        for (int i = 0; i < menuBar->property("count").toInt(); ++i)
            if (QObject *m = s->call("menuAt", i).value<QObject *>(); m && titles.contains(m->property("title").toString())) menus << m;
        QCOMPARE(menus.size(), 8);
        QObject *processingMenu = nullptr;
        for (QObject *m : menus) if (m->property("title").toString().contains(QStringLiteral("Обработка"))) processingMenu = m;
        QVERIFY(processingMenu);
        QCOMPARE(processingMenu->property("count").toInt(), 29);   // 23 commands + 6 separators
        QMetaObject::invokeMethod(processingMenu, "open");
        QTRY_VERIFY(processingMenu->property("opened").toBool());
        s->shot(QStringLiteral("ui-06-menu-processing.png"));
        QMetaObject::invokeMethod(processingMenu, "close");
    }

    void commandsAndShortcutsDriveTheWorkspace()
    {
        QTemporaryDir tmp;
        auto s = open();
        QVERIFY(s->projects.createProject(QUrl::fromLocalFile(tmp.path()), QStringLiteral("Съёмка UI")));
        QTRY_VERIFY(s->enabled(QStringLiteral("file.save")));
        QVERIFY(s->run(QStringLiteral("view.map")));
        QCOMPARE(s->prop("activeView").toInt(), 3);
        QVERIFY(s->checked(QStringLiteral("view.map")));
        // Shortcuts reach the same handlers.
        QTest::keyClick(s->window, Qt::Key_1, Qt::ControlModifier);
        QTRY_COMPARE(s->prop("activeView").toInt(), 0);
        QTest::keyClick(s->window, Qt::Key_4, Qt::ControlModifier);
        QTRY_COMPARE(s->prop("activeView").toInt(), 2);
        QTest::keyClick(s->window, Qt::Key_1, Qt::AltModifier);
        QTRY_COMPARE(s->prop("showLeft").toBool(), false);
        QTest::keyClick(s->window, Qt::Key_1, Qt::AltModifier);
        QTRY_COMPARE(s->prop("showLeft").toBool(), true);
        // Map tools need a CRS of the map; then they switch to the map and show as active.
        QVERIFY(!s->enabled(QStringLiteral("tool.ruler")));
        QVERIFY(!s->run(QStringLiteral("tool.ruler")));
        QCOMPARE(s->map.setMapCrs(QStringLiteral("32615")), QString());
        QTRY_VERIFY(s->enabled(QStringLiteral("tool.ruler")));
        QVERIFY(s->run(QStringLiteral("tool.ruler")));
        QCOMPARE(s->map.tool(), QStringLiteral("length"));
        QCOMPARE(s->prop("activeView").toInt(), 3);
        QVERIFY(s->checked(QStringLiteral("tool.ruler")));
        QTest::keyClick(s->window, Qt::Key_H);
        QTRY_COMPARE(s->map.tool(), QStringLiteral("pan"));
        QVERIFY(!s->enabled(QStringLiteral("edit.undo")));
        QVERIFY(!s->enabled(QStringLiteral("export.drawings")));
        QVERIFY(s->enabled(QStringLiteral("export.map")));
        // A user shortcut replaces the default in the running window.
        QCOMPARE(s->commands.setShortcut(QStringLiteral("view.photo"), QStringLiteral("F6")), QString());
        QTest::keyClick(s->window, Qt::Key_F6);
        QTRY_COMPARE(s->prop("activeView").toInt(), 0);
        s->commands.resetShortcuts();
        // The journal shows what the services reported.
        QVERIFY(s->log.count() > 0);
        bool opened = false;
        for (const AppLog::Entry &e : s->log.entries()) opened = opened || e.text.contains(QStringLiteral("Съёмка UI"));
        QVERIFY(opened);
        s->run(QStringLiteral("view.map"));
        s->shot(QStringLiteral("ui-02-map-empty.png"));
        s->run(QStringLiteral("view.processing"));
        s->shot(QStringLiteral("ui-03-processing.png"));
    }

    void projectTreeOpensResults()
    {
        QTemporaryDir tmp;
        QString project;
        {
            ProjectManager pm;
            QVERIFY(pm.createProject(QUrl::fromLocalFile(tmp.path()), QStringLiteral("Результаты")));
            project = pm.projectPath();
        }
        ProcessingJob job;
        job.id = QStringLiteral("20261014-130000");
        job.engine = QStringLiteral("mock");
        job.profile = QStringLiteral("fast");
        job.state = JobState::Completed;
        job.createdAt = QDateTime::currentDateTimeUtc();
        job.outputs << Artifact{QStringLiteral("orthophoto"), QStringLiteral("odm_orthophoto/odm_orthophoto.tif"), 0, QStringLiteral("EPSG:32615")};
        const QString ortho = QDir(project).filePath(QStringLiteral("jobs/%1/odm_orthophoto/odm_orthophoto.tif").arg(job.id));
        QVERIFY(QDir().mkpath(QFileInfo(ortho).absolutePath()));
        QVERIFY(makeOrthophoto(ortho));
        QFile jobFile(QDir(project).filePath(QStringLiteral("jobs/%1/job.json").arg(job.id)));
        QVERIFY(jobFile.open(QIODevice::WriteOnly));
        jobFile.write(QJsonDocument(job.toJson()).toJson());
        jobFile.close();

        auto s = open();
        QVERIFY(s->projects.openProject(QUrl::fromLocalFile(project)));
        QTRY_VERIFY(!s->tree.result(QStringLiteral("orthophoto")).isEmpty());
        // A real double click on a row of the tree opens it (the photos: the photo view).
        s->run(QStringLiteral("view.map"));
        QQuickItem *photosRow = nullptr;
        QTRY_VERIFY((photosRow = s->findItem(QStringLiteral("treeRow:photos"))) != nullptr);
        QTRY_VERIFY(photosRow->isVisible() && photosRow->width() > 0);
        QTest::mouseDClick(s->window, Qt::LeftButton, Qt::NoModifier,
                           photosRow->mapToScene(QPointF(photosRow->width() / 2, photosRow->height() / 2)).toPoint());
        QTRY_COMPARE(s->prop("activeView").toInt(), 0);
        // Double click on the orthophoto in the tree: added to the map once, selected and shown.
        const QVariantMap item{{"kind", "orthophoto"}, {"title", "Ортофотоплан"}, {"available", true},
                               {"payload", s->tree.result(QStringLiteral("orthophoto")).value("path")},
                               {"jobId", job.id}, {"file", job.outputs.first().file}};
        s->call("openTreeItem", item);
        QCOMPARE(s->prop("activeView").toInt(), 3);
        const QString layer = s->map.layerIdForSource(ortho);
        QVERIFY(!layer.isEmpty());
        QCOMPARE(s->map.selectedLayer(), layer);
        s->call("openTreeItem", item);
        s->run(QStringLiteral("view.photo"));
        QVERIFY(s->run(QStringLiteral("view.orthophoto")));   // «Вид → Ортофотоплан»: the same layer
        QCOMPARE(s->prop("activeView").toInt(), 3);
        QCOMPARE(s->map.selectedLayer(), layer);
        int rasters = 0;
        for (const MapLayer &l : s->map.layers()) rasters += l.type == MapLayer::Raster;
        QCOMPARE(rasters, 1);
        QVERIFY(s->map.crsDefined());
        // Context actions of a layer and of a planned section.
        const QVariantList layerActions = s->call("treeActions", QVariantMap{{"kind", "layer"}, {"payload", layer}, {"available", true}}).toList();
        QCOMPARE(layerActions.size(), 3);
        const QVariantList gcpActions = s->call("treeActions", QVariantMap{{"kind", "gcp"}, {"available", true}}).toList();
        QCOMPARE(gcpActions.size(), 4);
        QCOMPARE(gcpActions.at(1).toMap().value("enabled").toBool(), false);   // no points to place
        const QVariantList unknownActions = s->call("treeActions", QVariantMap{{"kind", "chunk"}, {"available", false}, {"status", "нет данных"}}).toList();
        QCOMPARE(unknownActions.size(), 1);
        QCOMPARE(unknownActions.first().toMap().value("enabled").toBool(), false);
        s->shot(QStringLiteral("ui-04-map-orthophoto.png"));
        // Export of the results: the command opens the dialog with the job's results checked.
        QVERIFY(s->run(QStringLiteral("export.all")));
        QObject *exportDialog = s->window->findChild<QObject *>(QStringLiteral("exportDialog"));
        QVERIFY(exportDialog);
        QTRY_VERIFY(exportDialog->property("visible").toBool());
        QCOMPARE(exportDialog->property("jobId").toString(), job.id);
        QCOMPARE(exportDialog->property("count").toInt(), 1);
        QVERIFY(exportDialog->property("anyRaster").toBool());
        const QString exportFolder = QDir(tmp.path()).filePath(QStringLiteral("export"));
        QSignalSpy exported(&s->exporter, &ResultExporter::finished);
        QCOMPARE(s->exporter.start(job.id, {job.outputs.first().file}, QUrl::fromLocalFile(exportFolder), {{"crs", "4326"}}), QString());
        QVERIFY(exported.wait(30000));
        QVERIFY2(exported.first().at(0).toBool(), qPrintable(exported.first().at(1).toString()));
        QVERIFY(QFileInfo::exists(exportFolder + QStringLiteral("/20261014-130000-odm_orthophoto-epsg4326.tif")));
        QTRY_VERIFY(exportDialog->property("resultText").toString().contains(QStringLiteral("Сохранено 1")));
        s->shot(QStringLiteral("ui-12-export.png"));
        QMetaObject::invokeMethod(exportDialog, "close");
        s->run(QStringLiteral("view.photo"));
        s->shot(QStringLiteral("ui-05-photos.png"));
    }

    // Terrain analysis in the window: slope and contours become layers, the profile tool draws
    // a section with its panel, the volume tool measures a box of known volume.
    void terrainAnalysisOnTheMap()
    {
        QTemporaryDir tmp;
        auto s = open();
        QVERIFY(s->projects.createProject(QUrl::fromLocalFile(tmp.path()), QStringLiteral("Рельеф")));
        QCOMPARE(s->map.setMapCrs(QStringLiteral("32615")), QString());
        // 60 × 60 m DEM, 1 m cells: flat at 100 m with a 20 × 20 m box 5 m high in the middle.
        const QString dem = QDir(tmp.path()).filePath(QStringLiteral("dem.tif"));
        {
            GDALDatasetH ds = GDALCreate(GDALGetDriverByName("GTiff"), dem.toUtf8().constData(), 60, 60, 1, GDT_Float32, nullptr);
            QVERIFY(ds);
            double gt[6] = {576000, 1, 0, 5188100, 0, -1};
            GDALSetGeoTransform(ds, gt);
            OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
            OSRImportFromEPSG(srs, 32615);
            GDALSetSpatialRef(ds, srs);
            OSRDestroySpatialReference(srs);
            std::vector<float> row(60);
            for (int r = 0; r < 60; ++r) {
                for (int c = 0; c < 60; ++c) row[c] = (c >= 20 && c < 40 && r >= 20 && r < 40) ? 105.f : 100.f;
                QCOMPARE(GDALRasterIO(GDALGetRasterBand(ds, 1), GF_Write, 0, r, 60, 1, row.data(), 60, 1, GDT_Float32, 0, 0), CE_None);
            }
            GDALClose(ds);
        }
        QCOMPARE(s->map.addLayerFile(dem), QString());
        const QString demId = s->map.layerIdForSource(dem);
        const auto demReady = [&] {
            for (const MapLayer &l : s->map.layers())
                if (l.id == demId) return !l.loading && l.raster != nullptr;
            return false;
        };
        QTRY_VERIFY_WITH_TIMEOUT(demReady(), 30000);
        QCOMPARE(s->map.elevationLayers().size(), 1);
        QCOMPARE(s->map.terrainLayerId(), demId);

        // Slope: the command opens the dialog; the analysis adds a layer with the slope ramp.
        QVERIFY(s->run(QStringLiteral("view.slope")));
        QObject *dialog = s->window->findChild<QObject *>(QStringLiteral("terrainDialog"));
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("visible").toBool());
        QCOMPARE(dialog->property("layerId").toString(), demId);
        QSignalSpy finished(&s->map, &MapService::analysisFinished);
        const int layersBefore = s->map.layerCount();
        QCOMPARE(s->map.analyzeTerrain(demId, QStringLiteral("slope")), QString());
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 30000);
        QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(1).toString()));
        QTRY_COMPARE(s->map.layerCount(), layersBefore + 1);
        const QString slopePath = QDir(s->projects.projectPath()).filePath(QStringLiteral("analysis/dem-slope.tif"));
        QVERIFY(QFileInfo::exists(slopePath));
        QCOMPARE(s->map.layerDetails(s->map.layerIdForSource(slopePath)).value("style").toMap().value("ramp").toString(), QStringLiteral("slope"));
        QVERIFY(dialog->property("resultText").toString().contains(QStringLiteral("Карта уклонов")));
        // The slope map on top, even selected, is not what the tools measure: the model of heights is.
        s->map.selectLayer(s->map.layerIdForSource(slopePath));
        QCOMPARE(s->map.terrainLayerId(), demId);
        QCOMPARE(s->map.elevationLayers().first().toMap().value("id").toString(), demId);
        s->shot(QStringLiteral("ui-08-terrain-slope.png"));
        QMetaObject::invokeMethod(dialog, "close");
        // Contours every metre: a vector layer.
        finished.clear();
        QCOMPARE(s->map.analyzeTerrain(demId, QStringLiteral("contours"), 1.0, 0.0), QString());
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 30000);
        QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(1).toString()));
        QVERIFY(finished.first().at(1).toString().contains(QStringLiteral("линий")));
        QCOMPARE(s->map.layerCount(), layersBefore + 2);

        // Difference of two models: the box over flat ground at 100 m (a copy of the DEM flattened).
        const QString flat = QDir(tmp.path()).filePath(QStringLiteral("flat.tif"));
        {
            GDALDatasetH src = GDALOpen(dem.toUtf8().constData(), GA_ReadOnly);
            GDALDatasetH ds = GDALCreateCopy(GDALGetDriverByName("GTiff"), flat.toUtf8().constData(), src, FALSE, nullptr, nullptr, nullptr);
            GDALClose(src);
            QVERIFY(ds);
            std::vector<float> row(60, 100.f);
            for (int r = 0; r < 60; ++r) GDALRasterIO(GDALGetRasterBand(ds, 1), GF_Write, 0, r, 60, 1, row.data(), 60, 1, GDT_Float32, 0, 0);
            GDALClose(ds);
        }
        QCOMPARE(s->map.addLayerFile(flat), QString());
        const QString flatId = s->map.layerIdForSource(flat);
        QTRY_VERIFY_WITH_TIMEOUT(s->map.elevationLayers().size() == 3, 30000);   // dem, slope, flat
        // The dialog pairs the two models of heights, never a model with the slope map or itself.
        QMetaObject::invokeMethod(dialog, "openFor", Q_ARG(QVariant, QStringLiteral("difference")));
        const QStringList pair{dialog->property("layerId").toString(), dialog->property("otherLayerId").toString()};
        QVERIFY2(pair.contains(demId) && pair.contains(flatId), qPrintable(pair.join(' ')));
        QMetaObject::invokeMethod(dialog, "close");
        finished.clear();
        QCOMPARE(s->map.analyzeTerrain(demId, QStringLiteral("difference"), 0, 0, demId), QStringLiteral("Выберите две разные модели высот."));
        QCOMPARE(s->map.analyzeTerrain(demId, QStringLiteral("difference"), 0, 0, flatId), QString());
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 30000);
        QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(1).toString()));
        QVERIFY2(finished.first().at(1).toString().contains(QStringLiteral("до 5,00 м")), qPrintable(finished.first().at(1).toString()));
        s->map.removeLayer(flatId);

        // Profile across the box: west to east through its middle.
        s->map.selectLayer(demId);
        QVERIFY(s->run(QStringLiteral("tool.profile")));
        QCOMPARE(s->map.tool(), QStringLiteral("profile"));
        QSignalSpy ready(&s->map, &MapService::terrainResultReady);
        for (const QPointF &p : {QPointF(576005.5, 5188070.5), QPointF(576055.5, 5188070.5)}) {
            const QPointF screen = s->map.toScreen(p);
            s->map.pointerPressed(screen, Qt::LeftButton, Qt::NoModifier);
            s->map.pointerReleased(screen);
        }
        s->map.finishSketch();
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 30000);
        const QVariantMap profile = s->map.profileForQml();
        QVERIFY(profile.value("valid").toBool());
        QCOMPARE(profile.value("maxZ").toDouble(), 105.0);
        QCOMPARE(profile.value("minZ").toDouble(), 100.0);
        QVERIFY(std::abs(profile.value("ascent").toDouble() - 5.0) < 1e-3);
        QVERIFY(s->map.measurementText().startsWith(QStringLiteral("Профиль:")));
        QObject *panel = s->window->findChild<QObject *>(QStringLiteral("profilePanel"));
        QVERIFY(panel);
        QTRY_VERIFY(panel->property("visible").toBool());
        const QString csv = QDir(tmp.path()).filePath(QStringLiteral("profile.csv"));
        QCOMPARE(s->map.exportProfile(QUrl::fromLocalFile(csv)), QString());
        QFile file(csv);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readLine().trimmed(), QByteArray("distance_m,elevation_m,x,y"));
        s->shot(QStringLiteral("ui-09-terrain-profile.png"));

        // Volume of the box above the boundary level: 20 × 20 × 5 = 2000 m³.
        QVERIFY(s->run(QStringLiteral("tool.volume")));
        QCOMPARE(s->map.tool(), QStringLiteral("volume"));
        ready.clear();
        for (const QPointF &p : {QPointF(576010, 5188090), QPointF(576050, 5188090), QPointF(576050, 5188050), QPointF(576010, 5188050)}) {
            const QPointF screen = s->map.toScreen(p);
            s->map.pointerPressed(screen, Qt::LeftButton, Qt::NoModifier);
            s->map.pointerReleased(screen);
        }
        s->map.finishSketch();
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 30000);
        QVERIFY2(std::abs(s->map.volumeResult().above - 2000) < 1e-3, qPrintable(s->map.measurementText()));
        QVERIFY2(s->map.measurementText().contains(QStringLiteral("2 000 м³")) || s->map.measurementText().contains(QStringLiteral("2\u00a0000 м³")),
                 qPrintable(s->map.measurementText()));
        // Another base recomputes the same outline: level 102 m → 1200 m³ above, 2400 m³ below.
        ready.clear();
        s->map.setVolumeLevel(102);
        s->map.setVolumeBase(QStringLiteral("level"));
        QTRY_VERIFY_WITH_TIMEOUT(ready.count() >= 1 && std::abs(s->map.volumeResult().below - 2400) < 1e-3, 30000);
        QVERIFY(std::abs(s->map.volumeResult().above - 1200) < 1e-3);
        s->shot(QStringLiteral("ui-10-terrain-volume.png"));

        // Saved measurements: the volume is kept in the project with its values and method.
        QVERIFY(s->map.canSaveMeasurement());
        const QString saved = s->map.saveMeasurement(QStringLiteral("Отвал"));
        QVERIFY(!saved.isEmpty());
        QCOMPARE(s->map.savedMeasurements().size(), 1);
        const MapService::SavedMeasurement m = s->map.savedMeasurements().first();
        QCOMPARE(m.kind, QStringLiteral("volume"));
        QCOMPARE(m.values.value("below_m3").toDouble(), 2400.0);
        QCOMPARE(m.layer, QStringLiteral("dem"));
        QCOMPARE(m.geometry.size(), 4);
        // WGS 84 longitude, latitude: 76 km east of the zone's central meridian (−93°) at 46.8° N.
        QVERIFY2(std::abs(m.geometry.first().x() + 92.0) < 0.05 && std::abs(m.geometry.first().y() - 46.84) < 0.05,
                 qPrintable(QStringLiteral("%1 %2").arg(m.geometry.first().x()).arg(m.geometry.first().y())));
        const QString measurementsFile = QDir(s->projects.projectPath()).filePath(QStringLiteral("measurements.json"));
        QVERIFY(QFileInfo::exists(measurementsFile));
        s->map.renameMeasurement(saved, QStringLiteral("Отвал № 1"));
        s->map.showMeasurement(saved);
        QCOMPARE(s->map.shownMeasurement(), saved);
        s->shot(QStringLiteral("ui-11-measurements.png"));
        // Reopening the project restores them.
        const QString projectPath = s->projects.projectPath();
        s->map.setProject(QString(), nullptr);
        QCOMPARE(s->map.savedMeasurements().size(), 0);
        s->map.setProject(projectPath, s->projects.photoModel());
        QCOMPARE(s->map.savedMeasurements().size(), 1);
        QCOMPARE(s->map.savedMeasurements().first().name, QStringLiteral("Отвал № 1"));
        const QString csvAll = QDir(tmp.path()).filePath(QStringLiteral("measurements.csv"));
        QCOMPARE(s->map.exportMeasurements(QUrl::fromLocalFile(csvAll)), QString());
        QFile all(csvAll);
        QVERIFY(all.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(all.readAll());
        QVERIFY(text.contains(QStringLiteral("POLYGON((")));
        QVERIFY(text.contains(QStringLiteral("2400.000")));
        s->map.removeMeasurement(saved);
        QCOMPARE(s->map.savedMeasurements().size(), 0);

        auto click = [&](const QPointF &p) {
            const QPointF screen = s->map.toScreen(p);
            s->map.pointerPressed(screen, Qt::LeftButton, Qt::NoModifier);
            s->map.pointerReleased(screen);
        };
        QSignalSpy terrain(&s->map, &MapService::terrainResultReady);
        // Slope between two points: from the ground onto the box (5 m up).
        QVERIFY(s->run(QStringLiteral("tool.slope")));
        QCOMPARE(s->map.tool(), QStringLiteral("slope"));
        click({576010.5, 5188070.5});
        click({576030.5, 5188070.5});
        QTRY_VERIFY_WITH_TIMEOUT(s->map.measurementText().startsWith(QStringLiteral("Уклон")), 30000);
        QVERIFY2(s->map.measurementText().contains(QStringLiteral("+5,00 м")), qPrintable(s->map.measurementText()));
        QVERIFY(s->map.canSaveMeasurement());
        // Height of a point; angle of three points.
        QVERIFY(s->run(QStringLiteral("tool.height")));
        click({576030.5, 5188070.5});
        QVERIFY2(s->map.measurementText().contains(QStringLiteral("105,00 м")), qPrintable(s->map.measurementText()));
        QVERIFY(s->run(QStringLiteral("tool.angle")));
        click({576000.5, 5188060});
        click({576010.5, 5188060});
        click({576010.5, 5188050});
        QVERIFY2(s->map.measurementText().startsWith(QStringLiteral("Угол 90,00°")), qPrintable(s->map.measurementText()));
        // Cross sections: the settings panel, one section per 10 m, navigation and CSV.
        QVERIFY(s->run(QStringLiteral("analysis.crossProfile")));
        QObject *settings = s->window->findChild<QObject *>(QStringLiteral("terrainSettings"));
        QVERIFY(settings);
        QTRY_VERIFY(settings->property("visible").toBool());
        s->map.setCrossHalfWidth(20);
        click({576030.5, 5188090});
        click({576030.5, 5188050});
        s->map.finishSketch();
        QTRY_VERIFY_WITH_TIMEOUT(s->map.crossCount() == 5, 30000);
        QCOMPARE(s->map.profileForQml().value("sections").toInt(), 5);
        s->map.setCrossIndex(2);   // through the box: 5 m up and down again
        QVERIFY(std::abs(s->map.profileResult().ascent - 5) < 0.01);
        const QString cross = QDir(tmp.path()).filePath(QStringLiteral("cross.csv"));
        QCOMPARE(s->map.exportCrossSections(QUrl::fromLocalFile(cross)), QString());
        QVERIFY(QFileInfo(cross).size() > 1000);
        // Viewshed: a layer of visible / hidden cells.
        QVERIFY(s->run(QStringLiteral("analysis.viewshed")));
        const int beforeView = s->map.layerCount();
        finished.clear();
        click({576005.5, 5188050});
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 30000);
        QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(1).toString()));
        QVERIFY(finished.first().at(1).toString().startsWith(QStringLiteral("Видно")));
        QTRY_COMPARE(s->map.layerCount(), beforeView + 1);
        // Raster calculator from its dialog's command.
        QVERIFY(s->run(QStringLiteral("tool.rasterCalc")));
        QObject *calc = s->window->findChild<QObject *>(QStringLiteral("rasterCalcDialog"));
        QVERIFY(calc);
        QTRY_VERIFY(calc->property("visible").toBool());
        QMetaObject::invokeMethod(calc, "close");
        finished.clear();
        QCOMPARE(s->map.checkRasterExpression(demId, QStringLiteral("B2")), QStringLiteral("Канала B2 нет: в растре каналов 1"));
        QCOMPARE(s->map.calculateRaster(demId, QStringLiteral("calc"), QStringLiteral("B1 - 100")), QString());
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 30000);
        QVERIFY2(finished.first().at(1).toString().contains(QStringLiteral("от 0,000 до 5,000")), qPrintable(finished.first().at(1).toString()));
        QVERIFY(s->map.calculateRaster(demId, QStringLiteral("exg")).contains(QStringLiteral("цветной")));
        // AI: the tool, its panel and the models dialog.
        QVERIFY(s->run(QStringLiteral("analysis.ai")));
        QCOMPARE(s->map.tool(), QStringLiteral("aiSelect"));
        QObject *ai = s->window->findChild<QObject *>(QStringLiteral("aiPanel"));
        QVERIFY(ai);
        QTRY_VERIFY(ai->property("visible").toBool());
        // AI retouch: only with a true orthophoto and its streak mask on the map (none here).
        QVERIFY(!s->enabled(QStringLiteral("analysis.retouch")));
        QVERIFY(s->map.retouchOrthophoto().contains(QStringLiteral("маской")));
        QVERIFY(s->run(QStringLiteral("analysis.aiModels")));
        QObject *models = s->window->findChild<QObject *>(QStringLiteral("aiModelsDialog"));
        QVERIFY(models);
        QTRY_VERIFY(models->property("visible").toBool());
        s->shot(QStringLiteral("ui-ai-models.png"));
        QMetaObject::invokeMethod(models, "close");
        // Everything the analysis produced, into a new folder.
        finished.clear();
        QCOMPARE(s->map.exportAnalysis(QUrl::fromLocalFile(tmp.path())), QString());
        QCOMPARE(finished.count(), 1);
        QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(1).toString()));
        QVERIFY(finished.first().at(1).toString().contains(QStringLiteral("анализ")));

        // Grid, legend and markers from the View menu; a marker with the height under it.
        QVERIFY(s->run(QStringLiteral("view.grid")));
        QVERIFY(s->map.showGrid());
        QVERIFY(s->checked(QStringLiteral("view.grid")));
        QVERIFY(s->run(QStringLiteral("view.legend")));
        QObject *legend = s->window->findChild<QObject *>(QStringLiteral("mapLegend"));
        QVERIFY(legend);
        QTRY_VERIFY(legend->property("visible").toBool());
        QVERIFY(s->run(QStringLiteral("tool.marker")));
        click({576030.5, 5188070.5});
        QCOMPARE(s->map.projectMarkers().size(), 1);
        QCOMPARE(s->map.projectMarkers().first().z, 105.0);
        s->shot(QStringLiteral("ui-map-grid-legend-marker.png"));
        // The dialogs of this release open from their commands.
        for (const auto &c : {std::pair{"project.markers", "markersDialog"}, std::pair{"tool.demEditor", "demEditorDialog"},
                              std::pair{"tool.cloudEditor", "cloudEditorDialog"}, std::pair{"tool.classify", "cloudEditorDialog"},
                              std::pair{"project.settings", "projectSettingsDialog"}, std::pair{"file.saveAs", "saveAsDialog"},
                              std::pair{"project.check", "projectCheckDialog"}}) {
            QVERIFY2(s->run(QString::fromLatin1(c.first)), c.first);
            QObject *d = s->window->findChild<QObject *>(QString::fromLatin1(c.second));
            QVERIFY2(d, c.second);
            QTRY_VERIFY2(d->property("visible").toBool(), c.second);
            if (QString::fromLatin1(c.second) == QLatin1String("projectCheckDialog"))
                QVERIFY(d->property("checkResult").toMap().contains(QStringLiteral("ok")));
            QMetaObject::invokeMethod(d, "close");
            QTRY_VERIFY(!d->property("visible").toBool());
        }
    }
    // A new project: once photos with GPS are imported, the map shows their points (not an empty
    // view); «Все слои» brings the view back to them.
    void newProjectMapShowsThePhotos()
    {
        QTemporaryDir tmp;
        auto s = open();
        QVERIFY(s->projects.createProject(QUrl::fromLocalFile(tmp.path()), QStringLiteral("Карта нового проекта")));
        QVERIFY(!s->map.crsDefined());
        const QString source = tmp.filePath(QStringLiteral("DJI_0001.JPG"));
        QVERIFY(TestImages::writeJpegWithGps(source, 64, 48));
        s->projects.importFiles({QUrl::fromLocalFile(source)});
        QTRY_VERIFY_WITH_TIMEOUT(!s->projects.busy() && s->projects.photoCount() == 1, 10000);
        QTRY_VERIFY(s->map.crsDefined());
        QRectF photos;
        for (const MapLayer &l : s->map.layers())
            if (l.type == MapLayer::Photos) photos = l.vector.extent;
        auto centred = [&] {
            const QPointF c = s->map.center();
            return std::hypot(c.x() - photos.center().x(), c.y() - photos.center().y()) < 1.0 && s->map.resolution() < 5.0;
        };
        QTRY_VERIFY2(centred(), qPrintable(QStringLiteral("centre %1 %2, photos %3 %4, resolution %5")
                                               .arg(s->map.center().x()).arg(s->map.center().y()).arg(photos.center().x())
                                               .arg(photos.center().y()).arg(s->map.resolution())));
        // Pan away, then «Все слои» comes back to the photos.
        s->map.panBy(QPointF(400, 300));
        QVERIFY(!centred());
        s->map.zoomToAll();
        QVERIFY(centred());
    }

    // «Схема съёмки» on the GPS tab: the photos of a project with an aligned job — the errors of
    // GPS against the alignment, a click selects the photo, a click in the placing mode puts a
    // control point with its height from the job's DSM.
    void surveySchemeOnTheGpsTab()
    {
        QTemporaryDir tmp;
        QString project;
        QStringList files;
        {
            auto first = open();
            QVERIFY(first->projects.createProject(QUrl::fromLocalFile(tmp.path()), QStringLiteral("Схема съёмки UI")));
            project = first->projects.projectPath();
            QList<QUrl> urls;
            for (int i = 0; i < 2; ++i) {
                const QString source = tmp.filePath(QStringLiteral("DJI_000%1.JPG").arg(i + 1));
                QVERIFY(TestImages::writeJpegWithGps(source, 64, 48));   // 43°14'N 76°56'E
                urls << QUrl::fromLocalFile(source);
            }
            first->projects.importFiles(urls);
            QTRY_VERIFY_WITH_TIMEOUT(!first->projects.busy() && first->projects.photoCount() == 2, 10000);
            for (const Photo &p : first->projects.photoModel()->photos()) files << QFileInfo(p.file).fileName();
        }
        // An aligned job: the two cameras 3 and 13 m east of their GPS, 40 m above a DSM at 100 m (UTM 43N).
        QPointF gps(76.0 + 56.0 / 60, 43.0 + 14.0 / 60);
        QVERIFY(CoordinateTransform(Crs::wgs84(), Crs::fromEpsg(32643)).transform(gps));
        const double offX = std::round(gps.x()), offY = std::round(gps.y());
        ProcessingJob job;
        job.id = QStringLiteral("20261020-120000");
        job.engine = QStringLiteral("mock");
        job.profile = QStringLiteral("balanced");
        job.state = JobState::Completed;
        job.createdAt = QDateTime::currentDateTimeUtc();
        const QDir jobDir(QDir(project).filePath(QStringLiteral("jobs/") + job.id));
        QVERIFY(QDir().mkpath(jobDir.filePath(QStringLiteral("odm_georeferencing"))));
        QVERIFY(QDir().mkpath(jobDir.filePath(QStringLiteral("opensfm"))));
        QVERIFY(QDir().mkpath(jobDir.filePath(QStringLiteral("odm_dem"))));
        const auto write = [](const QString &path, const QByteArray &data) {
            QFile f(path);
            return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
        };
        QVERIFY(write(jobDir.filePath(QStringLiteral("odm_georeferencing/coords.txt")),
                      QStringLiteral("WGS84 UTM 43N\n%1 %2\n").arg(offX, 0, 'f', 0).arg(offY, 0, 'f', 0).toUtf8()));
        QJsonObject shots;
        for (int i = 0; i < 2; ++i) {
            const double C[3] = {gps.x() - offX + 3 + 10 * i, gps.y() - offY, 140};
            const double t[3] = {-C[0], C[1], C[2]};   // R = diag(1, −1, −1): t = −R·C
            shots.insert(files[i], QJsonObject{{"camera", "c"}, {"rotation", QJsonArray{3.14159265358979323846, 0, 0}}, {"translation", QJsonArray{t[0], t[1], t[2]}}});
        }
        const QJsonObject camera{{"projection_type", "perspective"}, {"width", 64}, {"height", 48}, {"focal", 0.9}};
        QVERIFY(write(jobDir.filePath(QStringLiteral("opensfm/reconstruction.json")),
                      QJsonDocument(QJsonArray{QJsonObject{{"cameras", QJsonObject{{"c", camera}}}, {"shots", shots}}}).toJson()));
        {
            GDALDatasetH ds = GDALCreate(GDALGetDriverByName("GTiff"), QDir::toNativeSeparators(jobDir.filePath(QStringLiteral("odm_dem/dsm.tif"))).toUtf8().constData(),
                                         200, 200, 1, GDT_Float32, nullptr);
            QVERIFY(ds);
            double gt[6] = {offX - 100, 1, 0, offY + 100, 0, -1};
            GDALSetGeoTransform(ds, gt);
            GDALSetProjection(ds, Crs::fromEpsg(32643).wkt().toUtf8().constData());
            std::vector<float> v(200 * 200, 100.f);
            QCOMPARE(GDALRasterIO(GDALGetRasterBand(ds, 1), GF_Write, 0, 0, 200, 200, v.data(), 200, 200, GDT_Float32, 0, 0), CE_None);
            GDALClose(ds);
        }
        QVERIFY(write(jobDir.filePath(QStringLiteral("job.json")), QJsonDocument(job.toJson()).toJson()));

        auto s = open();
        QVERIFY(s->projects.openProject(QUrl::fromLocalFile(project)));
        QVERIFY(s->run(QStringLiteral("view.gps")));
        QCOMPARE(s->prop("activeView").toInt(), 1);
        QQuickItem *view = nullptr;
        QTRY_VERIFY((view = s->findItem(QStringLiteral("surveyView"))) != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(view->property("summary").toMap().value(QStringLiteral("aligned")).toInt() == 2, 15000);
        QCOMPARE(view->property("shownJob").toString(), job.id);
        const QVariantMap summary = view->property("summary").toMap();
        QVERIFY2(std::fabs(summary.value(QStringLiteral("rmseXY")).toDouble() - std::sqrt((9.0 + 169.0) / 2)) < 0.05,
                 qPrintable(summary.value(QStringLiteral("rmseXY")).toString()));
        QCOMPARE(view->property("cameraRows").toList().size(), 2);
        s->shot(QStringLiteral("ui-survey-scheme.png"));
        // A click on the second photo selects it.
        QPointF at;
        QMetaObject::invokeMethod(view, "screenPositionOf", Q_RETURN_ARG(QPointF, at), Q_ARG(int, 1));
        QVERIFY(at.x() >= 0);
        QTest::mouseClick(s->window, Qt::LeftButton, Qt::NoModifier, view->mapToScene(at).toPoint());
        QTRY_COMPARE(s->projects.selectedIndex(), 1);
        // A control point placed on the scheme: the job's CRS for the points, Z from the DSM.
        QString id;
        QMetaObject::invokeMethod(view, "placePointAt", Q_RETURN_ARG(QString, id), Q_ARG(QPointF, at + QPointF(20, 0)));
        QVERIFY(!id.isEmpty());
        QCOMPARE(s->survey.controlPoints().size(), 1);
        QCOMPARE(s->survey.crs().epsg(), 32643);
        QCOMPARE(s->survey.controlPoints().first().z, 100.0);
        QCOMPARE(s->survey.controlPoints().first().name, QStringLiteral("ОП-1"));
        QTRY_COMPARE(view->property("pointRows").toList().size(), 1);
    }

    // Ground control points: the dialog's tabs open from their commands, and in the photo view
    // a click places the selected point where it was clicked.
    void surveyOnThePhotos()
    {
        QTemporaryDir tmp;
        auto s = open();
        QVERIFY(s->projects.createProject(QUrl::fromLocalFile(tmp.path()), QStringLiteral("Опорные точки UI")));
        QImage photo(600, 400, QImage::Format_RGB32);
        photo.fill(QColor(90, 120, 80));
        const QString source = tmp.filePath(QStringLiteral("DJI_0001.JPG"));
        QVERIFY(photo.save(source, "JPG"));
        s->projects.importFiles({QUrl::fromLocalFile(source)});
        QTRY_VERIFY_WITH_TIMEOUT(!s->projects.busy() && s->projects.photoCount() == 1, 10000);
        QVERIFY(!s->enabled(QStringLiteral("project.gcpTag")));   // no points yet
        for (const char *id : {"project.gcp", "project.cameras", "project.calibrations", "project.scaleBars"}) {
            QVERIFY2(s->run(QString::fromLatin1(id)), id);
            QObject *d = s->window->findChild<QObject *>(QStringLiteral("surveyDialog"));
            QVERIFY(d);
            QTRY_VERIFY2(d->property("visible").toBool(), id);
            QMetaObject::invokeMethod(d, "close");
            QTRY_VERIFY(!d->property("visible").toBool());
        }
        QCOMPARE(s->survey.setCrs(QStringLiteral("EPSG:32637")), QString());
        const QString id = s->survey.addPoint(QStringLiteral("ОП-1"), 400000, 6200000, 150);
        QTRY_VERIFY(s->enabled(QStringLiteral("project.gcpTag")));
        QVERIFY(s->run(QStringLiteral("project.gcpTag")));
        QCOMPARE(s->prop("activeView").toInt(), 0);
        QVERIFY(s->checked(QStringLiteral("project.gcpTag")));
        QCOMPARE(s->prop("gcpPoint").toString(), id);
        QObject *bar = s->window->findChild<QObject *>(QStringLiteral("gcpBar"));
        QVERIFY(bar);
        QTRY_VERIFY(bar->property("visible").toBool());
        auto *preview = s->window->findChild<QQuickItem *>(QStringLiteral("photoPreview"));
        QVERIFY(preview);
        QTRY_VERIFY(preview->property("paintedWidth").toReal() > 10);
        const qreal pw = preview->property("paintedWidth").toReal(), ph = preview->property("paintedHeight").toReal();
        const QPointF at = preview->mapToScene(QPointF((preview->width() - pw) / 2 + 0.25 * pw, (preview->height() - ph) / 2 + 0.75 * ph));
        QTest::mouseClick(s->window, Qt::LeftButton, Qt::NoModifier, at.toPoint());
        QTRY_COMPARE(s->survey.controlPoints().first().observations.size(), 1);
        const QPointF n = s->survey.controlPoints().first().observations.first();
        QVERIFY2(std::fabs(n.x() - 0.25) < 0.01 && std::fabs(n.y() - 0.75) < 0.01, qPrintable(QStringLiteral("%1 %2").arg(n.x()).arg(n.y())));
        QTRY_COMPARE(s->prop("gcpMarks").toList().size(), 1);
        s->shot(QStringLiteral("ui-gcp-on-photo.png"));
        QTest::mouseClick(s->window, Qt::RightButton, Qt::NoModifier, at.toPoint());   // right click: removed
        QTRY_COMPARE(s->survey.controlPoints().first().observations.size(), 0);
        QVERIFY(s->run(QStringLiteral("project.gcpTag")));
        QVERIFY(!s->prop("gcpMode").toBool());

        // Split view: the map and the 3D scene side by side, each half of the width.
        QVERIFY(s->run(QStringLiteral("view.split")));
        QVERIFY(s->checked(QStringLiteral("view.split")));
        QCOMPARE(s->prop("activeView").toInt(), 3);
        auto *mapPane = s->window->findChild<QQuickItem *>(QStringLiteral("mapPane"));
        auto *scenePane = s->window->findChild<QQuickItem *>(QStringLiteral("scenePane"));
        QVERIFY(mapPane && scenePane);
        QTRY_VERIFY(mapPane->isVisible() && scenePane->isVisible());
        QVERIFY(qAbs(mapPane->width() - scenePane->width()) <= 2);
        QVERIFY(mapPane->x() + mapPane->width() <= scenePane->x() + 2);
        s->shot(QStringLiteral("ui-split-view.png"));
        QVERIFY(s->run(QStringLiteral("view.split")));
        QTRY_VERIFY(!scenePane->isVisible());
        // Batch processing (needs the engine) and the basemap open their windows.
        s->processing.checkEnvironment();
        QTRY_VERIFY(s->processing.engineAvailable());
        QVERIFY(s->run(QStringLiteral("proc.batch")));
        QObject *batch = s->window->findChild<QObject *>(QStringLiteral("batchDialog"));
        QVERIFY(batch);
        QTRY_VERIFY(batch->property("visible").toBool());
        QMetaObject::invokeMethod(batch, "close");
        QVERIFY(s->run(QStringLiteral("view.basemap")));
        QObject *basemap = s->window->findChild<QObject *>(QStringLiteral("basemapDialog"));
        QVERIFY(basemap);
        QTRY_VERIFY(basemap->property("visible").toBool());
        QVERIFY(basemap->property("providers").toList().size() >= 4);
        QMetaObject::invokeMethod(basemap, "close");

        // Chunks: a new chunk is active and empty (the photo panel shows its photos, the tree its
        // name and the other chunk as a row that activates it); split and merge open their pages.
        QVERIFY(!s->enabled(QStringLiteral("project.chunkMerge")));   // one chunk
        QVERIFY(s->run(QStringLiteral("project.chunkNew")));
        QCOMPARE(s->chunks.count(), 2);
        QCOMPARE(s->chunks.photoModel()->rowCount(), 0);
        QObject *chunksDialog = s->window->findChild<QObject *>(QStringLiteral("chunksDialog"));
        QVERIFY(chunksDialog);
        QTRY_VERIFY(chunksDialog->property("visible").toBool());
        QCOMPARE(chunksDialog->property("tab").toInt(), 0);
        s->shot(QStringLiteral("ui-chunks-list.png"));
        QMetaObject::invokeMethod(chunksDialog, "close");
        QTRY_VERIFY(s->tree.section(QStringLiteral("chunk")) && s->tree.section(QStringLiteral("chunk"))->text() == QStringLiteral("Блок 2"));
        QStandardItem *other = s->tree.section(QStringLiteral("chunkEntry"));
        QVERIFY(other);
        QCOMPARE(other->text(), QStringLiteral("Блок 1"));
        s->call("openTreeItem", QVariantMap{{"kind", "chunkEntry"}, {"payload", other->data(ProjectTreeModel::PayloadRole)}, {"available", true}});
        QCOMPARE(s->chunks.activeName(), QStringLiteral("Блок 1"));
        QCOMPARE(s->chunks.photoModel()->rowCount(), 1);
        QTRY_VERIFY(s->enabled(QStringLiteral("project.chunkMerge")));
        QVERIFY(s->run(QStringLiteral("project.chunkSplit")));
        QTRY_VERIFY(chunksDialog->property("visible").toBool());
        QCOMPARE(chunksDialog->property("tab").toInt(), 1);
        QMetaObject::invokeMethod(chunksDialog, "close");
        QVERIFY(s->run(QStringLiteral("project.chunkMerge")));
        QTRY_VERIFY(chunksDialog->property("visible").toBool());
        QCOMPARE(chunksDialog->property("tab").toInt(), 2);
        QMetaObject::invokeMethod(chunksDialog, "close");
        QVERIFY(s->run(QStringLiteral("project.chunkDuplicate")));
        QCOMPARE(s->chunks.count(), 3);
        QCOMPARE(s->chunks.activeName(), QStringLiteral("Блок 1 (копия)"));
        QCOMPARE(s->chunks.photoModel()->rowCount(), 1);
        QMetaObject::invokeMethod(chunksDialog, "close");
    }

    // The 3D view: commands, tools without a map CRS, and an ODM point cloud opened from the
    // project tree (the job's LAZ is shown through odm_filterpoints/point_cloud.ply). The
    // software renderer cannot draw it: the view says so instead of showing an empty black area.
    void scene3dInTheWindow()
    {
        QTemporaryDir tmp;
        QString project;
        {
            ProjectManager pm;
            QVERIFY(pm.createProject(QUrl::fromLocalFile(tmp.path()), QStringLiteral("Сцена 3D")));
            project = pm.projectPath();
        }
        ProcessingJob job;
        job.id = QStringLiteral("20261015-090000");
        job.engine = QStringLiteral("mock");
        job.profile = QStringLiteral("balanced");
        job.state = JobState::Completed;
        job.createdAt = QDateTime::currentDateTimeUtc();
        job.outputs << Artifact{QStringLiteral("pointcloud"), QStringLiteral("odm_georeferencing/odm_georeferenced_model.laz"), 0, QStringLiteral("LAS 1.4")};
        const QDir jobDir(QDir(project).filePath(QStringLiteral("jobs/") + job.id));
        QVERIFY(QDir().mkpath(jobDir.filePath(QStringLiteral("odm_georeferencing"))));
        QVERIFY(QDir().mkpath(jobDir.filePath(QStringLiteral("odm_filterpoints"))));
        const auto write = [](const QString &path, const QByteArray &data) {
            QFile f(path);
            return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
        };
        QVERIFY(write(jobDir.filePath(QStringLiteral("odm_georeferencing/coords.txt")), "WGS84 UTM 15N\n576705 5188170\n"));
        QVERIFY(write(jobDir.filePath(QStringLiteral("odm_georeferencing/odm_georeferenced_model.laz")), "LASF compressed"));
        QByteArray ply = "ply\nformat ascii 1.0\nelement vertex 101\nproperty float x\nproperty float y\nproperty float z\n"
                         "property uchar red\nproperty uchar green\nproperty uchar blue\nend_header\n";
        for (int i = 0; i < 100; ++i) ply += QByteArray::number(i % 10) + ' ' + QByteArray::number(i / 10) + " 160 200 180 120\n";
        ply += "4.5 4.5 160 255 0 0\n";   // at the centre of the cloud's extent
        QVERIFY(write(jobDir.filePath(QStringLiteral("odm_filterpoints/point_cloud.ply")), ply));
        QVERIFY(write(jobDir.filePath(QStringLiteral("job.json")), QJsonDocument(job.toJson()).toJson()));

        auto s = open();
        QVERIFY(s->projects.openProject(QUrl::fromLocalFile(project)));
        QTRY_VERIFY(!s->tree.result(QStringLiteral("pointcloud")).isEmpty());
        // Ctrl+3 opens the 3D view; with an empty scene it offers the project's results.
        QTest::keyClick(s->window, Qt::Key_3, Qt::ControlModifier);
        QTRY_COMPARE(s->prop("activeView").toInt(), 4);
        QVERIFY(s->checked(QStringLiteral("view.3d")));
        QVERIFY(s->window->findChild<QQuickItem *>(QStringLiteral("view3d")));
        // Navigation and measuring work in 3D without a map CRS.
        QVERIFY(!s->map.crsDefined());
        QVERIFY(s->run(QStringLiteral("tool.ruler")));
        QCOMPARE(s->scene3d.tool(), QStringLiteral("measure"));
        QCOMPARE(s->prop("activeView").toInt(), 4);
        QVERIFY(s->checked(QStringLiteral("tool.ruler")));
        QTest::keyClick(s->window, Qt::Key_R);
        QTRY_COMPARE(s->scene3d.tool(), QStringLiteral("orbit"));
        QVERIFY(s->checked(QStringLiteral("tool.rotate")));
        QTest::keyClick(s->window, Qt::Key_H);
        QTRY_COMPARE(s->scene3d.tool(), QStringLiteral("pan"));
        QTest::keyClick(s->window, Qt::Key_Z);
        QTRY_COMPARE(s->scene3d.tool(), QStringLiteral("zoom"));
        QVERIFY(s->run(QStringLiteral("view.projection")));
        QVERIFY(s->scene3d.orthographic());
        QVERIFY(s->checked(QStringLiteral("view.projection")));
        QVERIFY(s->run(QStringLiteral("view.projection")));
        QVERIFY(!s->scene3d.orthographic());
        QVERIFY(s->run(QStringLiteral("view.axes")));
        QVERIFY(!s->scene3d.showAxes());
        QVERIFY(s->run(QStringLiteral("view.axes")));

        // The point cloud from the tree: context action "Открыть в 3D".
        const QVariantMap result = s->tree.result(QStringLiteral("pointcloud"));
        const QVariantMap item{{"kind", "pointcloud"}, {"title", "Плотное облако точек"}, {"available", true},
                               {"payload", result.value("path")}, {"jobId", job.id}, {"file", job.outputs.first().file}};
        const QVariantList actions = s->call("treeActions", item).toList();
        QCOMPARE(actions.first().toMap().value("id").toString(), QStringLiteral("open3d"));
        s->call("runTreeAction", QStringLiteral("open3d"), item);
        QCOMPARE(s->scene3d.layerCount(), 1);
        QTRY_VERIFY(!s->scene3d.loading());
        const QVariantMap layer = s->scene3d.layersForQml().first().toMap();
        QVERIFY2(layer.value("error").toString().isEmpty(), qPrintable(layer.value("error").toString()));
        QVERIFY2(layer.value("info").toString().contains(QStringLiteral("point_cloud.ply")), qPrintable(layer.value("info").toString()));
        QCOMPARE(s->scene3d.origin().x, 576705.0);
        // Once more: not added twice.
        s->call("runTreeAction", QStringLiteral("open3d"), item);
        QCOMPARE(s->scene3d.layerCount(), 1);
        QVERIFY(s->run(QStringLiteral("view.top")));
        QVERIFY(s->scene3d.pitch() > 89.0f);
        QVERIFY(s->run(QStringLiteral("view.reset")));
        // Picking works on the data even without drawing: the centre of the view hits the cloud.
        QVERIFY(s->scene3d.pickAt(400, 300, 800, 600));
        QVERIFY(s->scene3d.pickText().contains(QStringLiteral("576")));
        s->shot(QStringLiteral("ui-07-3d-software.png"));
        // Closing the project empties the scene.
        s->projects.closeProject();
        QTRY_COMPARE(s->scene3d.layerCount(), 0);
    }

    // Инструменты → Привязка изображения по точкам, as ArcMap: the image beside the map, a link by
    // a click on the image and a click on the map (or first on the image shown on the map), the
    // RMS error and the worst link, a link switched off and typed in, Esc, the GeoTIFF on the map,
    // the links back when the image is opened again.
    void georeferenceAnImageByLinks()
    {
        QTemporaryDir tmp;
        auto s = open();
        QVERIFY(s->projects.createProject(QUrl::fromLocalFile(tmp.path()), QStringLiteral("Привязка UI")));
        QVERIFY(!s->enabled(QStringLiteral("tool.georeference")));   // the map has no CRS yet
        QCOMPARE(s->map.setMapCrs(QStringLiteral("32615")), QString());
        QVERIFY(s->enabled(QStringLiteral("tool.georeference")));
        QVERIFY(!s->enabled(QStringLiteral("tool.georeferencePhoto")));   // no photo selected
        QImage img(400, 300, QImage::Format_RGB32);
        img.fill(QColor(200, 200, 190));
        {
            QPainter p(&img);
            p.fillRect(QRect(30, 30, 20, 20), Qt::red);
            p.fillRect(QRect(350, 30, 20, 20), Qt::blue);
            p.fillRect(QRect(340, 250, 20, 20), Qt::darkGreen);
            p.fillRect(QRect(50, 240, 20, 20), Qt::black);
        }
        QTemporaryDir files;   // the user's image, outside the project
        const QString scan = files.filePath(QStringLiteral("scan.png"));
        QVERIFY(img.save(scan));
        s->window->setProperty("activeView", 3);
        s->map.setView(QPointF(576020, 5188185), 0.2);
        QCOMPARE(s->georef.openPath(scan), QString());
        QTRY_VERIFY(s->georef.active() && !s->georef.loading());
        QVERIFY(s->prop("georefShown").toBool());
        QQuickItem *pane = s->findItem(QStringLiteral("georefPane")), *panel = s->findItem(QStringLiteral("georefPanel")),
                   *image = s->findItem(QStringLiteral("georefImage"));
        QVERIFY(pane && panel && image);
        QTRY_VERIFY(pane->isVisible() && panel->isVisible());
        QCOMPARE(s->map.tool(), QStringLiteral("georef"));
        QCOMPARE(s->georef.placement(), QStringLiteral("в центре карты"));
        QCOMPARE(s->georef.imageWidth(), 400);

        // The truth: 0.1 m pixels from 576000, 5188200.
        auto truth = [](const QPointF &px) { return QPointF(576000 + 0.1 * px.x(), 5188200 - 0.1 * px.y()); };
        auto mapClick = [&](const QPointF &m) {
            const QPointF sc = s->map.toScreen(m);
            s->map.pointerPressed(sc, Qt::LeftButton, {});
            s->map.pointerReleased(sc);
        };
        // The first point "from" by a real click in the image pane, at the red square.
        QTRY_VERIFY(image->width() > 100 && image->property("zoom").toDouble() > 0);
        const double zoom = image->property("zoom").toDouble();
        const QPointF origin = QPointF(200, 150) - QPointF(image->width() / 2, image->height() / 2) / zoom;
        QTest::mouseClick(s->window, Qt::LeftButton, {}, image->mapToScene((QPointF(40, 40) - origin) * zoom).toPoint());
        QTRY_VERIFY(s->georef.pending());
        const QPointF first = *s->georef.pendingPoint();
        QVERIFY2(std::hypot(first.x() - 40, first.y() - 40) < 2 / zoom + 0.5, qPrintable(QStringLiteral("%1 %2").arg(first.x()).arg(first.y())));
        mapClick(truth(first));
        QCOMPARE(s->georef.linkCount(), 1);
        QVERIFY(!s->georef.pending());
        // With one link the image is moved so that the point lands where it was put.
        const QPointF moved = s->georef.displayTransform().map(first);
        QVERIFY(std::hypot(moved.x() - truth(first).x(), moved.y() - truth(first).y()) < 0.05);
        for (const QPointF &px : {QPointF(360, 40), QPointF(350, 260), QPointF(60, 250)}) {
            s->georef.addImagePoint(px.x(), px.y());
            mapClick(truth(px));
        }
        QCOMPARE(s->georef.linkCount(), 4);
        QVERIFY(s->georef.fitted());
        QVERIFY2(s->georef.rmse() < 0.02, qPrintable(s->georef.fitText()));   // map clicks are snapped to screen pixels
        QVERIFY(s->georef.fitText().contains(QStringLiteral("СКО")));
        // A link 3 m off: the worst; switched off, the error is gone; typed in, it agrees.
        s->georef.addImagePoint(200, 150);
        mapClick(truth(QPointF(200, 150)) + QPointF(3, 0));
        QCOMPARE(s->georef.linkCount(), 5);
        QVERIFY(s->georef.rmse() > 0.5);
        QVERIFY(s->georef.links().at(4).toMap().value(QStringLiteral("worst")).toBool());
        s->georef.setLinkEnabled(4, false);
        QVERIFY(s->georef.rmse() < 0.02);
        QCOMPARE(s->georef.setLinkValue(4, QStringLiteral("mx"), QLocale().toString(truth(QPointF(200, 150)).x(), 'f', 3)), QString());
        QCOMPARE(s->georef.setLinkValue(4, QStringLiteral("my"), QLocale().toString(truth(QPointF(200, 150)).y(), 'f', 3)), QString());
        s->georef.setLinkEnabled(4, true);
        QVERIFY2(s->georef.rmse() < 0.02, qPrintable(s->georef.fitText()));
        QVERIFY(!s->georef.setLinkValue(4, QStringLiteral("mx"), QStringLiteral("abc")).isEmpty());
        // Esc forgets a point "from".
        s->georef.addImagePoint(10, 10);
        QVERIFY(s->georef.pending());
        s->map.keyPressed(Qt::Key_Escape);
        QVERIFY(!s->georef.pending());
        // ArcMap's way: the first click on the image shown on the map, the second where it belongs.
        mapClick(truth(QPointF(300, 100)));
        QVERIFY(s->georef.pending());
        const QPointF onMap = *s->georef.pendingPoint();
        QVERIFY2(std::hypot(onMap.x() - 300, onMap.y() - 100) < 1, qPrintable(QStringLiteral("%1 %2").arg(onMap.x()).arg(onMap.y())));
        mapClick(truth(onMap));
        QCOMPARE(s->georef.linkCount(), 6);
        // Projective needs 4, the 2nd order polynomial 6, the 3rd 10.
        s->georef.setMethod(int(georef::Method::Polynomial3));
        QVERIFY(!s->georef.fitted());
        QCOMPARE(s->georef.fitText(), QStringLiteral("Нужно ещё 4 связи"));
        s->georef.setMethod(int(georef::Method::Affine));
        QVERIFY(s->georef.fitted());
        s->shot(QStringLiteral("ui-12-georeference.png"));

        // Transform: a GeoTIFF in the project on the map, where the truth puts the image.
        QSignalSpy finished(&s->georef, &GeorefService::finished);
        const int layers = s->map.layerCount();
        QCOMPARE(s->georef.rectify(0), QString());
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 30000);
        QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(1).toString()));
        const QString out = finished.first().at(2).toString();
        QCOMPARE(QFileInfo(out).fileName(), QStringLiteral("scan_привязка.tif"));
        QCOMPARE(QFileInfo(out).absolutePath(), QDir(s->projects.projectPath()).filePath(QStringLiteral("analysis")));
        QTRY_COMPARE(s->map.layerCount(), layers + 1);
        QVERIFY(!s->map.layerIdForSource(out).isEmpty());
        QVERIFY(QFileInfo::exists(out + QStringLiteral(".georef.json")));
        GDALDatasetH ds = GDALOpen(QDir::toNativeSeparators(out).toUtf8().constData(), GA_ReadOnly);
        QVERIFY(ds);
        double gt[6];
        QCOMPARE(GDALGetGeoTransform(ds, gt), CE_None);
        QVERIFY2(std::abs(gt[0] - 576000) < 0.2 && std::abs(gt[3] - 5188200) < 0.2 && std::abs(gt[1] - 0.1) < 0.002,
                 qPrintable(QStringLiteral("%1 %2 %3").arg(gt[0], 0, 'f', 3).arg(gt[3], 0, 'f', 3).arg(gt[1], 0, 'f', 4)));
        GDALClose(ds);
        QVERIFY(!s->georef.showImage());   // the result shows instead of the preview

        // "Завершить" closes the georeferencing; the links come back with the image.
        QQuickItem *close = s->findItem(QStringLiteral("georefClose"));
        QVERIFY(close);
        QMetaObject::invokeMethod(close, "clicked");
        QTRY_VERIFY(!s->georef.active());
        QVERIFY(!s->prop("georefShown").toBool());
        QVERIFY(s->map.tool() != QLatin1String("georef"));
        QCOMPARE(s->georef.openPath(scan), QString());
        QTRY_VERIFY(s->georef.active() && !s->georef.loading());
        QCOMPARE(s->georef.linkCount(), 6);
        QVERIFY(s->georef.fitted());
        s->georef.close();
    }

    // Инструменты → Привязать выбранный снимок по точкам: a photo of the project starts where it
    // was taken (its EXIF has no altitude and focal length: at its GPS position, sized to the view).
    void georeferenceAProjectPhoto()
    {
        QTemporaryDir tmp, files;
        auto s = open();
        QVERIFY(s->projects.createProject(QUrl::fromLocalFile(tmp.path()), QStringLiteral("Привязка снимка UI")));
        const QString photo = files.filePath(QStringLiteral("DJI_0001.JPG"));
        QVERIFY(TestImages::writeJpegWithGps(photo, 64, 48));
        s->projects.importFiles({QUrl::fromLocalFile(photo)});
        QTRY_VERIFY_WITH_TIMEOUT(!s->projects.busy() && s->projects.photoCount() == 1, 10000);
        QTRY_VERIFY(s->map.crsDefined());
        s->projects.selectPhoto(0);
        QVERIFY(s->enabled(QStringLiteral("tool.georeferencePhoto")));
        QVERIFY(s->run(QStringLiteral("tool.georeferencePhoto")));
        QTRY_VERIFY(s->georef.active() && !s->georef.loading());
        QCOMPARE(s->prop("activeView").toInt(), 3);
        QCOMPARE(s->georef.imageName(), QStringLiteral("DJI_0001.JPG"));
        QCOMPARE(s->georef.imageWidth(), 64);
        QVERIFY2(s->georef.placement().contains(QStringLiteral("GPS")), qPrintable(s->georef.placement()));
        QRectF photos;
        for (const MapLayer &l : s->map.layers())
            if (l.type == MapLayer::Photos) photos = l.vector.extent;
        const QPointF c = s->georef.displayTransform().map(QPointF(32, 24));
        QVERIFY2(std::hypot(c.x() - photos.center().x(), c.y() - photos.center().y()) < 0.5,
                 qPrintable(QStringLiteral("%1 %2 vs %3 %4").arg(c.x()).arg(c.y()).arg(photos.center().x()).arg(photos.center().y())));
        // The links of a project photo are kept by its id.
        s->georef.addImagePoint(10, 10);
        const QPointF sc = s->map.toScreen(c);
        s->map.pointerPressed(sc, Qt::LeftButton, {});
        s->map.pointerReleased(sc);
        QCOMPARE(s->georef.linkCount(), 1);
        const QString id = s->projects.photoModel()->photos().first().id;
        QVERIFY(QFileInfo::exists(QDir(s->projects.projectPath()).filePath(QStringLiteral("georef/%1.json").arg(id))));
        s->georef.close();
        QVERIFY(!s->georef.active());
    }

    // Файл → Импорт LiDAR (PDAL of the bundled ODM: GEOMAP_ODM_HOME, else skipped): the cloud in the
    // project's list, a DSM from it on the map, the list kept in lidar.json.
    void lidarImportOnTheMap()
    {
        QString odmError;
        if (odmtools::root(&odmError).isEmpty() || odmtools::program(QStringLiteral("pdal")).isEmpty())
            QSKIP(qPrintable(QStringLiteral("PDAL из OpenDroneMap не найден: %1").arg(odmError)));
        QTemporaryDir tmp, files;
        auto s = open();
        QVERIFY(s->projects.createProject(QUrl::fromLocalFile(tmp.path()), QStringLiteral("LiDAR UI")));
        // A 30 × 30 m cloud from a text file, through PDAL.
        QByteArray text = "X,Y,Z\n";
        for (int j = 0; j <= 60; ++j)
            for (int i = 0; i <= 60; ++i)
                text += QByteArray::number(576000 + i * 0.5, 'f', 2) + ',' + QByteArray::number(5188000 + j * 0.5, 'f', 2) + ','
                        + QByteArray::number(120 + 0.1 * i, 'f', 2) + '\n';
        QFile csv(files.filePath(QStringLiteral("points.csv")));
        QVERIFY(csv.open(QIODevice::WriteOnly) && csv.write(text) == text.size());
        csv.close();
        const QString las = files.filePath(QStringLiteral("scan.las"));
        const odmtools::Result made = odmtools::pdal({QStringLiteral("translate"), QDir::toNativeSeparators(csv.fileName()),
                                                      QDir::toNativeSeparators(las), QStringLiteral("--writers.las.a_srs=EPSG:32615")});
        QVERIFY2(made.ok(), qPrintable(made.error));

        QVERIFY(s->enabled(QStringLiteral("import.lidar")));
        QVERIFY(s->run(QStringLiteral("import.lidar")));
        QObject *dialog = s->window->findChild<QObject *>(QStringLiteral("lidarDialog"));
        QVERIFY(dialog);
        QTRY_VERIFY(dialog->property("visible").toBool());
        QCOMPARE(s->lidar.importPaths({las}), QString());
        QTRY_VERIFY_WITH_TIMEOUT(s->lidar.count() == 1 && !s->lidar.busy(), 60000);
        const QVariantMap c = s->lidar.clouds().first().toMap();
        QVERIFY2(c.value(QStringLiteral("summary")).toString().contains(QStringLiteral("3 721 точек"))
                     || c.value(QStringLiteral("summary")).toString().contains(QStringLiteral("3 721 точек")),
                 qPrintable(c.value(QStringLiteral("summary")).toString()));
        QVERIFY(c.value(QStringLiteral("crs")).toString().contains(QStringLiteral("32615")));
        QVERIFY(!s->lidar.importPaths({las}).isEmpty());   // already there

        QSignalSpy finished(&s->lidar, &LidarService::finished);
        QCOMPARE(s->lidar.buildSurface(0, QStringLiteral("dsm"), 1.0), QString());
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 60000);
        QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(1).toString()));
        const QString dsm = QDir(s->projects.projectPath()).filePath(QStringLiteral("lidar/scan_dsm.tif"));
        QCOMPARE(QDir::cleanPath(finished.first().at(2).toString()), QDir::cleanPath(dsm));
        QTRY_VERIFY(!s->map.layerIdForSource(dsm).isEmpty());
        s->shot(QStringLiteral("ui-13-lidar.png"));

        // The list comes back with the project.
        LidarService again(&s->map, &s->scene3d);
        again.setProject(s->projects.projectPath());
        QCOMPARE(again.count(), 1);
        QCOMPARE(QDir::cleanPath(again.clouds().first().toMap().value(QStringLiteral("dsm")).toString()), QDir::cleanPath(dsm));
        QMetaObject::invokeMethod(dialog, "close");
    }

    // GEOMAP_UI_PROJECT=<project with processing results>: the real project in the window,
    // screenshots of the views (with GEOMAP_TEST_OUTPUT). Skipped otherwise.
    void realProjectViews()
    {
        const QString project = qEnvironmentVariable("GEOMAP_UI_PROJECT");
        if (project.isEmpty()) QSKIP("GEOMAP_UI_PROJECT is not set");
        auto s = open();
        QVERIFY(s->projects.openProject(QUrl::fromLocalFile(project)));
        QTRY_VERIFY_WITH_TIMEOUT(s->processing.jobCount() > 0, 10000);
        s->run(QStringLiteral("view.photo"));
        s->shot(QStringLiteral("real-01-photos.png"));
        for (const QString &kind : {QStringLiteral("orthophoto"), QStringLiteral("dsm"), QStringLiteral("dtm")}) {
            const QVariantMap result = s->tree.result(kind);
            if (result.isEmpty()) continue;
            s->call("openTreeItem", QVariantMap{{"kind", kind}, {"available", true}, {"payload", result.value("path")},
                                              {"jobId", result.value("jobId")}, {"file", result.value("file")}});
            QTRY_VERIFY_WITH_TIMEOUT([&] {
                for (const MapLayer &l : s->map.layers()) if (l.type == MapLayer::Raster && (l.loading || !l.status.isEmpty())) return false;
                return true;
            }(), 60000);
            s->shot(QStringLiteral("real-02-map-%1.png").arg(kind));
        }
        s->run(QStringLiteral("view.processing"));
        s->processing.checkEnvironment();
        s->window->setProperty("selectedJobId", s->tree.result(QStringLiteral("orthophoto")).value("jobId"));
        QTest::qWait(1500);
        s->shot(QStringLiteral("real-03-processing-results.png"));
        s->window->setProperty("detailTab", 1);
        QTest::qWait(500);
        s->shot(QStringLiteral("real-04-processing-quality.png"));
    }

    void layoutIsSavedAndRestored()
    {
        {
            auto s = open();
            QVERIFY(s->run(QStringLiteral("view.panelProperties")));
            QCOMPARE(s->prop("showRight").toBool(), false);
            s->window->close();   // onClosing saves the layout
        }
        {
            auto s = open();
            QCOMPARE(s->prop("showRight").toBool(), false);
            QVERIFY(s->workspace.contains(QStringLiteral("mainSplit")));
            QVERIFY(s->run(QStringLiteral("view.resetLayout")));
            QCOMPARE(s->prop("showRight").toBool(), true);
            QVERIFY(!s->workspace.contains(QStringLiteral("showRight")));
        }
    }
};

int main(int argc, char *argv[])
{
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
#ifdef Q_OS_WIN
    // The offscreen platform reads fonts from a folder; use the system fonts.
    if (!qEnvironmentVariableIsSet("QT_QPA_FONTDIR")) qputenv("QT_QPA_FONTDIR", qgetenv("SystemRoot") + "\\Fonts");
#endif
    QGuiApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    UiTest test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}

#include "test_ui.moc"
