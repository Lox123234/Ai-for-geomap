#include "ProjectTreeModel.h"

#include "ChunkService.h"
#include "MapService.h"
#include "PhotoModel.h"
#include "ProcessingService.h"
#include "ProcessingTypes.h"
#include "ProjectManager.h"
#include "SurveyControl.h"

#include <QDir>
#include <QLocale>

namespace {
struct ResultKind
{
    const char *kind;
    const char *title;
    const char *icon;
};

// Newest result of each kind among the jobs; the order is the order of the tree.
constexpr ResultKind Results[] = {
    {"pointcloud", "Плотное облако точек", "cloud"},
    {"mesh", "3D-модель", "cube"},
    {"dsm", "DSM — модель поверхности", "dem"},
    {"dtm", "DTM — модель рельефа", "dem"},
    {"orthophoto", "Ортофотоплан", "map"},
    {"report", "Отчёт обработки", "report"},
};

QString layerIcon(const MapLayer &l)
{
    switch (l.type) {
    case MapLayer::Raster: return QStringLiteral("raster");
    case MapLayer::Vector: return QStringLiteral("vector");
    case MapLayer::Photos: return QStringLiteral("camera");
    case MapLayer::Drawings: return QStringLiteral("polygon");
    }
    return QStringLiteral("layer");
}
} // namespace

ProjectTreeModel::ProjectTreeModel(ProjectManager *projects, ProcessingService *processing, MapService *map, QObject *parent)
    : QStandardItemModel(parent), m_projects(projects), m_processing(processing), m_map(map)
{
    m_refreshTimer.setSingleShot(true);
    m_refreshTimer.setInterval(150);
    connect(&m_refreshTimer, &QTimer::timeout, this, &ProjectTreeModel::refresh);
    if (m_projects) {
        connect(m_projects, &ProjectManager::projectChanged, this, &ProjectTreeModel::rebuild);
        connect(m_projects, &ProjectManager::photosChanged, this, &ProjectTreeModel::scheduleRefresh);
    }
    if (m_processing) {
        connect(m_processing, &ProcessingService::jobsChanged, this, &ProjectTreeModel::scheduleRefresh);
        connect(m_processing, &ProcessingService::jobFinished, this, &ProjectTreeModel::scheduleRefresh);
    }
    if (m_map) {
        connect(m_map, &MapService::layersChanged, this, &ProjectTreeModel::scheduleRefresh);
        connect(m_map, &MapService::projectChanged, this, &ProjectTreeModel::scheduleRefresh);
        connect(m_map->layerModel(), &QAbstractItemModel::dataChanged, this, &ProjectTreeModel::scheduleRefresh);
        connect(m_map, &MapService::markersChanged, this, &ProjectTreeModel::scheduleRefresh);
    }
    rebuild();
}

void ProjectTreeModel::setSurvey(SurveyControl *survey)
{
    m_survey = survey;
    if (m_survey) connect(m_survey, &SurveyControl::changed, this, &ProjectTreeModel::scheduleRefresh);
    scheduleRefresh();
}

void ProjectTreeModel::setChunks(ChunkService *chunks)
{
    m_chunks = chunks;
    if (m_chunks) connect(m_chunks, &ChunkService::changed, this, &ProjectTreeModel::scheduleRefresh);
    scheduleRefresh();
}

QHash<int, QByteArray> ProjectTreeModel::roleNames() const
{
    return {{TitleRole, "title"}, {KindRole, "kind"}, {StatusRole, "status"}, {IconRole, "iconName"},
            {AvailableRole, "available"}, {PayloadRole, "payload"}, {JobRole, "jobId"}, {FileRole, "file"},
            {Qt::DisplayRole, "display"}};
}

void ProjectTreeModel::scheduleRefresh()
{
    m_refreshTimer.start();
}

QStandardItem *ProjectTreeModel::addSection(QStandardItem *parent, const QString &kind, const QString &title, const QString &icon)
{
    auto *item = new QStandardItem(title);
    item->setEditable(false);
    item->setData(title, TitleRole);
    item->setData(kind, KindRole);
    item->setData(icon, IconRole);
    item->setData(true, AvailableRole);
    parent->appendRow(item);
    return item;
}

