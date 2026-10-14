#pragma once

#include "IProcessingEngine.h"
#include <QPointer>
#include <QProcess>
#include <functional>

// OpenDroneMap in Docker, as an isolated process. The image is pinned by digest; commands are
// built as argument lists (no shell); the job directory is bind-mounted and the input images
// are mounted read-only. Docker is needed only for processing, not to run GeoMap AI.
class OdmEngine final : public IProcessingEngine
{
    Q_OBJECT
public:
    static constexpr auto ImageTag = "opendronemap/odm:3.6.2";
    static constexpr auto ImageDigest = "sha256:94a0dd2714be4e8b565bdabd8f67e0191f36e37d82c179433adcb0a403aac3bd";
    static constexpr auto Version = "3.6.2";

    explicit OdmEngine(QObject *parent = nullptr);
    ~OdmEngine() override;   // removes a still running container

    void setDockerPath(const QString &path) { m_docker = path; }
    QString dockerPath() const { return m_docker; }
    static QString findDocker();

    EngineCapabilities capabilities() const override;
    QVector<ProcessingProfile> profiles() const override;
    QString stageTitle(const QString &stage) const override;   // Russian UI name
    QStringList resourceWarnings(int imageCount, const ProcessingProfile &profile,
                                 const EnvironmentReport &env) const override;
    void validateEnvironment() override;
    void installRuntime() override;
    bool createJob(ProcessingJob &job, const ProcessingProfile &profile, const QStringList &extraArguments,
                   const EnvironmentReport &env, QString *error) override;
    bool start(const ProcessingJob &job, const QString &jobDirectory, QString *error) override;
    void cancel(const QString &jobId) override;
    void abort(const QString &jobId) override;
    bool isRunning(const QString &jobId) const override;
    JobProgress readProgress(const QString &jobId) const override;
    QVector<Artifact> collectOutputs(const QString &jobDirectory) const override;
    void cleanupInterrupted(const ProcessingJob &job) override;

    // Pure helpers, unit-tested without Docker.
    static QString imageReference();                       // "opendronemap/odm@sha256:…"
    static QStringList stageIds();                         // ODM 3.6 pipeline order
    static QString containerName(const QString &jobId);
    static QStringList runArguments(const ProcessingJob &job, const QString &jobDirectory, bool windowsHost);
    static QString parseStage(const QString &line);        // "opensfm" for "[INFO] Running opensfm stage"
    static QString stripAnsi(const QString &line);
    static bool validateExtraArguments(const QStringList &arguments, QString *error);
    static int concurrencyFor(int cpus, qint64 memoryBytes);

private:
    using DockerDone = std::function<void(int exitCode, const QByteArray &output)>;
    void runDocker(const QStringList &arguments, int timeoutMs, DockerDone done);
    void finishEnvironment(EnvironmentReport report);
    void handleOutput();
    void handleRunFinished(int exitCode, QProcess::ExitStatus status);
    void forceRemoveContainer(const QString &jobId, bool wait);

    QString m_docker;
    QPointer<QProcess> m_run;
    QString m_runningJob;
    bool m_cancelRequested = false;
    QByteArray m_partialLine;
    QString m_stage;
    int m_stageIndex = 0;
    QStringList m_recent;      // last lines of the running job
    QStringList m_errors;      // lines that look like errors, for the failure message
};
