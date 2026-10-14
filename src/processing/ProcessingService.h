#pragma once

#include "IProcessingEngine.h"
#include "ProcessingTypes.h"
#include <QAbstractListModel>
#include <QFile>
#include <QHash>
#include <QPointer>
#include <QTimer>
#include <QVariantMap>
#include <atomic>
#include <memory>

class PhotoModel;
class QThread;

// Processing jobs of the open project for QML, newest first.
class JobListModel final : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Roles {
        IdRole = Qt::UserRole + 1, StateRole, StateTextRole, ActiveRole, ProfileRole, CreatedRole,
        StageRole, StageTitleRole, StageIndexRole, StageCountRole, MessageRole, ImageCountRole,
        OutputCountRole, StartedAtRole, FinishedAtRole
    };
    explicit JobListModel(const IProcessingEngine *engine, QObject *parent = nullptr);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    void setJobs(const QVector<ProcessingJob> &jobs);
    void update(const ProcessingJob &job);   // replaces the row with the same id, or prepends
    void remove(const QString &id);
private:
    int rowOf(const QString &id) const;
    const IProcessingEngine *m_engine;
    QHash<QString, QString> m_profileTitles;
    QVector<ProcessingJob> m_jobs;
};

// Job queue and lifecycle on top of an IProcessingEngine. Jobs live in <project>/jobs/<id>/
// (job.json, log.txt, images/ and the engine outputs), so a new run never touches earlier
// results. One job runs at a time; jobs that were active when the application stopped are
// shown as interrupted after a restart, never as completed.
class ProcessingService final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString engineName READ engineName CONSTANT)
    Q_PROPERTY(QString engineVersion READ engineVersion CONSTANT)
    Q_PROPERTY(bool hasProject READ hasProject NOTIFY jobsChanged)
    Q_PROPERTY(bool checkingEnvironment READ checkingEnvironment NOTIFY environmentChanged)
    Q_PROPERTY(bool environmentChecked READ environmentChecked NOTIFY environmentChanged)
    Q_PROPERTY(bool engineAvailable READ engineAvailable NOTIFY environmentChanged)
    Q_PROPERTY(bool engineMissing READ engineMissing NOTIFY environmentChanged)
    Q_PROPERTY(bool runtimeMissing READ runtimeMissing NOTIFY environmentChanged)
    Q_PROPERTY(QString environmentSummary READ environmentSummary NOTIFY environmentChanged)
    Q_PROPERTY(QStringList environmentProblems READ environmentProblems NOTIFY environmentChanged)
    Q_PROPERTY(QStringList environmentWarnings READ environmentWarnings NOTIFY environmentChanged)
    Q_PROPERTY(QStringList environmentHints READ environmentHints NOTIFY environmentChanged)
    Q_PROPERTY(bool installing READ installing NOTIFY installChanged)
    Q_PROPERTY(QString installStatus READ installStatus NOTIFY installChanged)
    Q_PROPERTY(QVariantList profiles READ profilesForQml CONSTANT)
    Q_PROPERTY(bool active READ active NOTIFY jobsChanged)
    Q_PROPERTY(QString activeJobId READ activeJobId NOTIFY jobsChanged)
    Q_PROPERTY(int jobCount READ jobCount NOTIFY jobsChanged)
    Q_PROPERTY(JobListModel *jobModel READ jobModel CONSTANT)
