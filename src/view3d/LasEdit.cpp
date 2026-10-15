#include "LasEdit.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QtEndian>
#include <cmath>
#include <functional>
#include <limits>
#include <vector>
#include <gdal.h>

namespace las {

namespace {
struct Header
{
    QByteArray bytes;          // the whole public header
    int minor = 2, format = 0, recordLength = 0;
    quint32 dataOffset = 0;
    quint64 count = 0;
    double scale[3] = {1, 1, 1}, offset[3] = {0, 0, 0};
    quint64 evlrStart = 0;
    quint32 evlrCount = 0;
    QString error;

    quint16 u16(int o) const { return qFromLittleEndian<quint16>(bytes.constData() + o); }
    quint32 u32(int o) const { return qFromLittleEndian<quint32>(bytes.constData() + o); }
    quint64 u64(int o) const { return qFromLittleEndian<quint64>(bytes.constData() + o); }
    double f64(int o) const { return qFromLittleEndian<double>(bytes.constData() + o); }
};

Header readHeader(QFile &f, const QString &path)
{
    Header h;
    h.bytes = f.read(375);
    if (h.bytes.size() < 227 || !h.bytes.startsWith("LASF")) {
        h.error = QStringLiteral("Не LAS-файл: %1").arg(QDir::toNativeSeparators(path));
        return h;
    }
    const int size = h.u16(94);
    if (size < 227 || size > h.bytes.size() + 4096) {
        h.error = QStringLiteral("Повреждён заголовок LAS");
        return h;
    }
    f.seek(0);
    h.bytes = f.read(size);
    h.minor = quint8(h.bytes[25]);
    h.dataOffset = h.u32(96);
    h.format = quint8(h.bytes[104]);
    h.recordLength = h.u16(105);
    h.count = h.u32(107);
    if (h.minor >= 4 && size >= 255) h.count = qMax<quint64>(h.count, h.u64(247));
    for (int k = 0; k < 3; ++k) {
        h.scale[k] = h.f64(131 + 8 * k);
        h.offset[k] = h.f64(155 + 8 * k);
    }
    if (h.minor >= 4 && size >= 247) {
        h.evlrStart = h.u64(235);
        h.evlrCount = h.u32(243);
    }
    if (h.format & 0xC0 || path.endsWith(QLatin1String(".laz"), Qt::CaseInsensitive))
        h.error = QStringLiteral("Сжатый LAZ не редактируется: используйте LAS (профили обработки сохраняют облако и в LAS)");
    else if (h.format > 10 || h.recordLength < 20)
        h.error = QStringLiteral("Неподдерживаемый формат точек LAS %1").arg(h.format);
    return h;
}

// Classes and returns of a point record.
inline int classOf(const char *r, bool extended) { return extended ? quint8(r[16]) : (quint8(r[15]) & 0x1F); }
inline void setClass(char *r, bool extended, int c)
{
    if (extended) r[16] = char(c);
    else r[15] = char((quint8(r[15]) & 0xE0) | (c & 0x1F));
}
inline int returnOf(const char *r, bool extended) { return extended ? (quint8(r[14]) & 0x0F) : (quint8(r[14]) & 0x07); }

Crs crsFromKeys(const QByteArray &keys)
{
    // GeoKeyDirectory: version, revision, minor, count, then (id, location, count, value) per key.
    if (keys.size() < 8) return {};
    const int n = qFromLittleEndian<quint16>(keys.constData() + 6);
    for (int i = 0; i < n && 8 + i * 8 + 8 <= keys.size(); ++i) {
        const char *k = keys.constData() + 8 + i * 8;
        const quint16 id = qFromLittleEndian<quint16>(k), location = qFromLittleEndian<quint16>(k + 2), value = qFromLittleEndian<quint16>(k + 6);
        if (location == 0 && (id == 3072 || id == 2048) && value > 0 && value != 32767) return Crs::fromEpsg(value);
    }
    return {};
}

// Writes `header` (counts, bounds, extended records' offset updated), the points and the extended records.
struct Writer
{
    Header header;
    QSaveFile out;
    QByteArray vlrs;                     // bytes between the header and the points
    qint64 written = 0;
    quint64 byReturn[15] = {};
    double mn[3] = {std::numeric_limits<double>::max(), std::numeric_limits<double>::max(), std::numeric_limits<double>::max()};
    double mx[3] = {-std::numeric_limits<double>::max(), -std::numeric_limits<double>::max(), -std::numeric_limits<double>::max()};
    QMap<int, qint64> classes;

