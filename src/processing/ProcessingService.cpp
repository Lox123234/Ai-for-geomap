#include "ProcessingService.h"
#include "PhotoModel.h"
#include "SurveyControl.h"
#include "TrueOrthophoto.h"
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QSettings>
#include <QStorageInfo>
#include <QThread>
#include <QThreadPool>
#include <QUrl>
#include <algorithm>
#include <cmath>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {
constexpr int RecentLogLines = 200;
const QString JobsDir = QStringLiteral("jobs");
const QString JobFile = QStringLiteral("job.json");
const QString LogFile = QStringLiteral("log.txt");

QString sizeText(qint64 bytes)
{
    if (bytes < 0) return QStringLiteral("неизвестно");
    static const char *units[] = {"Б", "КБ", "МБ", "ГБ", "ТБ"};
    double value = double(bytes);
    int unit = 0;
    while (value >= 1024 && unit < 4) { value /= 1024; ++unit; }
    return unit == 0 ? QStringLiteral("%1 Б").arg(bytes)
                     : QStringLiteral("%1 %2").arg(QLocale(QLocale::Russian).toString(value, 'f', 1), QString::fromUtf8(units[unit]));
}

QString sample(const QStringList &names)
{
    const QString head = names.mid(0, 5).join(QStringLiteral(", "));
    return names.size() > 5 ? head + QStringLiteral(", …") : head;
}

QString kindTitle(const QString &kind)
{
    static const QHash<QString, QString> titles = {
        {QStringLiteral("orthophoto"), QStringLiteral("Ортофотоплан")},
        {QStringLiteral("dsm"), QStringLiteral("Цифровая модель поверхности (DSM)")},
        {QStringLiteral("dtm"), QStringLiteral("Цифровая модель рельефа (DTM)")},
        {QStringLiteral("pointcloud"), QStringLiteral("Облако точек")},
        {QStringLiteral("mesh"), QStringLiteral("Текстурированная модель")},
        {QStringLiteral("report"), QStringLiteral("Отчёт о качестве")},
        {QStringLiteral("tiles"), QStringLiteral("3D-тайлы")},
    };
    return titles.value(kind, kind);
}

// Hard link when the file system allows it (no extra space, same bytes), else a copy.
bool linkOrCopy(const QString &source, const QString &target, bool *copied)
{
    *copied = false;
#ifdef Q_OS_WIN
    if (CreateHardLinkW(QDir::toNativeSeparators(target).toStdWString().c_str(),
                        QDir::toNativeSeparators(source).toStdWString().c_str(), nullptr))
        return true;
#else
    if (::link(QFile::encodeName(source).constData(), QFile::encodeName(target).constData()) == 0)
        return true;
#endif
    *copied = true;
    return QFile::copy(source, target);
}

struct PrepareResult { QString error; int copied = 0; int masks = 0; SurveyControl::JobInputs control; };

// Runs on a worker thread: verifies every input photo and places it into <job>/images.
PrepareResult prepareInput(const QString &project, const QString &jobDirectory, const QStringList &images,
                           const std::atomic_bool &cancel)
{
    PrepareResult result;
    const QDir job(jobDirectory);
    if (!job.mkpath(QStringLiteral("images"))) {
        result.error = QStringLiteral("Не удалось создать папку %1").arg(QDir::toNativeSeparators(job.filePath(QStringLiteral("images"))));
        return result;
    }
    QStringList unreadable;
    for (const QString &file : images) {
        if (cancel) return result;
        const QString source = QDir(project).filePath(file);
        QImageReader reader(source);
        if (!reader.canRead()) {
            unreadable << QFileInfo(file).fileName();
            continue;
        }
        const QString target = job.filePath(QStringLiteral("images/") + QFileInfo(file).fileName());
        if (QFileInfo::exists(target)) continue;
        bool copied = false;
        if (!linkOrCopy(source, target, &copied)) {
            result.error = QStringLiteral("Не удалось поместить снимок %1 в папку задания (диск заполнен или нет доступа).")
                               .arg(QFileInfo(file).fileName());
            return result;
        }
        if (copied) ++result.copied;
        // Its mask (photos/<name>_mask.png: black = not used) goes with it; the engine finds it by name.
        const QString maskName = QFileInfo(file).completeBaseName() + QStringLiteral("_mask.png");
        const QString mask = QDir(project).filePath(QFileInfo(file).path() + QLatin1Char('/') + maskName);
        if (QFileInfo::exists(mask)) {
            const QString maskTarget = job.filePath(QStringLiteral("images/") + maskName);
            QFile::remove(maskTarget);
            bool maskCopied = false;
            if (linkOrCopy(mask, maskTarget, &maskCopied)) ++result.masks;
        }
    }
    if (!unreadable.isEmpty())
        result.error = QStringLiteral("Не читаются снимки (%1): %2. Удалите их из проекта или импортируйте заново.")
                           .arg(unreadable.size()).arg(sample(unreadable));
    // Ground control points and the camera calibration of the project (gcp.json, calibration/).
    if (result.error.isEmpty() && !cancel) {
        result.control = SurveyControl::prepareJob(project, jobDirectory, images);
        if (!result.control.error.isEmpty()) result.error = result.control.error;
    }
    return result;
}
} // namespace

// ---------------------------------------------------------------- JobListModel

JobListModel::JobListModel(const IProcessingEngine *engine, QObject *parent)
    : QAbstractListModel(parent), m_engine(engine)
{
    for (const ProcessingProfile &p : engine->profiles()) m_profileTitles.insert(p.id, p.title);
}

int JobListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_jobs.size());
}

QVariant JobListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_jobs.size()) return {};
    const ProcessingJob &j = m_jobs[index.row()];
    switch (role) {
    case IdRole: return j.id;
    case ChunkRole: return j.chunk;
    case StateRole: return jobStateId(j.state);
    case StateTextRole: return jobStateText(j.state);
    case ActiveRole: return isActive(j.state);
    case ProfileRole: return m_profileTitles.value(j.profile, j.profile);
    case CreatedRole: return j.createdAt;
    case StageRole: return j.stage;
    case StageTitleRole: return j.stage.isEmpty() ? QString() : m_engine->stageTitle(j.stage);
    case StageIndexRole: return j.stageIndex;
    case StageCountRole: return j.stageCount;
    case MessageRole: return j.message;
    case ImageCountRole: return int(j.images.size());
    case OutputCountRole: return int(j.outputs.size());
    case StartedAtRole: return j.startedAt;
    case FinishedAtRole: return j.finishedAt;
    default: return {};
    }
}

