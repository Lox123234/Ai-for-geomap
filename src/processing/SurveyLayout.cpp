#include "SurveyLayout.h"
#include "Crs.h"
#include "ExifReader.h"
#include "TrueOrthophoto.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <algorithm>
#include <gdal.h>
#include <vector>

namespace surveylayout {
namespace {
constexpr double Pi = 3.14159265358979323846;
QByteArray native(const QString &path) { return QDir::toNativeSeparators(path).toUtf8(); }

double median(std::vector<double> v)
{
    if (v.empty()) return qQNaN();
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

// Heights of a DSM at points of its CRS (NaN outside or on nodata).
class Dsm
{
public:
    explicit Dsm(const QString &path)
    {
        if (!QFileInfo::exists(path)) return;
        m_ds = GDALOpenEx(native(path).constData(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
        if (!m_ds || GDALGetGeoTransform(m_ds, m_gt) != CE_None || m_gt[2] != 0 || m_gt[4] != 0) {
            if (m_ds) GDALClose(m_ds);
            m_ds = nullptr;
            return;
        }
        m_band = GDALGetRasterBand(m_ds, 1);
        int has = 0;
        m_noData = GDALGetRasterNoDataValue(m_band, &has);
        m_hasNoData = has != 0;
    }
    ~Dsm() { if (m_ds) GDALClose(m_ds); }
    Dsm(const Dsm &) = delete;
    Dsm &operator=(const Dsm &) = delete;
    bool valid() const { return m_ds != nullptr; }
    double at(double x, double y) const
    {
        if (!m_ds) return qQNaN();
        const int c = int(std::floor((x - m_gt[0]) / m_gt[1])), r = int(std::floor((y - m_gt[3]) / m_gt[5]));
        if (c < 0 || r < 0 || c >= GDALGetRasterXSize(m_ds) || r >= GDALGetRasterYSize(m_ds)) return qQNaN();
        float v = 0;
        if (GDALRasterIO(m_band, GF_Read, c, r, 1, 1, &v, 1, 1, GDT_Float32, 0, 0) != CE_None) return qQNaN();
        if (!std::isfinite(v) || (m_hasNoData && std::fabs(v - m_noData) < 1e-3) || v < -1e5) return qQNaN();
        return v;
    }

private:
    GDALDatasetH m_ds = nullptr;
    GDALRasterBandH m_band = nullptr;
    double m_gt[6] = {0, 1, 0, 0, 0, -1};
    double m_noData = 0;
    bool m_hasNoData = false;
};

// The orthophoto reduced to `pixels` on its longest side (RGB, transparent where it has no data).
bool readBackground(const QString &path, int pixels, QImage *image, QRectF *rect)
{
    GDALDatasetH ds = GDALOpenEx(native(path).constData(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
    if (!ds) return false;
    double gt[6];
    const int w = GDALGetRasterXSize(ds), h = GDALGetRasterYSize(ds), bands = GDALGetRasterCount(ds);
    if (GDALGetGeoTransform(ds, gt) != CE_None || gt[2] != 0 || gt[4] != 0 || bands < 3 || w <= 0 || h <= 0) {
        GDALClose(ds);
        return false;
    }
    const double k = double(pixels) / qMax(w, h);
    const int ow = qMax(1, int(std::lround(w * qMin(1.0, k)))), oh = qMax(1, int(std::lround(h * qMin(1.0, k))));
    std::vector<uchar> rgba(size_t(ow) * oh * 4, 255);
    int list[4] = {1, 2, 3, 4};
    const int n = bands >= 4 && GDALGetRasterColorInterpretation(GDALGetRasterBand(ds, 4)) == GCI_AlphaBand ? 4 : 3;
    GDALRasterIOExtraArg extra;
    INIT_RASTERIO_EXTRA_ARG(extra);
    extra.eResampleAlg = GRIORA_Average;
    const bool ok = GDALDatasetRasterIOEx(ds, GF_Read, 0, 0, w, h, rgba.data(), ow, oh, GDT_Byte, n, list, 4, qint64(ow) * 4, 1, &extra) == CE_None;
    GDALClose(ds);
    if (!ok) return false;
    QImage img(ow, oh, QImage::Format_ARGB32);
    for (int y = 0; y < oh; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < ow; ++x) {
            const uchar *p = &rgba[(size_t(y) * ow + x) * 4];
            line[x] = qRgba(p[0], p[1], p[2], n == 4 ? p[3] : 255);
        }
    }
    *image = img;
    const double x0 = gt[0], x1 = gt[0] + w * gt[1], y0 = gt[3], y1 = gt[3] + h * gt[5];
    *rect = QRectF(QPointF(qMin(x0, x1), qMin(y0, y1)), QPointF(qMax(x0, x1), qMax(y0, y1)));
    return true;
}
} // namespace

Layout compute(const QVector<PhotoIn> &photos, const QString &jobDirectory, int backgroundPixels)
{
    Layout out;
    // --- The CRS: the job's results, else the UTM zone of the photos.
    Crs crs;
    if (!jobDirectory.isEmpty()) crs = Crs::fromWkt(ortho::resultCrsWkt(jobDirectory));
    if (!crs.isValid()) {
        double lon = 0, lat = 0;
        int n = 0;
        for (const PhotoIn &p : photos)
            if (p.hasGps) { lon += p.longitude; lat += p.latitude; ++n; }
        if (n) {
            lon /= n;
            lat /= n;
            crs = Crs::fromEpsg((lat < 0 ? 32700 : 32600) + qBound(1, int(std::floor((lon + 180) / 6)) + 1, 60));
        }
    }
    if (!crs.isValid()) {
        out.error = photos.isEmpty() ? QStringLiteral("В проекте нет снимков")
                                     : QStringLiteral("У снимков нет GPS-координат, а выравнивания ещё не было: схему не на чем построить");
        return out;
    }
    out.crsWkt = crs.wkt();
    out.crsLabel = crs.label();
    CoordinateTransform toCrs(Crs::wgs84(), crs);

    // --- The reconstruction of the job, by the photos' names (and by file names: the job's images
    // are the project's files under their internal names).
    ortho::Scene scene;
    QString sceneError;
    const bool aligned = !jobDirectory.isEmpty() && ortho::readOdmJob(jobDirectory, &scene, &sceneError, {}, false);
    if (aligned) out.job = QFileInfo(jobDirectory).fileName();
    QHash<QString, const ortho::Shot *> byName;
    for (const ortho::Shot &s : scene.shots) {
        byName.insert(s.name.toLower(), &s);
        byName.insert(QFileInfo(s.image).fileName().toLower(), &s);
    }
    const Dsm dsm(aligned ? QDir(jobDirectory).filePath(QStringLiteral("odm_dem/dsm.tif")) : QString());

    struct Exif { double relAlt = qQNaN(), alt = qQNaN(), yaw = qQNaN(), pitch = qQNaN(); int focal35 = 0, width = 0, height = 0; };
    QVector<Exif> exif(photos.size());
    for (int i = 0; i < photos.size(); ++i) {
        const ImageMetadata m = ExifReader::read(photos[i].path);
        exif[i] = {m.relativeAltitude, m.altitude, m.yaw, m.pitch, m.focal35, m.width, m.height};
    }

    // --- Cameras: GPS, the alignment.
    QVector<const ortho::Shot *> shotOf(photos.size(), nullptr);
    for (int i = 0; i < photos.size(); ++i) {
        const PhotoIn &p = photos[i];
        Camera c;
        c.row = p.row;
        c.name = p.name;
        c.takenAt = p.takenAt;
        if (p.hasGps) {
            QPointF q(p.longitude, p.latitude);
            if (toCrs.transform(q)) {
                c.hasGps = true;
                c.gpsX = q.x();
                c.gpsY = q.y();
                c.gpsZ = exif[i].alt;
            }
        }
        const ortho::Shot *s = byName.value(p.name.toLower(), nullptr);
        if (!s) s = byName.value(QFileInfo(p.path).fileName().toLower(), nullptr);
        if (s) {
            c.aligned = true;
            c.x = s->centre[0] + scene.offsetX;
            c.y = s->centre[1] + scene.offsetY;
            c.z = s->centre[2];
            shotOf[i] = s;
        }
        out.cameras << c;
    }

    // --- The ground under the cameras: the DSM at their nadir, else the camera heights less
    // their altitude above take-off.
    std::vector<double> ground, height;
    for (int i = 0; i < out.cameras.size(); ++i) {
        const Camera &c = out.cameras[i];
        if (!c.aligned) continue;
        const double g = dsm.at(c.x, c.y);
        if (std::isfinite(g)) ground.push_back(g);
        else if (std::isfinite(exif[i].relAlt)) height.push_back(c.z - exif[i].relAlt);
    }
    out.groundZ = !ground.empty() ? median(ground) : median(height);

    // --- Footprints: the corners of each aligned photo cast on the ground (the DSM's height under
    // the camera, a plane there); else from EXIF around the GPS position.
    for (int i = 0; i < out.cameras.size(); ++i) {
        Camera &c = out.cameras[i];
        if (const ortho::Shot *s = shotOf[i]; s && std::isfinite(out.groundZ)) {
            double g = dsm.at(c.x, c.y);
            if (!std::isfinite(g)) g = out.groundZ;
            QPolygonF poly;
            const double corners[4][2] = {{0, 0}, {double(s->camera.width), 0}, {double(s->camera.width), double(s->camera.height)},
                                          {0, double(s->camera.height)}};
            for (const auto &k : corners) {
                double dir[3];
                s->ray(k[0], k[1], dir);
                if (dir[2] > -0.05) { poly.clear(); break; }   // above the horizon: no footprint
                const double t = (g - s->centre[2]) / dir[2];
                poly << QPointF(s->centre[0] + t * dir[0] + scene.offsetX, s->centre[1] + t * dir[1] + scene.offsetY);
            }
            if (poly.size() == 4) {
                c.footprint = poly;
                c.footprintAligned = true;
                continue;
            }
        }
        const Exif &e = exif[i];
        if (!c.hasGps || !std::isfinite(e.relAlt) || e.relAlt <= 1 || e.focal35 <= 0 || e.width <= 0 || e.height <= 0) continue;
        if (std::isfinite(e.pitch) && std::fabs(e.pitch + 90) > 15) continue;   // oblique: not a rectangle on the ground
        const double diagonal = e.relAlt * 43.2666 / e.focal35, d = std::hypot(double(e.width), double(e.height));
        const double hw = diagonal * e.width / d / 2, hh = diagonal * e.height / d / 2;
        double yaw = e.yaw;
        if (!std::isfinite(yaw)) {   // the top of the photo along the flight: towards the next photo
            const int j = i + 1 < out.cameras.size() ? i + 1 : i - 1;
            yaw = j >= 0 && out.cameras[j].hasGps ? std::atan2((out.cameras[j].gpsX - c.gpsX) * (j > i ? 1 : -1),
                                                               (out.cameras[j].gpsY - c.gpsY) * (j > i ? 1 : -1)) * 180 / Pi
                                                  : 0;
        }
        const double a = yaw * Pi / 180;
        const QPointF up(std::sin(a), std::cos(a)), right(std::cos(a), -std::sin(a)), centre(c.gpsX, c.gpsY);
        c.footprint << centre - right * hw + up * hh << centre + right * hw + up * hh << centre + right * hw - up * hh << centre - right * hw - up * hh;
    }

    // --- Capture order.
    std::stable_sort(out.cameras.begin(), out.cameras.end(), [](const Camera &a, const Camera &b) {
        if (a.takenAt != b.takenAt) return !a.takenAt.isEmpty() && (b.takenAt.isEmpty() || a.takenAt < b.takenAt);
        return QString::compare(a.name, b.name, Qt::CaseInsensitive) < 0;
    });

    // --- Background: the job's orthophoto.
    if (aligned && backgroundPixels > 0)
        for (const char *rel : {"geomap/true_orthophoto.tif", "odm_orthophoto/odm_orthophoto.tif"}) {
            const QString path = QDir(jobDirectory).filePath(QString::fromLatin1(rel));
            if (QFileInfo::exists(path) && readBackground(path, backgroundPixels, &out.background, &out.backgroundRect)) {
                out.backgroundName = rel[0] == 'g' ? QStringLiteral("истинный ортофотоплан") : QStringLiteral("ортофотоплан ODM");
                break;
            }
        }

    // --- Summary and extent.
    Summary &s = out.summary;
    double sx = 0, sy = 0, sz = 0;
    int nz = 0;
    for (const Camera &c : out.cameras) {
        ++s.photos;
        s.withGps += c.hasGps;
        if (aligned) (c.aligned ? s.aligned : s.notAligned) += 1;
        if (c.hasError()) {
            ++s.compared;
            sx += c.dx() * c.dx();
            sy += c.dy() * c.dy();
            if (std::isfinite(c.dz())) { sz += c.dz() * c.dz(); ++nz; }
        }
        auto add = [&](const QRectF &r) { out.extent = out.extent.isNull() ? r : out.extent.united(r); };
        if (c.hasGps) add(QRectF(QPointF(c.gpsX, c.gpsY), QSizeF(1e-9, 1e-9)));
        if (c.aligned) add(QRectF(QPointF(c.x, c.y), QSizeF(1e-9, 1e-9)));
        if (!c.footprint.isEmpty()) add(c.footprint.boundingRect());
    }
    if (s.compared) {
        s.rmseX = std::sqrt(sx / s.compared);
        s.rmseY = std::sqrt(sy / s.compared);
        s.rmseXY = std::sqrt((sx + sy) / s.compared);
        if (nz) s.rmseZ = std::sqrt(sz / nz);
        s.rmse = std::isfinite(s.rmseZ) ? std::sqrt(s.rmseXY * s.rmseXY + s.rmseZ * s.rmseZ) : s.rmseXY;
    }
    if (!out.background.isNull()) out.extent = out.extent.isNull() ? out.backgroundRect : out.extent.united(out.backgroundRect);
    if (out.extent.isNull()) out.error = QStringLiteral("У снимков нет GPS-координат, а выравнивания ещё не было: схему не на чем построить");
    return out;
}

double heightAt(const QString &jobDirectory, double x, double y)
{
    if (jobDirectory.isEmpty()) return qQNaN();
    const Dsm dsm(QDir(jobDirectory).filePath(QStringLiteral("odm_dem/dsm.tif")));
    return dsm.at(x, y);
}

} // namespace surveylayout
