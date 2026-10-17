#pragma once

#include "ModelCard.h"

#include <QImage>
#include <memory>

// Fills parts of an image from their surroundings with an inpainting model exported to ONNX
// (LaMa: 512 × 512 image and mask in, the filled image out). Used to remove objects from an
// orthophoto and to fill its gaps; the result is a plausible picture, not a measurement.
namespace ai {

class Inpainter
{
public:
    static constexpr int WindowSize = 512;

    Inpainter();
    ~Inpainter();
    Inpainter(const Inpainter &) = delete;
    Inpainter &operator=(const Inpainter &) = delete;

    // Checks the files of the card (SHA-256) and opens the file with the role "inpainter".
    bool load(const ModelCard &card, QString *error);
    bool isLoaded() const;

    struct Result
    {
        QImage image;       // RGB888, the size of the input; only masked pixels differ
        int windows = 0;    // model runs
        int scaled = 0;     // of them, windows larger than 512 px scaled down (coarser fill)
        QString error;
        bool ok() const { return error.isEmpty() && !image.isNull(); }
    };
    // Fills the pixels where `mask` (Grayscale8, the size of the image) is not zero. Each part
    // is filled in a 512 × 512 window around it, with its surroundings as context; parts that do
    // not fit with their context are filled in a larger window scaled down to 512.
    Result inpaint(const QImage &image, const QImage &mask);
    // One run of the model on exactly one 512 × 512 window: the masked pixels painted from the
    // unmasked ones (nothing scaled, nothing else in the window seen as context).
    Result inpaintWindow(const QImage &image, const QImage &mask);

private:
    QImage run(const QImage &image512, const QImage &mask512, QString *error);   // the model's output, 512 × 512
    struct Impl;
    std::unique_ptr<Impl> d;
};

// Loaded once per model and shared; callers must not use it concurrently.
std::shared_ptr<Inpainter> cachedInpainter(const ModelCard &card, QString *error);

} // namespace ai
