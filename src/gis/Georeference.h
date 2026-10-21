#pragma once

#include "Crs.h"

#include <QPointF>
#include <QPolygonF>
#include <QSize>
#include <QString>
#include <QVector>
#include <functional>

// Georeferencing of an image by control point links: each
// link joins a pixel of the image to a point of the map; a transformation is fitted to the links by
// least squares; the residual of every link (map units) and the inverse residual (pixels) and the
// RMS error tell how well the links agree; the image is then warped into a GeoTIFF of the map's CRS.
namespace georef {

enum class Method {
    Affine = 1,        // first-order polynomial: shift, rotation, scale, skew (≥ 3 links)
    Polynomial2 = 2,   // second order (≥ 6 links): bends of scanned maps, lens distortion
    Polynomial3 = 3,   // third order (≥ 10 links)
    Projective = 4,    // perspective of a flat scene (≥ 4 links): an oblique photo of flat ground
};
int minimumLinks(Method m);
QString title(Method m);

struct Link
{
    QPointF image;     // pixel of the image (x right, y down; the centre of the top-left pixel is 0.5, 0.5)
    QPointF map;       // point of the map, in its CRS
    bool enabled = true;
};

// A fitted transformation from one plane to the other (image → map, or the inverse).
class Transform
{
public:
    Transform() = default;
    bool isValid() const { return m_valid; }
    Method method() const { return m_method; }
    QPointF map(const QPointF &p) const;
    // Projective transformation through four corners (an initial placement of a photo).
    static Transform fromCorners(const QSizeF &imageSize, const QPolygonF &corners);

private:
    friend struct Fitter;
    bool m_valid = false;
    Method m_method = Method::Affine;
    QPointF m_inCentre, m_outCentre;   // normalisation: (p − centre) / scale
    double m_inScale = 1, m_outScale = 1;
    QVector<double> m_x, m_y;          // polynomial coefficients; projective: a…h in m_x
};

struct Fit
{
    QString error;
    Transform forward;                 // image → map
    Transform inverse;                 // map → image
    QVector<double> residual;          // per link, map units (NaN for a disabled link)
    QVector<double> inverseResidual;   // per link, pixels
    double rmse = 0, inverseRmse = 0;  // over the enabled links
    int used = 0;
    bool ok() const { return error.isEmpty(); }
};

Fit fit(const QVector<Link> &links, Method method);

struct WarpResult
{
    QString error;
    QString path;
    int width = 0, height = 0;
    double resolution = 0;             // map units per pixel
    bool ok() const { return error.isEmpty(); }
};

// The image warped by `forward` into an RGBA GeoTIFF of `crs` (transparent outside the image).
// `resolution` 0: the image's own (its pixel at the centre, on the map); `maxPixels` > 0 makes a
// reduced preview (the image scaled to that longest side first).
WarpResult warp(const QString &imagePath, const Transform &forward, const Crs &crs, const QString &outputPath,
                double resolution = 0, int maxPixels = 0, const std::function<bool(double)> &progress = {});

// Pixel size of an image file (0 × 0 if it cannot be read).
QSize imageSize(const QString &imagePath);

} // namespace georef
