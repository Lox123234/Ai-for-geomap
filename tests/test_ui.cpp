#include <QtTest>
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
#include "ThumbnailProvider.h"
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
            QTest::qWaitForWindowExposed(s->window);
            s->window->requestActivate();
            QTest::qWaitForWindowActive(s->window, 2000);
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
        QVERIFY(!s->run(QStringLiteral("view.3d")));
        QVERIFY(s->prop("transientMessage").toString().contains(QStringLiteral("D (0.4)")));
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