QHash<int, QByteArray> JobListModel::roleNames() const
{
    // "jobState", not "state": delegates are Items, which already have a state property.
    return {{IdRole, "jobId"}, {StateRole, "jobState"}, {StateTextRole, "stateText"}, {ActiveRole, "active"},
            {ProfileRole, "profileTitle"}, {CreatedRole, "createdAt"}, {StageRole, "stage"},
            {StageTitleRole, "stageTitle"}, {StageIndexRole, "stageIndex"}, {StageCountRole, "stageCount"},
            {MessageRole, "message"}, {ImageCountRole, "imageCount"}, {OutputCountRole, "outputCount"},
            {StartedAtRole, "startedAt"}, {FinishedAtRole, "finishedAt"}, {ChunkRole, "chunkId"}};
}

void JobListModel::setJobs(const QVector<ProcessingJob> &jobs)
{
    beginResetModel();
    m_jobs = jobs;
    endResetModel();
}

int JobListModel::rowOf(const QString &id) const
{
    for (int i = 0; i < m_jobs.size(); ++i) if (m_jobs[i].id == id) return i;
    return -1;
}

void JobListModel::update(const ProcessingJob &job)
{
    const int row = rowOf(job.id);
    if (row >= 0) {
        m_jobs[row] = job;
        emit dataChanged(index(row), index(row));
        return;
    }
    beginInsertRows({}, 0, 0);
    m_jobs.prepend(job);
    endInsertRows();
}

void JobListModel::remove(const QString &id)
{
    const int row = rowOf(id);
    if (row < 0) return;
    beginRemoveRows({}, row, row);
    m_jobs.removeAt(row);
    endRemoveRows();
}

// ----------------------------------------------------------- ProcessingService

ProcessingService::ProcessingService(IProcessingEngine *engine, QObject *parent)
    : QObject(parent), m_engine(engine), m_model(engine), m_orthoPool(new QThreadPool(this))
{
    m_engine->setParent(this);
    m_orthoPool->setMaxThreadCount(1);
    m_profiles = m_engine->profiles();

    m_logNotify.setSingleShot(true);
    m_logNotify.setInterval(250);
    connect(&m_logNotify, &QTimer::timeout, this, [this] {
        if (m_log.isOpen()) m_log.flush();
        const QStringList ids = m_pendingLogIds;
        m_pendingLogIds.clear();
        for (const QString &id : ids) emit logUpdated(id);
    });
    connect(m_engine, &IProcessingEngine::environmentChecked, this, [this](const EnvironmentReport &report) {
        m_env = report;
        m_checking = false;
        emit environmentChanged();
    });
    connect(m_engine, &IProcessingEngine::installProgress, this, [this](const QString &line) {
        if (line.isEmpty()) return;
        m_installStatus = line;
        emit installChanged();
    });
    connect(m_engine, &IProcessingEngine::installFinished, this, [this](bool ok, const QString &message) {
        m_installing = false;
        m_installStatus = message;
        emit installChanged();
        if (ok) checkEnvironment();
    });
    connect(m_engine, &IProcessingEngine::stageChanged, this, &ProcessingService::onStage);
    connect(m_engine, &IProcessingEngine::logLine, this, &ProcessingService::onLog);
    connect(m_engine, &IProcessingEngine::finished, this, &ProcessingService::onFinished);
}

ProcessingService::~ProcessingService()
{
    if (m_orthoCancel) *m_orthoCancel = true;
    m_orthoPool->waitForDone();
    stopAll(QStringLiteral("Обработка остановлена: приложение закрыто во время выполнения."));
    m_engine->disconnect(this);
}

void ProcessingService::setProject(const QString &directory, const PhotoModel *photos)
{
    const QString dir = directory.isEmpty() ? QString() : QDir(directory).absolutePath();
    m_photos = photos;
    if (dir == m_project) return;
    if (m_orthoCancel) *m_orthoCancel = true;   // its finish is ignored for another project
    stopAll(QStringLiteral("Обработка остановлена: проект закрыт во время выполнения."));
    m_project = dir;
    m_chunk.clear();
    m_chunkPhotos.clear();
    m_firstChunk = true;
    loadJobs();
    m_model.setJobs(m_jobs);
    emit jobsChanged();
}

void ProcessingService::setChunk(const QString &id, const QStringList &photoIds, bool firstChunk)
{
    m_chunk = id;
    m_chunkPhotos = QSet<QString>(photoIds.cbegin(), photoIds.cend());
    m_firstChunk = firstChunk;
}

bool ProcessingService::jobInChunk(const ProcessingJob &job, const QString &chunk, bool firstChunk)
{
    return chunk.isEmpty() || job.chunk == chunk || (job.chunk.isEmpty() && firstChunk);
}

bool ProcessingService::jobInChunk(const ProcessingJob &job) const
{
    return jobInChunk(job, m_chunk, m_firstChunk);
}

void ProcessingService::adoptJobs(const QString &chunk)
{
    if (chunk.isEmpty()) return;
    bool any = false;
    for (ProcessingJob &j : m_jobs)
        if (j.chunk.isEmpty()) {
            j.chunk = chunk;
            saveJob(j);
            m_model.update(j);
            any = true;
        }
    if (any) emit jobsChanged();
}

void ProcessingService::stopAll(const QString &reason)
{
    if (m_prepareThread) {
        if (m_prepareCancel) *m_prepareCancel = true;
        m_prepareThread->wait();
    }
    const QDateTime now = QDateTime::currentDateTime();
    if (!m_current.isEmpty()) {
        const QString id = m_current;
        m_current.clear();
        m_engine->abort(id);
        if (ProcessingJob *job = findJob(id)) {
            job->finishedAt = now;
            job->outputs = m_engine->collectOutputs(jobDirectory(id));
            setState(*job, JobState::Interrupted, reason);
        }
        closeLog(reason);
    }
    const QStringList queued = m_queue;
    m_queue.clear();
    for (const QString &id : queued) {
        if (ProcessingJob *job = findJob(id)) {
            job->finishedAt = now;
            setState(*job, JobState::Cancelled, reason);
        }
    }
    if (!queued.isEmpty()) emit jobsChanged();
}

ProcessingJob ProcessingService::job(const QString &id) const
{
    for (const ProcessingJob &j : m_jobs) if (j.id == id) return j;
    return {};
}

ProcessingJob *ProcessingService::findJob(const QString &id)
{
    for (ProcessingJob &j : m_jobs) if (j.id == id) return &j;
    return nullptr;
}

QString ProcessingService::jobDirectory(const QString &id) const
{
    if (m_project.isEmpty() || id.isEmpty()) return {};
    return QDir(m_project).filePath(JobsDir + QLatin1Char('/') + id);
}

ProcessingProfile ProcessingService::profile(const QString &id, bool *found) const
{
    const ProcessingProfile *p = profilePointer(id);
    if (found) *found = p != nullptr;
    return p ? *p : ProcessingProfile();
}

const ProcessingProfile *ProcessingService::profilePointer(const QString &id) const
{
    for (const ProcessingProfile &p : m_profiles) if (p.id == id) return &p;
    return nullptr;
}


QVariantList ProcessingService::profilesForQml() const
{
    QVariantList list;
    for (const ProcessingProfile &p : m_profiles)
        list << QVariantMap{{"id", p.id}, {"title", p.title}, {"description", p.description},
                            {"arguments", p.arguments.join(QLatin1Char(' '))}};
    return list;
}

