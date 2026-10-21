#include "ImageQuality.h"

#include <QImageReader>
#include <algorithm>
#include <cmath>
#include <vector>

namespace imagequality {
namespace {
constexpr int TilesX = 8, TilesY = 6;
constexpr int Blur = 9;               // the further blur, pixels
constexpr double MinGradient = 2.0;   // mean |difference| of a tile with contrast, grey levels

// Linear between the closest ranks (numpy's default).
double percentile(std::vector<double> v, double p)
{
    if (v.empty()) return qQNaN();
    std::sort(v.begin(), v.end());
    const double k = p * double(v.size() - 1);
    const size_t i = size_t(std::floor(k));
    return i + 1 < v.size() ? v[i] + (k - double(i)) * (v[i + 1] - v[i]) : v[i];
}

} // namespace

double sharpness(const QImage &image)
{
    const QImage grey = image.format() == QImage::Format_Grayscale8 ? image : image.convertToFormat(QImage::Format_Grayscale8);
    const int w = grey.width(), h = grey.height();
    if (w < TilesX * 8 || h < TilesY * 8) return qQNaN();
    // Sums of Blur pixels along the rows and along the columns, edges repeated: Blur × the
    // blurred image, exact in integers (at most 9 × 255).
    const int r = Blur / 2;
    std::vector<quint16> across(size_t(w) * h), down(size_t(w) * h);
    for (int y = 0; y < h; ++y) {
        const uchar *in = grey.constScanLine(y);
        quint16 *out = across.data() + size_t(y) * w;
        int sum = 0;
        for (int i = -r; i <= r; ++i) sum += in[std::clamp(i, 0, w - 1)];
        for (int x = 0; x < w; ++x) {
            out[x] = quint16(sum);
            sum += in[std::min(w - 1, x + r + 1)] - in[std::max(0, x - r)];
        }
    }
    std::vector<int> column(size_t(w), 0);
    for (int i = -r; i <= r; ++i) {
        const uchar *line = grey.constScanLine(std::clamp(i, 0, h - 1));
        for (int x = 0; x < w; ++x) column[size_t(x)] += line[x];
    }
    for (int y = 0; y < h; ++y) {
        quint16 *out = down.data() + size_t(y) * w;
        const uchar *add = grey.constScanLine(std::min(h - 1, y + r + 1)), *drop = grey.constScanLine(std::max(0, y - r));
        for (int x = 0; x < w; ++x) {
            out[x] = quint16(column[size_t(x)]);
            column[size_t(x)] += add[x] - drop[x];
        }
    }

    std::vector<double> textured, all;
    for (int ty = 0; ty < TilesY; ++ty)
        for (int tx = 0; tx < TilesX; ++tx) {
            const int x0 = tx * w / TilesX, x1 = (tx + 1) * w / TilesX, y0 = ty * h / TilesY, y1 = (ty + 1) * h / TilesY;
            // Differences of the image (F) and of its blurred copy (B), vertical and horizontal:
            // what the blur removes, max(0, |ΔF| − |ΔB|), is the part of the gradient that was
            // sharp. In units of 1/Blur grey level.
            qint64 fv = 0, vv = 0, fh = 0, vh = 0;
            for (int y = y0; y < y1; ++y) {
                const uchar *line = grey.constScanLine(y), *above = grey.constScanLine(std::max(y0, y - 1));
                const quint16 *a = across.data() + size_t(y) * w, *d = down.data() + size_t(y) * w;
                for (int x = x0; x < x1; ++x) {
                    if (y > y0) {
                        const int dF = Blur * std::abs(line[x] - above[x]), dB = std::abs(d[x] - d[x - w]);
                        fv += dF;
                        vv += std::max(0, dF - dB);
                    }
                    if (x > x0) {
                        const int dF = Blur * std::abs(line[x] - line[x - 1]), dB = std::abs(a[x] - a[x - 1]);
                        fh += dF;
                        vh += std::max(0, dF - dB);
                    }
                }
            }
            if (fv <= 0 || fh <= 0) {
                all.push_back(0);
                continue;
            }
            const double blur = std::max(double(fv - vv) / double(fv), double(fh - vh) / double(fh));
            all.push_back(1 - blur);
            const double n = double(Blur) * (x1 - x0) * (y1 - y0);
            if (fv / n >= MinGradient && fh / n >= MinGradient) textured.push_back(1 - blur);
        }
    return percentile(textured.empty() ? all : textured, 0.9);
}

Result measure(const QString &path)
{
    Result r;
    QImageReader reader(path);
    reader.setAutoTransform(false);   // orientation does not change sharpness
    // Halved until the long side fits: JPEG decodes at 1/2, 1/4… directly (libjpeg's sizes, rounded
    // up), with no resampling that would soften the edges.
    const QSize size = reader.size();
    int f = 1;
    while (size.isValid() && qMax(size.width(), size.height()) > MaxSide * f) f *= 2;
    if (f > 1) reader.setScaledSize(QSize((size.width() + f - 1) / f, (size.height() + f - 1) / f));
    QImage image = reader.read();
    if (image.isNull()) {
        r.error = reader.errorString();
        return r;
    }
    image = image.convertToFormat(QImage::Format_Grayscale8);   // the colour copy goes
    r.sharpness = sharpness(image);
    if (qIsNaN(r.sharpness)) r.error = QStringLiteral("Снимок слишком мал");
    return r;
}

double reference(QVector<double> values)
{
    std::vector<double> v;
    for (double s : values)
        if (!qIsNaN(s)) v.push_back(s);
    return percentile(v, 0.9);
}

double quality(double sharpness, double reference)
{
    if (qIsNaN(sharpness) || qIsNaN(reference)) return qQNaN();
    return std::clamp(1 - (reference - sharpness) / Span, 0.0, 1.0);
}

} // namespace imagequality
