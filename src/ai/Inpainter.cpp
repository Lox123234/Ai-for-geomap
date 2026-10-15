#include "Inpainter.h"

#include <QPainter>
#include <QRect>
#include <QVector>
#include <cmath>
#include <mutex>

#ifdef GEOMAP_HAS_ONNXRUNTIME
#include "OrtCommon.h"
#endif

namespace ai {

namespace {
// Bounding boxes of the 8-connected parts of a binary mask.
QVector<QRect> parts(const QImage &mask)
{
    const int w = mask.width(), h = mask.height();
    QVector<int> label(w * h, 0);
    QVector<QRect> boxes;
    QVector<int> stack;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (!mask.constScanLine(y)[x] || label[y * w + x]) continue;
            const int id = int(boxes.size()) + 1;
            int x0 = x, x1 = x, y0 = y, y1 = y;
            stack = {y * w + x};
            label[y * w + x] = id;
            while (!stack.isEmpty()) {
                const int p = stack.takeLast(), px = p % w, py = p / w;
                x0 = qMin(x0, px); x1 = qMax(x1, px); y0 = qMin(y0, py); y1 = qMax(y1, py);
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = px + dx, ny = py + dy;
                        if (nx < 0 || ny < 0 || nx >= w || ny >= h || label[ny * w + nx] || !mask.constScanLine(ny)[nx]) continue;
                        label[ny * w + nx] = id;
                        stack << ny * w + nx;
                    }
            }
            boxes << QRect(QPoint(x0, y0), QPoint(x1, y1));
        }
    return boxes;
}

// A square window of side `side` around `box`, moved inside the image when it can be.
QRect windowAround(const QRect &box, int side, const QSize &image)
{
    QRect r(0, 0, side, side);
    r.moveCenter(box.center());
    if (r.width() <= image.width()) r.moveLeft(qBound(0, r.left(), image.width() - r.width()));
    if (r.height() <= image.height()) r.moveTop(qBound(0, r.top(), image.height() - r.height()));
    return r;
}
} // namespace

#ifdef GEOMAP_HAS_ONNXRUNTIME

struct Inpainter::Impl
{
    std::unique_ptr<Ort::Session> session;
    std::string imageName = "image", maskName = "mask", outputName = "output";
};

Inpainter::Inpainter() : d(std::make_unique<Impl>()) {}
Inpainter::~Inpainter() = default;

bool Inpainter::load(const ModelCard &card, QString *error)
{
    d->session.reset();
    if (!card.isValid()) {
        if (error) *error = card.error;
        return false;
    }
    if (!verifyFiles(card, error)) return false;
    const QString path = card.filePath(QStringLiteral("inpainter"));
    if (path.isEmpty()) {
        if (error) *error = QStringLiteral("В карточке нет файла с ролью inpainter");
        return false;
    }
    try {
        d->session = ort::open(path);
        const auto in = ort::names(*d->session, true), out = ort::names(*d->session, false);
        if (in.size() != 2 || out.empty()) {
            d->session.reset();
            if (error) *error = QStringLiteral("Модель дорисовки должна принимать изображение и маску");
            return false;
        }
        // The input with one channel is the mask.
        const auto first = d->session->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
        const bool firstIsMask = first.size() == 4 && first[1] == 1;
        d->imageName = in[firstIsMask ? 1 : 0];
        d->maskName = in[firstIsMask ? 0 : 1];
        d->outputName = out[0];
    } catch (const Ort::Exception &e) {
        d->session.reset();
        if (error) *error = ort::error(e);
        return false;
    }
    return true;
}

bool Inpainter::isLoaded() const { return bool(d->session); }

