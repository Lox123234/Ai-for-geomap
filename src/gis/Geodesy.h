#pragma once

#include <QPointF>
#include <QVector>

class Crs;

// Distances and areas on the ellipsoid (Karney's geodesic algorithms as shipped with PROJ),
// never from differences of latitude and longitude. Points are (longitude, latitude) in degrees.
namespace Geodesy {

struct Ellipsoid
{
    double a = 6378137.0;                 // WGS 84
    double f = 1.0 / 298.257223563;
};
Ellipsoid ellipsoidOf(const Crs &crs);    // WGS 84 if the CRS has no ellipsoid

double distance(const QPointF &from, const QPointF &to, const Ellipsoid &e = {});   // metres
double length(const QVector<QPointF> &line, const Ellipsoid &e = {});               // metres

struct Polygon
{
    double area = 0;        // m², always positive
    double perimeter = 0;   // m
};
Polygon polygon(const QVector<QPointF> &ring, const Ellipsoid &e = {});   // ring may be open or closed

} // namespace Geodesy