void ProcessingService::checkEnvironment()
{
    if (m_checking) return;
    m_checking = true;
    emit environmentChanged();
    m_engine->validateEnvironment();
}

void ProcessingService::installRuntime()
{
    if (m_installing) return;
    m_installing = true;
    m_installStatus = QStringLiteral("Подготовка…");
    emit installChanged();
    m_engine->installRuntime();
}

void ProcessingService::cancelInstall()
{
    if (m_installing) m_engine->cancelInstall();
}

ProcessingService::Check ProcessingService::runPreflight(const QString &profileId, const QString &extraOptions,
                                                         const QVariantMap &options) const
{
    Check c;
    bool found = false;
    const ProcessingProfile p = profile(profileId, &found);
    if (!found) c.errors << QStringLiteral("Неизвестный профиль обработки «%1».").arg(profileId);
    if (m_project.isEmpty() || !m_photos) {
        c.errors << QStringLiteral("Проект не открыт.");
        return c;
    }
    // Environment warnings are shown with the environment, not repeated for every job.
    if (!m_env.checked) c.errors << QStringLiteral("Окружение обработки ещё не проверено.");
    else if (!m_env.available) c.errors << m_env.problems;

    const EngineCapabilities caps = m_engine->capabilities();
    const QDir project(m_project);
    QStringList missing, unsupported;
    for (const Photo &photo : m_photos->photos()) {
        if (!m_chunk.isEmpty() && !m_chunkPhotos.contains(photo.id)) continue;   // another chunk's photo
        const QFileInfo info(project.filePath(photo.file));
        if (!info.isFile() || info.size() == 0) {
            missing << photo.name;
            continue;
        }
        if (!caps.inputSuffixes.contains(info.suffix().toLower())) {
            unsupported << photo.name;
            continue;
        }
        c.images << photo.file;
        c.inputBytes += info.size();
        if (photo.hasGps) ++c.withGps;
    }
    const int count = int(c.images.size());
    if (!missing.isEmpty())
        c.errors << QStringLiteral("Файлы снимков отсутствуют в папке проекта (%1): %2.").arg(missing.size()).arg(sample(missing));
    if (!unsupported.isEmpty())
        c.warnings << QStringLiteral("Формат не поддерживается движком, снимки не войдут в задание (%1): %2.")
                          .arg(unsupported.size()).arg(sample(unsupported));
    if (count < caps.minimumImages)
        c.errors << QStringLiteral("Для реконструкции нужно не меньше %1 снимков с перекрытием; подходящих %2: %3.")
                        .arg(caps.minimumImages).arg(m_chunk.isEmpty() ? QStringLiteral("в проекте") : QStringLiteral("в активном блоке"))
                        .arg(count);
    else if (found)
        c.warnings << m_engine->resourceWarnings(count, p, m_env);
    if (count > 0 && c.withGps == 0)
        c.warnings << QStringLiteral("Ни у одного снимка нет GPS в EXIF: результат не будет иметь географической привязки.");
    else if (c.withGps < count)
        c.warnings << QStringLiteral("Без GPS: %1 из %2 снимков.").arg(count - c.withGps).arg(count);

    c.request.profile = profilePointer(profileId);
    c.request.options = options;
    c.request.extraArguments = QProcess::splitCommand(extraOptions);
    c.request.dataset = {count, c.withGps, c.inputBytes};
    if (found) {
        ProcessingJob probe;
        QString error;
        if (m_engine->createJob(probe, c.request, m_env, &error)) {
            c.arguments = probe.arguments;
            c.notes = probe.notes;
        } else {
            c.errors << error;
        }
    }

    const QStorageInfo storage(m_project);
    c.free = storage.isValid() ? storage.bytesAvailable() : -1;
    c.needed = qint64(double(c.inputBytes) * p.diskFactor) + m_diskReserve;
    if (c.free >= 0 && c.free < c.needed)
        c.errors << QStringLiteral("Недостаточно места на диске проекта: нужно около %1, свободно %2.")
                        .arg(sizeText(c.needed), sizeText(c.free));
    return c;
}

QVariantMap ProcessingService::preflight(const QString &profileId, const QString &extraOptions,
                                         const QVariantMap &options) const
{
    const Check c = runPreflight(profileId, extraOptions, options);
    return {{"ok", c.errors.isEmpty()}, {"errors", c.errors}, {"warnings", c.warnings}, {"notes", c.notes},
            {"images", int(c.images.size())}, {"withGps", c.withGps},
            {"inputBytes", double(c.inputBytes)}, {"neededBytes", double(c.needed)}, {"freeBytes", double(c.free)},
            {"inputText", sizeText(c.inputBytes)}, {"neededText", sizeText(c.needed)}, {"freeText", sizeText(c.free)},
            {"arguments", c.arguments.join(QLatin1Char(' '))}};
}

QString ProcessingService::newJobId() const
{
    const QString base = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    QString id = base;
    for (int n = 2; QFileInfo::exists(jobDirectory(id)) || !job(id).id.isEmpty(); ++n)
        id = base + QLatin1Char('-') + QString::number(n);
    return id;
}

QString ProcessingService::startJob(const QString &profileId, const QString &extraOptions, const QVariantMap &options)
{
    const Check c = runPreflight(profileId, extraOptions, options);
    if (!c.errors.isEmpty()) {
        emit errorOccurred(c.errors.join(QLatin1Char('\n')));
        return {};
    }
    ProcessingJob job;
    job.id = newJobId();
    job.profile = profileId;
    job.images = c.images;
    job.chunk = m_chunk;
    job.createdAt = QDateTime::currentDateTime();
    QString error;
    if (!m_engine->createJob(job, c.request, m_env, &error)) {
        emit errorOccurred(error);
        return {};
    }
    job.state = JobState::Queued;
    job.message = QStringLiteral("Ожидает запуска.");
    if (!QDir().mkpath(jobDirectory(job.id)) || !saveJob(job)) {
        emit errorOccurred(QStringLiteral("Не удалось создать папку задания %1").arg(QDir::toNativeSeparators(jobDirectory(job.id))));
        return {};
    }
    m_jobs.prepend(job);
    m_queue.append(job.id);
    m_model.update(job);
    emit jobsChanged();
    runNext();
    return job.id;
}

