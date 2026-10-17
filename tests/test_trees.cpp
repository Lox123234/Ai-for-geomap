#include <QtTest>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <random>
#include <thread>
#include <cpl_conv.h>
#include <gdal.h>
#include <gdal_alg.h>
#include <ogr_srs_api.h>
#include "GisEnvironment.h"
#include "TrueOrthophoto.h"

// Trees on the orthophoto, against a known answer. A synthetic forest (domes and cones of
// different sizes, overlapping groups of crowns, a building, leaf texture finer than a crown)
// photographed from a flight with 80 % / 70 % overlap and slightly tilted cameras; the
// reconstruction is simulated the way photogrammetry delivers it: a point cloud that is noisy on
// foliage and misses its dark gaps, and a DSM gridded from it with gaps filled and smoothed, as
// OpenDroneMap does. The orthophotos are compared with the surface's own texture in zones:
// crowns, the ground next to crowns (where copies of crowns — "ghosts" — appear) and open ground.
// GEOMAP_TEST_OUTPUT=<folder> keeps the images (truth, results, error maps).
namespace {
constexpr double OffsetX = 400000, OffsetY = 6200000, Ground = 100;
constexpr double Half = 35;                     // scene −35…35 m
constexpr double Cell = 0.02;                   // true surface grid
constexpr int N = int(2 * Half / Cell + 0.5);   // 3500

template <typename F> void parallel(int count, F body)
{
    std::atomic<int> next{0};
    std::vector<std::thread> pool;
    const int threads = qMax(1, int(std::thread::hardware_concurrency()));
    for (int t = 0; t < threads; ++t)
        pool.emplace_back([&] {
            for (int i = next++; i < count; i = next++) body(i);
        });
    for (std::thread &t : pool) t.join();
}

uint32_t mix(int x, int y, int seed)
{
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u + uint32_t(seed) * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
double lattice(int x, int y, int seed) { return (mix(x, y, seed) & 0xFFFFFF) / double(0xFFFFFF); }
double noise(double x, double y, double scale, int seed)   // smooth value noise in [0, 1]
{
    const double fx = x / scale, fy = y / scale;
    const int ix = int(std::floor(fx)), iy = int(std::floor(fy));
    double ax = fx - ix, ay = fy - iy;
    ax = ax * ax * (3 - 2 * ax);
    ay = ay * ay * (3 - 2 * ay);
    const double a = lattice(ix, iy, seed), b = lattice(ix + 1, iy, seed), c = lattice(ix, iy + 1, seed), d = lattice(ix + 1, iy + 1, seed);
    return (a + (b - a) * ax) * (1 - ay) + (c + (d - c) * ax) * ay;
}

struct Tree
{
    double x, y, top, base, radius;
    bool cone;
    double r, g, b;   // leaf colour
};

// The forest: true surface (heights and what is there) on a 2 cm grid, colours analytic.
struct Forest
{
    std::vector<float> h;
    std::vector<uchar> id;   // 0 ground, 1 building, 2 + tree index
    std::vector<Tree> trees;
    double zmax = 0;

    static double groundAt(double x, double y) { return Ground + 0.8 * std::sin(x / 9) + 0.5 * std::cos(y / 7); }
    static bool inBuilding(double x, double y) { return x > -26 && x < -16 && y > 10 && y < 22; }

    void build()
    {
        std::mt19937 rng(20261016);
        auto uni = [&rng](double a, double b) { return a + (b - a) * (rng() / 4294967296.0); };
        // Single trees, then three groups of overlapping crowns.
        while (trees.size() < 22) {
            Tree t;
            t.x = uni(-30, 30);
            t.y = uni(-30, 30);
            t.radius = uni(1.8, 4.8);
            if (inBuilding(t.x, t.y) || std::fabs(t.x + 21) < 5 + t.radius && std::fabs(t.y - 16) < 6 + t.radius) continue;
            bool apart = true;
            for (const Tree &o : trees) apart &= std::hypot(o.x - t.x, o.y - t.y) > o.radius + t.radius + 1.5;
            if (!apart) continue;
            t.cone = uni(0, 1) < 0.35;
            t.top = groundAt(t.x, t.y) + uni(7, 21);
            t.base = groundAt(t.x, t.y) + (t.cone ? uni(1.5, 3) : uni(3, 7));
            t.r = uni(30, 75); t.g = uni(85, 140); t.b = uni(25, 60);
            trees.push_back(t);
        }
        for (const QPointF c : {QPointF(18, -18), QPointF(-12, -22), QPointF(22, 14)})
            for (int k = 0; k < 4; ++k) {
                Tree t;
                t.x = c.x() + uni(-3.5, 3.5);
                t.y = c.y() + uni(-3.5, 3.5);
                t.radius = uni(2.5, 4.2);
                t.cone = false;
                t.top = groundAt(t.x, t.y) + uni(10, 18);
                t.base = groundAt(t.x, t.y) + uni(4, 7);
                t.r = uni(35, 70); t.g = uni(95, 145); t.b = uni(30, 55);
                trees.push_back(t);
            }
        h.assign(size_t(N) * N, 0);
        id.assign(size_t(N) * N, 0);
        parallel(N, [&](int j) {
            const double y = Half - (j + 0.5) * Cell;
            for (int i = 0; i < N; ++i) {
                const double x = -Half + (i + 0.5) * Cell;
                double z = groundAt(x, y);
                uchar what = 0;
                if (inBuilding(x, y)) { z = Ground + 7; what = 1; }
                for (size_t k = 0; k < trees.size(); ++k) {
                    const Tree &t = trees[k];
                    const double r = std::hypot(x - t.x, y - t.y);
                    if (r >= t.radius) continue;
                    const double q = r / t.radius;
                    double c = t.cone ? t.top - (t.top - t.base) * q : t.base + (t.top - t.base) * std::sqrt(1 - q * q);
                    c += (noise(x, y, 0.9, int(k) + 50) - 0.5) * (t.cone ? 0.5 : 1.2) * (1 - q * q);   // lumps of the crown
                    if (c > z) { z = c; what = uchar(2 + k); }
                }
                h[size_t(j) * N + i] = float(z);
                id[size_t(j) * N + i] = what;
            }
        });
        zmax = *std::max_element(h.begin(), h.end());
    }
    int index(double v) const { return qBound(0, int((v + Half) / Cell), N - 1); }
    int row(double y) const { return qBound(0, int((Half - y) / Cell), N - 1); }
    double height(double x, double y) const   // bilinear between cell centres
    {
        const double fx = qBound(0.0, (x + Half) / Cell - 0.5, N - 1.001), fy = qBound(0.0, (Half - y) / Cell - 0.5, N - 1.001);
        const int i = int(fx), j = int(fy);
        const double ax = fx - i, ay = fy - j;
        const float *r0 = &h[size_t(j) * N + i], *r1 = r0 + N;
        return (r0[0] * (1 - ax) + r0[1] * ax) * (1 - ay) + (r1[0] * (1 - ax) + r1[1] * ax) * ay;
    }
    int what(double x, double y) const { return id[size_t(row(y)) * N + index(x)]; }
    // Colour of the top surface at (x, y): leaves with gaps, shaded by the sun; roof; grass, a
    // path and painted lines (sharp edges).
    QRgb colour(double x, double y) const
    {
        const int w = what(x, y);
        if (w >= 2) {
            const Tree &t = trees[w - 2];
            const double leaf = 0.5 * noise(x, y, 0.10, w) + 0.3 * noise(x, y, 0.27, w + 100) + 0.2 * noise(x, y, 0.7, w + 200);
            double v = leaf < 0.33 ? 0.28 : 0.45 + 0.8 * leaf;   // dark gaps between leaf clusters
            const double dzx = (height(x + 0.06, y) - height(x - 0.06, y)) / 0.12, dzy = (height(x, y + 0.06) - height(x, y - 0.06)) / 0.12;
            const double len = std::sqrt(dzx * dzx + dzy * dzy + 1);
            const double sun = qMax(0.0, (-dzx * -0.45 + -dzy * 0.5 + 0.74) / len);
            v *= 0.5 + 0.55 * sun;
            return qRgb(qBound(0, int(t.r * v), 255), qBound(0, int(t.g * v), 255), qBound(0, int(t.b * v), 255));
        }
        if (w == 1) {
            const bool stripe = int(std::floor((x + 30) / 0.5)) % 2 == 0;
            return stripe ? qRgb(178, 52, 40) : qRgb(220, 205, 190);
        }
        if (x > 4 && x < 28 && y > -32 && y < -27 && std::fmod(x + 100, 1.2) < 0.18) return qRgb(240, 240, 235);   // painted lines
        if (std::fabs(y - 0.35 * x - 3) < 1.1) {   // gravel path
            const double g = noise(x, y, 0.08, 7);
            return qRgb(int(150 + 60 * g), int(135 + 55 * g), int(105 + 45 * g));
        }
        const double g = 0.6 * noise(x, y, 0.09, 3) + 0.4 * noise(x, y, 0.4, 4);
        return qRgb(int(70 + 70 * g), int(100 + 70 * g), int(40 + 40 * g));
    }
    // First surface point along a ray (sphere tracing with a slope bound, then bisection).
    bool trace(const double o[3], const double d[3], double hit[3]) const
    {
        if (d[2] > -1e-6) return false;
        const double horizontal = std::hypot(d[0], d[1]);
        double t = qMax(0.0, (o[2] - (zmax + 0.3)) / -d[2]), previous = t;
        for (int i = 0; i < 3000; ++i) {
            const double x = o[0] + t * d[0], y = o[1] + t * d[1], z = o[2] + t * d[2];
            if (std::fabs(x) > Half - 0.05 || std::fabs(y) > Half - 0.05) return false;
            const double gap = z - height(x, y);
            if (gap < 0.003) {
                if (gap < 0) {
                    double a = previous, b = t;
                    for (int k = 0; k < 30; ++k) {
                        const double m = (a + b) / 2;
                        (o[2] + m * d[2] - height(o[0] + m * d[0], o[1] + m * d[1]) > 0 ? a : b) = m;
                    }
                    t = b;
                }
                for (int k = 0; k < 3; ++k) hit[k] = o[k] + t * d[k];
                return true;
            }
            previous = t;
            t += qMax(0.002, 0.9 * gap / (-d[2] + 12 * horizontal));
        }
        return false;
    }
};

ortho::Shot shotAt(double cx, double cy, double cz, double yaw, double tiltX, double tiltY, const QString &image)
{
    ortho::Shot s;
    s.image = image;
    ortho::Camera &c = s.camera;
    c.width = 1600;
    c.height = 1200;
    c.fx = c.fy = 0.75;
    c.cx = 0.003;
    c.cy = -0.002;
    c.k1 = -0.05;
    c.k2 = 0.01;
    c.p1 = 0.0005;
    c.p2 = -0.0003;
    // Looking down, image up along the heading, tilted by a few degrees.
    const double cyw = std::cos(yaw), syw = std::sin(yaw);
    double R[9] = {cyw, -syw, 0, -syw, -cyw, 0, 0, 0, -1};   // x right, y down in the image
    const double a = tiltX, b = tiltY;
    const double Rx[9] = {1, 0, 0, 0, std::cos(a), -std::sin(a), 0, std::sin(a), std::cos(a)};
    const double Ry[9] = {std::cos(b), 0, std::sin(b), 0, 1, 0, -std::sin(b), 0, std::cos(b)};
    double T[9], U[9];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            T[i * 3 + j] = 0;
            for (int k = 0; k < 3; ++k) T[i * 3 + j] += Rx[i * 3 + k] * Ry[k * 3 + j];
        }
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            U[i * 3 + j] = 0;
            for (int k = 0; k < 3; ++k) U[i * 3 + j] += T[i * 3 + k] * R[k * 3 + j];
        }
    std::copy(U, U + 9, s.R);
    const double C[3] = {cx, cy, cz};
    for (int i = 0; i < 3; ++i) s.t[i] = -(s.R[i * 3] * C[0] + s.R[i * 3 + 1] * C[1] + s.R[i * 3 + 2] * C[2]);
    std::copy(C, C + 3, s.centre);
    return s;
}

