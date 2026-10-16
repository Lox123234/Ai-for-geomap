#pragma once

#include "ModelCard.h"

#include <QImage>
#include <QPointF>
#include <QSize>
#include <QVector>
#include <memory>
#include <mutex>

// Interactive segmentation with a Segment Anything model exported to ONNX (MobileSAM): the image
// encoder turns an RGB image (longest side scaled to 1024) into an embedding once; the mask
// decoder then answers each set of point prompts in milliseconds. Runs in the calling thread.
namespace ai {

bool runtimeAvailable();     // built with ONNX Runtime
QString runtimeVersion();    // "ONNX Runtime 1.30.0", or why there is none

class SamSegmenter
{
public:
    SamSegmenter();
    ~SamSegmenter();
    SamSegmenter(const SamSegmenter &) = delete;
    SamSegmenter &operator=(const SamSegmenter &) = delete;

    // Checks the files of the card (SHA-256) and opens the encoder and the decoder.
    bool load(const ModelCard &card, QString *error);
    bool isLoaded() const;
    // The slow step: the embedding of `image` (any size, RGB). `key` names the image for
    // imageKey(): users sharing one segmenter re-encode only when it holds another image.
    bool setImage(const QImage &image, QString *error, const QString &key = {});
    bool hasImage() const;
    QSize imageSize() const;
    QString imageKey() const;
    // Held by whoever encodes and decodes with a shared segmenter.
    std::mutex &mutex();

    struct Result
    {
        QImage mask;        // Grayscale8, the size of the image: 255 inside the object
        float score = 0;    // the model's estimate of the mask quality (IoU)
        QString error;
        bool ok() const { return error.isEmpty() && !mask.isNull(); }
    };
    // Points in pixels of the image; labels 1 = on the object, 0 = background.
    Result segment(const QVector<QPointF> &points, const QVector<int> &labels);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
    std::mutex m_mutex;
    QString m_imageKey;
};

// The segmenter of `card`, loaded once and shared (loading checks SHA-256 and builds the ONNX
// sessions: seconds). Thread-safe; callers must not use the returned segmenter concurrently.
std::shared_ptr<SamSegmenter> cachedSegmenter(const ModelCard &card, QString *error);

} // namespace ai
