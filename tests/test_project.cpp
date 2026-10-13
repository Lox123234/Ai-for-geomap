#include <QtTest>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJSEngine>
#include <QLockFile>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <cmath>
#include "../app/ProjectManager.h"
#include "TestImages.h"

namespace {
QList<QUrl> urls(const QStringList &paths)
{
    QList<QUrl> list;
    for (const QString &path : paths) list.append(QUrl::fromLocalFile(path));
    return list;
}
QUrl dirUrl(const QString &path) { return QUrl::fromLocalFile(path); }
QJsonObject readManifest(const QString &projectDir)
{
    QFile file(QDir(projectDir).filePath("geomap.json"));
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}
bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
// JPEG with GPS, PNG and TIFF without GPS; all with Cyrillic names.
QStringList makeSources(const QString &dir)
{
    if (!QDir().mkpath(dir)) return {};
    const QString jpg = dir + QStringLiteral("/Снимок с GPS.jpg");
    const QString png = dir + QStringLiteral("/Без GPS.png");
    const QString tif = dir + QStringLiteral("/Ортофото.tiff");
    QImage pngImage(20, 10, QImage::Format_RGB32);
    pngImage.fill(Qt::red);
    QImage tifImage(24, 18, QImage::Format_RGB32);
    tifImage.fill(Qt::green);
    if (!TestImages::writeJpegWithGps(jpg) || !pngImage.save(png) || !tifImage.save(tif, "TIFF"))
        return {};
    return {jpg, png, tif};
}
bool copyTree(const QString &from, const QString &to)
{
    QDirIterator it(from, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString file = it.next();
        const QString target = to + file.mid(from.size());
        if (!QDir().mkpath(QFileInfo(target).absolutePath()) || !QFile::copy(file, target)) return false;
    }
    return true;
}
QString photoFile(const ProjectManager &pm) { return pm.selectedPhoto().value("url").toUrl().toLocalFile(); }
// Imports run on a worker thread; results arrive through the GUI-thread event loop.
bool finishImport(ProjectManager &pm)
{
    if (!pm.busy()) return true;
    QSignalSpy finished(&pm, &ProjectManager::importFinished);
    return finished.wait(60000);
}
// Larger JPEGs so an import takes long enough to observe and cancel.
QStringList makeBatch(const QString &dir, int count, int width = 1600, int height = 1200)
{
    if (!QDir().mkpath(dir)) return {};
    QStringList files;
    QImage image(width, height, QImage::Format_RGB32);
    for (int i = 0; i < count; ++i) {
        image.fill(QColor::fromHsv((i * 37) % 360, 160, 200));
        const QString file = QStringLiteral("%1/DJI_%2.JPG").arg(dir).arg(i, 4, 10, QLatin1Char('0'));
        if (!image.save(file, "JPG", 90)) return {};
        files.append(file);
    }
    return files;
}
int fileCount(const QString &dir)
{
    return int(QDir(dir).entryList(QDir::Files).size());
}
} // namespace

// Stand-in for QtQuick.Dialogs FileDialog: exposes selectedFiles as list<url>.
class FakeFileDialog : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QList<QUrl> selectedFiles MEMBER selectedFiles CONSTANT)
public:
    QList<QUrl> selectedFiles;
};

