#include "SamSegmenter.h"

#include <cmath>
#include <mutex>

#ifdef GEOMAP_HAS_ONNXRUNTIME
#include "OrtCommon.h"
#endif

namespace ai {

#ifdef GEOMAP_HAS_ONNXRUNTIME

namespace {
using ort::environment;
using ort::names;
constexpr int ImageSize = 1024;      // SAM's input side
constexpr int LowResSize = 256;      // mask input of the decoder
const float PixelMean[3] = {123.675f, 116.28f, 103.53f};
const float PixelStd[3] = {58.395f, 57.12f, 57.375f};
} // namespace

struct SamSegmenter::Impl
{
    std::unique_ptr<Ort::Session> encoder, decoder;
    bool preprocessInside = false;   // encoder takes [H, W, 3] floats 0..255 and normalises itself
    std::vector<float> embedding;
    std::vector<int64_t> embeddingShape;
    QSize size;                      // of the image given to setImage
    double scale = 1;                // image pixels → 1024 frame
};

bool runtimeAvailable() { return true; }
QString runtimeVersion() { return QStringLiteral("ONNX Runtime %1").arg(QString::fromStdString(Ort::GetVersionString())); }

SamSegmenter::SamSegmenter() : d(std::make_unique<Impl>()) {}
SamSegmenter::~SamSegmenter() = default;

bool SamSegmenter::load(const ModelCard &card, QString *error)
{
    d->encoder.reset();
    d->decoder.reset();
    d->embedding.clear();
    if (!card.isValid()) {
        if (error) *error = card.error;
        return false;
    }
    if (!verifyFiles(card, error)) return false;
    const QString enc = card.filePath(QStringLiteral("encoder")), dec = card.filePath(QStringLiteral("decoder"));
    if (enc.isEmpty() || dec.isEmpty()) {
        if (error) *error = QStringLiteral("В карточке нет кодировщика или декодера");
        return false;
    }
    try {
        d->encoder = ort::open(enc);
        d->decoder = ort::open(dec);
        const auto shape = d->encoder->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
        d->preprocessInside = shape.size() == 3;
    } catch (const Ort::Exception &e) {
        d->encoder.reset();
        d->decoder.reset();
        if (error) *error = QStringLiteral("ONNX Runtime: %1").arg(QString::fromUtf8(e.what()));
        return false;
    }
    return true;
}

bool SamSegmenter::isLoaded() const { return d->encoder && d->decoder; }
bool SamSegmenter::hasImage() const { return !d->embedding.empty(); }
QSize SamSegmenter::imageSize() const { return d->size; }

bool SamSegmenter::setImage(const QImage &image, QString *error)
{
    d->embedding.clear();
    if (!isLoaded() || image.isNull()) {
        if (error) *error = isLoaded() ? QStringLiteral("Пустое изображение") : QStringLiteral("Модель не загружена");
        return false;
    }
    d->size = image.size();
    d->scale = double(ImageSize) / qMax(image.width(), image.height());
    const QSize scaled(qMax(1, int(std::lround(image.width() * d->scale))), qMax(1, int(std::lround(image.height() * d->scale))));
    const QImage rgb = image.convertToFormat(QImage::Format_RGB888).scaled(scaled, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    std::vector<float> input;
    std::vector<int64_t> shape;
    if (d->preprocessInside) {
        shape = {rgb.height(), rgb.width(), 3};
        input.resize(size_t(rgb.width()) * rgb.height() * 3);
        for (int y = 0; y < rgb.height(); ++y) {
            const uchar *line = rgb.constScanLine(y);
            for (int x = 0; x < rgb.width() * 3; ++x) input[size_t(y) * rgb.width() * 3 + x] = line[x];
        }
    } else {
        // Normalised, channels first, zero-padded to 1024 × 1024 (right and bottom).
        shape = {1, 3, ImageSize, ImageSize};
        input.assign(size_t(3) * ImageSize * ImageSize, 0.0f);
        for (int y = 0; y < rgb.height(); ++y) {
            const uchar *line = rgb.constScanLine(y);
            for (int x = 0; x < rgb.width(); ++x)
                for (int c = 0; c < 3; ++c)
                    input[size_t(c) * ImageSize * ImageSize + size_t(y) * ImageSize + x] = (line[x * 3 + c] - PixelMean[c]) / PixelStd[c];
        }
    }
    try {
        const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value tensor = Ort::Value::CreateTensor<float>(memory, input.data(), input.size(), shape.data(), shape.size());
        const auto in = names(*d->encoder, true), out = names(*d->encoder, false);
        const char *inName = in[0].c_str(), *outName = out[0].c_str();
        auto result = d->encoder->Run(Ort::RunOptions{nullptr}, &inName, &tensor, 1, &outName, 1);
        const auto info = result[0].GetTensorTypeAndShapeInfo();
        d->embeddingShape = info.GetShape();
        const float *data = result[0].GetTensorData<float>();
        d->embedding.assign(data, data + info.GetElementCount());
    } catch (const Ort::Exception &e) {
        if (error) *error = QStringLiteral("ONNX Runtime: %1").arg(QString::fromUtf8(e.what()));
        return false;
    }
    return true;
}

SamSegmenter::Result SamSegmenter::segment(const QVector<QPointF> &points, const QVector<int> &labels)
{
    Result r;
    if (!hasImage()) {
        r.error = QStringLiteral("Изображение не обработано кодировщиком");
        return r;
    }
    if (points.isEmpty() || points.size() != labels.size()) {
        r.error = QStringLiteral("Нужна хотя бы одна точка-подсказка");
        return r;
    }
    // Points in the 1024 frame, plus SAM's padding point (label -1) used when there is no box.
    const int n = int(points.size()) + 1;
    std::vector<float> coords(2 * static_cast<size_t>(n));
    std::vector<float> pointLabels(static_cast<size_t>(n));
    for (int i = 0; i < points.size(); ++i) {
        coords[size_t(i) * 2] = float(points[i].x() * d->scale);
        coords[size_t(i) * 2 + 1] = float(points[i].y() * d->scale);
        pointLabels[size_t(i)] = float(labels[i]);
    }
    coords[size_t(n - 1) * 2] = 0;
    coords[size_t(n - 1) * 2 + 1] = 0;
    pointLabels[size_t(n - 1)] = -1;
    std::vector<float> maskInput(size_t(LowResSize) * LowResSize, 0.0f), hasMask{0.0f};
    std::vector<float> origSize{float(d->size.height()), float(d->size.width())};
    try {
        const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        const std::vector<int64_t> coordShape{1, n, 2}, labelShape{1, n}, maskShape{1, 1, LowResSize, LowResSize}, oneShape{1}, sizeShape{2};
        const auto in = names(*d->decoder, true), out = names(*d->decoder, false);
        std::vector<Ort::Value> inputs;
        std::vector<const char *> inNames;
        for (const std::string &name : in) {
            inNames.push_back(name.c_str());
            if (name == "image_embeddings")
                inputs.push_back(Ort::Value::CreateTensor<float>(memory, d->embedding.data(), d->embedding.size(), d->embeddingShape.data(), d->embeddingShape.size()));
            else if (name == "point_coords")
                inputs.push_back(Ort::Value::CreateTensor<float>(memory, coords.data(), coords.size(), coordShape.data(), coordShape.size()));
            else if (name == "point_labels")
                inputs.push_back(Ort::Value::CreateTensor<float>(memory, pointLabels.data(), pointLabels.size(), labelShape.data(), labelShape.size()));
            else if (name == "mask_input")
                inputs.push_back(Ort::Value::CreateTensor<float>(memory, maskInput.data(), maskInput.size(), maskShape.data(), maskShape.size()));
            else if (name == "has_mask_input")
                inputs.push_back(Ort::Value::CreateTensor<float>(memory, hasMask.data(), hasMask.size(), oneShape.data(), oneShape.size()));
            else if (name == "orig_im_size")
                inputs.push_back(Ort::Value::CreateTensor<float>(memory, origSize.data(), origSize.size(), sizeShape.data(), sizeShape.size()));
            else {
                r.error = QStringLiteral("Неизвестный вход декодера: %1").arg(QString::fromStdString(name));
                return r;
            }
        }
        std::vector<const char *> outNames;
        for (const std::string &name : out) outNames.push_back(name.c_str());
        auto outputs = d->decoder->Run(Ort::RunOptions{nullptr}, inNames.data(), inputs.data(), inputs.size(), outNames.data(), outNames.size());
        int maskIndex = -1, scoreIndex = -1;
        for (size_t i = 0; i < out.size(); ++i) {
            if (out[i] == "masks") maskIndex = int(i);
            else if (out[i] == "iou_predictions") scoreIndex = int(i);
        }
        if (maskIndex < 0) {
            r.error = QStringLiteral("Декодер не вернул маски");
            return r;
        }
        const auto shape = outputs[size_t(maskIndex)].GetTensorTypeAndShapeInfo().GetShape();   // [1, C, H, W]
        if (shape.size() != 4 || shape[2] != d->size.height() || shape[3] != d->size.width()) {
            r.error = QStringLiteral("Неожиданный размер маски");
            return r;
        }
        const int count = int(shape[1]);
        const float *scores = scoreIndex >= 0 ? outputs[size_t(scoreIndex)].GetTensorData<float>() : nullptr;
        // A click is ambiguous: SAM answers with a part, an object and its surroundings
        // (multimask outputs 1..3; 0 is its single answer for several points). On a map the
        // whole object is wanted (a roof, not its sunny half): the largest answer that the model
        // rates nearly as well as its best one, that covers every click on the object and none
        // of the excluded ones, and that is not the background (at most half of the image).
        const qint64 pixels = qint64(d->size.width()) * d->size.height();
        const float *allLogits = outputs[size_t(maskIndex)].GetTensorData<float>();
        const int first = points.size() == 1 && count >= 4 ? 1 : 0;
        int best = first;
        if (scores) {
            for (int i = first; i < count; ++i) if (scores[i] > scores[best]) best = i;
            if (points.size() > 1) best = 0;   // SAM's own answer when nothing better qualifies
            const float threshold = qMax(0.75f, scores[best] - 0.15f);
            qint64 bestArea = -1;
            for (int i = 0; i < count; ++i) {
                if (scores[i] < threshold || (i == 0 && first == 1)) continue;
                const float *m = allLogits + size_t(i) * size_t(pixels);
                bool fits = true;
                for (int p = 0; p < points.size() && fits; ++p) {
                    const int x = qBound(0, int(points[p].x()), d->size.width() - 1), y = qBound(0, int(points[p].y()), d->size.height() - 1);
                    fits = (m[size_t(y) * d->size.width() + x] > 0.0f) == (labels[p] == 1);
                }
                if (!fits) continue;
                qint64 area = 0;
                for (qint64 k = 0; k < pixels; ++k) area += m[k] > 0.0f;
                if (area * 2 <= pixels && area > bestArea) {
                    bestArea = area;
                    best = i;
                }
            }
        }
        r.score = scores ? scores[best] : 0.0f;
        const float *logits = allLogits + size_t(best) * size_t(pixels);
        r.mask = QImage(d->size, QImage::Format_Grayscale8);
        for (int y = 0; y < d->size.height(); ++y) {
            uchar *line = r.mask.scanLine(y);
            for (int x = 0; x < d->size.width(); ++x) line[x] = logits[size_t(y) * d->size.width() + x] > 0.0f ? 255 : 0;
        }
    } catch (const Ort::Exception &e) {
        r.error = QStringLiteral("ONNX Runtime: %1").arg(QString::fromUtf8(e.what()));
        r.mask = {};
    }
    return r;
}

std::shared_ptr<SamSegmenter> cachedSegmenter(const ModelCard &card, QString *error)
{
    environment();   // created first, so it is destroyed after the cached sessions
    static std::mutex mutex;
    static std::shared_ptr<SamSegmenter> cached;
    static QString cachedDirectory;
    std::lock_guard<std::mutex> lock(mutex);
    if (cached && cachedDirectory == card.directory) return cached;
    auto segmenter = std::make_shared<SamSegmenter>();
    if (!segmenter->load(card, error)) return {};
    cached = segmenter;
    cachedDirectory = card.directory;
    return cached;
}

#else   // built without ONNX Runtime

struct SamSegmenter::Impl {};
bool runtimeAvailable() { return false; }
QString runtimeVersion() { return QStringLiteral("эта сборка без ONNX Runtime"); }
SamSegmenter::SamSegmenter() : d(std::make_unique<Impl>()) {}
SamSegmenter::~SamSegmenter() = default;
bool SamSegmenter::load(const ModelCard &, QString *error)
{
    if (error) *error = QStringLiteral("Программа собрана без ONNX Runtime: локальный AI недоступен");
    return false;
}
bool SamSegmenter::isLoaded() const { return false; }
bool SamSegmenter::setImage(const QImage &, QString *error)
{
    if (error) *error = QStringLiteral("Локальный AI недоступен в этой сборке");
    return false;
}
bool SamSegmenter::hasImage() const { return false; }
QSize SamSegmenter::imageSize() const { return {}; }
SamSegmenter::Result SamSegmenter::segment(const QVector<QPointF> &, const QVector<int> &)
{
    Result r;
    r.error = QStringLiteral("Локальный AI недоступен в этой сборке");
    return r;
}
std::shared_ptr<SamSegmenter> cachedSegmenter(const ModelCard &, QString *error)
{
    if (error) *error = QStringLiteral("Программа собрана без ONNX Runtime: локальный AI недоступен");
    return {};
}

#endif

} // namespace ai
