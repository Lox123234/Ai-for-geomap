#include "Georeference.h"
#include "GisEnvironment.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <cmath>
#include <cpl_string.h>
#include <gdal.h>
#include <gdal_utils.h>
#include <vector>

namespace georef {

int minimumLinks(Method m)
{
    switch (m) {
    case Method::Affine: return 3;
    case Method::Polynomial2: return 6;
    case Method::Polynomial3: return 10;
    case Method::Projective: return 4;
    }
    return 3;
}

QString title(Method m)
{
    switch (m) {
    case Method::Affine: return QStringLiteral("Аффинное");
    case Method::Polynomial2: return QStringLiteral("Полином 2-го порядка");
    case Method::Polynomial3: return QStringLiteral("Полином 3-го порядка");
    case Method::Projective: return QStringLiteral("Проективное");
    }
    return {};
}

namespace {
QByteArray native(const QString &path) { return QDir::toNativeSeparators(path).toUtf8(); }

// Terms x^i·y^j with i + j ≤ order.
QVector<QPair<int, int>> terms(int order)
{
    QVector<QPair<int, int>> t;
    for (int n = 0; n <= order; ++n)
        for (int j = 0; j <= n; ++j) t << qMakePair(n - j, j);
    return t;
}

// Least squares A·c = b through the normal equations (Gaussian elimination with pivoting);
// false when the system is degenerate (points on a line, repeated points).
bool solve(const std::vector<std::vector<double>> &A, const std::vector<double> &b, QVector<double> *c)
{
    const size_t k = A.empty() ? 0 : A[0].size();
    std::vector<double> M(k * k, 0.0), v(k, 0.0);
    for (size_t r = 0; r < A.size(); ++r)
        for (size_t i = 0; i < k; ++i) {
            v[i] += A[r][i] * b[r];
            for (size_t j = 0; j < k; ++j) M[i * k + j] += A[r][i] * A[r][j];
        }
    double scale = 0;
    for (size_t i = 0; i < k; ++i) scale = qMax(scale, std::fabs(M[i * k + i]));
    for (size_t col = 0; col < k; ++col) {
        size_t p = col;
        for (size_t r = col + 1; r < k; ++r)
            if (std::fabs(M[r * k + col]) > std::fabs(M[p * k + col])) p = r;
        if (std::fabs(M[p * k + col]) < 1e-12 * qMax(1.0, scale)) return false;
        if (p != col) {
            for (size_t j = 0; j < k; ++j) std::swap(M[col * k + j], M[p * k + j]);
            std::swap(v[col], v[p]);
        }
        for (size_t r = col + 1; r < k; ++r) {
            const double f = M[r * k + col] / M[col * k + col];
            for (size_t j = col; j < k; ++j) M[r * k + j] -= f * M[col * k + j];
            v[r] -= f * v[col];
        }
    }
    c->resize(int(k));
    for (int r = int(k) - 1; r >= 0; --r) {
        double s = v[size_t(r)];
        for (size_t j = size_t(r) + 1; j < k; ++j) s -= M[size_t(r) * k + j] * (*c)[int(j)];
        (*c)[r] = s / M[size_t(r) * k + size_t(r)];
    }
    return true;
}

void normalisation(const QVector<QPointF> &pts, QPointF *centre, double *scale)
{
    QPointF c(0, 0);
    for (const QPointF &p : pts) c += p;
    c /= qMax(1, int(pts.size()));
    double s = 0;
    for (const QPointF &p : pts) s += std::hypot(p.x() - c.x(), p.y() - c.y());
    s = pts.isEmpty() ? 1 : s / pts.size();
    *centre = c;
    *scale = s > 1e-12 ? s : 1;
}
} // namespace

struct Fitter
{
    static bool fit(const QVector<QPointF> &in, const QVector<QPointF> &out, Method method, Transform *t)
    {
        Transform r;
        r.m_method = method;
        normalisation(in, &r.m_inCentre, &r.m_inScale);
        normalisation(out, &r.m_outCentre, &r.m_outScale);
        auto nIn = [&](const QPointF &p) { return (p - r.m_inCentre) / r.m_inScale; };
        auto nOut = [&](const QPointF &p) { return (p - r.m_outCentre) / r.m_outScale; };
        if (method == Method::Projective) {
            std::vector<std::vector<double>> A;
            std::vector<double> b;
            for (int i = 0; i < in.size(); ++i) {
                const QPointF p = nIn(in[i]), q = nOut(out[i]);
                A.push_back({p.x(), p.y(), 1, 0, 0, 0, -p.x() * q.x(), -p.y() * q.x()});
                b.push_back(q.x());
                A.push_back({0, 0, 0, p.x(), p.y(), 1, -p.x() * q.y(), -p.y() * q.y()});
                b.push_back(q.y());
            }
            if (!solve(A, b, &r.m_x)) return false;
        } else {
            const QVector<QPair<int, int>> t = terms(int(method));
            std::vector<std::vector<double>> A;
            std::vector<double> bx, by;
            for (int i = 0; i < in.size(); ++i) {
                const QPointF p = nIn(in[i]), q = nOut(out[i]);
                std::vector<double> row;
                for (const auto &e : t) row.push_back(std::pow(p.x(), e.first) * std::pow(p.y(), e.second));
                A.push_back(row);
                bx.push_back(q.x());
                by.push_back(q.y());
            }
            if (!solve(A, bx, &r.m_x) || !solve(A, by, &r.m_y)) return false;
        }
        r.m_valid = true;
        *t = r;
        return true;
    }
};

QPointF Transform::map(const QPointF &pIn) const
{
    if (!m_valid) return pIn;
    const QPointF p = (pIn - m_inCentre) / m_inScale;
    QPointF q;
    if (m_method == Method::Projective) {
        const double w = m_x[6] * p.x() + m_x[7] * p.y() + 1;
        q = QPointF((m_x[0] * p.x() + m_x[1] * p.y() + m_x[2]) / w, (m_x[3] * p.x() + m_x[4] * p.y() + m_x[5]) / w);
    } else {
        const QVector<QPair<int, int>> t = terms(int(m_method));
        double x = 0, y = 0;
        for (int i = 0; i < t.size(); ++i) {
            const double v = std::pow(p.x(), t[i].first) * std::pow(p.y(), t[i].second);
            x += m_x[i] * v;
            y += m_y[i] * v;
        }
        q = QPointF(x, y);
    }
    return q * m_outScale + m_outCentre;
}

Transform Transform::fromCorners(const QSizeF &s, const QPolygonF &corners)
{
    Transform t;
    if (corners.size() != 4) return t;
    const QVector<QPointF> in{{0, 0}, {s.width(), 0}, {s.width(), s.height()}, {0, s.height()}};
    const QVector<QPointF> out(corners.begin(), corners.end());
    Fitter::fit(in, out, Method::Projective, &t);
    return t;
}

Fit fit(const QVector<Link> &links, Method method)
{
    Fit f;
    QVector<QPointF> in, out;
    for (const Link &l : links)
        if (l.enabled) {
            in << l.image;
            out << l.map;
        }
    f.used = int(in.size());
    if (f.used < minimumLinks(method)) {
        f.error = QStringLiteral("Для преобразования «%1» нужно не меньше %2 связей (включено %3)")
                      .arg(title(method)).arg(minimumLinks(method)).arg(f.used);
        return f;
    }
    if (!Fitter::fit(in, out, method, &f.forward) || !Fitter::fit(out, in, method, &f.inverse)) {
        f.error = QStringLiteral("Точки на одной линии — разнесите их по снимку");
        return f;
    }
    double s = 0, si = 0;
    for (const Link &l : links) {
        if (!l.enabled) {
            f.residual << qQNaN();
            f.inverseResidual << qQNaN();
            continue;
        }
        const QPointF m = f.forward.map(l.image), i = f.inverse.map(l.map);
        const double r = std::hypot(m.x() - l.map.x(), m.y() - l.map.y()), ri = std::hypot(i.x() - l.image.x(), i.y() - l.image.y());
        f.residual << r;
        f.inverseResidual << ri;
        s += r * r;
        si += ri * ri;
    }
    f.rmse = std::sqrt(s / f.used);
    f.inverseRmse = std::sqrt(si / f.used);
    return f;
}

QSize imageSize(const QString &imagePath)
{
    GDALDatasetH ds = GDALOpenEx(native(imagePath).constData(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
    if (!ds) return {};
    const QSize s(GDALGetRasterXSize(ds), GDALGetRasterYSize(ds));
    GDALClose(ds);
    return s;
}

WarpResult warp(const QString &imagePath, const Transform &forward, const Crs &crs, const QString &outputPath, double resolution,
                int maxPixels, const std::function<bool(double)> &progress)
{
    WarpResult res;
    if (!forward.isValid()) {
        res.error = QStringLiteral("Нет преобразования: добавьте связи");
        return res;
    }
    if (!crs.isValid()) {
        res.error = QStringLiteral("У карты нет системы координат");
        return res;
    }
    GisEnvironment::clearError();
    GDALDatasetH src = GDALOpenEx(native(imagePath).constData(), GDAL_OF_RASTER | GDAL_OF_READONLY, nullptr, nullptr, nullptr);
    if (!src) {
        res.error = QStringLiteral("Изображение не открывается: %1").arg(GisEnvironment::lastError());
        return res;
    }
    const int w = GDALGetRasterXSize(src), h = GDALGetRasterYSize(src);
    const double k = maxPixels > 0 && qMax(w, h) > maxPixels ? double(maxPixels) / qMax(w, h) : 1.0;
    // A VRT of the image (reduced for a preview) that carries the control points: a grid of 21 × 21
    // points of the fitted transformation, warped by thin plate spline — the transformation itself
    // at every node, smooth between them.
    CPLStringList targs;
    targs.AddString("-of");
    targs.AddString("VRT");
    if (k < 1) {
        targs.AddString("-outsize");
        targs.AddString(QByteArray::number(qMax(1, int(std::lround(w * k)))).constData());
        targs.AddString(QByteArray::number(qMax(1, int(std::lround(h * k)))).constData());
    }
    GDALTranslateOptions *to = GDALTranslateOptionsNew(targs.List(), nullptr);
    int usage = 0;
    GDALDatasetH vrt = GDALTranslate("", src, to, &usage);
    GDALTranslateOptionsFree(to);
    if (!vrt) {
        GDALClose(src);
        res.error = QStringLiteral("Не удалось подготовить изображение: %1").arg(GisEnvironment::lastError());
        return res;
    }
    const int n = 21;
    std::vector<GDAL_GCP> gcps(size_t(n) * n);
    std::vector<QByteArray> ids(gcps.size());
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const QPointF px(w * double(i) / (n - 1), h * double(j) / (n - 1)), m = forward.map(px);
            GDAL_GCP &g = gcps[size_t(j) * n + i];
            ids[size_t(j) * n + i] = QByteArray::number(j * n + i);
            g.pszId = ids[size_t(j) * n + i].data();
            g.pszInfo = const_cast<char *>("");
            g.dfGCPPixel = px.x() * k;
            g.dfGCPLine = px.y() * k;
            g.dfGCPX = m.x();
            g.dfGCPY = m.y();
            g.dfGCPZ = 0;
        }
    const QByteArray wkt = crs.wkt().toUtf8();
    GDALSetGCPs(vrt, int(gcps.size()), gcps.data(), wkt.constData());
    if (!(resolution > 0)) {   // the image's own pixel at its centre
        const QPointF c(w / 2.0, h / 2.0), a = forward.map(c), bx = forward.map(c + QPointF(1, 0)), by = forward.map(c + QPointF(0, 1));
        resolution = std::sqrt(std::fabs((bx.x() - a.x()) * (by.y() - a.y()) - (bx.y() - a.y()) * (by.x() - a.x())));
        if (!(resolution > 0) || !std::isfinite(resolution)) resolution = 1;
    }
    resolution /= k;
    QDir().mkpath(QFileInfo(outputPath).absolutePath());
    const QString partial = outputPath + QStringLiteral(".partial.tif");
    QFile::remove(partial);
    CPLStringList wargs;
    for (const char *a : {"-of", "GTiff", "-tps", "-r", "bilinear", "-dstalpha", "-co", "COMPRESS=DEFLATE", "-co", "TILED=YES", "-co", "BIGTIFF=IF_SAFER",
                          "-multi", "-wo", "NUM_THREADS=ALL_CPUS"})
        wargs.AddString(a);
    wargs.AddString("-t_srs");
    wargs.AddString(wkt.constData());
    wargs.AddString("-tr");
    wargs.AddString(QByteArray::number(resolution, 'g', 17).constData());
    wargs.AddString(QByteArray::number(resolution, 'g', 17).constData());
    GDALWarpAppOptions *wo = GDALWarpAppOptionsNew(wargs.List(), nullptr);
    struct Ctx { const std::function<bool(double)> *f; } ctx{&progress};
    if (progress)
        GDALWarpAppOptionsSetProgress(wo, [](double v, const char *, void *p) -> int {
            const auto *c = static_cast<Ctx *>(p);
            return (*c->f)(v) ? TRUE : FALSE;
        }, &ctx);
    GDALDatasetH out = GDALWarp(native(partial).constData(), nullptr, 1, &vrt, wo, &usage);
    GDALWarpAppOptionsFree(wo);
    GDALClose(vrt);
    GDALClose(src);
    if (!out) {
        QFile::remove(partial);
        res.error = QStringLiteral("Не удалось трансформировать изображение: %1").arg(GisEnvironment::lastError());
        return res;
    }
    res.width = GDALGetRasterXSize(out);
    res.height = GDALGetRasterYSize(out);
    res.resolution = resolution;
    QVector<int> levels;
    for (int f = 2; qMax(res.width, res.height) / f >= 256; f *= 2) levels << f;
    if (!levels.isEmpty()) GDALBuildOverviews(out, "AVERAGE", int(levels.size()), levels.data(), 0, nullptr, nullptr, nullptr);
    GDALClose(out);
    QFile::remove(outputPath);
    if (!QFile::rename(partial, outputPath)) {
        QFile::remove(partial);
        res.error = QStringLiteral("Не удалось сохранить %1").arg(QDir::toNativeSeparators(outputPath));
        return res;
    }
    res.path = outputPath;
    return res;
}

} // namespace georef
