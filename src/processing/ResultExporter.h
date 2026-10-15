#pragma once

#include <QObject>
#include <QStringList>
#include <QThreadPool>
#include <QUrl>
#include <QVariantList>
#include <atomic>
#include <memory>

class ProcessingService;

// "Экспорт результатов": copies the results of a processing job to a folder of the user's choice —
// rasters (orthophoto, DSM, DTM) as GeoTIFF or COG, optionally in another CRS or cell size, the
// point cloud, models with their materials and textures, the report. Runs in the background;
// existing files are never overwritten (names get -2, -3, …). QML API "exporter".
class ResultExporter : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)

public:
    explicit ResultExporter(ProcessingService *processing, QObject *parent = nullptr);
    ~ResultExporter() override;

    // Results of a job: {file, kind, title, details, bytes, raster}.
    Q_INVOKABLE QVariantList results(const QString &jobId) const;
    // The newest job that has results, or "".
    Q_INVOKABLE QString latestJobWithResults() const;
    // Starts the export. options: {crs: "" | EPSG code / WKT / "map:<wkt>", cog: bool, resolution: number}.
    // "" when started, else the reason.
    Q_INVOKABLE QString start(const QString &jobId, const QStringList &files, const QUrl &folder, const QVariantMap &options = {});
    Q_INVOKABLE void cancel();

    bool busy() const { return m_busy; }
    QString status() const { return m_status; }

signals:
    void busyChanged();
    void statusChanged();
    void finished(bool ok, const QString &message, const QStringList &files);

private:
    ProcessingService *m_processing;
    bool m_busy = false;
    QString m_status;
    std::shared_ptr<std::atomic_bool> m_cancel;
    QThreadPool m_pool;
};
