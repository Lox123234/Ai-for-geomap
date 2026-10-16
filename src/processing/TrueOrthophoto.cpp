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
#include <cstring>
#include <deque>
#include <limits>
#include <list>
#include <map>
#include <thread>
#include <unordered_map>
#include <cpl_string.h>
#include <gdal.h>
#include <gdal_alg.h>
#include <gdal_utils.h>
#include <ogr_srs_api.h>

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

void parallelFor(int count, const std::function<void(int)> &body);

// Separable running minimum / maximum (monotonic queue), window 2r+1, NaN ignored; cells
// without a value stay empty unless `spread`.
void runningFilter(std::vector<float> &v, int w, int h, int r, bool minimum, bool spread = false)
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
            for (int i = 0; i < length; ++i) v[index(k, i)] = std::isnan(line[i]) && !spread ? NaN : out[i];
        }
    };
    pass(h, w, [w](int y, int x) { return size_t(y) * w + x; });
    pass(w, h, [w](int x, int y) { return size_t(y) * w + x; });
}

// Highest point of a dense point cloud (binary little-endian PLY, local frame as ODM writes
// odm_filterpoints/point_cloud.ply) in every cell of `grid`; NaN where no point fell.
bool gridPointCloud(const QString &path, const Grid &grid, double offsetX, double offsetY, std::vector<float> *top, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Не открыть облако точек %1").arg(QDir::toNativeSeparators(path));
        return false;
    }
    qint64 count = -1;
    bool binary = false, inVertex = false;
    int stride = 0, offset[3] = {-1, -1, -1}, size[3] = {0, 0, 0};
    for (;;) {
        const QByteArray line = f.readLine().trimmed();
        if (line.isEmpty() && f.atEnd()) break;
        if (line == "end_header") break;
        const QList<QByteArray> p = line.split(' ');
        if (p[0] == "format") {
            binary = p.value(1) == "binary_little_endian";
        } else if (p[0] == "element") {
            if (count >= 0 && !inVertex) break;
            inVertex = p.value(1) == "vertex";
            if (inVertex) count = p.value(2).toLongLong();
            else if (count < 0) {   // another element before the vertices: offsets unknown
                *error = QStringLiteral("Облако точек: неподдерживаемая структура PLY");
                return false;
            }
        } else if (p[0] == "property" && inVertex) {
            const QByteArray type = p.value(1);
            const int bytes = type == "char" || type == "uchar" || type == "int8" || type == "uint8"         ? 1
                            : type == "short" || type == "ushort" || type == "int16" || type == "uint16"     ? 2
                            : type == "int" || type == "uint" || type == "float" || type == "int32" || type == "uint32" || type == "float32" ? 4
                            : type == "double" || type == "float64"                                          ? 8
                                                                                                             : 0;
            if (!bytes || type == "list") {
                *error = QStringLiteral("Облако точек: неподдерживаемое свойство PLY «%1»").arg(QString::fromLatin1(line));
                return false;
            }
            const QByteArray name = p.value(2);
            const int k = name == "x" ? 0 : name == "y" ? 1 : name == "z" ? 2 : -1;
            if (k >= 0 && (type.startsWith("float") || type == "double")) {
                offset[k] = stride;
                size[k] = bytes;
            }
            stride += bytes;
        }
    }
    if (!binary || count <= 0 || offset[0] < 0 || offset[1] < 0 || offset[2] < 0) {
        *error = QStringLiteral("Облако точек: нужен двоичный PLY с координатами x, y, z");
        return false;
    }
    top->assign(size_t(grid.w) * grid.h, NaN);
    auto value = [&](const char *p, int k) {
        if (size[k] == 8) {
            double d;
            std::memcpy(&d, p + offset[k], 8);
            return d;
        }
        float v;
        std::memcpy(&v, p + offset[k], 4);
        return double(v);
    };
    const qint64 chunk = 1 << 16;
    for (qint64 done = 0; done < count;) {
        const qint64 n = qMin(chunk, count - done);
        const QByteArray block = f.read(n * stride);
        if (block.size() < n * stride) {
            *error = QStringLiteral("Облако точек обрывается: прочитано %1 из %2 точек").arg(done).arg(count);
            return false;
        }
        for (qint64 i = 0; i < n; ++i) {
            const char *p = block.constData() + i * stride;
            const double z = value(p, 2);
            const double gx = (value(p, 0) + offsetX - grid.gt[0]) / grid.gt[1], gy = (value(p, 1) + offsetY - grid.gt[3]) / grid.gt[5];
            if (!(gx >= 0 && gy >= 0 && gx < grid.w && gy < grid.h) || !std::isfinite(z)) continue;
            float &t = (*top)[size_t(int(gy)) * grid.w + int(gx)];
            if (std::isnan(t) || z > t) t = float(z);
        }
        done += n;
    }
    return true;
}

// Empty cells take the value of the nearest cell with one (two-pass chamfer propagation) up to
// `maxDistance` cells away: edges of roofs and crowns stay steps instead of interpolated ramps.
void nearestFill(std::vector<float> &v, int w, int h, float maxDistance)
{
    std::vector<float> dist(v.size());
    for (size_t i = 0; i < v.size(); ++i) dist[i] = std::isnan(v[i]) ? std::numeric_limits<float>::infinity() : 0.0f;
    const float diagonal = 1.41421356f;
    auto relax = [&](size_t p, int x, int y, int dx, int dy, float cost) {
        const int nx = x + dx, ny = y + dy;
        if (nx < 0 || ny < 0 || nx >= w || ny >= h) return;
        const size_t q = size_t(ny) * w + nx;
        if (dist[q] + cost < dist[p]) {
            dist[p] = dist[q] + cost;
            v[p] = v[q];
        }
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t p = size_t(y) * w + x;
            relax(p, x, y, -1, 0, 1);
            relax(p, x, y, 0, -1, 1);
            relax(p, x, y, -1, -1, diagonal);
            relax(p, x, y, 1, -1, diagonal);
        }
    for (int y = h - 1; y >= 0; --y)
        for (int x = w - 1; x >= 0; --x) {
            const size_t p = size_t(y) * w + x;
            relax(p, x, y, 1, 0, 1);
            relax(p, x, y, 0, 1, 1);
            relax(p, x, y, 1, 1, diagonal);
            relax(p, x, y, -1, 1, diagonal);
        }
    for (size_t i = 0; i < v.size(); ++i)
        if (dist[i] > maxDistance) v[i] = NaN;
}

