#include "Lidar.h"
#include "Crs.h"
#include "OdmTools.h"
#include "Platform.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cmath>
#include <gdal.h>

namespace lidar {

namespace {
QJsonObject jsonOf(const QString &output)
{
    // PDAL prints the JSON alone; anything before the first brace is a warning.
    const int start = int(output.indexOf('{'));
    return start < 0 ? QJsonObject() : QJsonDocument::fromJson(output.mid(start).toUtf8()).object();
}

// The CRS from the variable length records of a LAS/LAZ header (they are not compressed): an OGC
// WKT record (2112), else the EPSG code of the GeoTIFF keys (34735). PDAL ignores a WKT record
// of LAS 1.0–1.3 without the WKT bit of the global encoding; such files exist.
QString headerWkt(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray h = f.read(375);
    if (h.size() < 227 || !h.startsWith("LASF")) return {};
    auto u16 = [](const QByteArray &b, int o) { return o + 2 <= b.size() ? quint16(uchar(b[o]) | uchar(b[o + 1]) << 8) : quint16(0); };
    auto u32 = [&](const QByteArray &b, int o) { return quint32(u16(b, o)) | quint32(u16(b, o + 2)) << 16; };
    const quint16 headerSize = u16(h, 94);
    const quint32 records = u32(h, 100);
    qint64 at = headerSize;
    int epsg = 0;
    for (quint32 i = 0; i < records && i < 64; ++i) {
        if (!f.seek(at)) break;
        const QByteArray r = f.read(54);
        if (r.size() < 54) break;
        const QByteArray user = r.mid(2, 16);
        const quint16 id = u16(r, 18), length = u16(r, 20);
        const QByteArray body = f.read(length);
        if (user.startsWith("LASF_Projection")) {
            if (id == 2112) return QString::fromUtf8(body.constData());
            if (id == 34735)
                for (int k = 0; k < int(u16(body, 6)); ++k) {
                    const int o = 8 + k * 8;
                    const quint16 key = u16(body, o), value = u16(body, o + 6);
                    if ((key == 3072 || (key == 2048 && !epsg)) && value > 0 && value < 32767) epsg = value;
                }
        }
        at += 54 + length;
    }
    return epsg ? Crs::fromEpsg(epsg).wkt() : QString();
}

bool moveInto(const QString &from, const QString &to, QString *error)
{
    QDir().mkpath(QFileInfo(to).absolutePath());
    QFile::remove(to);
    if (QFile::rename(from, to) || (QFile::copy(from, to) && QFile::remove(from))) return true;
    if (error) *error = QStringLiteral("Не удалось записать %1").arg(QDir::toNativeSeparators(to));
    return false;
}
} // namespace

QString classTitle(int c)
{
    switch (c) {
    case 0: return QStringLiteral("не классифицировано");
    case 1: return QStringLiteral("без класса");
    case 2: return QStringLiteral("земля");
    case 3: return QStringLiteral("низкая растительность");
    case 4: return QStringLiteral("средняя растительность");
    case 5: return QStringLiteral("высокая растительность");
    case 6: return QStringLiteral("здания");
    case 7: return QStringLiteral("шум");
    case 9: return QStringLiteral("вода");
    case 17: return QStringLiteral("мосты");
    case 18: return QStringLiteral("высокий шум");
    default: return QStringLiteral("класс %1").arg(c);
    }
}

Info inspect(const QString &path, const CancelFn &cancelled)
{
    Info info;
    info.path = QFileInfo(path).absoluteFilePath();
    const QFileInfo fi(path);
    if (!fi.isFile()) {
        info.error = QStringLiteral("Файл не найден: %1").arg(QDir::toNativeSeparators(path));
        return info;
    }
    info.bytes = fi.size();
    const odmtools::Result summary = odmtools::pdal({QStringLiteral("info"), QStringLiteral("--summary"), odmtools::toolPath(path)}, 600000, {}, cancelled);
    if (!summary.ok()) {
        info.error = summary.error;
        return info;
    }
    const QJsonObject s = jsonOf(summary.output).value(QStringLiteral("summary")).toObject();
    if (s.isEmpty()) {
        info.error = QStringLiteral("PDAL не прочитал файл как облако точек");
        return info;
    }
    info.points = quint64(s.value(QStringLiteral("num_points")).toDouble());
    const QJsonObject b = s.value(QStringLiteral("bounds")).toObject();
    info.extent = QRectF(QPointF(b.value(QStringLiteral("minx")).toDouble(), b.value(QStringLiteral("miny")).toDouble()),
                         QPointF(b.value(QStringLiteral("maxx")).toDouble(), b.value(QStringLiteral("maxy")).toDouble()));
    info.minZ = b.value(QStringLiteral("minz")).toDouble();
    info.maxZ = b.value(QStringLiteral("maxz")).toDouble();
    const QJsonObject m = s.value(QStringLiteral("metadata")).toObject();
    info.compressed = m.value(QStringLiteral("compressed")).toBool() || path.endsWith(QLatin1String(".laz"), Qt::CaseInsensitive);
    info.format = QStringLiteral("LAS %1.%2, формат %3").arg(m.value(QStringLiteral("major_version")).toInt())
                      .arg(m.value(QStringLiteral("minor_version")).toInt()).arg(m.value(QStringLiteral("dataformat_id")).toInt());
    info.crsWkt = s.value(QStringLiteral("srs")).toObject().value(QStringLiteral("wkt")).toString();
    if (info.crsWkt.isEmpty()) info.crsWkt = m.value(QStringLiteral("comp_spatialreference")).toString();
    if (info.crsWkt.isEmpty()) info.crsWkt = headerWkt(path);
    const QString dims = s.value(QStringLiteral("dimensions")).toString();
    info.hasColor = dims.contains(QLatin1String("Red"));
    // The header's bounds may be wrong or empty: the extent and heights from the points themselves,
    // with the classes present.
    const bool classified = dims.contains(QLatin1String("Classification"));
    QStringList args{QStringLiteral("info"), QStringLiteral("--stats"), QStringLiteral("--dimensions"),
                     classified ? QStringLiteral("X,Y,Z,Classification") : QStringLiteral("X,Y,Z")};
    if (classified) args << QStringLiteral("--enumerate") << QStringLiteral("Classification");
    const odmtools::Result stats = odmtools::pdal(args << odmtools::toolPath(path), 1800000, {}, cancelled);
    if (cancelled && cancelled()) {
        info.error = QStringLiteral("Отменено");
        return info;
    }
    double x0 = info.extent.left(), x1 = info.extent.right(), y0 = info.extent.top(), y1 = info.extent.bottom();
    for (const QJsonValue &v : jsonOf(stats.output).value(QStringLiteral("stats")).toObject().value(QStringLiteral("statistic")).toArray()) {
        const QJsonObject d = v.toObject();
        const QString name = d.value(QStringLiteral("name")).toString();
        const double lo = d.value(QStringLiteral("minimum")).toDouble(), hi = d.value(QStringLiteral("maximum")).toDouble();
        if (name == QLatin1String("X")) { x0 = lo; x1 = hi; }
        else if (name == QLatin1String("Y")) { y0 = lo; y1 = hi; }
        else if (name == QLatin1String("Z")) { info.minZ = lo; info.maxZ = hi; }
        else if (name == QLatin1String("Classification"))
            for (const QJsonValue &c : d.value(QStringLiteral("values")).toArray()) info.classes << c.toInt();
    }
    info.extent = QRectF(QPointF(x0, y0), QPointF(x1, y1));
    return info;
}

QString uncompressed(const QString &path, QString *error, const CancelFn &cancelled)
{
    if (!path.endsWith(QLatin1String(".laz"), Qt::CaseInsensitive)) return path;
    const QFileInfo fi(path);
    const QByteArray key = (QDir::cleanPath(fi.absoluteFilePath()).toLower() + '|' + QString::number(fi.size()) + '|'
                            + QString::number(fi.lastModified().toMSecsSinceEpoch())).toUtf8();
    const QString cached = QDir(Platform::dataDirectory()).filePath(
        QStringLiteral("cache/las/%1.las").arg(QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Md5).toHex())));
    if (QFileInfo(cached).isFile() && QFileInfo(cached).size() > 0) return cached;
    QString e;
    const QString work = odmtools::workFolder(&e);
    if (work.isEmpty()) {
        if (error) *error = e;
        return {};
    }
    const QString out = QDir(work).filePath(QStringLiteral("cloud.las"));
    QStringList args{QStringLiteral("translate"), odmtools::toolPath(path), QDir::toNativeSeparators(out)};
    const QString wkt = headerWkt(path);
    if (!wkt.isEmpty()) args << QStringLiteral("--readers.las.default_srs=") + wkt;
    const odmtools::Result r = odmtools::pdal(args, 0, {}, cancelled);
    QString result;
    if (!r.ok()) e = r.error;
    else if (moveInto(out, cached, &e)) result = cached;
    QDir(work).removeRecursively();
    if (error) *error = result.isEmpty() ? (e.isEmpty() ? QStringLiteral("LAZ не распакован") : e) : QString();
    return result;
}

