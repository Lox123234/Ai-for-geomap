#pragma once

#include <QImage>
#include <QString>
#include <QVector>
#include <QtNumeric>

// Sharpness of photos (Обработка → Оценка качества снимков): how sharp the most focused part of
// a photo is. The no-reference blur metric of
// Crété-Roffet et al. (2007, "The blur effect"): the share of the image's own gradient that a
// further 9-pixel blur does not remove, on 8 × 6 tiles of the photo halved until its long side is
// at most MaxSide pixels; the photo's value is the 90th percentile of its tiles with contrast
// (sky, water and plain roofs do not count). It depends little on the scene: sharp DJI photos of
// one survey (4000 × 3000) come out at 0.82–0.86, copies with 9 px of motion blur at 0.64–0.69,
// with 21 px below 0.55.
// Strong sensor noise (high ISO) looks like detail and raises the value.
namespace imagequality {

constexpr int MaxSide = 3000;
constexpr double Threshold = 0.5;   // quality below: blurred (the usual threshold)

struct Result
{
    double sharpness = qQNaN();
    QString error;
    bool ok() const { return error.isEmpty(); }
};

Result measure(const QString &path);
double sharpness(const QImage &image);   // any format, used at its size

// Quality 0…1 against the sharpest photos of the set (the 90th percentile of their sharpness):
// 1 − (reference − sharpness) / Span; 0.5 is Span / 2 below the sharp ones. NaN without values.
constexpr double Span = 0.3;
double reference(QVector<double> sharpness);
double quality(double sharpness, double reference);

} // namespace imagequality