struct Point { float x, y, z; uchar r, g, b; };

// A photo of the forest (a ray per pixel) and, every 4th pixel, the point a dense matcher would
// give there: noisy on foliage, often missing on it and never in its dark gaps.
QImage photograph(const Forest &f, const ortho::Shot &s, double gain, int seed, std::vector<Point> *points)
{
    QImage img(s.camera.width, s.camera.height, QImage::Format_RGB32);
    img.fill(Qt::black);
    std::vector<std::vector<Point>> rows(size_t(img.height()));
    parallel(img.height(), [&](int v) {
        QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(v));
        std::mt19937 rng(uint32_t(seed * 100003 + v));
        std::normal_distribution<double> gauss(0, 1);
        for (int u = 0; u < img.width(); ++u) {
            double d[3], hit[3];
            s.ray(u, v, d);
            if (!f.trace(s.centre, d, hit)) continue;
            const QRgb c = f.colour(hit[0], hit[1]);
            line[u] = qRgb(qBound(0, int(qRed(c) * gain + 0.5), 255), qBound(0, int(qGreen(c) * gain + 0.5), 255),
                           qBound(0, int(qBlue(c) * gain + 0.5), 255));
            if (u % 4 || v % 4) continue;
            const bool tree = f.what(hit[0], hit[1]) >= 2;
            if (tree) {
                if (qGray(c) < 45 || rng() % 100 < 40) continue;   // gaps and textureless foliage: no match
            }
            const double sigma = tree ? 0.22 : 0.04, e = gauss(rng) * sigma;
            rows[size_t(v)].push_back({float(hit[0] + e * d[0]), float(hit[1] + e * d[1]), float(hit[2] + e * d[2]), uchar(qRed(c)),
                                       uchar(qGreen(c)), uchar(qBlue(c))});
        }
    });
    for (const auto &r : rows) points->insert(points->end(), r.begin(), r.end());
    return img;
}