void ProcessingService::runNext()
{
    while (m_current.isEmpty() && !m_queue.isEmpty()) {
        const QString id = m_queue.takeFirst();
        ProcessingJob *job = findJob(id);
        if (!job) continue;
        m_current = id;
        openLog(*job);
        setState(*job, JobState::Preparing, QStringLiteral("Проверка снимков и подготовка папки задания…"));

        auto cancel = std::make_shared<std::atomic_bool>(false);
        m_prepareCancel = cancel;
        const QString project = m_project, directory = jobDirectory(id);
        const QStringList images = job->images;
        QThread *thread = QThread::create([this, id, project, directory, images, cancel] {
            const PrepareResult result = prepareInput(project, directory, images, *cancel);
            QStringList notes;
            const SurveyControl::JobInputs &c = result.control;
            if (c.gcpUsed)
                notes << QStringLiteral("[GeoMap] Опорные точки: %1 (gcp_list.txt, привязка по ним вместо GPS снимков)").arg(c.points);
            else if (c.points > 0 || !c.dropped.isEmpty())
                notes << QStringLiteral("[GeoMap] Опорные точки не используются: нужно не меньше 3 точек, каждая отмечена на 3 снимках "
                                        "задания (сейчас таких %1)").arg(c.points);
            if (!c.dropped.isEmpty())
                notes << QStringLiteral("[GeoMap] Отмечены меньше чем на 3 снимках задания и пропущены: %1").arg(c.dropped.join(QStringLiteral(", ")));
            if (!c.calibration.isEmpty())
                notes << QStringLiteral("[GeoMap] Калибровка камеры: начальные значения из задания %1 (geomap_cameras.json)")
                             .arg(c.calibration == QLatin1String("?") ? QStringLiteral("проекта") : c.calibration);
            QMetaObject::invokeMethod(this, [this, id, result, notes] { prepareFinished(id, result.error, result.copied, result.masks, notes); },
                                      Qt::QueuedConnection);
        });
        connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        m_prepareThread = thread;
        thread->start();
    }
}

void ProcessingService::prepareFinished(const QString &id, const QString &error, int copied, int masks, const QStringList &notes)
{
    if (id != m_current) return;   // stopped meanwhile
    ProcessingJob *job = findJob(id);
    if (!job) {
        m_current.clear();
        runNext();
        return;
    }
    auto finish = [this, id](JobState state, const QString &message) {
        ProcessingJob *j = findJob(id);
        m_current.clear();
        j->finishedAt = QDateTime::currentDateTime();
        setState(*j, state, message);
        closeLog(message);
        emit jobFinished(id);
        runNext();
    };
    if (m_prepareCancel && *m_prepareCancel) {
        finish(JobState::Cancelled, QStringLiteral("Обработка отменена до запуска движка."));
        return;
    }
    if (!error.isEmpty()) {
        finish(JobState::Failed, error);
        return;
    }
    writeLog(QStringLiteral("[GeoMap] Снимки подготовлены: %1%2").arg(job->images.size())
                 .arg(copied > 0 ? QStringLiteral(" (скопировано %1: жёсткие ссылки на этом диске недоступны)").arg(copied)
                                 : QString()));
    if (masks > 0) writeLog(QStringLiteral("[GeoMap] Маски снимков: %1 (чёрные области не используются в реконструкции)").arg(masks));
    for (const QString &note : notes) writeLog(note);
    job->startedAt = QDateTime::currentDateTime();   // of this run
    m_resumed.removeAll(id);
    ++job->runs;
    setState(*job, JobState::Running, job->runs > 1 ? QStringLiteral("Продолжение обработки (запуск %1).").arg(job->runs)
                                                    : QStringLiteral("Запущен %1.").arg(engineName()));
    QString startError;
    // A start failure may also arrive through finished(); onFinished() ignores it once m_current moved on.
    if (!m_engine->start(*job, jobDirectory(id), &startError) && m_current == id)
        finish(JobState::Failed, startError);
}

void ProcessingService::cancelJob(const QString &id)
{
    ProcessingJob *job = findJob(id);
    if (!job) return;
    if (m_queue.removeAll(id) > 0) {
        job->finishedAt = QDateTime::currentDateTime();
        setState(*job, JobState::Cancelled, QStringLiteral("Отменено до запуска."));
        return;
    }
    if (id != m_current) return;
    if (job->state == JobState::Preparing) {
        if (m_prepareCancel) *m_prepareCancel = true;
        setState(*job, JobState::Cancelling, QStringLiteral("Отмена подготовки…"));
    } else if (job->state == JobState::Running) {
        setState(*job, JobState::Cancelling, QStringLiteral("Остановка обработки…"));
        writeLog(QStringLiteral("[GeoMap] Отмена по запросу пользователя."));
        m_engine->cancel(id);
    }
}

bool ProcessingService::resumeJob(const QString &id, const QString &profileId, const QVariantMap &options,
                                  const QString &extraOptions)
{
    ProcessingJob *job = findJob(id);
    auto fail = [this](const QString &message) {
        emit errorOccurred(message);
        return false;
    };
    if (!job || isActive(job->state) || id == m_current) return false;
    if (!m_env.available) return fail(QStringLiteral("Движок обработки недоступен: проверьте окружение."));
    if (!job->backend.isEmpty() && job->runs > 0 && job->backend != m_env.backend)
        return fail(QStringLiteral("Задание выполнялось в Docker (GeoMap AI 0.2.0). Docker больше не используется, а файлы "
                                   "промежуточных этапов содержат его пути: запустите новую обработку."));
    for (const QString &file : job->images)
        if (!QFileInfo::exists(QDir(m_project).filePath(file)) && !QFileInfo::exists(QDir(jobDirectory(id)).filePath(
                                                                       QStringLiteral("images/") + QFileInfo(file).fileName())))
            return fail(QStringLiteral("Снимок %1 больше не найден ни в проекте, ни в папке задания.").arg(QFileInfo(file).fileName()));
    // A run that failed or was stopped resumes by computing its last stage again: what that stage
    // left on disk may be half written (ODM would skip it and fail on the next one).
    const bool redo = job->state == JobState::Failed || job->state == JobState::Cancelled || job->state == JobState::Interrupted;
    m_failedStage[id] = redo ? job->stage : QString();
    m_autoRetries[id] = 0;
    if (profileId.isEmpty()) {
        job->arguments = m_engine->resumeArguments(job->arguments, redo ? job->stage : QString());
    } else {
        const ProcessingProfile *p = profilePointer(profileId);
        if (!p) return fail(QStringLiteral("Неизвестный профиль обработки «%1».").arg(profileId));
        int withGps = 0;
        if (m_photos)
            for (const Photo &photo : m_photos->photos()) if (photo.hasGps && job->images.contains(photo.file)) ++withGps;
        JobRequest request;
        request.profile = p;
        request.options = options;
        request.extraArguments = QProcess::splitCommand(extraOptions);
        request.dataset = {int(job->images.size()), withGps, 0};
        ProcessingJob copy = *job;
        QString error;
        if (!m_engine->createJob(copy, request, m_env, &error)) return fail(error);
        job->arguments = copy.arguments;
        job->notes = copy.notes;
        job->profile = profileId;
    }
    if (job->backend.isEmpty() || job->runs == 0) job->backend = m_env.backend;
    job->exitCode = 0;
    job->finishedAt = {};
    m_resumed << id;
    m_queue.append(id);
    setState(*job, JobState::Queued, QStringLiteral("Ожидает продолжения."));
    runNext();
    return true;
}

