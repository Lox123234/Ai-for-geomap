#include "Scene3D.h"

#include "RasterSource.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <gdal.h>
#include <ogr_srs_api.h>
#include <QPainter>
#include <unordered_map>

namespace view3d {

void Bounds::add(const QVector3D &p)
{
    if (!valid) {
        min = max = p;
        valid = true;
        return;
    }
    min = QVector3D(qMin(min.x(), p.x()), qMin(min.y(), p.y()), qMin(min.z(), p.z()));
    max = QVector3D(qMax(max.x(), p.x()), qMax(max.y(), p.y()), qMax(max.z(), p.z()));
}

void Bounds::add(const Bounds &b)
{
    if (!b.valid) return;
    add(b.min);
    add(b.max);
}

bool readOdmOffset(const QString &coordsFile, Origin *origin, QString *crsLine)
{
    if (coordsFile.isEmpty()) return false;
    QFile f(coordsFile);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return false;
    const QString crs = QString::fromUtf8(f.readLine()).trimmed();
    const QStringList parts = QString::fromUtf8(f.readLine()).simplified().split(QLatin1Char(' '));
    if (parts.size() < 2) return false;
    bool okX = false, okY = false;
    const double x = parts[0].toDouble(&okX), y = parts[1].toDouble(&okY);
    if (!okX || !okY) return false;
    if (origin) *origin = Origin{x, y, 0.0, true};
    if (crsLine) *crsLine = crs;
    return true;
}

QString odmCoordsFor(const QString &file)
{
    // <job>/<stage folder>/<file> → <job>/odm_georeferencing/coords.txt
    QDir dir = QFileInfo(file).absoluteDir();
    for (int i = 0; i < 3; ++i) {
        const QString candidate = dir.filePath(QStringLiteral("odm_georeferencing/coords.txt"));
        if (QFileInfo::exists(candidate)) return candidate;
        if (!dir.cdUp()) break;
    }
    return {};
}

namespace {

quint32 argb(int r, int g, int b, int a = 255)
{
    return (quint32(qBound(0, a, 255)) << 24) | (quint32(qBound(0, r, 255)) << 16) | (quint32(qBound(0, g, 255)) << 8)
         | quint32(qBound(0, b, 255));
}

// ------------------------------------------------------------------ PLY

enum class PlyType { Int8, UInt8, Int16, UInt16, Int32, UInt32, Float32, Float64, Invalid };

PlyType plyType(const QByteArray &name)
{
    if (name == "char" || name == "int8") return PlyType::Int8;
    if (name == "uchar" || name == "uint8") return PlyType::UInt8;
    if (name == "short" || name == "int16") return PlyType::Int16;
    if (name == "ushort" || name == "uint16") return PlyType::UInt16;
    if (name == "int" || name == "int32") return PlyType::Int32;
    if (name == "uint" || name == "uint32") return PlyType::UInt32;
    if (name == "float" || name == "float32") return PlyType::Float32;
    if (name == "double" || name == "float64") return PlyType::Float64;
    return PlyType::Invalid;
}

int plySize(PlyType t)
{
    switch (t) {
    case PlyType::Int8: case PlyType::UInt8: return 1;
    case PlyType::Int16: case PlyType::UInt16: return 2;
    case PlyType::Int32: case PlyType::UInt32: case PlyType::Float32: return 4;
    case PlyType::Float64: return 8;
    default: return 0;
    }
}

struct PlyProperty
{
    QByteArray name;
    PlyType type = PlyType::Invalid;
    bool list = false;
    PlyType countType = PlyType::Invalid;
};

struct PlyElement
{
    QByteArray name;
    qint64 count = 0;
    QVector<PlyProperty> properties;
    int index(const QByteArray &name) const
    {
        for (int i = 0; i < properties.size(); ++i) if (properties[i].name == name) return i;
        return -1;
    }
    bool fixedSize() const
    {
        for (const PlyProperty &p : properties) if (p.list) return false;
        return true;
    }
    int stride() const
    {
        int s = 0;
        for (const PlyProperty &p : properties) s += plySize(p.type);
        return s;
    }
};

enum class PlyFormat { Ascii, LittleEndian, BigEndian };

struct PlyHeader
{
    PlyFormat format = PlyFormat::Ascii;
    QVector<PlyElement> elements;
    qint64 dataOffset = 0;
    QString error;
};

PlyHeader readPlyHeader(QFile &f)
{
    PlyHeader h;
    if (f.readLine().trimmed() != "ply") {
        h.error = QStringLiteral("Не PLY-файл");
        return h;
    }
    while (!f.atEnd()) {
        const QByteArray line = f.readLine().trimmed();
        const QList<QByteArray> t = line.split(' ');
        if (line == "end_header") {
            h.dataOffset = f.pos();
            return h;
        }
        if (t.isEmpty() || t[0] == "comment" || t[0] == "obj_info") continue;
        if (t[0] == "format" && t.size() >= 2) {
            h.format = t[1] == "ascii" ? PlyFormat::Ascii : t[1] == "binary_big_endian" ? PlyFormat::BigEndian : PlyFormat::LittleEndian;
        } else if (t[0] == "element" && t.size() >= 3) {
            PlyElement e;
            e.name = t[1];
            e.count = t[2].toLongLong();
            h.elements << e;
        } else if (t[0] == "property" && !h.elements.isEmpty()) {
            PlyProperty p;
            if (t.size() >= 5 && t[1] == "list") {
                p.list = true;
                p.countType = plyType(t[2]);
                p.type = plyType(t[3]);
                p.name = t[4];
            } else if (t.size() >= 3) {
                p.type = plyType(t[1]);
                p.name = t[2];
            }
            if (p.type == PlyType::Invalid) {
                h.error = QStringLiteral("Неизвестный тип свойства PLY: %1").arg(QString::fromLatin1(line));
                return h;
            }
            h.elements.last().properties << p;
        }
    }
    h.error = QStringLiteral("В PLY нет конца заголовка");
    return h;
}

double readValue(const char *p, PlyType t, bool bigEndian)
{
    switch (t) {
    case PlyType::Int8: return qint8(*p);
    case PlyType::UInt8: return quint8(*p);
    case PlyType::Int16: return bigEndian ? qFromBigEndian<qint16>(p) : qFromLittleEndian<qint16>(p);
    case PlyType::UInt16: return bigEndian ? qFromBigEndian<quint16>(p) : qFromLittleEndian<quint16>(p);
    case PlyType::Int32: return bigEndian ? qFromBigEndian<qint32>(p) : qFromLittleEndian<qint32>(p);
    case PlyType::UInt32: return bigEndian ? qFromBigEndian<quint32>(p) : qFromLittleEndian<quint32>(p);
    case PlyType::Float32: return bigEndian ? qFromBigEndian<float>(p) : qFromLittleEndian<float>(p);
    case PlyType::Float64: return bigEndian ? qFromBigEndian<double>(p) : qFromLittleEndian<double>(p);
    default: return 0;
    }
}

int colorByte(double v, PlyType t)
{
    if (t == PlyType::Float32 || t == PlyType::Float64) return int(std::lround(v * 255.0));
    if (t == PlyType::UInt16 || t == PlyType::Int16) return int(v / 257.0);
    return int(v);
}

// Vertices (positions, colours) and faces of a PLY file. Keeps every `step`-th vertex when
// `step` > 1 (point clouds); meshes keep all vertices.
struct PlyData
{
    QVector<QVector3D> positions;
    QVector<quint32> colors;
    QVector<quint32> triangles;
    qint64 vertexCount = 0;
    QString error;
};

// shift: positions relative to `origin`; an invalid origin becomes the first vertex (rounded).
PlyData readPly(const QString &path, qint64 maxVertices, bool withFaces, Origin &origin, bool shift)
{
    PlyData d;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        d.error = QStringLiteral("Не удалось открыть %1").arg(QDir::toNativeSeparators(path));
        return d;
    }
    const PlyHeader h = readPlyHeader(f);
    if (!h.error.isEmpty()) {
        d.error = h.error;
        return d;
    }
    const bool big = h.format == PlyFormat::BigEndian;
    for (const PlyElement &e : h.elements) {
        if (e.name == "vertex") {
            const int ix = e.index("x"), iy = e.index("y"), iz = e.index("z");
            if (ix < 0 || iy < 0 || iz < 0) {
                d.error = QStringLiteral("У вершин PLY нет координат x, y, z");
                return d;
            }
            int ir = e.index("red"), ig = e.index("green"), ib = e.index("blue");
            if (ir < 0) { ir = e.index("diffuse_red"); ig = e.index("diffuse_green"); ib = e.index("diffuse_blue"); }
            const bool color = ir >= 0 && ig >= 0 && ib >= 0;
            d.vertexCount = e.count;
            const qint64 step = maxVertices > 0 && e.count > maxVertices ? (e.count + maxVertices - 1) / maxVertices : 1;
            d.positions.reserve(int(qMin<qint64>(e.count / step + 1, 200000000)));
            if (color) d.colors.reserve(d.positions.capacity());
            const auto place = [&](double x, double y, double z) {
                if (shift && !origin.valid) origin = Origin{std::floor(x), std::floor(y), 0.0, true};
                if (!shift) return QVector3D(float(x), float(y), float(z));
                return QVector3D(float(x - origin.x), float(y - origin.y), float(z - origin.z));
            };
            if (h.format == PlyFormat::Ascii) {
                for (qint64 i = 0; i < e.count && !f.atEnd(); ++i) {
                    const QList<QByteArray> v = f.readLine().simplified().split(' ');
                    if (v.size() < e.properties.size()) {
                        d.error = QStringLiteral("Повреждённая вершина PLY %1").arg(i);
                        return d;
                    }
                    if (i % step) continue;
                    d.positions << place(v[ix].toDouble(), v[iy].toDouble(), v[iz].toDouble());
                    if (color)
                        d.colors << argb(colorByte(v[ir].toDouble(), e.properties[ir].type), colorByte(v[ig].toDouble(), e.properties[ig].type),
                                         colorByte(v[ib].toDouble(), e.properties[ib].type));
                }
            } else {
                if (!e.fixedSize()) {
                    d.error = QStringLiteral("Списки в вершинах PLY не поддерживаются");
                    return d;
                }
                QVector<int> offset(e.properties.size());
                int o = 0;
                for (int i = 0; i < e.properties.size(); ++i) { offset[i] = o; o += plySize(e.properties[i].type); }
                const int stride = o;
                const qint64 chunk = 65536;
                QByteArray buffer;
                for (qint64 start = 0; start < e.count; start += chunk) {
                    const qint64 n = qMin(chunk, e.count - start);
                    buffer = f.read(n * stride);
                    if (buffer.size() < n * stride) {
                        d.error = QStringLiteral("PLY обрывается на вершине %1").arg(start + buffer.size() / stride);
                        return d;
                    }
                    for (qint64 k = 0; k < n; ++k) {
                        if ((start + k) % step) continue;
                        const char *r = buffer.constData() + k * stride;
                        const auto val = [&](int idx) { return readValue(r + offset[idx], e.properties[idx].type, big); };
                        d.positions << place(val(ix), val(iy), val(iz));
                        if (color)
                            d.colors << argb(colorByte(val(ir), e.properties[ir].type), colorByte(val(ig), e.properties[ig].type),
                                             colorByte(val(ib), e.properties[ib].type));
                    }
                }
            }
        } else if (e.name == "face" && withFaces) {
            const int il = e.index("vertex_indices") >= 0 ? e.index("vertex_indices") : e.index("vertex_index");
            for (qint64 i = 0; i < e.count; ++i) {
                QVector<quint32> poly;
                for (int p = 0; p < e.properties.size(); ++p) {
                    const PlyProperty &prop = e.properties[p];
                    if (h.format == PlyFormat::Ascii) break;
                    if (!prop.list) {
                        f.read(plySize(prop.type));
                        continue;
                    }
                    const QByteArray c = f.read(plySize(prop.countType));
                    const int count = int(readValue(c.constData(), prop.countType, big));
                    const QByteArray items = f.read(qint64(count) * plySize(prop.type));
                    if (items.size() < count * plySize(prop.type)) {
                        d.error = QStringLiteral("PLY обрывается на грани %1").arg(i);
                        return d;
                    }
                    if (p != il) continue;
                    for (int k = 0; k < count; ++k) poly << quint32(readValue(items.constData() + k * plySize(prop.type), prop.type, big));
                }
                if (h.format == PlyFormat::Ascii) {
                    const QList<QByteArray> v = f.readLine().simplified().split(' ');
                    const int count = v.isEmpty() ? 0 : v[0].toInt();
                    for (int k = 1; k <= count && k < v.size(); ++k) poly << v[k].toUInt();
                }
                for (int k = 2; k < poly.size(); ++k) d.triangles << poly[0] << poly[k - 1] << poly[k];
            }
        } else {
            // Other elements: skip (fixed-size binary ones in one read, otherwise item by item).
            if (h.format == PlyFormat::Ascii) {
                for (qint64 i = 0; i < e.count; ++i) f.readLine();
            } else if (e.fixedSize()) {
                f.seek(f.pos() + e.count * e.stride());
            } else {
                for (qint64 i = 0; i < e.count; ++i)
                    for (const PlyProperty &prop : e.properties) {
                        if (!prop.list) { f.read(plySize(prop.type)); continue; }
                        const QByteArray c = f.read(plySize(prop.countType));
                        f.read(qint64(readValue(c.constData(), prop.countType, big)) * plySize(prop.type));
                    }
            }
            if (!withFaces && e.name == "face") break;
        }
    }
    return d;
}

} // namespace