// Point cloud in the format of OpenDroneMap's odm_filterpoints/point_cloud.ply.
bool writePly(const QString &path, const std::vector<Point> &points)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QStringLiteral("ply\nformat binary_little_endian 1.0\ncomment Generated by FPCFilter v0.2\nelement vertex %1\n"
                           "property float x\nproperty float y\nproperty float z\nproperty float nx\nproperty float ny\nproperty float nz\n"
                           "property uchar red\nproperty uchar blue\nproperty uchar green\nproperty uchar views\nend_header\n")
                .arg(points.size()).toLatin1());
    QByteArray block;
    for (const Point &p : points) {
        const float v[6] = {p.x, p.y, p.z, 0, 0, 1};
        block.append(reinterpret_cast<const char *>(v), sizeof v);
        const uchar c[4] = {p.r, p.b, p.g, 2};
        block.append(reinterpret_cast<const char *>(c), 4);
        if (block.size() > (1 << 22)) { f.write(block); block.clear(); }
    }
    f.write(block);
    return true;
}

GDALDatasetH createGrid(const QString &path, double cell, double nodata)
{
    const int n = int(std::lround(2 * Half / cell));
    GDALDatasetH ds = GDALCreate(GDALGetDriverByName("GTiff"), QDir::toNativeSeparators(path).toUtf8().constData(), n, n, 1, GDT_Float32, nullptr);
    if (!ds) return nullptr;
    double gt[6] = {OffsetX - Half, cell, 0, OffsetY + Half, 0, -cell};
    GDALSetGeoTransform(ds, gt);
    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    OSRImportFromEPSG(srs, 32637);
    GDALSetSpatialRef(ds, srs);
    OSRDestroySpatialReference(srs);
    GDALSetRasterNoDataValue(GDALGetRasterBand(ds, 1), nodata);
    return ds;
}

