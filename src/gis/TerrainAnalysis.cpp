#include "TerrainAnalysis.h"

#include "Geodesy.h"
#include "GisEnvironment.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPolygonF>
#include <QVector3D>
#include <QtMath>
#include <cmath>
#include <memory>
#include <cpl_string.h>
#include <cpl_vsi.h>
#include <gdal.h>
#include <gdal_alg.h>
#include <gdal_utils.h>
#include <ogr_api.h>
#include <ogr_srs_api.h>

namespace terrain {

namespace {

QByteArray nativePath(const QString &path) { return QDir::toNativeSeparators(path).toUtf8(); }

// An elevation model opened read-only: first band, geotransform (north-up only), CRS, nodata.
struct Dem
{
    GDALDatasetH ds = nullptr;
    GDALRasterBandH band = nullptr;
    double gt[6] = {0, 1, 0, 0, 0, -1};
    int width = 0, height = 0;
    Crs crs;
    bool hasNoData = false;
    double noData = 0;
    QString error;

    explicit Dem(const QString &path)
    {
        GisEnvironment::clearError();
        ds = GDALOpenEx(nativePath(path).constData(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
        if (!ds) {
            error = QStringLiteral("Модель высот не открывается: %1").arg(GisEnvironment::lastError());
            return;
        }
        if (GDALGetRasterCount(ds) < 1) {
            error = QStringLiteral("В файле нет растровых каналов");
            return;
        }
        band = GDALGetRasterBand(ds, 1);
        width = GDALGetRasterXSize(ds);
        height = GDALGetRasterYSize(ds);
        if (GDALGetGeoTransform(ds, gt) != CE_None || gt[2] != 0 || gt[4] != 0 || gt[1] == 0 || gt[5] == 0) {
            error = QStringLiteral("У модели высот нет геопривязки (или растр повёрнут)");
            return;
        }
        const char *wkt = GDALGetProjectionRef(ds);
        if (wkt && *wkt) crs = Crs::fromWkt(QString::fromUtf8(wkt));
        int has = 0;
        noData = GDALGetRasterNoDataValue(band, &has);
        hasNoData = has != 0;
    }
    ~Dem() { if (ds) GDALClose(ds); }
    Dem(const Dem &) = delete;
    Dem &operator=(const Dem &) = delete;

    bool valid(double z) const
    {
        return std::isfinite(z) && !(hasNoData && std::fabs(z - noData) <= 1e-6 * qMax(1.0, std::fabs(noData))) && z > -1e30;
    }
    // Height at a point of the DEM's CRS, bilinear between cell centres; NaN outside or on nodata.
    double sample(double x, double y) const
    {
        const double px = (x - gt[0]) / gt[1] - 0.5, py = (y - gt[3]) / gt[5] - 0.5;
        if (px < -0.5 || py < -0.5 || px > width - 0.5 || py > height - 0.5) return qQNaN();
        const int ix = qBound(0, int(std::floor(px)), qMax(0, width - 2)), iy = qBound(0, int(std::floor(py)), qMax(0, height - 2));
        const int nx = qMin(2, width), ny = qMin(2, height);
        double v[4] = {qQNaN(), qQNaN(), qQNaN(), qQNaN()};
        double buffer[4];
        if (GDALRasterIO(band, GF_Read, ix, iy, nx, ny, buffer, nx, ny, GDT_Float64, 0, 0) != CE_None) return qQNaN();
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i) v[j * 2 + i] = buffer[j * nx + i];
        if (nx == 1) { v[1] = v[0]; v[3] = v[2]; }
        if (ny == 1) { v[2] = v[0]; v[3] = v[1]; }
        const double fx = qBound(0.0, px - ix, 1.0), fy = qBound(0.0, py - iy, 1.0);
        const double w[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
        double sum = 0, weight = 0;
        for (int k = 0; k < 4; ++k) {
            if (w[k] <= 1e-12) continue;
            if (!valid(v[k])) return qQNaN();   // no heights invented across holes
            sum += w[k] * v[k];
            weight += w[k];
        }
        return weight > 0 ? sum / weight : qQNaN();
    }
    double cellSize() const { return qMin(std::fabs(gt[1]), std::fabs(gt[5])); }
};

Stats bandStats(GDALDatasetH ds)
{
    Stats s;
    if (!ds) return s;
    double mn = 0, mx = 0, mean = 0, sd = 0;
    if (GDALComputeRasterStatistics(GDALGetRasterBand(ds, 1), FALSE, &mn, &mx, &mean, &sd, nullptr, nullptr) == CE_None) {
        s.min = mn;
        s.max = mx;
        s.mean = mean;
        s.valid = true;
    }
    return s;
}

RasterResult runDem(const char *processing, const QString &demPath, const QString &out, QStringList extra)
{
    RasterResult r;
    r.path = out;
    Dem dem(demPath);
    if (!dem.error.isEmpty()) {
        r.error = dem.error;
        return r;
    }
    // Horizontal units to metres (heights are metres).
    if (dem.crs.isGeographic()) extra << QStringLiteral("-s") << QStringLiteral("111120");
    else if (dem.crs.isProjected() && std::fabs(dem.crs.metersPerUnit() - 1.0) > 1e-9)
        extra << QStringLiteral("-s") << QString::number(dem.crs.metersPerUnit(), 'g', 12);
    // No -compute_edges: GDAL pads the outer ring by repeating values, which halves the slope there;
    // the outer ring stays nodata rather than wrong.
    QStringList args = {QStringLiteral("-of"), QStringLiteral("GTiff"),
                        QStringLiteral("-co"), QStringLiteral("COMPRESS=DEFLATE"), QStringLiteral("-co"), QStringLiteral("TILED=YES"),
                        QStringLiteral("-co"), QStringLiteral("BIGTIFF=IF_SAFER")};
    args << extra;
    QList<QByteArray> bytes;
    for (const QString &a : args) bytes << a.toUtf8();
    QVector<char *> argv;
    for (QByteArray &b : bytes) argv << b.data();
    argv << nullptr;
    GDALDEMProcessingOptions *options = GDALDEMProcessingOptionsNew(argv.data(), nullptr);
    QDir().mkpath(QFileInfo(out).absolutePath());
    GisEnvironment::clearError();
    int usageError = 0;
    GDALDatasetH dst = GDALDEMProcessing(nativePath(out).constData(), dem.ds, processing, nullptr, options, &usageError);
    GDALDEMProcessingOptionsFree(options);
    if (!dst) {
        r.error = QStringLiteral("GDAL: %1").arg(GisEnvironment::lastError());
        QFile::remove(out);
        return r;
    }
    r.stats = bandStats(dst);
    GDALClose(dst);
    return r;
}

} // namespace

RasterResult slope(const QString &dem, const QString &out, bool percent)
{
    return runDem("slope", dem, out, percent ? QStringList{QStringLiteral("-p")} : QStringList{});
}

RasterResult aspect(const QString &dem, const QString &out)
{
    return runDem("aspect", dem, out, {});
}

RasterResult difference(const QString &demA, const QString &demB, const QString &out)
{
    RasterResult r;
    r.path = out;
    Dem a(demA);
    if (!a.error.isEmpty()) {
        r.error = QStringLiteral("Модель A: %1").arg(a.error);
        return r;
    }
    Dem b(demB);
    if (!b.error.isEmpty()) {
        r.error = QStringLiteral("Модель B: %1").arg(b.error);
        return r;
    }
    if (!a.crs.isValid() || !b.crs.isValid()) {
        r.error = QStringLiteral("У одной из моделей нет системы координат");
        return r;
    }
    // B on the grid of A, lazily (a warped VRT in memory): only the rows read are resampled.
    const double xmin = a.gt[0], xmax = a.gt[0] + a.gt[1] * a.width;
    const double y0 = a.gt[3], y1 = a.gt[3] + a.gt[5] * a.height;
    QList<QByteArray> args = {"-of", "VRT", "-r", "bilinear", "-t_srs", a.crs.wkt().toUtf8(),
                              "-te", QByteArray::number(xmin, 'f', 9), QByteArray::number(qMin(y0, y1), 'f', 9),
                              QByteArray::number(xmax, 'f', 9), QByteArray::number(qMax(y0, y1), 'f', 9),
                              "-ts", QByteArray::number(a.width), QByteArray::number(a.height), "-dstnodata", "-3.4e38"};
    QVector<char *> argv;
    for (QByteArray &s : args) argv << s.data();
    argv << nullptr;
    GDALWarpAppOptions *options = GDALWarpAppOptionsNew(argv.data(), nullptr);
    GDALDatasetH sources[1] = {b.ds};
    int usageError = 0;
    const QByteArray memPath = QByteArray("/vsimem/geomap-diff-") + QByteArray::number(quintptr(&r)) + ".vrt";
    GisEnvironment::clearError();
    GDALDatasetH warped = GDALWarp(memPath.constData(), nullptr, 1, sources, options, &usageError);
    GDALWarpAppOptionsFree(options);
    if (!warped) {
        r.error = QStringLiteral("Не удалось совместить модели: %1").arg(GisEnvironment::lastError());
        return r;
    }
    GDALRasterBandH wb = GDALGetRasterBand(warped, 1);
    QDir().mkpath(QFileInfo(out).absolutePath());
    QFile::remove(out);
    const char *create[] = {"COMPRESS=DEFLATE", "TILED=YES", "BIGTIFF=IF_SAFER", nullptr};
    GDALDatasetH dst = GDALCreate(GDALGetDriverByName("GTiff"), nativePath(out).constData(), a.width, a.height, 1, GDT_Float32,
                                  const_cast<char **>(create));
    if (!dst) {
        GDALClose(warped);
        VSIUnlink(memPath.constData());
        r.error = QStringLiteral("Не удалось создать %1: %2").arg(QDir::toNativeSeparators(out), GisEnvironment::lastError());
        return r;
    }
    GDALSetGeoTransform(dst, a.gt);
    GDALSetProjection(dst, GDALGetProjectionRef(a.ds));
    GDALRasterBandH db = GDALGetRasterBand(dst, 1);
    constexpr double NoData = -9999.0;
    GDALSetRasterNoDataValue(db, NoData);
    QVector<double> ra(a.width), rb(a.width);
    QVector<float> rd(a.width);
    double sum = 0;
    qint64 n = 0;
    bool ok = true;
    for (int y = 0; y < a.height && ok; ++y) {
        ok = GDALRasterIO(a.band, GF_Read, 0, y, a.width, 1, ra.data(), a.width, 1, GDT_Float64, 0, 0) == CE_None
          && GDALRasterIO(wb, GF_Read, 0, y, a.width, 1, rb.data(), a.width, 1, GDT_Float64, 0, 0) == CE_None;
        for (int x = 0; ok && x < a.width; ++x) {
            const bool valid = a.valid(ra[x]) && std::isfinite(rb[x]) && rb[x] > -1e30 && b.valid(rb[x]);
            const double d = valid ? ra[x] - rb[x] : NoData;
            rd[x] = float(d);
            if (valid) {
                r.stats.min = n == 0 ? d : qMin(r.stats.min, d);
                r.stats.max = n == 0 ? d : qMax(r.stats.max, d);
                sum += d;
                ++n;
            }
        }
        ok = ok && GDALRasterIO(db, GF_Write, 0, y, a.width, 1, rd.data(), a.width, 1, GDT_Float32, 0, 0) == CE_None;
    }
    GDALClose(dst);
    GDALClose(warped);
    VSIUnlink(memPath.constData());
    if (!ok) {
        QFile::remove(out);
        r.error = QStringLiteral("Ошибка чтения или записи высот");
        return r;
    }
    if (n == 0) {
        QFile::remove(out);
        r.error = QStringLiteral("Модели не перекрываются (нет общих ячеек с данными)");
        return r;
    }
    r.stats.mean = sum / n;
    r.stats.valid = true;
    return r;
}

double suggestedInterval(double minZ, double maxZ)
{
    const double range = maxZ - minZ;
    if (!(range > 0) || !std::isfinite(range)) return 1.0;
    const double raw = range / 15.0;
    const double magnitude = std::pow(10.0, std::floor(std::log10(raw)));
    for (double k : {1.0, 2.0, 2.5, 5.0, 10.0})
        if (k * magnitude >= raw * 0.999) return k * magnitude;
    return 10.0 * magnitude;
}

ContourResult contours(const QString &demPath, const QString &outGpkg, double interval, double base)
{
    ContourResult r;
    r.path = outGpkg;
    if (!(interval > 0)) {
        r.error = QStringLiteral("Шаг горизонталей должен быть больше нуля");
        return r;
    }
    Dem dem(demPath);
    if (!dem.error.isEmpty()) {
        r.error = dem.error;
        return r;
    }
    // Refuse levels that would flood the file (a typo such as 0.001 m on a 100 m relief).
    double mm[2] = {0, 0};
    if (GDALComputeRasterMinMax(dem.band, FALSE, mm) == CE_None && (mm[1] - mm[0]) / interval > 5000) {
        r.error = QStringLiteral("Слишком частые горизонтали: перепад %1 м при шаге %2 м даёт больше 5000 уровней")
                      .arg(mm[1] - mm[0], 0, 'f', 1).arg(interval);
        return r;
    }
    GDALDriverH driver = GDALGetDriverByName("GPKG");
    if (!driver) {
        r.error = QStringLiteral("Нет драйвера GeoPackage в GDAL");
        return r;
    }
    QDir().mkpath(QFileInfo(outGpkg).absolutePath());
    QFile::remove(outGpkg);
    GisEnvironment::clearError();
    GDALDatasetH dst = GDALCreate(driver, nativePath(outGpkg).constData(), 0, 0, 0, GDT_Unknown, nullptr);
    if (!dst) {
        r.error = QStringLiteral("Не удалось создать %1: %2").arg(QDir::toNativeSeparators(outGpkg), GisEnvironment::lastError());
        return r;
    }
    OGRLayerH layer = GDALDatasetCreateLayer(dst, "contours", GDALGetSpatialRef(dem.ds), wkbLineString, nullptr);
    OGRFieldDefnH idField = OGR_Fld_Create("id", OFTInteger);
    OGRFieldDefnH elevField = OGR_Fld_Create("elev", OFTReal);
    const bool fields = layer && OGR_L_CreateField(layer, idField, TRUE) == OGRERR_NONE && OGR_L_CreateField(layer, elevField, TRUE) == OGRERR_NONE;
    OGR_Fld_Destroy(idField);
    OGR_Fld_Destroy(elevField);
    if (!fields) {
        GDALClose(dst);
        QFile::remove(outGpkg);
        r.error = QStringLiteral("Не удалось создать слой горизонталей: %1").arg(GisEnvironment::lastError());
        return r;
    }
    char **options = nullptr;
    options = CSLSetNameValue(options, "LEVEL_INTERVAL", QByteArray::number(interval, 'g', 12).constData());
    options = CSLSetNameValue(options, "LEVEL_BASE", QByteArray::number(base, 'g', 12).constData());
    options = CSLSetNameValue(options, "ID_FIELD", "0");
    options = CSLSetNameValue(options, "ELEV_FIELD", "1");
    if (dem.hasNoData) options = CSLSetNameValue(options, "NODATA", QByteArray::number(dem.noData, 'g', 17).constData());
    GDALDatasetStartTransaction(dst, FALSE);
    const CPLErr err = GDALContourGenerateEx(dem.band, layer, options, nullptr, nullptr);
    GDALDatasetCommitTransaction(dst);
    CSLDestroy(options);
    if (err != CE_None) {
        GDALClose(dst);
        QFile::remove(outGpkg);
        r.error = QStringLiteral("GDAL: %1").arg(GisEnvironment::lastError());
        return r;
    }
    OGR_L_ResetReading(layer);
    bool first = true;
    while (OGRFeatureH f = OGR_L_GetNextFeature(layer)) {
        const double z = OGR_F_GetFieldAsDouble(f, 1);
        r.minLevel = first ? z : qMin(r.minLevel, z);
        r.maxLevel = first ? z : qMax(r.maxLevel, z);
        first = false;
        ++r.lines;
        OGR_F_Destroy(f);
    }
    GDALClose(dst);
    return r;
}

Profile profile(const QString &demPath, const QVector<QPointF> &line, const Crs &lineCrs, int samples)
{
    Profile p;
    if (line.size() < 2) {
        p.error = QStringLiteral("Профиль строится по линии из двух и более точек");
        return p;
    }
    Dem dem(demPath);
    if (!dem.error.isEmpty()) {
        p.error = dem.error;
        return p;
    }
    if (!dem.crs.isValid() || !lineCrs.isValid()) {
        p.error = QStringLiteral("У модели высот или карты не задана система координат");
        return p;
    }
    samples = qBound(2, samples, 20000);
    // Even spacing along the line in its CRS, distances on the ellipsoid.
    QVector<double> cumulative{0.0};
    for (int i = 1; i < line.size(); ++i) cumulative << cumulative.last() + std::hypot(line[i].x() - line[i - 1].x(), line[i].y() - line[i - 1].y());
    const double planar = cumulative.last();
    if (!(planar > 0)) {
        p.error = QStringLiteral("У линии нулевая длина");
        return p;
    }
    QVector<QPointF> pts;
    pts.reserve(samples);
    int seg = 1;
    for (int k = 0; k < samples; ++k) {
        const double s = planar * k / (samples - 1);
        while (seg < line.size() - 1 && cumulative[seg] < s) ++seg;
        const double len = cumulative[seg] - cumulative[seg - 1];
        const double t = len > 0 ? qBound(0.0, (s - cumulative[seg - 1]) / len, 1.0) : 0.0;
        pts << line[seg - 1] + (line[seg] - line[seg - 1]) * t;
    }
    QVector<QPointF> geo = pts, inDem = pts, geoLine = line;
    CoordinateTransform toGeo(lineCrs, lineCrs.geographic()), toDem(lineCrs, dem.crs);
    if (!toGeo.transform(geo) || !toGeo.transform(geoLine) || !toDem.transform(inDem)) {
        p.error = QStringLiteral("Не удалось пересчитать координаты линии в систему модели высот");
        return p;
    }
    const Geodesy::Ellipsoid e = Geodesy::ellipsoidOf(lineCrs);
    p.length = Geodesy::length(geoLine, e);
    double distance = 0;
    QVector<double> distances{0.0};
    for (int k = 1; k < samples; ++k) {
        distance += Geodesy::distance(geo[k - 1], geo[k], e);
        distances << distance;
    }
    const double scale = distance > 0 ? p.length / distance : 1.0;   // samples on chords of the vertices' geodesics
    bool first = true;
    const ProfilePoint *last = nullptr;
    p.points.reserve(samples);
    for (int k = 0; k < samples; ++k) {
        ProfilePoint pp;
        pp.distance = distances[k] * scale;
        pp.x = pts[k].x();
        pp.y = pts[k].y();
        pp.z = dem.sample(inDem[k].x(), inDem[k].y());
        p.points << pp;
    }
    for (const ProfilePoint &pp : p.points) {
        if (!std::isfinite(pp.z)) {
            ++p.noData;
            last = nullptr;
            continue;
        }
        p.minZ = first ? pp.z : qMin(p.minZ, pp.z);
        p.maxZ = first ? pp.z : qMax(p.maxZ, pp.z);
        first = false;
        if (last) {
            const double dz = pp.z - last->z, dd = pp.distance - last->distance;
            if (dz > 0) p.ascent += dz; else p.descent -= dz;
            if (dd > 1e-9) p.maxSlope = qMax(p.maxSlope, qRadiansToDegrees(std::atan(std::fabs(dz) / dd)));
        }
        last = &pp;
    }
    if (first) p.error = QStringLiteral("Линия не проходит по данным модели высот");
    return p;
}

Volume volume(const QString &demPath, const QVector<QPointF> &polygon, const Crs &polygonCrs, Base base, double level,
              const QString &baseSurface)
{
    Volume v;
    if (polygon.size() < 3) {
        v.error = QStringLiteral("Объём считается внутри полигона из трёх и более точек");
        return v;
    }
    Dem dem(demPath);
    if (!dem.error.isEmpty()) {
        v.error = dem.error;
        return v;
    }
    if (!dem.crs.isProjected()) {
        v.error = QStringLiteral("Объём считается для модели высот в проекционной системе координат (метры), например UTM");
        return v;
    }
    QVector<QPointF> ring = polygon;
    CoordinateTransform toDem(polygonCrs, dem.crs);
    if (!toDem.transform(ring)) {
        v.error = QStringLiteral("Не удалось пересчитать полигон в систему модели высот");
        return v;
    }
    const QPolygonF poly(ring);
    const QRectF box = poly.boundingRect();
    const double k = dem.crs.metersPerUnit();
    const double cellArea = std::fabs(dem.gt[1] * dem.gt[5]) * k * k;
    // Pixel window of the polygon.
    const int c0 = qBound(0, int(std::floor((box.left() - dem.gt[0]) / dem.gt[1])), dem.width);
    const int c1 = qBound(0, int(std::ceil((box.right() - dem.gt[0]) / dem.gt[1])), dem.width);
    const double ya = (box.top() - dem.gt[3]) / dem.gt[5], yb = (box.bottom() - dem.gt[3]) / dem.gt[5];
    const int r0 = qBound(0, int(std::floor(qMin(ya, yb))), dem.height), r1 = qBound(0, int(std::ceil(qMax(ya, yb))), dem.height);
    if (c1 <= c0 || r1 <= r0) {
        v.error = QStringLiteral("Полигон вне модели высот");
        return v;
    }
    const qint64 cellsInWindow = qint64(c1 - c0) * (r1 - r0);
    if (cellsInWindow > 400'000'000) {
        v.error = QStringLiteral("Полигон слишком велик для этой модели высот (больше 400 млн ячеек)");
        return v;
    }

    // Boundary samples (every cell along the edges) for the bases taken from the boundary.
    QVector<QVector3D> edge;
    if (base == Base::BoundaryMean || base == Base::BoundaryMin || base == Base::BestFit) {
        const double step = dem.cellSize();
        for (int i = 0; i < ring.size(); ++i) {
            const QPointF a = ring[i], b = ring[(i + 1) % ring.size()];
            const int n = qMax(1, int(std::ceil(std::hypot(b.x() - a.x(), b.y() - a.y()) / step)));
            for (int j = 0; j < n; ++j) {
                const QPointF q = a + (b - a) * (double(j) / n);
                const double z = dem.sample(q.x(), q.y());
                if (std::isfinite(z)) edge << QVector3D(float(q.x() - box.center().x()), float(q.y() - box.center().y()), float(z));
            }
        }
        if (edge.size() < 3) {
            v.error = QStringLiteral("На границе полигона нет высот (контур вне данных модели)");
            return v;
        }
    }
    double baseA = 0, baseB = 0, baseC = level;   // base(x, y) = A·x + B·y + C (centred coordinates)
    if (base == Base::BoundaryMean) {
        double sum = 0;
        for (const QVector3D &q : edge) sum += q.z();
        baseC = sum / edge.size();
    } else if (base == Base::BoundaryMin) {
        baseC = edge.first().z();
        for (const QVector3D &q : edge) baseC = qMin(baseC, double(q.z()));
    } else if (base == Base::BestFit) {
        // Normal equations of z = A x + B y + C.
        double sxx = 0, sxy = 0, syy = 0, sx = 0, sy = 0, sz = 0, sxz = 0, syz = 0;
        const double n = edge.size();
        for (const QVector3D &q : edge) {
            sxx += q.x() * q.x(); sxy += q.x() * q.y(); syy += q.y() * q.y();
            sx += q.x(); sy += q.y(); sz += q.z(); sxz += q.x() * q.z(); syz += q.y() * q.z();
        }
        const double m[3][3] = {{sxx, sxy, sx}, {sxy, syy, sy}, {sx, sy, n}};
        const double rhs[3] = {sxz, syz, sz};
        const auto det3 = [](const double a[3][3]) {
            return a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
                 + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
        };
        const double d = det3(m);
        if (std::fabs(d) < 1e-12) {
            v.error = QStringLiteral("Плоскость по границе не определяется (вырожденный контур)");
            return v;
        }
        double sol[3];
        for (int c = 0; c < 3; ++c) {
            double t[3][3];
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) t[i][j] = j == c ? rhs[i] : m[i][j];
            sol[c] = det3(t) / d;
        }
        baseA = sol[0];
        baseB = sol[1];
        baseC = sol[2];
    }
    if (base != Base::BestFit && base != Base::Surface) v.baseLevel = baseC;