PointCloud readPointCloudPly(const QString &path, int maxPoints, const Origin &origin)
{
    PointCloud cloud;
    cloud.source = path;
    Origin used = origin;
    QString crs;
    const bool odm = readOdmOffset(odmCoordsFor(path), &used, &crs);   // ODM: local coordinates + offset
    if (origin.valid) used = origin;
    cloud.georeferenced = odm;
    cloud.crs = crs;
    // ODM's PLY files are already local to coords.txt; other files are shifted by the origin
    // (given, or the first point's).
    const bool odmLocal = !origin.valid && odm;
    PlyData d = readPly(path, maxPoints, false, used, !odmLocal);
    if (!d.error.isEmpty()) {
        cloud.error = d.error;
        return cloud;
    }
    cloud.positions = std::move(d.positions);
    cloud.colors = std::move(d.colors);
    cloud.totalPoints = quint64(d.vertexCount);
    cloud.origin = used;
    for (const QVector3D &p : cloud.positions) cloud.bounds.add(p);
    if (cloud.positions.isEmpty()) cloud.error = QStringLiteral("В файле нет точек");
    return cloud;
}

PointCloud readPointCloudLas(const QString &path, int maxPoints, const Origin &origin)
{
    PointCloud cloud;
    cloud.source = path;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        cloud.error = QStringLiteral("Не удалось открыть %1").arg(QDir::toNativeSeparators(path));
        return cloud;
    }
    const QByteArray h = f.read(375);
    if (h.size() < 227 || !h.startsWith("LASF")) {
        cloud.error = QStringLiteral("Не LAS-файл");
        return cloud;
    }
    const auto u16 = [&h](int o) { return qFromLittleEndian<quint16>(h.constData() + o); };
    const auto u32 = [&h](int o) { return qFromLittleEndian<quint32>(h.constData() + o); };
    const auto f64 = [&h](int o) { return qFromLittleEndian<double>(h.constData() + o); };
    const int minor = quint8(h[25]);
    const quint32 dataOffset = u32(96);
    const int format = quint8(h[104]);
    const int recordLength = u16(105);
    if (format & 0xC0 || path.endsWith(QLatin1String(".laz"), Qt::CaseInsensitive)) {
        cloud.error = QStringLiteral("Сжатый LAZ пока не читается: откройте облако ODM в PLY (odm_filterpoints) или LAS.");
        return cloud;
    }
    quint64 count = u32(107);
    if (minor >= 4 && h.size() >= 255) count = qMax<quint64>(count, qFromLittleEndian<quint64>(h.constData() + 247));
    const double sx = f64(131), sy = f64(139), sz = f64(147), ox = f64(155), oy = f64(163), oz = f64(171);
    static const int rgbOffset[] = {-1, -1, 20, 28, -1, 28, -1, 30, 30, -1, 30};
    const int rgb = format <= 10 ? rgbOffset[format] : -1;
    if (recordLength < 12 || (rgb >= 0 && recordLength < rgb + 6)) {
        cloud.error = QStringLiteral("Неподдерживаемый формат точек LAS %1").arg(format);
        return cloud;
    }
    Origin used = origin;
    if (!used.valid) used = Origin{std::floor(f64(187)), std::floor(f64(203)), 0.0, true};   // min X, min Y
    cloud.origin = used;
    // A CRS record (GeoTIFF keys 34735 or OGC WKT 2112) among the variable-length records.
    {
        const quint32 records = u32(100);
        qint64 pos = u16(94);
        for (quint32 i = 0; i < records && i < 1000; ++i) {
            f.seek(pos);
            const QByteArray vlr = f.read(54);
            if (vlr.size() < 54) break;
            const quint16 id = qFromLittleEndian<quint16>(vlr.constData() + 18);
            const quint16 length = qFromLittleEndian<quint16>(vlr.constData() + 20);
            if (id == 34735 || id == 2112) {
                cloud.georeferenced = true;
                if (id == 2112) cloud.crs = QString::fromUtf8(f.read(qMin<int>(length, 4096))).section(QLatin1Char('"'), 1, 1);
            }
            pos += 54 + length;
        }
    }
    cloud.totalPoints = count;
    const quint64 step = maxPoints > 0 && count > quint64(maxPoints) ? (count + maxPoints - 1) / maxPoints : 1;
    // Classification: bits 0–4 of byte 15 in formats 0–5, byte 16 in formats 6–10. Intensity: bytes 12–13.
    const bool extended = format >= 6;
    QVector<quint16> rawIntensity;
    bool anyClass = false;
    f.seek(dataOffset);
    const quint64 chunk = 65536;
    for (quint64 start = 0; start < count; start += chunk) {
        const quint64 n = qMin(chunk, count - start);
        const QByteArray block = f.read(qint64(n) * recordLength);
        if (quint64(block.size()) < n * quint64(recordLength)) {
            cloud.error = QStringLiteral("LAS обрывается на точке %1").arg(start + quint64(block.size()) / recordLength);
            break;
        }
        for (quint64 k = 0; k < n; ++k) {
            if ((start + k) % step) continue;
            const char *r = block.constData() + k * recordLength;
            const double x = qFromLittleEndian<qint32>(r) * sx + ox, y = qFromLittleEndian<qint32>(r + 4) * sy + oy,
                         z = qFromLittleEndian<qint32>(r + 8) * sz + oz;
            const QVector3D p(float(x - used.x), float(y - used.y), float(z - used.z));
            cloud.positions << p;
            cloud.bounds.add(p);
            const quint8 cls = extended ? quint8(r[16]) : quint8(quint8(r[15]) & 0x1F);
            cloud.classes << cls;
            anyClass = anyClass || cls > 1;
            rawIntensity << qFromLittleEndian<quint16>(r + 12);
            if (rgb >= 0) {
                const int cr = qFromLittleEndian<quint16>(r + rgb), cg = qFromLittleEndian<quint16>(r + rgb + 2),
                          cb = qFromLittleEndian<quint16>(r + rgb + 4);
                const bool wide = cr > 255 || cg > 255 || cb > 255;   // 16-bit colours, or 8-bit stored as is
                cloud.colors << (wide ? argb(cr >> 8, cg >> 8, cb >> 8) : argb(cr, cg, cb));
            }
        }
    }
    if (!anyClass) cloud.classes.clear();   // 0 / 1 everywhere: never classified
    // Intensity to 0..255 between the 1st and 99th percentile (a few bright returns would darken all).
    if (!rawIntensity.isEmpty()) {
        QVector<quint16> sorted = rawIntensity;
        std::sort(sorted.begin(), sorted.end());
        const double lo = sorted[int(sorted.size() * 0.01)], hi = sorted[qMin(int(sorted.size()) - 1, int(sorted.size() * 0.99))];
        if (hi > lo) {
            cloud.intensity.reserve(rawIntensity.size());
            for (quint16 v : rawIntensity) cloud.intensity << quint8(qBound(0.0, (v - lo) / (hi - lo) * 255.0, 255.0));
        }
    }
    if (cloud.positions.isEmpty() && cloud.error.isEmpty()) cloud.error = QStringLiteral("В файле нет точек");
    return cloud;
}

