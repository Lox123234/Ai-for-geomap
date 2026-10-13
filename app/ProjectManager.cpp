#include "ProjectManager.h"
#include "ExifReader.h"
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSettings>
#include <QUuid>
#include <QUrl>
#include <QSet>
#include <cmath>

namespace {
constexpr auto ManifestFile = "geomap.json";
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
bool supported(const QString &path)
{
    static const QSet<QString> suffixes = {"jpg", "jpeg", "png", "tif", "tiff"};
    return suffixes.contains(QFileInfo(path).suffix().toLower());
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
    m_projectPath = QDir(path).absolutePath();
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
        const QString prefix = QStringLiteral("photos/");
        if (!p.file.startsWith(prefix) || p.file.mid(prefix.size()).contains('/')
            || p.file.contains('\\') || p.file.contains("..")) continue;
        if (!QFileInfo::exists(QDir(path).filePath(p.file))) continue;
        list.append(p);
    }
    m_projectPath = path;
    m_projectName = root.value("name").toString(QFileInfo(path).fileName());
    m_createdAt = root.value("createdAt").toString();
    m_photos.resetPhotos(list, path);
    m_selectedIndex = list.isEmpty() ? -1 : 0;
    updateRecents(path);
    emit projectChanged(); emit photosChanged(); emit selectionChanged();
    updateStatus(QStringLiteral("Проект открыт: ") + m_projectName);
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
    m_projectPath.clear(); m_projectName.clear(); m_createdAt.clear();
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
    QStringList paths;
    // Entries share the canonical root, so they can be matched against the canonical
    // project prefix. Qt paths always use '/', unlike QDir::separator() on Windows.
    QDirIterator it(path, QDir::Files | QDir::Readable, QDirIterator::Subdirectories);
    const QString currentProjectPrefix = QDir(m_projectPath).canonicalPath() + QLatin1Char('/');
    while (it.hasNext()) {
        const QString file = it.next();
        if (file.startsWith(currentProjectPrefix)) continue;
        if (supported(file)) paths.append(file);
        if (paths.size() >= 10000) break;
    }
    importPaths(paths);
}

void ProjectManager::importPaths(const QStringList &files)
{
    if (!hasProject()) { error(QStringLiteral("Сначала создайте или откройте проект.")); return; }
    if (m_busy) return;
    if (files.isEmpty()) { updateStatus(QStringLiteral("Нет файлов для импорта.")); return; }
    setBusy(true);
    int imported = 0, rejected = 0;
    const QString imagesDir = QDir(m_projectPath).filePath("photos");
    QSet<QString> sources;
    for (const Photo &p : m_photos.photos()) sources.insert(p.sourcePath);
    for (const QString &path : files) {
        const QFileInfo info(path);
        if (!info.isFile() || !supported(path) || sources.contains(info.canonicalFilePath())) {
            ++rejected; continue;
        }
        const ImageMetadata metadata = ExifReader::read(path);
        if (!metadata.valid) { ++rejected; continue; }
        const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString relative = QStringLiteral("photos/") + id + "." + info.suffix().toLower();
        const QString target = QDir(m_projectPath).filePath(relative);
        if (!QFile::copy(path, target)) {
            ++rejected; continue;
        }
        // The copy inherits the read-only attribute of camera/SD-card originals on Windows;
        // the project owns its copy and must be able to delete it.
        QFile::setPermissions(target, QFile::permissions(target) | QFileDevice::WriteOwner | QFileDevice::WriteUser);
        Photo p;
        p.id = id;
        p.file = relative;
        p.name = info.fileName();
        p.sourcePath = info.canonicalFilePath();
        p.cameraMake = metadata.cameraMake;
        p.cameraModel = metadata.cameraModel;
        p.takenAt = metadata.dateTime;
        p.bytes = info.size();
        p.width = metadata.width;
        p.height = metadata.height;
        p.hasGps = metadata.hasGps;
        p.latitude = metadata.latitude;
        p.longitude = metadata.longitude;
        m_photos.append(p);
        sources.insert(p.sourcePath);
        ++imported;
    }
    if (m_selectedIndex == -1 && photoCount() > 0) {
        m_selectedIndex = 0;
        emit selectionChanged();
    }
    const bool saved = writeProject();
    emit photosChanged();
    setBusy(false);
    if (!saved) error(QStringLiteral("Файлы импортированы, но не удалось записать geomap.json — проверьте доступ к диску."));
    else updateStatus(QStringLiteral("Импортировано: %1. Пропущено: %2.").arg(imported).arg(rejected));
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
