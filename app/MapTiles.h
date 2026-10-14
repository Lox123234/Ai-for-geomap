#pragma once

#include <QCache>
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QThreadPool>
#include <memory>

class RasterSource;

struct TileKey
{
    quintptr source = 0;   // RasterSource identity
    int version = 0;       // style / pyramid version of the layer
    int level = 0;
    int x = 0;
    int y = 0;
    bool operator==(const TileKey &o) const
    {
        return source == o.source && version == o.version && level == o.level && x == o.x && y == o.y;
    }
};
inline size_t qHash(const TileKey &k, size_t seed = 0)
{
    return qHashMulti(seed, k.source, k.version, k.level, k.x, k.y);
}

// Raster tiles rendered on worker threads and kept in a memory-bounded cache. Only tiles the
// last frame still wants are rendered, so fast panning and zooming never builds a backlog.
class TileCache final : public QObject
{
    Q_OBJECT
public:
    explicit TileCache(QObject *parent = nullptr);
    ~TileCache() override;

    QImage tile(const TileKey &key) const;            // null if not rendered yet
    void request(const TileKey &key, const std::shared_ptr<RasterSource> &source);
    // Keys needed by the current frame; pending tiles outside the set are skipped.
    void setWanted(const QSet<TileKey> &keys);
    void clear();
    int pending() const;

signals:
    void tileReady();

private:
    mutable QMutex m_mutex;
    QCache<TileKey, QImage> m_cache;
    QSet<TileKey> m_pending;
    QSet<TileKey> m_wanted;
    QThreadPool m_pool;
};