Inpainter::Result Inpainter::inpaint(const QImage &input, const QImage &maskInput)
{
    Result r;
    if (!isLoaded()) {
        r.error = QStringLiteral("Модель дорисовки не загружена");
        return r;
    }
    if (input.isNull() || maskInput.size() != input.size()) {
        r.error = QStringLiteral("Маска должна быть размером с изображение");
        return r;
    }
    QImage image = input.convertToFormat(QImage::Format_RGB888);
    QImage mask = maskInput.convertToFormat(QImage::Format_Grayscale8);
    const QVector<QRect> boxes = parts(mask);
    QVector<bool> done(boxes.size(), false);
    const int N = WindowSize;
    std::vector<float> imageTensor(size_t(3) * N * N), maskTensor(size_t(N) * N);
    const std::vector<int64_t> imageShape{1, 3, N, N}, maskShape{1, 1, N, N};
    try {
        const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        for (int b = 0; b < boxes.size(); ++b) {
            if (done[b]) continue;
            const QRect box = boxes[b];
            // Context: half the part's size on every side, at least 24 px.
            const int margin = qMax(24, qMax(box.width(), box.height()) / 2);
            const int side = qMax(N, qMax(box.width(), box.height()) + 2 * margin);
            const QRect window = windowAround(box, side, image.size());
            const bool scaled = side > N;
            // The window (edge pixels repeated outside the image) and its mask, at 512 × 512.
            QImage crop(window.size(), QImage::Format_RGB888), cropMask(window.size(), QImage::Format_Grayscale8);
            for (int y = 0; y < window.height(); ++y) {
                const int sy = qBound(0, window.top() + y, image.height() - 1);
                uchar *c = crop.scanLine(y), *m = cropMask.scanLine(y);
                const uchar *src = image.constScanLine(sy), *srcMask = mask.constScanLine(sy);
                const bool inside = window.top() + y >= 0 && window.top() + y < image.height();
                for (int x = 0; x < window.width(); ++x) {
                    const int sx = qBound(0, window.left() + x, image.width() - 1);
                    const bool in = inside && window.left() + x >= 0 && window.left() + x < image.width();
                    c[x * 3] = src[sx * 3]; c[x * 3 + 1] = src[sx * 3 + 1]; c[x * 3 + 2] = src[sx * 3 + 2];
                    m[x] = in && srcMask[sx] ? 255 : 0;
                }
            }
            const QImage net = scaled ? crop.scaled(N, N, Qt::IgnoreAspectRatio, Qt::SmoothTransformation) : crop;
            const QImage netMask = scaled ? cropMask.scaled(N, N, Qt::IgnoreAspectRatio, Qt::FastTransformation) : cropMask;
            for (int y = 0; y < N; ++y) {
                const uchar *c = net.constScanLine(y), *m = netMask.constScanLine(y);
                for (int x = 0; x < N; ++x) {
                    for (int ch = 0; ch < 3; ++ch) imageTensor[size_t(ch) * N * N + size_t(y) * N + x] = c[x * 3 + ch] / 255.0f;
                    maskTensor[size_t(y) * N + x] = m[x] ? 1.0f : 0.0f;
                }
            }
            Ort::Value inputs[2] = {Ort::Value::CreateTensor<float>(memory, imageTensor.data(), imageTensor.size(), imageShape.data(), imageShape.size()),
                                    Ort::Value::CreateTensor<float>(memory, maskTensor.data(), maskTensor.size(), maskShape.data(), maskShape.size())};
            const char *inNames[2] = {d->imageName.c_str(), d->maskName.c_str()};
            const char *outName = d->outputName.c_str();
            auto out = d->session->Run(Ort::RunOptions{nullptr}, inNames, inputs, 2, &outName, 1);
            const auto shape = out[0].GetTensorTypeAndShapeInfo().GetShape();
            if (shape.size() != 4 || shape[1] != 3 || shape[2] != N || shape[3] != N) {
                r.error = QStringLiteral("Неожиданный размер результата модели дорисовки");
                return r;
            }
            const float *o = out[0].GetTensorData<float>();
            // LaMa exports return 0..255; others 0..1.
            float peak = 0;
            for (size_t i = 0; i < size_t(3) * N * N; i += 97) peak = qMax(peak, o[i]);
            const float k = peak > 2.0f ? 1.0f : 255.0f;
            QImage filled(N, N, QImage::Format_RGB888);
            for (int y = 0; y < N; ++y) {
                uchar *f = filled.scanLine(y);
                for (int x = 0; x < N; ++x)
                    for (int ch = 0; ch < 3; ++ch)
                        f[x * 3 + ch] = uchar(qBound(0.0f, o[size_t(ch) * N * N + size_t(y) * N + x] * k + 0.5f, 255.0f));
            }
            if (scaled) filled = filled.scaled(window.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            // Only the masked pixels of the image change; they are no longer holes for later windows.
            for (int y = 0; y < window.height(); ++y) {
                const int iy = window.top() + y;
                if (iy < 0 || iy >= image.height()) continue;
                uchar *dst = image.scanLine(iy), *m = mask.scanLine(iy);
                const uchar *f = filled.constScanLine(y);
                for (int x = 0; x < window.width(); ++x) {
                    const int ix = window.left() + x;
                    if (ix < 0 || ix >= image.width() || !m[ix]) continue;
                    dst[ix * 3] = f[x * 3]; dst[ix * 3 + 1] = f[x * 3 + 1]; dst[ix * 3 + 2] = f[x * 3 + 2];
                    m[ix] = 0;
                }
            }
            for (int o2 = b; o2 < boxes.size(); ++o2)
                if (window.contains(boxes[o2])) done[o2] = true;
            ++r.windows;
            r.scaled += scaled;
        }
    } catch (const Ort::Exception &e) {
        r.error = ort::error(e);
        return r;
    }
    r.image = image;
    return r;
}

std::shared_ptr<Inpainter> cachedInpainter(const ModelCard &card, QString *error)
{
    ort::environment();   // created first, so it is destroyed after the cached session
    static std::mutex mutex;
    static std::shared_ptr<Inpainter> cached;
    static QString cachedDirectory;
    std::lock_guard<std::mutex> lock(mutex);
    if (cached && cachedDirectory == card.directory) return cached;
    auto inpainter = std::make_shared<Inpainter>();
    if (!inpainter->load(card, error)) return {};
    cached = inpainter;
    cachedDirectory = card.directory;
    return cached;
}

#else   // built without ONNX Runtime

struct Inpainter::Impl {};
Inpainter::Inpainter() : d(std::make_unique<Impl>()) {}
Inpainter::~Inpainter() = default;
bool Inpainter::load(const ModelCard &, QString *error)
{
    if (error) *error = QStringLiteral("Программа собрана без ONNX Runtime: локальный AI недоступен");
    return false;
}
bool Inpainter::isLoaded() const { return false; }
Inpainter::Result Inpainter::inpaint(const QImage &, const QImage &)
{
    Result r;
    r.error = QStringLiteral("Локальный AI недоступен в этой сборке");
    return r;
}
std::shared_ptr<Inpainter> cachedInpainter(const ModelCard &, QString *error)
{
    if (error) *error = QStringLiteral("Программа собрана без ONNX Runtime: локальный AI недоступен");
    return {};
}

#endif

} // namespace ai
