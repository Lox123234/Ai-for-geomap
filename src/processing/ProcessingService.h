#pragma once

#include "IProcessingEngine.h"
#include "ProcessingTypes.h"
#include <QAbstractListModel>
#include <QFile>
#include <QHash>
#include <QSet>
#include <QPointer>
#include <QTimer>
#include <QVariantMap>
#include <atomic>
#include <memory>

class PhotoModel;
class QThread;
class QThreadPool;

// Processing jobs of the open project for QML, newest first.
class JobListModel final : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Roles {
        IdRole = Qt::UserRole + 1, StateRole, StateTextRole, ActiveRole, ProfileRole, CreatedRole,
        StageRole, StageTitleRole, StageIndexRole, StageCountRole, MessageRole, ImageCountRole,
        OutputCountRole, StartedAtRole, FinishedAtRole, ChunkRole
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
    Q_PROPERTY(QString backend READ backend NOTIFY environmentChanged)

    Q_PROPERTY(QString gpuName READ gpuName NOTIFY environmentChanged)
    Q_PROPERTY(bool canInstall READ canInstall NOTIFY environmentChanged)
    Q_PROPERTY(QString installTitle READ installTitle NOTIFY environmentChanged)
    Q_PROPERTY(QString installPrompt READ installPrompt NOTIFY environmentChanged)
    Q_PROPERTY(QVariantList optionDefinitions READ optionDefinitions NOTIFY environmentChanged)
    Q_PROPERTY(bool installing READ installing NOTIFY installChanged)
    Q_PROPERTY(QString installStatus READ installStatus NOTIFY installChanged)
    Q_PROPERTY(QVariantList profiles READ profilesForQml CONSTANT)
    Q_PROPERTY(bool active READ active NOTIFY jobsChanged)
    Q_PROPERTY(QString activeJobId READ activeJobId NOTIFY jobsChanged)
    Q_PROPERTY(int jobCount READ jobCount NOTIFY jobsChanged)
    Q_PROPERTY(JobListModel *jobModel READ jobModel CONSTANT)
    // True orthophoto (GeoMap AI) from the photos, poses and DSM of a finished job: built
    // automatically after jobs with a DSM (when enabled) or on request.
    Q_PROPERTY(bool trueOrthoBusy READ trueOrthoBusy NOTIFY trueOrthoChanged)
    Q_PROPERTY(QString trueOrthoStatus READ trueOrthoStatus NOTIFY trueOrthoChanged)
    Q_PROPERTY(bool autoTrueOrthophoto READ autoTrueOrthophoto WRITE setAutoTrueOrthophoto NOTIFY trueOrthoChanged)
    // Обработка → Оптимизация камер running (the bundle adjustment, before the job is built again).
    Q_PROPERTY(bool optimizing READ optimizing NOTIFY optimizeChanged)
public:
    // Takes ownership of the engine.
    explicit ProcessingService(IProcessingEngine *engine, QObject *parent = nullptr);
    ~ProcessingService() override;

    // Switches to another project (empty = none); photos supplies the project's photo list.
    // Work of the previous project is stopped and recorded as interrupted.
    void setProject(const QString &directory, const PhotoModel *photos);
    QString projectDirectory() const { return m_project; }
    // The chunk new jobs process (ChunkService): its id and photo ids; jobs written before
    // chunks existed (no chunk) belong to the first chunk. An empty id: all photos.
    void setChunk(const QString &id, const QStringList &photoIds, bool firstChunk);
    QString chunk() const { return m_chunk; }
    bool jobInChunk(const ProcessingJob &job) const;
    static bool jobInChunk(const ProcessingJob &job, const QString &chunk, bool firstChunk);
    // Jobs written before chunks (no chunk) join this one; their job.json is updated.
    void adoptJobs(const QString &chunk);
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
    QString backend() const { return m_env.backend; }

    QString gpuName() const { return m_env.gpuName; }
    bool canInstall() const { return m_env.canInstall; }
    QString installTitle() const { return m_env.installTitle; }
    QString installPrompt() const { return m_env.installPrompt; }
    QVariantList optionDefinitions() const { return m_engine->optionDefinitions(m_env); }
    bool installing() const { return m_installing; }
    QString installStatus() const { return m_installStatus; }
    QVariantList profilesForQml() const;
    bool active() const { return !m_current.isEmpty() || !m_queue.isEmpty(); }
    QString activeJobId() const { return m_current; }
    int jobCount() const { return int(m_jobs.size()); }
    JobListModel *jobModel() { return &m_model; }