// The DSM as photogrammetry grids it: highest point per 5 cm cell, gaps filled by
// interpolation, one 5×5 median pass.
bool writeDsm(const QString &path, const std::vector<Point> &points)
{
    const double cell = 0.05;
    const int n = int(std::lround(2 * Half / cell));
    std::vector<float> z(size_t(n) * n, -9999.0f);
    for (const Point &p : points) {
        const int i = int((p.x + Half) / cell), j = int((Half - p.y) / cell);
        if (i < 0 || j < 0 || i >= n || j >= n) continue;
        float &v = z[size_t(j) * n + i];
        v = qMax(v, p.z);
    }
    GDALDatasetH ds = createGrid(path, cell, -9999);
    if (!ds) return false;
    GDALRasterBandH band = GDALGetRasterBand(ds, 1);
    GDALRasterIO(band, GF_Write, 0, 0, n, n, z.data(), n, n, GDT_Float32, 0, 0);
    GDALFillNodata(band, nullptr, 100, 0, 0, nullptr, nullptr, nullptr);
    GDALRasterIO(band, GF_Read, 0, 0, n, n, z.data(), n, n, GDT_Float32, 0, 0);
    std::vector<float> m(z.size());
    parallel(n, [&](int j) {
        float w[25];
        for (int i = 0; i < n; ++i) {
            int k = 0;
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx) {
                    const int x = qBound(0, i + dx, n - 1), y = qBound(0, j + dy, n - 1);
                    if (z[size_t(y) * n + x] > -9000) w[k++] = z[size_t(y) * n + x];
                }
            std::nth_element(w, w + k / 2, w + k);
            m[size_t(j) * n + i] = k ? w[k / 2] : -9999.0f;
        }
    });
    GDALRasterIO(band, GF_Write, 0, 0, n, n, m.data(), n, n, GDT_Float32, 0, 0);
    GDALClose(ds);
    return true;
}

