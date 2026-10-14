#pragma once

#include "ProcessingTypes.h"
#include <QObject>

// A reconstruction engine (ODM in a container, later native engines), independent of the UI.
// ProcessingService owns job state and persistence; an engine runs one prepared job at a time
// in its job directory and reports what really happens. Engines never invent progress.
class IProcessingEngine : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    ~IProcessingEngine() override = default;

    virtual EngineCapabilities capabilities() const = 0;
    virtual QVector<ProcessingProfile> profiles() const = 0;
    // UI name of an engine stage id reported by stageChanged().
    virtual QString stageTitle(const QString &stage) const { return stage; }
    // Non-blocking hints about resources for a job of this size (memory, time).
    virtual QStringList resourceWarnings(int imageCount, const ProcessingProfile &profile,
                                         const EnvironmentReport &env) const
    {
        Q_UNUSED(imageCount) Q_UNUSED(profile) Q_UNUSED(env)
        return {};
    }

    // Asynchronous: emits environmentChecked(). Must not block the caller.
    virtual void validateEnvironment() = 0;
    // Downloads what validateEnvironment() reported as missing (only on user request).
    virtual void installRuntime() { emit installFinished(false, tr("Not supported by this engine")); }

    // Engine-specific part of job creation: final arguments for the profile and any extra,
    // already validated options. Returns false with an error for options it cannot accept.
    virtual bool createJob(ProcessingJob &job, const ProcessingProfile &profile,
                           const QStringList &extraArguments, const EnvironmentReport &env,
                           QString *error) = 0;

    // Starts the job prepared in jobDirectory (input images in jobDirectory/images).
    virtual bool start(const ProcessingJob &job, const QString &jobDirectory, QString *error) = 0;
    // Asynchronous stop; finished() follows with JobOutcome::Cancelled.
    virtual void cancel(const QString &jobId) = 0;
    // Synchronous stop without finished() (application exit, project closed).
    virtual void abort(const QString &jobId) = 0;
    virtual bool isRunning(const QString &jobId) const = 0;
    // Current stage and the most recent log lines of a running job.
    virtual JobProgress readProgress(const QString &jobId) const = 0;
    // Results present in a finished job directory, with verified size and properties.
    virtual QVector<Artifact> collectOutputs(const QString &jobDirectory) const = 0;
    // Called for jobs that were active when the application stopped (e.g. stop a container).
    virtual void cleanupInterrupted(const ProcessingJob &job) { Q_UNUSED(job) }

signals:
    void environmentChecked(const EnvironmentReport &report);
    void installProgress(const QString &line);
    void installFinished(bool ok, const QString &message);
    // Real engine stage; index is the 1-based position in the engine's stage list.
    void stageChanged(const QString &jobId, const QString &stage, int index, int count);
    void logLine(const QString &jobId, const QString &line);
    void finished(const QString &jobId, JobOutcome outcome, int exitCode, const QString &message);
};