QString ProcessingService::runStep(const QString &id, const QString &step)
{
    ProcessingJob *job = findJob(id);
    if (!job) return QStringLiteral("Задание не найдено");
    if (isActive(job->state) || id == m_current) return QStringLiteral("Задание уже выполняется");
    if (job->state != JobState::Completed) return QStringLiteral("Задание не завершено — сначала продолжите его");
    if (!m_env.available) {
        if (!m_env.checked && !m_checking) checkEnvironment();
        return m_env.checked ? QStringLiteral("Движок обработки недоступен: проверьте окружение")
                             : QStringLiteral("Идёт проверка движка — повторите через несколько секунд");
    }
    QString error;
    const QStringList args = m_engine->stepArguments(step, *job, &error);
    if (args.isEmpty()) return error;
    job->arguments = args;
    m_failedStage[id] = QString();
    m_autoRetries[id] = 0;
    job->exitCode = 0;
    job->finishedAt = {};
    m_resumed << id;
    m_queue.append(id);
    setState(*job, JobState::Queued, step == QLatin1String("tiles") ? QStringLiteral("Ожидает: 3D-тайлы")
                                                                    : QStringLiteral("Ожидает: карты глубины и плотное облако"));
    runNext();
    return {};
}

void ProcessingService::refreshOutputs(const QString &id)
{
    ProcessingJob *job = findJob(id);
    if (!job || isActive(job->state)) return;
    job->outputs = m_engine->collectOutputs(jobDirectory(id));
    setState(*job, job->state, job->message);
}

QString ProcessingService::newestJob(const QString &kind) const
{
    for (const ProcessingJob &j : m_jobs) {   // newest first
        if (j.state != JobState::Completed || !jobInChunk(j)) continue;
        const QDir dir(jobDirectory(j.id));
        const int end = int(j.arguments.indexOf(QStringLiteral("--end-with")));
        const QString endWith = end >= 0 && end + 1 < j.arguments.size() ? j.arguments[end + 1] : QString();
        if (kind == QLatin1String("aligned") && QFileInfo::exists(dir.filePath(QStringLiteral("opensfm/reconstruction.json")))) return j.id;
        if (kind == QLatin1String("alignedOnly") && endWith == QLatin1String("opensfm")) return j.id;
        if (kind == QLatin1String("model") && endWith.isEmpty()
            && (QFileInfo::exists(dir.filePath(QStringLiteral("odm_texturing/odm_textured_model_geo.obj")))
                || QFileInfo::exists(dir.filePath(QStringLiteral("odm_texturing_25d/odm_textured_model_geo.obj")))))
            return j.id;
    }
    return {};
}

QString ProcessingService::sparseCloud(const QString &jobId) const
{
    const QString id = jobId.isEmpty() ? newestJob(QStringLiteral("aligned")) : jobId;
    if (id.isEmpty()) return {};
    const QDir dir(jobDirectory(id));
    // ODM writes it with the fast orthophoto only; else GeoMap AI writes the points of the
    // reconstruction (the same coordinates: local to odm_georeferencing/coords.txt).
    const QString odm = dir.filePath(QStringLiteral("opensfm/reconstruction.ply"));
    if (QFileInfo::exists(odm)) return odm;
    const QString json = dir.filePath(QStringLiteral("opensfm/reconstruction.json"));
    const QString ply = dir.filePath(QStringLiteral("geomap/tie_points.ply"));
    if (!QFileInfo::exists(json)) return {};
    if (QFileInfo::exists(ply) && QFileInfo(ply).lastModified() >= QFileInfo(json).lastModified()) return ply;
    QFile in(json);
    if (!in.open(QIODevice::ReadOnly)) return {};
    QByteArray body;
    int count = 0;
    for (const QJsonValue &rec : QJsonDocument::fromJson(in.readAll()).array()) {
        const QJsonObject points = rec.toObject().value(QStringLiteral("points")).toObject();
        for (auto it = points.begin(); it != points.end(); ++it) {
            const QJsonObject p = it.value().toObject();
            const QJsonArray c = p.value(QStringLiteral("coordinates")).toArray(), rgb = p.value(QStringLiteral("color")).toArray();
            if (c.size() != 3) continue;
            for (int k = 0; k < 3; ++k) {
                const float v = float(c[k].toDouble());
                body.append(reinterpret_cast<const char *>(&v), sizeof v);   // little-endian on every supported platform
            }
            for (int k = 0; k < 3; ++k) body.append(char(qBound(0, rgb.size() == 3 ? int(std::lround(rgb[k].toDouble())) : 200, 255)));
            ++count;
        }
    }
    if (count == 0) return {};
    QDir().mkpath(QFileInfo(ply).absolutePath());
    QSaveFile out(ply);
    if (!out.open(QIODevice::WriteOnly)) return {};
    out.write("ply\nformat binary_little_endian 1.0\ncomment GeoMap AI: tie points of the OpenSfM reconstruction\nelement vertex "
              + QByteArray::number(count)
              + "\nproperty float x\nproperty float y\nproperty float z\nproperty uchar red\nproperty uchar green\nproperty uchar blue\nend_header\n");
    out.write(body);
    return out.commit() ? ply : QString();
}

void ProcessingService::onStage(const QString &id, const QString &stage, int index, int count)
{
    ProcessingJob *job = findJob(id);
    if (!job || id != m_current) return;
    job->stage = stage;
    job->stageIndex = index;
    job->stageCount = count;
    writeLog(QStringLiteral("[GeoMap] Этап %1 из %2: %3").arg(index).arg(count).arg(m_engine->stageTitle(stage)));
    setState(*job, job->state);
}

void ProcessingService::onLog(const QString &id, const QString &line)
{
    if (id != m_current) return;
    if (m_log.isOpen()) {
        m_log.write(line.toUtf8());
        m_log.write("\n");
    }
    QStringList &recent = m_recentLog[id];
    recent.append(line);
    if (recent.size() > RecentLogLines) recent.removeFirst();
    if (!m_pendingLogIds.contains(id)) m_pendingLogIds.append(id);
    if (!m_logNotify.isActive()) m_logNotify.start();
}