// Cells without a label (−1) take the label of the nearest labelled cell up to `maxDistance`
// cells away (same propagation as nearestFill).
void nearestLabel(std::vector<int> &v, int w, int h, float maxDistance)
{
    std::vector<float> dist(v.size());
    for (size_t i = 0; i < v.size(); ++i) dist[i] = v[i] < 0 ? std::numeric_limits<float>::infinity() : 0.0f;
    const float diagonal = 1.41421356f;
    auto relax = [&](size_t p, int x, int y, int dx, int dy, float cost) {
        const int nx = x + dx, ny = y + dy;
        if (nx < 0 || ny < 0 || nx >= w || ny >= h) return;
        const size_t q = size_t(ny) * w + nx;
        if (dist[q] + cost < dist[p]) {
            dist[p] = dist[q] + cost;
            v[p] = v[q];
        }
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t p = size_t(y) * w + x;
            relax(p, x, y, -1, 0, 1);
            relax(p, x, y, 0, -1, 1);
            relax(p, x, y, -1, -1, diagonal);
            relax(p, x, y, 1, -1, diagonal);
        }
    for (int y = h - 1; y >= 0; --y)
        for (int x = w - 1; x >= 0; --x) {
            const size_t p = size_t(y) * w + x;
            relax(p, x, y, 1, 0, 1);
            relax(p, x, y, 0, 1, 1);
            relax(p, x, y, 1, 1, diagonal);
            relax(p, x, y, -1, 1, diagonal);
        }
    for (size_t i = 0; i < v.size(); ++i)
        if (dist[i] > maxDistance) v[i] = -1;
}

// Distance of every cell of `mask` to the nearest cell outside it (cells, chamfer 1/√2).
std::vector<float> distanceInside(const std::vector<uchar> &mask, int w, int h)
{
    std::vector<float> d(mask.size());
    for (size_t i = 0; i < mask.size(); ++i) d[i] = mask[i] ? std::numeric_limits<float>::infinity() : 0.0f;
    const float diagonal = 1.41421356f;
    auto relax = [&](size_t p, int x, int y, int dx, int dy, float cost) {
        const int nx = x + dx, ny = y + dy;
        const float q = nx < 0 || ny < 0 || nx >= w || ny >= h ? 0.0f : d[size_t(ny) * w + nx];
        if (q + cost < d[p]) d[p] = q + cost;
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t p = size_t(y) * w + x;
            if (!mask[p]) continue;
            relax(p, x, y, -1, 0, 1);
            relax(p, x, y, 0, -1, 1);
            relax(p, x, y, -1, -1, diagonal);
            relax(p, x, y, 1, -1, diagonal);
        }
    for (int y = h - 1; y >= 0; --y)
        for (int x = w - 1; x >= 0; --x) {
            const size_t p = size_t(y) * w + x;
            if (!mask[p]) continue;
            relax(p, x, y, 1, 0, 1);
            relax(p, x, y, 0, 1, 1);
            relax(p, x, y, 1, 1, diagonal);
            relax(p, x, y, -1, 1, diagonal);
        }
    return d;
}

// 3×3 median of the cells with a value (single spikes of the matching removed, edges kept).
void median3(std::vector<float> &v, int w, int h)
{
    std::vector<float> out = v;
    parallelFor(h, [&](int y) {
        float window[9];
        for (int x = 0; x < w; ++x) {
            if (std::isnan(v[size_t(y) * w + x])) continue;
            int n = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int xx = x + dx, yy = y + dy;
                    if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                    const float z = v[size_t(yy) * w + xx];
                    if (!std::isnan(z)) window[n++] = z;
                }
            std::nth_element(window, window + n / 2, window + n);
            out[size_t(y) * w + x] = window[n / 2];
        }
    });
    v.swap(out);
}

