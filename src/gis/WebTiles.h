#pragma once

#include "Crs.h"
#include "RasterSource.h"

#include <QCache>
#include <QDateTime>
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QStringList>

class QNetworkAccessManager;
class QNetworkReply;

// Online basemaps (XYZ tiles in Web Mercator) under the map, like the satellite and street maps of
// Agisoft Metashape. Tiles are downloaded only as a view needs them (no area is fetched ahead:
// the tile servers' usage policies forbid it), at most a few at a time, with an identifying
// User-Agent, and kept in a disk cache; the provider's attribution is always shown on the map.
namespace webtiles {

struct Provider
{
    QString id, title;
    QString url;                 // {z} {x} {y}, optionally {s} (one of `subdomains`)
    QStringList subdomains;
    int minZoom = 0, maxZoom = 19;
    QString attribution;         // shown on the map while the basemap is visible
    QString termsUrl;            // the provider's usage terms, accepted by the user before first use
    QString kind;                // "map" or "satellite"
    int connections = 2;         // downloads at once
    bool isValid() const { return !id.isEmpty() && url.contains(QLatin1String("{z}")); }
};

QVector<Provider> providers();                       // the built-in catalogue
// A built-in provider, or "custom:<url template>" for a server of the user's choice.
Provider provider(const QString &id);

// Downloads and caches tiles; lives in the main thread, used from any thread.
class TileStore final : public QObject
{
    Q_OBJECT
public:
    static TileStore *instance();
    void setCacheDirectory(const QString &directory);
    QString cacheDirectory() const;
    void setUserAgent(const QString &agent);
    // The tile when cached (memory or disk), else a null image; with `fetch` a missing tile is
    // queued for download once (failures are retried after a few minutes).
    QImage tile(const Provider &p, int z, int x, int y, bool fetch);
    int pending() const;
    QString lastError() const;

signals:
    void arrived(const QString &providerId);   // a tile was downloaded: views repaint
    void failed(const QString &providerId, const QString &error);

private:
    explicit TileStore(QObject *parent = nullptr);
    QString path(const Provider &p, int z, int x, int y) const;
    void startNext();
    void finished(QNetworkReply *reply);
    struct Job { Provider provider; int z, x, y; QString key; };
    mutable QMutex m_mutex;
    QCache<QString, QImage> m_memory;
    QQueue<Job> m_queue;
    QSet<QString> m_queued;                  // queued or downloading
    QHash<QString, QDateTime> m_failed;      // key → when; not retried for a while
    QHash<QString, int> m_running;           // provider → downloads in progress
    QString m_cache, m_agent, m_lastError;
    QNetworkAccessManager *m_network = nullptr;
};

// A basemap as a raster source of the map: a grid in the map CRS around `centre` (±extent
// metres), every map tile drawn from the provider's tiles at the matching zoom, reprojected.
class WebRasterSource final : public RasterSource
{
public:
    WebRasterSource(const Provider &provider, const Crs &mapCrs, const QPointF &centre, double extentMetres = 100000);
    QImage renderTile(int level, int tx, int ty) const override;
    QImage renderRegion(const QRectF &mapRect, const QSize &size) const override;
    double valueAt(const QPointF &, int) const override;
    const Provider &provider() const { return m_provider; }

private:
    QImage render(const QRectF &mapRect, int width, int height, bool fetch) const;
    Provider m_provider;
};

} // namespace webtiles