void ProcessingService::onFinished(const QString &id, JobOutcome outcome, int exitCode, const QString &message)
{
    if (id != m_current) return;
    m_current.clear();
    if (ProcessingJob *job = findJob(id)) {
        job->exitCode = exitCode;
        job->finishedAt = QDateTime::currentDateTime();
        job->outputs = m_engine->collectOutputs(jobDirectory(id));
        JobState state = JobState::Failed;
        QString text = message;
        if (outcome == JobOutcome::Cancelled) {
            state = JobState::Cancelled;
        } else if (outcome == JobOutcome::Succeeded) {
            const QVariantMap quality = m_engine->qualityReport(jobDirectory(id));
            if (job->arguments.contains(QStringLiteral("--end-with")) && job->outputs.isEmpty() && !quality.isEmpty()) {
                // A partial run (e.g. alignment only) is complete when the engine reports on it.
                state = JobState::Completed;
                text = QStringLiteral("Выравнивание готово: %1 из %2 снимков. Проверьте качество и продолжите обработку.")
                           .arg(quality.value(QStringLiteral("alignedImages")).toInt())
                           .arg(quality.value(QStringLiteral("totalImages")).toInt());
            } else if (job->outputs.isEmpty()) {
                text = QStringLiteral("%1 завершился без ошибок, но результатов в папке задания нет.").arg(engineName());
            } else {
                state = JobState::Completed;
                text = QStringLiteral("Готово. Результатов: %1.").arg(job->outputs.size());
                const bool ortho = std::any_of(job->outputs.cbegin(), job->outputs.cend(),
                                               [](const Artifact &a) { return a.kind == QLatin1String("orthophoto"); });
                if (!ortho) text += QStringLiteral(" Ортофотоплан не создан — см. журнал.");
            }
        }
        // A crash of one of the engine's programs: the stage is computed again automatically (up to
        // twice); failing again on the same stage, from the stage before it, whose outputs may be
        // what made it fail (a half-written point cloud after a crash, for example).
        if (state == JobState::Failed && outcome == JobOutcome::Failed && !job->stage.isEmpty() && m_autoRetries.value(id) < 2
            && m_engine->retryable(exitCode, text)) {
            QString from = job->stage;
            if (m_failedStage.value(id) == job->stage && !m_engine->previousStage(job->stage).isEmpty())
                from = m_engine->previousStage(job->stage);
            m_failedStage[id] = job->stage;
            const int attempt = ++m_autoRetries[id];
            job->arguments = m_engine->resumeArguments(job->arguments, from);
            const QString note = QStringLiteral("%1 Повтор с этапа «%2» автоматически (попытка %3 из 2).")
                                     .arg(text, m_engine->stageTitle(from)).arg(attempt);
            closeLog(note);
            job->exitCode = 0;
            job->finishedAt = {};
            m_resumed << id;
            m_queue.prepend(id);
            setState(*job, JobState::Queued, note);
            emit jobsChanged();
            runNext();
            return;
        }
        setState(*job, state, text);
        closeLog(QStringLiteral("%1 (код %2)").arg(text).arg(exitCode));
        const bool dsm = std::any_of(job->outputs.cbegin(), job->outputs.cend(), [](const Artifact &a) { return a.kind == QLatin1String("dsm"); });
        if (state == JobState::Completed && dsm && autoTrueOrthophoto() && !trueOrthoBusy()) buildTrueOrthophoto(id);
    } else {
        closeLog(message);
    }
    emit jobFinished(id);
    runNext();
}

bool ProcessingService::autoTrueOrthophoto() const
{
    return QSettings().value(QStringLiteral("processing/autoTrueOrthophoto"), true).toBool();
}

void ProcessingService::setAutoTrueOrthophoto(bool on)
{
    if (on == autoTrueOrthophoto()) return;
    QSettings().setValue(QStringLiteral("processing/autoTrueOrthophoto"), on);
    emit trueOrthoChanged();
}

QString ProcessingService::buildTrueOrthophoto(const QString &requested)
{
    if (m_project.isEmpty()) return QStringLiteral("Проект не открыт.");
    if (trueOrthoBusy()) return QStringLiteral("Истинный ортофотоплан уже строится.");
    QString id = requested;
    const QString dsmFile = QStringLiteral("odm_dem/dsm.tif");
    if (id.isEmpty())
        for (const ProcessingJob &j : std::as_const(m_jobs))   // newest first, of the active chunk
            if (jobInChunk(j) && j.state == JobState::Completed && QFileInfo::exists(QDir(jobDirectory(j.id)).filePath(dsmFile))) {
                id = j.id;
                break;
            }
    if (id.isEmpty() || !findJob(id)) return QStringLiteral("Нет завершённого задания с моделью поверхности (DSM). Обработайте снимки профилем с DSM.");
    const QString dir = jobDirectory(id);
    if (!QFileInfo::exists(QDir(dir).filePath(dsmFile))) return QStringLiteral("В задании %1 нет модели поверхности (DSM).").arg(id);
    if (!QFileInfo::exists(QDir(dir).filePath(QStringLiteral("opensfm/reconstruction.json"))))
        return QStringLiteral("В задании %1 нет реконструкции снимков (opensfm).").arg(id);
    m_orthoJob = id;
    m_orthoStatus = QStringLiteral("Истинный ортофотоплан: подготовка…");
    m_orthoCancel = std::make_shared<std::atomic_bool>(false);
    emit trueOrthoChanged();
    const auto cancel = m_orthoCancel;
    const QString project = m_project;
    // The results are written as .partial files and replace the previous ones on the main thread
    // (see the end): a result shown on the map is open there, and Windows does not replace an open file.
    const QStringList finals = {QDir(dir).filePath(QStringLiteral("geomap/true_orthophoto.tif")),
                                QDir(dir).filePath(QStringLiteral("geomap/true_surface.tif")),
                                QDir(dir).filePath(QStringLiteral("geomap/true_orthophoto_stretch.tif"))};
    auto partialOf = [](const QString &path) { return path.left(path.size() - 4) + QStringLiteral(".partial.tif"); };
    m_orthoPool->start([this, id, dir, cancel, project, finals, partialOf] {
        ortho::Scene scene;
        QString error;
        ortho::Result r;
        const QString partial = partialOf(finals[0]);
        if (!ortho::readOdmJob(dir, &scene, &error)) {
            r.error = error;
        } else {
            ortho::Options options;
            const QString dtm = QDir(dir).filePath(QStringLiteral("odm_dem/dtm.tif"));
            if (QFileInfo::exists(dtm)) options.groundPath = dtm;
            // The dense point cloud gives crowns and roofs their true outline and height.
            const QString cloud = QDir(dir).filePath(QStringLiteral("odm_filterpoints/point_cloud.ply"));
            if (QFileInfo::exists(cloud)) {
                options.pointCloudPath = cloud;
                options.surfacePath = partialOf(finals[1]);   // the 3D view drapes the orthophoto over it
            }
            // Where its pixels are streaks: the input of «Анализ → Ретушь ИИ».
            options.stretchMaskPath = partialOf(finals[2]);
            std::atomic_int lastPercent{-1};
            options.progress = [this, cancel, &lastPercent](double v) {
                const int percent = int(v * 100);
                if (percent != lastPercent.exchange(percent))
                    QMetaObject::invokeMethod(this, [this, percent] {
                        if (m_orthoJob.isEmpty()) return;
                        m_orthoStatus = QStringLiteral("Истинный ортофотоплан: %1 %").arg(percent);
                        emit trueOrthoChanged();
                    });
                return !*cancel;
            };
            for (const QString &f : finals) QFile::remove(partialOf(f));
            r = ortho::build(scene, QDir(dir).filePath(QStringLiteral("odm_dem/dsm.tif")), partial, options);
        }
        QMetaObject::invokeMethod(this, [this, id, r, cancel, project, finals, partialOf]() mutable {
            m_orthoJob.clear();
            if (*cancel || project != m_project || !r.ok()) {
                for (const QString &f : finals) QFile::remove(partialOf(f));
            }
            if (*cancel || project != m_project) {
                m_orthoStatus.clear();
                emit trueOrthoChanged();
                return;
            }
            // The orthophoto first: if it cannot be replaced, the others stay as they were too.
            for (int k = 0; r.ok() && k < finals.size(); ++k) {
                const QString &target = finals[k], partial = partialOf(target);
                if (!QFileInfo::exists(partial)) {
                    if (k == 2) QFile::remove(target);   // no mask this time: none, not an old one
                    continue;
                }
                emit resultFileReplacing(target);   // the map closes the file
                bool replaced = false;
                for (int attempt = 0; attempt < 30 && !replaced; ++attempt) {   // a tile still rendering holds it a moment
                    if (attempt) QThread::msleep(100);
                    replaced = (!QFileInfo::exists(target) || QFile::remove(target)) && QFile::rename(partial, target);
                }
                emit resultFileReplaced(target);   // the map opens it again (the new one, or the old one if kept)
                if (!replaced) {
                    if (k == 0) {
                        r.error = QStringLiteral("Не удалось заменить %1: файл занят другой программой").arg(QDir::toNativeSeparators(target));
                        for (const QString &f : finals) QFile::remove(partialOf(f));
                    }
                    QFile::remove(partial);
                }
            }
            const bool ok = r.ok();
            m_orthoStatus = ok ? QStringLiteral("Истинный ортофотоплан готов: %1×%2 пикс., %3 см/пикс., %4 с%5")
                                     .arg(r.width).arg(r.height).arg(QLocale(QLocale::Russian).toString(r.resolution * 100, 'f', 1))
                                     .arg(QLocale(QLocale::Russian).toString(r.seconds, 'f', 0))
                                     .arg(r.stretched >= 0.0005 ? QStringLiteral("; растянутых участков %1 % (их можно дорисовать: «Анализ → Ретушь ИИ»)")
                                                                      .arg(QLocale(QLocale::Russian).toString(r.stretched * 100, 'f', 1))
                                                                : QString())
                               : QStringLiteral("Истинный ортофотоплан не построен: %1").arg(r.error);
            if (ProcessingJob *job = findJob(id)) {
                job->outputs = m_engine->collectOutputs(jobDirectory(id));
                QString message = job->message;
                const int at = message.indexOf(QStringLiteral(" Истинный ортофотоплан"));
                if (at >= 0) message.truncate(at);
                setState(*job, job->state, message + QLatin1Char(' ') + m_orthoStatus + QLatin1Char('.'));
            }
            emit trueOrthoChanged();
            emit trueOrthoFinished(id, ok, m_orthoStatus);
        });
    });
    return {};
}

