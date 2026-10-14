#pragma once

#include "IProcessingEngine.h"
#include <QDir>
#include <QFileInfo>
#include <QTimer>
#include <memory>

// Scriptable engine for ProcessingService tests: the test decides when stages, log lines and
// the end of a job happen. Nothing here is shown to users as a reconstruction.
class MockEngine final : public IProcessingEngine
{
    Q_OBJECT
public:
    // Survives the engine (the service owns and deletes it).
    struct Record {
        QStringList started, cancelled, aborted, cleaned;
        QString lastDirectory;
        QStringList lastImages;   // file names found in <job>/images at start()
    };

    explicit MockEngine(std::shared_ptr<Record> record) : m_record(std::move(record))
    {
        env.checked = true;
        env.available = true;
        env.summary = QStringLiteral("mock");
        env.cpus = 4;
        env.memoryBytes = 8LL << 30;
        env.backend = QStringLiteral("mock");
    }

    EnvironmentReport env;
    bool failStart = false;

    EngineCapabilities capabilities() const override
    {
        EngineCapabilities c;
        c.id = QStringLiteral("mock");
        c.displayName = QStringLiteral("Mock");
        c.version = QStringLiteral("1.0");
        c.outputs = {QStringLiteral("orthophoto"), QStringLiteral("report")};
        c.inputSuffixes = {QStringLiteral("jpg"), QStringLiteral("tif")};
        c.minimumImages = 2;
        return c;
    }
    QVector<ProcessingProfile> profiles() const override
    {
        return {{QStringLiteral("fast"), QStringLiteral("Быстрый"), QString(), {QStringLiteral("--fast")}, 2},
                {QStringLiteral("part"), QStringLiteral("Частично"), QString(), {QStringLiteral("--partial"), QStringLiteral("--end-with"), QStringLiteral("x")}, 1}};
    }
    QString stageTitle(const QString &stage) const override { return QStringLiteral("Этап ") + stage; }
    void validateEnvironment() override
    {
        QTimer::singleShot(0, this, [this] { emit environmentChecked(env); });
    }
    bool createJob(ProcessingJob &job, const JobRequest &request, const EnvironmentReport &env, QString *error) override
    {
        for (const QString &a : request.extraArguments) {
            if (a != QLatin1String("--ok")) {
                if (error) *error = QStringLiteral("bad option ") + a;
                return false;
            }
        }
        QStringList fromOptions;
        for (auto it = request.options.cbegin(); it != request.options.cend(); ++it) {
            if (it.key() != QLatin1String("level")) {
                if (error) *error = QStringLiteral("bad option ") + it.key();
                return false;
            }
            fromOptions << QStringLiteral("--level") << it.value().toString();
        }
        job.engine = QStringLiteral("mock");
        job.engineVersion = QStringLiteral("1.0");
        job.backend = env.backend;
        job.arguments = request.profile->arguments + fromOptions + request.extraArguments;
        job.notes.clear();
        if (request.dataset.withGps == 0) job.notes << QStringLiteral("no gps");
        job.stageCount = 3;
        lastDataset = request.dataset;
        return true;
    }
    QStringList resumeArguments(const QStringList &arguments) const override
    {
        QStringList args = arguments;
        args.removeAll(QStringLiteral("--partial"));
        const int i = args.indexOf(QStringLiteral("--end-with"));
        if (i >= 0) args.remove(i, qMin(2, int(args.size()) - i));
        return args;
    }
    QVariantMap qualityReport(const QString &jobDirectory) const override
    {
        return QFileInfo::exists(jobDirectory + QStringLiteral("/stats.json")) ? QVariantMap{{"alignedImages", 2}, {"totalImages", 2}}
                                                                              : QVariantMap();
    }
    DatasetSummary lastDataset;
    bool start(const ProcessingJob &job, const QString &jobDirectory, QString *error) override
    {
        if (failStart) {
            if (error) *error = QStringLiteral("cannot start");
            return false;
        }
        m_record->started << job.id;
        m_record->lastDirectory = jobDirectory;
        m_record->lastImages = QDir(jobDirectory + QStringLiteral("/images")).entryList(QDir::Files, QDir::Name);
        m_running = job.id;
        emit logLine(job.id, QStringLiteral("mock started"));
        return true;
    }
    void cancel(const QString &jobId) override
    {
        m_record->cancelled << jobId;
        QTimer::singleShot(0, this, [this, jobId] {
            if (m_running == jobId) finish(JobOutcome::Cancelled, 130, QStringLiteral("stopped"));
        });
    }
    void abort(const QString &jobId) override
    {
        if (m_running != jobId) return;
        m_record->aborted << jobId;
        m_running.clear();
    }
    bool isRunning(const QString &jobId) const override { return m_running == jobId; }
    JobProgress readProgress(const QString &) const override { return {}; }
    QVector<Artifact> collectOutputs(const QString &jobDirectory) const override
    {
        QVector<Artifact> outputs;
        for (const QFileInfo &f : QDir(jobDirectory + QStringLiteral("/out")).entryInfoList(QDir::Files, QDir::Name)) {
            Artifact a;
            a.kind = f.suffix() == QLatin1String("pdf") ? QStringLiteral("report") : QStringLiteral("orthophoto");
            a.file = QStringLiteral("out/") + f.fileName();
            a.bytes = f.size();
            outputs << a;
        }
        return outputs;
    }
    void cleanupInterrupted(const ProcessingJob &job) override { m_record->cleaned << job.id; }

    // Test drivers.
    QString running() const { return m_running; }
    void stage(const QString &id, int index) { emit stageChanged(m_running, id, index, 3); }
    void log(const QString &line) { emit logLine(m_running, line); }
    void finish(JobOutcome outcome, int exitCode, const QString &message)
    {
        const QString id = m_running;
        m_running.clear();
        emit finished(id, outcome, exitCode, message);
    }

private:
    std::shared_ptr<Record> m_record;
    QString m_running;
};
