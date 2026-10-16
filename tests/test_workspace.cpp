#include <QtTest>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSettings>
#include <QTemporaryDir>
#include "AppLog.h"
#include "CommandRegistry.h"
#include "GisEnvironment.h"
#include "MapService.h"
#include "MockEngine.h"
#include "ProcessingService.h"
#include "ProjectManager.h"
#include "ProjectTreeModel.h"
#include "WorkspaceState.h"

// Command registry, shortcuts, layout persistence, operations journal and project tree of the
// professional workspace (no QML; the window itself is covered by GeoMapAIUiTests).
class WorkspaceTest : public QObject
{
    Q_OBJECT
    QTemporaryDir m_settings;

    static QString readSource(const QString &relative)
    {
        QFile f(QStringLiteral(GEOMAP_SOURCE_DIR "/") + relative);
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_settings.isValid());
        QCoreApplication::setOrganizationName(QStringLiteral("GeoMapAI-Tests"));
        QCoreApplication::setApplicationName(QStringLiteral("Workspace"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settings.path());
        GisEnvironment::initialize(QCoreApplication::applicationDirPath());
    }

    void registryIsConsistent()
    {
        CommandRegistry r;
        QVERIFY2(r.problems().isEmpty(), qPrintable(r.problems().join(QLatin1Char('\n'))));
        QVERIFY(r.ids().size() > 100);
        // Menus and toolbar groups of the Professional Edition spec, in its order.
        QStringList menus;
        for (const QVariant &m : r.menus()) menus << m.toMap().value("title").toString().remove(QLatin1Char('&'));
        QCOMPARE(menus, (QStringList{"Файл", "Правка", "Проект", "Обработка", "Вид", "Инструменты", "Анализ", "Справка"}));
        QStringList groups;
        for (const QVariant &g : r.toolbar()) groups << g.toMap().value("group").toString();
        QCOMPARE(groups, (QStringList{"Проекты", "Источники", "История", "Навигация", "Измерения", "Геометрия", "Обработка", "Просмотр", "Дополнительно"}));
        // Every icon exists, every command is documented in docs/UI_COMMANDS.md.
        const QString doc = readSource(QStringLiteral("docs/UI_COMMANDS.md"));
        QVERIFY2(!doc.isEmpty(), "docs/UI_COMMANDS.md missing");
        for (const CommandRegistry::Command &c : r.commands()) {
            if (!c.icon.isEmpty())
                QVERIFY2(QFileInfo::exists(QStringLiteral(GEOMAP_SOURCE_DIR "/qml/icons/%1.svg").arg(c.icon)), qPrintable(c.id));
            QVERIFY2(doc.contains(QStringLiteral("`%1`").arg(c.id)), qPrintable(c.id + QStringLiteral(" not in UI_COMMANDS.md")));
        }
        // Unavailable commands say why, ready ones say what they do.
        QVERIFY(r.unavailableReason(QStringLiteral("proc.tiled")).contains(QStringLiteral("после 1.0")));
        QVERIFY(r.unavailableReason(QStringLiteral("analysis.ai")).isEmpty());
        QVERIFY(r.unavailableReason(QStringLiteral("proc.optimize")).startsWith(QStringLiteral("Не поддерживается выбранным движком")));
        QVERIFY(r.unavailableReason(QStringLiteral("file.save")).isEmpty());
        QVERIFY(r.tooltip(QStringLiteral("file.save")).contains(QStringLiteral("Ctrl+S")));
        QVERIFY(r.tooltip(QStringLiteral("proc.tiled")).contains(QStringLiteral("этап после 1.0")));   // planned: the stage in the tooltip
        QVERIFY(r.tooltip(QStringLiteral("tool.slope")).contains(QStringLiteral("уклон")));
        QVERIFY(!r.isReady(QStringLiteral("unknown.command")));
    }