void ProcessingService::setState(ProcessingJob &job, JobState state, const QString &message)
{
    job.state = state;
    if (!message.isNull()) job.message = message;
    if (!saveJob(job))
        emit errorOccurred(QStringLiteral("Не удалось сохранить состояние задания %1 (диск заполнен или нет доступа).").arg(job.id));
    m_model.update(job);
    emit jobUpdated(job.id);
    emit jobsChanged();
}

bool ProcessingService::saveJob(const ProcessingJob &job) const
{
    QSaveFile file(QDir(jobDirectory(job.id)).filePath(JobFile));
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.write(QJsonDocument(job.toJson()).toJson(QJsonDocument::Indented));
    return file.commit();
}

void ProcessingService::loadJobs()
{
    m_jobs.clear();
    m_recentLog.clear();
    if (m_project.isEmpty()) return;
    const QDir jobs(QDir(m_project).filePath(JobsDir));
    const QStringList names = jobs.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &name : names) {
        QFile file(jobs.filePath(name + QLatin1Char('/') + JobFile));
        if (!file.open(QIODevice::ReadOnly)) continue;
        bool ok = false;
        ProcessingJob job = ProcessingJob::fromJson(QJsonDocument::fromJson(file.readAll()).object(), &ok);
        file.close();
        if (!ok || job.id != name) {
            qWarning("Skipping unreadable job description %s", qUtf8Printable(QDir::toNativeSeparators(file.fileName())));
            continue;
        }
        if (isActive(job.state)) {
            // The application stopped while the job ran: never report it as completed.
            m_engine->cleanupInterrupted(job);
            job.state = JobState::Interrupted;
            job.message = QStringLiteral("Обработка прервана: приложение было закрыто или завершилось аварийно. "
                                         "Результаты могут быть неполными; запустите обработку заново.");
            job.outputs = m_engine->collectOutputs(jobs.filePath(name));
            saveJob(job);
        }
        m_jobs.append(job);
    }
    std::sort(m_jobs.begin(), m_jobs.end(), [](const ProcessingJob &a, const ProcessingJob &b) {
        return a.createdAt != b.createdAt ? a.createdAt > b.createdAt : a.id > b.id;
    });
}

bool ProcessingService::deleteJob(const QString &id)
{
    ProcessingJob *job = findJob(id);
    if (!job || isActive(job->state) || id == m_current) return false;
    if (id == m_orthoJob) {
        emit errorOccurred(QStringLiteral("Для этого задания строится истинный ортофотоплан — дождитесь окончания."));
        return false;
    }
    const QFileInfo dir(jobDirectory(id));
    if (dir.exists()) {
        emit releaseDirectory(dir.absoluteFilePath());   // the map closes the job's rasters first
        // Only a real directory directly inside <project>/jobs, never through a link to elsewhere.
        const QString jobsRoot = QFileInfo(QDir(m_project).filePath(JobsDir)).canonicalFilePath();
        if (jobsRoot.isEmpty() || dir.isSymLink() || dir.isJunction() || !dir.isDir() || dir.canonicalPath() != jobsRoot) {
            emit errorOccurred(QStringLiteral("Папка задания %1 не является обычной папкой внутри проекта; удаление отменено.").arg(id));
            return false;
        }
        if (!QDir(dir.absoluteFilePath()).removeRecursively()) {
            emit errorOccurred(QStringLiteral("Папку задания удалось удалить не полностью: %1. Возможно, файлы открыты в другой программе.")
                                   .arg(QDir::toNativeSeparators(dir.absoluteFilePath())));
            // Keep the job listed while its description still exists.
            if (QFileInfo::exists(dir.absoluteFilePath() + QLatin1Char('/') + JobFile)) return false;
        }
    }
    m_recentLog.remove(id);
    m_jobs.erase(std::remove_if(m_jobs.begin(), m_jobs.end(), [&id](const ProcessingJob &j) { return j.id == id; }),
                 m_jobs.end());
    m_model.remove(id);
    emit jobsChanged();
    return true;
}

