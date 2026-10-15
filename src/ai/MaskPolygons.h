#pragma once

#include <QImage>
#include <QPolygonF>
#include <QString>
#include <QVector>

// Turns a segmentation mask into polygons: outlines along pixel edges (GDALPolygonize,
// 8-connected), then Douglas–Peucker to remove the pixel staircase.
namespace ai {

struct MaskPolygon
{
    QPolygonF outer;              // pixel coordinates (corners), y down
    QVector<QPolygonF> holes;
    double area = 0;              // pixels, without the holes
    bool contains(const QPointF &pixel) const;
};

// Polygons of the non-zero pixels of a Grayscale8 mask; empty with `error` on failure.
QVector<MaskPolygon> maskPolygons(const QImage &mask, QString *error = nullptr);
// Douglas–Peucker on a closed ring (open form: the first point is not repeated at the end);
// `tolerance` in the units of the ring. Keeps at least a triangle.
QPolygonF simplifyRing(const QPolygonF &ring, double tolerance);

} // namespace ai
