#pragma once

#include "ProjectReport.h"

#include <QObject>
#include <QPointer>
#include <QString>

class ProcessingService;
class ProjectManager;
class SurveyControl;

// Обработка → Генерация отчёта: GeoMap AI's PDF report of a job (projectreport) into
// <job>/geomap/report.pdf, registered with the job's results.
class ReportService final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool openWhenDone MEMBER m_openWhenDone CONSTANT)   // the window opens the PDF (not in tests)
public:
    ReportService(ProjectManager *projects, ProcessingService *processing, SurveyControl *survey, QObject *parent = nullptr);
    bool busy() const { return m_busy; }
    // The newest completed job with a reconstruction when jobId is empty. "" when started.
    Q_INVOKABLE QString build(const QString &jobId = {});
    Q_INVOKABLE QString reportOf(const QString &jobId = {}) const;   // the PDF if written, else ""
    // What the report of a job shows (job, quality, photos, control points, results); its jobId empty
    // with the reason in `error` when there is no such job.
    projectreport::Input inputFor(const QString &jobId, QString *error = nullptr) const;

signals:
    void busyChanged();
    void finished(bool ok, const QString &message, const QString &path);

private:
    QPointer<ProjectManager> m_projects;
    QPointer<ProcessingService> m_processing;
    QPointer<SurveyControl> m_survey;
    bool m_busy = false;
public:
    bool m_openWhenDone = true;
};
