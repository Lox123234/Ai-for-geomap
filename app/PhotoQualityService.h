#pragma once

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QThreadPool>
#include <QVariantList>
#include <atomic>
#include <memory>

class ChunkService;
class ProjectManager;

// Обработка → Оценка качества снимков, as Estimate Image Quality in Metashape: the sharpness of
// the photos (imagequality::measure, in the background, several at a time), kept by photo id in
// <project>/quality.json with the file's size and time; the quality of each against the sharpest
// photos of the project; blurred photos switched off in the active chunk (ChunkService).
class PhotoQualityService final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList photos READ photos NOTIFY changed)   // of the active chunk, worst first
    Q_PROPERTY(int measured READ measured NOTIFY changed)        // of the active chunk
    Q_PROPERTY(int blurred READ blurred NOTIFY changed)          // below the threshold, in the active chunk
    Q_PROPERTY(int blurredEnabled READ blurredEnabled NOTIFY changed)   // of those, still on
    Q_PROPERTY(double threshold READ threshold WRITE setThreshold NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY progressChanged)
    Q_PROPERTY(int done READ done NOTIFY progressChanged)
    Q_PROPERTY(int total READ total NOTIFY progressChanged)
public:
    PhotoQualityService(ProjectManager *projects, ChunkService *chunks, QObject *parent = nullptr);
    ~PhotoQualityService() override;

    QVariantList photos() const;
    int measured() const;
    int blurred() const { return countBlurred(false); }
    int blurredEnabled() const { return countBlurred(true); }
    double threshold() const { return m_threshold; }
    void setThreshold(double threshold);
    bool busy() const { return m_cancel != nullptr; }
    int done() const { return m_done; }
    int total() const { return m_total; }

    // Photos of the active chunk without a value or changed since (all: every one again). "" when started.
    Q_INVOKABLE QString estimate(bool all = false);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE double qualityOf(const QString &photoId) const;   // −1: not measured
    // Blurred photos of the active chunk off / every photo of it on. "" or the reason.
    Q_INVOKABLE QString disableBlurred();
    Q_INVOKABLE QString enableAll();

signals:
    void changed();
    void progressChanged();
    void finished(bool ok, const QString &message);

private:
    struct Value
    {
        double sharpness = 0;
        qint64 bytes = 0;
        QDateTime modified;
    };
    void load();
    void save() const;
    QStringList chunkPhotos() const;
    double reference() const;
    int countBlurred(bool enabledOnly) const;
    void finish(bool cancelled);

    ProjectManager *m_projects;
    ChunkService *m_chunks;
    QString m_project;
    QHash<QString, Value> m_values;   // by photo id
    double m_threshold = 0.5;
    QThreadPool m_pool;
    std::shared_ptr<std::atomic_bool> m_cancel;
    int m_generation = 0;
    int m_done = 0, m_total = 0, m_measuredNow = 0;
    QStringList m_failed;
};