void ProjectTreeModel::setStatus(QStandardItem *item, const QString &status, bool available)
{
    if (!item) return;
    if (item->data(StatusRole).toString() != status) item->setData(status, StatusRole);
    if (item->data(AvailableRole).toBool() != available) item->setData(available, AvailableRole);
}

QStandardItem *ProjectTreeModel::section(const QString &kind) const
{
    const QModelIndexList found = match(index(0, 0), KindRole, kind, 1, Qt::MatchExactly | Qt::MatchRecursive);
    return found.isEmpty() ? nullptr : itemFromIndex(found.first());
}

QVariantMap ProjectTreeModel::result(const QString &kind) const
{
    const QStandardItem *item = section(kind);
    if (!item || item->data(PayloadRole).toString().isEmpty()) return {};
    return {{"path", item->data(PayloadRole)}, {"jobId", item->data(JobRole)}, {"file", item->data(FileRole)},
            {"status", item->data(StatusRole)}};
}

void ProjectTreeModel::rebuild()
{
    m_refreshTimer.stop();
    clear();
    if (!m_projects || !m_projects->hasProject()) {
        ++m_revision;
        emit revisionChanged();
        return;
    }
    QStandardItem *root = invisibleRootItem();
    QStandardItem *project = addSection(root, QStringLiteral("project"), m_projects->projectName(), QStringLiteral("project"));
    QStandardItem *chunk = addSection(project, QStringLiteral("chunk"), QStringLiteral("Блок 1"), QStringLiteral("chunk"));
    addSection(chunk, QStringLiteral("photos"), QStringLiteral("Фотографии"), QStringLiteral("camera"));
    addSection(chunk, QStringLiteral("calibrations"), QStringLiteral("Калибровки камер"), QStringLiteral("calibration"));
    addSection(chunk, QStringLiteral("gcp"), QStringLiteral("Маркеры и опорные точки"), QStringLiteral("marker"));
    for (const ResultKind &r : Results)
        addSection(chunk, QString::fromLatin1(r.kind), QString::fromUtf8(r.title), QString::fromLatin1(r.icon));
    addSection(project, QStringLiteral("layers"), QStringLiteral("Слои карты"), QStringLiteral("layers"));
    addSection(project, QStringLiteral("jobs"), QStringLiteral("Задания обработки"), QStringLiteral("process"));
    refresh();
}

