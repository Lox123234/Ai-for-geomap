#include "ProjectManager.h"
#include "ProjectFormat.h"
#include "ThumbnailCache.h"
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QSettings>
#include <QUrl>
#include <cmath>

namespace {
using ProjectFormat::ManifestFile;
// Manifest checkpoints while a long import runs, so a crash loses little.
constexpr qint64 ImportCheckpointMs = 2000;
QString now() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); }
QString normalizedName(QString name)
{
    name = name.trimmed();
    // Prevent traversal, reserved filesystem characters, and trailing Windows dots.
    static const QString invalid = QStringLiteral("<>:\"/\\|?*");
    for (QChar c : invalid) name.replace(c, '_');
    while (name.endsWith('.') || name.endsWith(' ')) name.chop(1);
    if (name == "." || name == "..") name.clear();
    return name.left(80);
}
QJsonObject serialize(const Photo &p)
{
    QJsonObject o{
        {"id", p.id}, {"file", p.file}, {"name", p.name},
        {"sourcePath", p.sourcePath}, {"cameraMake", p.cameraMake},
        {"cameraModel", p.cameraModel}, {"takenAt", p.takenAt},
        {"bytes", double(p.bytes)}, {"width", p.width}, {"height", p.height},
        {"hasGps", p.hasGps}
    };
    if (p.hasGps) {
        o["latitude"] = p.latitude;
        o["longitude"] = p.longitude;
    }
    return o;
}
Photo deserialize(const QJsonObject &o)
{
    Photo p;
    p.id = o.value("id").toString();
    p.file = o.value("file").toString();
    p.name = o.value("name").toString();
    p.sourcePath = o.value("sourcePath").toString();
    p.cameraMake = o.value("cameraMake").toString();
    p.cameraModel = o.value("cameraModel").toString();
    p.takenAt = o.value("takenAt").toString();
    p.bytes = qint64(o.value("bytes").toDouble());
    p.width = o.value("width").toInt();
    p.height = o.value("height").toInt();
    p.hasGps = o.value("hasGps").toBool();
    p.latitude = o.value("latitude").toDouble();
    p.longitude = o.value("longitude").toDouble();
    if (!std::isfinite(p.latitude) || !std::isfinite(p.longitude)
        || std::abs(p.latitude) > 90 || std::abs(p.longitude) > 180) p.hasGps = false;
    return p;
}
} // namespace

ProjectManager::ProjectManager(QObject *parent) : QObject(parent), m_photos(this)
{
    QSettings settings;
    m_recentProjects = settings.value("recentProjects").toStringList();
    m_recentProjects.removeDuplicates();
}

ProjectManager::~ProjectManager()
{
    // Closing the window during an import: keep and record what was copied so far.
    if (m_import) {
        m_import->cancel();
        m_import->wait();
        collectImportResults();
        writeProject();
    }
}

bool ProjectManager::acquireLock(const QString &path, std::unique_ptr<QLockFile> &lock)
{
    if (m_lock && QDir(path).canonicalPath() == QDir(m_projectPath).canonicalPath()) {
        lock = std::move(m_lock);   // reopening the project this process already holds
        return true;
    }
    auto candidate = std::make_unique<QLockFile>(QDir(path).filePath(ProjectFormat::LockFile));
    // Held for the whole session; locks of crashed processes are still detected as stale.
    candidate->setStaleLockTime(0);
    if (candidate->tryLock(0)) {
        lock = std::move(candidate);
        return true;
    }
    if (candidate->error() == QLockFile::LockFailedError) {
        error(QStringLiteral("Проект уже открыт в другом окне GeoMap AI. Закройте его там и повторите."));
        return false;
    }
    // No write access (e.g. read-only media): viewing stays possible, writes report errors.
    lock.reset();
    return true;
}

void ProjectManager::setBusy(bool value)
{
    if (m_busy == value) return;
    m_busy = value;
    emit busyChanged();
}

void ProjectManager::updateStatus(const QString &status)
{
    m_status = status;
    emit statusChanged();
}

void ProjectManager::error(const QString &message)
{
    updateStatus(message);
    emit errorOccurred(message);
}

void ProjectManager::updateRecents(const QString &path)
{
    m_recentProjects.removeAll(path);
    m_recentProjects.prepend(path);
    while (m_recentProjects.size() > 8) m_recentProjects.removeLast();
    QSettings settings;
    settings.setValue("recentProjects", m_recentProjects);
    emit recentProjectsChanged();
}

