#include <QtTest>
#include <QSignalSpy>
#include <QDir>
#include <QJsonDocument>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSettings>
#include <QTemporaryDir>
#include <QtQml/qqmlextensionplugin.h>
#include <gdal.h>
#include <ogr_srs_api.h>
#include "AppInfo.h"
#include "AppLog.h"
#include "CommandRegistry.h"
#include "GisEnvironment.h"
#include "Journal.h"
#include "MapService.h"
#include "MapView.h"
#include "MockEngine.h"
#include "ProcessingService.h"
#include "ProjectManager.h"
#include "ProjectTreeModel.h"
#include "Scene3DService.h"
#include "ThumbnailProvider.h"
#include "View3DItem.h"
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
    MapService map;
    Scene3DService scene3d;
    CommandRegistry commands;
    WorkspaceState workspace;
    AppLog log;
    AppInfo info{QString()};
    ProjectTreeModel tree{&projects, &processing, &map};
    ProjectTreeFilter treeView;
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
        treeView.setSourceModel(&tree);
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
        QVERIFY(!s->run(QStringLiteral("view.basemap")));
        QVERIFY(s->prop("transientMessage").toString().contains(QStringLiteral("C (0.3.x)")));
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
        QCOMPARE(processingMenu->property("count").toInt(), 28);   // 22 commands + 6 separators
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
        int rasters = 0;
        for (const MapLayer &l : s->map.layers()) rasters += l.type == MapLayer::Raster;
        QCOMPARE(rasters, 1);
        QVERIFY(s->map.crsDefined());
        // Context actions of a layer and of a planned section.
        const QVariantList layerActions = s->call("treeActions", QVariantMap{{"kind", "layer"}, {"payload", layer}, {"available", true}}).toList();
        QCOMPARE(layerActions.size(), 3);
        const QVariantList gcpActions = s->call("treeActions", QVariantMap{{"kind", "gcp"}, {"available", false}, {"status", "этап F"}}).toList();
        QCOMPARE(gcpActions.size(), 1);
        QCOMPARE(gcpActions.first().toMap().value("enabled").toBool(), false);
        s->shot(QStringLiteral("ui-04-map-orthophoto.png"));
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
