#include "ImportJob.h"
#include "ExifReader.h"
#include "ProjectFormat.h"
#include "ThumbnailCache.h"
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QMutexLocker>
#include <QStorageInfo>
#include <QThread>
#include <QUuid>
#include <utility>

namespace {
// Room for thumbnails and manifest writes on top of the copied photos.
constexpr qint64 FreeSpaceReserve = 64 * 1024 * 1024;
constexpr int MaxIssueLines = 1000;
} // namespace

ImportJob::ImportJob(Request request, QObject *parent)
    : QObject(parent), m_request(std::move(request))
{
}

ImportJob::~ImportJob()
{
    cancel();
    wait();
}

bool ImportJob::isSupportedImage(const QString &path)
{
    static const QSet<QString> suffixes = {"jpg", "jpeg", "png", "tif", "tiff"};
    return suffixes.contains(QFileInfo(path).suffix().toLower());
}

void ImportJob::start()
{
    Q_ASSERT(!m_thread);
    m_thread = QThread::create([this] { run(); });
    m_thread->setParent(this);
    m_thread->setObjectName(QStringLiteral("GeoMapImport"));
    // Queued to the owner's thread, after any pending progress/results events.
    connect(m_thread, &QThread::finished, this, &ImportJob::finished);
    m_thread->start(QThread::LowPriority);
}

void ImportJob::wait()
{
    if (m_thread) m_thread->wait();
}

ImportJob::Progress ImportJob::progress() const
{
    QMutexLocker lock(&m_mutex);
    return m_progress;
}

QVector<Photo> ImportJob::takeResults()
{
    // Reset first: a result appended after the swap below raises a new signal.
    m_resultsPending = false;
    QMutexLocker lock(&m_mutex);
    return std::exchange(m_results, {});
}

QStringList ImportJob::issues() const
{
    QMutexLocker lock(&m_mutex);
    return m_issues;
}

QString ImportJob::failure() const
{
    QMutexLocker lock(&m_mutex);
    return m_failure;
}

void ImportJob::run()
{
    QStringList files = collectCandidates();
    {
        QMutexLocker lock(&m_mutex);
        m_progress.scanning = false;
        m_progress.total = files.size();
    }
    publishProgress(true);
    if (m_cancel || !hasFreeSpace(files)) return;

    QSet<QString> known = m_request.knownSources;
    for (const QString &path : std::as_const(files)) {
        if (m_cancel) break;
        importFile(path, known);
        publishProgress(false);
    }
    {
        QMutexLocker lock(&m_mutex);
        m_progress.currentFile.clear();
    }
    publishProgress(true);
}

QStringList ImportJob::collectCandidates()
{
    QStringList files;
    if (m_request.folder.isEmpty()) {
        for (const QString &file : m_request.files) {
            if (isSupportedImage(file)) files.append(file);
            else addIssue(file, Issue::Unsupported);
        }
        return files;
    }
    {
        QMutexLocker lock(&m_mutex);
        m_progress.scanning = true;
    }
    publishProgress(true);
    // Entries share the canonical root, so the canonical project prefix matches them.
    const QString projectPrefix = m_request.projectDir + QLatin1Char('/');
    QDirIterator it(m_request.folder, QDir::Files | QDir::Readable, QDirIterator::Subdirectories);
    while (it.hasNext() && !m_cancel) {
        const QString file = it.next();
        if (file.startsWith(projectPrefix) || !isSupportedImage(file)) continue;
        if (files.size() >= m_request.maxFiles) {
            QMutexLocker lock(&m_mutex);
            m_progress.truncated = true;
            break;
        }
        files.append(file);
        {
            QMutexLocker lock(&m_mutex);
            m_progress.total = files.size();   // "found so far" while scanning
        }
        publishProgress(false);
    }
    return files;
}