void ProcessingService::openLog(const ProcessingJob &job)
{
    m_log.close();
    m_log.setFileName(QDir(jobDirectory(job.id)).filePath(LogFile));
    if (!m_log.open(QIODevice::WriteOnly | QIODevice::Append)) return;
    const bool resumed = m_resumed.contains(job.id);
    QStringList header = {
        QStringLiteral("==== %1 · задание %2 · %3%4")
            .arg(QStringLiteral("GeoMap AI %1").arg(QCoreApplication::applicationVersion()).trimmed(), job.id,
                 QDateTime::currentDateTime().toString(Qt::ISODate),
                 resumed ? QStringLiteral(" · продолжение: готовые этапы используются повторно") : QString()),
        QStringLiteral("Движок: %1 %2%3").arg(engineName(), job.engineVersion,
                                             job.backend.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(backendTitle(job.backend))),
        QStringLiteral("Профиль: %1 (%2)").arg(profile(job.profile).title, job.profile),
        QStringLiteral("Параметры: %1").arg(job.arguments.join(QLatin1Char(' '))),
        QStringLiteral("Снимков: %1").arg(job.images.size()),
    };
    for (const QString &note : job.notes) header << QStringLiteral("Автоматически: %1").arg(note);
    for (const QString &line : header) writeLog(line);
}

void ProcessingService::writeLog(const QString &line)
{
    if (m_current.isEmpty()) {
        if (m_log.isOpen()) m_log.write((line + QLatin1Char('\n')).toUtf8());
        return;
    }
    onLog(m_current, line);
}

void ProcessingService::closeLog(const QString &line)
{
    if (!m_log.isOpen()) return;
    m_log.write(QStringLiteral("==== %1 · %2\n").arg(QDateTime::currentDateTime().toString(Qt::ISODate), line).toUtf8());
    m_log.close();
}

QStringList ProcessingService::logTail(const QString &id, int lines) const
{
    if (id == m_current && m_recentLog.contains(id)) {
        const QStringList &recent = m_recentLog[id];
        return recent.mid(qMax(0, int(recent.size()) - lines));
    }
    QFile file(QDir(jobDirectory(id)).filePath(LogFile));
    if (!file.open(QIODevice::ReadOnly)) return {};
    const qint64 window = 64 * 1024;
    const bool partial = file.size() > window;
    if (partial) file.seek(file.size() - window);
    QStringList all = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (partial && !all.isEmpty()) all.removeFirst();   // cut in the middle of a line
    return all.mid(qMax(0, int(all.size()) - lines));
}

QVariantMap ProcessingService::jobDetails(const QString &id) const
{
    const ProcessingJob j = job(id);
    if (j.id.isEmpty()) return {};
    const QDir dir(jobDirectory(id));
    QVariantList outputs;
    for (const Artifact &a : j.outputs)
        outputs << QVariantMap{{"kind", a.kind}, {"title", kindTitle(a.kind)}, {"file", a.file},
                               {"path", QDir::toNativeSeparators(dir.filePath(a.file))},
                               {"bytes", double(a.bytes)}, {"size", sizeText(a.bytes)}, {"details", a.details}};
    QStringList missing;
    const bool partial = j.arguments.contains(QStringLiteral("--end-with"));
    if (j.state == JobState::Completed && !partial) {
        for (const QString &kind : m_engine->capabilities().outputs) {
            const bool present = std::any_of(j.outputs.cbegin(), j.outputs.cend(), [&kind](const Artifact &a) { return a.kind == kind; });
            if (!present) missing << kindTitle(kind);
        }
    }
    qint64 seconds = -1;
    if (j.startedAt.isValid())
        seconds = j.startedAt.secsTo(j.finishedAt.isValid() ? j.finishedAt : QDateTime::currentDateTime());
    const bool running = id == m_current;
    return {{"id", j.id}, {"state", jobStateId(j.state)}, {"stateText", jobStateText(j.state)},
            {"active", isActive(j.state)}, {"running", running},
            {"profile", j.profile}, {"profileTitle", profile(j.profile).title},
            {"engine", QStringLiteral("%1 %2").arg(engineName(), j.engineVersion)},
            {"arguments", j.arguments.join(QLatin1Char(' '))}, {"message", j.message},
            {"stage", j.stage}, {"stageTitle", j.stage.isEmpty() ? QString() : m_engine->stageTitle(j.stage)},
            {"stageIndex", j.stageIndex}, {"stageCount", j.stageCount},
            {"createdAt", j.createdAt}, {"startedAt", j.startedAt}, {"finishedAt", j.finishedAt},
            {"startedMs", j.startedAt.isValid() ? double(j.startedAt.toMSecsSinceEpoch()) : -1.0},
            {"seconds", double(seconds)}, {"imageCount", int(j.images.size())}, {"exitCode", j.exitCode},
            {"directory", QDir::toNativeSeparators(dir.absolutePath())},
            {"logFile", QDir::toNativeSeparators(dir.filePath(LogFile))},
            {"outputs", outputs}, {"missing", missing},
            {"backend", j.backend}, {"backendTitle", backendTitle(j.backend)}, {"notes", j.notes}, {"runs", j.runs},
            {"partial", partial},
            // Stopped, failed or partial jobs continue in place; the engine reuses finished stages.
            {"canResume", !isActive(j.state) && !running && (j.state != JobState::Completed || partial)},
            {"quality", running ? QVariantMap() : m_engine->qualityReport(dir.absolutePath())},
            {"log", logTail(id, RecentLogLines).join(QLatin1Char('\n'))}};
}

QString ProcessingService::backendTitle(const QString &backend)
{
    if (backend == QLatin1String("native")) return QStringLiteral("встроенный");
    if (backend == QLatin1String("docker")) return QStringLiteral("Docker");
    return backend;
}

void ProcessingService::revealJob(const QString &id)
{
    const QString dir = jobDirectory(id);
    if (dir.isEmpty() || !QFileInfo(dir).isDir() || !QDesktopServices::openUrl(QUrl::fromLocalFile(dir)))
        emit errorOccurred(QStringLiteral("Не удалось открыть папку задания."));
}

void ProcessingService::openOutput(const QString &id, const QString &file)
{
    const ProcessingJob j = job(id);
    const bool registered = std::any_of(j.outputs.cbegin(), j.outputs.cend(), [&file](const Artifact &a) { return a.file == file; });
    const QString path = QDir(jobDirectory(id)).filePath(file);
    if (!registered || !QFileInfo(path).isFile()) {
        emit errorOccurred(QStringLiteral("Файл результата не найден: %1").arg(file));
        return;
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
        emit errorOccurred(QStringLiteral("Нет программы для открытия %1. Откройте папку задания и выберите программу вручную "
                                          "(например, QGIS для GeoTIFF и LAZ).").arg(QFileInfo(path).fileName()));
}