    void shortcutsOverridePersistAndConflict()
    {
        {
            CommandRegistry r;
            QCOMPARE(r.shortcut(QStringLiteral("file.save")), QStringLiteral("Ctrl+S"));
            QSignalSpy changed(&r, &CommandRegistry::shortcutsChanged);
            QCOMPARE(r.setShortcut(QStringLiteral("file.save"), QStringLiteral("ctrl+shift+s")), QString());
            QCOMPARE(r.shortcut(QStringLiteral("file.save")), QStringLiteral("Ctrl+Shift+S"));
            QCOMPARE(changed.count(), 1);
            // Taken by another command: refused with its name.
            const QString conflict = r.setShortcut(QStringLiteral("file.open"), QStringLiteral("Ctrl+Shift+S"));
            QVERIFY2(conflict.contains(QStringLiteral("Сохранить")), qPrintable(conflict));
            QCOMPARE(r.shortcut(QStringLiteral("file.open")), QStringLiteral("Ctrl+O"));
            QVERIFY(!r.setShortcut(QStringLiteral("file.open"), QStringLiteral("Ctrl+Нечто")).isEmpty());
            QVERIFY(!r.setShortcut(QStringLiteral("proc.tiled"), QStringLiteral("F9")).isEmpty());   // planned
            // Removing a shortcut.
            QCOMPARE(r.setShortcut(QStringLiteral("tool.pan"), QString()), QString());
            QCOMPARE(r.shortcut(QStringLiteral("tool.pan")), QString());
            QCOMPARE(r.keyText(Qt::Key_K, Qt::ControlModifier | Qt::ShiftModifier), QStringLiteral("Ctrl+Shift+K"));
            QCOMPARE(r.keyText(Qt::Key_Control, Qt::ControlModifier), QString());
        }
        {
            CommandRegistry again;   // overrides come back from the settings
            QCOMPARE(again.shortcut(QStringLiteral("file.save")), QStringLiteral("Ctrl+Shift+S"));
            QCOMPARE(again.shortcut(QStringLiteral("tool.pan")), QString());
            bool changed = false;
            for (const QVariant &v : again.shortcutList())
                if (v.toMap().value("id") == QLatin1String("file.save")) changed = v.toMap().value("changed").toBool();
            QVERIFY(changed);
            // Back to the default removes the override.
            QCOMPARE(again.setShortcut(QStringLiteral("file.save"), QStringLiteral("Ctrl+S")), QString());
            again.resetShortcuts();
            QCOMPARE(again.shortcut(QStringLiteral("tool.pan")), QStringLiteral("H"));
        }
        CommandRegistry clean;
        QCOMPARE(clean.shortcut(QStringLiteral("file.save")), QStringLiteral("Ctrl+S"));
        QVERIFY(QSettings().childGroups().contains(QStringLiteral("shortcuts")) == false);
    }

    void workspaceStateRoundTrip()
    {
        WorkspaceState s;
        QVERIFY(!s.contains(QStringLiteral("mainSplit")));
        s.setValue(QStringLiteral("mainSplit"), QByteArray("\x01\x02state", 7));
        s.setValue(QStringLiteral("showLeft"), false);
        WorkspaceState other;
        QCOMPARE(other.value(QStringLiteral("mainSplit")).toByteArray(), QByteArray("\x01\x02state", 7));
        QCOMPARE(other.value(QStringLiteral("showLeft")).toBool(), false);
        QSignalSpy cleared(&other, &WorkspaceState::cleared);
        other.clear();
        QCOMPARE(cleared.count(), 1);
        QVERIFY(!s.contains(QStringLiteral("mainSplit")));
        QCOMPARE(s.value(QStringLiteral("showLeft"), true).toBool(), true);
    }

    void journalKeepsEntriesAndWritesFile()
    {
        QTemporaryDir dir;
        AppLog log(dir.filePath(QStringLiteral("logs")));
        log.add(AppLog::Info, QStringLiteral("Проект"), QStringLiteral("Проект открыт: Тест"));
        log.add(AppLog::Info, QStringLiteral("Проект"), QStringLiteral("Проект открыт: Тест"));   // repeated: kept once
        log.add(AppLog::Error, QStringLiteral("Карта"), QStringLiteral("Не удалось\nсохранить"));
        log.add(AppLog::Info, QStringLiteral("Проект"), QStringLiteral("   "));                  // empty: ignored
        QCOMPARE(log.count(), 2);
        QCOMPARE(log.data(log.index(1), AppLog::LevelRole).toInt(), int(AppLog::Error));
        QCOMPARE(log.lastText(), QStringLiteral("Не удалось\nсохранить"));
        QFile file(log.logFilePath());
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        QCOMPARE(lines.size(), 2);
        QVERIFY(lines[1].contains(QStringLiteral("ERROR [Карта] Не удалось / сохранить")));
        for (int i = 0; i < AppLog::MaxEntries + 5; ++i) log.add(AppLog::Info, QStringLiteral("T"), QString::number(i));
        QCOMPARE(log.count(), AppLog::MaxEntries);
        QCOMPARE(log.data(log.index(log.count() - 1), AppLog::TextRole).toString(), QString::number(AppLog::MaxEntries + 4));
    }