quint32 classColor(int c)
{
    switch (c) {
    case 0: return 0xffa0a0a0;   // never classified
    case 1: return 0xffbebebe;   // unclassified
    case 2: return 0xffa6763c;   // ground
    case 3: return 0xffa6d66e;   // low vegetation
    case 4: return 0xff50af46;   // medium vegetation
    case 5: return 0xff227832;   // high vegetation
    case 6: return 0xffe15a46;   // building
    case 7: case 18: return 0xffff00ff;   // noise
    case 9: return 0xff3c82e6;   // water
    case 17: return 0xffe6c85a;  // bridge deck
    default: return 0xffc8c8c8;
    }
}

QString className(int c)
{
    switch (c) {
    case 0: return QStringLiteral("не классифицировано");
    case 1: return QStringLiteral("без класса");
    case 2: return QStringLiteral("земля");
    case 3: return QStringLiteral("низкая растительность");
    case 4: return QStringLiteral("средняя растительность");
    case 5: return QStringLiteral("высокая растительность");
    case 6: return QStringLiteral("здания");
    case 7: return QStringLiteral("шум (низкие точки)");
    case 9: return QStringLiteral("вода");
    case 17: return QStringLiteral("мосты");
    case 18: return QStringLiteral("шум (высокие точки)");
    default: return QStringLiteral("класс %1").arg(c);
    }
}

