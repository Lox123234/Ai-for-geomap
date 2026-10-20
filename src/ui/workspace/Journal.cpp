#include "Journal.h"

#include "AppLog.h"
#include "MapService.h"
#include "ProcessingService.h"
#include "ProcessingTypes.h"
#include "ProjectManager.h"

#include <QHash>
#include <memory>

void connectJournal(AppLog *log, ProjectManager *projects, ProcessingService *processing, MapService *map)
{
    if (!log) return;
    if (projects) {
        QObject::connect(projects, &ProjectManager::statusChanged, log, [log, projects] {
            log->add(AppLog::Info, QStringLiteral("Проект"), projects->statusMessage());
        });
        QObject::connect(projects, &ProjectManager::errorOccurred, log, [log](const QString &message) {
            log->add(AppLog::Error, QStringLiteral("Проект"), message);
        });
    }
    if (processing) {
        QObject::connect(processing, &ProcessingService::errorOccurred, log, [log](const QString &message) {
            log->add(AppLog::Error, QStringLiteral("Обработка"), message);
        });
        // One entry per state change of a job (stage changes are in the job's own log).
        auto states = std::make_shared<QHash<QString, JobState>>();
        // The jobs a project opens with are known in their state: only their changes are news.
        auto project = std::make_shared<QString>();
        QObject::connect(processing, &ProcessingService::jobsChanged, log, [processing, states, project] {
            if (processing->projectDirectory() == *project) return;
            *project = processing->projectDirectory();
            states->clear();
            for (const ProcessingJob &j : processing->jobs()) states->insert(j.id, j.state);
        });
        QObject::connect(processing, &ProcessingService::jobUpdated, log, [log, processing, states](const QString &id) {
            const QVector<ProcessingJob> jobs = processing->jobs();
            for (const ProcessingJob &j : jobs) {
                if (j.id != id) continue;
                const auto it = states->constFind(id);
                if (it != states->constEnd() && *it == j.state) return;
                states->insert(id, j.state);
                const AppLog::Level level = j.state == JobState::Completed ? AppLog::Success
                                          : j.state == JobState::Failed ? AppLog::Error
                                          : (j.state == JobState::Cancelled || j.state == JobState::Interrupted) ? AppLog::Warning
                                          : AppLog::Info;
                QString text = QStringLiteral("Задание %1: %2").arg(j.id, jobStateText(j.state));
                if (!j.message.isEmpty() && !isActive(j.state)) text += QStringLiteral(" — ") + j.message;
                log->add(level, QStringLiteral("Обработка"), text);
                return;
            }
        });
        QObject::connect(processing, &ProcessingService::environmentChanged, log, [log, processing] {
            if (!processing->environmentChecked() || processing->checkingEnvironment()) return;
            log->add(processing->engineAvailable() ? AppLog::Info : AppLog::Warning, QStringLiteral("Обработка"),
                     processing->engineAvailable() ? processing->environmentSummary()
                         : QStringLiteral("Движок недоступен: %1").arg(processing->environmentProblems().join(QStringLiteral("; "))));
        });
        // Installation: its start and its result (download progress stays in the processing view).
        auto installing = std::make_shared<bool>(false);
        QObject::connect(processing, &ProcessingService::installChanged, log, [log, processing, installing] {
            if (processing->installing() == *installing) return;
            *installing = processing->installing();
            log->add(AppLog::Info, QStringLiteral("Установка"),
                     *installing ? QStringLiteral("Начата установка движка") : processing->installStatus());
        });
    }
    if (map) {
        QObject::connect(map, &MapService::errorOccurred, log, [log](const QString &message) {
            log->add(AppLog::Error, QStringLiteral("Карта"), message);
        });
        QObject::connect(map, &MapService::exportFinished, log, [log](bool ok, const QString &message) {
            log->add(ok ? AppLog::Success : AppLog::Error, QStringLiteral("Карта"), message);
        });
    }
}
