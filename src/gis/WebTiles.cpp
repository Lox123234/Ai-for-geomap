#include "WebTiles.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QThread>
#include <QUrl>
#include <cmath>

namespace webtiles {
namespace {
constexpr double R = 6378137.0;
constexpr double Pi = 3.14159265358979323846;
constexpr double HalfWorld = Pi * R;
constexpr int MaxQueue = 400;
}

QVector<Provider> providers()
{
    QVector<Provider> list;
    Provider osm;
    osm.id = QStringLiteral("osm");
    osm.title = QStringLiteral("OpenStreetMap — карта");
    osm.url = QStringLiteral("https://tile.openstreetmap.org/{z}/{x}/{y}.png");
    osm.maxZoom = 19;
    osm.attribution = QStringLiteral("© участники OpenStreetMap");
    osm.termsUrl = QStringLiteral("https://operations.osmfoundation.org/policies/tiles/");
    osm.kind = QStringLiteral("map");
    osm.connections = 2;
    list << osm;
    Provider esri;
    esri.id = QStringLiteral("esri-imagery");
    esri.title = QStringLiteral("Esri World Imagery — спутник");
    esri.url = QStringLiteral("https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}");
    esri.maxZoom = 19;
    esri.attribution = QStringLiteral("Esri, Maxar, Earthstar Geographics и сообщество пользователей ГИС");
    esri.termsUrl = QStringLiteral("https://www.esri.com/en-us/legal/terms/full-master-agreement");
    esri.kind = QStringLiteral("satellite");
    esri.connections = 4;
    list << esri;
    Provider s2;
    s2.id = QStringLiteral("s2cloudless");
    s2.title = QStringLiteral("Sentinel-2 cloudless 2016 — спутник 10 м");
    s2.url = QStringLiteral("https://tiles.maps.eox.at/wmts/1.0.0/s2cloudless_3857/default/g/{z}/{y}/{x}.jpg");
    s2.maxZoom = 15;
    s2.attribution = QStringLiteral("Sentinel-2 cloudless — s2maps.eu, EOX IT Services GmbH (изменённые данные Copernicus Sentinel 2016)");
    s2.termsUrl = QStringLiteral("https://s2maps.eu/");
    s2.kind = QStringLiteral("satellite");
    s2.connections = 4;
    list << s2;
    Provider topo;
    topo.id = QStringLiteral("opentopomap");
    topo.title = QStringLiteral("OpenTopoMap — рельеф");
    topo.url = QStringLiteral("https://{s}.tile.opentopomap.org/{z}/{x}/{y}.png");
    topo.subdomains = {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")};
    topo.maxZoom = 17;
    topo.attribution = QStringLiteral("© участники OpenStreetMap, SRTM · © OpenTopoMap (CC-BY-SA)");
    topo.termsUrl = QStringLiteral("https://opentopomap.org/about");
    topo.kind = QStringLiteral("map");
    topo.connections = 2;
    list << topo;
    return list;
}

Provider provider(const QString &id)
{
    for (const Provider &p : providers())
        if (p.id == id) return p;
    if (id.startsWith(QLatin1String("custom:"))) {
        Provider p;
        p.id = id;
        p.title = QStringLiteral("Свой сервер тайлов");
        p.url = id.mid(7);
        p.maxZoom = 20;
        const QString host = QUrl(p.url).host();
        p.attribution = host.isEmpty() ? QStringLiteral("Свой сервер тайлов") : host;
        p.kind = QStringLiteral("map");
        p.connections = 2;
        return p;
    }
    return {};
}

// ------------------------------------------------------------------ TileStore

TileStore *TileStore::instance()
{
    static TileStore *store = [] {
        auto *s = new TileStore;
        if (QCoreApplication::instance()) s->moveToThread(QCoreApplication::instance()->thread());
        return s;
    }();
    return store;
}

TileStore::TileStore(QObject *parent) : QObject(parent)
{
    m_memory.setMaxCost(256 * 1024);   // KiB of decoded tiles
    m_agent = QStringLiteral("GeoMapAI/%1 (desktop photogrammetry; https://github.com/Lox123234/Ai-for-geomap)")
                  .arg(QCoreApplication::applicationVersion().isEmpty() ? QStringLiteral("dev") : QCoreApplication::applicationVersion());
}

void TileStore::setCacheDirectory(const QString &directory)
{
    QMutexLocker lock(&m_mutex);
    m_cache = directory;
}

QString TileStore::cacheDirectory() const
{
    QMutexLocker lock(&m_mutex);
    return m_cache;
}

void TileStore::setUserAgent(const QString &agent)
{
    QMutexLocker lock(&m_mutex);
    m_agent = agent;
}

int TileStore::pending() const
{
    QMutexLocker lock(&m_mutex);
    return int(m_queued.size());
}

QString TileStore::lastError() const
{
    QMutexLocker lock(&m_mutex);
    return m_lastError;
}

QString TileStore::path(const Provider &p, int z, int x, int y) const
{
    if (m_cache.isEmpty()) return {};
    QString id = p.id;
    if (id.startsWith(QLatin1String("custom:")))
        id = QStringLiteral("custom-") + QString::number(qHash(p.url), 16);
    return QStringLiteral("%1/%2/%3/%4/%5.tile").arg(m_cache, id).arg(z).arg(x).arg(y);
}

QImage TileStore::tile(const Provider &p, int z, int x, int y, bool fetch)
{
    const QString key = QStringLiteral("%1/%2/%3/%4").arg(p.id).arg(z).arg(x).arg(y);
    QString file;
    {
        QMutexLocker lock(&m_mutex);
        if (const QImage *image = m_memory.object(key)) return *image;
        file = path(p, z, x, y);
    }
    if (!file.isEmpty() && QFileInfo::exists(file)) {
        QImage image(file);
        if (!image.isNull()) {
            image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
            QMutexLocker lock(&m_mutex);
            m_memory.insert(key, new QImage(image), qMax<int>(1, int(image.sizeInBytes() / 1024)));
            return image;
        }
    }
    if (!fetch || z < p.minZoom || z > p.maxZoom) return {};
    {
        QMutexLocker lock(&m_mutex);
        if (m_queued.contains(key)) return {};
        const auto failed = m_failed.constFind(key);
        if (failed != m_failed.constEnd() && failed->secsTo(QDateTime::currentDateTimeUtc()) < 300) return {};
        m_queued.insert(key);
        m_queue.prepend(Job{p, z, x, y, key});   // the latest view first
        while (m_queue.size() > MaxQueue) m_queued.remove(m_queue.takeLast().key);   // views long gone
    }
    QMetaObject::invokeMethod(this, &TileStore::startNext, Qt::QueuedConnection);
    return {};
}

void TileStore::startNext()
{
    if (!m_network) m_network = new QNetworkAccessManager(this);
    for (;;) {
        Job job;
        QString agent;
        {
            QMutexLocker lock(&m_mutex);
            int index = -1;
            for (int i = 0; i < m_queue.size(); ++i)
                if (m_running.value(m_queue[i].provider.id) < qMax(1, m_queue[i].provider.connections)) {
                    index = i;
                    break;
                }
            if (index < 0) return;
            job = m_queue.takeAt(index);
            ++m_running[job.provider.id];
            agent = m_agent;
        }
        QString url = job.provider.url;
        url.replace(QLatin1String("{z}"), QString::number(job.z));
        url.replace(QLatin1String("{x}"), QString::number(job.x));
        url.replace(QLatin1String("{y}"), QString::number(job.y));
        if (!job.provider.subdomains.isEmpty())
            url.replace(QLatin1String("{s}"), job.provider.subdomains[(job.x + job.y) % job.provider.subdomains.size()]);
        QNetworkRequest request{QUrl(url)};
        request.setHeader(QNetworkRequest::UserAgentHeader, agent);
        request.setTransferTimeout(20000);
        QNetworkReply *reply = m_network->get(request);
        reply->setProperty("key", job.key);
        reply->setProperty("provider", job.provider.id);
        reply->setProperty("z", job.z);
        reply->setProperty("x", job.x);
        reply->setProperty("y", job.y);
        reply->setProperty("url", job.provider.url);
        connect(reply, &QNetworkReply::finished, this, [this, reply] { finished(reply); });
    }
}

void TileStore::finished(QNetworkReply *reply)
{
    reply->deleteLater();
    const QString key = reply->property("key").toString(), providerId = reply->property("provider").toString();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray data = reply->readAll();
    QImage image;
    QString error;
    if (reply->error() != QNetworkReply::NoError) error = reply->errorString();
    else if (status != 0 && status != 200) error = QStringLiteral("HTTP %1").arg(status);
    else if (!image.loadFromData(data)) error = QStringLiteral("сервер прислал не изображение");
    Provider p = provider(providerId);
    if (!p.isValid()) {
        p.id = providerId;
        p.url = reply->property("url").toString();
    }
    const int z = reply->property("z").toInt(), x = reply->property("x").toInt(), y = reply->property("y").toInt();
    QString file;
    {
        QMutexLocker lock(&m_mutex);
        --m_running[providerId];
        m_queued.remove(key);
        if (error.isEmpty()) {
            image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
            m_memory.insert(key, new QImage(image), qMax<int>(1, int(image.sizeInBytes() / 1024)));
            m_failed.remove(key);
            file = path(p, z, x, y);
        } else {
            m_failed.insert(key, QDateTime::currentDateTimeUtc());
            m_lastError = error;
        }
    }
    if (!file.isEmpty()) {   // the bytes as the server sent them (PNG or JPEG)
        QDir().mkpath(QFileInfo(file).absolutePath());
        QSaveFile f(file);
        if (f.open(QIODevice::WriteOnly)) {
            f.write(data);
            f.commit();
        }
    }
    if (error.isEmpty()) emit arrived(providerId);
    else emit failed(providerId, error);
    startNext();
}

// ------------------------------------------------------------------ WebRasterSource

WebRasterSource::WebRasterSource(const Provider &provider, const Crs &mapCrs, const QPointF &centre, double extentMetres)
    : m_provider(provider)
{
    m_mapCrs = mapCrs;
    m_path = QStringLiteral("web:") + provider.id;
    if (!provider.isValid()) {
        m_error = QStringLiteral("Неизвестный сервер подложки");
        return;
    }
    if (!mapCrs.isValid()) {
        m_error = QStringLiteral("У карты не задана система координат.");
        return;
    }
    m_sourceCrs = Crs::fromEpsg(3857);
    const double perUnit = mapCrs.isGeographic() ? 111320.0 : (mapCrs.metersPerUnit() > 0 ? mapCrs.metersPerUnit() : 1.0);
    const double cell = 0.15 / perUnit, half = extentMetres / perUnit;   // about zoom 20 at the finest level
    m_gridWidth = m_gridHeight = int(std::ceil(2 * half / cell));
    const double gt[6] = {centre.x() - half, cell, 0, centre.y() + half, 0, -cell};
    std::copy(gt, gt + 6, m_gridTransform);
    m_valid = true;
}

double WebRasterSource::valueAt(const QPointF &, int) const
{
    return std::numeric_limits<double>::quiet_NaN();
}

QImage WebRasterSource::renderTile(int level, int tx, int ty) const
{
    if (!m_valid || level < 0) return {};
    const qint64 span = qint64(TileSize) << level;
    const qint64 x0 = tx * span, y0 = ty * span;
    if (tx < 0 || ty < 0 || x0 >= m_gridWidth || y0 >= m_gridHeight) return {};
    const qint64 w = qMin<qint64>(span, m_gridWidth - x0), h = qMin<qint64>(span, m_gridHeight - y0);
    const int outW = qMax(1, int(std::ceil(double(w) / double(qint64(1) << level))));
    const int outH = qMax(1, int(std::ceil(double(h) / double(qint64(1) << level))));
    const double *gt = m_gridTransform;
    const double left = gt[0] + x0 * gt[1], right = gt[0] + (x0 + w) * gt[1];
    const double top = gt[3] + y0 * gt[5], bottom = gt[3] + (y0 + h) * gt[5];
    return render(QRectF(left, bottom, right - left, top - bottom), outW, outH, true);
}

QImage WebRasterSource::renderRegion(const QRectF &mapRect, const QSize &size) const
{
    if (!m_valid || size.isEmpty()) return {};
    return render(mapRect, size.width(), size.height(), true);
}

QImage WebRasterSource::render(const QRectF &rect, int width, int height, bool fetch) const
{
    QImage out(width, height, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    // Web Mercator metres of a coarse grid over the image (reprojection is smooth at this scale).
    constexpr int N = 16;
    CoordinateTransform toWgs(m_mapCrs, Crs::wgs84());
    if (!toWgs.isValid()) return out;
    double mx[N + 1][N + 1], my[N + 1][N + 1];
    for (int j = 0; j <= N; ++j)
        for (int i = 0; i <= N; ++i) {
            double x = rect.left() + rect.width() * i / N, y = rect.bottom() - rect.height() * j / N;   // rows from the top (max y)
            if (!toWgs.transform(x, y)) return out;
            const double lat = qBound(-85.05, y, 85.05);
            mx[j][i] = R * x * Pi / 180;
            my[j][i] = R * std::log(std::tan(Pi / 4 + lat * Pi / 360));
        }
    const double pixel = std::hypot(mx[0][N] - mx[0][0], my[0][N] - my[0][0]) / width;   // mercator metres per output pixel
    if (!(pixel > 0)) return out;
    const int z = qBound(m_provider.minZoom, int(std::ceil(std::log2(2 * HalfWorld / 256 / pixel) - 0.25)), m_provider.maxZoom);
    const double worldPixels = 256.0 * double(qint64(1) << z);
    struct Tile { int z = -1; QImage image; int x = 0, y = 0; };
    QHash<qint64, Tile> tiles;   // tile of zoom z → image (its own or a coarser one)
    TileStore *store = TileStore::instance();
    auto tileFor = [&](int tx, int ty) -> const Tile & {
        const qint64 key = (qint64(tx) << 32) | quint32(ty);
        auto it = tiles.find(key);
        if (it != tiles.end()) return *it;
        Tile t;
        const int n = 1 << z;
        if (tx >= 0 && ty >= 0 && tx < n && ty < n) {
            t.image = store->tile(m_provider, z, tx, ty, fetch);
            if (!t.image.isNull()) { t.z = z; t.x = tx; t.y = ty; }
            for (int up = 1; t.image.isNull() && up <= 6 && z - up >= m_provider.minZoom; ++up) {   // a coarser tile meanwhile
                const QImage coarse = store->tile(m_provider, z - up, tx >> up, ty >> up, false);
                if (!coarse.isNull()) { t.image = coarse; t.z = z - up; t.x = tx >> up; t.y = ty >> up; }
            }
        }
        return *tiles.insert(key, t);
    };
    for (int py = 0; py < height; ++py) {
        QRgb *line = reinterpret_cast<QRgb *>(out.scanLine(py));
        const double fy = (py + 0.5) / height * N;
        const int j = qMin(N - 1, int(fy));
        const double ay = fy - j;
        for (int px = 0; px < width; ++px) {
            const double fx = (px + 0.5) / width * N;
            const int i = qMin(N - 1, int(fx));
            const double ax = fx - i;
            const double x = (mx[j][i] * (1 - ax) + mx[j][i + 1] * ax) * (1 - ay) + (mx[j + 1][i] * (1 - ax) + mx[j + 1][i + 1] * ax) * ay;
            const double y = (my[j][i] * (1 - ax) + my[j][i + 1] * ax) * (1 - ay) + (my[j + 1][i] * (1 - ax) + my[j + 1][i + 1] * ax) * ay;
            const double gx = (x + HalfWorld) / (2 * HalfWorld) * worldPixels, gy = (HalfWorld - y) / (2 * HalfWorld) * worldPixels;
            const Tile &t = tileFor(int(std::floor(gx / 256)), int(std::floor(gy / 256)));
            if (t.image.isNull()) continue;
            // Position inside the tile actually held (its zoom may be coarser).
            const double scale = double(qint64(1) << (z - t.z));
            const double u = gx / scale - t.x * 256.0 - 0.5, v = gy / scale - t.y * 256.0 - 0.5;
            const int w = t.image.width(), h = t.image.height();
            const double uu = qBound(0.0, u * w / 256.0, w - 1.001), vv = qBound(0.0, v * h / 256.0, h - 1.001);
            const int u0 = int(uu), v0 = int(vv);
            const double bx = uu - u0, by = vv - v0;
            const QRgb *r0 = reinterpret_cast<const QRgb *>(t.image.constScanLine(v0)), *r1 = reinterpret_cast<const QRgb *>(t.image.constScanLine(qMin(v0 + 1, h - 1)));
            const QRgb a = r0[u0], b = r0[qMin(u0 + 1, w - 1)], c = r1[u0], d = r1[qMin(u0 + 1, w - 1)];
            auto mix = [&](int (*ch)(QRgb)) { return int((ch(a) * (1 - bx) + ch(b) * bx) * (1 - by) + (ch(c) * (1 - bx) + ch(d) * bx) * by + 0.5); };
            line[px] = qRgba(mix(qRed), mix(qGreen), mix(qBlue), mix(qAlpha));
        }
    }
    return out;
}

} // namespace webtiles