bool ImportJob::hasFreeSpace(const QStringList &files)
{
    qint64 needed = FreeSpaceReserve;
    for (const QString &file : files) {
        const QFileInfo info(file);
        if (!m_request.knownSources.contains(info.canonicalFilePath())) needed += info.size();
    }
    QStorageInfo storage(m_request.projectDir);
    storage.refresh();
    const qint64 available = storage.bytesAvailable();
    if (!storage.isValid() || available < 0 || available >= needed) return true;
    const QLocale locale;
    QMutexLocker lock(&m_mutex);
    m_failure = QStringLiteral("Недостаточно места на диске проекта: нужно %1, свободно %2. Импорт не начат.")
                    .arg(locale.formattedDataSize(needed), locale.formattedDataSize(available));
    return false;
}

void ImportJob::importFile(const QString &path, QSet<QString> &known)
{
    const QFileInfo info(path);
    {
        QMutexLocker lock(&m_mutex);
        m_progress.currentFile = info.fileName();
    }
    const QString canonical = info.canonicalFilePath();
    if (!info.isFile() || canonical.isEmpty()) {
        addIssue(path, Issue::Unreadable);
        return;
    }
    if (known.contains(canonical)) {
        addIssue(path, Issue::Duplicate);
        return;
    }
    const ImageMetadata metadata = ExifReader::read(path);
    if (!metadata.valid) {
        addIssue(path, Issue::Unreadable);
        return;
    }
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString relative = QStringLiteral("%1/%2.%3").arg(ProjectFormat::PhotosDir, id, info.suffix().toLower());
    const QString target = QDir(m_request.projectDir).filePath(relative);
    if (!QFile::copy(path, target)) {
        QFile::remove(target);   // no partial copies
        addIssue(path, Issue::CopyFailed);
        return;
    }
    // The copy inherits the read-only attribute of camera/SD-card originals on Windows;
    // the project owns its copy and must be able to delete it.
    QFile::setPermissions(target, QFile::permissions(target) | QFileDevice::WriteOwner | QFileDevice::WriteUser);
    // Best effort: the image provider rebuilds missing previews on demand.
    ThumbnailCache::load(m_request.projectDir, relative);

    Photo p;
    p.id = id;
    p.file = relative;
    p.name = info.fileName();
    p.sourcePath = canonical;
    p.cameraMake = metadata.cameraMake;
    p.cameraModel = metadata.cameraModel;
    p.takenAt = metadata.dateTime;
    p.bytes = info.size();
    p.width = metadata.width;
    p.height = metadata.height;
    p.hasGps = metadata.hasGps;
    p.latitude = metadata.latitude;
    p.longitude = metadata.longitude;
    known.insert(canonical);
    {
        QMutexLocker lock(&m_mutex);
        m_results.append(p);
        ++m_progress.imported;
        ++m_progress.processed;
    }
    if (!m_resultsPending.exchange(true)) emit resultsAvailable();
}

void ImportJob::addIssue(const QString &path, Issue issue)
{
    QString reason;
    QMutexLocker lock(&m_mutex);
    switch (issue) {
    case Issue::Duplicate:
        ++m_progress.duplicates;
        reason = QStringLiteral("уже есть в проекте");
        break;
    case Issue::Unsupported:
        ++m_progress.unsupported;
        reason = QStringLiteral("формат не поддерживается");
        break;
    case Issue::Unreadable:
        ++m_progress.unreadable;
        reason = QStringLiteral("файл повреждён или не читается");
        break;
    case Issue::CopyFailed:
        ++m_progress.copyFailed;
        reason = QStringLiteral("не удалось скопировать в проект");
        break;
    }
    // Explicitly selected unsupported files are not queued, so they are not "processed".
    if (issue != Issue::Unsupported) ++m_progress.processed;
    if (m_issues.size() < MaxIssueLines)
        m_issues.append(QStringLiteral("%1 — %2").arg(QDir::toNativeSeparators(path), reason));
}

void ImportJob::publishProgress(bool force)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!force && now - m_lastProgress < 100) return;
    m_lastProgress = now;
    emit progressChanged();
}