    void projectTreeFollowsTheServices()
    {
        QTemporaryDir tmp;
        ProjectManager projects;
        auto record = std::make_shared<MockEngine::Record>();
        ProcessingService processing(new MockEngine(record));
        connect(&projects, &ProjectManager::projectDirectoryChanged, &processing,
                [&](const QString &directory) { processing.setProject(directory, projects.photoModel()); });
        MapService map;
        connect(&projects, &ProjectManager::projectChanged, &map, [&] {
            if (projects.projectPath() != map.projectDirectory()) map.setProject(projects.projectPath(), projects.photoModel());
        });
        ProjectTreeModel tree(&projects, &processing, &map);
        QCOMPARE(tree.rowCount(), 0);   // no project

        QVERIFY(projects.createProject(QUrl::fromLocalFile(tmp.path()), QStringLiteral("Съёмка")));
        const QString project = projects.projectPath();
        QVERIFY(tree.section(QStringLiteral("project")));
        QCOMPARE(tree.section(QStringLiteral("project"))->data(ProjectTreeModel::TitleRole).toString(), QStringLiteral("Съёмка"));
        QCOMPARE(tree.section(QStringLiteral("photos"))->data(ProjectTreeModel::StatusRole).toString(), QStringLiteral("0 · с GPS 0"));
        QCOMPARE(tree.section(QStringLiteral("gcp"))->data(ProjectTreeModel::StatusRole).toString(), QStringLiteral("маркеров 0"));
        QCOMPARE(tree.section(QStringLiteral("calibrations"))->data(ProjectTreeModel::StatusRole).toString(), QStringLiteral("самокалибровка"));
        QCOMPARE(tree.section(QStringLiteral("orthophoto"))->data(ProjectTreeModel::StatusRole).toString(), QStringLiteral("нет обработки"));
        QTRY_COMPARE(tree.section(QStringLiteral("layers"))->rowCount(), 2);   // photos and drawings

        // A finished job with an orthophoto: written as ProcessingService stores it, then reopened.
        projects.closeProject();
        QCOMPARE(tree.rowCount(), 0);
        ProcessingJob job;
        job.id = QStringLiteral("20261014-120000");
        job.engine = QStringLiteral("mock");
        job.profile = QStringLiteral("fast");
        job.state = JobState::Completed;
        job.createdAt = QDateTime::currentDateTimeUtc();
        job.outputs << Artifact{QStringLiteral("orthophoto"), QStringLiteral("odm_orthophoto/odm_orthophoto.tif"), 1234,
                                QStringLiteral("EPSG:32615, 5,0 см/пикс"), };
        QVERIFY(QDir().mkpath(QDir(project).filePath(QStringLiteral("jobs/") + job.id)));
        QFile jobFile(QDir(project).filePath(QStringLiteral("jobs/%1/job.json").arg(job.id)));
        QVERIFY(jobFile.open(QIODevice::WriteOnly));
        jobFile.write(QJsonDocument(job.toJson()).toJson());
        jobFile.close();
        QVERIFY(projects.openProject(QUrl::fromLocalFile(project)));
        QTRY_COMPARE(tree.section(QStringLiteral("jobs"))->rowCount(), 1);
        QStandardItem *ortho = tree.section(QStringLiteral("orthophoto"));
        QVERIFY(ortho->data(ProjectTreeModel::AvailableRole).toBool());
        QVERIFY(ortho->data(ProjectTreeModel::StatusRole).toString().contains(job.id));
        const QVariantMap result = tree.result(QStringLiteral("orthophoto"));
        QCOMPARE(QDir::fromNativeSeparators(result.value("path").toString()),
                 QDir(project).filePath(QStringLiteral("jobs/20261014-120000/odm_orthophoto/odm_orthophoto.tif")));
        QCOMPARE(result.value("jobId").toString(), job.id);
        QVERIFY(tree.result(QStringLiteral("dsm")).isEmpty());
        QCOMPARE(tree.section(QStringLiteral("dsm"))->data(ProjectTreeModel::StatusRole).toString(), QStringLiteral("не построено"));
        QVERIFY(tree.section(QStringLiteral("jobs"))->child(0)->data(ProjectTreeModel::TitleRole).toString().startsWith(QStringLiteral("Быстрый")));

        // Search keeps matches and their parents.
        ProjectTreeFilter filter;
        filter.setSourceModel(&tree);
        filter.setFilterText(QStringLiteral("ортофото"));
        QCOMPARE(filter.rowCount(), 1);                          // project
        const QModelIndex projectRow = filter.index(0, 0);
        QCOMPARE(filter.rowCount(projectRow), 1);                // chunk
        const QModelIndex chunkRow = filter.index(0, 0, projectRow);
        QCOMPARE(filter.rowCount(chunkRow), 1);                  // orthophoto only
        QCOMPARE(filter.index(0, 0, chunkRow).data(ProjectTreeModel::KindRole).toString(), QStringLiteral("orthophoto"));
        filter.setFilterText(QString());
        QCOMPARE(filter.rowCount(projectRow), 3);                // chunk, layers, jobs
    }
};

QTEST_MAIN(WorkspaceTest)
#include "test_workspace.moc"
