#include "Overlap.h"
#include "Crs.h"
#include "ExifReader.h"
#include "TrueOrthophoto.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>
#include <vector>
#include <cpl_conv.h>
#include <cpl_string.h>
#include <gdal.h>
#include <ogr_srs_api.h>

namespace overlap {
namespace {
constexpr double Pi = 3.14159265358979323846;
QByteArray native(const QString &path) { return QDir::toNativeSeparators(path).toUtf8(); }

// Grid of photo counts in a projected CRS (north up).
struct Grid
{
    double x0 = 0, y0 = 0, cell = 1;   // top-left corner
    int width = 0, height = 0;
    std::vector<std::atomic<uint16_t>> count;
    void init(double minX, double minY, double maxX, double maxY, double cellSize)
    {
        cell = cellSize;
        width = qMax(1, int(std::ceil((maxX - minX) / cell)));
        height = qMax(1, int(std::ceil((maxY - minY) / cell)));
        x0 = minX;
        y0 = maxY;
        count = std::vector<std::atomic<uint16_t>>(size_t(width) * height);
    }
    double cx(int c) const { return x0 + (c + 0.5) * cell; }
    double cy(int r) const { return y0 - (r + 0.5) * cell; }
};

template <typename F> void parallelRows(int rows, F body)
{
    std::atomic<int> next{0};
    const int threads = qMax(1, qMin(rows, int(std::thread::hardware_concurrency())));
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t)
        pool.emplace_back([&] {
            for (int r = next++; r < rows; r = next++) body(r);
        });
    for (std::thread &t : pool) t.join();
}

// Statistics and the GeoTIFF (Byte, 0 = no photo, counts above 255 clipped).
bool finish(const Grid &g, const QString &wkt, const QString &out, Result *res)
{
    std::vector<int> histogram(256, 0);
    std::vector<uchar> bytes(size_t(g.width) * g.height);
    for (size_t i = 0; i < bytes.size(); ++i) {
        const int n = qMin<int>(255, g.count[i].load());
        bytes[i] = uchar(n);
        ++histogram[n];
    }
    qint64 covered = 0;
    for (int n = 1; n < 256; ++n) covered += histogram[n];
    if (covered == 0) {
        res->error = QStringLiteral("Снимки не покрывают ни одной точки местности");
        return false;
    }
    auto shareAtLeast = [&](int k) {
        qint64 s = 0;
        for (int n = k; n < 256; ++n) s += histogram[n];
        return double(s) / covered;
    };
    qint64 half = 0;
    for (int n = 1; n < 256; ++n) {
        half += histogram[n];
        if (half * 2 >= covered) {
            res->median = n;
            break;
        }
    }
    res->share2 = shareAtLeast(2);
    res->share3 = shareAtLeast(3);
    res->share5 = shareAtLeast(5);
    res->share9 = shareAtLeast(9);
    res->area = covered * g.cell * g.cell;
    res->cell = g.cell;
    res->width = g.width;
    res->height = g.height;

    QDir().mkpath(QFileInfo(out).absolutePath());
    const QString partial = out + QStringLiteral(".partial.tif");
    char **options = nullptr;
    options = CSLSetNameValue(options, "COMPRESS", "DEFLATE");
    options = CSLSetNameValue(options, "TILED", "YES");
    GDALDatasetH ds = GDALCreate(GDALGetDriverByName("GTiff"), native(partial).constData(), g.width, g.height, 1, GDT_Byte, options);
    CSLDestroy(options);
    if (!ds) {
        res->error = QStringLiteral("Не удалось создать %1: %2").arg(QDir::toNativeSeparators(out), QString::fromUtf8(CPLGetLastErrorMsg()));
        return false;
    }
    double gt[6] = {g.x0, g.cell, 0, g.y0, 0, -g.cell};
    GDALSetGeoTransform(ds, gt);
    GDALSetProjection(ds, wkt.toUtf8().constData());
    GDALRasterBandH band = GDALGetRasterBand(ds, 1);
    GDALSetRasterNoDataValue(band, 0);
    GDALSetDescription(band, "photos");
    const bool written = GDALRasterIO(band, GF_Write, 0, 0, g.width, g.height, bytes.data(), g.width, g.height, GDT_Byte, 0, 0) == CE_None;
    GDALClose(ds);
    QFile::remove(out);
    if (!written || !QFile::rename(partial, out)) {
        QFile::remove(partial);
        res->error = QStringLiteral("Не удалось записать %1").arg(QDir::toNativeSeparators(out));
        return false;
    }
    res->path = out;
    return true;
}

double median(std::vector<double> v)
{
    if (v.empty()) return NAN;
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}
} // namespace