// Mean over the cells of `mask` in a (2r+1)² box, twice (≈ Gaussian), for the cells of the mask
// only: smoothing never mixes a crown with the ground next to it.
void maskedSmooth(std::vector<float> &v, const std::vector<uchar> &mask, int w, int h, int r)
{
    std::vector<double> sv(v.size()), sm(v.size());
    for (size_t i = 0; i < v.size(); ++i) {
        const bool m = mask[i] && !std::isnan(v[i]);
        sv[i] = m ? v[i] : 0.0;
        sm[i] = m ? 1.0 : 0.0;
    }
    auto box = [&](std::vector<double> &a) {
        std::vector<double> tmp(a.size());
        parallelFor(h, [&](int y) {   // rows
            double s = 0;
            const double *row = &a[size_t(y) * w];
            for (int x = -r; x <= r; ++x) s += row[qBound(0, x, w - 1)];
            for (int x = 0; x < w; ++x) {
                tmp[size_t(y) * w + x] = s;
                s += row[qMin(w - 1, x + r + 1)] - row[qMax(0, x - r)];
            }
        });
        parallelFor(w, [&](int x) {   // columns
            double s = 0;
            for (int y = -r; y <= r; ++y) s += tmp[size_t(qBound(0, y, h - 1)) * w + x];
            for (int y = 0; y < h; ++y) {
                a[size_t(y) * w + x] = s;
                s += tmp[size_t(qMin(h - 1, y + r + 1)) * w + x] - tmp[size_t(qMax(0, y - r)) * w + x];
            }
        });
    };
    for (int pass = 0; pass < 2; ++pass) {
        box(sv);
        box(sm);
    }
    for (size_t i = 0; i < v.size(); ++i)
        if (mask[i] && sm[i] > 1e-9) v[i] = float(sv[i] / sm[i]);
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

// Catmull-Rom bicubic sample: keeps the fine texture of foliage that bilinear sampling blurs.
inline void sampleCubic(const QImage &img, double px, double py, double rgb[3])
{
    const int w = img.width(), h = img.height();
    px = qBound(0.0, px, w - 1.0);
    py = qBound(0.0, py, h - 1.0);
    const int x0 = int(std::floor(px)), y0 = int(std::floor(py));
    const double ax = px - x0, ay = py - y0;
    auto weights = [](double t, double k[4]) {
        const double t2 = t * t, t3 = t2 * t;
        k[0] = -0.5 * t3 + t2 - 0.5 * t;
        k[1] = 1.5 * t3 - 2.5 * t2 + 1;
        k[2] = -1.5 * t3 + 2 * t2 + 0.5 * t;
        k[3] = 0.5 * t3 - 0.5 * t2;
    };
    double wx[4], wy[4];
    weights(ax, wx);
    weights(ay, wy);
    double r = 0, g = 0, b = 0;
    for (int j = 0; j < 4; ++j) {
        const QRgb *line = reinterpret_cast<const QRgb *>(img.constScanLine(qBound(0, y0 - 1 + j, h - 1)));
        double lr = 0, lg = 0, lb = 0;
        for (int i = 0; i < 4; ++i) {
            const QRgb c = line[qBound(0, x0 - 1 + i, w - 1)];
            lr += wx[i] * qRed(c);
            lg += wx[i] * qGreen(c);
            lb += wx[i] * qBlue(c);
        }
        r += wy[j] * lr;
        g += wy[j] * lg;
        b += wy[j] * lb;
    }
    rgb[0] = r;
    rgb[1] = g;
    rgb[2] = b;
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

QString resultCrsWkt(const QString &job)
{
    for (const char *rel : {"odm_dem/dsm.tif", "odm_orthophoto/odm_orthophoto.tif", "odm_dem/dtm.tif"}) {
        const QString path = QDir(job).filePath(QString::fromLatin1(rel));
        if (!QFileInfo::exists(path)) continue;
        GDALDatasetH ds = GDALOpenEx(native(path).constData(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
        if (!ds) continue;
        const QString wkt = QString::fromUtf8(GDALGetProjectionRef(ds));
        GDALClose(ds);
        if (!wkt.isEmpty()) return wkt;
    }
    QFile coords(QDir(job).filePath(QStringLiteral("odm_georeferencing/coords.txt")));
    if (!coords.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    const QString first = QString::fromUtf8(coords.readLine()).trimmed();
    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    const QStringList parts = first.split(QLatin1Char(' '), Qt::SkipEmptyParts);   // WGS84 UTM 32N
    bool ok = false;
    if (parts.size() == 3 && parts[1].compare(QLatin1String("UTM"), Qt::CaseInsensitive) == 0) {
        const int zone = parts[2].left(parts[2].size() - 1).toInt();
        const bool south = parts[2].endsWith(QLatin1Char('S'), Qt::CaseInsensitive);
        ok = zone >= 1 && zone <= 60 && OSRImportFromEPSG(srs, (south ? 32700 : 32600) + zone) == OGRERR_NONE;
    } else {
        ok = OSRSetFromUserInput(srs, first.toUtf8().constData()) == OGRERR_NONE;
    }
    QString wkt;
    char *text = nullptr;
    if (ok && OSRExportToWkt(srs, &text) == OGRERR_NONE && text) wkt = QString::fromUtf8(text);
    CPLFree(text);
    OSRDestroySpatialReference(srs);
    return wkt;
}

void Shot::ray(double px, double py, double dir[3]) const
{
    const Camera &c = camera;
    const double size = qMax(c.width, c.height);
    const double xd = ((px + 0.5 - c.width / 2.0) / size - c.cx) / c.fx;
    const double yd = ((py + 0.5 - c.height / 2.0) / size - c.cy) / c.fy;
    double xn = xd, yn = yd;
    for (int i = 0; i < 30; ++i) {   // the Brown distortion inverted by fixed-point iteration
        const double r2 = xn * xn + yn * yn;
        const double radial = 1 + r2 * (c.k1 + r2 * (c.k2 + r2 * c.k3));
        const double dx = 2 * c.p1 * xn * yn + c.p2 * (r2 + 2 * xn * xn);
        const double dy = c.p1 * (r2 + 2 * yn * yn) + 2 * c.p2 * xn * yn;
        xn = (xd - dx) / radial;
        yn = (yd - dy) / radial;
    }
    double len = 0;
    for (int k = 0; k < 3; ++k) {
        dir[k] = R[0 + k] * xn + R[3 + k] * yn + R[6 + k];
        len += dir[k] * dir[k];
    }
    len = std::sqrt(len);
    for (int k = 0; k < 3; ++k) dir[k] /= len;
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
    // --- The surface from the dense point cloud: the highest point of every cell, empty cells
    // from the nearest point. Gridded DSMs interpolate across the edges of crowns and roofs
    // (ramps that smear their edges over the ground); here an edge stays a step.
    std::vector<uchar> fromPoints;   // cells measured by the point cloud (not filled by the DSM's gridding)
    if (!opt.pointCloudPath.isEmpty()) {
        std::vector<float> top;
        QString pointError;
        if (gridPointCloud(opt.pointCloudPath, dsm, scene.offsetX, scene.offsetY, &top, &pointError)) {
            nearestFill(top, dsm.w, dsm.h, float(qMax(2.0, 0.6 / cell)));
            median3(top, dsm.w, dsm.h);
            qint64 used = 0;
            fromPoints.assign(top.size(), 0);
            // Only within the DSM's own area: stray points far outside the survey would add blobs.
            for (size_t i = 0; i < top.size(); ++i)
                if (!std::isnan(top[i]) && valid[i]) {
                    dsm.v[i] = top[i];
                    fromPoints[i] = 1;
                    ++used;
                }
            res.pointCloudShare = top.empty() ? 0 : double(used) / top.size();
        } else {
            res.notes << pointError;
        }
    }
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
    std::vector<uchar> high(dsm.v.size(), 0);
    for (size_t i = 0; i < high.size(); ++i)
        high[i] = !std::isnan(dsm.v[i]) && !std::isnan(ground[i]) && dsm.v[i] - ground[i] > opt.objectHeight;
    {
        // Outlines without the noise of the reconstruction: closing (0.5 m: gaps in a canopy
        // narrower than a metre belong to it and are taken from the same photo as the crowns
        // around them, not as patches from another one), then opening (0.1 m), so a crown's edge
        // is a smooth curve rather than a fringe of single cells.
        const float close = float(0.5 / cell);   // round, by distances: no square gaps
        std::vector<uchar> outside(high.size());
        for (size_t i = 0; i < high.size(); ++i) outside[i] = !high[i];
        const std::vector<float> toObject = distanceInside(outside, dsm.w, dsm.h);
        std::vector<uchar> dilated(high.size());
        for (size_t i = 0; i < high.size(); ++i) dilated[i] = high[i] || toObject[i] <= close;
        const std::vector<float> toOutside = distanceInside(dilated, dsm.w, dsm.h);
        std::vector<float> m(high.size());
        for (size_t i = 0; i < m.size(); ++i) m[i] = high[i] || (dilated[i] && toOutside[i] > close) ? 1.0f : 0.0f;
        const int r = qMax(1, int(std::lround(0.1 / cell)));
        runningFilter(m, dsm.w, dsm.h, r, true);
        runningFilter(m, dsm.w, dsm.h, r, false);
        for (size_t i = 0; i < m.size(); ++i) high[i] = m[i] > 0.5f && !std::isnan(dsm.v[i]);
    }
    {
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

    // --- Heights for projecting the photos: smoothed inside the objects and over the ground
    // separately. Noise of the reconstruction on foliage would tear the texture of a crown into
    // shifted pieces; smoothing within each kind keeps the edges between them sharp.
    // The ground is tested against the unsmoothed surface (highest points: crowns as large as
    // the reconstruction saw them) with a margin in the photo (see the z-buffers below).
    const std::vector<float> occluder = dsm.v;
    // Objects smoothed finely (their tops and valleys for the crowns below) and how rough they are:
    // a roof is smooth, foliage scattered by the reconstruction by decimetres to a metre.
    // Canopies (connected objects larger than a few crowns) are projected over their surface
    // smoothed at 2.5 m: there the reconstruction cannot tell crowns apart, and a smooth surface
    // makes every piece a gentle warp of its photo, so neighbouring photos meet without steps.
    std::vector<float> crowns, rough(dsm.v.size(), 0.0f), canopy, canopyWeight(dsm.v.size(), 0.0f);
    {
        std::vector<qint64> componentCells(res.objects, 0);
        for (int o : object)
            if (o >= 0) ++componentCells[o];
        for (size_t i = 0; i < object.size(); ++i)
            if (object[i] >= 0) canopyWeight[i] = qBound(0.0f, float((componentCells[object[i]] * cell * cell - 100.0) / 200.0), 1.0f);
        std::vector<uchar> low(high.size());
        for (size_t i = 0; i < low.size(); ++i) low[i] = !high[i];
        canopy = dsm.v;
        maskedSmooth(canopy, high, dsm.w, dsm.h, qMax(1, int(std::lround(2.5 / cell))));
        crowns = dsm.v;
        maskedSmooth(crowns, high, dsm.w, dsm.h, qMax(1, int(std::lround(0.3 / cell))));
        for (size_t i = 0; i < rough.size(); ++i)
            if (high[i] && !std::isnan(dsm.v[i]) && !std::isnan(crowns[i])) rough[i] = std::fabs(dsm.v[i] - crowns[i]);
        maskedSmooth(rough, high, dsm.w, dsm.h, qMax(1, int(std::lround(1.0 / cell))));
        maskedSmooth(dsm.v, low, dsm.w, dsm.h, qMax(1, int(std::lround(0.12 / cell))));
        for (size_t i = 0; i < crowns.size(); ++i)
            if (!high[i]) crowns[i] = dsm.v[i];
    }
    // --- Crowns: a connected canopy split along its valleys (watershed of the height above the
    // ground from its tops; tops less than 1 m above the valley to a higher neighbour and pieces
    // under 1 m² are merged). Each piece is later taken whole from one photo, so seams run
    // between crowns, where they are not seen, instead of through them.
    {
        std::vector<int> order;
        order.reserve(object.size() / 4);
        std::vector<float> chm(object.size(), NaN);
        for (size_t i = 0; i < object.size(); ++i)
            if (object[i] >= 0 && !std::isnan(crowns[i])) {
                // In a canopy the tops of its smooth surface (spurious tops of the scatter would cut
                // it into pieces with every photo switch visible), single trees their own. Heights
                // themselves, not above the ground: a DTM under a forest is guessed, often in
                // blocks, and its steps would become straight seams.
                chm[i] = high[i] && !std::isnan(canopy[i]) ? crowns[i] + (canopy[i] - crowns[i]) * canopyWeight[i] : crowns[i];
                order.push_back(int(i));
            }
        std::sort(order.begin(), order.end(), [&chm](int a, int b) { return chm[a] > chm[b]; });
        std::vector<int> label(object.size(), -1);
        std::vector<float> peak;
        std::vector<qint64> area;
        std::unordered_map<quint64, float> saddle;   // pair of labels → highest cell between them
        for (int c : order) {
            const int x = c % dsm.w, y = c / dsm.w;
            int best = -1;
            int around[8];
            int count = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    if (!dx && !dy) continue;
                    const int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= dsm.w || ny >= dsm.h) continue;
                    const int l = label[size_t(ny) * dsm.w + nx];
                    if (l < 0) continue;
                    around[count++] = l;
                    if (best < 0 || peak[l] > peak[best]) best = l;
                }
            if (best < 0) {
                best = int(peak.size());
                peak.push_back(chm[c]);
                area.push_back(0);
            }
            label[c] = best;
            ++area[best];
            for (int k = 0; k < count; ++k)
                if (around[k] != best) {
                    const quint64 key = (quint64(qMin(best, around[k])) << 32) | quint64(qMax(best, around[k]));
                    auto it = saddle.find(key);
                    if (it == saddle.end()) saddle.emplace(key, chm[c]);
                    else it->second = qMax(it->second, chm[c]);
                }
        }
        // Merging from the highest valleys down (union-find): a top less than 1 m above the
        // valley joins its neighbour, as does a piece smaller than 1 m².
        std::vector<int> parent(peak.size());
        for (size_t i = 0; i < parent.size(); ++i) parent[i] = int(i);
        std::function<int(int)> root = [&](int a) { return parent[a] == a ? a : parent[a] = root(parent[a]); };
        std::vector<std::pair<float, quint64>> pairs;
        pairs.reserve(saddle.size());
        for (const auto &s : saddle) pairs.emplace_back(s.second, s.first);
        std::sort(pairs.begin(), pairs.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
        const qint64 minArea = qMax<qint64>(4, qint64(1.0 / (cell * cell)));
        for (const auto &p : pairs) {
            const int a = root(int(p.second >> 32)), b = root(int(p.second & 0xffffffffu));
            if (a == b) continue;
            if (qMin(peak[a], peak[b]) - p.first < 1.0f || area[a] < minArea || area[b] < minArea) {
                const int keep = peak[a] >= peak[b] ? a : b, gone = keep == a ? b : a;
                parent[gone] = keep;
                area[keep] += area[gone];
            }
        }
        std::vector<int> compact(peak.size(), -1);
        int pieces = 0;
        for (size_t i = 0; i < object.size(); ++i) {
            if (object[i] < 0) continue;
            if (label[i] < 0) { object[i] = -1; continue; }   // object cell without heights: ground
            const int r = root(label[i]);
            if (compact[r] < 0) compact[r] = pieces++;
            object[i] = compact[r];
        }
        res.objects = pieces;
    }
    // --- The surface of every crown: a dome (quadric z = a + bx + cy + dx² + exy + fy², robust
    // least squares over its highest points). It keeps the crown's height and shape — the photo
    // lands where the crown is — without the scatter of the reconstruction that would swirl the
    // leaves. Smooth objects (roofs) keep their finely smoothed surface: ridges stay straight.
    {
        struct Fit { double A[36] = {0}, b[6] = {0}, coef[6] = {0}; double cx = 0, cy = 0, zmin = 1e30, zmax = -1e30; qint64 n = 0; bool ok = false; };
        std::vector<Fit> fits(res.objects);
        for (size_t i = 0; i < object.size(); ++i)
            if (object[i] >= 0 && high[i] && !std::isnan(occluder[i])) {
                Fit &f = fits[object[i]];
                f.cx += i % dsm.w;
                f.cy += i / dsm.w;
                ++f.n;
            }
        for (Fit &f : fits)
            if (f.n) { f.cx /= f.n; f.cy /= f.n; }
        auto basis = [&](const Fit &f, size_t i, double phi[6]) {
            const double x = (double(i % dsm.w) - f.cx) * cell, y = (double(i / dsm.w) - f.cy) * cell;
            phi[0] = 1; phi[1] = x; phi[2] = y; phi[3] = x * x; phi[4] = x * y; phi[5] = y * y;
        };
        for (int pass = 0; pass < 3; ++pass) {   // least squares, then twice with Huber weights (0.3 m)
            for (Fit &f : fits) { std::fill(f.A, f.A + 36, 0.0); std::fill(f.b, f.b + 6, 0.0); }
            for (size_t i = 0; i < object.size(); ++i) {
                if (object[i] < 0 || !high[i] || std::isnan(occluder[i])) continue;
                Fit &f = fits[object[i]];
                double phi[6];
                basis(f, i, phi);
                double weight = 1;
                if (pass > 0 && f.ok) {
                    double model = 0;
                    for (int k = 0; k < 6; ++k) model += f.coef[k] * phi[k];
                    const double r = std::fabs(occluder[i] - model);
                    weight = r <= 0.3 ? 1 : 0.3 / r;
                }
                for (int r = 0; r < 6; ++r) {
                    for (int c = 0; c < 6; ++c) f.A[r * 6 + c] += weight * phi[r] * phi[c];
                    f.b[r] += weight * phi[r] * occluder[i];
                }
                if (pass == 0) {
                    f.zmin = qMin(f.zmin, double(occluder[i]));
                    f.zmax = qMax(f.zmax, double(occluder[i]));
                }
            }
            for (Fit &f : fits) {
                if (f.n < 50) { f.ok = false; continue; }
                double M[36], v[6];
                std::copy(f.A, f.A + 36, M);
                std::copy(f.b, f.b + 6, v);
                bool solved = true;
                for (int c = 0; c < 6 && solved; ++c) {   // Gaussian elimination with partial pivoting
                    int p = c;
                    for (int r = c + 1; r < 6; ++r) if (std::fabs(M[r * 6 + c]) > std::fabs(M[p * 6 + c])) p = r;
                    if (std::fabs(M[p * 6 + c]) < 1e-9) { solved = false; break; }
                    if (p != c) { for (int k = 0; k < 6; ++k) std::swap(M[c * 6 + k], M[p * 6 + k]); std::swap(v[c], v[p]); }
                    for (int r = c + 1; r < 6; ++r) {
                        const double q = M[r * 6 + c] / M[c * 6 + c];
                        for (int k = c; k < 6; ++k) M[r * 6 + k] -= q * M[c * 6 + k];
                        v[r] -= q * v[c];
                    }
                }
                if (!solved) { f.ok = false; continue; }
                for (int r = 5; r >= 0; --r) {
                    double s = v[r];
                    for (int k = r + 1; k < 6; ++k) s -= M[r * 6 + k] * f.coef[k];
                    f.coef[r] = s / M[r * 6 + r];
                }
                f.ok = true;
            }
        }
        // Within 0.6 m of its outline a crown keeps its finely smoothed surface: a dome's steep
        // rim is where a quadric strays, and the rim must land on the crown's real edge.
        const std::vector<float> inside = distanceInside(high, dsm.w, dsm.h);
        const float rim = float(0.6 / cell);
        for (size_t i = 0; i < object.size(); ++i) {
            if (!high[i]) continue;
            // 6 cm: roof, 15 cm: foliage. Smooth only counts where points measured it: the DSM's
            // own filling of gaps is smooth too, in blocks whose steps must not survive.
            const float t = !fromPoints.empty() && !fromPoints[i] ? 1.0f : qBound(0.0f, (rough[i] - 0.06f) / 0.09f, 1.0f);
            double foliage = canopy[i];
            if (object[i] >= 0 && fits[object[i]].ok) {   // a single tree: its dome, near its outline its own surface
                const Fit &f = fits[object[i]];
                double phi[6], model = 0;
                basis(f, i, phi);
                for (int k = 0; k < 6; ++k) model += f.coef[k] * phi[k];
                model = qBound(qMax(f.zmin, std::isnan(ground[i]) ? f.zmin : double(ground[i]) + 0.2), model, f.zmax);
                const double edge = qBound(0.0f, inside[i] / rim, 1.0f);
                const double tree = crowns[i] * (1 - edge) + model * edge;
                foliage = tree * (1 - canopyWeight[i]) + canopy[i] * canopyWeight[i];
            }
            dsm.v[i] = float(crowns[i] * (1 - t) + foliage * t);
        }
    }
    // The surface for the 3D view: the one the photos were projected on (domes of single trees,
    // a smooth canopy, roofs and edges as the points measured them), so the orthophoto draped over
    // it fits it exactly; the area of the DSM only.
    if (!opt.surfacePath.isEmpty()) {
        QDir().mkpath(QFileInfo(opt.surfacePath).absolutePath());
        const char *create[] = {"TILED=YES", "COMPRESS=DEFLATE", "PREDICTOR=3", nullptr};
        GDALDatasetH s = GDALCreate(GDALGetDriverByName("GTiff"), native(opt.surfacePath).constData(), dsm.w, dsm.h, 1, GDT_Float32,
                                    const_cast<char **>(create));
        if (s) {
            GDALSetGeoTransform(s, dsm.gt);
            GDALSetProjection(s, srsWkt);
            GDALRasterBandH band = GDALGetRasterBand(s, 1);
            GDALSetRasterNoDataValue(band, -9999);
            std::vector<float> z(dsm.v.size());
            for (size_t i = 0; i < z.size(); ++i) z[i] = valid[i] && !std::isnan(dsm.v[i]) ? dsm.v[i] : -9999.0f;
            if (GDALRasterIO(band, GF_Write, 0, 0, dsm.w, dsm.h, z.data(), dsm.w, dsm.h, GDT_Float32, 0, 0) != CE_None)
                res.notes << QStringLiteral("Не записана поверхность для 3D");
            GDALClose(s);
        } else {
            res.notes << QStringLiteral("Не создан файл поверхности для 3D: %1").arg(QString::fromUtf8(CPLGetLastErrorMsg()));
        }
    }
    if (!progress(0.08)) { res.error = QStringLiteral("Отменено"); CPLFree(srsWkt); return res; }

    // --- Z-buffers in every photo (reduced): of the projection surface (for objects) and of the
    // unsmoothed surface taken at its nearest within 0.4 m on the ground (for the ground: a
    // ground point is used from a photo only when no crown or roof is next to it in that photo,
    // otherwise a crown underestimated by the reconstruction leaves copies of itself on the
    // ground — "ghosts"); the photo footprints.
    double groundMid = 0;
    {
        std::vector<float> g;
        for (size_t i = 0; i < ground.size(); i += 101)
            if (!std::isnan(ground[i])) g.push_back(ground[i]);
        if (!g.empty()) {
            std::nth_element(g.begin(), g.begin() + g.size() / 2, g.end());
            groundMid = g[g.size() / 2];
        }
    }
    struct View
    {
        int div = 1, zw = 0, zh = 0;
        std::vector<float> z, zo;
        double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;   // footprint, DSM CRS
    };
    std::vector<View> views(n);
    const int stride = qMax(1, int(std::ceil(std::sqrt(double(dsm.w) * dsm.h / 6e6))));
    parallelFor(n, [&](int s) {
        const Shot &shot = scene.shots[s];
        View &v = views[s];
        // At most ~1.2 GB for all z-buffers.
        const double budget = std::sqrt(double(shot.camera.width) * shot.camera.height * 8.0 * n / 1.2e9);
        v.div = qMax(qMax(1, int(std::ceil(shot.camera.width / 1024.0))), int(std::ceil(budget)));
        v.zw = (shot.camera.width + v.div - 1) / v.div;
        v.zh = (shot.camera.height + v.div - 1) / v.div;
        for (int pass = 0; pass < 2; ++pass) {
        const std::vector<float> &surface = pass == 0 ? dsm.v : occluder;
        std::vector<float> &zbuffer = pass == 0 ? v.z : v.zo;
        zbuffer.assign(size_t(v.zw) * v.zh, std::numeric_limits<float>::infinity());
        const int gw = (dsm.w - 1) / stride + 1, gh = (dsm.h - 1) / stride + 1;
        std::vector<float> px(size_t(gw) * gh * 3, NaN);   // projected vertices: x, y, depth (reduced pixels)
        for (int j = 0; j < gh; ++j)
            for (int i = 0; i < gw; ++i) {
                const int x = i * stride, y = j * stride;
                const float z = surface[size_t(y) * dsm.w + x];
                if (std::isnan(z)) continue;
                double P[3], u, w, d;
                local(x, y, z, P);
                if (!shot.project(P, &u, &w, &d)) continue;
                float *o = &px[(size_t(j) * gw + i) * 3];
                o[0] = float(u / v.div);
                o[1] = float(w / v.div);
                o[2] = float(d);
                if (pass == 0 && u >= 0 && w >= 0 && u < shot.camera.width && w < shot.camera.height) {
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
                    float &zb = zbuffer[size_t(y) * v.zw + x];
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
        }
        const double height = qMax(1.0, shot.centre[2] - groundMid);
        const int margin = qMax(1, int(std::ceil(0.4 * shot.camera.fx * qMax(shot.camera.width, shot.camera.height) / height / v.div)));
        runningFilter(v.zo, v.zw, v.zh, margin, true);
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
    // surface normal of DSM cell `cell` (radians). Points of objects against the projection
    // surface, ground points against the occluders with their margin.
    auto see = [&](int s, const double P[3], size_t cell, double *u, double *w, double *vertical, double *slant) {
        const Shot &shot = scene.shots[s];
        double d;
        if (!shot.project(P, u, w, &d)) return false;
        if (*u < 1 || *w < 1 || *u > shot.camera.width - 2 || *w > shot.camera.height - 2) return false;
        const View &v = views[s];
        const int zx = qMin(v.zw - 1, int(*u / v.div)), zy = qMin(v.zh - 1, int(*w / v.div));
        const bool ground = object[cell] < 0;
        if (d > (ground ? v.zo : v.z)[size_t(zy) * v.zw + zx] + (ground ? 0.45 : 0.25) + 0.005 * d) return false;
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

    // --- Object photos: each object from the photo that sees all of it most vertically; the next
    // best ones for what it does not see and for the edges of its frame.
    std::vector<int> objectShot(res.objects, -1);
    std::vector<std::array<int, 4>> objectShots(res.objects);
    {
        std::vector<std::vector<int>> cells(res.objects);
        for (size_t i = 0; i < object.size(); ++i)
            if (object[i] >= 0 && (cells[object[i]].size() < 4000 || i % 3 == 0)) cells[object[i]].push_back(int(i));
        parallelFor(res.objects, [&](int o) {
            if (cells[o].empty()) return;
            double cx = 0, cy = 0;   // centre of the piece, to skip photos that do not cover it
            for (int c : cells[o]) {
                cx += dsm.gt[0] + (c % dsm.w + 0.5) * dsm.gt[1];
                cy += dsm.gt[3] + (c / dsm.w + 0.5) * dsm.gt[5];
            }
            cx /= cells[o].size();
            cy /= cells[o].size();
            std::vector<std::pair<double, int>> ranked;
            for (int s = 0; s < n; ++s) {
                if (cx < views[s].minX || cx > views[s].maxX || cy < views[s].minY || cy > views[s].maxY) continue;
                int seen = 0;
                double angles = 0;
                for (int c : cells[o]) {
                    double P[3], u, w, a, slant;
                    local(c % dsm.w, c / dsm.w, dsm.v[c], P);
                    if (see(s, P, size_t(c), &u, &w, &a, &slant)) { ++seen; angles += a; }
                }
                if (!seen) continue;
                const double fraction = double(seen) / cells[o].size();
                ranked.emplace_back(fraction >= 0.97 ? 10 - angles / seen : fraction, s);
            }
            std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
            objectShots[o].fill(-1);
            for (size_t k = 0; k < ranked.size() && k < 4; ++k) objectShots[o][k] = ranked[k].second;
            objectShot[o] = objectShots[o][0];
        });
    }
    // Ground within 0.6 m of an object (gaps in a canopy, the foot of a crown) is taken from that
    // object's photos when they see it, so a gap does not become a patch from another photo.
    std::vector<int> nearObject = object;
    nearestLabel(nearObject, dsm.w, dsm.h, float(0.6 / cell));
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
    GDALDatasetH sources = nullptr;
    if (!opt.sourceMapPath.isEmpty()) {
        const char *small[] = {"TILED=YES", "COMPRESS=DEFLATE", nullptr};
        sources = GDALCreate(GDALGetDriverByName("GTiff"), native(opt.sourceMapPath).constData(), res.width, res.height, 1, GDT_Byte,
                             const_cast<char **>(small));
        if (sources) {
            GDALSetGeoTransform(sources, ogt);
            GDALSetProjection(sources, GDALGetProjectionRef(out));
            GDALSetRasterNoDataValue(GDALGetRasterBand(sources, 1), 255);
        }
    }

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
        std::vector<uchar> rgba(size_t(tw) * th * 4, 0), source(sources ? size_t(tw) * th : 0, 255);
        std::map<int, QImage> images;
        auto photo = [&](int s) -> const QImage & {
            auto it = images.find(s);
            if (it == images.end()) it = images.emplace(s, photos.get(scene.shots[s].image)).first;
            return it->second;
        };
        auto colourOf = [&](int s, double u, double w, double c[3]) {   // balanced colour of a photo
            sampleCubic(photo(s), u, w, c);
            for (int ch = 0; ch < 3; ++ch) c[ch] *= gain[size_t(s) * 3 + ch];
        };
        // 0 at the edge of a photo's frame, 1 from 6 % of its size inwards.
        auto frameWeight = [&](int s, double u, double w) {
            const Camera &c = scene.shots[s].camera;
            const double d = qMin(qMin(u, w), qMin(c.width - 1 - u, c.height - 1 - w));
            return qBound(0.0, d / (0.06 * qMax(c.width, c.height)), 1.0);
        };
        // The first of the ranked photos that sees the point, faded into the next one near the
        // edge of its frame.
        auto fromRanked = [&](const std::array<int, 4> &ranked, const double P[3], size_t cellIndex, bool object, double c[3], int *used) {
            int first = -1;
            double weight = 1, c1[3];
            for (int s : ranked) {
                if (s < 0) break;
                double u, w, a, slant;
                if (!see(s, P, cellIndex, &u, &w, &a, &slant) || (object && slant >= MaxObjectSlant)) continue;
                if (first < 0) {
                    first = s;
                    *used = s;
                    colourOf(s, u, w, c1);
                    weight = frameWeight(s, u, w);
                    if (weight >= 1) break;
                    continue;
                }
                double c2[3];
                colourOf(s, u, w, c2);
                for (int ch = 0; ch < 3; ++ch) c[ch] = c1[ch] * weight + c2[ch] * (1 - weight);
                return true;
            }
            if (first < 0) return false;
            if (weight < 1) {
                // None of the others ranked sees it: fade into the photo used beyond the edge of
                // the frame — the one that looks at the surface most squarely.
                int other = -1;
                double best = 1e9, ou = 0, ow = 0;
                for (int s : candidates) {
                    double u, w, a, slant;
                    if (s == first || !see(s, P, cellIndex, &u, &w, &a, &slant)) continue;
                    if (slant < best) { best = slant; other = s; ou = u; ow = w; }
                }
                if (other >= 0) {
                    double c2[3];
                    colourOf(other, ou, ow, c2);
                    for (int ch = 0; ch < 3; ++ch) c[ch] = c1[ch] * weight + c2[ch] * (1 - weight);
                    return true;
                }
            }
            std::copy(c1, c1 + 3, c);
            return true;
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
                const size_t cellIndex = size_t(cy) * dsm.w + cx;
                const int o = object[cellIndex];
                const bool isObject = o >= 0 && objectShot[o] >= 0;
                // Objects from their ranked photos; ground right next to an object from that
                // object's photos when they see it (gaps in a canopy stay in the crowns' photo).
                bool done = false;
                int used = -1;
                if (isObject) done = fromRanked(objectShots[o], P, cellIndex, true, rgb, &used);
                else if (nearObject[cellIndex] >= 0 && objectShot[nearObject[cellIndex]] >= 0)
                    done = fromRanked(objectShots[nearObject[cellIndex]], P, cellIndex, false, rgb, &used);
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
                    if (s1 < 0) {
                        // Hidden from every photo (a pit in a noisy crown, ground between crowns):
                        // the object's own photo, else the photo where it is least hidden — its
                        // depth nearest to the surface seen at its pixel — rather than a hole.
                        ++hidden;
                        const int own = isObject ? objectShot[o] : -1;
                        double leastHidden = 1e300;
                        for (int s : candidates) {
                            double u, w, d;
                            const Shot &shot = scene.shots[s];
                            if (!shot.project(P, &u, &w, &d) || u < 1 || w < 1 || u > shot.camera.width - 2 || w > shot.camera.height - 2) continue;
                            const View &v = views[s];
                            const double excess = s == own ? -1e300
                                                           : d - v.z[size_t(qMin(v.zh - 1, int(w / v.div))) * v.zw + qMin(v.zw - 1, int(u / v.div))];
                            if (excess < leastHidden) { leastHidden = excess; s1 = s; u1 = u; w1 = w; }
                        }
                        s2 = -1;
                    }
                    if (s1 < 0) continue;
                    used = s1;
                    colourOf(s1, u1, w1, rgb);
                    // Soft transitions where two photos are almost as good, and towards the second
                    // photo near the edge of the first one's frame (no straight seams).
                    if (s2 >= 0) {
                        double k = a2 - a1 < blend ? 0.5 * (1 - (a2 - a1) / blend) : 0.0;
                        k = qMax(k, 1 - frameWeight(s1, u1, w1));
                        if (k > 0) {
                            double c2[3];
                            colourOf(s2, u2, w2, c2);
                            for (int ch = 0; ch < 3; ++ch) rgb[ch] = rgb[ch] * (1 - k) + c2[ch] * k;
                        }
                    }
                }
                uchar *p = &rgba[(size_t(y) * tw + x) * 4];
                for (int ch = 0; ch < 3; ++ch) p[ch] = uchar(qBound(0.0, rgb[ch] + 0.5, 255.0));
                p[3] = 255;
                if (sources && used >= 0) source[size_t(y) * tw + x] = uchar(qMin(used, 254));
                ++coloured;
            }
        {
            QMutexLocker lock(&writeMutex);
            int bands[4] = {1, 2, 3, 4};
            GDALDatasetRasterIO(out, GF_Write, x0, y0, tw, th, rgba.data(), tw, th, GDT_Byte, 4, bands, 4, tw * 4, 1);
            if (sources) GDALRasterIO(GDALGetRasterBand(sources, 1), GF_Write, x0, y0, tw, th, source.data(), tw, th, GDT_Byte, 0, 0);
        }
        colouredPixels += coloured;
        hiddenPixels += hidden;
        validPixels += validHere;
        if (!progress(0.35 + 0.55 * double(++doneTiles) / (tilesX * tilesY))) cancelled = true;
    });
    if (sources) GDALClose(sources);
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