bool writeDtm(const QString &path)
{
    const double cell = 0.1;
    const int n = int(std::lround(2 * Half / cell));
    std::vector<float> z(size_t(n) * n);
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) z[size_t(j) * n + i] = float(Forest::groundAt(-Half + (i + 0.5) * cell, Half - (j + 0.5) * cell));
    GDALDatasetH ds = createGrid(path, cell, -9999);
    if (!ds) return false;
    GDALRasterIO(GDALGetRasterBand(ds, 1), GF_Write, 0, 0, n, n, z.data(), n, n, GDT_Float32, 0, 0);
    GDALClose(ds);
    return true;
}

struct Zone
{
    double sum = 0;
    qint64 count = 0, gross = 0;
    double gradOut = 0, gradTrue = 0;
    double mean() const { return count ? sum / count : 0; }
    double grossShare() const { return count ? double(gross) / count : 0; }
    double sharpness() const { return gradTrue > 0 ? std::sqrt(gradOut / gradTrue) : 0; }
};
struct Score
{
    Zone crowns, ring, ground, roof;
    QImage image, errors;
};

// Orthophoto against the surface's texture on the same grid (3×3 box blur on both: a
// resampled photo is never sharper than one pixel).
Score score(const Forest &f, const QString &path)
{
    Score s;
    GDALDatasetH ds = GDALOpen(QDir::toNativeSeparators(path).toUtf8().constData(), GA_ReadOnly);
    if (!ds) return s;
    double gt[6];
    GDALGetGeoTransform(ds, gt);
    const int w = GDALGetRasterXSize(ds), h = GDALGetRasterYSize(ds), bands = GDALGetRasterCount(ds);
    std::vector<uchar> rgba(size_t(w) * h * 4, 255);
    int list[4] = {1, 2, 3, 4};
    GDALDatasetRasterIO(ds, GF_Read, 0, 0, w, h, rgba.data(), w, h, GDT_Byte, qMin(bands, 4), list, 4, w * 4, 1);
    GDALClose(ds);
    std::vector<float> out(size_t(w) * h * 3), truth(size_t(w) * h * 3);
    std::vector<uchar> zone(size_t(w) * h, 255), ok(size_t(w) * h, 0);
    // Distance to the nearest crown, for the ring around crowns.
    parallel(h, [&](int y) {
        for (int x = 0; x < w; ++x) {
            const double X = gt[0] + (x + 0.5) * gt[1] - OffsetX, Y = gt[3] + (y + 0.5) * gt[5] - OffsetY;
            double c[3] = {0, 0, 0};
            for (double dy : {-0.25, 0.25})
                for (double dx : {-0.25, 0.25}) {
                    const QRgb q = f.colour(X + dx * gt[1], Y + dy * gt[1]);
                    c[0] += qRed(q) / 4.0; c[1] += qGreen(q) / 4.0; c[2] += qBlue(q) / 4.0;
                }
            const size_t i = size_t(y) * w + x;
            for (int k = 0; k < 3; ++k) {
                truth[i * 3 + k] = float(c[k]);
                out[i * 3 + k] = rgba[i * 4 + k];
            }
            ok[i] = rgba[i * 4 + 3] > 0;
            const int what = f.what(X, Y);
            // Zone: crowns away from their edges, roof, ground within 2.5 m of a crown, open ground.
            bool edge = false, nearTree = false;
            for (int k = 0; k < 16 && !edge; ++k) {
                const double a = k * 3.14159265 / 8;
                edge |= f.what(X + 0.12 * std::cos(a), Y + 0.12 * std::sin(a)) != what;
            }
            if (edge) continue;
            if (what >= 2) zone[i] = 0;
            else if (what == 1) zone[i] = 3;
            else {
                for (int k = 0; k < 24 && !nearTree; ++k) {
                    const double a = k * 3.14159265 / 12;
                    for (double r : {0.5, 1.2, 2.5}) nearTree |= f.what(X + r * std::cos(a), Y + r * std::sin(a)) >= 2;
                }
                zone[i] = nearTree ? 1 : 2;
            }
        }
    });
    auto blurred = [&](const std::vector<float> &v, size_t i, int x, int y, int k) {
        double s = 0;
        int n = 0;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const int xx = qBound(0, x + dx, w - 1), yy = qBound(0, y + dy, h - 1);
                s += v[(size_t(yy) * w + xx) * 3 + k];
                ++n;
            }
        Q_UNUSED(i);
        return s / n;
    };
    s.image = QImage(w, h, QImage::Format_RGB32);
    s.errors = QImage(w, h, QImage::Format_RGB32);
    for (int y = 1; y + 1 < h; ++y)
        for (int x = 1; x + 1 < w; ++x) {
            const size_t i = size_t(y) * w + x;
            s.image.setPixel(x, y, qRgb(int(out[i * 3]), int(out[i * 3 + 1]), int(out[i * 3 + 2])));
            double err = 0;
            for (int k = 0; k < 3; ++k) err += std::fabs(blurred(out, i, x, y, k) - blurred(truth, i, x, y, k)) / 3;
            if (!ok[i]) err = 255;
            const int e = qMin(255, int(err * 4));
            s.errors.setPixel(x, y, zone[i] == 255 ? qRgb(30, 30, 30) : qRgb(e, e, e));
            if (zone[i] == 255) continue;
            Zone &z = zone[i] == 0 ? s.crowns : zone[i] == 1 ? s.ring : zone[i] == 2 ? s.ground : s.roof;
            z.sum += err;
            ++z.count;
            z.gross += err > 35;
            auto lum = [&](const std::vector<float> &v, size_t j) { return 0.3 * v[j * 3] + 0.59 * v[j * 3 + 1] + 0.11 * v[j * 3 + 2]; };
            const double go = lum(out, i + 1) - lum(out, i - 1), gt2 = lum(truth, i + 1) - lum(truth, i - 1);
            z.gradOut += go * go;
            z.gradTrue += gt2 * gt2;
        }
    return s;
}