double suggestedResolution(const Info &cloud)
{
    const double area = cloud.extent.width() * cloud.extent.height();
    if (!(area > 0) || cloud.points == 0) return 1;
    const double r = std::sqrt(2.0 * area / double(cloud.points));
    for (double step : {0.05, 0.1, 0.2, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0})
        if (r <= step) return step;
    return 10;
}

Surface buildSurface(const Info &cloud, const QString &kind, double resolution, const QString &output, const LineFn &onLine,
                     const CancelFn &cancelled)
{
    Surface s;
    const bool dtm = kind == QLatin1String("dtm");
    if (!dtm && kind != QLatin1String("dsm")) {
        s.error = QStringLiteral("Неизвестная модель «%1»").arg(kind);
        return s;
    }
    if (!(resolution > 0)) resolution = suggestedResolution(cloud);
    s.resolution = resolution;
    QString e;
    const QString work = odmtools::workFolder(&e);
    if (work.isEmpty()) {
        s.error = e;
        return s;
    }
    const QString tif = QDir(work).filePath(QStringLiteral("surface.tif"));
    QJsonArray stages;
    QJsonObject reader{{QStringLiteral("type"), QStringLiteral("readers.las")}, {QStringLiteral("filename"), odmtools::toolPath(cloud.path)}};
    if (!cloud.crsWkt.isEmpty()) reader.insert(QStringLiteral("default_srs"), cloud.crsWkt);
    stages << reader;
    if (dtm) {
        s.groundFromFile = cloud.classes.contains(2);
        if (!s.groundFromFile) {
            // Ground found by the simple morphological filter, noise left out.
            stages << QJsonObject{{QStringLiteral("type"), QStringLiteral("filters.assign")},
                                  {QStringLiteral("value"), QStringLiteral("Classification = 0")}};
            stages << QJsonObject{{QStringLiteral("type"), QStringLiteral("filters.outlier")},
                                  {QStringLiteral("method"), QStringLiteral("statistical")},
                                  {QStringLiteral("mean_k"), 8}, {QStringLiteral("multiplier"), 3.0}};
            stages << QJsonObject{{QStringLiteral("type"), QStringLiteral("filters.smrf")},
                                  {QStringLiteral("ignore"), QStringLiteral("Classification[7:7]")}};
        }
        stages << QJsonObject{{QStringLiteral("type"), QStringLiteral("filters.range")},
                              {QStringLiteral("limits"), QStringLiteral("Classification[2:2]")}};
    } else {
        // Everything but noise (7, 18).
        stages << QJsonObject{{QStringLiteral("type"), QStringLiteral("filters.range")},
                              {QStringLiteral("limits"), QStringLiteral("Classification[0:6],Classification[8:17],Classification[19:255]")}};
    }
    stages << QJsonObject{{QStringLiteral("type"), QStringLiteral("writers.gdal")},
                          {QStringLiteral("filename"), QDir::toNativeSeparators(tif)},
                          {QStringLiteral("resolution"), resolution},
                          {QStringLiteral("output_type"), dtm ? QStringLiteral("idw") : QStringLiteral("max")},
                          {QStringLiteral("window_size"), dtm ? 8 : 2},
                          {QStringLiteral("data_type"), QStringLiteral("float32")},
                          {QStringLiteral("nodata"), -9999},
                          {QStringLiteral("gdaldriver"), QStringLiteral("GTiff")},
                          {QStringLiteral("gdalopts"), QStringLiteral("COMPRESS=DEFLATE,TILED=YES,PREDICTOR=3,BIGTIFF=IF_SAFER")}};
    const QString pipeline = QDir(work).filePath(QStringLiteral("pipeline.json"));
    {
        QFile f(pipeline);
        if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(QJsonObject{{QStringLiteral("pipeline"), stages}}).toJson()) < 0) {
            QDir(work).removeRecursively();
            s.error = QStringLiteral("Не удалось записать задание PDAL");
            return s;
        }
    }
    const odmtools::Result r = odmtools::pdal({QStringLiteral("pipeline"), QDir::toNativeSeparators(pipeline)}, 0, onLine, cancelled);
    if (!r.ok()) {
        QDir(work).removeRecursively();
        s.error = r.error;
        return s;
    }
    if (GDALDatasetH ds = GDALOpen(QDir::toNativeSeparators(tif).toUtf8().constData(), GA_ReadOnly)) {
        s.width = GDALGetRasterXSize(ds);
        s.height = GDALGetRasterYSize(ds);
        GDALClose(ds);
    } else {
        QDir(work).removeRecursively();
        s.error = dtm ? QStringLiteral("Точек земли не найдено — DTM не построена") : QStringLiteral("PDAL не создал растр");
        return s;
    }
    if (!moveInto(tif, output, &s.error)) {
        QDir(work).removeRecursively();
        return s;
    }
    QDir(work).removeRecursively();
    s.path = output;
    return s;
}

} // namespace lidar