bool ProjectManager::createProject(const QUrl &parentDirectory, const QString &name)
{
    if (m_busy) return false;
    const QString dirname = normalizedName(name);
    const QString parent = parentDirectory.toLocalFile();
    if (dirname.isEmpty() || !QFileInfo(parent).isDir()) {
        error(QStringLiteral("Выберите существующую папку и укажите название проекта."));
        return false;
    }
    const QString path = QDir(parent).absoluteFilePath(dirname);
    if (QFileInfo::exists(path)) {
        error(QStringLiteral("Папка проекта уже существует: ") + path);
        return false;
    }
    if (!QDir().mkpath(QDir(path).filePath("photos"))) {
        error(QStringLiteral("Не удалось создать папку проекта."));
        return false;
    }
    // Persist first; only then switch away from an existing open project.
    const QString manifestPath = QDir(path).filePath(ManifestFile);
    QSaveFile f(manifestPath);
    const QString created = now();
    const QJsonObject manifest{{"schemaVersion", 1}, {"name", dirname},
                               {"createdAt", created}, {"updatedAt", created},
                               {"photos", QJsonArray{}}};
    if (!f.open(QIODevice::WriteOnly)) {
        error(QStringLiteral("Не удалось создать файл проекта."));
        return false;
    }
    const QByteArray data = QJsonDocument(manifest).toJson(QJsonDocument::Indented);
    if (f.write(data) != data.size()) {
        f.cancelWriting();
        error(QStringLiteral("Недостаточно места для сохранения проекта."));
        return false;
    }
    if (!f.commit()) {
        error(QStringLiteral("Не удалось сохранить новый проект."));
        return false;
    }
    std::unique_ptr<QLockFile> lock;
    if (!acquireLock(path, lock)) return false;
    m_lock = std::move(lock);
    clearImportIssues();
    setProjectPath(QDir(path).absolutePath());
    m_projectName = dirname;
    m_createdAt = created;
    m_photos.resetPhotos({}, m_projectPath);
    m_selectedIndex = -1;
    updateRecents(m_projectPath);
    emit projectChanged(); emit photosChanged(); emit selectionChanged();
    updateStatus(QStringLiteral("Проект создан: ") + dirname);
    return true;
}

bool ProjectManager::openRecent(const QString &directory)
{
    return openProject(QUrl::fromLocalFile(directory));
}

bool ProjectManager::openProject(const QUrl &projectDirectory)
{
    if (m_busy) return false;
    QString path = projectDirectory.toLocalFile();
    QFileInfo selected(path);
    if (selected.isFile() && selected.fileName() == ManifestFile)
        path = selected.absolutePath();
    path = QDir(path).absolutePath();
    QFile file(QDir(path).filePath(ManifestFile));
    if (!file.open(QIODevice::ReadOnly)) {
        error(QStringLiteral("В папке нет файла geomap.json."));
        return false;
    }
    if (file.size() > 64 * 1024 * 1024) {
        error(QStringLiteral("Файл проекта слишком большой."));
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    const QJsonObject root = document.object();
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
        || root.value("schemaVersion").toInt() != 1 || !root.value("photos").isArray()) {
        error(QStringLiteral("Некорректный или неподдерживаемый файл проекта."));
        return false;
    }
    QVector<Photo> list;
    const QJsonArray photos = root.value("photos").toArray();
    list.reserve(photos.size());
    for (const QJsonValue &v : photos) {
        if (!v.isObject()) continue;
        Photo p = deserialize(v.toObject());
        // Only allow files within project/photos; never trust arbitrary JSON paths.
        if (!ProjectFormat::isSafePhotoFile(p.file)) continue;
        if (!QFileInfo::exists(QDir(path).filePath(p.file))) continue;
        list.append(p);
    }
    std::unique_ptr<QLockFile> lock;
    if (!acquireLock(path, lock)) return false;
    m_lock = std::move(lock);
    clearImportIssues();
    setProjectPath(path);
    m_projectName = root.value("name").toString(QFileInfo(path).fileName());
    m_createdAt = root.value("createdAt").toString();
    m_photos.resetPhotos(list, path);
    m_selectedIndex = list.isEmpty() ? -1 : 0;
    updateRecents(path);
    emit projectChanged(); emit photosChanged(); emit selectionChanged();
    updateStatus(m_lock ? QStringLiteral("Проект открыт: ") + m_projectName
                        : QStringLiteral("Проект открыт без блокировки (нет прав на запись в папку): ") + m_projectName);
    return true;
}