// A classic orthophoto for comparison: every pixel from the most vertical photo at the height
// of a smoothed DSM (a decimated mesh), without a visibility check.
bool classicOrtho(const ortho::Scene &scene, const QString &dsmPath, const QString &out, double resolution)
{
    GDALDatasetH ds = GDALOpen(QDir::toNativeSeparators(dsmPath).toUtf8().constData(), GA_ReadOnly);
    if (!ds) return false;
    double gt[6];
    GDALGetGeoTransform(ds, gt);
    const int dw = GDALGetRasterXSize(ds), dh = GDALGetRasterYSize(ds);
    std::vector<float> z(size_t(dw) * dh), sm(z.size());
    GDALRasterIO(GDALGetRasterBand(ds, 1), GF_Read, 0, 0, dw, dh, z.data(), dw, dh, GDT_Float32, 0, 0);
    GDALClose(ds);
    const int r = 10;   // 0.5 m box smoothing
    parallel(dh, [&](int j) {
        for (int i = 0; i < dw; ++i) {
            double s = 0;
            int n = 0;
            for (int dy = -r; dy <= r; dy += 2)
                for (int dx = -r; dx <= r; dx += 2) {
                    const int x = qBound(0, i + dx, dw - 1), y = qBound(0, j + dy, dh - 1);
                    s += z[size_t(y) * dw + x];
                    ++n;
                }
            sm[size_t(j) * dw + i] = float(s / n);
        }
    });
    std::vector<QImage> photos;
    for (const ortho::Shot &s : scene.shots) photos.push_back(QImage(s.image).convertToFormat(QImage::Format_RGB32));
    const int w = int(std::ceil(dw * gt[1] / resolution)), h = int(std::ceil(dh * -gt[5] / resolution));
    std::vector<uchar> rgb(size_t(w) * h * 3, 0);
    parallel(h, [&](int y) {
        for (int x = 0; x < w; ++x) {
            const double X = gt[0] + (x + 0.5) * resolution, Y = gt[3] - (y + 0.5) * resolution;
            const int i = qBound(0, int((X - gt[0]) / gt[1]), dw - 1), j = qBound(0, int((Y - gt[3]) / gt[5]), dh - 1);
            const double P[3] = {X - scene.offsetX, Y - scene.offsetY, sm[size_t(j) * dw + i]};
            int best = -1;
            double bestD = 1e300, bu = 0, bv = 0;
            for (int s = 0; s < scene.shots.size(); ++s) {
                double u, v, d;
                const ortho::Shot &shot = scene.shots[s];
                if (!shot.project(P, &u, &v, &d) || u < 0 || v < 0 || u > shot.camera.width - 2 || v > shot.camera.height - 2) continue;
                const double off = std::hypot(shot.centre[0] - P[0], shot.centre[1] - P[1]);
                if (off < bestD) { bestD = off; best = s; bu = u; bv = v; }
            }
            if (best < 0) continue;
            const QImage &img = photos[size_t(best)];
            const int u0 = int(bu), v0 = int(bv);
            const double ax = bu - u0, ay = bv - v0;
            const QRgb a = img.pixel(u0, v0), b = img.pixel(u0 + 1, v0), c = img.pixel(u0, v0 + 1), d = img.pixel(u0 + 1, v0 + 1);
            auto mixc = [&](int (*ch)(QRgb)) { return (ch(a) * (1 - ax) + ch(b) * ax) * (1 - ay) + (ch(c) * (1 - ax) + ch(d) * ax) * ay; };
            uchar *p = &rgb[(size_t(y) * w + x) * 3];
            p[0] = uchar(mixc(qRed) + 0.5);
            p[1] = uchar(mixc(qGreen) + 0.5);
            p[2] = uchar(mixc(qBlue) + 0.5);
        }
    });
    GDALDatasetH o = GDALCreate(GDALGetDriverByName("GTiff"), QDir::toNativeSeparators(out).toUtf8().constData(), w, h, 3, GDT_Byte, nullptr);
    if (!o) return false;
    double ogt[6] = {gt[0], resolution, 0, gt[3], 0, -resolution};
    GDALSetGeoTransform(o, ogt);
    int bands[3] = {1, 2, 3};
    GDALDatasetRasterIO(o, GF_Write, 0, 0, w, h, rgb.data(), w, h, GDT_Byte, 3, bands, 3, w * 3, 1);
    GDALClose(o);
    return true;
}

