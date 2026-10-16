#pragma once

#include <QObject>
#include <QPointF>
#include <QSortFilterProxyModel>
#include <QStringList>
#include <QThreadPool>
#include <QVariantList>
#include <QVector>
#include <atomic>
#include <memory>

class ProjectManager;
class ProcessingService;
struct ProcessingJob;

// Photos of the active chunk, for the photo grid and list: rows of the project's PhotoModel
// whose photo is in the chunk (role "photoRow" keeps the project row for the selection).
class ChunkPhotoModel : public QSortFilterProxyModel
{
    Q_OBJECT
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;
    void setPhotos(const QStringList &ids);
protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;
private:
    QSet<QString> m_ids;
};

// Chunks of the project (chunks.json), as in Metashape: named sets of the project's photos, each
// processed by its own jobs. One chunk holding every photo when the file is absent (projects
// before 0.5.4). A photo may be in several chunks (a split keeps overlaps); every photo is in at
// least one, new photos go to the active chunk. Split: a grid over the photo positions with
// overlap, or by flights (pauses between shots). Merge: the photos into a new chunk, and the
// newest orthophotos/DSM/DTM of the chunks into one mosaic (mosaic::build) under mosaics/.
class ChunkService : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList chunks READ chunksForQml NOTIFY changed)
    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(QString active READ active NOTIFY changed)
    Q_PROPERTY(QString activeName READ activeName NOTIFY changed)
    Q_PROPERTY(int activePhotoCount READ activePhotoCount NOTIFY changed)
    Q_PROPERTY(QAbstractItemModel *photoModel READ photoModel CONSTANT)
    Q_PROPERTY(bool merging READ merging NOTIFY mergeChanged)
    Q_PROPERTY(QString mergeStatus READ mergeStatus NOTIFY mergeChanged)

public:
    struct Chunk
    {
        QString id;
        QString name;
        QStringList photos;          // photo ids, in the project's order
        QVector<QPointF> cell;       // WGS 84 (longitude, latitude) of the grid cell it was split for; empty otherwise
        QString created;
    };

    ChunkService(ProjectManager *projects, ProcessingService *processing, QObject *parent = nullptr);
    ~ChunkService() override;

    const QVector<Chunk> &chunkList() const { return m_chunks; }
    const Chunk *chunk(const QString &id) const;
    QVariantList chunksForQml() const;
    int count() const { return int(m_chunks.size()); }
    QString active() const { return m_active; }
    QString activeName() const;
    int activePhotoCount() const;
    QAbstractItemModel *photoModel() { return &m_photoModel; }
    bool merging() const { return m_mergeCancel != nullptr; }
    QString mergeStatus() const { return m_mergeStatus; }
    // Whether a job belongs to a chunk (jobs before chunks: the first one).
    bool jobInChunk(const ProcessingJob &job, const QString &chunkId) const;

    Q_INVOKABLE bool setActive(const QString &id);
    Q_INVOKABLE QString nameOf(const QString &id) const;   // of a job's chunk ("" = the first)
    // New empty chunk ("Блок N" when no name), made active; its id.
    Q_INVOKABLE QString create(const QString &name = {});
    // A copy with the same photos (not the processing results), made active; its id.
    Q_INVOKABLE QString duplicate(const QString &id);
    Q_INVOKABLE QString rename(const QString &id, const QString &name);   // "" or the reason
    // The chunk goes; its photos stay in the project (those of no other chunk move to the first
    // chunk); its jobs and their results stay in the job list. Not the last chunk. "" or the reason.
    Q_INVOKABLE QString remove(const QString &id);
    // The selected photo leaves / joins a chunk. "" or the reason.
    Q_INVOKABLE QString removePhoto(const QString &chunkId, const QString &photoId);
    Q_INVOKABLE QString addPhoto(const QString &chunkId, const QString &photoId);
    // options: mode "grid" (rows, cols, overlap in %) or "flights" (gapMinutes); minPhotos (3).
    // {ok, error, parts:[{name, photos}], noPosition, skipped, notes}
    Q_INVOKABLE QVariantMap splitPreview(const QString &id, const QVariantMap &options) const;
    // Creates the parts as new chunks (the source stays): {ok, error, created:[ids], notes}.
    Q_INVOKABLE QVariantMap split(const QString &id, const QVariantMap &options);
    // A new chunk with the photos of all of them (each once); its id ("" with errorOccurred).
    Q_INVOKABLE QString mergePhotos(const QStringList &ids, const QString &name = {});
    // Newest result of a kind (orthophoto, dsm, dtm) of each chunk: [{chunk, name, path, job}].
    Q_INVOKABLE QVariantList resultsOf(const QStringList &ids, const QString &kind) const;
    // One mosaic of the chunks' newest results of a kind into mosaics/; "" when started, else the reason.
    Q_INVOKABLE QString mergeResults(const QStringList &ids, const QString &kind);
    Q_INVOKABLE void cancelMerge();

signals:
    void changed();
    void mergeChanged();
    void mergeFinished(bool ok, const QString &path, const QString &message);
    void errorOccurred(const QString &message);

private:
    void load();
    void sync();          // photos added to or removed from the project
    bool save();
    void publish();       // the active chunk to the photo grid and the processing
    QString newId() const;
    QString uniqueName(const QString &base) const;
    Chunk *find(const QString &id);
    struct Part { QString name; QStringList photos; QVector<QPointF> cell; };
    QVector<Part> parts(const Chunk &source, const QVariantMap &options, QStringList *notes, int *noPosition, int *skipped,
                        QString *error) const;

    ProjectManager *m_projects;
    ProcessingService *m_processing;
    QString m_project;
    QVector<Chunk> m_chunks;
    QString m_active;
    bool m_stored = false;   // chunks.json exists (written once anything about chunks changed)
    ChunkPhotoModel m_photoModel;
    QString m_mergeStatus;
    std::shared_ptr<std::atomic_bool> m_mergeCancel;
    QThreadPool m_pool;
};