PointCloud readPointCloud(const QString &path, int maxPoints, const Origin &origin)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("las") || suffix == QLatin1String("laz")) return readPointCloudLas(path, maxPoints, origin);
    if (suffix == QLatin1String("ply")) return readPointCloudPly(path, maxPoints, origin);
    PointCloud cloud;
    cloud.source = path;
    cloud.error = QStringLiteral("Формат облака не поддерживается: %1 (поддерживаются PLY и LAS)").arg(suffix);
    return cloud;
}

// ------------------------------------------------------------------ meshes

void computeNormals(Mesh &mesh)
{
    mesh.normals = QVector<QVector3D>(mesh.positions.size(), QVector3D());
    for (const Mesh::Part &part : mesh.parts)
        for (int i = 0; i + 2 < part.indices.size(); i += 3) {
            const quint32 a = part.indices[i], b = part.indices[i + 1], c = part.indices[i + 2];
            if (a >= quint32(mesh.positions.size()) || b >= quint32(mesh.positions.size()) || c >= quint32(mesh.positions.size())) continue;
            const QVector3D n = QVector3D::crossProduct(mesh.positions[b] - mesh.positions[a], mesh.positions[c] - mesh.positions[a]);
            mesh.normals[a] += n;
            mesh.normals[b] += n;
            mesh.normals[c] += n;
        }
    for (QVector3D &n : mesh.normals) n = n.isNull() ? QVector3D(0, 0, 1) : n.normalized();
}

