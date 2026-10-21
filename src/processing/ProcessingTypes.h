#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

// Data exchanged between ProcessingService and processing engines (no UI dependency).

enum class JobState {
    Queued,
    Preparing,
    Running,
    Cancelling,
    Completed,
    Failed,
    Cancelled,
    Interrupted   // the application stopped while the job was active
};

QString jobStateId(JobState state);            // "queued", ... (stored in job.json)
JobState jobStateFromId(const QString &id, bool *ok = nullptr);
QString jobStateText(JobState state);          // Russian UI text
bool isActive(JobState state);                 // queued, preparing, running, cancelling

enum class JobOutcome { Succeeded, Failed, Cancelled };

struct EngineCapabilities
{
    QString id;                 // "odm"
    QString displayName;        // "OpenDroneMap"
    QString version;            // engine version the adapter targets
    bool supportsCancel = true;
    bool supportsPause = false; // pause/resume only if the engine really supports it
    QStringList outputs;        // artifact kinds the engine may produce
    QStringList inputSuffixes;  // accepted photo file suffixes, lower case without the dot
    int minimumImages = 2;
};

struct EnvironmentReport
{
    bool checked = false;
    bool available = false;     // everything needed to start a job is present
    bool engineMissing = false; // nothing to run the engine with
    bool imageMissing = false;  // the engine is missing from the program (can be restored)
    QString engineVersion;
    QString summary;            // one line for the UI
    QStringList problems;       // blocking
    QStringList warnings;       // non-blocking (e.g. little memory)
    QStringList hints;          // what the user can do
    int cpus = 0;
    qint64 memoryBytes = 0;
    QString backend;            // "native" when the built-in engine is ready; empty if none
    QString gpuName;            // usable GPU reported by the driver, empty if none
    bool canInstall = false;    // installRuntime() can fix what is missing
    QString installTitle;       // button text
    QString installPrompt;      // confirmation text: what is downloaded, size, license
};

// What the project offers to a job: used to adapt engine parameters (e.g. no GPS).
struct DatasetSummary
{
    int images = 0;
    int withGps = 0;
    qint64 bytes = 0;
};

struct ProcessingProfile;

// Everything the user chose for a new job.
struct JobRequest
{
    const ProcessingProfile *profile = nullptr;
    QVariantMap options;        // structured options (engine option definitions)
    QStringList extraArguments; // expert command-line options
    DatasetSummary dataset;
};

struct ProcessingProfile
{
    QString id;                 // fast | balanced | high
    QString title;
    QString description;
    QStringList arguments;      // engine arguments, documented in docs/PROCESSING_ODM.md
    double diskFactor = 3;      // rough output size relative to the input photos
};

struct JobProgress
{
    QString stage;              // engine stage id, empty before the first stage
    int stageIndex = 0;         // 1-based position in the engine's stage list
    int stageCount = 0;
    QStringList recentLog;      // last lines, oldest first
};

struct Artifact
{
    QString kind;               // orthophoto, dsm, dtm, pointcloud, mesh, report
    QString file;               // relative to the job directory, '/' separators
    qint64 bytes = 0;
    QString details;            // e.g. "EPSG:32630, 5,0 см/пикс, 2048×1536, каналов: 4"
    QJsonObject toJson() const;
    static Artifact fromJson(const QJsonObject &o);
};

struct ProcessingJob
{
    QString id;
    QString engine;
    QString engineVersion;
    QString backend;            // runtime of the job: "native" ("docker" only in jobs of 0.2.0)
    QString profile;
    QStringList arguments;      // final engine arguments
    QStringList notes;          // automatic adjustments of the request, shown and logged
    int runs = 0;               // starts so far (resuming continues in the same directory)
    JobState state = JobState::Queued;
    QString message;            // last status / diagnostic message
    QString stage;              // engine stage id, e.g. "opensfm"
    int stageIndex = 0;         // 1-based position in the engine's known stage list, 0 = none
    int stageCount = 0;
    QDateTime createdAt;
    QDateTime startedAt;
    QDateTime finishedAt;
    QStringList images;         // project photo files ("photos/<id>.jpg") used as input
    QString chunk;              // id of the project's chunk the job processes ("" in jobs before 0.7.0: the first chunk)
    int exitCode = 0;
    QVector<Artifact> outputs;

    QJsonObject toJson() const;
    static ProcessingJob fromJson(const QJsonObject &o, bool *ok = nullptr);
};

Q_DECLARE_METATYPE(EnvironmentReport)