    explicit Writer(const QString &path) : out(path) {}
    bool begin()
    {
        if (!out.open(QIODevice::WriteOnly)) return false;
        return out.write(header.bytes) == header.bytes.size() && out.write(vlrs) == vlrs.size();
    }
    void add(const char *r)
    {
        const bool extended = header.format >= 6;
        for (int k = 0; k < 3; ++k) {
            const double v = qFromLittleEndian<qint32>(r + 4 * k) * header.scale[k] + header.offset[k];
            mn[k] = qMin(mn[k], v);
            mx[k] = qMax(mx[k], v);
        }
        const int ret = returnOf(r, extended);
        if (ret >= 1 && ret <= 15) ++byReturn[ret - 1];
        ++classes[classOf(r, extended)];
        ++written;
    }
    bool finish(const QByteArray &evlrs)
    {
        QByteArray &h = header.bytes;
        auto put32 = [&h](int o, quint32 v) { qToLittleEndian<quint32>(v, h.data() + o); };
        auto put64 = [&h](int o, quint64 v) { qToLittleEndian<quint64>(v, h.data() + o); };
        auto putF = [&h](int o, double v) { qToLittleEndian<double>(v, h.data() + o); };
        const bool legacy = header.format < 6 && written <= qint64(std::numeric_limits<quint32>::max());
        put32(107, legacy ? quint32(written) : 0);
        for (int i = 0; i < 5; ++i) put32(111 + 4 * i, legacy ? quint32(byReturn[i]) : 0);
        if (written == 0) for (int k = 0; k < 3; ++k) mn[k] = mx[k] = 0;
        for (int k = 0; k < 3; ++k) {
            putF(179 + 16 * k, mx[k]);
            putF(187 + 16 * k, mn[k]);
        }
        if (header.minor >= 4 && h.size() >= 375) {
            put64(247, quint64(written));
            for (int i = 0; i < 15; ++i) put64(255 + 8 * i, byReturn[i]);
            put64(235, evlrs.isEmpty() ? 0 : quint64(header.dataOffset) + quint64(written) * quint64(header.recordLength));
        }
        if (!evlrs.isEmpty() && out.write(evlrs) != evlrs.size()) return false;
        if (!out.seek(0) || out.write(h) != h.size()) return false;
        return out.commit();
    }
};

Result run(const QString &in, const QString &out, const std::function<bool(char *, bool *keep)> &perPoint,
           const std::function<bool(const Header &)> &prepare)
{
    Result r;
    r.path = out;
    QFile f(in);
    if (!f.open(QIODevice::ReadOnly)) {
        r.error = QStringLiteral("Не удалось открыть %1").arg(QDir::toNativeSeparators(in));
        return r;
    }
    const Header h = readHeader(f, in);
    if (!h.error.isEmpty()) {
        r.error = h.error;
        return r;
    }
    if (prepare && !prepare(h)) {
        r.error = QStringLiteral("Не удалось подготовить правку");
        return r;
    }
    QDir().mkpath(QFileInfo(out).absolutePath());
    Writer w(out);
    w.header = h;
    f.seek(h.bytes.size());
    w.vlrs = f.read(qint64(h.dataOffset) - h.bytes.size());
    QByteArray evlrs;
    if (h.evlrStart > 0 && h.evlrCount > 0 && h.evlrStart < quint64(f.size())) {
        f.seek(qint64(h.evlrStart));
        evlrs = f.readAll();
    }
    if (!w.begin()) {
        r.error = QStringLiteral("Не удалось записать %1").arg(QDir::toNativeSeparators(out));
        return r;
    }
    f.seek(h.dataOffset);
    const quint64 chunk = 65536;
    QByteArray kept;
    for (quint64 start = 0; start < h.count; start += chunk) {
        const quint64 n = qMin(chunk, h.count - start);
        QByteArray block = f.read(qint64(n) * h.recordLength);
        if (quint64(block.size()) < n * quint64(h.recordLength)) {
            r.error = QStringLiteral("LAS обрывается на точке %1").arg(start + quint64(block.size()) / h.recordLength);
            return r;
        }
        kept.clear();
        for (quint64 k = 0; k < n; ++k) {
            char *rec = block.data() + k * h.recordLength;
            bool keep = true;
            if (perPoint(rec, &keep)) ++r.changed;
            ++r.total;
            if (!keep) continue;
            kept.append(rec, h.recordLength);
            w.add(rec);
        }
        if (w.out.write(kept) != kept.size()) {
            r.error = QStringLiteral("Не удалось записать %1").arg(QDir::toNativeSeparators(out));
            return r;
        }
    }
    if (!w.finish(evlrs)) {
        r.error = QStringLiteral("Не удалось записать %1").arg(QDir::toNativeSeparators(out));
        return r;
    }
    r.written = w.written;
    r.classes = w.classes;
    return r;
}
} // namespace

Crs fileCrs(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const Header h = readHeader(f, path);
    if (h.bytes.size() < 227) return {};
    const quint32 records = h.u32(100);
    qint64 pos = h.bytes.size();
    for (quint32 i = 0; i < records && i < 1000 && pos < h.dataOffset; ++i) {
        f.seek(pos);
        const QByteArray vlr = f.read(54);
        if (vlr.size() < 54) break;
        const quint16 id = qFromLittleEndian<quint16>(vlr.constData() + 18), length = qFromLittleEndian<quint16>(vlr.constData() + 20);
        const QByteArray body = f.read(length);
        if (id == 2112) {
            const Crs crs = Crs::fromWkt(QString::fromUtf8(body).trimmed().remove(QChar(0)));
            if (crs.isValid()) return crs;
        }
        if (id == 34735) {
            const Crs crs = crsFromKeys(body);
            if (crs.isValid()) return crs;
        }
        pos += 54 + length;
    }
    return {};
}

Result edit(const QString &in, const QString &out, Operation op, const QVector<QPolygonF> &ringsIn, const Crs &ringsCrs, int classCode,
            const Crs &fallback)
{
    if (ringsIn.isEmpty() || ringsIn.first().size() < 3) {
        Result r;
        r.error = QStringLiteral("Нужен контур (полигон)");
        return r;
    }
    Crs lasCrs = fileCrs(in);
    if (!lasCrs.isValid()) lasCrs = fallback;
    QVector<QPolygonF> rings = ringsIn;
    if (ringsCrs.isValid() && lasCrs.isValid() && !ringsCrs.isSame(lasCrs)) {
        const CoordinateTransform t(ringsCrs, lasCrs);
        for (QPolygonF &ring : rings) {
            QVector<QPointF> pts(ring.begin(), ring.end());
            if (!t.transform(pts)) {
                Result r;
                r.error = QStringLiteral("Не удалось пересчитать контур в систему координат облака");
                return r;
            }
            ring = QPolygonF(pts);
        }
    }
    const QRectF box = rings.first().boundingRect();
    double scale[3], offset[3];
    bool extended = false;
    auto inside = [&](const char *rec) {
        const QPointF p(qFromLittleEndian<qint32>(rec) * scale[0] + offset[0], qFromLittleEndian<qint32>(rec + 4) * scale[1] + offset[1]);
        if (!box.contains(p) || !rings.first().containsPoint(p, Qt::OddEvenFill)) return false;
        for (int i = 1; i < rings.size(); ++i)
            if (rings[i].containsPoint(p, Qt::OddEvenFill)) return false;
        return true;
    };
    return run(in, out,
               [&](char *rec, bool *keep) {
                   const bool in = inside(rec);
                   if (op == Operation::DeleteInside) { *keep = !in; return in; }
                   if (op == Operation::KeepInside) { *keep = in; return !in; }
                   if (in && classOf(rec, extended) != classCode) {
                       setClass(rec, extended, classCode);
                       return true;
                   }
                   return false;
               },
               [&](const Header &h) {
                   std::copy(h.scale, h.scale + 3, scale);
                   std::copy(h.offset, h.offset + 3, offset);
                   extended = h.format >= 6;
                   return true;
               });
}

Result classify(const QString &in, const QString &out, const QString &dtmPath, const ClassifyOptions &o, const Crs &fallback)
{
    Result r;
    GDALDatasetH ds = GDALOpenEx(QDir::toNativeSeparators(dtmPath).toUtf8().constData(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
    if (!ds) {
        r.error = QStringLiteral("Модель рельефа (DTM) не открывается: %1").arg(QDir::toNativeSeparators(dtmPath));
        return r;
    }
    double gt[6];
    GDALGetGeoTransform(ds, gt);
    const int w = GDALGetRasterXSize(ds), hgt = GDALGetRasterYSize(ds);
    std::vector<float> dtm(size_t(w) * hgt);
    int hasNoData = 0;
    const double noData = GDALGetRasterNoDataValue(GDALGetRasterBand(ds, 1), &hasNoData);
    const bool readOk = GDALRasterIO(GDALGetRasterBand(ds, 1), GF_Read, 0, 0, w, hgt, dtm.data(), w, hgt, GDT_Float32, 0, 0) == CE_None;
    const Crs dtmCrs = Crs::fromWkt(QString::fromUtf8(GDALGetProjectionRef(ds)));
    GDALClose(ds);
    if (!readOk || gt[2] != 0 || gt[4] != 0) {
        r.error = QStringLiteral("Модель рельефа не читается или повёрнута");
        return r;
    }
    Crs lasCrs = fileCrs(in);
    if (!lasCrs.isValid()) lasCrs = fallback;
    const bool transform = lasCrs.isValid() && dtmCrs.isValid() && !lasCrs.isSame(dtmCrs);
    const CoordinateTransform toDtm(lasCrs, dtmCrs);
    auto ground = [&](double x, double y) {
        QPointF p(x, y);
        if (transform && !toDtm.transform(p)) return std::numeric_limits<double>::quiet_NaN();
        const double fx = (p.x() - gt[0]) / gt[1] - 0.5, fy = (p.y() - gt[3]) / gt[5] - 0.5;
        if (fx < -0.5 || fy < -0.5 || fx > w - 0.5 || fy > hgt - 0.5) return std::numeric_limits<double>::quiet_NaN();
        const int x0 = qBound(0, int(std::floor(fx)), qMax(0, w - 2)), y0 = qBound(0, int(std::floor(fy)), qMax(0, hgt - 2));
        const int x1 = qMin(x0 + 1, w - 1), y1 = qMin(y0 + 1, hgt - 1);
        const double ax = qBound(0.0, fx - x0, 1.0), ay = qBound(0.0, fy - y0, 1.0);
        const float v[4] = {dtm[size_t(y0) * w + x0], dtm[size_t(y0) * w + x1], dtm[size_t(y1) * w + x0], dtm[size_t(y1) * w + x1]};
        for (float z : v)
            if (!std::isfinite(z) || (hasNoData && z == float(noData))) return std::numeric_limits<double>::quiet_NaN();
        return (v[0] * (1 - ax) + v[1] * ax) * (1 - ay) + (v[2] * (1 - ax) + v[3] * ax) * ay;
    };
    double scale[3], offset[3];
    bool extended = false;
    int rgb = -1;
    static const int rgbOffset[] = {-1, -1, 20, 28, -1, 28, -1, 30, 30, -1, 30};
    r = run(in, out,
            [&](char *rec, bool *) {
                const double x = qFromLittleEndian<qint32>(rec) * scale[0] + offset[0], y = qFromLittleEndian<qint32>(rec + 4) * scale[1] + offset[1],
                             z = qFromLittleEndian<qint32>(rec + 8) * scale[2] + offset[2];
                const double h = z - ground(x, y);
                int c = 1;
                if (std::isfinite(h)) {
                    bool green = false;
                    if (rgb >= 0) {
                        const double R = qFromLittleEndian<quint16>(rec + rgb), G = qFromLittleEndian<quint16>(rec + rgb + 2),
                                     B = qFromLittleEndian<quint16>(rec + rgb + 4);
                        green = R + G + B > 0 && (2 * G - R - B) / (R + G + B) > o.greenness;
                    }
                    if (h < -o.noiseBelow) c = 7;
                    else if (std::fabs(h) <= o.groundTolerance) c = 2;
                    else if (green && h > 0) c = h < o.lowVegetation ? 3 : h < o.mediumVegetation ? 4 : 5;
                    else if (h >= o.building) c = 6;
                }
                if (classOf(rec, extended) == c) return false;
                setClass(rec, extended, c);
                return true;
            },
            [&](const Header &hd) {
                std::copy(hd.scale, hd.scale + 3, scale);
                std::copy(hd.offset, hd.offset + 3, offset);
                extended = hd.format >= 6;
                rgb = hd.format <= 10 ? rgbOffset[hd.format] : -1;
                return true;
            });
    return r;
}

} // namespace las