namespace {
struct ObjKey
{
    int v, t, n;
    bool operator==(const ObjKey &o) const { return v == o.v && t == o.t && n == o.n; }
};
struct ObjKeyHash
{
    size_t operator()(const ObjKey &k) const
    {
        return size_t(k.v) * 73856093u ^ size_t(k.t) * 19349663u ^ size_t(k.n) * 83492791u;
    }
};

// "1.25" etc. without allocations; advances p.
double number(const char *&p)
{
    char *end = nullptr;
    const double v = std::strtod(p, &end);
    p = end;
    return v;
}
} // namespace

Mesh readObj(const QString &path, int maxTexture)
{
    Mesh mesh;
    mesh.source = path;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        mesh.error = QStringLiteral("Не удалось открыть %1").arg(QDir::toNativeSeparators(path));
        return mesh;
    }
    const QDir dir = QFileInfo(path).absoluteDir();
    QVector<QVector3D> v, vn;
    QVector<QVector2D> vt;
    QVector<quint32> vColors;
    QHash<QString, QString> textures;   // material -> texture file
    std::unordered_map<ObjKey, quint32, ObjKeyHash> unique;
    int part = -1;
    auto partFor = [&mesh](const QString &material) {
        for (int i = 0; i < mesh.parts.size(); ++i) if (mesh.parts[i].material == material) return i;
        mesh.parts.append(Mesh::Part{material, {}, {}});
        return int(mesh.parts.size() - 1);
    };
    QByteArray line;
    while (!f.atEnd()) {
        line = f.readLine();
        const char *p = line.constData();
        while (*p == ' ' || *p == '\t') ++p;
        if (p[0] == 'v' && p[1] == ' ') {
            p += 2;
            const double x = number(p), y = number(p), z = number(p);
            v << QVector3D(float(x), float(y), float(z));
            const char *q = p;
            while (*q == ' ') ++q;
            if (*q && *q != '\n' && *q != '\r') {   // "v x y z r g b"
                const double r = number(p), g = number(p), b = number(p);
                vColors.resize(v.size() - 1, argb(200, 200, 200));
                vColors << argb(int(r * 255), int(g * 255), int(b * 255));
            }
        } else if (p[0] == 'v' && p[1] == 't' && p[2] == ' ') {
            p += 3;
            const double s = number(p), t = number(p);
            vt << QVector2D(float(s), float(1.0 - t));   // OBJ: v up from the bottom; images: rows from the top
        } else if (p[0] == 'v' && p[1] == 'n' && p[2] == ' ') {
            p += 3;
            const double x = number(p), y = number(p), z = number(p);
            vn << QVector3D(float(x), float(y), float(z));
        } else if (p[0] == 'f' && p[1] == ' ') {
            if (part < 0) part = partFor(QString());
            p += 2;
            QVector<quint32> poly;
            while (*p && *p != '\n' && *p != '\r') {
                while (*p == ' ' || *p == '\t') ++p;
                if (!*p || *p == '\n' || *p == '\r') break;
                char *end = nullptr;
                long iv = std::strtol(p, &end, 10), it = 0, in = 0;
                p = end;
                if (*p == '/') {
                    ++p;
                    if (*p != '/') { it = std::strtol(p, &end, 10); p = end; }
                    if (*p == '/') { ++p; in = std::strtol(p, &end, 10); p = end; }
                }
                // 1-based; negative indices count from the end.
                const int vi = iv < 0 ? int(v.size() + iv) : int(iv - 1);
                const int ti = it < 0 ? int(vt.size() + it) : int(it - 1);
                const int ni = in < 0 ? int(vn.size() + in) : int(in - 1);
                if (vi < 0 || vi >= v.size()) {
                    mesh.error = QStringLiteral("Грань ссылается на несуществующую вершину (%1)").arg(iv);
                    return mesh;
                }
                const ObjKey key{vi, it ? ti : -1, in ? ni : -1};
                auto found = unique.find(key);
                quint32 index;
                if (found == unique.end()) {
                    index = quint32(mesh.positions.size());
                    unique.emplace(key, index);
                    mesh.positions << v[vi];
                    if (!vt.isEmpty()) mesh.uvs << (key.t >= 0 && key.t < vt.size() ? vt[key.t] : QVector2D());
                    if (!vn.isEmpty()) mesh.normals << (key.n >= 0 && key.n < vn.size() ? vn[key.n] : QVector3D(0, 0, 1));
                    if (!vColors.isEmpty()) mesh.colors << (vi < vColors.size() ? vColors[vi] : argb(200, 200, 200));
                } else {
                    index = found->second;
                }
                poly << index;
                while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') ++p;
            }
            for (int k = 2; k < poly.size(); ++k) {
                mesh.parts[part].indices << poly[0] << poly[k - 1] << poly[k];
                ++mesh.triangles;
            }
        } else if (line.startsWith("usemtl ")) {
            part = partFor(QString::fromUtf8(line.mid(7).trimmed()));
        } else if (line.startsWith("mtllib ")) {
            QFile mtl(dir.filePath(QString::fromUtf8(line.mid(7).trimmed())));
            if (!mtl.open(QIODevice::ReadOnly)) continue;
            QString current;
            while (!mtl.atEnd()) {
                const QByteArray m = mtl.readLine().trimmed();
                if (m.startsWith("newmtl ")) current = QString::fromUtf8(m.mid(7).trimmed());
                else if (m.startsWith("map_Kd ")) textures.insert(current, QString::fromUtf8(m.mid(7).trimmed()));
            }
        }
    }
    if (mesh.normals.size() != mesh.positions.size()) mesh.normals.clear();
    if (mesh.uvs.size() != mesh.positions.size()) mesh.uvs.clear();
    if (mesh.colors.size() != mesh.positions.size()) mesh.colors.clear();
    for (Mesh::Part &pt : mesh.parts) {
        const QString file = textures.value(pt.material);
        if (file.isEmpty()) continue;
        QImage image(dir.filePath(file));
        if (image.isNull()) {
            mesh.note += QStringLiteral("Текстура %1 не прочитана. ").arg(file);
            continue;
        }
        if (image.width() > maxTexture || image.height() > maxTexture) {
            mesh.note += QStringLiteral("Текстура %1 уменьшена с %2×%3 до %4 px. ").arg(file).arg(image.width()).arg(image.height()).arg(maxTexture);
            image = image.scaled(maxTexture, maxTexture, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        pt.texture = image.convertToFormat(QImage::Format_RGBA8888);
    }
    for (const QVector3D &pos : mesh.positions) mesh.bounds.add(pos);
    if (mesh.normals.isEmpty()) computeNormals(mesh);
    mesh.georeferenced = readOdmOffset(odmCoordsFor(path), &mesh.origin, &mesh.crs);   // ODM models are local to coords.txt
    if (mesh.positions.isEmpty() || mesh.triangles == 0) mesh.error = QStringLiteral("В модели нет граней");
    return mesh;
}

Mesh readMeshPly(const QString &path)
{
    Mesh mesh;
    mesh.source = path;
    Origin odm;
    const bool odmLocal = readOdmOffset(odmCoordsFor(path), &odm, &mesh.crs);
    mesh.georeferenced = odmLocal;
    PlyData d = readPly(path, 0, true, odm, !odmLocal);
    if (!d.error.isEmpty()) {
        mesh.error = d.error;
        return mesh;
    }
    mesh.positions = std::move(d.positions);
    mesh.colors = std::move(d.colors);
    mesh.parts.append(Mesh::Part{QString(), {}, std::move(d.triangles)});
    mesh.triangles = quint64(mesh.parts.first().indices.size() / 3);
    mesh.origin = odm;
    for (const QVector3D &pos : mesh.positions) mesh.bounds.add(pos);
    computeNormals(mesh);
    if (mesh.triangles == 0) mesh.error = QStringLiteral("В PLY нет граней: это облако точек");
    return mesh;
}

QImage readDrape(const QString &orthoPath, const QString &demPath, int maxSize, QString *note)
{
    auto fail = [note](const QString &text) {
        if (note) *note = text;
        return QImage();
    };
    GDALDatasetH dem = GDALOpen(QDir::toNativeSeparators(demPath).toUtf8().constData(), GA_ReadOnly);
    GDALDatasetH ortho = GDALOpen(QDir::toNativeSeparators(orthoPath).toUtf8().constData(), GA_ReadOnly);
    struct Closer { GDALDatasetH a, b; ~Closer() { if (a) GDALClose(a); if (b) GDALClose(b); } } closer{dem, ortho};
    if (!dem || !ortho) return fail(QStringLiteral("Ортофотоплан или модель высот не открываются"));
    double dg[6], og[6];
    if (GDALGetGeoTransform(dem, dg) != CE_None || GDALGetGeoTransform(ortho, og) != CE_None)
        return fail(QStringLiteral("Нет геопривязки"));
    OGRSpatialReferenceH demSrs = GDALGetSpatialRef(dem), orthoSrs = GDALGetSpatialRef(ortho);
    if (!demSrs || !orthoSrs || !OSRIsSame(demSrs, orthoSrs))
        return fail(QStringLiteral("Ортофотоплан в другой системе координат: рельеф без текстуры"));
    const int bands = GDALGetRasterCount(ortho);
    if (bands < 3) return fail(QStringLiteral("Ортофотоплан не цветной"));
    // DEM extent in orthophoto pixels.
    const double x0 = dg[0], y0 = dg[3], x1 = dg[0] + dg[1] * GDALGetRasterXSize(dem), y1 = dg[3] + dg[5] * GDALGetRasterYSize(dem);
    const double px0 = (x0 - og[0]) / og[1], py0 = (y0 - og[3]) / og[5], px1 = (x1 - og[0]) / og[1], py1 = (y1 - og[3]) / og[5];
    const int W = GDALGetRasterXSize(ortho), H = GDALGetRasterYSize(ortho);
    const int c0 = qBound(0, int(std::floor(qMin(px0, px1))), W), r0 = qBound(0, int(std::floor(qMin(py0, py1))), H);
    const int c1 = qBound(0, int(std::ceil(qMax(px0, px1))), W), r1 = qBound(0, int(std::ceil(qMax(py0, py1))), H);
    if (c1 - c0 < 2 || r1 - r0 < 2) return fail(QStringLiteral("Ортофотоплан не покрывает модель высот"));
    const double scale = qMin(1.0, double(maxSize) / qMax(c1 - c0, r1 - r0));
    const int w = qMax(1, int((c1 - c0) * scale)), h = qMax(1, int((r1 - r0) * scale));
    QImage image(w, h, QImage::Format_RGBA8888);
    image.fill(Qt::transparent);
    std::vector<unsigned char> channel(size_t(w) * h);
    GDALRasterIOExtraArg extra;
    INIT_RASTERIO_EXTRA_ARG(extra);
    extra.eResampleAlg = GRIORA_Average;
    for (int b = 0; b < qMin(bands, 4); ++b) {
        if (GDALRasterIOEx(GDALGetRasterBand(ortho, b + 1), GF_Read, c0, r0, c1 - c0, r1 - r0, channel.data(), w, h, GDT_Byte, 0, 0, &extra) != CE_None)
            return fail(QStringLiteral("Ортофотоплан не читается"));
        for (int y = 0; y < h; ++y) {
            uchar *line = image.scanLine(y);
            for (int x = 0; x < w; ++x) line[x * 4 + b] = channel[size_t(y) * w + x];
        }
    }
    if (bands < 4)
        for (int y = 0; y < h; ++y) { uchar *line = image.scanLine(y); for (int x = 0; x < w; ++x) line[x * 4 + 3] = 255; }
    // The DEM extent may reach past the orthophoto: the drape covers the cut region only.
    if (note && (c0 > px0 + 1 || r0 > py0 + 1 || c1 < px1 - 1 || r1 < py1 - 1))
        *note = QStringLiteral("Ортофотоплан покрывает модель высот не полностью");
    if (c0 > px0 + 1 || r0 > py0 + 1 || c1 < px1 - 1 || r1 < py1 - 1) {
        // Place the cut into an image of the full DEM extent.
        const double fullW = (std::fabs(px1 - px0)) * scale, fullH = (std::fabs(py1 - py0)) * scale;
        QImage full(qMax(1, int(fullW)), qMax(1, int(fullH)), QImage::Format_RGBA8888);
        full.fill(Qt::transparent);
        QPainter painter(&full);
        painter.drawImage(QPointF((c0 - qMin(px0, px1)) * scale, (r0 - qMin(py0, py1)) * scale), image);
        painter.end();
        return full;
    }
    return image;
}

Mesh demMesh(const QString &path, int maxGrid, const Origin &origin, const QImage &drape)
{
    Mesh mesh;
    mesh.source = path;
    GDALDatasetH ds = GDALOpen(QDir::toNativeSeparators(path).toUtf8().constData(), GA_ReadOnly);
    if (!ds) {
        mesh.error = QStringLiteral("Не удалось открыть модель высот %1").arg(QDir::toNativeSeparators(path));
        return mesh;
    }
    double gt[6];
    if (GDALGetGeoTransform(ds, gt) != CE_None || gt[2] != 0 || gt[4] != 0) {
        GDALClose(ds);
        mesh.error = QStringLiteral("У модели высот нет геопривязки (или она повёрнута)");
        return mesh;
    }
    const int W = GDALGetRasterXSize(ds), H = GDALGetRasterYSize(ds);
    if (OGRSpatialReferenceH srs = GDALGetSpatialRef(ds)) {
        mesh.georeferenced = true;
        mesh.crs = QString::fromUtf8(OSRGetName(srs));
    }
    const int step = qMax(1, int(std::ceil(double(qMax(W, H)) / qMax(2, maxGrid))));
    const int w = qMax(2, (W + step - 1) / step), h = qMax(2, (H + step - 1) / step);
    GDALRasterBandH band = GDALGetRasterBand(ds, 1);
    int hasNoData = 0;
    const double noData = GDALGetRasterNoDataValue(band, &hasNoData);
    QVector<float> z(w * h);
    GDALRasterIOExtraArg extra;
    INIT_RASTERIO_EXTRA_ARG(extra);
    extra.eResampleAlg = GRIORA_Average;
    const CPLErr err = GDALRasterIOEx(band, GF_Read, 0, 0, W, H, z.data(), w, h, GDT_Float32, 0, 0, &extra);
    GDALClose(ds);
    if (err != CE_None) {
        mesh.error = QStringLiteral("Не удалось прочитать высоты");
        return mesh;
    }
    const auto valid = [&](float value) { return std::isfinite(value) && !(hasNoData && std::fabs(value - noData) < 1e-3) && value > -1e5f; };
    float zMin = 1e9f, zMax = -1e9f;
    for (float value : z) if (valid(value)) { zMin = qMin(zMin, value); zMax = qMax(zMax, value); }
    if (zMin > zMax) {
        mesh.error = QStringLiteral("В модели высот нет данных");
        return mesh;
    }
    const double cellX = gt[1] * W / w, cellY = gt[5] * H / h;   // size of an output cell
    Origin used = origin;
    if (!used.valid) used = Origin{std::floor(gt[0]), std::floor(gt[3] + gt[5] * H), 0.0, true};
    mesh.origin = used;
    QVector<int> index(w * h, -1);
    for (int r = 0; r < h; ++r)
        for (int c = 0; c < w; ++c) {
            const float value = z[r * w + c];
            if (!valid(value)) continue;
            const double x = gt[0] + (c + 0.5) * cellX, y = gt[3] + (r + 0.5) * cellY;
            index[r * w + c] = int(mesh.positions.size());
            mesh.positions << QVector3D(float(x - used.x), float(y - used.y), float(value - used.z));
            const QColor col = RasterStyle::rampColor(RasterStyle::Terrain, (value - zMin) / qMax(1e-6f, zMax - zMin));
            mesh.colors << argb(col.red(), col.green(), col.blue());
            if (!drape.isNull()) mesh.uvs << QVector2D(float((c + 0.5) / w), float((r + 0.5) / h));
        }
    Mesh::Part part;
    if (!drape.isNull()) part.texture = drape.convertToFormat(QImage::Format_RGBA8888);
    for (int r = 0; r + 1 < h; ++r)
        for (int c = 0; c + 1 < w; ++c) {
            const int a = index[r * w + c], b = index[r * w + c + 1], d = index[(r + 1) * w + c], e = index[(r + 1) * w + c + 1];
            if (a >= 0 && b >= 0 && d >= 0) part.indices << quint32(a) << quint32(d) << quint32(b);
            if (b >= 0 && d >= 0 && e >= 0) part.indices << quint32(b) << quint32(d) << quint32(e);
        }
    mesh.triangles = quint64(part.indices.size() / 3);
    mesh.parts << part;
    for (const QVector3D &pos : mesh.positions) mesh.bounds.add(pos);
    computeNormals(mesh);
    if (mesh.triangles == 0) mesh.error = QStringLiteral("В модели высот нет связных ячеек");
    return mesh;
}

} // namespace view3d
