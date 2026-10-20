#pragma once

#include "ProcessingService.h"
#include "ProjectManager.h"
#include "SurveyControl.h"
#include "SurveyLayout.h"

#include <QPointer>
#include <QQuickPaintedItem>
#include <QVariantList>
#include <QVariantMap>
#include <memory>

// «Схема съёмки» (the GPS tab): the survey as Agisoft Metashape's Reference view shows it — every
// photo where its GPS put it and where the alignment of a job placed it, the error between them
// (a vector, exaggerated; its colour the error in height), the photos' footprints on the ground,
// the flight path in capture order, the control and check points with their errors, over the
// job's orthophoto. A click selects a photo (the project's selection), a double click opens it;
// in the placing mode a click puts a control point there (X, Y from the click, Z from the DSM),
// which the photo view then shows on every photo where it should be.
class SurveyView : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(ProjectManager *projectManager READ projects WRITE setProjects NOTIFY sourcesChanged)
    Q_PROPERTY(ProcessingService *processingService READ processing WRITE setProcessing NOTIFY sourcesChanged)
    Q_PROPERTY(SurveyControl *surveyControl READ survey WRITE setSurvey NOTIFY sourcesChanged)
    Q_PROPERTY(QString jobId READ jobId WRITE setJobId NOTIFY jobIdChanged)            // "" = the newest aligned job
    Q_PROPERTY(QVariantList jobs READ jobs NOTIFY layoutChanged)                       // {id, title} of aligned jobs
    Q_PROPERTY(QString shownJob READ shownJob NOTIFY layoutChanged)                    // job shown ("" before alignment)
    Q_PROPERTY(bool busy READ busy NOTIFY layoutChanged)
    Q_PROPERTY(QString status READ status NOTIFY layoutChanged)                        // why there is nothing to show
    Q_PROPERTY(QVariantMap summary READ summary NOTIFY layoutChanged)
    Q_PROPERTY(QVariantList cameraRows READ cameraRows NOTIFY layoutChanged)          // the table of cameras, worst first
    Q_PROPERTY(QVariantList pointRows READ pointRows NOTIFY pointsChanged)            // control and check points
    Q_PROPERTY(QVariantMap pointSummary READ pointSummary NOTIFY pointsChanged)
    Q_PROPERTY(bool showBackground MEMBER m_showBackground NOTIFY optionsChanged)
    Q_PROPERTY(bool showFootprints MEMBER m_showFootprints NOTIFY optionsChanged)
    Q_PROPERTY(bool showPath MEMBER m_showPath NOTIFY optionsChanged)
    Q_PROPERTY(bool showLabels MEMBER m_showLabels NOTIFY optionsChanged)
    Q_PROPERTY(double errorScale MEMBER m_errorScale NOTIFY optionsChanged)            // 0 = automatic
    Q_PROPERTY(double effectiveErrorScale READ effectiveErrorScale NOTIFY optionsChanged)
    Q_PROPERTY(bool placing MEMBER m_placing NOTIFY optionsChanged)                    // a click puts a control point
    Q_PROPERTY(int selectedRow READ selectedRow WRITE setSelectedRow NOTIFY selectedRowChanged)
    Q_PROPERTY(QString scaleText READ scaleText NOTIFY viewChanged)

public:
    explicit SurveyView(QQuickItem *parent = nullptr);
    ~SurveyView() override;

    ProjectManager *projects() const { return m_projects; }
    void setProjects(ProjectManager *p);
    ProcessingService *processing() const { return m_processing; }
    void setProcessing(ProcessingService *p);
    SurveyControl *survey() const { return m_survey; }
    void setSurvey(SurveyControl *s);
    QString jobId() const { return m_jobId; }
    void setJobId(const QString &id);
    QVariantList jobs() const { return m_jobs; }
    QString shownJob() const { return m_layout ? m_layout->job : QString(); }
    bool busy() const { return m_busy; }
    QString status() const;
    QVariantMap summary() const;
    QVariantList cameraRows() const;
    QVariantList pointRows() const { return m_pointRows; }
    QVariantMap pointSummary() const { return m_pointSummary; }
    double effectiveErrorScale() const;
    int selectedRow() const { return m_selectedRow; }
    void setSelectedRow(int row);
    QString scaleText() const { return m_scaleText; }

    Q_INVOKABLE void refresh();     // recomputes (photos, jobs or points changed)
    Q_INVOKABLE void fitAll();
    Q_INVOKABLE void zoomToRow(int row);

    void paint(QPainter *painter) override;

    // For tests: the screen position of a photo (estimated if aligned, else GPS); (-1, -1) if none.
    Q_INVOKABLE QPointF screenPositionOf(int row) const;
    // For tests and the placing mode: a control point at a screen position. Its id, or "" (and
    // message()) when it cannot be placed.
    Q_INVOKABLE QString placePointAt(const QPointF &screen);

signals:
    void sourcesChanged();
    void jobIdChanged();
    void layoutChanged();
    void pointsChanged();
    void optionsChanged();
    void selectedRowChanged();
    void viewChanged();
    void photoActivated(int row);                 // double click
    void pointPlaced(const QString &id, const QString &name, bool heightFromModel);
    void message(const QString &text);

protected:
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    QPointF toScreen(double x, double y) const;
    QPointF toMap(const QPointF &s) const;
    int cameraAt(const QPointF &screen) const;   // index in m_layout->cameras, -1 none
    void updatePoints();                         // control points in the layout's CRS and their errors
    void updateScaleText();
    QString jobDirectoryOf(const QString &id) const;
    QString newestAlignedJob() const;

    QPointer<ProjectManager> m_projects;
    QPointer<ProcessingService> m_processing;
    QPointer<SurveyControl> m_survey;
    QString m_jobId;
    QVariantList m_jobs;
    std::shared_ptr<const surveylayout::Layout> m_layout;
    bool m_busy = false;
    int m_generation = 0;
    bool m_fitted = false;
    bool m_autoFit = false;   // the view follows its size (fitted, not moved since)
    struct Point { QString id, name; bool check = false; double x = 0, y = 0; bool solved = false; double dx = 0, dy = 0, dz = 0; int photos = 0; };
    QVector<Point> m_points;
    QVariantList m_pointRows;
    QVariantMap m_pointSummary;
    bool m_showBackground = true, m_showFootprints = true, m_showPath = true, m_showLabels = false, m_placing = false;
    double m_errorScale = 0;
    int m_selectedRow = -1;
    int m_hover = -1;
    // View: centre and metres per screen pixel.
    QPointF m_centre;
    double m_resolution = 1;
    QPointF m_pressPos, m_lastPos;
    bool m_dragged = false;
    QString m_scaleText;
};
