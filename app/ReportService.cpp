#include "ReportService.h"
#include "IProcessingEngine.h"
#include "PhotoModel.h"
#include "ProcessingService.h"
#include "ProjectManager.h"
#include "ProjectReport.h"
#include "SurveyControl.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QThreadPool>

ReportService::ReportService(ProjectManager *projects, ProcessingService *processing, SurveyControl *survey, QObject *parent)
    : QObject(parent), m_projects(projects), m_processing(processing), m_survey(survey)
{
}

QString ReportService::reportOf(const QString &jobId) const
{
    if (!m_processing) return {};
    const QString id = jobId.isEmpty() ? m_processing->newestJob(QStringLiteral("aligned")) : jobId;
    if (id.isEmpty()) return {};
    const QString pdf = QDir(m_processing->jobDirectory(id)).filePath(QStringLiteral("geomap/report.pdf"));
    return QFileInfo::exists(pdf) ? pdf : QString();
}

QString ReportService::build(const QString &jobIdIn)
{
    if (m_busy) return QStringLiteral("Отчёт уже строится");
    if (!m_processing || !m_projects || !m_projects->hasProject()) return QStringLiteral("Проект не открыт");
    const QString id = jobIdIn.isEmpty() ? m_processing->newestJob(QStringLiteral("aligned")) : jobIdIn;
    if (id.isEmpty()) return QStringLiteral("Нет завершённого задания с выравниванием");
    const ProcessingJob job = m_processing->job(id);
    if (job.id.isEmpty()) return QStringLiteral("Задание не найдено");

    projectreport::Input in;
    in.appVersion = QCoreApplication::applicationVersion();
    in.engine = m_processing->engineName() + QLatin1Char(' ') + m_processing->engineVersion();
    in.project = QFileInfo(m_projects->projectPath()).fileName();
    in.jobId = id;
    in.jobDirectory = m_processing->jobDirectory(id);
    in.profile = job.profile;
    if (IProcessingEngine *engine = m_processing->engine()) {
        for (const ProcessingProfile &p : engine->profiles())
            if (p.id == job.profile) in.profile = p.title;
        in.quality = engine->qualityReport(in.jobDirectory);
    }
    in.started = job.startedAt;
    in.finished = job.finishedAt;
    in.arguments = job.arguments;
    in.outputs = job.outputs;
    if (m_survey) in.accuracy = m_survey->accuracy(id);
    const QString project = m_projects->projectPath();
    const QVector<Photo> &photos = m_projects->photoModel()->photos();
    for (int i = 0; i < photos.size(); ++i) {
        const Photo &p = photos[i];
        if (!job.images.isEmpty() && !job.images.contains(p.file)) continue;   // the job's photos
        surveylayout::PhotoIn photo;
        photo.row = i;
        photo.name = p.name;
        photo.path = QDir(project).filePath(p.file);
        photo.takenAt = p.takenAt;
        photo.hasGps = p.hasGps;
        photo.latitude = p.latitude;
        photo.longitude = p.longitude;
        in.photos << photo;
    }
    const QString pdf = QDir(in.jobDirectory).filePath(QStringLiteral("geomap/report.pdf"));
    m_busy = true;
    emit busyChanged();
    QPointer<ReportService> self(this);
    QThreadPool::globalInstance()->start([self, in, pdf, id] {
        const QString error = projectreport::write(in, pdf);
        QMetaObject::invokeMethod(qApp, [self, error, pdf, id] {
            if (!self) return;
            self->m_busy = false;
            emit self->busyChanged();
            if (self->m_processing) self->m_processing->refreshOutputs(id);
            emit self->finished(error.isEmpty(), error.isEmpty() ? QStringLiteral("Отчёт готов: %1").arg(QDir::toNativeSeparators(pdf)) : error,
                                error.isEmpty() ? pdf : QString());
        }, Qt::QueuedConnection);
    });
    return {};
}