    Q_INVOKABLE void checkEnvironment();
    Q_INVOKABLE void installRuntime();
    Q_INVOKABLE void cancelInstall();
    // {ok, errors[], warnings[], notes[], images, withGps, inputBytes, neededBytes, freeBytes, arguments}
    Q_INVOKABLE QVariantMap preflight(const QString &profileId, const QString &extraOptions = {},
                                      const QVariantMap &options = {}) const;
    // Returns the new job id, or an empty string after errorOccurred().
    Q_INVOKABLE QString startJob(const QString &profileId, const QString &extraOptions = {},
                                 const QVariantMap &options = {});
    // Continues a stopped or partial job in its own directory; the engine reuses finished
    // stages. With a profile, the job continues with that profile's settings.
    Q_INVOKABLE bool resumeJob(const QString &id, const QString &profileId = {},
                               const QVariantMap &options = {}, const QString &extraOptions = {});
    // One more step of the engine on a finished job, in its directory: "tiles" (3D Tiles of the
    // model and the cloud), "dense" (depth maps and the dense cloud of an aligned job). "" when queued.
    Q_INVOKABLE QString runStep(const QString &id, const QString &step);
    // The newest completed job of the active chunk that is "aligned" (has a reconstruction),
    // "alignedOnly" (stopped after the alignment) or has a textured "model"; "" if none.
    Q_INVOKABLE QString newestJob(const QString &kind) const;
    // The tie points of a job's alignment as a PLY: ODM's opensfm/reconstruction.ply, else written
    // from the reconstruction into geomap/tie_points.ply; "" if none. An empty id: the newest aligned job.
    Q_INVOKABLE QString sparseCloud(const QString &jobId = {}) const;
    Q_INVOKABLE void cancelJob(const QString &id);
    // Обработка → Оптимизация камер: the job's cameras bundled again with the project's control points
    // (cameraoptimizer), then its results built again by the engine from the new poses. "" when started.
    Q_INVOKABLE QString optimizeCameras(const QString &id, bool refineCalibration = true);
    bool optimizing() const { return !m_optimizeJob.isEmpty(); }
    // The results of a job read again from its folder (after GeoMap AI wrote one, e.g. the report).
    void refreshOutputs(const QString &id);
    // Removes a finished job and its directory (results included). Callers confirm first.
    Q_INVOKABLE bool deleteJob(const QString &id);
    // Detached snapshot for the details panel: fields, outputs and the log tail.
    Q_INVOKABLE QVariantMap jobDetails(const QString &id) const;
    Q_INVOKABLE void revealJob(const QString &id);
    // Opens a registered result of the job with the system's default application.
    Q_INVOKABLE void openOutput(const QString &id, const QString &file);

    bool trueOrthoBusy() const { return !m_orthoJob.isEmpty(); }
    QString trueOrthoStatus() const { return m_orthoStatus; }
    bool autoTrueOrthophoto() const;
    void setAutoTrueOrthophoto(bool on);
    // Builds <job>/geomap/true_orthophoto.tif for the job (empty = the newest completed job with a
    // DSM). "" when started, else the reason.
    Q_INVOKABLE QString buildTrueOrthophoto(const QString &jobId = {});

signals:
    void environmentChanged();
    void installChanged();
    void jobsChanged();
    void jobUpdated(const QString &id);
    void logUpdated(const QString &id);
    void jobFinished(const QString &id);
    void errorOccurred(const QString &message);
    void trueOrthoChanged();
    void trueOrthoFinished(const QString &jobId, bool ok, const QString &message);
    void optimizeChanged();
    void optimizeFinished(const QString &jobId, bool ok, const QString &message);
    // A result file is about to be replaced by a rebuilt one, and was (or could not be): whoever
    // shows it closes it before and opens it again after (on Windows an open file cannot be replaced).
    void resultFileReplacing(const QString &path);
    void resultFileReplaced(const QString &path);
    // Emitted just before a job folder is deleted: whoever holds its files open closes them.
    void releaseDirectory(const QString &directory);

private:
    struct Check {
        QStringList errors, warnings, notes, images, arguments;
        int withGps = 0;
        qint64 inputBytes = 0, needed = 0, free = -1;
        JobRequest request;
    };
    Check runPreflight(const QString &profileId, const QString &extraOptions, const QVariantMap &options) const;
    ProcessingProfile profile(const QString &id, bool *found = nullptr) const;
    const ProcessingProfile *profilePointer(const QString &id) const;
    static QString backendTitle(const QString &backend);
    QString newJobId() const;
    void loadJobs();
    bool saveJob(const ProcessingJob &job) const;
    ProcessingJob *findJob(const QString &id);
    void setState(ProcessingJob &job, JobState state, const QString &message = {});
    void runNext();
    void prepareFinished(const QString &id, const QString &error, int copied, int masks = 0, const QStringList &notes = {});
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
    QString m_chunk;                  // "" = every photo of the project
    QSet<QString> m_chunkPhotos;
    bool m_firstChunk = true;
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

    QVector<ProcessingProfile> m_profiles;   // the engine's profiles, kept for JobRequest pointers
    QStringList m_resumed;                   // jobs whose next start continues an earlier run
    QHash<QString, QString> m_failedStage;   // stage of a job's last failure (resumes and automatic retries)
    QHash<QString, int> m_autoRetries;       // automatic retries since the user last started the job
    QString m_installStatus;
    qint64 m_diskReserve = 512LL * 1024 * 1024;
    JobListModel m_model;
    QString m_optimizeJob;                     // job whose cameras are being optimised
    std::shared_ptr<std::atomic_bool> m_optimizeCancel;
    QString m_orthoJob;                        // job whose true orthophoto is being built
    QString m_orthoStatus;
    std::shared_ptr<std::atomic_bool> m_orthoCancel;
    QThreadPool *m_orthoPool;
};