    std::unique_ptr<Dem> surface;
    std::unique_ptr<CoordinateTransform> toSurface;
    if (base == Base::Surface) {
        surface = std::make_unique<Dem>(baseSurface);
        if (!surface->error.isEmpty()) {
            v.error = QStringLiteral("Базовая поверхность: %1").arg(surface->error);
            return v;
        }
        if (!surface->crs.isValid()) {
            v.error = QStringLiteral("У базовой поверхности нет системы координат");
            return v;
        }
        toSurface = std::make_unique<CoordinateTransform>(dem.crs, surface->crs);
    }

    const int w = c1 - c0;
    QVector<double> row(w);
    const QPointF centre = box.center();
    for (int r = r0; r < r1; ++r) {
        if (GDALRasterIO(dem.band, GF_Read, c0, r, w, 1, row.data(), w, 1, GDT_Float64, 0, 0) != CE_None) {
            v.error = QStringLiteral("Не удалось прочитать высоты");
            return v;
        }
        const double y = dem.gt[3] + (r + 0.5) * dem.gt[5];
        for (int c = 0; c < w; ++c) {
            const double x = dem.gt[0] + (c0 + c + 0.5) * dem.gt[1];
            if (!poly.containsPoint(QPointF(x, y), Qt::OddEvenFill)) continue;
            const double z = row[c];
            double b;
            if (base == Base::Surface) {
                double sx = x, sy = y;
                b = toSurface->transform(sx, sy) ? surface->sample(sx, sy) : qQNaN();
            } else {
                b = baseA * (x - centre.x()) + baseB * (y - centre.y()) + baseC;
            }
            if (!dem.valid(z) || !std::isfinite(b)) {
                ++v.noData;
                continue;
            }
            const double dz = z - b;
            if (dz > 0) v.above += dz * cellArea;
            else v.below -= dz * cellArea;
            ++v.cells;
        }
    }
    v.area = v.cells * cellArea;
    if (v.cells == 0) v.error = QStringLiteral("Внутри полигона нет ячеек модели высот с данными");
    return v;
}

// ------------------------------------------------------------------ cross sections

QVector<Profile> crossSections(const QString &demPath, const QVector<QPointF> &line, const Crs &lineCrs, double spacing,
                               double halfWidth, int samples, QVector<QVector<QPointF>> *sections)
{
    QVector<Profile> result;
    if (line.size() < 2 || !(spacing > 0) || !(halfWidth > 0)) return result;
    if (!lineCrs.isProjected()) {
        Profile p;
        p.error = QStringLiteral("Поперечные профили строятся в проекции с метрами (задайте проецированную CRS карты)");
        result << p;
        return result;
    }
    const double k = lineCrs.metersPerUnit();
    double total = 0;
    QVector<double> at{0};
    for (int i = 1; i < line.size(); ++i) {
        total += std::hypot(line[i].x() - line[i - 1].x(), line[i].y() - line[i - 1].y()) * k;
        at << total;
    }
    for (double s = 0; s <= total + 1e-9; s += spacing) {
        int seg = 1;
        while (seg < line.size() - 1 && at[seg] < s) ++seg;
        const QPointF a = line[seg - 1], b = line[seg];
        const double len = qMax(1e-12, at[seg] - at[seg - 1]);
        const double t = qBound(0.0, (s - at[seg - 1]) / len, 1.0);
        const QPointF c = a + (b - a) * t;
        const double dx = (b.x() - a.x()) * k / len, dy = (b.y() - a.y()) * k / len;   // unit direction
        const QPointF left(c.x() - dy * halfWidth / k, c.y() + dx * halfWidth / k), right(c.x() + dy * halfWidth / k, c.y() - dx * halfWidth / k);
        result << profile(demPath, {left, right}, lineCrs, samples);
        if (sections) *sections << QVector<QPointF>{left, right};
    }
    return result;
}

// ------------------------------------------------------------------ viewshed

Viewshed viewshed(const QString &demPath, const QPointF &observer, const Crs &observerCrs, double observerHeight,
                  double targetHeight, double radius, const QString &out)
{
    Viewshed v;
    v.path = out;
    Dem dem(demPath);
    if (!dem.error.isEmpty()) {
        v.error = dem.error;
        return v;
    }
    if (!dem.crs.isProjected()) {
        v.error = QStringLiteral("Анализ видимости выполняется для модели высот в проекции с метрами");
        return v;
    }
    QVector<QPointF> p{observer};
    if (observerCrs.isValid() && !observerCrs.isSame(dem.crs) && !CoordinateTransform(observerCrs, dem.crs).transform(p)) {
        v.error = QStringLiteral("Не удалось пересчитать точку наблюдения в систему координат модели");
        return v;
    }
    v.observerZ = dem.sample(p[0].x(), p[0].y());
    if (!std::isfinite(v.observerZ)) {
        v.error = QStringLiteral("Точка наблюдения вне модели высот или на участке без данных");
        return v;
    }
    const double k = dem.crs.metersPerUnit();
    QDir().mkpath(QFileInfo(out).absolutePath());
    // Into an uncompressed file first: the cells without heights are masked in place below, then
    // the result is copied compressed.
    const QString raw = out + QStringLiteral(".partial.tif");
    const char *rawOptions[] = {"TILED=YES", nullptr};
    GisEnvironment::clearError();
    GDALDatasetH ds = GDALViewshedGenerate(dem.band, "GTiff", nativePath(raw).constData(), rawOptions, p[0].x(), p[0].y(),
                                           observerHeight / k, targetHeight / k, 1, 0, 255, 255, 0.85714, GVM_Edge, radius / k,
                                           nullptr, nullptr, GVOT_NORMAL, nullptr);
    if (!ds) {
        v.error = QStringLiteral("GDAL: %1").arg(GisEnvironment::lastError());
        QFile::remove(raw);
        return v;
    }
    GDALRasterBandH band = GDALGetRasterBand(ds, 1);
    GDALSetRasterNoDataValue(band, 255);
    double gt[6];
    GDALGetGeoTransform(ds, gt);
    const int w = GDALGetRasterXSize(ds), h = GDALGetRasterYSize(ds);
    const double cell = std::fabs(gt[1] * gt[5]) * k * k;
    // GDAL's viewshed ignores the model's nodata: such cells come out "not visible". They become
    // nodata here, so the map and the areas cover only the cells with heights.
    const int xoff = int(std::lround((gt[0] - dem.gt[0]) / dem.gt[1])), yoff = int(std::lround((gt[3] - dem.gt[3]) / dem.gt[5]));
    const int x0 = qMax(0, -xoff), x1 = qMin(w, dem.width - xoff);
    std::vector<uchar> row(w);
    std::vector<double> heights(size_t(qMax(0, x1 - x0)));
    for (int y = 0; y < h; ++y) {
        if (GDALRasterIO(band, GF_Read, 0, y, w, 1, row.data(), w, 1, GDT_Byte, 0, 0) != CE_None) break;
        bool masked = false;
        const int dy = yoff + y;
        if (x1 > x0 && dy >= 0 && dy < dem.height
            && GDALRasterIO(dem.band, GF_Read, xoff + x0, dy, x1 - x0, 1, heights.data(), x1 - x0, 1, GDT_Float64, 0, 0) == CE_None) {
            for (int x = x0; x < x1; ++x)
                if (row[x] != 255 && !dem.valid(heights[size_t(x - x0)])) {
                    row[x] = 255;
                    masked = true;
                }
        }
        if (masked) GDALRasterIO(band, GF_Write, 0, y, w, 1, row.data(), w, 1, GDT_Byte, 0, 0);
        for (int x = 0; x < w; ++x) {
            if (row[x] == 255) continue;
            v.area += cell;
            if (row[x] == 1) v.visibleArea += cell;
        }
    }
    const char *create[] = {"COMPRESS=DEFLATE", "TILED=YES", nullptr};
    GDALDatasetH copy = GDALCreateCopy(GDALGetDriverByName("GTiff"), nativePath(out).constData(), ds, FALSE,
                                       const_cast<char **>(create), nullptr, nullptr);
    GDALClose(ds);
    QFile::remove(raw);
    if (!copy) {
        v.error = QStringLiteral("GDAL: %1").arg(GisEnvironment::lastError());
        return v;
    }
    GDALClose(copy);
    return v;
}

// ------------------------------------------------------------------ raster calculator

namespace {
// The expression compiled to a stack program.
struct Program
{
    enum Op { Number, Band, Add, Sub, Mul, Div, Neg, Sqrt, Abs, Min, Max };
    struct Step { Op op; double value = 0; int band = 0; };
    QVector<Step> steps;
    QSet<int> bands;
    QString error;
};

class Parser
{
public:
    Parser(const QString &text, int bands) : m_text(QString(text).remove(QLatin1Char(' ')).toUpper()), m_bands(bands) {}
    Program parse()
    {
        if (m_text.isEmpty()) m_p.error = QStringLiteral("Пустое выражение");
        else {
            sum();
            if (m_p.error.isEmpty() && m_pos < m_text.size()) fail(QStringLiteral("Лишний символ «%1»").arg(m_text[m_pos]));
        }
        return m_p;
    }

private:
    void fail(const QString &e) { if (m_p.error.isEmpty()) m_p.error = e; }
    bool eat(QChar c)
    {
        if (m_pos < m_text.size() && m_text[m_pos] == c) { ++m_pos; return true; }
        return false;
    }
    void sum()
    {
        product();
        while (m_p.error.isEmpty()) {
            if (eat(QLatin1Char('+'))) { product(); m_p.steps << Program::Step{Program::Add}; }
            else if (eat(QLatin1Char('-'))) { product(); m_p.steps << Program::Step{Program::Sub}; }
            else break;
        }
    }
    void product()
    {
        unary();
        while (m_p.error.isEmpty()) {
            if (eat(QLatin1Char('*'))) { unary(); m_p.steps << Program::Step{Program::Mul}; }
            else if (eat(QLatin1Char('/'))) { unary(); m_p.steps << Program::Step{Program::Div}; }
            else break;
        }
    }
    void unary()
    {
        if (eat(QLatin1Char('-'))) { unary(); m_p.steps << Program::Step{Program::Neg}; return; }
        eat(QLatin1Char('+'));
        primary();
    }
    void primary()
    {
        if (m_pos >= m_text.size()) { fail(QStringLiteral("Выражение оборвано")); return; }
        const QChar c = m_text[m_pos];
        if (eat(QLatin1Char('('))) {
            sum();
            if (!eat(QLatin1Char(')'))) fail(QStringLiteral("Не хватает «)»"));
            return;
        }
        if (c.isDigit() || c == QLatin1Char('.')) {
            int end = m_pos;
            while (end < m_text.size() && (m_text[end].isDigit() || m_text[end] == QLatin1Char('.'))) ++end;
            bool ok = false;
            const double v = m_text.mid(m_pos, end - m_pos).toDouble(&ok);
            if (!ok) fail(QStringLiteral("Неверное число"));
            m_pos = end;
            m_p.steps << Program::Step{Program::Number, v};
            return;
        }
        if (c == QLatin1Char('B') && m_pos + 1 < m_text.size() && m_text[m_pos + 1].isDigit()) {
            int end = m_pos + 1;
            while (end < m_text.size() && m_text[end].isDigit()) ++end;
            const int band = m_text.mid(m_pos + 1, end - m_pos - 1).toInt();
            m_pos = end;
            if (band < 1 || band > m_bands || band > 63) fail(QStringLiteral("Канала B%1 нет: в растре каналов %2").arg(band).arg(m_bands));
            m_p.steps << Program::Step{Program::Band, 0, band};
            m_p.bands.insert(band);
            return;
        }
        static const struct { const char *name; Program::Op op; int args; } functions[] = {
            {"SQRT", Program::Sqrt, 1}, {"ABS", Program::Abs, 1}, {"MIN", Program::Min, 2}, {"MAX", Program::Max, 2}};
        for (const auto &f : functions) {
            const QString name = QString::fromLatin1(f.name);
            if (!m_text.mid(m_pos).startsWith(name + QLatin1Char('('))) continue;
            m_pos += int(name.size()) + 1;
            sum();
            if (f.args == 2) {
                if (!eat(QLatin1Char(','))) { fail(QStringLiteral("%1(a, b): нужны два аргумента").arg(name.toLower())); return; }
                sum();
            }
            if (!eat(QLatin1Char(')'))) fail(QStringLiteral("Не хватает «)»"));
            m_p.steps << Program::Step{f.op};
            return;
        }
        fail(QStringLiteral("Непонятно: «%1»").arg(m_text.mid(m_pos, 8)));
    }
    QString m_text;
    int m_pos = 0, m_bands;
    Program m_p;
};

inline double run(const Program &p, const double *bandValues, double *stack)
{
    int top = -1;
    for (const Program::Step &s : p.steps) {
        switch (s.op) {
        case Program::Number: stack[++top] = s.value; break;
        case Program::Band: stack[++top] = bandValues[s.band]; break;
        case Program::Add: stack[top - 1] += stack[top]; --top; break;
        case Program::Sub: stack[top - 1] -= stack[top]; --top; break;
        case Program::Mul: stack[top - 1] *= stack[top]; --top; break;
        case Program::Div: stack[top - 1] = stack[top] == 0 ? qQNaN() : stack[top - 1] / stack[top]; --top; break;
        case Program::Neg: stack[top] = -stack[top]; break;
        case Program::Sqrt: stack[top] = stack[top] < 0 ? qQNaN() : std::sqrt(stack[top]); break;
        case Program::Abs: stack[top] = std::fabs(stack[top]); break;
        case Program::Min: stack[top - 1] = qMin(stack[top - 1], stack[top]); --top; break;
        case Program::Max: stack[top - 1] = qMax(stack[top - 1], stack[top]); --top; break;
        }
    }
    return top == 0 ? stack[0] : qQNaN();
}
} // namespace

QString checkExpression(const QString &expression, int bands)
{
    return Parser(expression, bands).parse().error;
}

QString indexExpression(const QString &index, int red, int green, int blue, int nir)
{
    const QString R = QStringLiteral("B%1").arg(red), G = QStringLiteral("B%1").arg(green), B = QStringLiteral("B%1").arg(blue),
                  N = QStringLiteral("B%1").arg(nir);
    if (index == QLatin1String("exg")) return QStringLiteral("(2*%2-%1-%3)/(%1+%2+%3)").arg(R, G, B);
    if (index == QLatin1String("vari")) return QStringLiteral("(%2-%1)/(%2+%1-%3)").arg(R, G, B);
    if (index == QLatin1String("gli")) return QStringLiteral("(2*%2-%1-%3)/(2*%2+%1+%3)").arg(R, G, B);
    if (index == QLatin1String("ndvi")) return QStringLiteral("(%2-%1)/(%2+%1)").arg(R, N);
    return {};
}

double indexThreshold(const QString &index)
{
    if (index == QLatin1String("ndvi")) return 0.3;
    if (index == QLatin1String("exg") || index == QLatin1String("vari") || index == QLatin1String("gli")) return 0.05;
    return qQNaN();
}

RasterResult calculate(const QString &rasterPath, const QString &expression, const QString &out, double threshold, double *above)
{
    RasterResult r;
    r.path = out;
    GisEnvironment::clearError();
    GDALDatasetH src = GDALOpenEx(nativePath(rasterPath).constData(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
    if (!src) {
        r.error = QStringLiteral("Растр не открывается: %1").arg(GisEnvironment::lastError());
        return r;
    }
    const int bands = GDALGetRasterCount(src), w = GDALGetRasterXSize(src), h = GDALGetRasterYSize(src);
    const Program program = Parser(expression, bands).parse();
    if (!program.error.isEmpty()) {
        GDALClose(src);
        r.error = program.error;
        return r;
    }
    QDir().mkpath(QFileInfo(out).absolutePath());
    const char *create[] = {"COMPRESS=DEFLATE", "TILED=YES", "BIGTIFF=IF_SAFER", "PREDICTOR=3", nullptr};
    GDALDatasetH dst = GDALCreate(GDALGetDriverByName("GTiff"), nativePath(out).constData(), w, h, 1, GDT_Float32, const_cast<char **>(create));
    if (!dst) {
        GDALClose(src);
        r.error = QStringLiteral("Не создать файл: %1").arg(GisEnvironment::lastError());
        return r;
    }
    double gt[6];
    if (GDALGetGeoTransform(src, gt) == CE_None) GDALSetGeoTransform(dst, gt);
    GDALSetProjection(dst, GDALGetProjectionRef(src));
    GDALRasterBandH outBand = GDALGetRasterBand(dst, 1);
    const float NoData = -9999.0f;
    GDALSetRasterNoDataValue(outBand, NoData);
    const QList<int> used = program.bands.values();
    const int rows = 256;
    std::vector<std::vector<float>> data(bands + 1);
    std::vector<std::vector<uchar>> mask(bands + 1);
    std::vector<float> result;
    double sum = 0, mn = 1e300, mx = -1e300;
    qint64 count = 0, over = 0;
    bool ok = true;
    for (int y0 = 0; y0 < h && ok; y0 += rows) {
        const int n = qMin(rows, h - y0);
        for (int b : used) {
            data[b].resize(size_t(w) * n);
            mask[b].resize(size_t(w) * n);
            GDALRasterBandH band = GDALGetRasterBand(src, b);
            ok = ok && GDALRasterIO(band, GF_Read, 0, y0, w, n, data[b].data(), w, n, GDT_Float32, 0, 0) == CE_None;
            // GDAL's mask of the band: nodata, alpha or "all valid".
            ok = ok && GDALRasterIO(GDALGetMaskBand(band), GF_Read, 0, y0, w, n, mask[b].data(), w, n, GDT_Byte, 0, 0) == CE_None;
        }
        result.assign(size_t(w) * n, NoData);
        double values[64] = {0}, stack[64];
        for (size_t i = 0; i < result.size(); ++i) {
            bool valid = true;
            for (int b : used) {
                if (!mask[b][i]) { valid = false; break; }
                values[b] = data[b][i];
            }
            if (!valid) continue;
            const double v = run(program, values, stack);
            if (!std::isfinite(v)) continue;
            result[i] = float(v);
            sum += v;
            mn = qMin(mn, v);
            mx = qMax(mx, v);
            ++count;
            over += std::isfinite(threshold) && v > threshold;
        }
        ok = ok && GDALRasterIO(outBand, GF_Write, 0, y0, w, n, result.data(), w, n, GDT_Float32, 0, 0) == CE_None;
    }
    GDALClose(dst);
    GDALClose(src);
    if (!ok) {
        QFile::remove(out);
        r.error = QStringLiteral("Ошибка чтения или записи растра: %1").arg(GisEnvironment::lastError());
        return r;
    }
    if (count) {
        r.stats.min = mn;
        r.stats.max = mx;
        r.stats.mean = sum / count;
        r.stats.valid = true;
    }
    if (above) *above = count ? double(over) / count : qQNaN();
    return r;
}

} // namespace terrain
