#include "RasterSource.h"
#include "GisEnvironment.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QMutexLocker>
#include <QPainter>
#include <QRgb>
#include <QtMath>
#include <cpl_conv.h>
#include <gdal.h>
#include <gdal_utils.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

QByteArray utf8(const QString &path) { return QDir::toNativeSeparators(path).toUtf8(); }

GDALDatasetH openRaster(const QString &path)
{
    return GDALOpenEx(utf8(path).constData(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
}

bool northUp(const double *gt) { return gt[2] == 0.0 && gt[4] == 0.0 && gt[1] > 0 && gt[5] < 0; }

// Same order as the C++ argument vectors GDAL's utilities expect.
struct ArgList
{
    QList<QByteArray> storage;
    QVector<char *> pointers;
    explicit ArgList(const QStringList &args)
    {
        for (const QString &a : args) storage << a.toUtf8();
        for (QByteArray &b : storage) pointers << b.data();
        pointers << nullptr;
    }
    char **data() { return pointers.data(); }
};

int progressAdapter(double complete, const char *, void *arg)
{
    auto *fn = static_cast<const std::function<bool(double)> *>(arg);
    return (*fn)(complete) ? TRUE : FALSE;
}
} // namespace

// ----------------------------------------------------------------- RasterInfo

RasterInfo RasterInfo::read(const QString &path)
{
    RasterInfo info;
    info.path = path;
    GisEnvironment::clearError();
    GDALDatasetH ds = openRaster(path);
    if (!ds) {
        info.error = QStringLiteral("Не удалось открыть растр: %1").arg(GisEnvironment::lastError());
        return info;
    }
    info.driver = QString::fromLatin1(GDALGetDriverShortName(GDALGetDatasetDriver(ds)));
    info.width = GDALGetRasterXSize(ds);
    info.height = GDALGetRasterYSize(ds);
    info.bands = GDALGetRasterCount(ds);
    if (info.bands == 0) {
        GDALClose(ds);
        info.error = QStringLiteral("В файле нет растровых каналов.");
        return info;
    }
    GDALRasterBandH band1 = GDALGetRasterBand(ds, 1);
    info.dataType = QString::fromLatin1(GDALGetDataTypeName(GDALGetRasterDataType(band1)));
    info.overviews = GDALGetOverviewCount(band1);
    for (int b = 1; b <= info.bands; ++b) {
        GDALRasterBandH band = GDALGetRasterBand(ds, b);
        if (GDALGetRasterColorInterpretation(band) == GCI_AlphaBand) info.alphaBand = b;
        int has = 0;
        const double nd = GDALGetRasterNoDataValue(band, &has);
        info.noData << (has ? nd : NaN);
    }
    info.hasGeoTransform = GDALGetGeoTransform(ds, info.geoTransform) == CE_None;
    const char *wkt = GDALGetProjectionRef(ds);
    if (wkt && *wkt) info.crs = Crs::fromWkt(QString::fromUtf8(wkt));
    if (info.hasGeoTransform) {
        const double *gt = info.geoTransform;
        QVector<QPointF> corners;
        for (const QPointF &px : {QPointF(0, 0), QPointF(info.width, 0), QPointF(0, info.height), QPointF(info.width, info.height)})
            corners << QPointF(gt[0] + px.x() * gt[1] + px.y() * gt[2], gt[3] + px.x() * gt[4] + px.y() * gt[5]);
        double minX = corners[0].x(), maxX = minX, minY = corners[0].y(), maxY = minY;
        for (const QPointF &c : corners) {
            minX = qMin(minX, c.x()); maxX = qMax(maxX, c.x());
            minY = qMin(minY, c.y()); maxY = qMax(maxY, c.y());
        }
        info.extent = QRectF(minX, minY, maxX - minX, maxY - minY);
        info.pixelSizeX = std::hypot(gt[1], gt[4]);
        info.pixelSizeY = std::hypot(gt[2], gt[5]);
    }
    GDALClose(ds);
    info.valid = true;
    return info;
}

QString RasterInfo::summary() const
{
    if (!valid) return error;
    const QLocale ru(QLocale::Russian);
    QStringList parts;
    parts << QStringLiteral("%1×%2").arg(width).arg(height);
    parts << QStringLiteral("каналов: %1 (%2)").arg(bands).arg(dataType);
    if (hasGeoTransform && crs.isProjected() && crs.metersPerUnit() > 0)
        parts << QStringLiteral("%1 см/пикс").arg(ru.toString(pixelSizeX * crs.metersPerUnit() * 100.0, 'f', 1));
    else if (hasGeoTransform && crs.isGeographic())
        parts << QStringLiteral("%1°/пикс").arg(ru.toString(pixelSizeX, 'g', 3));
    parts << (crs.isValid() ? crs.label() : QStringLiteral("CRS не задан"));
    if (!hasGeoTransform) parts << QStringLiteral("без геопривязки");
    return parts.join(QStringLiteral(", "));
}

QString RasterInfo::brief() const
{
    if (!valid) return error;
    const QLocale ru(QLocale::Russian);
    QStringList parts;
    const double metres = hasGeoTransform && crs.isProjected() && crs.metersPerUnit() > 0 ? pixelSizeX * crs.metersPerUnit() : 0;
    if (metres >= 1)
        parts << QStringLiteral("%1 м/пикс").arg(ru.toString(metres, 'f', 1));
    else if (metres > 0)
        parts << QStringLiteral("%1 см/пикс").arg(ru.toString(metres * 100.0, 'f', 1));
    else if (hasGeoTransform && crs.isGeographic())
        parts << QStringLiteral("%1°/пикс").arg(ru.toString(pixelSizeX, 'g', 3));
    else
        parts << QStringLiteral("%1×%2").arg(width).arg(height);
    if (!crs.isValid()) parts << QStringLiteral("CRS не задан");
    if (!hasGeoTransform) parts << QStringLiteral("без геопривязки");
    return parts.join(QStringLiteral(" · "));
}

// ----------------------------------------------------------------- RasterStyle

RasterStyle RasterStyle::automatic(const RasterInfo &info, const QVector<QPair<double, double>> &bandRanges)
{
    RasterStyle s;
    auto range = [&](int band) {
        return band >= 1 && band <= bandRanges.size() ? bandRanges[band - 1] : qMakePair(0.0, 255.0);
    };
    const int colourBands = info.bands - (info.alphaBand ? 1 : 0);
    if (colourBands >= 3) {
        s.mode = Rgb;
        s.red = 1; s.green = 2; s.blue = 3;
        s.alpha = info.alphaBand;
        const bool byte = info.dataType == QLatin1String("Byte");
        for (int i = 0; i < 3; ++i) {
            s.rgbMin[i] = byte ? 0 : range(i + 1).first;
            s.rgbMax[i] = byte ? 255 : range(i + 1).second;
        }
    } else {
        s.mode = SingleBand;
        s.band = 1;
        s.alpha = info.alphaBand;
        s.min = range(1).first;
        s.max = range(1).second;
        const bool elevation = info.dataType.startsWith(QLatin1String("Float")) || info.dataType.startsWith(QLatin1String("Int"));
        s.ramp = elevation ? Terrain : Gray;
        s.hillshade = elevation && info.crs.isProjected();
    }
    return s;
}

QVariantMap RasterStyle::toVariant() const
{
    return {{"mode", mode == Rgb ? "rgb" : "single"}, {"red", red}, {"green", green}, {"blue", blue}, {"alpha", alpha},
            {"band", band}, {"ramp", ramp == Gray ? "gray" : ramp == Terrain ? "terrain" : ramp == Slope ? "slope" : ramp == Aspect ? "aspect" : ramp == Vegetation ? "vegetation" : ramp == Visibility ? "visibility" : ramp == Overlap ? "overlap" : "spectral"},
            {"min", min}, {"max", max}, {"rgbMin", QVariantList{rgbMin[0], rgbMin[1], rgbMin[2]}},
            {"rgbMax", QVariantList{rgbMax[0], rgbMax[1], rgbMax[2]}}, {"hillshade", hillshade}};
}

RasterStyle RasterStyle::fromVariant(const QVariantMap &m, const RasterStyle &fallback)
{
    RasterStyle s = fallback;
    if (m.contains("mode")) s.mode = m.value("mode").toString() == QLatin1String("single") ? SingleBand : Rgb;
    s.red = m.value("red", s.red).toInt();
    s.green = m.value("green", s.green).toInt();
    s.blue = m.value("blue", s.blue).toInt();
    s.alpha = m.value("alpha", s.alpha).toInt();
    s.band = m.value("band", s.band).toInt();
    const QString ramp = m.value("ramp").toString();
    if (ramp == QLatin1String("gray")) s.ramp = Gray;
    else if (ramp == QLatin1String("terrain")) s.ramp = Terrain;
    else if (ramp == QLatin1String("spectral")) s.ramp = Spectral;
    else if (ramp == QLatin1String("slope")) s.ramp = Slope;
    else if (ramp == QLatin1String("aspect")) s.ramp = Aspect;
    else if (ramp == QLatin1String("vegetation")) s.ramp = Vegetation;
    else if (ramp == QLatin1String("visibility")) s.ramp = Visibility;
    else if (ramp == QLatin1String("overlap")) s.ramp = Overlap;
    s.min = m.value("min", s.min).toDouble();
    s.max = m.value("max", s.max).toDouble();
    const QVariantList lo = m.value("rgbMin").toList(), hi = m.value("rgbMax").toList();
    for (int i = 0; i < 3; ++i) {
        if (i < lo.size()) s.rgbMin[i] = lo[i].toDouble();
        if (i < hi.size()) s.rgbMax[i] = hi[i].toDouble();
    }
    s.hillshade = m.value("hillshade", s.hillshade).toBool();
    return s;
}

QVector<QPair<double, QColor>> RasterStyle::rampStops(Ramp ramp)
{
    switch (ramp) {
    case Gray: return {{0.0, QColor(0, 0, 0)}, {1.0, QColor(255, 255, 255)}};
    case Terrain:
        return {{0.0, QColor(40, 110, 60)}, {0.25, QColor(120, 170, 80)}, {0.5, QColor(230, 220, 130)},
                {0.75, QColor(170, 120, 70)}, {1.0, QColor(245, 245, 245)}};
    case Spectral:
        return {{0.0, QColor(43, 131, 186)}, {0.25, QColor(171, 221, 164)}, {0.5, QColor(255, 255, 191)},
                {0.75, QColor(253, 174, 97)}, {1.0, QColor(215, 25, 28)}};
    case Slope:
        return {{0.0, QColor(26, 152, 80)}, {0.2, QColor(145, 207, 96)}, {0.4, QColor(254, 224, 139)},
                {0.6, QColor(252, 141, 89)}, {0.8, QColor(215, 48, 39)}, {1.0, QColor(110, 20, 90)}};
    case Aspect:   // north red, east yellow, south cyan, west blue, back to north
        return {{0.0, QColor(230, 60, 60)}, {0.25, QColor(240, 210, 60)}, {0.5, QColor(60, 200, 210)},
                {0.75, QColor(70, 100, 230)}, {1.0, QColor(230, 60, 60)}};
    case Vegetation:   // bare soil brown, sparse yellow, dense green
        return {{0.0, QColor(120, 80, 50)}, {0.3, QColor(200, 170, 110)}, {0.5, QColor(235, 225, 120)},
                {0.7, QColor(120, 190, 70)}, {1.0, QColor(20, 110, 40)}};
    case Visibility:   // 0 hidden, 1 visible
        return {{0.0, QColor(200, 60, 70)}, {0.49, QColor(200, 60, 70)}, {0.51, QColor(70, 210, 110)}, {1.0, QColor(70, 210, 110)}};
    case Overlap:   // photos per point over 0…10: 1 red, 2 orange, 3–4 yellow, 5–8 green, 9+ dark green
        return {{0.0, QColor(200, 40, 50)}, {0.15, QColor(200, 40, 50)}, {0.16, QColor(240, 140, 50)}, {0.25, QColor(240, 140, 50)},
                {0.26, QColor(240, 215, 80)}, {0.45, QColor(240, 215, 80)}, {0.46, QColor(150, 210, 90)}, {0.85, QColor(60, 170, 80)}, {1.0, QColor(20, 110, 60)}};
    }
    return {};
}

QColor RasterStyle::rampColor(Ramp ramp, double t)
{
    const auto stops = rampStops(ramp);
    t = qBound(0.0, t, 1.0);
    for (int i = 1; i < stops.size(); ++i) {
        if (t <= stops[i].first) {
            const double k = (t - stops[i - 1].first) / (stops[i].first - stops[i - 1].first);
            const QColor &a = stops[i - 1].second, &b = stops[i].second;
            return QColor::fromRgbF(float(a.redF() + (b.redF() - a.redF()) * k), float(a.greenF() + (b.greenF() - a.greenF()) * k),
                                    float(a.blueF() + (b.blueF() - a.blueF()) * k));
        }
    }
    return stops.last().second;
}

// --------------------------------------------------------------- RasterSource

struct RasterSource::Handle
{
    GDALDatasetH source = nullptr;
    GDALDatasetH warped = nullptr;
    int generation = 0;
    GDALDatasetH dataset() const { return warped ? warped : source; }
    ~Handle()
    {
        if (warped) GDALClose(warped);
        if (source) GDALClose(source);
    }
};

RasterSource::RasterSource(const QString &path, const Crs &crsOverride, const Crs &mapCrs)
    : m_path(path), m_mapCrs(mapCrs)
{
    const RasterInfo info = RasterInfo::read(path);
    if (!info.valid) {
        m_error = info.error;
        return;
    }
    if (!info.hasGeoTransform) {
        m_error = QStringLiteral("У растра нет геопривязки (ни координат в файле, ни файла привязки).");
        return;
    }
    m_sourceCrs = crsOverride.isValid() ? crsOverride : info.crs;
    if (!m_sourceCrs.isValid()) {
        m_error = QStringLiteral("У растра не задана система координат.");
        return;
    }
    if (!mapCrs.isValid()) {
        m_error = QStringLiteral("У карты не задана система координат.");
        return;
    }
    m_noData = info.noData;
    m_warp = !m_sourceCrs.isSame(mapCrs) || !northUp(info.geoTransform);
    Handle *h = acquire();
    if (!h) return;   // m_error set
    GDALDatasetH ds = h->dataset();
    m_gridWidth = GDALGetRasterXSize(ds);
    m_gridHeight = GDALGetRasterYSize(ds);
    GDALGetGeoTransform(ds, m_gridTransform);
    release(h);
    m_valid = m_gridWidth > 0 && m_gridHeight > 0;
}

RasterSource::~RasterSource()
{
    QMutexLocker lock(&m_mutex);
    qDeleteAll(m_free);
    m_free.clear();
}

QString RasterSource::renderPath() const
{
    QMutexLocker lock(&m_mutex);
    return m_pyramidPath.isEmpty() ? m_path : m_pyramidPath;
}

RasterSource::Handle *RasterSource::acquire() const
{
    QString path;
    int generation = 0;
    {
        QMutexLocker lock(&m_mutex);
        if (!m_free.isEmpty()) return m_free.takeLast();
        path = m_pyramidPath.isEmpty() ? m_path : m_pyramidPath;
        generation = m_generation;
    }
    auto h = std::make_unique<Handle>();
    h->generation = generation;
    GisEnvironment::clearError();
    h->source = openRaster(path);
    if (!h->source) {
        m_error = QStringLiteral("Не удалось открыть растр: %1").arg(GisEnvironment::lastError());
        return nullptr;
    }
    if (m_warp) {
        // An in-memory warped VRT in the map CRS with an alpha band for areas outside the raster.
        QStringList args = {QStringLiteral("-of"), QStringLiteral("VRT"), QStringLiteral("-t_srs"), m_mapCrs.wkt(),
                            QStringLiteral("-r"), QStringLiteral("bilinear"), QStringLiteral("-dstalpha"),
                            QStringLiteral("-s_srs"), m_sourceCrs.wkt()};
        ArgList list(args);
        GDALWarpAppOptions *options = GDALWarpAppOptionsNew(list.data(), nullptr);
        int usageError = FALSE;
        GDALDatasetH sources[] = {h->source};
        h->warped = GDALWarp("", nullptr, 1, sources, options, &usageError);
        GDALWarpAppOptionsFree(options);
        if (!h->warped) {
            m_error =
                QStringLiteral("Не удалось перепроецировать растр в CRS карты: %1").arg(GisEnvironment::lastError());
            return nullptr;
        }
    }
    return h.release();
}

void RasterSource::release(Handle *h) const
{
    if (!h) return;
    QMutexLocker lock(&m_mutex);
    if (h->generation != m_generation || m_free.size() >= 8) {
        lock.unlock();
        delete h;
        return;
    }
    m_free << h;
}

void RasterSource::setStyle(const RasterStyle &style)
{
    QMutexLocker lock(&m_mutex);
    m_style = style;
}

RasterStyle RasterSource::style() const
{
    QMutexLocker lock(&m_mutex);
    return m_style;
}

void RasterSource::setPyramidPath(const QString &vrtPath)
{
    QMutexLocker lock(&m_mutex);
    m_pyramidPath = vrtPath;
    ++m_generation;
    qDeleteAll(m_free);
    m_free.clear();
}

void RasterSource::reload()
{
    QMutexLocker lock(&m_mutex);
    ++m_generation;
    qDeleteAll(m_free);
    m_free.clear();
}

QRectF RasterSource::mapExtent() const
{
    const double *gt = m_gridTransform;
    const double x0 = gt[0], x1 = gt[0] + m_gridWidth * gt[1];
    const double y0 = gt[3] + m_gridHeight * gt[5], y1 = gt[3];
    return QRectF(qMin(x0, x1), qMin(y0, y1), std::fabs(x1 - x0), std::fabs(y1 - y0));
}

int RasterSource::maxLevel() const
{
    const int size = qMax(m_gridWidth, m_gridHeight);
    int level = 0;
    while ((TileSize << level) < size && level < 30) ++level;
    return level;
}

QImage RasterSource::renderTile(int level, int tx, int ty) const
{
    if (!m_valid || level < 0) return {};
    const qint64 span = qint64(TileSize) << level;
    const qint64 x0 = tx * span, y0 = ty * span;
    if (tx < 0 || ty < 0 || x0 >= m_gridWidth || y0 >= m_gridHeight) return {};
    const int w = int(qMin<qint64>(span, m_gridWidth - x0)), h = int(qMin<qint64>(span, m_gridHeight - y0));
    const int outW = qMax(1, int(std::ceil(double(w) / double(qint64(1) << level))));
    const int outH = qMax(1, int(std::ceil(double(h) / double(qint64(1) << level))));
    Handle *handle = acquire();
    if (!handle) return {};
    const QImage image = render(handle->dataset(), int(x0), int(y0), w, h, outW, outH, style());
    release(handle);
    return image;
}

QImage RasterSource::renderRegion(const QRectF &mapRect, const QSize &size) const
{
    QImage out(size, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    if (!m_valid || size.isEmpty() || mapRect.isEmpty()) return out;
    const double *gt = m_gridTransform;
    // Grid window of the map rectangle (y axis of the map points up, rows go down).
    double c0 = (mapRect.left() - gt[0]) / gt[1], c1 = (mapRect.right() - gt[0]) / gt[1];
    double r0 = (mapRect.bottom() - gt[3]) / gt[5], r1 = (mapRect.top() - gt[3]) / gt[5];
    const double cc0 = qMax(0.0, c0), cc1 = qMin(double(m_gridWidth), c1);
    const double rr0 = qMax(0.0, r0), rr1 = qMin(double(m_gridHeight), r1);
    if (cc1 <= cc0 || rr1 <= rr0) return out;
    const double sx = size.width() / (c1 - c0), sy = size.height() / (r1 - r0);
    const int ox = int(std::floor((cc0 - c0) * sx)), oy = int(std::floor((rr0 - r0) * sy));
    const int ow = qMax(1, int(std::ceil((cc1 - cc0) * sx))), oh = qMax(1, int(std::ceil((rr1 - rr0) * sy)));
    Handle *handle = acquire();
    if (!handle) return out;
    const int x0 = int(std::floor(cc0)), y0 = int(std::floor(rr0));
    const int w = qMax(1, qMin(m_gridWidth - x0, int(std::ceil(cc1)) - x0)), h = qMax(1, qMin(m_gridHeight - y0, int(std::ceil(rr1)) - y0));
    const QImage part = render(handle->dataset(), x0, y0, w, h, ow, oh, style());
    release(handle);
    QPainter painter(&out);
    painter.drawImage(QPoint(ox, oy), part);
    return out;
}

double RasterSource::valueAt(const QPointF &p, int band) const
{
    if (!m_valid) return NaN;
    const double *gt = m_gridTransform;
    const int col = int(std::floor((p.x() - gt[0]) / gt[1])), row = int(std::floor((p.y() - gt[3]) / gt[5]));
    if (col < 0 || row < 0 || col >= m_gridWidth || row >= m_gridHeight) return NaN;
    Handle *handle = acquire();
    if (!handle) return NaN;
    GDALDatasetH ds = handle->dataset();
    double value = NaN;
    if (band >= 1 && band <= GDALGetRasterCount(ds)) {
        GDALRasterBandH b = GDALGetRasterBand(ds, band);
        if (GDALRasterIO(b, GF_Read, col, row, 1, 1, &value, 1, 1, GDT_Float64, 0, 0) != CE_None) value = NaN;
        int has = 0;
        const double nd = GDALGetRasterNoDataValue(b, &has);
        if (has && value == nd) value = NaN;
        if (m_warp) {   // outside the warped source
            double a = 0;
            GDALRasterBandH alpha = GDALGetRasterBand(ds, GDALGetRasterCount(ds));
            if (GDALRasterIO(alpha, GF_Read, col, row, 1, 1, &a, 1, 1, GDT_Float64, 0, 0) == CE_None && a == 0) value = NaN;
        }
    }
    release(handle);
    return value;
}

QImage RasterSource::render(void *dsHandle, int x0, int y0, int w, int h, int outW, int outH, const RasterStyle &style) const
{
    auto ds = static_cast<GDALDatasetH>(dsHandle);
    const int count = GDALGetRasterCount(ds);
    QVector<int> bands = style.mode == RasterStyle::Rgb ? QVector<int>{style.red, style.green, style.blue}
                                                        : QVector<int>{style.band};
    const int alphaBand = m_warp ? count : style.alpha;
    for (int b : bands) if (b < 1 || b > count) return {};
    if (alphaBand > 0 && alphaBand <= count) bands << alphaBand;

    GDALRasterIOExtraArg extra;
    INIT_RASTERIO_EXTRA_ARG(extra);
    extra.eResampleAlg = outW < w ? GRIORA_Average : GRIORA_Bilinear;
    const int pixels = outW * outH;
    const bool shade = style.mode == RasterStyle::SingleBand && style.hillshade && outW > 2 && outH > 2;
    QVector<float> data(qsizetype(pixels) * bands.size());
    if (GDALDatasetRasterIOEx(ds, GF_Read, x0, y0, w, h, data.data(), outW, outH, GDT_Float32, int(bands.size()),
                              bands.data(), sizeof(float), qsizetype(sizeof(float)) * outW,
                              qsizetype(sizeof(float)) * pixels, &extra) != CE_None)
        return {};

    QVector<double> noData;
    for (int i = 0; i < bands.size(); ++i) {
        int has = 0;
        const double nd = GDALGetRasterNoDataValue(GDALGetRasterBand(ds, bands[i]), &has);
        noData << (has ? nd : NaN);
    }
    const bool hasAlpha = bands.size() > (style.mode == RasterStyle::Rgb ? 3 : 1);
    const float *alpha = hasAlpha ? data.constData() + qsizetype(pixels) * (bands.size() - 1) : nullptr;

    QImage image(outW, outH, QImage::Format_ARGB32_Premultiplied);
    // Cell size of the output pixels in map units, for the hillshade slope.
    const double cellX = std::fabs(m_gridTransform[1]) * w / outW, cellY = std::fabs(m_gridTransform[5]) * h / outH;
    const double zenith = qDegreesToRadians(45.0), azimuth = qDegreesToRadians(360.0 - 315.0 + 90.0);
    for (int y = 0; y < outH; ++y) {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < outW; ++x) {
            const int i = y * outW + x;
            int a = alpha ? qBound(0, int(alpha[i]), 255) : 255;
            int r = 0, g = 0, b = 0;
            if (style.mode == RasterStyle::Rgb) {
                const float v[3] = {data[i], data[pixels + i], data[2 * pixels + i]};
                bool allNoData = true;
                int rgb[3];
                for (int k = 0; k < 3; ++k) {
                    allNoData = allNoData && !std::isnan(noData[k]) && v[k] == noData[k];
                    const double range = style.rgbMax[k] - style.rgbMin[k];
                    rgb[k] = qBound(0, int(std::lround((v[k] - style.rgbMin[k]) / (range > 0 ? range : 1) * 255.0)), 255);
                }
                if (allNoData) a = 0;
                r = rgb[0]; g = rgb[1]; b = rgb[2];
            } else {
                const float v = data[i];
                if (std::isnan(v) || (!std::isnan(noData[0]) && v == noData[0])) {
                    a = 0;
                } else {
                    const double range = style.max - style.min;
                    QColor c = RasterStyle::rampColor(style.ramp, range > 0 ? (v - style.min) / range : 0.5);
                    double factor = 1.0;
                    if (shade) {
                        auto at = [&](int xx, int yy) {
                            xx = qBound(0, xx, outW - 1);
                            yy = qBound(0, yy, outH - 1);
                            const float z = data[yy * outW + xx];
                            return (std::isnan(z) || (!std::isnan(noData[0]) && z == noData[0])) ? double(v) : double(z);
                        };
                        // Horn's method on the 3x3 neighbourhood.
                        const double dzdx = ((at(x + 1, y - 1) + 2 * at(x + 1, y) + at(x + 1, y + 1))
                                             - (at(x - 1, y - 1) + 2 * at(x - 1, y) + at(x - 1, y + 1))) / (8 * cellX);
                        const double dzdy = ((at(x - 1, y + 1) + 2 * at(x, y + 1) + at(x + 1, y + 1))
                                             - (at(x - 1, y - 1) + 2 * at(x, y - 1) + at(x + 1, y - 1))) / (8 * cellY);
                        const double slope = std::atan(std::hypot(dzdx, dzdy));
                        const double aspect = std::atan2(dzdy, -dzdx);
                        const double hs = std::cos(zenith) * std::cos(slope)
                                          + std::sin(zenith) * std::sin(slope) * std::cos(azimuth - aspect);
                        factor = 0.45 + 0.55 * qBound(0.0, hs, 1.0);
                    }
                    r = int(c.red() * factor); g = int(c.green() * factor); b = int(c.blue() * factor);
                }
            }
            line[x] = qPremultiply(qRgba(r, g, b, a));
        }
    }
    return image;
}

// ------------------------------------------------------------- PyramidBuilder

bool PyramidBuilder::needed(const RasterInfo &info)
{
    return info.valid && info.overviews == 0 && qMax(info.width, info.height) > 4096;
}

QString PyramidBuilder::cachePathFor(const QString &rasterPath, const QString &cacheDirectory)
{
    const QFileInfo fi(rasterPath);
    const QByteArray key = (fi.absoluteFilePath() + QLatin1Char('|') + QString::number(fi.size()) + QLatin1Char('|')
                            + QString::number(fi.lastModified().toMSecsSinceEpoch())).toUtf8();
    const QString name = QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex().left(16));
    return QDir(cacheDirectory).filePath(name + QStringLiteral(".vrt"));
}

