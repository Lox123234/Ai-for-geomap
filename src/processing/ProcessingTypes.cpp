#include "ProcessingTypes.h"
#include <QJsonArray>

namespace {
struct StateName { JobState state; const char *id; const char *text; };
const StateName States[] = {
    {JobState::Queued, "queued", "В очереди"},
    {JobState::Preparing, "preparing", "Подготовка"},
    {JobState::Running, "running", "Выполняется"},
    {JobState::Cancelling, "cancelling", "Отмена…"},
    {JobState::Completed, "completed", "Завершено"},
    {JobState::Failed, "failed", "Ошибка"},
    {JobState::Cancelled, "cancelled", "Отменено"},
    {JobState::Interrupted, "interrupted", "Прервано"},
};
QString dateToJson(const QDateTime &d) { return d.isValid() ? d.toUTC().toString(Qt::ISODateWithMs) : QString(); }
QDateTime dateFromJson(const QJsonValue &v) { return QDateTime::fromString(v.toString(), Qt::ISODateWithMs); }
} // namespace

QString jobStateId(JobState state)
{
    for (const StateName &s : States) if (s.state == state) return QString::fromLatin1(s.id);
    return QStringLiteral("failed");
}

JobState jobStateFromId(const QString &id, bool *ok)
{
    for (const StateName &s : States) {
        if (id == QLatin1String(s.id)) {
            if (ok) *ok = true;
            return s.state;
        }
    }
    if (ok) *ok = false;
    return JobState::Failed;
}

QString jobStateText(JobState state)
{
    for (const StateName &s : States) if (s.state == state) return QString::fromUtf8(s.text);
    return {};
}

bool isActive(JobState state)
{
    return state == JobState::Queued || state == JobState::Preparing
        || state == JobState::Running || state == JobState::Cancelling;
}

QJsonObject Artifact::toJson() const
{
    return {{"kind", kind}, {"file", file}, {"bytes", double(bytes)}, {"details", details}};
}

Artifact Artifact::fromJson(const QJsonObject &o)
{
    Artifact a;
    a.kind = o.value("kind").toString();
    a.file = o.value("file").toString();
    a.bytes = qint64(o.value("bytes").toDouble());
    a.details = o.value("details").toString();
    return a;
}

QJsonObject ProcessingJob::toJson() const
{
    QJsonArray outputArray;
    for (const Artifact &a : outputs) outputArray.append(a.toJson());
    QJsonObject o{
        {"schemaVersion", 1}, {"id", id}, {"engine", engine}, {"engineVersion", engineVersion},
        {"backend", backend}, {"profile", profile}, {"arguments", QJsonArray::fromStringList(arguments)},
        {"notes", QJsonArray::fromStringList(notes)}, {"runs", runs},
        {"state", jobStateId(state)}, {"message", message},
        {"stage", stage}, {"stageIndex", stageIndex}, {"stageCount", stageCount},
        {"createdAt", dateToJson(createdAt)}, {"startedAt", dateToJson(startedAt)},
        {"finishedAt", dateToJson(finishedAt)}, {"images", QJsonArray::fromStringList(images)},
        {"exitCode", exitCode}, {"outputs", outputArray}
    };
    if (!chunk.isEmpty()) o.insert("chunk", chunk);
    return o;
}

ProcessingJob ProcessingJob::fromJson(const QJsonObject &o, bool *ok)
{
    ProcessingJob job;
    bool stateOk = false;
    job.id = o.value("id").toString();
    job.engine = o.value("engine").toString();
    job.engineVersion = o.value("engineVersion").toString();
    // "docker" for jobs written by 0.2.0, which had no other runtime.
    job.backend = o.value("backend").toString(QStringLiteral("docker"));
    job.profile = o.value("profile").toString();
    for (const QJsonValue &v : o.value("arguments").toArray()) job.arguments << v.toString();
    for (const QJsonValue &v : o.value("notes").toArray()) job.notes << v.toString();
    job.runs = o.value("runs").toInt(1);
    job.state = jobStateFromId(o.value("state").toString(), &stateOk);
    job.message = o.value("message").toString();
    job.stage = o.value("stage").toString();
    job.stageIndex = o.value("stageIndex").toInt();
    job.stageCount = o.value("stageCount").toInt();
    job.createdAt = dateFromJson(o.value("createdAt"));
    job.startedAt = dateFromJson(o.value("startedAt"));
    job.finishedAt = dateFromJson(o.value("finishedAt"));
    for (const QJsonValue &v : o.value("images").toArray()) job.images << v.toString();
    job.chunk = o.value("chunk").toString();
    job.exitCode = o.value("exitCode").toInt();
    for (const QJsonValue &v : o.value("outputs").toArray()) job.outputs << Artifact::fromJson(v.toObject());
    if (ok) *ok = stateOk && o.value("schemaVersion").toInt() == 1 && !job.id.isEmpty();
    return job;
}