bool ProjectManager::writeProject()
{
    if (!hasProject()) return false;
    QJsonArray photos;
    for (const Photo &p : m_photos.photos()) photos.append(serialize(p));
    const QJsonObject root{{"schemaVersion", 1}, {"name", m_projectName},
                           {"createdAt", m_createdAt}, {"updatedAt", now()},
                           {"photos", photos}};
    QSaveFile f(QDir(m_projectPath).filePath(ManifestFile));
    if (!f.open(QIODevice::WriteOnly)) return false;
    const QByteArray data = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (f.write(data) != data.size()) { f.cancelWriting(); return false; }
    return f.commit();
}

bool ProjectManager::saveProject()
{
    if (!hasProject() || m_busy) return false;
    if (!writeProject()) {
        error(QStringLiteral("Не удалось сохранить проект."));
        return false;
    }
    updateStatus(QStringLiteral("Проект сохранён."));
    return true;
}

void ProjectManager::closeProject()
{
    if (m_busy || !hasProject()) return;
    m_lock.reset();
    clearImportIssues();
    setProjectPath({});
    m_projectName.clear(); m_createdAt.clear();
    m_photos.resetPhotos({}, {});
    m_selectedIndex = -1;
    emit projectChanged(); emit photosChanged(); emit selectionChanged();
    updateStatus(QStringLiteral("Проект закрыт."));
}

void ProjectManager::importFiles(const QList<QUrl> &files)
{
    QStringList paths;
    for (const QUrl &url : files) {
        if (url.isLocalFile()) paths.append(url.toLocalFile());
    }
    importPaths(paths);
}

void ProjectManager::importFolder(const QUrl &folder)
{
    if (!folder.isLocalFile() || !hasProject()) return;
    // Canonical root: empty for a missing folder (QDirIterator("") would walk the CWD).
    const QString path = QDir(folder.toLocalFile()).canonicalPath();
    if (path.isEmpty()) {
        error(QStringLiteral("Папка не найдена: ") + QDir::toNativeSeparators(folder.toLocalFile()));
        return;
    }
    // Avoid recursively importing already-copied photos from the active project.
    if (path == QDir(m_projectPath).canonicalPath()
        || path == QDir(QDir(m_projectPath).filePath("photos")).canonicalPath()) {
        error(QStringLiteral("Выберите папку с исходными фотографиями, а не папку проекта."));
        return;
    }
    if (m_busy) return;
    ImportJob::Request request;
    request.folder = path;     // scanned on the worker thread
    startImport(std::move(request));
}

void ProjectManager::importPaths(const QStringList &files)
{
    if (!hasProject()) { error(QStringLiteral("Сначала создайте или откройте проект.")); return; }
    if (m_busy) return;
    if (files.isEmpty()) { updateStatus(QStringLiteral("Нет файлов для импорта.")); return; }
    ImportJob::Request request;
    request.files = files;
    startImport(std::move(request));
}

void ProjectManager::startImport(ImportJob::Request request)
{
    request.projectDir = QDir(m_projectPath).canonicalPath();
    for (const Photo &p : m_photos.photos()) request.knownSources.insert(p.sourcePath);
    m_import = new ImportJob(std::move(request), this);
    connect(m_import, &ImportJob::resultsAvailable, this, &ProjectManager::collectImportResults);
    connect(m_import, &ImportJob::progressChanged, this, [this] {
        if (!m_import) return;
        m_importProgress = m_import->progress();
        emit importProgressChanged();
    });
    connect(m_import, &ImportJob::finished, this, &ProjectManager::finishImport);
    m_importProgress = {};
    m_importIssues.clear();
    emit importIssuesChanged();
    emit importProgressChanged();
    m_lastCheckpoint.start();
    setBusy(true);
    updateStatus(QStringLiteral("Импорт выполняется…"));
    m_import->start();
}

void ProjectManager::collectImportResults()
{
    if (!m_import) return;
    const QVector<Photo> photos = m_import->takeResults();
    if (photos.isEmpty()) return;
    m_photos.append(photos);
    if (m_selectedIndex == -1) {
        m_selectedIndex = 0;
        emit selectionChanged();
    }
    emit photosChanged();
    if (m_lastCheckpoint.elapsed() >= ImportCheckpointMs) {
        writeProject();
        m_lastCheckpoint.restart();
    }
}