class ProjectManagerTest : public QObject
{
    Q_OBJECT
    QTemporaryDir m_settings;
private slots:
    void initTestCase()
    {
        // Keep recent-project entries out of the real user profile/registry.
        QVERIFY(m_settings.isValid());
        QCoreApplication::setOrganizationName("GeoMapAITests");
        QCoreApplication::setApplicationName("GeoMapAITests");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settings.path());
    }

    void createsEmptyProject()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString parent = tmp.filePath(QStringLiteral("Проекты БПЛА"));
        QVERIFY(QDir().mkpath(parent));
        ProjectManager pm;
        QVERIFY(pm.createProject(dirUrl(parent), QStringLiteral("Съёмка №1")));
        QVERIFY(pm.hasProject());
        QCOMPARE(pm.projectName(), QStringLiteral("Съёмка №1"));
        QCOMPARE(pm.photoCount(), 0);
        QCOMPARE(pm.gpsCount(), 0);
        QCOMPARE(pm.selectedIndex(), -1);
        QVERIFY(pm.selectedPhoto().isEmpty());
        QVERIFY(pm.geoPoints().isEmpty());

        const QString dir = QDir(parent).filePath(QStringLiteral("Съёмка №1"));
        QVERIFY(QFileInfo(dir + "/photos").isDir());
        const QJsonObject manifest = readManifest(dir);
        QCOMPARE(manifest.value("schemaVersion").toInt(), 1);
        QCOMPARE(manifest.value("name").toString(), QStringLiteral("Съёмка №1"));
        QVERIFY(manifest.value("photos").toArray().isEmpty());

        QVERIFY(pm.saveProject());
        pm.closeProject();
        QVERIFY(!pm.hasProject());
        QVERIFY(pm.openProject(dirUrl(dir)));
        QCOMPARE(pm.photoCount(), 0);
        QCOMPARE(pm.selectedIndex(), -1);
        QCOMPARE(pm.recentProjects().value(0), pm.projectPath());
    }

    void rejectsInvalidCreateArguments()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        ProjectManager pm;
        QSignalSpy errors(&pm, &ProjectManager::errorOccurred);
        QVERIFY(!pm.createProject(dirUrl(tmp.path()), "   "));
        QVERIFY(!pm.createProject(dirUrl(tmp.filePath("missing-parent")), "Name"));
        QVERIFY(!pm.createProject(QUrl(), "Name"));
        QVERIFY(QDir().mkpath(tmp.filePath("Existing")));
        QVERIFY(!pm.createProject(dirUrl(tmp.path()), "Existing"));
        QCOMPARE(errors.count(), 4);
        QVERIFY(!pm.hasProject());

        // Path separators and traversal are neutralised; the project stays inside the parent.
        QVERIFY(pm.createProject(dirUrl(tmp.path()), "../evil/name"));
        QCOMPARE(QFileInfo(pm.projectPath()).absolutePath(), QDir(tmp.path()).absolutePath());
    }

    void importsSaveCloseReopen()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QStringList sources = makeSources(tmp.filePath(QStringLiteral("Исходные снимки")));
        QCOMPARE(sources.size(), 3);
        ProjectManager pm;
        QVERIFY(pm.createProject(dirUrl(tmp.path()), QStringLiteral("Проект")));
        const QString dir = pm.projectPath();

        pm.importFiles(urls(sources));
        QVERIFY(finishImport(pm));
        QCOMPARE(pm.photoCount(), 3);
        QCOMPARE(pm.gpsCount(), 1);
        QCOMPARE(pm.selectedIndex(), 0);
        QCOMPARE(pm.geoPoints().size(), 1);
        QCOMPARE(pm.statusMessage(), QStringLiteral("Импортировано: 3. Пропущено: 0."));

        pm.importFiles(urls(sources));                  // same originals again
        QVERIFY(finishImport(pm));
        QCOMPARE(pm.photoCount(), 3);
        QCOMPARE(pm.statusMessage(), QStringLiteral("Импортировано: 0. Пропущено: 3 (уже в проекте — 3)."));
        QCOMPARE(pm.importIssues().size(), 3);

        // The QML delegates rely on these role names.
        const QHash<int, QByteArray> roles = pm.photoModel()->roleNames();
        for (const char *role : {"photoName", "photoUrl", "photoWidth", "photoHeight", "photoHasGps", "photoThumb"})
            QVERIFY2(roles.values().contains(role), role);
        const QModelIndex first = pm.photoModel()->index(0, 0);
        QCOMPARE(first.data(roles.key("photoName")).toString(), QStringLiteral("Снимок с GPS.jpg"));
        const QString thumb = first.data(roles.key("photoThumb")).toUrl().toString();
        QVERIFY2(thumb.startsWith("image://thumbs/") && thumb.endsWith(".jpg"), qUtf8Printable(thumb));
        // Thumbnails are prepared during the import, one per photo.
        QCOMPARE(fileCount(dir + "/cache/thumbs"), 3);

        QVariantMap photo = pm.selectedPhoto();
        QCOMPARE(photo.value("width").toInt(), 16);
        QCOMPARE(photo.value("height").toInt(), 12);
        QVERIFY(photo.value("hasGps").toBool());
        QVERIFY(std::abs(photo.value("latitude").toDouble() - (43 + 14.0 / 60)) < 1e-6);
        QVERIFY(std::abs(photo.value("longitude").toDouble() - (76 + 56.0 / 60)) < 1e-6);
        pm.selectPhoto(2);
        photo = pm.selectedPhoto();
        QCOMPARE(photo.value("name").toString(), QStringLiteral("Ортофото.tiff"));
        QCOMPARE(photo.value("width").toInt(), 24);
        QVERIFY(!photo.value("hasGps").toBool());
        pm.selectPhoto(99);                             // out of range is ignored
        QCOMPARE(pm.selectedIndex(), 2);

        // Copies live in photos/ and the manifest only stores relative paths.
        const QJsonArray manifestPhotos = readManifest(dir).value("photos").toArray();
        QCOMPARE(manifestPhotos.size(), 3);
        for (const QJsonValue &value : manifestPhotos) {
            const QString file = value.toObject().value("file").toString();
            QVERIFY2(file.startsWith("photos/") && !file.contains(':'), qUtf8Printable(file));
            QVERIFY(QFileInfo::exists(QDir(dir).filePath(file)));
        }

        QVERIFY(pm.saveProject());
        pm.closeProject();
        QVERIFY(!pm.hasProject());
        QCOMPARE(pm.photoCount(), 0);
        QVERIFY(pm.selectedPhoto().isEmpty());

        // Opening via the manifest file itself is accepted too.
        QVERIFY(pm.openProject(dirUrl(QDir(dir).filePath("geomap.json"))));
        QCOMPARE(pm.photoCount(), 3);
        QCOMPARE(pm.gpsCount(), 1);
        QCOMPARE(pm.selectedIndex(), 0);
        photo = pm.selectedPhoto();
        QCOMPARE(photo.value("name").toString(), QStringLiteral("Снимок с GPS.jpg"));
        QVERIFY(std::abs(photo.value("latitude").toDouble() - (43 + 14.0 / 60)) < 1e-6);
        QVERIFY(QImageReader(photoFile(pm)).canRead());
    }

    void importsFileDialogSelectionFromQml()
    {
        // Main.qml calls projects.importFiles(selectedFiles); the urls must arrive intact.
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QStringList sources = makeSources(tmp.filePath(QStringLiteral("Исходные снимки")));
        QCOMPARE(sources.size(), 3);
        ProjectManager pm;
        QVERIFY(pm.createProject(dirUrl(tmp.path()), "Survey"));
        FakeFileDialog dialog;
        dialog.selectedFiles = urls(sources);

        QJSEngine engine;
        QJSEngine::setObjectOwnership(&pm, QJSEngine::CppOwnership);
        QJSEngine::setObjectOwnership(&dialog, QJSEngine::CppOwnership);
        engine.globalObject().setProperty("projects", engine.newQObject(&pm));
        engine.globalObject().setProperty("photoDialog", engine.newQObject(&dialog));
        const QJSValue result = engine.evaluate("projects.importFiles(photoDialog.selectedFiles)");
        QVERIFY2(!result.isError(), qUtf8Printable(result.toString()));
        QVERIFY(finishImport(pm));
        QCOMPARE(pm.photoCount(), 3);
    }

    void projectIsPortable()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString sourceDir = tmp.filePath("src");
        const QStringList sources = makeSources(sourceDir);
        QCOMPARE(sources.size(), 3);
        QString original;
        {
            ProjectManager pm;
            QVERIFY(pm.createProject(dirUrl(tmp.path()), "Survey"));
            pm.importFiles(urls(sources));
            QVERIFY(finishImport(pm));
            QCOMPARE(pm.photoCount(), 3);
            original = pm.projectPath();
        }
        // Move to "another computer": new location, originals gone.
        const QString moved = tmp.filePath(QStringLiteral("Другой диск/Перенесённый проект"));
        QVERIFY(copyTree(original, moved));
        QVERIFY(QDir(original).removeRecursively());
        QVERIFY(QDir(sourceDir).removeRecursively());

        ProjectManager pm;
        QVERIFY(pm.openProject(dirUrl(moved)));
        QCOMPARE(pm.photoCount(), 3);
        QCOMPARE(pm.gpsCount(), 1);
        for (int i = 0; i < pm.photoCount(); ++i) {
            pm.selectPhoto(i);
            const QString file = photoFile(pm);
            QVERIFY2(file.startsWith(QDir(moved).absolutePath() + "/photos/"), qUtf8Printable(file));
            QVERIFY(QImageReader(file).canRead());
        }
    }

    void removingPhotoKeepsOriginal()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QStringList sources = makeSources(tmp.filePath("src"));
        QCOMPARE(sources.size(), 3);
        // Originals from cameras/SD cards are often read-only.
        const QFile::Permissions readOnly = QFile::ReadOwner | QFile::ReadUser | QFile::ReadGroup | QFile::ReadOther;
        QVERIFY(QFile::setPermissions(sources[1], readOnly));

        ProjectManager pm;
        QVERIFY(pm.createProject(dirUrl(tmp.path()), "Survey"));
        pm.importFiles(urls(sources));
        QVERIFY(finishImport(pm));
        QCOMPARE(pm.photoCount(), 3);
        const QString thumbs = pm.projectPath() + "/cache/thumbs";
        QCOMPARE(fileCount(thumbs), 3);

        while (pm.photoCount() > 0) {
            pm.selectPhoto(0);
            const QString copy = photoFile(pm);
            QVERIFY(QFileInfo::exists(copy));
            QVERIFY(QFileInfo(copy).isWritable());      // even the copy of the read-only original
            QVERIFY2(pm.removeSelectedPhoto(), qUtf8Printable(pm.statusMessage()));
            QVERIFY(!QFileInfo::exists(copy));
        }
        QCOMPARE(fileCount(thumbs), 0);                 // thumbnails go with their photos
        QCOMPARE(pm.selectedIndex(), -1);
        QVERIFY(!pm.removeSelectedPhoto());             // nothing selected
        for (const QString &source : sources) QVERIFY(QFileInfo::exists(source));
        QVERIFY(readManifest(pm.projectPath()).value("photos").toArray().isEmpty());

        QFile::setPermissions(sources[1], readOnly | QFile::WriteOwner | QFile::WriteUser);
    }

    void rejectsCorruptImages()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QByteArray garbage(2048, '\0');
        for (int i = 0; i < garbage.size(); ++i) garbage[i] = char((i * 97 + 13) & 0xff);
        const QStringList broken = {
            tmp.filePath(QStringLiteral("битый.jpg")), tmp.filePath("empty.png"),
            tmp.filePath("text.tif"), tmp.filePath("fake.jpeg"), tmp.filePath("notes.txt")
        };
        QVERIFY(writeFile(broken[0], garbage));
        QVERIFY(writeFile(broken[1], {}));
        QVERIFY(writeFile(broken[2], "plain text"));
        QVERIFY(writeFile(broken[3], QByteArray::fromHex("ffd8ffe1") + garbage));
        QVERIFY(writeFile(broken[4], "notes"));

        ProjectManager pm;
        QVERIFY(pm.createProject(dirUrl(tmp.path()), "Survey"));
        pm.importFiles(urls(broken + QStringList{tmp.filePath("missing.jpg")}));
        QVERIFY(finishImport(pm));
        QCOMPARE(pm.photoCount(), 0);
        QCOMPARE(pm.statusMessage(),
                 QStringLiteral("Импортировано: 0. Пропущено: 6 (повреждённых или нечитаемых — 5, неподдерживаемых — 1)."));
        QCOMPARE(pm.importIssues().size(), 6);
        QVERIFY(pm.importIssues().join('\n').contains(QStringLiteral("битый.jpg — файл повреждён или не читается")));
        QVERIFY(QDir(pm.projectPath() + "/photos").isEmpty());
        QVERIFY(!pm.busy());

        // Without a project nothing is imported and an error is reported.
        pm.closeProject();
        QSignalSpy errors(&pm, &ProjectManager::errorOccurred);
        pm.importFiles(urls(broken));
        QCOMPARE(errors.count(), 1);
    }

    void rejectsCorruptManifest_data()
    {
        QTest::addColumn<bool>("writeManifest");
        QTest::addColumn<QByteArray>("content");
        QTest::newRow("missing manifest") << false << QByteArray();
        QTest::newRow("empty file") << true << QByteArray();
        QTest::newRow("broken json") << true << QByteArray("{ \"schemaVersion\": 1, \"photos\": [");
        QTest::newRow("binary garbage") << true << QByteArray::fromHex("00ff10ee8899aabb");
        QTest::newRow("array root") << true << QByteArray("[]");
        QTest::newRow("future schema") << true << QByteArray(R"({"schemaVersion": 2, "photos": []})");
        QTest::newRow("photos not array") << true << QByteArray(R"({"schemaVersion": 1, "photos": {}})");
    }
    void rejectsCorruptManifest()
    {
        QFETCH(bool, writeManifest);
        QFETCH(QByteArray, content);
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        ProjectManager pm;
        QVERIFY(pm.createProject(dirUrl(tmp.path()), QStringLiteral("Рабочий")));
        const QString openPath = pm.projectPath();
        const QString broken = tmp.filePath("broken");
        QVERIFY(QDir().mkpath(broken));
        if (writeManifest) QVERIFY(writeFile(broken + "/geomap.json", content));

        QSignalSpy errors(&pm, &ProjectManager::errorOccurred);
        QVERIFY(!pm.openProject(dirUrl(broken)));
        QCOMPARE(errors.count(), 1);
        QCOMPARE(pm.projectPath(), openPath);           // the open project stays usable
        QVERIFY(pm.saveProject());
    }

    void ignoresUnsafeManifestPaths()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString dir = tmp.filePath("project");
        QVERIFY(QDir().mkpath(dir + "/photos/sub"));
        QImage image(8, 8, QImage::Format_RGB32);
        image.fill(Qt::blue);
        for (const QString &path : {dir + "/photos/ok.png", tmp.filePath("outside.png"), dir + "/photos/sub/nested.png"})
            QVERIFY(image.save(path));

        auto photo = [](const QString &file) {
            return QJsonObject{{"id", file}, {"file", file}, {"name", file}, {"width", 8}, {"height", 8}};
        };
        QJsonObject badGps = photo("photos/ok.png");
        badGps["hasGps"] = true;
        badGps["latitude"] = 200.0;
        badGps["longitude"] = 10.0;
        const QJsonArray photos{
            badGps, photo("../outside.png"), photo("photos/../../outside.png"),
            photo(tmp.filePath("outside.png")), photo("photos\\ok.png"),
            photo("photos/sub/nested.png"), photo("photos/missing.png"), QJsonValue(42)
        };
        const QJsonObject root{{"schemaVersion", 1}, {"name", "Unsafe"}, {"photos", photos}};
        QVERIFY(writeFile(dir + "/geomap.json", QJsonDocument(root).toJson()));

        ProjectManager pm;
        QVERIFY(pm.openProject(dirUrl(dir)));
        QCOMPARE(pm.photoCount(), 1);
        QCOMPARE(pm.selectedPhoto().value("name").toString(), QStringLiteral("photos/ok.png"));
        QCOMPARE(pm.gpsCount(), 0);                     // latitude 200° is rejected
    }

    void folderImportSkipsProjectCopies()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString flight = tmp.filePath(QStringLiteral("Полёт"));
        QCOMPARE(makeSources(flight + "/raw").size(), 3);
        QVERIFY(writeFile(flight + "/raw/readme.txt", "ignored"));
        ProjectManager pm;
        QVERIFY(pm.createProject(dirUrl(flight), QStringLiteral("Проект")));

        pm.importFolder(dirUrl(flight + "/raw"));
        QVERIFY(finishImport(pm));
        QCOMPARE(pm.photoCount(), 3);

        // The parent folder also contains the project with its own copies.
        pm.importFolder(dirUrl(flight));
        QVERIFY(finishImport(pm));
        QCOMPARE(pm.photoCount(), 3);
        QCOMPARE(pm.statusMessage(), QStringLiteral("Импортировано: 0. Пропущено: 3 (уже в проекте — 3)."));

        QSignalSpy errors(&pm, &ProjectManager::errorOccurred);
        pm.importFolder(dirUrl(pm.projectPath()));
        pm.importFolder(dirUrl(pm.projectPath() + "/photos"));
        pm.importFolder(dirUrl(tmp.filePath("missing")));
        QCOMPARE(errors.count(), 3);
        QCOMPARE(pm.photoCount(), 3);
    }

    void importKeepsEventLoopResponsive()
    {
        // Stage A acceptance: the GUI thread keeps serving events while 120 photos import.
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QCOMPARE(makeBatch(tmp.filePath("flight"), 120).size(), 120);
        ProjectManager pm;
        QVERIFY(pm.createProject(dirUrl(tmp.path()), "Survey"));
        QSignalSpy progress(&pm, &ProjectManager::importProgressChanged);
        QSignalSpy photos(&pm, &ProjectManager::photosChanged);

        QElapsedTimer clock;
        qint64 lastTick = 0;
        qint64 maxGap = 0;
        QTimer timer;
        timer.setInterval(10);
        QObject::connect(&timer, &QTimer::timeout, &timer, [&] {
            const qint64 now = clock.elapsed();
            maxGap = qMax(maxGap, now - lastTick);
            lastTick = now;
        });
        pm.importFolder(dirUrl(tmp.filePath("flight")));
        QVERIFY(pm.busy());                             // returns at once; work continues
        clock.start();
        timer.start();
        QVERIFY(finishImport(pm));
        timer.stop();
        const qint64 total = clock.elapsed();

        QCOMPARE(pm.photoCount(), 120);
        QCOMPARE(readManifest(pm.projectPath()).value("photos").toArray().size(), 120);
        QVERIFY2(maxGap < 250, qPrintable(QStringLiteral("GUI thread stalled for %1 ms").arg(maxGap)));
        QVERIFY(progress.count() > 2);
        QVERIFY(photos.count() > 1);                    // photos appear while importing
        qInfo("120 photos imported in %lld ms; longest GUI-thread gap %lld ms", total, maxGap);
    }

    void cancelKeepsProjectConsistent()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QCOMPARE(makeBatch(tmp.filePath("flight"), 200).size(), 200);
        ProjectManager pm;
        QVERIFY(pm.createProject(dirUrl(tmp.path()), "Survey"));
        const QString dir = pm.projectPath();
        const auto connection = QObject::connect(&pm, &ProjectManager::importProgressChanged, &pm, [&pm] {
            if (pm.importProcessed() >= 5) pm.cancelImport();
        });
        pm.importFolder(dirUrl(tmp.filePath("flight")));
        QVERIFY(finishImport(pm));
        QObject::disconnect(connection);

        const int imported = pm.photoCount();
        QVERIFY2(imported >= 5 && imported < 200, qPrintable(QString::number(imported)));
        QCOMPARE(pm.statusMessage(), QStringLiteral("Импорт отменён. Импортировано: %1 из 200.").arg(imported));
        // Every copied file is registered; nothing is half-written or orphaned.
        QCOMPARE(fileCount(dir + "/photos"), imported);
        QCOMPARE(fileCount(dir + "/cache/thumbs"), imported);
        QCOMPARE(readManifest(dir).value("photos").toArray().size(), imported);

        // The project stays usable: importing again adds only the rest.
        pm.importFolder(dirUrl(tmp.filePath("flight")));
        QVERIFY(finishImport(pm));
        QCOMPARE(pm.photoCount(), 200);
        QCOMPARE(pm.statusMessage(),
                 QStringLiteral("Импортировано: %1. Пропущено: %2 (уже в проекте — %2).").arg(200 - imported).arg(imported));
    }

    void closingDuringImportKeepsCopiedPhotos()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QCOMPARE(makeBatch(tmp.filePath("flight"), 200).size(), 200);
        QString dir;
        {
            ProjectManager pm;
            QVERIFY(pm.createProject(dirUrl(tmp.path()), "Survey"));
            dir = pm.projectPath();
            pm.importFolder(dirUrl(tmp.filePath("flight")));
            QTRY_VERIFY_WITH_TIMEOUT(pm.photoCount() >= 3, 30000);
        }   // window closed mid-import: cancel, wait, record what was copied
        const int onDisk = fileCount(dir + "/photos");
        QVERIFY2(onDisk >= 3 && onDisk < 200, qPrintable(QString::number(onDisk)));
        QCOMPARE(readManifest(dir).value("photos").toArray().size(), onDisk);
        QVERIFY(!QFileInfo::exists(dir + "/geomap.lock"));
    }

    void projectDirectoryIsAnnouncedBeforeModelReset()
    {
        // Delegates of the reset model request thumbnails at once; the image provider
        // must already know the new project then (blank thumbnails otherwise).
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        ProjectManager pm;
        QVERIFY(pm.createProject(dirUrl(tmp.path()), "Order"));
        const QString dir = pm.projectPath();
        pm.closeProject();

        QStringList events;
        QObject::connect(&pm, &ProjectManager::projectDirectoryChanged, &pm,
                         [&events](const QString &d) { events << "dir:" + d; });
        QObject::connect(pm.photoModel(), &QAbstractItemModel::modelAboutToBeReset, &pm,
                         [&events] { events << "reset"; });
        QVERIFY(pm.openProject(dirUrl(dir)));
        pm.closeProject();
        QCOMPARE(events, QStringList({"dir:" + dir, "reset", "dir:", "reset"}));
    }

    void secondWriterIsLockedOut()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        ProjectManager first;
        ProjectManager second;              // stands in for a second GeoMap AI window
        QVERIFY(first.createProject(dirUrl(tmp.path()), "Shared"));
        const QString dir = first.projectPath();
        QVERIFY(QFileInfo::exists(dir + "/geomap.lock"));

        QSignalSpy errors(&second, &ProjectManager::errorOccurred);
        QVERIFY(!second.openProject(dirUrl(dir)));
        QCOMPARE(errors.count(), 1);
        QVERIFY(errors.at(0).at(0).toString().contains(QStringLiteral("уже открыт")));
        QVERIFY(!second.hasProject());

        QVERIFY(first.openProject(dirUrl(dir)));        // reopening in the owning window works
        first.closeProject();
        QVERIFY(!QFileInfo::exists(dir + "/geomap.lock"));
        QVERIFY(second.openProject(dirUrl(dir)));
    }

    void staleLockOfCrashedProcessIsReplaced()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QString dir;
        {
            ProjectManager pm;
            QVERIFY(pm.createProject(dirUrl(tmp.path()), "Crashed"));
            dir = pm.projectPath();
        }
        // A lock file in QLockFile's own format whose owner process no longer exists.
        QByteArray contents;
        {
            QLockFile probe(tmp.filePath("probe.lock"));
            QVERIFY(probe.tryLock(0));
            QFile file(tmp.filePath("probe.lock"));
            QVERIFY(file.open(QIODevice::ReadOnly));
            contents = file.readAll();
        }
        contents.replace(0, contents.indexOf('\n'), "2147483644");
        QVERIFY(writeFile(dir + "/geomap.lock", contents));

        ProjectManager pm;
        QVERIFY2(pm.openProject(dirUrl(dir)), qUtf8Printable(pm.statusMessage()));
    }
};

QTEST_GUILESS_MAIN(ProjectManagerTest)
#include "test_project.moc"