QString report(const char *name, const Score &s)
{
    auto zone = [](const Zone &z) {
        return QStringLiteral("%1 (gross %2 %, sharpness %3)").arg(z.mean(), 0, 'f', 1).arg(100 * z.grossShare(), 0, 'f', 2).arg(z.sharpness(), 0, 'f', 2);
    };
    return QStringLiteral("%1: crowns %2; ground next to crowns %3; open ground %4; roof %5")
        .arg(QString::fromLatin1(name), zone(s.crowns), zone(s.ring), zone(s.ground), zone(s.roof));
}
} // namespace

class TreesTest : public QObject
{
    Q_OBJECT
    QTemporaryDir m_dir;
    Forest m_forest;
    ortho::Scene m_scene;
    QString m_dsm, m_dtm, m_ply, m_out;

private slots:
    void initTestCase()
    {
        QVERIFY(m_dir.isValid());
        QVERIFY2(GisEnvironment::initialize(QCoreApplication::applicationDirPath()), qPrintable(GisEnvironment::problems().join('\n')));
        m_out = qEnvironmentVariable("GEOMAP_TEST_OUTPUT");
        if (!m_out.isEmpty()) QDir().mkpath(m_out);
        QElapsedTimer timer;
        timer.start();
        m_forest.build();
        qInfo("Forest: %d trees, surface %dx%d, %lld ms", int(m_forest.trees.size()), N, N, timer.elapsed());
        // 3 strips of 7 photos 50 m above the ground: 80 % forward, 70 % side overlap.
        m_scene.offsetX = OffsetX;
        m_scene.offsetY = OffsetY;
        std::vector<Point> points;
        std::mt19937 rng(7);
        int k = 0;
        timer.restart();
        for (int strip = 0; strip < 3; ++strip)
            for (int i = 0; i < 7; ++i, ++k) {
                const double x = -20 + 20 * strip, y = (strip % 2 ? 30 - 10 * i : -30 + 10 * i);
                const double yaw = strip % 2 ? 3.14159265358979 : 0;
                const double tx = ((rng() % 1000) / 1000.0 - 0.5) * 0.07, ty = ((rng() % 1000) / 1000.0 - 0.5) * 0.07;
                const QString path = m_dir.filePath(QStringLiteral("DJI_%1.JPG").arg(k + 1, 4, 10, QLatin1Char('0')));
                ortho::Shot s = shotAt(x, y, Ground + 50, yaw, tx, ty, path);
                const double gain = 0.92 + 0.16 * ((rng() % 1000) / 1000.0);
                QVERIFY(photograph(m_forest, s, gain, k, &points).save(path, "JPG", 92));
                m_scene.shots << s;
            }
        qInfo("21 photos and %zu points in %lld ms", points.size(), timer.elapsed());
        m_dsm = m_dir.filePath(QStringLiteral("dsm.tif"));
        m_dtm = m_dir.filePath(QStringLiteral("dtm.tif"));
        m_ply = m_dir.filePath(QStringLiteral("point_cloud.ply"));
        QVERIFY(writeDsm(m_dsm, points));
        QVERIFY(writeDtm(m_dtm));
        QVERIFY(writePly(m_ply, points));
    }

