#include "MaskPolygons.h"

#include <cpl_string.h>
#include <gdal.h>
#include <gdal_alg.h>
#include <ogr_api.h>
#include <cmath>

namespace ai {

namespace {
QPolygonF ringOf(OGRGeometryH ring)
{
    QPolygonF r;
    const int n = OGR_G_GetPointCount(ring);
    for (int i = 0; i < n; ++i) r << QPointF(OGR_G_GetX(ring, i), OGR_G_GetY(ring, i));
    if (r.size() > 1 && r.first() == r.last()) r.removeLast();
    return r;
}

double segmentDistance2(const QPointF &p, const QPointF &a, const QPointF &b)
{
    const double dx = b.x() - a.x(), dy = b.y() - a.y();
    const double len2 = dx * dx + dy * dy;
    double t = len2 > 0 ? ((p.x() - a.x()) * dx + (p.y() - a.y()) * dy) / len2 : 0;
    t = qBound(0.0, t, 1.0);
    const double ex = a.x() + t * dx - p.x(), ey = a.y() + t * dy - p.y();
    return ex * ex + ey * ey;
}

// Marks the points of pts[a..b] that Douglas–Peucker keeps (a and b are kept by the caller).
void simplifySpan(const QVector<QPointF> &pts, int a, int b, double tolerance2, QVector<bool> &keep)
{
    QVector<QPair<int, int>> stack{{a, b}};
    while (!stack.isEmpty()) {
        const auto [s, e] = stack.takeLast();
        int far = -1;
        double best = tolerance2;
        for (int i = s + 1; i < e; ++i) {
            const double d = segmentDistance2(pts[i], pts[s], pts[e]);
            if (d > best) { best = d; far = i; }
        }
        if (far < 0) continue;
        keep[far] = true;
        stack.append({s, far});
        stack.append({far, e});
    }
}
} // namespace

bool MaskPolygon::contains(const QPointF &pixel) const
{
    if (!outer.containsPoint(pixel, Qt::OddEvenFill)) return false;
    for (const QPolygonF &h : holes)
        if (h.containsPoint(pixel, Qt::OddEvenFill)) return false;
    return true;
}

QVector<MaskPolygon> maskPolygons(const QImage &maskImage, QString *error)
{
    QVector<MaskPolygon> result;
    const QImage mask = maskImage.convertToFormat(QImage::Format_Grayscale8);
    if (mask.isNull()) {
        if (error) *error = QStringLiteral("Пустая маска");
        return result;
    }
    GDALDriverH mem = GDALGetDriverByName("MEM");
    GDALDatasetH raster = mem ? GDALCreate(mem, "", mask.width(), mask.height(), 1, GDT_Byte, nullptr) : nullptr;
    if (!raster) {
        if (error) *error = QStringLiteral("GDAL: нет драйвера MEM");
        return result;
    }
    GDALRasterBandH band = GDALGetRasterBand(raster, 1);
    CPLErr io = GDALRasterIO(band, GF_Write, 0, 0, mask.width(), mask.height(), const_cast<uchar *>(mask.constBits()),
                             mask.width(), mask.height(), GDT_Byte, 1, int(mask.bytesPerLine()));
    double gt[6] = {0, 1, 0, 0, 0, 1};   // pixel corners, y down
    GDALSetGeoTransform(raster, gt);
    // In-memory vector layer: the MEM driver has vector support since GDAL 3.11, "Memory" before.
    GDALDatasetH vector = GDALCreate(mem, "", 0, 0, 0, GDT_Unknown, nullptr);
    OGRLayerH layer = vector ? GDALDatasetCreateLayer(vector, "mask", nullptr, wkbPolygon, nullptr) : nullptr;
    if (!layer) {
        if (vector) GDALClose(vector);
        GDALDriverH memory = GDALGetDriverByName("Memory");
        vector = memory ? GDALCreate(memory, "", 0, 0, 0, GDT_Unknown, nullptr) : nullptr;
        layer = vector ? GDALDatasetCreateLayer(vector, "mask", nullptr, wkbPolygon, nullptr) : nullptr;
    }
    if (io != CE_None || !layer) {
        if (vector) GDALClose(vector);
        GDALClose(raster);
        if (error) *error = QStringLiteral("GDAL: не удалось подготовить векторизацию маски");
        return result;
    }
    OGRFieldDefnH field = OGR_Fld_Create("value", OFTInteger);
    OGR_L_CreateField(layer, field, TRUE);
    OGR_Fld_Destroy(field);
    char **options = CSLSetNameValue(nullptr, "8CONNECTED", "8");
    // The band is its own mask: zero pixels produce no polygons.
    const CPLErr err = GDALPolygonize(band, band, layer, 0, options, nullptr, nullptr);
    CSLDestroy(options);
    if (err != CE_None) {
        if (error) *error = QStringLiteral("GDAL: %1").arg(QString::fromUtf8(CPLGetLastErrorMsg()));
    } else {
        OGR_L_ResetReading(layer);
        while (OGRFeatureH feature = OGR_L_GetNextFeature(layer)) {
            OGRGeometryH g = OGR_F_GetGeometryRef(feature);
            if (g && wkbFlatten(OGR_G_GetGeometryType(g)) == wkbPolygon && OGR_G_GetGeometryCount(g) > 0) {
                MaskPolygon p;
                p.outer = ringOf(OGR_G_GetGeometryRef(g, 0));
                for (int i = 1; i < OGR_G_GetGeometryCount(g); ++i) p.holes << ringOf(OGR_G_GetGeometryRef(g, i));
                p.area = OGR_G_Area(g);
                if (p.outer.size() >= 3) result << p;
            }
            OGR_F_Destroy(feature);
        }
    }
    GDALClose(vector);
    GDALClose(raster);
    return result;
}

QPolygonF simplifyRing(const QPolygonF &ring, double tolerance)
{
    QVector<QPointF> pts(ring.begin(), ring.end());
    if (pts.size() > 1 && pts.first() == pts.last()) pts.removeLast();
    const int n = int(pts.size());
    if (n <= 4 || tolerance <= 0) return QPolygonF(pts);
    // Split the ring at its first point and the point farthest from it, simplify both halves.
    int far = 0;
    double best = -1;
    for (int i = 1; i < n; ++i) {
        const double d = std::hypot(pts[i].x() - pts[0].x(), pts[i].y() - pts[0].y());
        if (d > best) { best = d; far = i; }
    }
    QVector<QPointF> closed = pts;
    closed << pts.first();
    QVector<bool> keep(n + 1, false);
    keep[0] = keep[far] = keep[n] = true;
    simplifySpan(closed, 0, far, tolerance * tolerance, keep);
    simplifySpan(closed, far, n, tolerance * tolerance, keep);
    QPolygonF out;
    for (int i = 0; i < n; ++i)
        if (keep[i]) out << pts[i];
    return out.size() >= 3 ? out : QPolygonF(pts);
}

} // namespace ai