void ProjectManager::finishImport()
{
    if (!m_import) return;
    collectImportResults();
    const ImportJob::Progress p = m_import->progress();
    const bool cancelled = m_import->isCancelled();
    const QString failure = m_import->failure();
    m_importIssues = m_import->issues();
    m_import->deleteLater();
    m_import = nullptr;
    m_importProgress = p;
    const bool saved = writeProject();
    setBusy(false);
    emit importProgressChanged();
    emit importIssuesChanged();

    QStringList reasons;
    if (p.unreadable) reasons << QStringLiteral("повреждённых или нечитаемых — %1").arg(p.unreadable);
    if (p.duplicates) reasons << QStringLiteral("уже в проекте — %1").arg(p.duplicates);
    if (p.unsupported) reasons << QStringLiteral("неподдерживаемых — %1").arg(p.unsupported);
    if (p.copyFailed) reasons << QStringLiteral("ошибок копирования — %1").arg(p.copyFailed);
    QString skipped = QStringLiteral("Пропущено: %1").arg(p.skipped());
    if (!reasons.isEmpty()) skipped += QStringLiteral(" (%1)").arg(reasons.join(QStringLiteral(", ")));
    QString summary = cancelled
        ? QStringLiteral("Импорт отменён. Импортировано: %1 из %2.").arg(p.imported).arg(p.total)
        : QStringLiteral("Импортировано: %1. %2.").arg(p.imported).arg(skipped);
    if (cancelled && p.skipped()) summary += QLatin1Char(' ') + skipped + QLatin1Char('.');
    if (p.truncated) summary += QStringLiteral(" В папке больше %1 снимков: импортированы первые %1.").arg(p.total);
    if (!cancelled && p.total == 0 && p.skipped() == 0) summary = QStringLiteral("Нет файлов для импорта.");

    if (!failure.isEmpty()) error(failure);
    else if (!saved) error(QStringLiteral("Файлы импортированы, но не удалось записать geomap.json — проверьте доступ к диску."));
    else updateStatus(summary);
    emit importFinished();
}

void ProjectManager::clearImportIssues()
{
    // The report belongs to the last import of the project that is being left.
    if (m_importIssues.isEmpty()) return;
    m_importIssues.clear();
    emit importIssuesChanged();
}

void ProjectManager::setProjectPath(const QString &path)
{
    m_projectPath = path;
    emit projectDirectoryChanged(path);
}

void ProjectManager::cancelImport()
{
    if (!m_import || m_import->isCancelled()) return;
    m_import->cancel();
    emit importProgressChanged();
}

void ProjectManager::selectPhoto(int index)
{
    if (index < -1 || index >= photoCount() || index == m_selectedIndex) return;
    m_selectedIndex = index;
    emit selectionChanged();
}

bool ProjectManager::removeSelectedPhoto()
{
    if (!hasProject() || m_busy || m_selectedIndex < 0) return false;
    const Photo p = m_photos.at(m_selectedIndex);
    const QString file = QDir(m_projectPath).filePath(p.file);
    // Projects from earlier builds may hold read-only copies; only the copy is touched.
    QFile::setPermissions(file, QFile::permissions(file) | QFileDevice::WriteOwner | QFileDevice::WriteUser);
    if (!QFile::remove(file)) {
        error(QStringLiteral("Не удалось удалить фотографию из проекта."));
        return false;
    }
    QFile::remove(ThumbnailCache::thumbnailPath(m_projectPath, p.file));
    m_photos.erase(m_selectedIndex);
    m_selectedIndex = photoCount() ? qMin(m_selectedIndex, photoCount() - 1) : -1;
    emit selectionChanged(); emit photosChanged();
    if (!writeProject()) {
        error(QStringLiteral("Файл удалён, но сохранение проекта не удалось."));
        return false;
    }
    updateStatus(QStringLiteral("Фотография удалена из проекта."));
    return true;
}

int ProjectManager::gpsCount() const
{
    int count = 0;
    for (const Photo &p : m_photos.photos()) if (p.hasGps) ++count;
    return count;
}

QVariantList ProjectManager::geoPoints() const
{
    QVariantList points;
    for (int i = 0; i < m_photos.photos().size(); ++i) {
        const Photo &p = m_photos.photos().at(i);
        if (p.hasGps) points.push_back(QVariantMap{{"latitude", p.latitude},
            {"longitude", p.longitude}, {"index", i}, {"name", p.name}});
    }
    return points;
}

void ProjectManager::revealProject() const
{
    if (hasProject()) QDesktopServices::openUrl(QUrl::fromLocalFile(m_projectPath));
}

QString ProjectManager::displayPath(const QUrl &url) const
{
    // Decoded native path ("C:\Съёмки"), not "/C:/%D0%A1..." from string-mangling the URL.
    return QDir::toNativeSeparators(url.toLocalFile());
}