public:
    // Takes ownership of the engine.
    explicit ProcessingService(IProcessingEngine *engine, QObject *parent = nullptr);
    ~ProcessingService() override;

    // Switches to another project (empty = none); photos supplies the project's photo list.
    // Work of the previous project is stopped and recorded as interrupted.
    void setProject(const QString &directory, const PhotoModel *photos);
    QString projectDirectory() const { return m_project; }
    QVector<ProcessingJob> jobs() const { return m_jobs; }
    ProcessingJob job(const QString &id) const;
    QString jobDirectory(const QString &id) const;
    IProcessingEngine *engine() const { return m_engine; }
    // Free space kept in addition to the profile estimate.
    void setDiskReserveBytes(qint64 bytes) { m_diskReserve = bytes; }
    // Stops the running job synchronously (application exit, project switch).
    void stopAll(const QString &reason);

    QString engineName() const { return m_engine->capabilities().displayName; }
    QString engineVersion() const { return m_engine->capabilities().version; }
    bool hasProject() const { return !m_project.isEmpty(); }
    bool checkingEnvironment() const { return m_checking; }
    bool environmentChecked() const { return m_env.checked; }
    bool engineAvailable() const { return m_env.available; }
    bool engineMissing() const { return m_env.engineMissing; }
    bool runtimeMissing() const { return m_env.imageMissing; }
    QString environmentSummary() const { return m_env.summary; }
    QStringList environmentProblems() const { return m_env.problems; }
    QStringList environmentWarnings() const { return m_env.warnings; }
    QStringList environmentHints() const { return m_env.hints; }
    bool installing() const { return m_installing; }
    QString installStatus() const { return m_installStatus; }
    QVariantList profilesForQml() const;
    bool active() const { return !m_current.isEmpty() || !m_queue.isEmpty(); }
    QString activeJobId() const { return m_current; }
    int jobCount() const { return int(m_jobs.size()); }
    JobListModel *jobModel() { return &m_model; }

    Q_INVOKABLE void checkEnvironment();
    Q_INVOKABLE void installRuntime();
    // {ok, errors[], warnings[], images, withGps, inputBytes, neededBytes, freeBytes}
    Q_INVOKABLE QVariantMap preflight(const QString &profileId, const QString &extraOptions = {}) const;
    // Returns the new job id, or an empty string after errorOccurred().
    Q_INVOKABLE QString startJob(const QString &profileId, const QString &extraOptions = {});
    Q_INVOKABLE void cancelJob(const QString &id);
    // Removes a finished job and its directory (results included). Callers confirm first.
    Q_INVOKABLE bool deleteJob(const QString &id);
    // Detached snapshot for the details panel: fields, outputs and the log tail.
    Q_INVOKABLE QVariantMap jobDetails(const QString &id) const;
    Q_INVOKABLE void revealJob(const QString &id);
    // Opens a registered result of the job with the system's default application.
    Q_INVOKABLE void openOutput(const QString &id, const QString &file);

signals:
    void environmentChanged();
    void installChanged();
    void jobsChanged();
    void jobUpdated(const QString &id);
    void logUpdated(const QString &id);
    void jobFinished(const QString &id);
    void errorOccurred(const QString &message);

private:
    struct Check {
        QStringList errors, warnings, images, arguments;
        int withGps = 0;
        qint64 inputBytes = 0, needed = 0, free = -1;
    };
    Check runPreflight(const QString &profileId, const QString &extraOptions) const;
    ProcessingProfile profile(const QString &id, bool *found = nullptr) const;
    QString newJobId() const;
    void loadJobs();
    bool saveJob(const ProcessingJob &job) const;
    ProcessingJob *findJob(const QString &id);
    void setState(ProcessingJob &job, JobState state, const QString &message = {});
    void runNext();
    void prepareFinished(const QString &id, const QString &error, int copied);
    void onStage(const QString &id, const QString &stage, int index, int count);
    void onLog(const QString &id, const QString &line);
    void onFinished(const QString &id, JobOutcome outcome, int exitCode, const QString &message);
    void openLog(const ProcessingJob &job);
    void writeLog(const QString &line);
    void closeLog(const QString &line);
    QStringList logTail(const QString &id, int lines) const;

    IProcessingEngine *m_engine;
    QString m_project;
    const PhotoModel *m_photos = nullptr;
    QVector<ProcessingJob> m_jobs;    // newest first
    QStringList m_queue;              // ids waiting to run, oldest first
    QString m_current;                // preparing, running or cancelling
    std::shared_ptr<std::atomic_bool> m_prepareCancel;
    QPointer<QThread> m_prepareThread;
    QFile m_log;
    QHash<QString, QStringList> m_recentLog;
    QTimer m_logNotify;
    QStringList m_pendingLogIds;
    EnvironmentReport m_env;
    bool m_checking = false;
    bool m_installing = false;
    QString m_installStatus;
    qint64 m_diskReserve = 512LL * 1024 * 1024;
    JobListModel m_model;
};
