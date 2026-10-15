#include "TrueOrthophoto.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <deque>
#include <limits>
#include <list>
#include <map>
#include <thread>
#include <cpl_string.h>
#include <gdal.h>
#include <gdal_alg.h>
#include <gdal_utils.h>

namespace ortho {

namespace {
constexpr float NaN = std::numeric_limits<float>::quiet_NaN();
constexpr double Pi = 3.14159265358979323846;
constexpr double MaxObjectSlant = 55 * Pi / 180;   // object's photo used while it sees the surface at most this obliquely

void rodrigues(const double r[3], double R[9])
{
    const double theta = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (theta < 1e-12) {
        const double I[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        std::copy(I, I + 9, R);
        return;
    }
    const double k[3] = {r[0] / theta, r[1] / theta, r[2] / theta};
    const double c = std::cos(theta), s = std::sin(theta), v = 1 - c;
    R[0] = c + k[0] * k[0] * v;        R[1] = k[0] * k[1] * v - k[2] * s; R[2] = k[0] * k[2] * v + k[1] * s;
    R[3] = k[1] * k[0] * v + k[2] * s; R[4] = c + k[1] * k[1] * v;        R[5] = k[1] * k[2] * v - k[0] * s;
    R[6] = k[2] * k[0] * v - k[1] * s; R[7] = k[2] * k[1] * v + k[0] * s; R[8] = c + k[2] * k[2] * v;
}

QByteArray native(const QString &path) { return QDir::toNativeSeparators(path).toUtf8(); }

// Grid of the DSM with its values (NaN = no data).
struct Grid
{
    int w = 0, h = 0;
    double gt[6] = {0, 1, 0, 0, 0, -1};
    std::vector<float> v;
    float at(int x, int y) const { return v[size_t(y) * w + x]; }
    // Height on the triangulated surface (cell centres at integers; triangles a-b-c and b-d-c of
    // every cell, as in the z-buffers), so a point never hides behind its own surface.
    float surface(double fx, double fy, int stride) const
    {
        const double sx = qBound(0.0, fx, double(w - 1)) / stride, sy = qBound(0.0, fy, double(h - 1)) / stride;
        const int gw = (w - 1) / stride, gh = (h - 1) / stride;
        const int i = qMin(int(sx), qMax(0, gw - 1)), j = qMin(int(sy), qMax(0, gh - 1));
        const double ax = qBound(0.0, sx - i, 1.0), ay = qBound(0.0, sy - j, 1.0);
        const int x0 = i * stride, y0 = j * stride, x1 = qMin(x0 + stride, w - 1), y1 = qMin(y0 + stride, h - 1);
        const float a = at(x0, y0), b = at(x1, y0), c = at(x0, y1), d = at(x1, y1);
        if (std::isnan(a) || std::isnan(b) || std::isnan(c) || std::isnan(d))
            return at(qBound(0, int(std::lround(fx)), w - 1), qBound(0, int(std::lround(fy)), h - 1));
        if (ax + ay <= 1) return float(a + ax * (b - a) + ay * (c - a));
        return float(d + (1 - ax) * (c - d) + (1 - ay) * (b - d));
    }
};

// Separable running minimum / maximum (monotonic queue), window 2r+1, NaN ignored.
void runningFilter(std::vector<float> &v, int w, int h, int r, bool minimum)
{
    std::vector<float> line, out;
    auto pass = [&](int count, int length, auto index) {
        line.resize(length);
        out.resize(length);
        for (int k = 0; k < count; ++k) {
            for (int i = 0; i < length; ++i) line[i] = v[index(k, i)];
            std::deque<int> q;
            for (int i = 0; i < length + r; ++i) {
                if (i < length && !std::isnan(line[i])) {
                    while (!q.empty() && (minimum ? line[q.back()] >= line[i] : line[q.back()] <= line[i])) q.pop_back();
                    q.push_back(i);
                }
                const int c = i - r;
                if (c < 0) continue;
                while (!q.empty() && q.front() < c - r) q.pop_front();
                out[c] = q.empty() ? NaN : line[q.front()];
            }
            for (int i = 0; i < length; ++i) v[index(k, i)] = std::isnan(line[i]) ? NaN : out[i];
        }
    };
    pass(h, w, [w](int y, int x) { return size_t(y) * w + x; });
    pass(w, h, [w](int x, int y) { return size_t(y) * w + x; });
}

// Photos decoded on demand, kept while they fit in the budget (least recently used dropped).
class PhotoCache
{
public:
    explicit PhotoCache(qint64 budgetBytes) : m_budget(budgetBytes) {}
    QImage get(const QString &path)
    {
        QMutexLocker lock(&m_mutex);
        for (auto it = m_items.begin(); it != m_items.end(); ++it)
            if (it->first == path) {
                m_items.splice(m_items.begin(), m_items, it);
                return m_items.front().second;
            }
        QImageReader reader(path);
        reader.setAutoTransform(false);   // poses refer to the stored pixel grid
        QImage image = reader.read().convertToFormat(QImage::Format_RGB32);
        m_items.emplace_front(path, image);
        m_used += image.sizeInBytes();
        while (m_used > m_budget && m_items.size() > 1) {
            m_used -= m_items.back().second.sizeInBytes();
            m_items.pop_back();
        }
        return image;
    }

private:
    QMutex m_mutex;
    std::list<std::pair<QString, QImage>> m_items;
    qint64 m_used = 0, m_budget;
};

inline void sample(const QImage &img, double px, double py, double rgb[3])
{
    const int w = img.width(), h = img.height();
    px = qBound(0.0, px, w - 1.001);
    py = qBound(0.0, py, h - 1.001);
    const int x0 = int(px), y0 = int(py);
    const double ax = px - x0, ay = py - y0;
    const QRgb *l0 = reinterpret_cast<const QRgb *>(img.constScanLine(y0)), *l1 = reinterpret_cast<const QRgb *>(img.constScanLine(y0 + 1));
    const QRgb a = l0[x0], b = l0[x0 + 1], c = l1[x0], d = l1[x0 + 1];
    rgb[0] = (qRed(a) * (1 - ax) + qRed(b) * ax) * (1 - ay) + (qRed(c) * (1 - ax) + qRed(d) * ax) * ay;
    rgb[1] = (qGreen(a) * (1 - ax) + qGreen(b) * ax) * (1 - ay) + (qGreen(c) * (1 - ax) + qGreen(d) * ax) * ay;
    rgb[2] = (qBlue(a) * (1 - ax) + qBlue(b) * ax) * (1 - ay) + (qBlue(c) * (1 - ax) + qBlue(d) * ax) * ay;
}

// Brown & Lowe gain compensation: gains g minimising Σ N_ij (g_i I_ij − g_j I_ji)² / σn² + Σ N_ij (1 − g_i)² / σg².
std::vector<double> solveGains(int n, const std::vector<double> &N, const std::vector<double> &I)
{
    const double alpha = 1.0 / (10.0 * 10.0), beta = 1.0 / (0.1 * 0.1);
    std::vector<double> A(size_t(n) * n, 0.0), b(n, 0.0);
    for (int i = 0; i < n; ++i) {
        A[size_t(i) * n + i] = 1e-9;
        for (int j = 0; j < n; ++j) {
            const double nij = N[size_t(i) * n + j];
            if (i == j || nij <= 0) continue;
            const double Iij = I[size_t(i) * n + j], Iji = I[size_t(j) * n + i];
            A[size_t(i) * n + i] += nij * (Iij * Iij * alpha + beta);
            A[size_t(i) * n + j] -= nij * alpha * Iij * Iji;
            b[i] += nij * beta;
        }
        if (A[size_t(i) * n + i] < 1e-6) { A[size_t(i) * n + i] = 1; b[i] = 1; }   // no overlaps: gain 1
    }
    // Gaussian elimination with partial pivoting.
    for (int c = 0; c < n; ++c) {
        int p = c;
        for (int r = c + 1; r < n; ++r) if (std::fabs(A[size_t(r) * n + c]) > std::fabs(A[size_t(p) * n + c])) p = r;
        if (p != c) {
            for (int k = 0; k < n; ++k) std::swap(A[size_t(c) * n + k], A[size_t(p) * n + k]);
            std::swap(b[c], b[p]);
        }
        const double d = A[size_t(c) * n + c];
        if (std::fabs(d) < 1e-12) continue;
        for (int r = c + 1; r < n; ++r) {
            const double f = A[size_t(r) * n + c] / d;
            if (f == 0) continue;
            for (int k = c; k < n; ++k) A[size_t(r) * n + k] -= f * A[size_t(c) * n + k];
            b[r] -= f * b[c];
        }
    }
    std::vector<double> g(n, 1.0);
    for (int r = n - 1; r >= 0; --r) {
        double s = b[r];
        for (int k = r + 1; k < n; ++k) s -= A[size_t(r) * n + k] * g[k];
        const double d = A[size_t(r) * n + r];
        g[r] = std::fabs(d) < 1e-12 ? 1.0 : s / d;
        g[r] = qBound(0.5, g[r], 2.0);
    }
    return g;
}

void parallelFor(int count, const std::function<void(int)> &body)
{
    std::atomic_int next{0};
    const int threads = qMax(1, qMin(count, int(std::thread::hardware_concurrency())));
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t)
        pool.emplace_back([&] {
            for (int i = next++; i < count; i = next++) body(i);
        });
    for (std::thread &t : pool) t.join();
}
} // namespace

bool Shot::project(const double X[3], double *px, double *py, double *depth) const
{
    const double x = R[0] * X[0] + R[1] * X[1] + R[2] * X[2] + t[0];
    const double y = R[3] * X[0] + R[4] * X[1] + R[5] * X[2] + t[1];
    const double z = R[6] * X[0] + R[7] * X[1] + R[8] * X[2] + t[2];
    if (z <= 1e-6) return false;
    const double xn = x / z, yn = y / z;
    if (std::fabs(xn) > 2 || std::fabs(yn) > 2) return false;   // far outside: the distortion polynomial folds back
    const Camera &c = camera;
    const double r2 = xn * xn + yn * yn;
    const double radial = 1 + r2 * (c.k1 + r2 * (c.k2 + r2 * c.k3));
    const double xd = xn * radial + 2 * c.p1 * xn * yn + c.p2 * (r2 + 2 * xn * xn);
    const double yd = yn * radial + c.p1 * (r2 + 2 * yn * yn) + 2 * c.p2 * xn * yn;
    const double size = qMax(c.width, c.height);
    *px = (c.fx * xd + c.cx) * size - 0.5 + c.width / 2.0;
    *py = (c.fy * yd + c.cy) * size - 0.5 + c.height / 2.0;
    *depth = z;
    return true;
}

bool readOdmJob(const QString &job, Scene *scene, QString *error, const QString &photoFolder)
{
    auto fail = [error](const QString &text) {
        if (error) *error = text;
        return false;
    };
    QFile f(QDir(job).filePath(QStringLiteral("opensfm/reconstruction.json")));
    if (!f.open(QIODevice::ReadOnly)) return fail(QStringLiteral("В задании нет реконструкции (opensfm/reconstruction.json)"));
    const QJsonArray all = QJsonDocument::fromJson(f.readAll()).array();
    if (all.isEmpty()) return fail(QStringLiteral("Реконструкция пуста"));
    // The largest reconstruction (ODM may produce a few when photos do not connect).
    QJsonObject rec = all.first().toObject();
    for (const QJsonValue &v : all)
        if (v.toObject().value(QStringLiteral("shots")).toObject().size() > rec.value(QStringLiteral("shots")).toObject().size()) rec = v.toObject();
    QFile coords(QDir(job).filePath(QStringLiteral("odm_georeferencing/coords.txt")));
    if (!coords.open(QIODevice::ReadOnly | QIODevice::Text)) return fail(QStringLiteral("В задании нет odm_georeferencing/coords.txt"));
    const QStringList lines = QString::fromUtf8(coords.readAll()).split(QLatin1Char('\n'));
    const QStringList off = lines.value(1).simplified().split(QLatin1Char(' '));
    if (off.size() < 2) return fail(QStringLiteral("Не прочитано смещение в coords.txt"));
    scene->offsetX = off[0].toDouble();
    scene->offsetY = off[1].toDouble();

    std::map<QString, Camera> cameras;
    const QJsonObject cams = rec.value(QStringLiteral("cameras")).toObject();
    for (auto it = cams.begin(); it != cams.end(); ++it) {
        const QJsonObject o = it.value().toObject();
        const QString type = o.value(QStringLiteral("projection_type")).toString();
        Camera c;
        c.width = o.value(QStringLiteral("width")).toInt();
        c.height = o.value(QStringLiteral("height")).toInt();
        if (type == QLatin1String("brown")) {
            c.fx = o.value(QStringLiteral("focal_x")).toDouble();
            c.fy = o.value(QStringLiteral("focal_y")).toDouble();
            c.cx = o.value(QStringLiteral("c_x")).toDouble();
            c.cy = o.value(QStringLiteral("c_y")).toDouble();
            c.k3 = o.value(QStringLiteral("k3")).toDouble();
            c.p1 = o.value(QStringLiteral("p1")).toDouble();
            c.p2 = o.value(QStringLiteral("p2")).toDouble();
        } else if (type == QLatin1String("perspective")) {
            c.fx = c.fy = o.value(QStringLiteral("focal")).toDouble();
        } else {
            return fail(QStringLiteral("Модель камеры «%1» пока не поддерживается истинным ортофотопланом").arg(type));
        }
        c.k1 = o.value(QStringLiteral("k1")).toDouble();
        c.k2 = o.value(QStringLiteral("k2")).toDouble();
        if (c.width <= 0 || c.height <= 0 || c.fx <= 0) return fail(QStringLiteral("Неполные параметры камеры %1").arg(it.key()));
        cameras[it.key()] = c;
    }
    const QString photos = photoFolder.isEmpty() ? QDir(job).filePath(QStringLiteral("images")) : photoFolder;
    const QJsonObject shots = rec.value(QStringLiteral("shots")).toObject();
    for (auto it = shots.begin(); it != shots.end(); ++it) {
        const QJsonObject o = it.value().toObject();
        const auto cam = cameras.find(o.value(QStringLiteral("camera")).toString());
        if (cam == cameras.end()) continue;
        Shot s;
        s.camera = cam->second;
        s.image = QDir(photos).filePath(it.key());
        if (!QFileInfo::exists(s.image)) return fail(QStringLiteral("Нет снимка %1").arg(QDir::toNativeSeparators(s.image)));
        const QJsonArray r = o.value(QStringLiteral("rotation")).toArray(), t = o.value(QStringLiteral("translation")).toArray();
        const double rv[3] = {r.at(0).toDouble(), r.at(1).toDouble(), r.at(2).toDouble()};
        rodrigues(rv, s.R);
        for (int k = 0; k < 3; ++k) s.t[k] = t.at(k).toDouble();
        for (int k = 0; k < 3; ++k) s.centre[k] = -(s.R[0 + k] * s.t[0] + s.R[3 + k] * s.t[1] + s.R[6 + k] * s.t[2]);
        scene->shots << s;
    }
    if (scene->shots.isEmpty()) return fail(QStringLiteral("В реконструкции нет снимков"));
    return true;
}

Result build(const Scene &scene, const QString &dsmPath, const QString &outputPath, const Options &opt)
{
    Result res;
    QElapsedTimer timer;
    timer.start();
    auto progress = [&](double v) { return !opt.progress || opt.progress(v); };
    const int n = int(scene.shots.size());
    res.photos = n;

    // --- DSM: values, validity, small gaps filled for the heights.
    Grid dsm;
    std::vector<uchar> valid;
    char *srsWkt = nullptr;
    {
        GDALDatasetH ds = GDALOpenEx(native(dsmPath).constData(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
        if (!ds) {
            res.error = QStringLiteral("Не открыть модель высот: %1").arg(QString::fromUtf8(CPLGetLastErrorMsg()));
            return res;
        }
        GDALGetGeoTransform(ds, dsm.gt);
        if (dsm.gt[2] != 0 || dsm.gt[4] != 0 || dsm.gt[5] >= 0) {
            GDALClose(ds);
            res.error = QStringLiteral("Модель высот повёрнута — не поддерживается");
            return res;
        }
        srsWkt = CPLStrdup(GDALGetProjectionRef(ds));
        dsm.w = GDALGetRasterXSize(ds);
        dsm.h = GDALGetRasterYSize(ds);
        GDALDriverH mem = GDALGetDriverByName("MEM");
        GDALDatasetH copy = GDALCreateCopy(mem, "", ds, FALSE, nullptr, nullptr, nullptr);
        GDALClose(ds);
        GDALRasterBandH band = GDALGetRasterBand(copy, 1);
        int has = 0;
        const double nodata = GDALGetRasterNoDataValue(band, &has);
        dsm.v.resize(size_t(dsm.w) * dsm.h);
        GDALRasterIO(band, GF_Read, 0, 0, dsm.w, dsm.h, dsm.v.data(), dsm.w, dsm.h, GDT_Float32, 0, 0);
        valid.resize(dsm.v.size());
        for (size_t i = 0; i < dsm.v.size(); ++i) valid[i] = !(has && dsm.v[i] == float(nodata)) && std::isfinite(dsm.v[i]);
        if (has) GDALFillNodata(band, nullptr, 30, 0, 0, nullptr, nullptr, nullptr);
        GDALRasterIO(band, GF_Read, 0, 0, dsm.w, dsm.h, dsm.v.data(), dsm.w, dsm.h, GDT_Float32, 0, 0);
        for (float &z : dsm.v) if (has && z == float(nodata)) z = NaN;
        GDALClose(copy);
    }
    const double cell = dsm.gt[1];
    auto local = [&](double gx, double gy, double z, double out[3]) {   // DSM cell coordinates → local frame
        out[0] = dsm.gt[0] + (gx + 0.5) * dsm.gt[1] - scene.offsetX;
        out[1] = dsm.gt[3] + (gy + 0.5) * dsm.gt[5] - scene.offsetY;
        out[2] = z;
    };

    // --- Ground and objects: DTM if given, else a grey opening of the DSM (window 12 m).
    std::vector<float> ground;
    if (!opt.groundPath.isEmpty()) {
        GDALDatasetH dtm = GDALOpenEx(native(opt.groundPath).constData(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
        if (dtm) {
            const QByteArray te = QByteArray::number(dsm.gt[0], 'f', 6) + ' ' + QByteArray::number(dsm.gt[3] + dsm.h * dsm.gt[5], 'f', 6) + ' '
                                  + QByteArray::number(dsm.gt[0] + dsm.w * dsm.gt[1], 'f', 6) + ' ' + QByteArray::number(dsm.gt[3], 'f', 6);
            const QList<QByteArray> parts = te.split(' ');
            const QByteArray ws = QByteArray::number(dsm.w), hs = QByteArray::number(dsm.h);
            const char *args[] = {"-of", "MEM", "-te", parts[0].constData(), parts[1].constData(), parts[2].constData(), parts[3].constData(),
                                  "-ts", ws.constData(), hs.constData(), "-r", "bilinear", "-t_srs", srsWkt, "-dstnodata", "-9999", nullptr};
            GDALWarpAppOptions *wo = GDALWarpAppOptionsNew(const_cast<char **>(args), nullptr);
            int usage = 0;
            GDALDatasetH warped = GDALWarp("", nullptr, 1, &dtm, wo, &usage);
            GDALWarpAppOptionsFree(wo);
            if (warped) {
                ground.resize(dsm.v.size());
                GDALRasterIO(GDALGetRasterBand(warped, 1), GF_Read, 0, 0, dsm.w, dsm.h, ground.data(), dsm.w, dsm.h, GDT_Float32, 0, 0);
                for (float &z : ground) if (z <= -9998) z = NaN;
                GDALClose(warped);
            }
            GDALClose(dtm);
        }
    }
    if (ground.empty()) {
        ground = dsm.v;
        const int r = qMax(1, int(std::lround(6.0 / cell)));
        runningFilter(ground, dsm.w, dsm.h, r, true);
        runningFilter(ground, dsm.w, dsm.h, r, false);
    }
    // Objects: connected parts higher than objectHeight, grown by two cells (crown edges).
    std::vector<int> object(dsm.v.size(), -1);
    {
        std::vector<uchar> high(dsm.v.size(), 0);
        for (size_t i = 0; i < high.size(); ++i)
            high[i] = !std::isnan(dsm.v[i]) && !std::isnan(ground[i]) && dsm.v[i] - ground[i] > opt.objectHeight;
        std::vector<uchar> grown = high;
        for (int pass = 0; pass < 2; ++pass) {
            std::vector<uchar> g = grown;
            for (int y = 0; y < dsm.h; ++y)
                for (int x = 0; x < dsm.w; ++x)
                    if (!grown[size_t(y) * dsm.w + x])
                        for (int d = 0; d < 4 && !g[size_t(y) * dsm.w + x]; ++d) {
                            const int nx = x + (d == 0) - (d == 1), ny = y + (d == 2) - (d == 3);
                            if (nx >= 0 && ny >= 0 && nx < dsm.w && ny < dsm.h && grown[size_t(ny) * dsm.w + nx]) g[size_t(y) * dsm.w + x] = 1;
                        }
            grown.swap(g);
        }
        std::vector<int> stack;
        for (int y = 0; y < dsm.h; ++y)
            for (int x = 0; x < dsm.w; ++x) {
                const size_t i = size_t(y) * dsm.w + x;
                if (!grown[i] || object[i] >= 0) continue;
                const int id = res.objects++;
                stack = {int(i)};
                object[i] = id;
                while (!stack.empty()) {
                    const int p = stack.back();
                    stack.pop_back();
                    const int px = p % dsm.w, py = p / dsm.w;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int nx = px + dx, ny = py + dy;
                            if (nx < 0 || ny < 0 || nx >= dsm.w || ny >= dsm.h) continue;
                            const size_t q = size_t(ny) * dsm.w + nx;
                            if (grown[q] && object[q] < 0) {
                                object[q] = id;
                                stack.push_back(int(q));
                            }
                        }
                }
            }
    }
    if (!progress(0.05)) { res.error = QStringLiteral("Отменено"); CPLFree(srsWkt); return res; }

    // --- Z-buffer of the DSM in every photo (reduced), and the photo footprints.
    struct View
    {
        int div = 1, zw = 0, zh = 0;
        std::vector<float> z;
        double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;   // footprint, DSM CRS
    };
    std::vector<View> views(n);
    const int stride = qMax(1, int(std::ceil(std::sqrt(double(dsm.w) * dsm.h / 6e6))));
    parallelFor(n, [&](int s) {
        const Shot &shot = scene.shots[s];
        View &v = views[s];
        v.div = qMax(1, int(std::ceil(shot.camera.width / 1024.0)));
        v.zw = (shot.camera.width + v.div - 1) / v.div;
        v.zh = (shot.camera.height + v.div - 1) / v.div;
        v.z.assign(size_t(v.zw) * v.zh, std::numeric_limits<float>::infinity());
        const int gw = (dsm.w - 1) / stride + 1, gh = (dsm.h - 1) / stride + 1;
        std::vector<float> px(size_t(gw) * gh * 3, NaN);   // projected vertices: x, y, depth (reduced pixels)
        for (int j = 0; j < gh; ++j)
            for (int i = 0; i < gw; ++i) {
                const int x = i * stride, y = j * stride;
                const float z = dsm.at(x, y);
                if (std::isnan(z)) continue;
                double P[3], u, w, d;
                local(x, y, z, P);
                if (!shot.project(P, &u, &w, &d)) continue;
                float *o = &px[(size_t(j) * gw + i) * 3];
                o[0] = float(u / v.div);
                o[1] = float(w / v.div);
                o[2] = float(d);
                if (u >= 0 && w >= 0 && u < shot.camera.width && w < shot.camera.height) {
                    const double X = P[0] + scene.offsetX, Y = P[1] + scene.offsetY;
                    v.minX = qMin(v.minX, X); v.maxX = qMax(v.maxX, X);
                    v.minY = qMin(v.minY, Y); v.maxY = qMax(v.maxY, Y);
                }
            }
        auto triangle = [&](const float *a, const float *b, const float *c) {
            const float x0 = qMax(0.0f, std::floor(qMin(a[0], qMin(b[0], c[0])))), x1 = qMin(float(v.zw - 1), std::ceil(qMax(a[0], qMax(b[0], c[0]))));
            const float y0 = qMax(0.0f, std::floor(qMin(a[1], qMin(b[1], c[1])))), y1 = qMin(float(v.zh - 1), std::ceil(qMax(a[1], qMax(b[1], c[1]))));
            if (x1 < x0 || y1 < y0 || x1 - x0 > 64 || y1 - y0 > 64) return;   // huge = folded projection
            const float area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
            if (std::fabs(area) < 1e-9f) return;
            for (int y = int(y0); y <= int(y1); ++y)
                for (int x = int(x0); x <= int(x1); ++x) {
                    const float qx = x + 0.5f, qy = y + 0.5f;
                    const float w0 = ((b[0] - qx) * (c[1] - qy) - (b[1] - qy) * (c[0] - qx)) / area;
                    const float w1 = ((c[0] - qx) * (a[1] - qy) - (c[1] - qy) * (a[0] - qx)) / area;
                    const float w2 = 1 - w0 - w1;
                    if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) continue;
                    const float d = w0 * a[2] + w1 * b[2] + w2 * c[2];
                    float &zb = v.z[size_t(y) * v.zw + x];
                    if (d < zb) zb = d;
                }
        };
        for (int j = 0; j + 1 < gh; ++j)
            for (int i = 0; i + 1 < gw; ++i) {
                const float *a = &px[(size_t(j) * gw + i) * 3], *b = &px[(size_t(j) * gw + i + 1) * 3];
                const float *c = &px[(size_t(j + 1) * gw + i) * 3], *d = &px[(size_t(j + 1) * gw + i + 1) * 3];
                if (!std::isnan(a[2]) && !std::isnan(b[2]) && !std::isnan(c[2])) triangle(a, b, c);
                if (!std::isnan(b[2]) && !std::isnan(d[2]) && !std::isnan(c[2])) triangle(b, d, c);
            }
    });
    if (!progress(0.2)) { res.error = QStringLiteral("Отменено"); CPLFree(srsWkt); return res; }

    // Normals of the surface (differences over ±2 cells): a photo that looks at a slope along it
    // stretches few of its pixels over many of the orthophoto (the "smears" of draped textures).
    std::vector<float> normals(dsm.v.size() * 3, 0.0f);
    parallelFor(dsm.h, [&](int y) {
        for (int x = 0; x < dsm.w; ++x) {
            const int x0 = qMax(0, x - 2), x1 = qMin(dsm.w - 1, x + 2), y0 = qMax(0, y - 2), y1 = qMin(dsm.h - 1, y + 2);
            const float zx0 = dsm.at(x0, y), zx1 = dsm.at(x1, y), zy0 = dsm.at(x, y0), zy1 = dsm.at(x, y1);
            double gx = 0, gy = 0;
            if (!std::isnan(zx0) && !std::isnan(zx1) && x1 > x0) gx = (zx1 - zx0) / ((x1 - x0) * dsm.gt[1]);
            if (!std::isnan(zy0) && !std::isnan(zy1) && y1 > y0) gy = (zy1 - zy0) / ((y1 - y0) * dsm.gt[5]);   // map y grows north
            const double len = std::sqrt(gx * gx + gy * gy + 1);
            float *nrm = &normals[(size_t(y) * dsm.w + x) * 3];
            nrm[0] = float(-gx / len);
            nrm[1] = float(-gy / len);
            nrm[2] = float(1 / len);
        }
    });
    // Visibility of a local point in a photo; the view's angles from the vertical and from the
    // surface normal of DSM cell `cell` (radians).
    auto see = [&](int s, const double P[3], size_t cell, double *u, double *w, double *vertical, double *slant) {
        const Shot &shot = scene.shots[s];
        double d;
        if (!shot.project(P, u, w, &d)) return false;
        if (*u < 1 || *w < 1 || *u > shot.camera.width - 2 || *w > shot.camera.height - 2) return false;
        const View &v = views[s];
        const int zx = qMin(v.zw - 1, int(*u / v.div)), zy = qMin(v.zh - 1, int(*w / v.div));
        if (d > v.z[size_t(zy) * v.zw + zx] + 0.25 + 0.005 * d) return false;
        const double dx = shot.centre[0] - P[0], dy = shot.centre[1] - P[1], dz = shot.centre[2] - P[2];
        const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
        *vertical = std::acos(qBound(-1.0, dz / len, 1.0));
        const float *nrm = &normals[cell * 3];
        *slant = std::acos(qBound(-1.0, (dx * nrm[0] + dy * nrm[1] + dz * nrm[2]) / len, 1.0));
        return true;
    };

    // --- Ground sample distance and the output grid.
    {
        std::vector<double> gsd;
        double gmid = 0;
        int count = 0;
        for (size_t i = 0; i < dsm.v.size(); i += 97) if (!std::isnan(ground[i])) { gmid += ground[i]; ++count; }
        gmid = count ? gmid / count : 0;
        for (const Shot &s : scene.shots) gsd.push_back((s.centre[2] - gmid) / (s.camera.fx * qMax(s.camera.width, s.camera.height)));
        std::sort(gsd.begin(), gsd.end());
        res.groundSampleDistance = gsd.empty() ? 0 : gsd[gsd.size() / 2];
    }
    res.resolution = opt.resolution > 0 ? opt.resolution : qMax(0.005, std::ceil(res.groundSampleDistance * 1000) / 1000);
    const double extentX = dsm.w * dsm.gt[1], extentY = -dsm.h * dsm.gt[5];
    res.width = int(std::ceil(extentX / res.resolution));
    res.height = int(std::ceil(extentY / res.resolution));
    if (qint64(res.width) * res.height > qint64(2'000'000'000)) {
        res.error = QStringLiteral("Слишком большой ортофотоплан при таком разрешении");
        CPLFree(srsWkt);
        return res;
    }

    // --- Object photos: each object from the photo that sees all of it most vertically.
    std::vector<int> objectShot(res.objects, -1);
    {
        std::vector<std::vector<int>> cells(res.objects);
        for (size_t i = 0; i < object.size(); ++i)
            if (object[i] >= 0 && (cells[object[i]].size() < 4000 || i % 3 == 0)) cells[object[i]].push_back(int(i));
        parallelFor(res.objects, [&](int o) {
            double bestScore = -1e9;
            for (int s = 0; s < n; ++s) {
                int seen = 0;
                double angles = 0;
                for (int c : cells[o]) {
                    double P[3], u, w, a, slant;
                    local(c % dsm.w, c / dsm.w, dsm.v[c], P);
                    if (see(s, P, size_t(c), &u, &w, &a, &slant)) { ++seen; angles += a; }
                }
                if (!seen) continue;
                const double fraction = double(seen) / cells[o].size();
                const double score = fraction >= 0.97 ? 10 - angles / seen : fraction;
                if (score > bestScore) { bestScore = score; objectShot[o] = s; }
            }
        });
    }
    if (!progress(0.3)) { res.error = QStringLiteral("Отменено"); CPLFree(srsWkt); return res; }

    // --- Brightness balance on the ground seen by several photos (photos decoded at 1/8).
    std::vector<double> gain(size_t(n) * 3, 1.0);
    if (opt.balanceColours && n > 1 && n <= 800) {
        std::vector<QImage> small(n);
        parallelFor(n, [&](int s) {
            QImageReader r(scene.shots[s].image);
            r.setAutoTransform(false);
            const QSize full = r.size();
            r.setScaledSize(QSize(qMax(1, full.width() / 8), qMax(1, full.height() / 8)));
            small[s] = r.read().convertToFormat(QImage::Format_RGB32);
        });
        std::vector<double> N(size_t(n) * n, 0.0), I[3];
        for (auto &m : I) m.assign(size_t(n) * n, 0.0);
        const int step = qMax(1, int(std::lround(0.5 / cell)));
        QMutex mutex;
        const int rows = (dsm.h + step - 1) / step;
        parallelFor(rows, [&](int r) {
            std::vector<double> nl(size_t(n) * n, 0.0), il[3];
            for (auto &m : il) m.assign(size_t(n) * n, 0.0);
            std::vector<std::pair<int, std::array<double, 3>>> seen;
            const int y = r * step;
            for (int x = 0; x < dsm.w; x += step) {
                const size_t i = size_t(y) * dsm.w + x;
                if (!valid[i] || object[i] >= 0 || std::isnan(dsm.v[i])) continue;
                double P[3];
                local(x, y, dsm.v[i], P);
                seen.clear();
                for (int s = 0; s < n; ++s) {
                    double u, w, a, slant, rgb[3];
                    if (!see(s, P, i, &u, &w, &a, &slant) || small[s].isNull()) continue;
                    sample(small[s], u / 8.0, w / 8.0, rgb);
                    seen.push_back({s, {rgb[0], rgb[1], rgb[2]}});
                }
                for (size_t a = 0; a < seen.size(); ++a)
                    for (size_t b = 0; b < seen.size(); ++b) {
                        if (a == b) continue;
                        const size_t k = size_t(seen[a].first) * n + seen[b].first;
                        nl[k] += 1;
                        for (int ch = 0; ch < 3; ++ch) il[ch][k] += seen[a].second[ch];
                    }
            }
            QMutexLocker lock(&mutex);
            for (size_t k = 0; k < N.size(); ++k) {
                N[k] += nl[k];
                for (int ch = 0; ch < 3; ++ch) I[ch][k] += il[ch][k];
            }
        });
        for (int ch = 0; ch < 3; ++ch) {
            std::vector<double> mean(size_t(n) * n, 0.0);
            for (size_t k = 0; k < N.size(); ++k) mean[k] = N[k] > 0 ? I[ch][k] / N[k] : 0;
            const std::vector<double> g = solveGains(n, N, mean);
            for (int s = 0; s < n; ++s) gain[size_t(s) * 3 + ch] = g[s];
        }
    }
    for (int s = 0; s < n; ++s) res.gains << (gain[size_t(s) * 3] + gain[size_t(s) * 3 + 1] + gain[size_t(s) * 3 + 2]) / 3;
    if (!progress(0.35)) { res.error = QStringLiteral("Отменено"); CPLFree(srsWkt); return res; }

    // --- Output.
    QDir().mkpath(QFileInfo(outputPath).absolutePath());
    const char *create[] = {"TILED=YES", "BLOCKXSIZE=512", "BLOCKYSIZE=512", "COMPRESS=DEFLATE", "PREDICTOR=2", "BIGTIFF=IF_SAFER",
                            "PHOTOMETRIC=RGB", "ALPHA=YES", "NUM_THREADS=ALL_CPUS", nullptr};
    GDALDatasetH out = GDALCreate(GDALGetDriverByName("GTiff"), native(outputPath).constData(), res.width, res.height, 4, GDT_Byte,
                                  const_cast<char **>(create));
    if (!out) {
        res.error = QStringLiteral("Не создать файл ортофотоплана: %1").arg(QString::fromUtf8(CPLGetLastErrorMsg()));
        CPLFree(srsWkt);
        return res;
    }
    double ogt[6] = {dsm.gt[0], res.resolution, 0, dsm.gt[3], 0, -res.resolution};
    GDALSetGeoTransform(out, ogt);
    GDALSetProjection(out, srsWkt);
    CPLFree(srsWkt);
    GDALSetRasterColorInterpretation(GDALGetRasterBand(out, 4), GCI_AlphaBand);

    PhotoCache photos(qint64(3) << 30);
    QMutex writeMutex;
    const int T = 512, tilesX = (res.width + T - 1) / T, tilesY = (res.height + T - 1) / T;
    std::atomic_int doneTiles{0};
    std::atomic_llong colouredPixels{0}, validPixels{0}, hiddenPixels{0};
    std::atomic_bool cancelled{false};
    const double blend = opt.blendDegrees * Pi / 180;
    parallelFor(tilesX * tilesY, [&](int tile) {
        if (cancelled) return;
        const int tx = tile % tilesX, ty = tile / tilesX;
        const int x0 = tx * T, y0 = ty * T, tw = qMin(T, res.width - x0), th = qMin(T, res.height - y0);
        const double X0 = ogt[0] + x0 * res.resolution, X1 = X0 + tw * res.resolution;
        const double Y1 = ogt[3] - y0 * res.resolution, Y0 = Y1 - th * res.resolution;
        std::vector<int> candidates;
        for (int s = 0; s < n; ++s)
            if (views[s].maxX >= X0 && views[s].minX <= X1 && views[s].maxY >= Y0 && views[s].minY <= Y1) candidates.push_back(s);
        std::vector<uchar> rgba(size_t(tw) * th * 4, 0);
        std::map<int, QImage> images;
        auto photo = [&](int s) -> const QImage & {
            auto it = images.find(s);
            if (it == images.end()) it = images.emplace(s, photos.get(scene.shots[s].image)).first;
            return it->second;
        };
        qint64 coloured = 0, validHere = 0, hidden = 0;
        for (int y = 0; y < th; ++y)
            for (int x = 0; x < tw; ++x) {
                const double X = X0 + (x + 0.5) * res.resolution, Y = Y1 - (y + 0.5) * res.resolution;
                const double gx = (X - dsm.gt[0]) / dsm.gt[1] - 0.5, gy = (Y - dsm.gt[3]) / dsm.gt[5] - 0.5;
                const int cx = qBound(0, int(std::lround(gx)), dsm.w - 1), cy = qBound(0, int(std::lround(gy)), dsm.h - 1);
                if (!valid[size_t(cy) * dsm.w + cx]) continue;
                const float z = dsm.surface(gx, gy, stride);
                if (std::isnan(z)) continue;
                ++validHere;
                const double P[3] = {X - scene.offsetX, Y - scene.offsetY, z};
                double rgb[3] = {0, 0, 0};
                bool done = false;
                // Objects: their own photo, except where it sees the surface too obliquely (smear).
                const size_t cellIndex = size_t(cy) * dsm.w + cx;
                const int o = object[cellIndex];
                if (o >= 0 && objectShot[o] >= 0) {
                    double u, w, a, slant;
                    if (see(objectShot[o], P, cellIndex, &u, &w, &a, &slant) && slant < MaxObjectSlant) {
                        sample(photo(objectShot[o]), u, w, rgb);
                        for (int ch = 0; ch < 3; ++ch) rgb[ch] *= gain[size_t(objectShot[o]) * 3 + ch];
                        done = true;
                    }
                }
                if (!done) {
                    // The photo that looks at the surface most squarely (on flat ground: the most vertical).
                    int s1 = -1, s2 = -1;
                    double a1 = 1e9, a2 = 1e9, u1 = 0, w1 = 0, u2 = 0, w2 = 0;
                    for (int s : candidates) {
                        double u, w, vertical, a;
                        if (!see(s, P, cellIndex, &u, &w, &vertical, &a)) continue;
                        if (a < a1) { s2 = s1; a2 = a1; u2 = u1; w2 = w1; s1 = s; a1 = a; u1 = u; w1 = w; }
                        else if (a < a2) { s2 = s; a2 = a; u2 = u; w2 = w; }
                    }
                    // Hidden from every photo by the surface model (a pit in a noisy crown): the
                    // most vertical photo it falls into, rather than a hole.
                    for (int s : candidates) {
                        if (s1 >= 0) break;
                        double u, w, d;
                        const Shot &shot = scene.shots[s];
                        if (!shot.project(P, &u, &w, &d) || u < 1 || w < 1 || u > shot.camera.width - 2 || w > shot.camera.height - 2) continue;
                        const double dx = shot.centre[0] - P[0], dy = shot.centre[1] - P[1], dz = shot.centre[2] - P[2];
                        const double a = std::acos(qBound(-1.0, dz / std::sqrt(dx * dx + dy * dy + dz * dz), 1.0));
                        if (a < a2) { a2 = a; s2 = s; u2 = u; w2 = w; }
                    }
                    if (s1 < 0 && s2 >= 0) {
                        s1 = s2; a1 = a2; u1 = u2; w1 = w2;
                        s2 = -1;
                        ++hidden;
                    }
                    if (s1 < 0) continue;
                    sample(photo(s1), u1, w1, rgb);
                    for (int ch = 0; ch < 3; ++ch) rgb[ch] *= gain[size_t(s1) * 3 + ch];
                    if (s2 >= 0 && a2 - a1 < blend) {   // soft transition where two photos are almost as good
                        double c2[3];
                        sample(photo(s2), u2, w2, c2);
                        const double k = 0.5 * (1 - (a2 - a1) / blend);
                        for (int ch = 0; ch < 3; ++ch) rgb[ch] = rgb[ch] * (1 - k) + c2[ch] * gain[size_t(s2) * 3 + ch] * k;
                    }
                }
                uchar *p = &rgba[(size_t(y) * tw + x) * 4];
                for (int ch = 0; ch < 3; ++ch) p[ch] = uchar(qBound(0.0, rgb[ch] + 0.5, 255.0));
                p[3] = 255;
                ++coloured;
            }
        {
            QMutexLocker lock(&writeMutex);
            int bands[4] = {1, 2, 3, 4};
            GDALDatasetRasterIO(out, GF_Write, x0, y0, tw, th, rgba.data(), tw, th, GDT_Byte, 4, bands, 4, tw * 4, 1);
        }
        colouredPixels += coloured;
        hiddenPixels += hidden;
        validPixels += validHere;
        if (!progress(0.35 + 0.55 * double(++doneTiles) / (tilesX * tilesY))) cancelled = true;
    });
    if (cancelled) {
        GDALClose(out);
        QFile::remove(outputPath);
        res.error = QStringLiteral("Отменено");
        return res;
    }
    QVector<int> levels;
    for (int f = 2; qMax(res.width, res.height) / f >= 256; f *= 2) levels << f;
    if (!levels.isEmpty()) GDALBuildOverviews(out, "AVERAGE", int(levels.size()), levels.data(), 0, nullptr, nullptr, nullptr);
    GDALClose(out);
    res.coverage = validPixels ? double(colouredPixels) / validPixels : 0;
    res.hidden = colouredPixels ? double(hiddenPixels) / colouredPixels : 0;
    res.seconds = timer.elapsed() / 1000.0;
    progress(1.0);
    return res;
}

} // namespace ortho
