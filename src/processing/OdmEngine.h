#pragma once

#include "IProcessingEngine.h"
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <functional>
#include <memory>

class Downloader;
class ProcessTree;

// OpenDroneMap 3.6.2, run as a separate program in one of two runtimes:
//  - "native": the official Windows build (ODM_Setup_3.6.2.exe, signed, pinned by SHA-256),
//    bundled with GeoMap AI or installed by it per user; no Docker, no WSL, no admin rights;
//  - "docker": the official image pinned by digest (optional, e.g. on Linux).
// Commands are argument lists (no shell). Input photos are never modified.
class OdmEngine final : public IProcessingEngine
{
    Q_OBJECT
public:
    static constexpr auto Version = "3.6.2";
    static constexpr auto ImageTag = "opendronemap/odm:3.6.2";
    static constexpr auto ImageDigest = "sha256:94a0dd2714be4e8b565bdabd8f67e0191f36e37d82c179433adcb0a403aac3bd";
    static constexpr auto NativeInstallerUrl = "https://github.com/OpenDroneMap/ODM/releases/download/v3.6.2/ODM_Setup_3.6.2.exe";
    static constexpr auto NativeInstallerSha256 = "c9dbfabf0d066529a7b6c753fd45bf3366a0314266160cd677373bab4caff294";
    static constexpr qint64 NativeInstallerBytes = 244864104;
    static constexpr auto NativeAppId = "{443998BA-9F8F-4A69-9A96-0D8FBC8C6393}";   // ODM's Inno Setup AppId

    explicit OdmEngine(QObject *parent = nullptr);
    ~OdmEngine() override;   // stops a running job and its process tree / container

    void setDockerPath(const QString &path) { m_docker = path; }
    QString dockerPath() const { return m_docker; }
    static QString findDocker();
    void setNativeRoot(const QString &root) { m_nativeRoot = root; }   // instead of the search
    QString nativeRoot() const { return m_nativeRoot; }
    QString backend() const { return m_backend; }

    EngineCapabilities capabilities() const override;
    QVector<ProcessingProfile> profiles() const override;
    QString stageTitle(const QString &stage) const override;
    QStringList resourceWarnings(int imageCount, const ProcessingProfile &profile,
                                 const EnvironmentReport &env) const override;
    QVariantList optionDefinitions(const EnvironmentReport &env) const override;
    void setPreferredBackend(const QString &backend) override { m_preferred = backend; }
    void validateEnvironment() override;
    void installRuntime() override;
    void cancelInstall() override;
    bool createJob(ProcessingJob &job, const JobRequest &request, const EnvironmentReport &env, QString *error) override;
    QStringList resumeArguments(const QStringList &arguments) const override;
    bool start(const ProcessingJob &job, const QString &jobDirectory, QString *error) override;
    void cancel(const QString &jobId) override;
    void abort(const QString &jobId) override;
    bool isRunning(const QString &jobId) const override;
    JobProgress readProgress(const QString &jobId) const override;
    QVector<Artifact> collectOutputs(const QString &jobDirectory) const override;
    QVariantMap qualityReport(const QString &jobDirectory) const override;
    void cleanupInterrupted(const ProcessingJob &job) override;

    // Pure helpers, unit-tested without ODM or Docker.
    static QString imageReference();                       // "opendronemap/odm@sha256:…"
    static QStringList stageIds();                         // ODM 3.6 pipeline order
    static QString containerName(const QString &jobId);
    static QStringList runArguments(const ProcessingJob &job, const QString &jobDirectory, bool windowsHost);
    static QString parseStage(const QString &line);        // "opensfm" for "[INFO] Running opensfm stage"
    static QString stripAnsi(const QString &line);
    static bool validateExtraArguments(const QStringList &arguments, QString *error);
    static QStringList optionArguments(const QVariantMap &options, QString *error);
    static int concurrencyFor(int cpus, qint64 memoryBytes);
    static QVariantMap parseQualityStats(const QByteArray &statsJson);

    // Native runtime. A root is the folder of an ODM installation (run.py, venv\, SuperBuild\).
    static QStringList nativeCandidates();                 // in search order, may not exist
    static QString nativeProblem(const QString &root);     // empty if the files are complete
    static QString managedNativeRoot();                    // where GeoMap AI installs ODM
    static QString registeredNativeInstall(QString *version = nullptr);   // ODM's own installer
    static QProcessEnvironment nativeEnvironment(const QString &root, const QString &cacheDirectory,
                                                 const QProcessEnvironment &base);
    static QStringList nativeArguments(const ProcessingJob &job, const QString &root, const QString &projectPath);
    // Re-points venv\pyvenv.cfg and OpenCV's loader config at `root` (what ODM's
    // winpostinstall.bat does), so an installation keeps working after it was moved.
    static bool patchNativeVirtualEnv(const QString &root, QString *error);
    static QString nativePython(const QString &root);
    // `root` itself if its path is ASCII-only, else a directory link to it in an ASCII folder.
    static QString effectiveNativeRoot(const QString &root, QString *error);

private:
    using DoneCallback = std::function<void(int exitCode, const QByteArray &output)>;
    void runTool(const QString &program, const QStringList &arguments, int timeoutMs, DoneCallback done,
                 const QProcessEnvironment &environment = {});
    void runDocker(const QStringList &arguments, int timeoutMs, DoneCallback done);
    void checkNative(EnvironmentReport report, const QString &root);
    void checkDocker(EnvironmentReport report, bool fallback);
    void finishEnvironment(EnvironmentReport report, const QString &installKind = {});
    static QString nativeInstallPrompt();
    void installNative();
    void runNativeInstaller(const QString &installer);
    void verifyNativeInstall(const QString &root);
    void finishInstall(bool ok, const QString &message);
    void handleOutput();
    void handleRunFinished(int exitCode, QProcess::ExitStatus status);
    void forceRemoveContainer(const QString &jobId, bool wait);
    void releaseRun();
    QString workLinkFor(const QString &jobId) const;

    QString m_docker;
    QString m_nativeRoot;
    QString m_preferred;      // "", "native" or "docker"
    QString m_backend;        // chosen by the last environment check
    QString m_installKind;    // "native" or "docker"
    EnvironmentReport m_lastReport;
    QPointer<Downloader> m_download;
    QPointer<QProcess> m_installer;
    bool m_installCancelled = false;

    QPointer<QProcess> m_run;
    std::unique_ptr<ProcessTree> m_tree;
    QString m_runningJob;
    QString m_runBackend;
    QString m_workLink;       // tool-safe alias of the job directory (native runtime)
    bool m_cancelRequested = false;
    QByteArray m_partialLine;
    QString m_stage;
    int m_stageIndex = 0;
    QStringList m_recent;      // last lines of the running job
    QStringList m_errors;      // lines that look like errors, for the failure message
};