bool PyramidBuilder::isComplete(const QString &vrtPath)
{
    return QFileInfo(vrtPath).isFile() && QFileInfo(vrtPath + QStringLiteral(".ovr")).isFile();
}

bool PyramidBuilder::build(const QString &rasterPath, const QString &vrtPath,
                           const std::function<bool(double)> &progress, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error) *error = message;
        return false;
    };
    QDir().mkpath(QFileInfo(vrtPath).absolutePath());
    const QString tmp = vrtPath.left(vrtPath.size() - 4) + QStringLiteral(".tmp.vrt");
    QFile::remove(tmp);
    QFile::remove(tmp + QStringLiteral(".ovr"));
    GisEnvironment::clearError();
    GDALDatasetH src = openRaster(rasterPath);
    if (!src) return fail(QStringLiteral("Не удалось открыть растр: %1").arg(GisEnvironment::lastError()));
    ArgList list({QStringLiteral("-of"), QStringLiteral("VRT")});
    GDALTranslateOptions *options = GDALTranslateOptionsNew(list.data(), nullptr);
    int usageError = FALSE;
    GDALDatasetH vrt = GDALTranslate(utf8(tmp).constData(), src, options, &usageError);
    GDALTranslateOptionsFree(options);
    if (!vrt) {
        GDALClose(src);
        return fail(QStringLiteral("Не удалось подготовить пирамиды: %1").arg(GisEnvironment::lastError()));
    }
    GDALClose(vrt);
    GDALClose(src);

    // Read-only open: GDAL writes the overviews to <tmp>.ovr, never into the source file.
    vrt = openRaster(tmp);
    if (!vrt) return fail(QStringLiteral("Не удалось открыть VRT пирамид: %1").arg(GisEnvironment::lastError()));
    QVector<int> levels;
    const int size = qMax(GDALGetRasterXSize(vrt), GDALGetRasterYSize(vrt));
    for (int f = 2; size / f >= 128; f *= 2) levels << f;
    CPLSetThreadLocalConfigOption("COMPRESS_OVERVIEW", "DEFLATE");
    CPLSetThreadLocalConfigOption("BIGTIFF_OVERVIEW", "IF_SAFER");
    CPLSetThreadLocalConfigOption("GDAL_NUM_THREADS", "ALL_CPUS");
    const CPLErr result = GDALBuildOverviews(vrt, "AVERAGE", int(levels.size()), levels.data(), 0, nullptr,
                                             progressAdapter, const_cast<std::function<bool(double)> *>(&progress));
    CPLSetThreadLocalConfigOption("COMPRESS_OVERVIEW", nullptr);
    CPLSetThreadLocalConfigOption("BIGTIFF_OVERVIEW", nullptr);
    CPLSetThreadLocalConfigOption("GDAL_NUM_THREADS", nullptr);
    const QString message = GisEnvironment::lastError();
    GDALClose(vrt);
    if (result != CE_None) {
        QFile::remove(tmp);
        QFile::remove(tmp + QStringLiteral(".ovr"));
        return fail(message.contains(QLatin1String("User terminated"), Qt::CaseInsensitive) || message.isEmpty()
                        ? QStringLiteral("Построение пирамид отменено.")
                        : QStringLiteral("Не удалось построить пирамиды: %1").arg(message));
    }
    QFile::remove(vrtPath);
    QFile::remove(vrtPath + QStringLiteral(".ovr"));
    // The .ovr name follows the VRT name: rename both.
    if (!QFile::rename(tmp + QStringLiteral(".ovr"), vrtPath + QStringLiteral(".ovr")) || !QFile::rename(tmp, vrtPath))
        return fail(QStringLiteral("Не удалось сохранить пирамиды в %1").arg(QDir::toNativeSeparators(vrtPath)));
    return true;
}