void ProjectTreeModel::refresh()
{
    QStandardItem *project = section(QStringLiteral("project"));
    if (!project || !m_projects || !m_projects->hasProject()) return;
    const QLocale ru(QLocale::Russian);
    if (project->data(TitleRole).toString() != m_projects->projectName()) {
        project->setData(m_projects->projectName(), TitleRole);
        project->setText(m_projects->projectName());
    }
    setStatus(project, QStringLiteral("%1 фото").arg(m_projects->photoCount()), true);
    // The active chunk with its photos and results; the other chunks as rows that activate them.
    const QVector<ProcessingJob> jobs = m_processing ? m_processing->jobs() : QVector<ProcessingJob>();
    int photoCount = m_projects->photoCount(), gpsCount = m_projects->gpsCount();
    if (m_chunks && m_chunks->count() > 0) {
        if (const ChunkService::Chunk *active = m_chunks->chunk(m_chunks->active())) {
            QSet<QString> ids(active->photos.cbegin(), active->photos.cend());
            photoCount = int(ids.size());
            gpsCount = 0;
            for (const Photo &p : m_projects->photoModel()->photos()) gpsCount += p.hasGps && ids.contains(p.id) ? 1 : 0;
            if (QStandardItem *chunk = section(QStringLiteral("chunk")); chunk && chunk->data(TitleRole).toString() != active->name) {
                chunk->setData(active->name, TitleRole);
                chunk->setText(active->name);
            }
        }
        QList<QStringList> rows;   // id, name, status
        for (const ChunkService::Chunk &c : m_chunks->chunkList()) {
            if (c.id == m_chunks->active()) continue;
            int jobCount = 0;
            for (const ProcessingJob &j : jobs) jobCount += m_chunks->jobInChunk(j, c.id) ? 1 : 0;
            rows << QStringList{c.id, c.name, QStringLiteral("%1 фото · заданий %2").arg(c.photos.size()).arg(jobCount)};
        }
        QList<int> entries;
        for (int i = 0; i < project->rowCount(); ++i)
            if (project->child(i)->data(KindRole).toString() == QLatin1String("chunkEntry")) entries << i;
        bool same = entries.size() == rows.size();
        for (int i = 0; same && i < rows.size(); ++i) same = project->child(entries[i])->data(PayloadRole).toString() == rows[i][0];
        if (!same) {
            for (int i = int(entries.size()) - 1; i >= 0; --i) project->removeRow(entries[i]);
            entries.clear();
            for (int i = 0; i < rows.size(); ++i) {
                auto *item = new QStandardItem(rows[i][1]);
                item->setEditable(false);
                item->setData(rows[i][1], TitleRole);
                item->setData(QStringLiteral("chunkEntry"), KindRole);
                item->setData(QStringLiteral("chunk"), IconRole);
                item->setData(true, AvailableRole);
                item->setData(rows[i][0], PayloadRole);
                project->insertRow(1 + i, item);   // after the active chunk
                entries << 1 + i;
            }
        }
        for (int i = 0; i < rows.size(); ++i) {
            QStandardItem *item = project->child(entries[i]);
            if (item->data(TitleRole).toString() != rows[i][1]) {
                item->setData(rows[i][1], TitleRole);
                item->setText(rows[i][1]);
            }
            setStatus(item, rows[i][2], true);
        }
    }
    setStatus(section(QStringLiteral("chunk")),
              m_chunks && m_chunks->count() > 1 ? QStringLiteral("активный · %1 фото").arg(photoCount) : QStringLiteral("активный"), true);
    setStatus(section(QStringLiteral("photos")), QStringLiteral("%1 · с GPS %2").arg(photoCount).arg(gpsCount), photoCount > 0);
    // Survey control: the calibration the next jobs start from, the points and the markers.
    const QString calibration = m_survey ? m_survey->calibration() : QString();
    setStatus(section(QStringLiteral("calibrations")),
              calibration.isEmpty() ? QStringLiteral("самокалибровка")
                                    : calibration == QLatin1String("?") ? QStringLiteral("калибровка проекта") : QStringLiteral("из задания ") + calibration,
              true);
    const int markers = m_map ? int(m_map->projectMarkers().size()) : 0;
    setStatus(section(QStringLiteral("gcp")),
              m_survey ? QStringLiteral("опорных %1 · контрольных %2 · маркеров %3").arg(m_survey->controlCount()).arg(m_survey->checkCount()).arg(markers)
                       : QStringLiteral("маркеров %1").arg(markers),
              true);

    // Results: the newest completed artifact of each kind, of the active chunk.
    for (const ResultKind &r : Results) {
        QStandardItem *item = section(QString::fromLatin1(r.kind));
        if (!item) continue;
        const ProcessingJob *found = nullptr;
        const Artifact *artifact = nullptr;
        for (const ProcessingJob &j : jobs) {   // newest first
            if (!m_processing->jobInChunk(j)) continue;
            for (const Artifact &a : j.outputs)
                if (a.kind == QLatin1String(r.kind)) {
                    found = &j;
                    artifact = &a;
                    break;
                }
            if (artifact) break;
        }
        if (!artifact) {
            setStatus(item, jobs.isEmpty() ? QStringLiteral("нет обработки") : QStringLiteral("не построено"), false);
            item->setData({}, PayloadRole);
            item->setData({}, JobRole);
            item->setData({}, FileRole);
            continue;
        }
        const QString path = QDir::toNativeSeparators(QDir(m_processing->jobDirectory(found->id)).filePath(artifact->file));
        const QString details = artifact->details.isEmpty() ? QString() : artifact->details + QStringLiteral(" · ");
        setStatus(item, details + QStringLiteral("задание ") + found->id, true);
        if (item->data(PayloadRole).toString() != path) item->setData(path, PayloadRole);
        if (item->data(JobRole).toString() != found->id) item->setData(found->id, JobRole);
        if (item->data(FileRole).toString() != artifact->file) item->setData(artifact->file, FileRole);
    }

    // Map layers, top first as in the layer panel.
    if (QStandardItem *layers = section(QStringLiteral("layers"))) {
        QList<QStringList> rows;   // id, name, status, icon
        if (m_map) {
            const QVector<MapLayer> list = m_map->layers();
            for (int i = int(list.size()) - 1; i >= 0; --i) {
                const MapLayer &l = list[i];
                QString status = l.needsCrs ? QStringLiteral("CRS не задан") : !l.status.isEmpty() ? l.status
                               : l.visible ? QStringLiteral("видим") : QStringLiteral("скрыт");
                rows << QStringList{l.id, l.name, status, layerIcon(l)};
            }
        }
        bool same = layers->rowCount() == rows.size();
        for (int i = 0; same && i < rows.size(); ++i) same = layers->child(i)->data(PayloadRole).toString() == rows[i][0];
        if (!same) {
            layers->removeRows(0, layers->rowCount());
            for (const QStringList &row : rows) {
                QStandardItem *item = addSection(layers, QStringLiteral("layer"), row[1], row[3]);
                item->setData(row[0], PayloadRole);
            }
        }
        for (int i = 0; i < rows.size(); ++i) {
            QStandardItem *item = layers->child(i);
            if (item->data(TitleRole).toString() != rows[i][1]) {
                item->setData(rows[i][1], TitleRole);
                item->setText(rows[i][1]);
            }
            setStatus(item, rows[i][2], !rows[i][2].contains(QStringLiteral("CRS не задан")));
        }
        setStatus(layers, QString::number(rows.size()), true);
    }

    // Jobs, newest first.
    if (QStandardItem *jobsItem = section(QStringLiteral("jobs"))) {
        QHash<QString, QString> profileTitles;
        if (m_processing)
            for (const QVariant &p : m_processing->profilesForQml())
                profileTitles.insert(p.toMap().value("id").toString(), p.toMap().value("title").toString());
        bool same = jobsItem->rowCount() == jobs.size();
        for (int i = 0; same && i < jobs.size(); ++i) same = jobsItem->child(i)->data(PayloadRole).toString() == jobs[i].id;
        if (!same) {
            jobsItem->removeRows(0, jobsItem->rowCount());
            for (const ProcessingJob &j : jobs) {
                QStandardItem *item = addSection(jobsItem, QStringLiteral("job"), j.id, QStringLiteral("job"));
                item->setData(j.id, PayloadRole);
            }
        }
        for (int i = 0; i < jobs.size(); ++i) {
            const ProcessingJob &j = jobs[i];
            QStandardItem *item = jobsItem->child(i);
            QString title = QStringLiteral("%1 · %2").arg(profileTitles.value(j.profile, j.profile),
                                                         ru.toString(j.createdAt.toLocalTime(), QStringLiteral("dd.MM HH:mm")));
            if (m_chunks && m_chunks->count() > 1) title += QStringLiteral(" · ") + m_chunks->nameOf(j.chunk);
            if (item->data(TitleRole).toString() != title) {
                item->setData(title, TitleRole);
                item->setText(title);
            }
            setStatus(item, jobStateText(j.state), true);
        }
        setStatus(jobsItem, QString::number(jobs.size()), true);
    }
    ++m_revision;
    emit revisionChanged();
}

// ------------------------------------------------------------------ filter

ProjectTreeFilter::ProjectTreeFilter(QObject *parent) : QSortFilterProxyModel(parent)
{
    setRecursiveFilteringEnabled(true);
}

void ProjectTreeFilter::setFilterText(const QString &text)
{
    if (text == m_text) return;
    m_text = text;
    invalidateFilter();
    emit filterTextChanged();
}

bool ProjectTreeFilter::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    if (m_text.trimmed().isEmpty()) return true;
    const QModelIndex index = sourceModel()->index(sourceRow, 0, sourceParent);
    return index.data(ProjectTreeModel::TitleRole).toString().contains(m_text.trimmed(), Qt::CaseInsensitive)
        || index.data(ProjectTreeModel::StatusRole).toString().contains(m_text.trimmed(), Qt::CaseInsensitive);
}
