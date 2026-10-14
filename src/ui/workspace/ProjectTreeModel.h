#pragma once

#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QTimer>

class MapService;
class ProcessingService;
class ProjectManager;

// Tree of the "Проект" panel, built from what the services hold: the photos, the newest
// result of every kind among the processing jobs (orthophoto, DSM, DTM, point cloud, model,
// report), the map layers and the jobs. Sections that do not exist yet (calibrations, GCP)
// are shown as unavailable with their stage. Fixed rows are updated in place and only the
// layer and job lists are replaced, so the view keeps its expanded branches.
class ProjectTreeModel : public QStandardItemModel
{
    Q_OBJECT
public:
    enum Roles {
        TitleRole = Qt::UserRole + 1,
        KindRole,        // project, chunk, photos, calibrations, gcp, orthophoto, dsm, dtm, pointcloud, mesh, report, layers, layer, jobs, job
        StatusRole,      // short state text
        IconRole,        // icon file name (qml/icons)
        AvailableRole,   // false for sections that are not implemented or have no data
        PayloadRole,     // absolute file path (results), layer id or job id
        JobRole,         // job id of a result
        FileRole         // result file relative to its job folder
    };

    ProjectTreeModel(ProjectManager *projects, ProcessingService *processing, MapService *map, QObject *parent = nullptr);
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void rebuild();   // new project (or none)
    Q_INVOKABLE void refresh();   // counts, results, layers, jobs
    QStandardItem *section(const QString &kind) const;
    // Newest result of a kind (orthophoto, dsm, dtm, pointcloud, mesh, report): {path, jobId, file}, or {}.
    Q_INVOKABLE QVariantMap result(const QString &kind) const;

private:
    QStandardItem *addSection(QStandardItem *parent, const QString &kind, const QString &title, const QString &icon);
    void setStatus(QStandardItem *item, const QString &status, bool available);
    void scheduleRefresh();

    ProjectManager *m_projects;
    ProcessingService *m_processing;
    MapService *m_map;
    QTimer m_refreshTimer;
};

// Search field of the tree: keeps the rows whose title contains the text, with their parents.
class ProjectTreeFilter : public QSortFilterProxyModel
{
    Q_OBJECT
    Q_PROPERTY(QString filterText READ filterText WRITE setFilterText NOTIFY filterTextChanged)
public:
    explicit ProjectTreeFilter(QObject *parent = nullptr);
    QString filterText() const { return m_text; }
    void setFilterText(const QString &text);

signals:
    void filterTextChanged();

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;

private:
    QString m_text;
};
