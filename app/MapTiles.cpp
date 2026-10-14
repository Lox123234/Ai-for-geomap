#include "MapTiles.h"
#include "RasterSource.h"
#include <QMetaObject>
#include <QRunnable>

TileCache::TileCache(QObject *parent) : QObject(parent)
{
    m_cache.setMaxCost(256 * 1024 * 1024);   // bytes of tile images
    m_pool.setMaxThreadCount(qMax(2, QThread::idealThreadCount() / 2));
}

TileCache::~TileCache()
{
    {
        QMutexLocker lock(&m_mutex);
        m_wanted.clear();   // queued tiles are skipped
    }
    m_pool.clear();
    m_pool.waitForDone();
}

QImage TileCache::tile(const TileKey &key) const
{
    QMutexLocker lock(&m_mutex);
    const QImage *image = m_cache.object(key);
    return image ? *image : QImage();
}

void TileCache::request(const TileKey &key, const std::shared_ptr<RasterSource> &source)
{
    {
        QMutexLocker lock(&m_mutex);
        if (m_cache.contains(key) || m_pending.contains(key)) return;
        m_pending.insert(key);
        m_wanted.insert(key);
    }
    m_pool.start([this, key, source] {
        {
            QMutexLocker lock(&m_mutex);
            if (!m_wanted.contains(key)) {   // no longer on screen
                m_pending.remove(key);
                return;
            }
        }
        QImage image = source->renderTile(key.level, key.x, key.y);
        if (image.isNull()) {   // outside the grid or a read error: remember an empty tile
            image = QImage(1, 1, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
        }
        {
            QMutexLocker lock(&m_mutex);
            m_pending.remove(key);
            m_cache.insert(key, new QImage(image), int(qMin<qsizetype>(image.sizeInBytes(), INT_MAX)));
        }
        QMetaObject::invokeMethod(this, &TileCache::tileReady, Qt::QueuedConnection);
    });
}

void TileCache::setWanted(const QSet<TileKey> &keys)
{
    QMutexLocker lock(&m_mutex);
    m_wanted = keys;
}

void TileCache::clear()
{
    QMutexLocker lock(&m_mutex);
    m_cache.clear();
    m_wanted.clear();
}

int TileCache::pending() const
{
    QMutexLocker lock(&m_mutex);
    return int(m_pending.size());
}