Result fromJob(const QString &job, const QString &out, const QString &photoFolder)
{
    Result res;
    res.method = QStringLiteral("job");
    res.job = QFileInfo(job).fileName();
    ortho::Scene scene;
    QString images = QDir(job).exists(QStringLiteral("images")) ? QDir(job).filePath(QStringLiteral("images")) : photoFolder;
    if (!ortho::readOdmJob(job, &scene, &res.error, images)) {
        if (photoFolder.isEmpty() || images == photoFolder) return res;
        res.error.clear();
        scene = {};
        if (!ortho::readOdmJob(job, &scene, &res.error, photoFolder)) return res;
    }
    const QString wkt = ortho::resultCrsWkt(job);
    if (wkt.isEmpty()) {
        res.error = QStringLiteral("Не определена система координат результатов задания");
        return res;
    }
    res.photos = int(scene.shots.size());

    // The ground: the job's DSM, else a plane at the median height of the sparse points.
    double z0 = NAN;
    {
        QFile f(QDir(job).filePath(QStringLiteral("opensfm/reconstruction.json")));
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonArray all = QJsonDocument::fromJson(f.readAll()).array();
            std::vector<double> z;
            for (const QJsonValue &rec : all) {
                const QJsonObject points = rec.toObject().value(QStringLiteral("points")).toObject();
                for (auto it = points.begin(); it != points.end(); ++it)
                    z.push_back(it.value().toObject().value(QStringLiteral("coordinates")).toArray().at(2).toDouble());
            }
            z0 = median(z);
        }
    }
    GDALDatasetH dsm = nullptr;
    const QString dsmPath = QDir(job).filePath(QStringLiteral("odm_dem/dsm.tif"));
    if (QFileInfo::exists(dsmPath)) dsm = GDALOpenEx(native(dsmPath).constData(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
    double dsmGt[6] = {0, 1, 0, 0, 0, -1};
    int dsmW = 0, dsmH = 0;
    if (dsm) {
        GDALGetGeoTransform(dsm, dsmGt);
        dsmW = GDALGetRasterXSize(dsm);
        dsmH = GDALGetRasterYSize(dsm);
        if (!std::isfinite(z0)) {   // no sparse points: the middle of the DSM's range (nothing written next to it)
            double range[2] = {0, 0};
            GDALComputeRasterMinMax(GDALGetRasterBand(dsm, 1), TRUE, range);
            z0 = (range[0] + range[1]) / 2;
        }
    }
    if (!dsm && !std::isfinite(z0)) {
        res.error = QStringLiteral("В задании нет ни модели поверхности, ни точек реконструкции");
        return res;
    }

    // Footprints on the ground plane: the grid's extent and each photo's cells.
    const double ground = z0;
    struct Box { double minX, minY, maxX, maxY; bool all; };
    std::vector<Box> boxes;
    double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;
    double gsd = 1e300;
    for (const ortho::Shot &s : scene.shots) {
        Box b{1e300, 1e300, -1e300, -1e300, false};
        const double w = s.camera.width, h = s.camera.height;
        const double samples[9][2] = {{0, 0}, {w / 2, 0}, {w, 0}, {w, h / 2}, {w, h}, {w / 2, h}, {0, h}, {0, h / 2}, {w / 2, h / 2}};
        for (const auto &p : samples) {
            double d[3];
            s.ray(p[0] - 0.5, p[1] - 0.5, d);
            if (d[2] > -0.05) {   // towards the horizon: no bounded footprint
                b.all = true;
                break;
            }
            const double t = (ground - s.centre[2]) / d[2];
            const double x = s.centre[0] + t * d[0] + scene.offsetX, y = s.centre[1] + t * d[1] + scene.offsetY;
            b.minX = qMin(b.minX, x);
            b.maxX = qMax(b.maxX, x);
            b.minY = qMin(b.minY, y);
            b.maxY = qMax(b.maxY, y);
        }
        if (!b.all) {
            // Relief and the plane's error: a margin of a quarter of the footprint.
            const double mx = (b.maxX - b.minX) * 0.25, my = (b.maxY - b.minY) * 0.25;
            b.minX -= mx;
            b.maxX += mx;
            b.minY -= my;
            b.maxY += my;
            minX = qMin(minX, b.minX);
            maxX = qMax(maxX, b.maxX);
            minY = qMin(minY, b.minY);
            maxY = qMax(maxY, b.maxY);
            const double height = s.centre[2] - ground;
            if (height > 0) gsd = qMin(gsd, height / (s.camera.fx * qMax(w, h)));
        }
        boxes.push_back(b);
    }
    if (dsm) {   // the DSM bounds the result
        minX = qMax(minX, dsmGt[0]);
        maxX = qMin(maxX, dsmGt[0] + dsmW * dsmGt[1]);
        maxY = qMin(maxY, dsmGt[3]);
        minY = qMax(minY, dsmGt[3] + dsmH * dsmGt[5]);
    }
    if (!(maxX > minX && maxY > minY)) {
        if (dsm) GDALClose(dsm);
        res.error = QStringLiteral("Не удалось определить площадь съёмки по позам камер");
        return res;
    }
    // About 1000 cells across, never finer than ten pixels of the photos.
    const double cell = qMax(qMax(maxX - minX, maxY - minY) / 1000.0, gsd < 1e300 ? gsd * 10 : 0.0);
    Grid g;
    g.init(minX, minY, maxX, maxY, cell);

    // Surface heights of the cells.
    std::vector<float> z(size_t(g.width) * g.height, float(ground));
    if (dsm) {   // the DSM window under the grid, resampled to it in one read
        GDALRasterBandH band = GDALGetRasterBand(dsm, 1);
        int hasNodata = 0;
        const double nodata = GDALGetRasterNoDataValue(band, &hasNodata);
        const int x0 = qBound(0, int(std::floor((g.x0 - dsmGt[0]) / dsmGt[1])), dsmW - 1);
        const int y0 = qBound(0, int(std::floor((g.y0 - dsmGt[3]) / dsmGt[5])), dsmH - 1);
        const int x1 = qBound(x0 + 1, int(std::ceil((g.x0 + g.width * g.cell - dsmGt[0]) / dsmGt[1])), dsmW);
        const int y1 = qBound(y0 + 1, int(std::ceil((g.y0 - g.height * g.cell - dsmGt[3]) / dsmGt[5])), dsmH);
        std::vector<float> values(z.size());
        if (GDALRasterIO(band, GF_Read, x0, y0, x1 - x0, y1 - y0, values.data(), g.width, g.height, GDT_Float32, 0, 0) == CE_None)
            for (size_t i = 0; i < z.size(); ++i)
                if (std::isfinite(values[i]) && !(hasNodata && values[i] == float(nodata))) z[i] = values[i];
        GDALClose(dsm);
    }

    parallelRows(g.height, [&](int r) {
        const double y = g.cy(r);
        for (int i = 0; i < int(scene.shots.size()); ++i) {
            const Box &b = boxes[size_t(i)];
            if (!b.all && (y < b.minY || y > b.maxY)) continue;
            const int c0 = b.all ? 0 : qMax(0, int((b.minX - g.x0) / g.cell));
            const int c1 = b.all ? g.width - 1 : qMin(g.width - 1, int((b.maxX - g.x0) / g.cell));
            const ortho::Shot &s = scene.shots[i];
            for (int c = c0; c <= c1; ++c) {
                const double X[3] = {g.cx(c) - scene.offsetX, y - scene.offsetY, z[size_t(r) * g.width + c]};
                double px, py, depth;
                if (s.project(X, &px, &py, &depth) && px >= -0.5 && py >= -0.5 && px < s.camera.width - 0.5 && py < s.camera.height - 0.5)
                    g.count[size_t(r) * g.width + c].fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    res.note = dsm ? QStringLiteral("Снимки спроецированы на модель поверхности задания (без учёта заслонения).")
                   : QStringLiteral("Модели поверхности нет: снимки спроецированы на плоскость на медианной высоте точек реконструкции.");
    finish(g, wkt, out, &res);
    return res;
}

Result fromPhotos(const QStringList &paths, const QString &out)
{
    Result res;
    res.method = QStringLiteral("exif");
    struct Foot { double x, y, halfW, halfH, yaw; bool hasYaw; };
    std::vector<Foot> feet;
    std::vector<QPointF> lonLat;
    std::vector<ImageMetadata> metas;
    int noAltitude = 0, noFocal = 0, oblique = 0, noGps = 0;
    for (const QString &path : paths) {
        const ImageMetadata m = ExifReader::read(path);
        if (!m.hasGps) { ++noGps; continue; }
        if (!std::isfinite(m.relativeAltitude) || m.relativeAltitude <= 1) { ++noAltitude; continue; }
        if (m.focal35 <= 0) { ++noFocal; continue; }
        if (std::isfinite(m.pitch) && std::fabs(m.pitch + 90) > 15) { ++oblique; continue; }
        metas.push_back(m);
        lonLat.emplace_back(m.longitude, m.latitude);
    }
    res.skipped = noGps + noAltitude + noFocal + oblique;
    if (metas.empty()) {
        QStringList why;
        if (noGps) why << QStringLiteral("без GPS: %1").arg(noGps);
        if (noAltitude) why << QStringLiteral("без высоты над точкой взлёта (DJI XMP RelativeAltitude): %1").arg(noAltitude);
        if (noFocal) why << QStringLiteral("без эквивалентного фокусного расстояния (EXIF FocalLengthIn35mmFilm): %1").arg(noFocal);
        if (oblique) why << QStringLiteral("наклонных: %1").arg(oblique);
        res.error = QStringLiteral("Оценка перекрытия до выравнивания невозможна: нет снимков с нужными данными (%1). "
                                   "После выравнивания перекрытие считается по реконструкции.").arg(why.join(QStringLiteral("; ")));
        return res;
    }
    // UTM zone of the survey.
    const double lon0 = lonLat.front().x(), lat0 = lonLat.front().y();
    const int zone = qBound(1, int(std::floor((lon0 + 180) / 6)) + 1, 60);
    const Crs utm = Crs::fromEpsg((lat0 < 0 ? 32700 : 32600) + zone);
    CoordinateTransform toUtm(Crs::wgs84(), utm);
    if (!utm.isValid() || !toUtm.isValid()) {
        res.error = QStringLiteral("Не удалось перевести координаты снимков в UTM: %1").arg(toUtm.error());
        return res;
    }
    double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300, smallest = 1e300;
    for (size_t i = 0; i < metas.size(); ++i) {
        const ImageMetadata &m = metas[i];
        QPointF p = lonLat[i];
        if (!toUtm.transform(p)) continue;
        // Ground diagonal from the 35 mm equivalent focal length (diagonal of 36×24 mm = 43.27 mm).
        const double diagonal = m.relativeAltitude * 43.2666 / m.focal35;
        const double d = std::hypot(double(m.width), double(m.height));
        Foot f{p.x(), p.y(), diagonal * m.width / d / 2, diagonal * m.height / d / 2, std::isfinite(m.yaw) ? m.yaw : 0, std::isfinite(m.yaw)};
        feet.push_back(f);
        const double r = std::hypot(f.halfW, f.halfH);
        minX = qMin(minX, f.x - r);
        maxX = qMax(maxX, f.x + r);
        minY = qMin(minY, f.y - r);
        maxY = qMax(maxY, f.y + r);
        smallest = qMin(smallest, qMin(f.halfW, f.halfH));
    }
    // Without a heading the photo's top looks along the flight (towards the next photo).
    bool guessed = false;
    for (size_t i = 0; i < feet.size(); ++i) {
        if (feet[i].hasYaw) continue;
        const Foot &a = feet[i == feet.size() - 1 && i > 0 ? i - 1 : i];
        const Foot &b = feet[i == feet.size() - 1 && i > 0 ? i : qMin(i + 1, feet.size() - 1)];
        if (&a != &b) feet[i].yaw = std::atan2(b.x - a.x, b.y - a.y) * 180 / Pi;
        guessed = true;
    }
    res.photos = int(feet.size());
    if (feet.empty()) {
        res.error = QStringLiteral("Не удалось перевести координаты снимков в UTM");
        return res;
    }
    Grid g;
    g.init(minX, minY, maxX, maxY, qMax(qMax(maxX - minX, maxY - minY) / 1000.0, smallest / 50));
    parallelRows(g.height, [&](int r) {
        const double y = g.cy(r);
        for (const Foot &f : feet) {
            const double reach = std::hypot(f.halfW, f.halfH);
            if (std::fabs(y - f.y) > reach) continue;
            const double a = f.yaw * Pi / 180, ca = std::cos(a), sa = std::sin(a);
            const int c0 = qMax(0, int((f.x - reach - g.x0) / g.cell)), c1 = qMin(g.width - 1, int((f.x + reach - g.x0) / g.cell));
            for (int c = c0; c <= c1; ++c) {
                const double dx = g.cx(c) - f.x, dy = y - f.y;
                // Photo axes: v along the heading (top of the image), u to its right.
                const double v = dx * sa + dy * ca, u = dx * ca - dy * sa;
                if (std::fabs(u) <= f.halfW && std::fabs(v) <= f.halfH) g.count[size_t(r) * g.width + c].fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    res.note = QStringLiteral("Оценка до выравнивания: снимки считаются надирными над плоской местностью на высоте над точкой взлёта; "
                              "рельеф, высокие объекты и наклоны не учтены.")
               + (guessed ? QStringLiteral(" Курс части снимков взят по направлению полёта.") : QString())
               + (res.skipped > 0 ? QStringLiteral(" Пропущено снимков без нужных данных: %1.").arg(res.skipped) : QString());
    finish(g, utm.wkt(), out, &res);
    return res;
}

} // namespace overlap