    // The true orthophoto from the photogrammetric DSM (and the point cloud) against the classic one.
    void forest()
    {
        const QString out = m_dir.filePath(QStringLiteral("true_ortho.tif")), classic = m_dir.filePath(QStringLiteral("classic.tif"));
        ortho::Options options;
        options.groundPath = m_dtm;
        options.pointCloudPath = m_ply;
        options.stretchMaskPath = m_dir.filePath(QStringLiteral("true_stretch.tif"));   // as the program builds it: streaks re-sourced
        const ortho::Result r = ortho::build(m_scene, m_dsm, out, options);
        QVERIFY2(r.ok(), qPrintable(r.error));
        qInfo("True orthophoto %dx%d at %.3f m, %d objects, coverage %.4f, hidden %.4f, %.1f s", r.width, r.height, r.resolution, r.objects,
              r.coverage, r.hidden, r.seconds);
        QVERIFY(classicOrtho(m_scene, m_dsm, classic, r.resolution));
        const Score ours = score(m_forest, out), base = score(m_forest, classic);
        qInfo("%s", qUtf8Printable(report("true", ours)));
        qInfo("%s", qUtf8Printable(report("classic", base)));
        if (!m_out.isEmpty()) {
            ours.image.save(QDir(m_out).filePath(QStringLiteral("trees-true.png")));
            ours.errors.save(QDir(m_out).filePath(QStringLiteral("trees-true-errors.png")));
            base.image.save(QDir(m_out).filePath(QStringLiteral("trees-classic.png")));
            base.errors.save(QDir(m_out).filePath(QStringLiteral("trees-classic-errors.png")));
            QImage truth(ours.image.size(), QImage::Format_RGB32);
            double gt[6] = {OffsetX - Half, r.resolution, 0, OffsetY + Half, 0, -r.resolution};
            for (int y = 0; y < truth.height(); ++y)
                for (int x = 0; x < truth.width(); ++x)
                    truth.setPixel(x, y, m_forest.colour(gt[0] + (x + 0.5) * gt[1] - OffsetX, gt[3] + (y + 0.5) * gt[5] - OffsetY));
            truth.save(QDir(m_out).filePath(QStringLiteral("trees-truth.png")));
        }
        QVERIFY(r.coverage > 0.99);
        const QString why = report("true", ours) + QStringLiteral(" | ") + report("classic", base);
        // Crowns whole, sharp and in place; no copies of crowns ("ghosts") on the ground next to
        // them — the classic orthophoto leaves twice as many there or more; the roof not smeared.
        // (Its crowns are not compared on the mean: it cuts them along the photo edges and takes
        // every piece from its most vertical photo, which a mean error does not see.)
        QVERIFY2(ours.crowns.grossShare() < 0.02, qUtf8Printable(why));
        QVERIFY2(ours.crowns.sharpness() > 0.9, qUtf8Printable(why));
        QVERIFY2(ours.ring.grossShare() < 0.025 && ours.ring.grossShare() < base.ring.grossShare() / 2, qUtf8Printable(why));
        QVERIFY2(ours.ring.mean() < base.ring.mean(), qUtf8Printable(why));
        QVERIFY2(ours.ground.grossShare() < 0.01, qUtf8Printable(why));
        QVERIFY2(ours.roof.grossShare() < 0.015 && ours.roof.mean() < base.roof.mean(), qUtf8Printable(why));
    }
};

QTEST_MAIN(TreesTest)
#include "test_trees.moc"
