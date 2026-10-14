#include "ProcessingService.h"
#include "PhotoModel.h"
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QProcess>
#include <QSaveFile>
#include <QStorageInfo>
#include <QThread>
#include <QUrl>
#include <algorithm>

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

struct PrepareResult { QString error; int copied = 0; };

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
    }
    if (!unreadable.isEmpty())
        result.error = QStringLiteral("Не читаются снимки (%1): %2. Удалите их из проекта или импортируйте заново.")
                           .arg(unreadable.size()).arg(sample(unreadable));
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
            {StartedAtRole, "startedAt"}, {FinishedAtRole, "finishedAt"}};
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
    : QObject(parent), m_engine(engine), m_model(engine)
{
    m_engine->setParent(this);
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
    stopAll(QStringLiteral("Обработка остановлена: приложение закрыто во время выполнения."));
    m_engine->disconnect(this);
}

void ProcessingService::setProject(const QString &directory, const PhotoModel *photos)
{
    const QString dir = directory.isEmpty() ? QString() : QDir(directory).absolutePath();
    m_photos = photos;
    if (dir == m_project) return;
    stopAll(QStringLiteral("Обработка остановлена: проект закрыт во время выполнения."));
    m_project = dir;
    loadJobs();
    m_model.setJobs(m_jobs);
    emit jobsChanged();
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
    for (const ProcessingProfile &p : m_engine->profiles()) {
        if (p.id == id) {
            if (found) *found = true;
            return p;
        }
    }
    if (found) *found = false;
    return {};
}

QVariantList ProcessingService::profilesForQml() const
{
    QVariantList list;
    for (const ProcessingProfile &p : m_engine->profiles())
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
    m_installStatus = QStringLiteral("Загрузка образа…");
    emit installChanged();
    m_engine->installRuntime();
}

ProcessingService::Check ProcessingService::runPreflight(const QString &profileId, const QString &extraOptions) const
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
        c.errors << QStringLiteral("Для реконструкции нужно не меньше %1 снимков с перекрытием; подходящих в проекте: %2.")
                        .arg(caps.minimumImages).arg(count);
    else if (found)
        c.warnings << m_engine->resourceWarnings(count, p, m_env);
    if (count > 0 && c.withGps == 0)
        c.warnings << QStringLiteral("Ни у одного снимка нет GPS в EXIF: результат не будет иметь географической привязки.");
    else if (c.withGps < count)
        c.warnings << QStringLiteral("Без GPS: %1 из %2 снимков.").arg(count - c.withGps).arg(count);

    if (found) {
        ProcessingJob probe;
        QString error;
        if (m_engine->createJob(probe, p, QProcess::splitCommand(extraOptions), m_env, &error))
            c.arguments = probe.arguments;
        else
            c.errors << error;
    }

    const QStorageInfo storage(m_project);
    c.free = storage.isValid() ? storage.bytesAvailable() : -1;
    c.needed = qint64(double(c.inputBytes) * p.diskFactor) + m_diskReserve;
    if (c.free >= 0 && c.free < c.needed)
        c.errors << QStringLiteral("Недостаточно места на диске проекта: нужно около %1, свободно %2.")
                        .arg(sizeText(c.needed), sizeText(c.free));
    return c;
}

QVariantMap ProcessingService::preflight(const QString &profileId, const QString &extraOptions) const
{
    const Check c = runPreflight(profileId, extraOptions);
    return {{"ok", c.errors.isEmpty()}, {"errors", c.errors}, {"warnings", c.warnings},
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

QString ProcessingService::startJob(const QString &profileId, const QString &extraOptions)
{
    const Check c = runPreflight(profileId, extraOptions);
    if (!c.errors.isEmpty()) {
        emit errorOccurred(c.errors.join(QLatin1Char('\n')));
        return {};
    }
    ProcessingJob job;
    job.id = newJobId();
    job.profile = profileId;
    job.images = c.images;
    job.createdAt = QDateTime::currentDateTime();
    QString error;
    if (!m_engine->createJob(job, profile(profileId), QProcess::splitCommand(extraOptions), m_env, &error)) {
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
            QMetaObject::invokeMethod(this, [this, id, result] { prepareFinished(id, result.error, result.copied); },
                                      Qt::QueuedConnection);
        });
        connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        m_prepareThread = thread;
        thread->start();
    }
}

void ProcessingService::prepareFinished(const QString &id, const QString &error, int copied)
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
    job->startedAt = QDateTime::currentDateTime();
    setState(*job, JobState::Running, QStringLiteral("Запущен %1.").arg(engineName()));
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
            if (job->outputs.isEmpty()) {
                text = QStringLiteral("%1 завершился без ошибок, но результатов в папке задания нет.").arg(engineName());
            } else {
                state = JobState::Completed;
                text = QStringLiteral("Готово. Результатов: %1.").arg(job->outputs.size());
                const bool ortho = std::any_of(job->outputs.cbegin(), job->outputs.cend(),
                                               [](const Artifact &a) { return a.kind == QLatin1String("orthophoto"); });
                if (!ortho) text += QStringLiteral(" Ортофотоплан не создан — см. журнал.");
            }
        }
        setState(*job, state, text);
        closeLog(QStringLiteral("%1 (код %2)").arg(text).arg(exitCode));
    } else {
        closeLog(message);
    }
    emit jobFinished(id);
    runNext();
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
    const QFileInfo dir(jobDirectory(id));
    if (dir.exists()) {
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
    const QStringList header = {
        QStringLiteral("==== %1 · задание %2 · %3")
            .arg(QStringLiteral("GeoMap AI %1").arg(QCoreApplication::applicationVersion()).trimmed(), job.id,
                 QDateTime::currentDateTime().toString(Qt::ISODate)),
        QStringLiteral("Движок: %1 %2").arg(engineName(), job.engineVersion),
        QStringLiteral("Профиль: %1 (%2)").arg(profile(job.profile).title, job.profile),
        QStringLiteral("Параметры: %1").arg(job.arguments.join(QLatin1Char(' '))),
        QStringLiteral("Снимков: %1").arg(job.images.size()),
    };
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
    if (j.state == JobState::Completed) {
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
            {"log", logTail(id, RecentLogLines).join(QLatin1Char('\n'))}};
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
